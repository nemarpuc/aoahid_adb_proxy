# aoahid_adb_proxy

Use `adb` and [Libaoa_hid](https://github.com/nemarpuc/Libaoa_hid) on the same Android device at the same time.

On Windows, a USB device can be opened by only one process (WinUSB is exclusive), so `adb.exe` and a Libaoa_hid app cannot share a phone. This small C library runs inside your Libaoa_hid app: the app owns the USB device, and the proxy forwards the ADB interface to a local TCP port that `adb connect` can use.

```text
adb ──TCP──▶ [your app + aoahid_adb_proxy] ──USB bulk (ADB)──▶ adbd
                         └────────── USB EP0 (AOA HID) ──────▶ input
```

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
| `-2` | ADB interface unavailable: adb server holds it, USB debugging is off, or the device is not `18d1:2d01` |
| `-3` | Socket setup failed |
| `-4` | Port in use |
| `-5` | `listen` failed |

Rules:
- Create the Context with `AOAHID_EVENT_INTERNAL_THREAD`. The proxy reads and writes from its own threads.
- Avoid ports 5555-5585. adb scans them for emulators at startup. `6555` is used in the examples.
- Call `stop` before `aoahid_device_close`.

## Build

Requires CMake 3.20+, a C++11 compiler, and Libaoa_hid 2.0.x.

```sh
# Recommended: an extracted official Libaoa_hid release package
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/libaoahid-2.0.1-<platform>-shared
cmake --build build --config Release

# Otherwise Libaoa_hid v2.0.1 is fetched from source (needs libusb 1.0.30+)
cmake -S . -B build && cmake --build build
```

Release archives contain this library, its header, and the example. At runtime, place the Libaoa_hid runtime (`aoahid` and `libusb-1.0`) next to them.

## Latency

- USB HID input goes over EP0 and never waits on the ADB path.
- Device-to-host bytes are sent to TCP as soon as they arrive. USB transfers are one packet each, so nothing waits for a transfer to fill.
- Host-to-device packets are written as soon as their payload is complete, the same way host adb writes them. On loopback this adds microseconds. `TCP_NODELAY` is on.
- Blocking waits wake immediately on data. The 100 ms timeout only bounds shutdown.

## More

- [docs/USAGE.md](docs/USAGE.md): step-by-step usage, keeping other devices on adb, troubleshooting
- [docs/DESIGN.md](docs/DESIGN.md): how it works, with AOSP references
- [CHANGELOG.md](CHANGELOG.md)

## Status

- Framing was checked against AOSP adb sources: current adbd, pre-2024 adbd, and legacy adbd, as well as the host USB writers. Channel behavior was checked against the Libaoa_hid 2.0.1 sources.
- Tested with a real host `adb` (37.0.0) through the proxy to a fake device that enforces pre-2024 adbd framing: `connect`, `devices`, `shell`, and reconnect all work, with zero framing violations. Stub tests cover partial-write resume and malformed headers. Everything also runs clean under ASan, UBSan, and TSan.
- **Not yet verified end-to-end on real hardware** (Windows or Linux). Libaoa_hid also lists simultaneous ADB Channel + HID on `2d01` as not yet hardware-tested.

## License

MIT, see [LICENSE](LICENSE).
