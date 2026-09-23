# Usage

## Steps

| # | Action | Why |
|---|---|---|
| 1 | `adb kill-server` | While the adb server holds the ADB interface, your app cannot claim it. |
| 2 | Start your app: switch to accessory mode, `aoahid_device_open`, `aoahid_adb_proxy_start` | Your app now owns the whole USB device. |
| 3 | `adb connect 127.0.0.1:6555` | adb restarts its server and reaches the phone over TCP. |

On first use the phone may ask to allow USB debugging. This is the same RSA prompt as a direct USB connection.

When the adb server restarts in step 3, it also sees the phone on USB. It cannot open the phone because your app holds it, and the TCP connection works normally. To stop adb from probing USB at all, start the server with `ADB_USB=0`. This hides all other USB devices from adb too.

## Keeping other devices on adb

- **Windows default adb:** there is no way to release a single device. Use `adb kill-server`. `adb detach` works only with the libusb backend (AOSP `client/usb_libusb.cpp`, `SupportsDetach`). Windows defaults to the AdbWinApi backend (`client/transport_usb.cpp`, `is_libusb_enabled`).
- **`adb -s <serial> detach` before switching does not help.** Switching to accessory mode re-enumerates the phone. adb then sees a new device and claims it again.
- **libusb backend:** start the server with every USB device detached, then attach only the devices adb should use:
  ```sh
  adb kill-server
  ADB_LIBUSB=1 ADB_LIBUSB_START_DETACHED=1 adb start-server
  adb -s <other-device> attach
  ```
  PowerShell: `$env:ADB_LIBUSB="1"; $env:ADB_LIBUSB_START_DETACHED="1"; adb start-server`.
  On Windows this requires a WinUSB-based driver, such as the Google USB Driver. This path is not hardware-verified yet.

## Port

On startup, the adb server scans ports 5555-5585 for emulators (AOSP `client/transport_emulator.cpp`). A proxy in that range may appear as an unexpected `emulator-XXXX` device. Use a port outside it, such as `6555`. The proxy listens on `127.0.0.1` only.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `start` returns `-2` | The adb server holds the interface: run `adb kill-server`. Or USB debugging is off, so the device came up as `2d00` (no ADB). |
| `start` returns `-4` | The port is in use. Pick another port. |
| `offline` after `adb connect` | Accept the prompt on the phone, then `adb disconnect` and reconnect. |
| Connection drops | The phone was unplugged, or USB writes stalled. The proxy keeps listening, so run `adb connect` again. If the USB device itself was lost, restart your app. |

## Limits

- One adb client at a time. Normally there is only one adb server, so this is enough.
- End-to-end testing on real hardware has not been done yet.
