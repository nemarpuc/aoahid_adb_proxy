# Changelog

## Unreleased

- The source fallback fetches libaoahid 4.2.0 (was 4.0.0) and CI builds against
  the 4.2.0 release assets. The proxy's own code and the `find_package`
  minimum (4.0) are unchanged.

## 3.2.0

- Release archives for macOS arm64 and x86_64 (`aoahid_adb_proxy-v3.2.0-macos-{arm64,x86_64}.tar.gz`),
  built against the libaoahid 4.1.0 macOS archives. CI runs the test suite on
  both. The source is unchanged: it already sets `SO_NOSIGPIPE` where
  `MSG_NOSIGNAL` does not exist. It has not been run against a phone on macOS.
- No API or ABI change.

## 3.1.3

- Back to the 3.1.1 behavior: when the Channel is lost the proxy closes its
  port, so `adb connect` is refused. 3.1.2 kept the port bound and closed
  each new connection, to keep another local program from binding the port
  while adb still listed it as the device. That is left to adb's user: adb
  does not verify what answers at an address, and the port is free after
  `aoahid_adb_proxy_stop` in any case. Closing the port needs no extra state.
- No API or ABI change.

## 3.1.2

- After the Channel is lost the proxy keeps its port and closes every new
  connection at once. 3.1.1 closed the port instead, which let another local
  program bind it while adb still listed `127.0.0.1:<port>` as the device, and
  answer in the device's place. Use 3.1.2 instead of 3.1.1. `adb connect`
  still fails promptly rather than hanging, as in 3.1.1.
- No API or ABI change.

## 3.1.1

- When reading from the device fails (unplugged, or any USB read error), the
  proxy now closes its port as it stops serving. `adb connect` is refused at
  once; before, the port kept accepting connections that nothing answered
  until `aoahid_adb_proxy_stop` was called. `stop` is still required to
  release the Channel. A test covers it.
- The libaoahid source fallback (`FetchContent`) is pinned to the commit of
  `v4.0.0` instead of the tag name.
- Added `docs/INTERNALS.md`: invariants, thread and shutdown order, the
  Channel contract the proxy relies on, and how to test and vendor it.
- No API or ABI change.

## 3.1.0

- The header names the `aoahid_adb_proxy_start` results: `AOAHID_ADB_PROXY_OK`
  and `AOAHID_ADB_PROXY_ERR_ARGUMENT`, `_INTERFACE`, `_SOCKET`, `_BIND`,
  `_LISTEN`, `_RESOURCE`. The numbers (`0`, `-1` to `-6`) are unchanged.
- `*out_proxy` is set to `NULL` when `device` is null too; before, that one
  failure left it untouched.
- Tests: `tests/test_proxy.cpp` runs the proxy against an in-memory Channel
  and a loopback TCP client, with no phone. Built by default
  (`AOAHID_ADB_PROXY_BUILD_TESTS`), run with `ctest`, and run in CI on Linux
  and Windows x86_64.
- No behavior change otherwise: the ADB header offsets are named constants and
  `start` releases what it acquired through one helper.

## 3.0.3

- A payload whose header already reached the device is always written, even
  when the client disconnects or `stop()` runs in between. Before, the header
  could go out alone and adbd then read the next packet as its payload.
- The example opens the phone in its current USB mode; switching to accessory
  mode is the application's choice.
- Docs: `-4` covers every bind failure (in use, reserved, not permitted); after
  a USB read error the proxy stops serving and must be restarted; `stop()`
  timing includes finishing a started payload and the Channel close; the adb
  server has to be restarted after the proxy stops (with adb source
  references).

## 3.0.1

- Docs: Windows `adb.exe` keeps seeing the phone after its driver is switched to WinUSB; the docs said it no longer did. Only tools that need the manufacturer's driver (and possibly MTP) are affected.
- Docs: record the verified hardware (Samsung Galaxy Tab S11 and POCO F6 Pro, on Windows 10 x64 and Arch Linux, with WinUSB and libusbK).
- The default branch is now `main`.

## 3.0.0

- Requires libaoahid 4.0. The fallback fetch and CI use libaoahid 4.0.0, and an installed libaoahid 3.x is no longer accepted. The proxy's own API is unchanged.
- The example no longer sets `startup_mode`, which libaoahid 4.0 removed.

## 2.1.0

- Fixed: a client that disconnected while the device was still sending could kill the host process with `SIGPIPE` on Linux and macOS. Sends now use `MSG_NOSIGNAL` (`SO_NOSIGPIPE` where that is the mechanism), so the session just ends.
- Fixed: outside Windows, socket waits use `poll` instead of `select`, so a socket whose descriptor number is `FD_SETSIZE` (1024) or higher, as in an application with many open files, no longer overruns `fd_set`.
- Fixed: `aoahid_adb_proxy_start` no longer lets an allocation or thread-creation failure escape as a C++ exception through the C API. It returns the new code `-6` instead, and a session whose threads cannot start drops only that client.
- The two packet buffers are allocated once when the proxy starts and are not zero-filled, so only the pages payloads actually use become resident, and a session allocates nothing.
- The header's version macros match the release again (2.0.1 and 2.0.2 still said 2.0.0), and CI now checks them against `CMakeLists.txt`.
- The fallback fetch and CI use libaoahid 3.0.5.

## 2.0.2

- libaoahid's repository is now `nemarpuc/libaoahid` (was `nemarpuc/Libaoa_hid`); links, CI, and the fallback fetch use the new name.
- The fallback fetch and CI use libaoahid 3.0.4, which includes 3.0.2's fix for a Channel losing its claim on Windows. An installed libaoahid 3.0.x is still accepted.

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
