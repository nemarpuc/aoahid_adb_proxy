#include "aoahid_adb_proxy.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
typedef int socklen_t;
typedef int io_len_t;
#define SHUT_RDWR SD_BOTH
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_t;
typedef size_t io_len_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

// AOSP packages/modules/adb: sizeof(amessage), MAX_PAYLOAD
static const size_t kHeaderSize = 24;
static const uint32_t kMaxPayload = 1024 * 1024;
static const uint32_t kPollMs = 100;
// adbd keeps USB reads queued, so a pool that stays full this long means a stuck device.
static const uint32_t kUsbWriteMs = 1000;

// A peer that has gone away must fail send() with EPIPE, not kill the host
// process with SIGPIPE.
#ifdef MSG_NOSIGNAL
static const int kSendFlags = MSG_NOSIGNAL;
#else
static const int kSendFlags = 0;
#endif

struct aoahid_adb_proxy_context {
    aoahid_channel* channel;
    socket_t listen_sock;
    std::atomic<bool> running;
    std::thread accept_thread;
    // USB -> TCP packet being assembled; survives a session so a new client
    // never starts mid-packet. Only the rx thread touches it while it runs.
    std::unique_ptr<uint8_t[]> rx_packet;
    size_t rx_have;
    size_t rx_need;
    // TCP -> USB payload; only the tx thread touches it.
    std::unique_ptr<uint8_t[]> tx_payload;
};

namespace {

struct Session {
    aoahid_adb_proxy_context* ctx;
    socket_t sock;
    std::atomic<bool> alive;

