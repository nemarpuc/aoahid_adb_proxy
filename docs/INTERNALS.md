# Internals

What a maintainer needs beyond [DESIGN.md](DESIGN.md) (why the proxy frames
data the way it does) and [USAGE.md](USAGE.md) (how to run it): the state the
code keeps, which thread touches it, the order things stop in, and how to
test and vendor it.

Source references are `path#Lnnn` and are valid at v3.1.1.

## Shape

One source file, `src/aoahid_adb_proxy.cpp#L48`,
and two exported functions. It links libaoahid and uses four of its calls:
`aoahid_channel_open`, `aoahid_channel_read`, `aoahid_channel_write`, and
`aoahid_channel_close`. Everything else is sockets and C++11 threads.

| Thread | Function | Lives |
| --- | --- | --- |
| accept | `src/aoahid_adb_proxy.cpp#L225` | From `start` until `stop`, or until the Channel is lost. |
| tx (TCP to USB) | `src/aoahid_adb_proxy.cpp#L140` | One per client session. |
| rx (USB to TCP) | `src/aoahid_adb_proxy.cpp#L175` | One per client session. |

The accept thread runs `src/aoahid_adb_proxy.cpp#L198` itself, so one
client is served at a time. The listen backlog is 1.

## State and who owns it

| State | Owner | Meaning |
| --- | --- | --- |
| `running` (atomic) | Set by `start`. Cleared by `stop`, and by the rx thread when the Channel is lost. | False ends every loop. |
| `Session::alive` (atomic) | Cleared by whichever of tx or rx ends first. | False ends the other one. |
| `rx_packet`, `rx_have`, `rx_need` | The rx thread of the current session. | The packet being assembled from USB. It lives in the context, not the session, so it survives a client. |
| `tx_payload` | The tx thread of the current session. | One payload read from TCP before it is written to USB. |
| `listen_sock` | The accept thread. | Closed by that thread when its loop ends. |

`rx_packet` is 24 bytes plus 1 MiB and `tx_payload` is 1 MiB. Both are
allocated uninitialized at `start`, so only the pages a payload touches become
resident.

Sessions do not overlap, and starting a thread orders it after everything the
previous session's threads did, so `rx_packet` needs no lock.

## Invariants

1. A header is validated before anything reaches the device: `magic` must be
   `command ^ 0xFFFFFFFF` and `data_length` at most `MAX_PAYLOAD`
   (`src/aoahid_adb_proxy.cpp#L146`). A client that fails
   this is dropped and nothing is written.
2. The header and the payload are separate `aoahid_channel_write` calls, so
   they are separate USB transfers.
3. Once a header has been written its payload is always written, even when
   the client disconnects or `stop` runs in between
   (`src/aoahid_adb_proxy.cpp#L119`, `finish`). Only a write
   that times out without moving a byte, while the session is ending, gives up.
4. A client never receives part of a packet: the rx thread sends a packet
   only when it is whole, and a packet a previous client left half read is
   finished and discarded (`src/aoahid_adb_proxy.cpp#L178`).
5. An IN header that announces more than `MAX_PAYLOAD` means framing is lost.
   The rx thread clears `running`; the proxy cannot resynchronize.

## Timing constants

| Constant | Value | Bounds |
| --- | --- | --- |
| `kPollMs` | 100 ms | How long a socket wait or a USB read waits before `running` and `alive` are checked again. It bounds how long `stop` takes. It is not a polling interval: both waits return as soon as data arrives. |
| `kUsbWriteMs` | 1000 ms | How long one `aoahid_channel_write` waits for a free OUT transfer. |
| `kMaxPayload` | 1 MiB | `MAX_PAYLOAD` in AOSP `adb.h`; also the Channel's `transfer_bytes`. |
| `kHeaderSize` | 24 | The size of the ADB message header. |

## The Channel contract the proxy relies on

From `aoahid.h` (libaoahid 4.x):

- The Context must be in `AOAHID_EVENT_INTERNAL_THREAD` mode. Only then may
  one thread read while another writes.
- `read_mode = AOAHID_CHANNEL_READ_REQUEST`: a read with nothing buffered
  submits one IN transfer sized from its capacity. A read that times out
  leaves that transfer pending and the next read continues it, so the 100 ms
  timeout loses no bytes.
- `aoahid_channel_write` reports the bytes it queued in `*out_written` on
  every return, including a timeout. `usb_write_all` resumes after them.
- `zero_length_termination = 1`: a write whose length is a multiple of
  `wMaxPacketSize` ends with a zero-length packet. If a write times out with
  every byte queued and only that packet pending, there is no call to send it
  alone, so the session is dropped.
- An OUT error is reported by the next write, not the one that caused it.
- A failed IN transfer loses the Channel for good. That is the only way the
  proxy learns the device is gone.
- `aoahid_channel_close` must not overlap a read or a write. `stop` joins the
  accept thread, which has joined tx and rx, before it closes the Channel.
