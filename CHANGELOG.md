# Changelog

## 0.1.0

The first release of this line. Builds against libaoahid 0.1.0; before 1.0.0 a change of the
minor number may change the API or the ABI.

- Forwards a phone's ADB bulk interface, through libaoahid's Channels, to a local TCP port
  (`aoahid_adb_proxy_start(device, tcp_port, drain_timeout_ms, &proxy)` and
  `aoahid_adb_proxy_stop(proxy, drain_timeout_ms)`; `drain_timeout_ms` has no default and zero
  from `start` is `AOAHID_ADB_PROXY_ERR_ARGUMENT`). Three threads (accept, TCP to USB, USB to
  TCP); the TCP stream is re-framed into exact ADB packets (the 24-byte header as one write, the
  payload as another), because `adbd` relies on USB transfer boundaries.
- Verification: an earlier version of this proxy ran against a Samsung Galaxy Tab S11 and a POCO
  F6 Pro (HyperOS), on Windows 10 x64 and Arch Linux, with WinUSB and libusbK; see
  docs/USAGE.md for what each driver needs. **This version has not been run against a phone**:
  its tests use a fake libaoahid Channel and a real host `adb` against a fake device.
- Shared library `SOVERSION` is the major version (0 for now).
