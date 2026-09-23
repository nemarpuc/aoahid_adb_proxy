# Design

## Goal

WinUSB lets only one process open a device. Libaoa_hid drives HID on EP0 and a bulk Channel on the ADB interface through a **single handle**. So the app owns the device, and only ADB bulk traffic is exposed over TCP.

## Threads

```text
accept : select(100 ms) -> accept -> TCP_NODELAY -> drop stale USB input
  ├ tx : TCP -> re-frame per apacket -> aoahid_channel_write
  └ rx : aoahid_channel_read -> batch what is queued -> send
```

- Waits return as soon as data is ready. The 100 ms timeout only bounds how long `stop` takes (up to 1 s if the device has stopped reading).
- All socket waits use `select`. On Windows, `SO_RCVTIMEO` does not apply to `accept`, and a socket whose receive timed out is left in an undefined state.
- If a client stops reading and rx blocks in `send`, the socket is `shutdown` after tx exits.

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

Behavior of Libaoa_hid `Channel::write` (`src/transport/channel.cpp`):

- Each call is split into back-to-back full-packet transfers, followed by one short tail. It never shares a transfer with another call.
- With `zero_length_termination = 1`, a call whose length is a multiple of `wMaxPacketSize` ends with a zero-length packet (ZLP). Host adb does the same (`zero_mask`, `zlp_mask_`), and legacy adbd expects it (`reads_zero_packets = true`).

### Write timeouts

`aoahid_channel_write` waits up to 1 s for a free transfer slot. adbd always keeps reads queued, so a pool that stays full that long means the device is stuck. On a timeout, the first `*written` bytes are already queued:

- `written < remaining`: write the rest again, starting after the queued bytes.
- `written == remaining`: all data is queued and only the ZLP is pending. The API cannot send a ZLP alone, so the session is dropped.

## Channel transfer size

adbd sends **no** ZLP after a packet-aligned payload. Host adb reads exact lengths instead (`client/usb_libusb_device.cpp` `Read`: 24 bytes, then `data_length`). With Libaoa_hid's default 64 KiB read-ahead, an IN transfer holding, for example, a 4096-byte payload would stay incomplete until the device sends more. The device may be waiting for the host's reply, which is a deadlock.

The proxy therefore opens the Channel with `transfer_bytes = 1`, which Libaoa_hid rounds up to `wMaxPacketSize`. Every IN transfer then completes on every packet. 32 IN and 32 OUT transfers stay queued to keep the bus busy.

## USB -> TCP is forwarded as is

Host adb parses TCP as a byte stream (`transport_fd.cpp`), so no framing is needed. After the first bytes arrive, the rx thread collects everything already received with non-blocking reads and sends it in one `send`. This adds no waiting.

The device uses TLS (`A_STLS`) only on its Wi-Fi transport. Over USB it uses the normal CNXN/AUTH exchange, which the proxy passes through unchanged.

## USB vs TCP transport

In adbd, packets from USB and from TCP both become `apacket`s handled by `handle_packet()`. The only transport difference is the USB transfer framing described above, and the proxy handles it.

## Libaoa_hid requirements

- The Context must use `AOAHID_EVENT_INTERNAL_THREAD`. In `CALLER_POLL` mode, Channel reads and writes must be serialized by the caller, so separate threads cannot call them concurrently (see `aoahid_channel_write` in `aoahid.h`).
- There is one reader thread and one writer thread. `aoahid_channel_close` runs in `stop`, after both threads have been joined.
