# Contributing

Use the [issue tracker](https://github.com/nemarpuc/aoahid_adb_proxy/issues) for
proposed changes and submit patches through
[pull requests](https://github.com/nemarpuc/aoahid_adb_proxy/pulls).

## Rules

- **Framing follows adb.** The proxy re-frames the TCP stream into exact ADB
  packets, because `adbd` relies on USB transfer boundaries. A change to the
  reading or writing code must say which AOSP adb source it was checked against;
  [docs/DESIGN.md](docs/DESIGN.md) lists the references.
- **Nothing is defaulted.** `drain_timeout_ms` has no default value; zero from
  `aoahid_adb_proxy_start` is `AOAHID_ADB_PROXY_ERR_ARGUMENT`, as in libaoahid.
- **Standards.** The public header is C; the implementation is C++11. Keep it that way.
- **Return codes are stable.** The numbers of `AOAHID_ADB_PROXY_*` never change;
  a new failure gets a new number.

## Build and test

Tests need no phone: the proxy runs against an in-memory Channel and a loopback
TCP client.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/libaoahid
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

[docs/INTERNALS.md](docs/INTERNALS.md) explains the threads, the stop order, the
Channel contract, and the tests. It also describes the copies of this library
that other projects vendor.

## Releasing

Push a tag `vX.Y.Z`. The release workflow checks that `project(aoahid_adb_proxy
VERSION X.Y.Z ...)` in `CMakeLists.txt` and the `## X.Y.Z` heading in
`CHANGELOG.md` match the tag, builds six platform archives, writes `SHA256SUMS`,
and publishes them.

## Submitting changes

- Add a test for every new failure path or framing case.
- Run the whole CTest suite.
- Put a user-visible change in `CHANGELOG.md`.
- Say in the pull request what was run, and on which phone if one was involved. A
  claim that the proxy works with a phone needs the phone, the host OS, and the
  driver.

## Code of Conduct

Everyone taking part is expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
