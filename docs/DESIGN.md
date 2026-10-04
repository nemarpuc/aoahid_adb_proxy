# Design

## Goal

WinUSB lets only one process open a device. libaoahid drives HID on EP0 and a bulk Channel on the ADB interface through a **single handle**. So the app owns the device, and only ADB bulk traffic is exposed over TCP.

## Threads

```text
accept : poll(100 ms) -> accept -> TCP_NODELAY
  ├ tx : TCP -> re-frame per apacket -> aoahid_channel_write
  └ rx : aoahid_channel_read (header, then data_length) -> send
```

- Waits return as soon as data is ready. The 100 ms timeout only bounds how long `stop` takes (up to 1 s if the device has stopped reading).
- All socket waits use `poll` (`select` on Windows). On Windows, `SO_RCVTIMEO` does not apply to `accept`, and a socket whose receive timed out is left in an undefined state. Unlike `select`, `poll` has no `FD_SETSIZE` limit on the descriptor value, which a host application with many open files can exceed.
- If a client stops reading and rx blocks in `send`, the socket is `shutdown` after tx exits.
- The accept thread closes the listening socket when it leaves its loop, which happens on `stop` and when the Channel is lost (a failed USB read, or an IN header announcing more than `MAX_PAYLOAD`). After a loss the port therefore refuses new connections instead of accepting ones nothing would answer.
- Sends use `MSG_NOSIGNAL` (`SO_NOSIGPIPE` where that is the mechanism), so a client that disconnects while the device is still sending ends the session with `EPIPE` instead of killing the host process with `SIGPIPE`.

## Why TCP -> USB is re-framed

An ADB message (apacket) is a 24-byte header (`amessage`) followed by `data_length` payload bytes. TCP delivers a plain byte stream, so `recv` boundaries do not match apacket boundaries.

**Before September 2024, adbd depended on USB transfer boundaries.** The code before AOSP `packages/modules/adb` commit `4af6e4ff`, in `daemon/usb.cpp`:

```cpp
if (block->payload.size() != sizeof(amessage)) {
    HandleError("received packet of unexpected length while reading header");
...
if (block->payload.size() > bytes_left) {
    HandleError("received too many bytes while waiting for payload");
```

A header must arrive as its own 24-byte transfer, and a payload transfer must not contain the next header. Since `4af6e4ff`, `APacketReader` accepts any split. adbd is updated through Mainline (APEX), so older devices still run the old reader.

Legacy adbd (Android 8 and 9, `daemon/usb_legacy.cpp`, removed in `6b55e755`) goes further. With aio it reads a payload by submitting all `data_length / io_size` read blocks at once, at fixed offsets. A short USB packet in the middle of the payload ends one block early and corrupts the packet.

So the tx thread writes exactly the way host adb does (`client/usb_windows.cpp` `usb_write`, `client/usb_libusb_device.cpp` `Write`):

1. Read the 24-byte header. Check that `command ^ 0xFFFFFFFF == magic` and `data_length <= 1 MiB` (`MAX_PAYLOAD`). If either check fails, drop the client.
2. Read the whole payload.
3. Write the header with one `aoahid_channel_write`, then the payload with one more.

Buffering the payload costs only a loopback copy (microseconds).

Behavior of libaoahid `Channel::write` (`src/transport/channel.cpp`):

- Each call is split into transfers of at most `transfer_bytes` (1 MiB here, see below), so a header or a payload up to `MAX_PAYLOAD` is one transfer. It never shares a transfer with another call.
- With `zero_length_termination = 1`, a call whose length is a multiple of `wMaxPacketSize` ends with a zero-length packet (ZLP). Host adb does the same (`zero_mask`, `zlp_mask_`), and legacy adbd expects it (`reads_zero_packets = true`).

### Write timeouts

`aoahid_channel_write` waits up to 1 s for a free transfer slot. adbd always keeps reads queued, so a pool that stays full that long means the device is stuck. On a timeout, the first `*written` bytes are already queued:

- `written < remaining`: write the rest again, starting after the queued bytes.
- `written == remaining`: all data is queued and only the ZLP is pending. The API cannot send a ZLP alone, so the session is dropped.

## Channel settings

adbd sends **no** ZLP after a packet-aligned payload. Host adb reads exact lengths instead (`client/usb_libusb_device.cpp` `Read`: 24 bytes, then `data_length`; `client/transport_usb.cpp` `UsbReadPayload`). A Bulk IN transfer completes only when full or on a short packet. With read-ahead transfers, a 4096-byte payload inside a larger transfer would wait for more data. The device may be waiting for the host's reply, which is a deadlock.

The proxy opens the Channel the way host adb uses USB:

- `read_mode = AOAHID_CHANNEL_READ_REQUEST` (libaoahid 3.0). Each read submits one IN transfer for exactly the bytes still missing, rounded up to `wMaxPacketSize`: the header, then `data_length`. It completes the moment the data is in.
- `transfer_bytes = 1 MiB` (`MAX_PAYLOAD`). Every payload goes out as one OUT transfer, followed by a ZLP when packet-aligned.
- `out_transfers = 2`, so the next header can queue behind a payload.

## USB -> TCP

The rx thread assembles one whole apacket (header, then exactly `data_length`) and sends it in one `send`. The packet being assembled lives in the proxy context. If a client disconnects mid-packet, the next session finishes reading that packet and drops it, so the new client never starts mid-packet.

The device uses TLS (`A_STLS`) only on its Wi-Fi transport. Over USB it uses the normal CNXN/AUTH exchange, which the proxy passes through unchanged.

## USB vs TCP transport

In adbd, packets from USB and from TCP both become `apacket`s handled by `handle_packet()`. The only transport difference is the USB transfer framing described above, and the proxy handles it.

## libaoahid requirements

- libaoahid 4.x (`read_mode` arrived in 3.0).
- The Context must use `AOAHID_EVENT_INTERNAL_THREAD`. In `CALLER_POLL` mode, Channel reads and writes must be serialized by the caller, so separate threads cannot call them concurrently (see `aoahid_channel_write` in `aoahid.h`).
- There is one reader thread and one writer thread. `aoahid_channel_close` runs in `stop`, after both threads have been joined.
