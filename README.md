# aoahid_adb_proxy

Use `adb` and [Libaoa_hid](https://github.com/nemarpuc/Libaoa_hid) on the same Android device at the same time.

On Windows, a USB device can be opened by only one process (WinUSB is exclusive), so `adb.exe` and a Libaoa_hid app cannot share a phone. This small C library runs inside your Libaoa_hid app: the app owns the USB device, and the proxy forwards the ADB interface to a local TCP port that `adb connect` can use.

```text
adb ──TCP──▶ [your app + aoahid_adb_proxy] ──USB bulk (ADB)──▶ adbd
                         └────────── USB EP0 (AOA HID) ──────▶ input
```

This also keeps adb usable on Windows when the phone's driver is WinUSB or libusbK, which libusb-based tools such as Libaoa_hid need. With either driver, Windows `adb.exe` on its own could not use the phone; through the proxy, it can.

It is a **library**, not a standalone tool. `examples/basic_proxy.cpp` is a complete, runnable example.

## Quick start

```sh
adb kill-server                  # 1. release the phone from adb
./basic_proxy_example            # 2. your app: open the phone, start the proxy
adb connect 127.0.0.1:6555       # 3. use adb over the proxy
adb -s 127.0.0.1:6555 shell
```

The order matters: if the adb server holds the ADB interface, the proxy cannot claim it (`-2`).

## API

```c
#include "aoahid_adb_proxy.h"

int  aoahid_adb_proxy_start(aoahid_device* device, uint16_t tcp_port,
                            aoahid_adb_proxy_context** out_proxy);
void aoahid_adb_proxy_stop(aoahid_adb_proxy_context* proxy);
```

```c
aoahid_context_options co = {0};
co.struct_size = sizeof co;
co.event_mode = AOAHID_EVENT_INTERNAL_THREAD;          /* required */
/* aoahid_context_create -> aoahid_accessory_start -> aoahid_device_open */

aoahid_adb_proxy_context* proxy = NULL;
if (aoahid_adb_proxy_start(device, 6555, &proxy) == 0) {
    /* HID work with aoahid_node_* runs here, unaffected */
    aoahid_adb_proxy_stop(proxy);                       /* before aoahid_device_close */
}
```

| Return | Meaning |
|---|---|
| `0` | Success |
| `-1` | Null argument |
| `-2` | ADB interface unavailable: adb server holds it, USB debugging is off, or the device is not `18d1:2d01`. On Windows it can also be the phone's driver; see [Troubleshooting](docs/USAGE.md#troubleshooting) |
| `-3` | Socket setup failed |
| `-4` | Port in use |
| `-5` | `listen` failed |

Rules:
- Create the Context with `AOAHID_EVENT_INTERNAL_THREAD`. The proxy reads and writes from its own threads.
- Avoid ports 5555-5585. adb scans them for emulators at startup. `6555` is used in the examples.
- Call `stop` before `aoahid_device_close`.

## Build

Requires CMake 3.20+, a C++11 compiler, and Libaoa_hid 3.0.x.

```sh
# Recommended: an extracted official Libaoa_hid release package
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/libaoahid-3.0.0-<platform>-shared
cmake --build build --config Release

# Otherwise Libaoa_hid v3.0.0 is fetched from source (needs libusb 1.0.30+)
cmake -S . -B build && cmake --build build
```

Release archives contain this library, its header, and the example. At runtime, place the Libaoa_hid runtime (`aoahid` and `libusb-1.0`) next to them.

## Latency

- USB HID input goes over EP0 and never waits on the ADB path.
- USB I/O matches host adb. Each read asks for exactly the bytes still missing: the 24-byte header, then `data_length`. So a transfer completes the moment its data is in, and payloads move as single large transfers.
- Each apacket is forwarded as soon as it is complete, in either direction. On loopback this adds microseconds. `TCP_NODELAY` is on.
- Blocking waits wake immediately on data. The 100 ms timeout only bounds shutdown.

## More

- [docs/USAGE.md](docs/USAGE.md): step-by-step usage, keeping other devices on adb, troubleshooting
- [docs/DESIGN.md](docs/DESIGN.md): how it works, with AOSP references
- [CHANGELOG.md](CHANGELOG.md)

## Status

- Framing was checked against AOSP adb sources: current adbd, pre-2024 adbd, and legacy adbd, as well as the host USB readers and writers. Channel behavior was checked against the Libaoa_hid 3.0.0 sources.
- Tested with a real host `adb` (37.0.0) through the proxy to a fake device. The fake enforces pre-2024 adbd write framing and models USB IN transfers, including payloads with no zero-length packet. `connect`, `devices`, `shell`, packet-aligned payloads (512 and 4096 bytes), 1 MiB payloads, and reconnect all work, with zero framing violations and zero would-stall reads. A negative control with oversized reads is correctly flagged. Everything also runs clean under ASan and UBSan.
- **Verified end to end on real hardware** through [aoahid_player](https://github.com/nemarpuc/aoahid_player)'s ADB Bridge, with HID input running at the same time: a Samsung tablet and a HyperOS phone, each on Linux and on Windows. That app keeps the phone in its current USB mode (no accessory switch), so the accessory-mode path of `examples/basic_proxy.cpp` was not part of it. On Windows the HyperOS phone came up with WinUSB and worked as plugged in; the Samsung tablet first needed its dedicated Samsung driver replaced with WinUSB (see [Troubleshooting](docs/USAGE.md#troubleshooting)).

## License

MIT, see [LICENSE](LICENSE).