    bool active() const { return alive.load() && ctx->running.load(); }
};

bool wait_readable(socket_t sock, uint32_t ms) {
#ifdef _WIN32
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    timeval tv;
    tv.tv_sec = static_cast<long>(ms / 1000);
    tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
    return select(0, &fds, nullptr, nullptr, &tv) > 0;
#else
    // poll, unlike select, has no FD_SETSIZE limit on the descriptor value.
    pollfd fd;
    fd.fd = sock;
    fd.events = POLLIN;
    fd.revents = 0;
    return poll(&fd, 1, static_cast<int>(ms)) > 0;
#endif
}

// Returns false on disconnect, error, or shutdown.
bool recv_exact(Session& s, uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        if (!s.active()) return false;
        if (!wait_readable(s.sock, kPollMs)) continue;
        const auto n = recv(s.sock, reinterpret_cast<char*>(buf + got), static_cast<io_len_t>(len - got), 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

bool send_all(socket_t sock, const uint8_t* buf, size_t len) {
    while (len > 0) {
        const auto n = send(sock, reinterpret_cast<const char*>(buf), static_cast<io_len_t>(len), kSendFlags);
        if (n <= 0) return false;
        buf += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// A timeout keeps the queued prefix and resumes with the rest. If only the ZLP
// is left pending there is no API to send it alone, so the session is dropped.
bool usb_write_all(Session& s, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (s.active()) {
        size_t written = 0;
        aoahid_result r = aoahid_channel_write(s.ctx->channel, data + off, len - off, &written, kUsbWriteMs);
        if (r == AOAHID_OK) return true;
        if (r != AOAHID_ERR_TIMEOUT || written == len - off) return false;
        off += written;
    }
    return false;
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// TCP -> USB, sent the way host adb does (client/usb_*.cpp): the header as one
// write, then the whole payload as one write. adbd before AOSP 4af6e4ff rejects
// other splits, and legacy aio adbd needs the payload in one contiguous run.
void tx_loop(Session* s) {
    uint8_t* payload = s->ctx->tx_payload.get();
    uint8_t header[kHeaderSize];
    while (recv_exact(*s, header, kHeaderSize)) {
        uint32_t command = le32(header);
        uint32_t length = le32(header + 12);
        if ((command ^ 0xFFFFFFFFu) != le32(header + 20) || length > kMaxPayload) break;
        if (!recv_exact(*s, payload, length)) break;
        if (!usb_write_all(*s, header, kHeaderSize)) break;
        if (length && !usb_write_all(*s, payload, length)) break;
    }
    s->alive = false;
}

// Reads until `need` bytes of the current packet are in. Request-mode reads
// size each USB transfer to what is still missing, as host adb does, so a
// packet-aligned payload with no ZLP completes as soon as it is full.
bool usb_read_to(Session& s, size_t need) {
    aoahid_adb_proxy_context* c = s.ctx;
    while (c->rx_have < need) {
        if (!s.active()) return false;
        size_t got = 0;
        aoahid_result r =
            aoahid_channel_read(c->channel, &c->rx_packet[c->rx_have], need - c->rx_have, &got, kPollMs);
        if (r == AOAHID_ERR_TIMEOUT) continue;
        if (r != AOAHID_OK) {
            c->running = false;  // the Channel is lost
            return false;
        }
        c->rx_have += got;
    }
    return true;
}

// USB -> TCP, one whole apacket per send: header, then exactly data_length.
void rx_loop(Session* s) {
    aoahid_adb_proxy_context* c = s->ctx;
    // Finish, but drop, a packet the previous client was receiving.
    bool drop = c->rx_have != 0;
    while (s->active()) {
        if (c->rx_have < kHeaderSize) {
            if (!usb_read_to(*s, kHeaderSize)) break;
            uint32_t length = le32(&c->rx_packet[12]);
            if (length > kMaxPayload) {
                c->running = false;  // framing lost; host adb would reject it too
                break;
            }
            c->rx_need = kHeaderSize + length;
        }
        if (!usb_read_to(*s, c->rx_need)) break;
        size_t n = c->rx_need;
        c->rx_have = 0;
        if (!drop && !send_all(s->sock, c->rx_packet.get(), n)) break;
        drop = false;
    }
    s->alive = false;
}

void serve(aoahid_adb_proxy_context* ctx, socket_t client) {
    int one = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    Session s;
    s.ctx = ctx;
    s.sock = client;
    s.alive = true;
    std::thread tx;
    std::thread rx;
    try {
        tx = std::thread(tx_loop, &s);
        rx = std::thread(rx_loop, &s);
    } catch (...) {
        // No thread could be started for this client; drop it.
        s.alive = false;
    }
    if (tx.joinable()) tx.join();
    // Unblocks an rx thread stuck in send() to a client that stopped reading.
    shutdown(client, SHUT_RDWR);
    if (rx.joinable()) rx.join();
    closesocket(client);
}

void accept_loop(aoahid_adb_proxy_context* ctx) {
    while (ctx->running) {
        if (!wait_readable(ctx->listen_sock, kPollMs)) continue;
        socket_t client = accept(ctx->listen_sock, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        serve(ctx, client);
    }
}

}  // namespace

int aoahid_adb_proxy_start(aoahid_device* device, uint16_t tcp_port, aoahid_adb_proxy_context** out_proxy) {
    if (!device || !out_proxy) return -1;
    *out_proxy = nullptr;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -3;
#endif

    aoahid_channel_options opt;
    std::memset(&opt, 0, sizeof(opt));
    opt.struct_size = sizeof(opt);
    opt.interface_class = 0xFF;  // AOSP adb.h ADB_CLASS/SUBCLASS/PROTOCOL
    opt.interface_subclass = 0x42;
    opt.interface_protocol = 0x01;
    // Mirrors host adb: each IN transfer asks for exactly the bytes still
    // missing (adbd sends no ZLP after a packet-aligned payload), and each
    // payload goes out as one OUT transfer (up to MAX_PAYLOAD) ending in a ZLP
    // when packet-aligned, which legacy adbd expects.
    opt.read_mode = AOAHID_CHANNEL_READ_REQUEST;
    opt.transfer_bytes = kMaxPayload;
    opt.out_transfers = 2;  // the next header can queue behind a payload
    opt.zero_length_termination = 1;

    aoahid_channel* channel = nullptr;
    aoahid_adb_proxy_context* ctx = nullptr;
    int err = 0;
    socket_t sock = INVALID_SOCKET;
    if (aoahid_channel_open(device, &opt, &channel) != AOAHID_OK) {
        err = -2;
    } else if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        err = -3;
    } else {
#ifndef _WIN32
        // Windows SO_REUSEADDR would let another process steal the port.
        int one = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(tcp_port);
        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            err = -4;
        } else if (listen(sock, 1) == SOCKET_ERROR) {
            err = -5;
        } else {
            ctx = new (std::nothrow) aoahid_adb_proxy_context();
            if (!ctx) {
                err = -6;
            } else {
                ctx->channel = channel;
                ctx->listen_sock = sock;
                ctx->running = true;
                // Left uninitialized, so only the pages a payload uses become resident.
                ctx->rx_packet.reset(new (std::nothrow) uint8_t[kHeaderSize + kMaxPayload]);
                ctx->tx_payload.reset(new (std::nothrow) uint8_t[kMaxPayload]);
                ctx->rx_have = 0;
                ctx->rx_need = 0;
                if (!ctx->rx_packet || !ctx->tx_payload) {
                    err = -6;
                } else {
                    try {
                        ctx->accept_thread = std::thread(accept_loop, ctx);
                    } catch (...) {
                        err = -6;
                    }
                }
            }
        }
    }
    if (err != 0) {
        delete ctx;
        if (sock != INVALID_SOCKET) closesocket(sock);
        if (channel) aoahid_channel_close(channel);
#ifdef _WIN32
        WSACleanup();
#endif
        return err;
    }
    *out_proxy = ctx;
    return 0;
}

void aoahid_adb_proxy_stop(aoahid_adb_proxy_context* proxy) {
    if (!proxy) return;
    proxy->running = false;
    if (proxy->accept_thread.joinable()) proxy->accept_thread.join();
    closesocket(proxy->listen_sock);
    aoahid_channel_close(proxy->channel);
    delete proxy;
#ifdef _WIN32
    WSACleanup();
#endif
}
