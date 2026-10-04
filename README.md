# aoahid_adb_proxy

Use `adb` and [libaoahid](https://github.com/nemarpuc/libaoahid) on the same Android device at the same time.

On Windows, a USB device can be opened by only one process (WinUSB is exclusive), so `adb.exe` and a libaoahid app cannot share a phone. This small library (C API, C++11 implementation) runs inside your libaoahid app: the app owns the USB device, and the proxy forwards the ADB interface to a local TCP port that `adb connect` can use.

```text
adb ──TCP──▶ [your app + aoahid_adb_proxy] ──USB bulk (ADB)──▶ adbd
                         └────────── USB EP0 (AOA HID) ──────▶ input
```

On Windows this works with the phone's driver on WinUSB or libusbK, the drivers libusb-based tools such as libaoahid need. The ADB interface is a bulk interface, so the whole device has to be on one of them. If the manufacturer ships its own driver (Samsung is the case verified here), replacing it with WinUSB in [Zadig](https://zadig.akeo.ie/) may fix a failing start; see [Troubleshooting](docs/USAGE.md#troubleshooting). HID input alone does not need the change.

It is a **library**, not a standalone tool. `examples/basic_proxy.cpp` is a complete, runnable example. The proxy works in whatever USB mode the application opened the phone in: its current mode, or AOA accessory mode if the application chose to switch with `aoahid_accessory_start`. USB debugging must be on either way.

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
/* aoahid_context_create -> aoahid_device_open (current USB mode, or after
   aoahid_accessory_start if the device needs accessory mode) */

aoahid_adb_proxy_context* proxy = NULL;
if (aoahid_adb_proxy_start(device, 6555, &proxy) == AOAHID_ADB_PROXY_OK) {
    /* HID work with aoahid_node_* runs here, unaffected */
    aoahid_adb_proxy_stop(proxy);                       /* before aoahid_device_close */
}
```

| Return | Value | Meaning |
|---|---|---|
| `AOAHID_ADB_PROXY_OK` | `0` | Success |
| `AOAHID_ADB_PROXY_ERR_ARGUMENT` | `-1` | Null argument |
| `AOAHID_ADB_PROXY_ERR_INTERFACE` | `-2` | ADB interface unavailable: adb server holds it, or USB debugging is off (the phone then has no ADB interface, in either USB mode). On Windows it can also be the phone's driver; see [Troubleshooting](docs/USAGE.md#troubleshooting) |
| `AOAHID_ADB_PROXY_ERR_SOCKET` | `-3` | Socket setup failed |
| `AOAHID_ADB_PROXY_ERR_BIND` | `-4` | The port could not be bound: in use, reserved (Windows excluded port ranges), or not permitted |
| `AOAHID_ADB_PROXY_ERR_LISTEN` | `-5` | `listen` failed |
| `AOAHID_ADB_PROXY_ERR_RESOURCE` | `-6` | Out of memory, or no thread could be started |

The numbers are stable; the names were added in 3.1.0. `*out_proxy` is `NULL` on every failure.

Rules:
- Create the Context with `AOAHID_EVENT_INTERNAL_THREAD`. The proxy reads and writes from its own threads.
- Avoid ports 5555-5585. adb scans them for emulators at startup. `6555` is used in the examples.
- Call `stop` before `aoahid_device_close`.
- After stopping, run `adb kill-server`. On Linux and macOS an adb server that saw the phone while the proxy held it does not retry it over USB; see [docs/USAGE.md](docs/USAGE.md) for the adb source references.

## Build

Requires CMake 3.20+, a C++11 compiler, and libaoahid 4.0.x.

```sh
# Recommended: an extracted official libaoahid release package
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/libaoahid-4.0.0-<platform>-shared
cmake --build build --config Release

# Otherwise libaoahid v4.0.0 is fetched from source (needs libusb 1.0.30+)
cmake -S . -B build && cmake --build build
```

Tests are built by default and need no phone: the proxy source runs against an in-memory Channel and a loopback TCP client.

```sh
ctest --test-dir build -C Release --output-on-failure
```

`-DAOAHID_ADB_PROXY_BUILD_TESTS=OFF` and `-DAOAHID_ADB_PROXY_BUILD_EXAMPLES=OFF` skip the tests and the example.

Release archives contain this library, its header, and the example. At runtime, place the libaoahid runtime (`aoahid` and `libusb-1.0`) next to them.

## Latency

- USB HID input goes over EP0 and never waits on the ADB path.
- USB I/O matches host adb. Each read asks for exactly the bytes still missing: the 24-byte header, then `data_length`. So a transfer completes the moment its data is in, and payloads move as single large transfers.
- Each apacket is forwarded as soon as it is complete, in either direction. On loopback this adds microseconds. `TCP_NODELAY` is on.
- Blocking waits wake immediately on data. The 100 ms timeout only bounds shutdown.

## More

- [docs/USAGE.md](docs/USAGE.md): step-by-step usage, keeping other devices on adb, troubleshooting
- [docs/DESIGN.md](docs/DESIGN.md): how it works, with AOSP references
- [docs/INTERNALS.md](docs/INTERNALS.md): for maintainers: state and threads, stop order, the Channel contract, tests, vendoring
- [CHANGELOG.md](CHANGELOG.md)

## Status

- Framing was checked against AOSP adb sources: current adbd, pre-2024 adbd, and legacy adbd, as well as the host USB readers and writers. Channel behavior was checked against the libaoahid sources.
- CI runs `tests/test_proxy.cpp` on Linux, macOS (arm64 and x86_64), and Windows x86_64: start failures, the Channel options, header and payload as separate USB writes, USB packets arriving in pieces, rejected client headers, a reconnect in the middle of a packet, and a lost Channel.
- Tested with a real host `adb` (37.0.0) through the proxy to a fake device. The fake enforces pre-2024 adbd write framing and models USB IN transfers, including payloads with no zero-length packet. `connect`, `devices`, `shell`, packet-aligned payloads (512 and 4096 bytes), 1 MiB payloads, and reconnect all work, with zero framing violations and zero would-stall reads. A negative control with oversized reads is correctly flagged. Everything also runs clean under ASan and UBSan.
- On macOS the proxy is built and its test suite runs in CI (arm64 and x86_64), but it has not been run against a phone.
- **Verified end to end on real hardware** through [aoahid_player](https://github.com/nemarpuc/aoahid_player)'s ADB Bridge, with HID input running at the same time: a Samsung Galaxy Tab S11 and a POCO F6 Pro (HyperOS), each on Windows 10 x64 and Arch Linux, with the phone's driver on WinUSB and on libusbK on Windows. That app keeps the phone in its current USB mode (no accessory switch), so the accessory-mode path was not part of it. On Windows the HyperOS phone came up with WinUSB and worked as plugged in; the Samsung tablet first needed its dedicated Samsung driver replaced with WinUSB (see [Troubleshooting](docs/USAGE.md#troubleshooting)).

## License

MIT, see [LICENSE](LICENSE).
