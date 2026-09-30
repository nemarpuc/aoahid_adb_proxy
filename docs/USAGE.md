# Usage

## Steps

| # | Action | Why |
|---|---|---|
| 1 | `adb kill-server` | While the adb server holds the ADB interface, your app cannot claim it. |
| 2 | Start your app: `aoahid_device_open` (in the current USB mode, or after `aoahid_accessory_start`), then `aoahid_adb_proxy_start` | Your app now owns the whole USB device. |
| 3 | `adb connect 127.0.0.1:6555` | adb restarts its server and reaches the phone over TCP. |
| 4 | To stop: `adb disconnect 127.0.0.1:6555`, `aoahid_adb_proxy_stop`, `aoahid_device_close` | Releases the ADB interface. |
| 5 | `adb kill-server` again | See below: without this, adb does not see the phone over USB again. |

On first use the phone may ask to allow USB debugging. This is the same RSA prompt as a direct USB connection.

When the adb server restarts in step 3, it also sees the phone on USB. It cannot open the phone because your app holds it, and the TCP connection works normally. To stop adb from probing USB at all, start the server with `ADB_USB=0`. This hides all other USB devices from adb too.

**Restart the adb server after stopping the proxy.** Run `adb kill-server` once the proxy is stopped and the device closed; the next adb command starts a fresh server that picks the phone up over USB. aoahid_player does this automatically when a bridge is turned off or the phone is disconnected. The reason, from the adb sources (AOSP `platform/packages/modules/adb`, commit [`1cf2f017`](https://android.googlesource.com/platform/packages/modules/adb/+/1cf2f017d312f73b3dc53bda85ef2610e35a80e9)):

- On Linux and macOS the adb server uses its libusb backend by default (`client/transport_usb.cpp`, `is_libusb_enabled`: on everywhere except Windows; `ADB_LIBUSB=1` or `ADB_LIBUSB=0` overrides it).
- That backend only looks at a device when libusb reports it as arrived: once for each device present at startup (`LIBUSB_HOTPLUG_ENUMERATE`) and again only when it is plugged in (`client/usb_libusb_hotplug.cpp`, `usb_init_libusb_hotplug`, `process_device`).
- Starting the transport opens the device and claims the ADB interface (`client/usb_libusb_device.cpp`, `LibUsbDevice::Open`, `ClaimInterface`). While your app holds the interface the claim fails, and the transport code just returns (`transport.cpp`, `fdevent_register_transport`: "failed to start."). Nothing retries it later.
- So a server that saw the phone while the proxy held it never uses it over USB again, even after the interface is released, until the phone is replugged or the server restarts.

Reproduced on Arch Linux with a Samsung Galaxy Tab S11 (adb 37.0.0): with the proxy running, `adb start-server` then `adb devices` listed nothing; after the proxy stopped, the same server still listed nothing; after `adb kill-server` the phone was listed again.

On Windows the default backend (AdbWinApi, `client/usb_windows.cpp`) instead polls every second (`device_poll_thread`, `find_devices`) and tries again to open any interface it does not already hold, so by the source it should pick the phone up again without a restart. Restarting it does no harm; the Windows case has not been tested separately.

## Keeping other devices on adb

- **Windows default adb:** there is no way to release a single device. Use `adb kill-server`. `adb detach` works only with the libusb backend (AOSP `client/usb_libusb.cpp`, `SupportsDetach`). Windows defaults to the AdbWinApi backend (`client/transport_usb.cpp`, `is_libusb_enabled`).
- **If your app switches to accessory mode, `adb -s <serial> detach` beforehand does not help.** The switch re-enumerates the phone. adb then sees a new device and claims it again.
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
| `start` returns `-4` | The port could not be bound: another program uses it, Windows reserves it (`netsh interface ipv4 show excludedportrange protocol=tcp`), or it needs privileges. Pick another port. |
| `offline` after `adb connect` | Accept the prompt on the phone, then `adb disconnect` and reconnect. |
| Connection drops | If a USB write failed or stalled, the proxy keeps listening: run `adb connect` again. If reading from the phone failed (unplugged, or any USB read error), the proxy stops serving; the port still accepts connections but nothing answers, so call `aoahid_adb_proxy_stop`, reopen the device if it was lost, and start the proxy again. |

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

After this, Windows `adb.exe` still sees the phone over USB as before. Tools that need the manufacturer's driver (such as Samsung Smart Switch) may stop working with it, and Windows file transfer (MTP) may too; MTP was not checked. To undo, repeat steps 1-3, or reinstall the manufacturer's USB driver.

## Limits

- One adb client at a time. Normally there is only one adb server, so this is enough.
- Verified end to end on real hardware with a Samsung Galaxy Tab S11 and a POCO F6 Pro (HyperOS), each on Windows 10 x64 and Arch Linux (phone driver WinUSB or libusbK on Windows), through aoahid_player's ADB Bridge. That app keeps the phone in its current USB mode (no `aoahid_accessory_start`), so the accessory-mode path was not part of it. On Windows the HyperOS phone worked with its default WinUSB driver; the Samsung tablet needed the Zadig step above.
