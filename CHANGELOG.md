# Changelog

## 2.0.1

- The example's `-2` hint on Windows also points at the phone's driver, since a manufacturer driver instead of WinUSB gives the same result.
- Documented the Samsung-on-Windows driver fix (Device Manager, then Zadig on the whole device) and the end-to-end hardware results: a Samsung tablet and a HyperOS phone, on Linux and Windows.

## 2.0.0

- Requires Libaoa_hid 3.0. The ADB Channel now uses `AOAHID_CHANNEL_READ_REQUEST` and reads each apacket the way host adb does: the 24-byte header, then exactly `data_length` in one IN transfer. Payloads go out as one OUT transfer of up to 1 MiB. This replaces the one-packet transfers of 1.0.0, which were correct but spent a transfer per 512 bytes.
- Device-to-host traffic is forwarded one whole apacket per `send`.
- A packet cut off by a client disconnect is finished and dropped at the start of the next session, instead of draining USB input.

## 1.0.0

First release.

- Forwards the ADB interface of a Libaoa_hid device to `127.0.0.1:<port>` for `adb connect`.
- Writes each apacket to USB the way host adb does: the header in one write, the payload in one write, and a ZLP when the payload is packet-aligned. This works with current, pre-2024, and legacy (Android 8/9) adbd.
- Uses one-packet Channel transfers (32 queued each way), so a packet-aligned payload from the device never stalls.
- Forwards device output to TCP as soon as it arrives, with `TCP_NODELAY`.
- Resumes partially queued USB writes and partial TCP sends.
- Uses `select` for all socket waits, so `stop` returns within about 100 ms on every platform.
- Drops clients that send malformed headers, and discards stale USB input when a new client connects.
- Finds an installed Libaoa_hid (`find_package(aoahid)`). If none is found, fetches tag `v2.0.1`.
- Includes a runnable example that switches the phone to accessory + ADB mode and serves ADB on port 6555.