- `aoahid_last_error()` is thread-local. After
  `AOAHID_ADB_PROXY_ERR_INTERFACE` the calling thread's last error is the
  failed `aoahid_channel_open`, because `start` makes no other libaoahid call
  after it. aoahid_player reads `libusb_status` there to tell a driver problem
  from an adb server holding the interface.

## Stop order

`stop` clears `running`, joins the accept thread, closes the Channel, and
frees the context (`src/aoahid_adb_proxy.cpp#L320`).

Inside `serve`, tx is joined first. Then the client socket is shut down, which
releases an rx thread blocked in `send` to a client that stopped reading. Then
rx is joined.

A session ends when either side ends:

| Event | First to notice | Result |
| --- | --- | --- |
| Client closes | tx (`recv` returns 0), within 100 ms | Session ends; the proxy keeps listening. |
| Client sends a bad header | tx | Same. |
| USB write fails | tx | Same. The next client can connect. |
| USB read fails | rx | `running` is cleared. The session ends, the accept loop ends, and the port closes. |
| `stop` | Every loop, within 100 ms | A payload whose header is already on the wire is written first (up to 1 s if the device has stopped reading). |

Since 3.1.1 the accept thread closes the listening socket when its loop ends,
so after a lost Channel the port refuses connections. The context stays
allocated until `stop`.

## Sockets

- The listener binds `127.0.0.1` only. Any local process can connect; ADB's
  own key authorization on the device still applies.
- `SO_REUSEADDR` is set on POSIX and not on Windows, where it would let
  another process bind the same port.
- Waits use `poll` on POSIX (no `FD_SETSIZE` limit) and `select` on Windows.
- `MSG_NOSIGNAL`, or `SO_NOSIGPIPE` where that is the mechanism, keeps a
  vanished client from raising `SIGPIPE`.
- A `send` or `recv` interrupted by a signal (`EINTR`) ends the session. A
  host that installs handlers without `SA_RESTART` can see that.
- `TCP_NODELAY` is set on the client socket.
- On Windows `start` calls `WSAStartup` and `stop` calls `WSACleanup`.

## Tests

`tests/test_proxy.cpp#L404` builds the proxy source together with its
own definitions of the four `aoahid_channel_*` functions, so it needs
libaoahid's headers but neither the library nor a device. The fake Channel is
a byte queue for IN and a list with one entry per `aoahid_channel_write` call
for OUT, which is how the tests check transfer boundaries. A loopback TCP
client plays adb.

It covers the start failures, the Channel options, TCP to USB framing under
arbitrary TCP segmentation, USB to TCP reassembly, a bad client header, a
reconnect in the middle of a packet, a lost Channel, and that the port is free
after `stop`.

Run it with `ctest --test-dir build`. CI runs it on Linux and Windows x86_64
and builds, without running, the cross-compiled targets.

Not covered by a test: a USB write that times out, and the `finish` path.

## Versions

The version is in two places, which CI compares:
`CMakeLists.txt#L2` and the three
`AOAHID_ADB_PROXY_VERSION_*` macros in
`include/aoahid_adb_proxy.h#L10`.

A pushed `v*` tag runs `.github/workflows/release.yml`. It requires the tag to
match the CMake version and `CHANGELOG.md` to have a `## X.Y.Z` section, which
becomes the release notes.

libaoahid is found as an installed package (`find_package(aoahid 4.0)`). When
none is found, its source is fetched at a pinned commit
(`CMakeLists.txt#L22`). CI builds against the 4.0.0 release assets, the
oldest version supported.

## Vendoring

aoahid_player carries a copy of `src/aoahid_adb_proxy.cpp` and
`include/aoahid_adb_proxy.h` under `third_party/aoahid_adb_proxy/` and
compiles it into its core library. After a release here:

1. Copy both files over the vendored ones, unmodified.
2. Update the version and commit named in the comment above the vendored
   source in that project's `src/CMakeLists.txt`, and its changelog.
3. Check that the two trees are identical (`diff -r`).

## Sources

- ADB message header (24 bytes: `command`, `arg0`, `arg1`, `data_length`,
  `data_crc32`, `magic`; `magic` is `command ^ 0xffffffff`): AOSP
  `packages/modules/adb`, `docs/dev/protocol.md`,
  <https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/dev/protocol.md>.
- `MAX_PAYLOAD = 1024 * 1024`, `ADB_CLASS 0xff`, `ADB_SUBCLASS 0x42`,
  `ADB_PROTOCOL 0x1`: AOSP `packages/modules/adb`, `adb.h`,
  <https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/adb.h>.
- adbd accepting any split of header and payload across USB transfers: AOSP
  `packages/modules/adb` commit `4af6e4ff6ff587b344236c30cb3d6765cb1de6be`
  ("Make daemon resilient to unbound bulk transfer", 2024-09-30),
  <https://android.googlesource.com/platform/packages/modules/adb/+/4af6e4ff6ff587b344236c30cb3d6765cb1de6be>.
- The adb server scanning odd-numbered ports 5555 to 5585 for emulators:
  <https://developer.android.com/tools/adb>.

All retrieved 2026-10-04.
