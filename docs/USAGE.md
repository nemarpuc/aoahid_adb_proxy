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
| Windows: `start` returns `-2`, and `aoahid_last_error()->libusb_status` is `-12` | The phone's driver is not one libusb can use (WinUSB, libusbK or libusb0). Some manufacturers install their own driver by default. Switch the phone to WinUSB with Zadig; see [the Samsung example](#example-samsung-on-windows). |
| `start` returns `-4` | The port is in use. Pick another port. |
| `offline` after `adb connect` | Accept the prompt on the phone, then `adb disconnect` and reconnect. |
| Connection drops | The phone was unplugged, or USB writes stalled. The proxy keeps listening, so run `adb connect` again. If the USB device itself was lost, restart your app. |

### Example: Samsung on Windows

A Samsung tablet with Samsung's default USB driver opened and took HID input normally, but `aoahid_adb_proxy_start` returned `-2`. Right after it, on the same thread, `aoahid_last_error()` reported `AOAHID_ERR_UNSUPPORTED` from `channel.open` with `libusb_status` `-12` (`LIBUSB_ERROR_NOT_SUPPORTED`). On Windows, libusb can only claim an interface whose driver is WinUSB, libusbK or libusb0. HID uses the control endpoint, so it still worked. The adb server and USB debugging were not the cause.

Why Samsung and not the HyperOS phone: on Windows the HyperOS phone's ADB interface came up with WinUSB from the start, so the proxy worked with no changes. Samsung ships its own dedicated USB driver, and Windows uses it for Samsung devices instead of WinUSB. On the tablet, the parent device "SAMSUNG Mobile USB Composite Device" used Samsung's `dg_ssudbus` (version 2.21.4.0). libusb accepts `dg_ssudbus` as a composite parent, but the ADB interface under it did not get WinUSB, so libusb could not claim it.

The fix:

1. Plug in the phone and open Device Manager.
2. Under *Universal Serial Bus controllers*, right-click the phone's parent device ("SAMSUNG Mobile USB Composite Device" for Samsung) and choose *Uninstall device*. Tick *Attempt to remove the driver for this device* (*Delete the driver software for this device* on Windows 10) and click *Uninstall*. Without this tick, Windows puts the same driver back.
3. Unplug the phone and plug it back in.
4. Download and run [Zadig](https://zadig.akeo.ie/).
5. Turn on *Options → List All Devices*.
6. Pick the entry for the whole phone, not one of its interfaces. On the Samsung tablet it was "SAMSUNG Android"; the entries for its ADB and MTP interfaces (names ending in "(Interface N)") did not fix it when changed. The names depend on the device; the whole phone's *USB ID* has two boxes (vendor and product), while an interface entry has a third one.
7. Set the driver on the right of the arrow to *WinUSB* and click *Replace Driver* (*Install Driver* if it had none).
8. Open the device in your app again and start the proxy.

With the whole phone on WinUSB, libusb treats it as one WinUSB device and reaches every interface, ADB included, through it (`winusbx_claim_interface` uses `WinUsb_GetAssociatedInterface`). Replacing only the ADB interface's driver left `start` failing on the Samsung tablet, most likely because Samsung's composite driver does not expose that interface in a way libusb can map; this was not examined further.

Other manufacturers that ship their own dedicated USB driver can be handled the same way. A phone whose ADB interface is already WinUSB, like the HyperOS phone, needs none of this.

After this, Windows `adb.exe` on its own no longer sees the phone over USB, Windows no longer shows the phone for file transfer (MTP), since the whole phone is on WinUSB, and tools that need the manufacturer's driver (such as Samsung Smart Switch) may stop working with it. adb keeps working through the proxy. To undo, repeat steps 1-3, or reinstall the manufacturer's USB driver.

## Limits

- One adb client at a time. Normally there is only one adb server, so this is enough.
- Verified end to end on real hardware with a Samsung tablet and a HyperOS phone, each on Linux and on Windows, through aoahid_player's ADB Bridge. That app keeps the phone in its current USB mode (`AOAHID_START_CURRENT_USB_MODE`, no accessory switch), so the accessory-mode path in `examples/basic_proxy.cpp` was not part of it. On Windows the HyperOS phone worked with its default WinUSB driver; the Samsung tablet needed the Zadig step above.
