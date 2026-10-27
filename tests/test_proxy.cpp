// Runs the proxy against an in-memory Channel and a loopback TCP client: the
// four aoahid_channel_* functions it calls are defined here, so no libaoahid
// library and no USB device are involved.
#include "aoahid_adb_proxy.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
typedef int io_len_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int socket_t;
typedef size_t io_len_t;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

struct aoahid_channel {
    int unused;
};

namespace {

int g_failures = 0;

#define CHECK(expression)                                                                   \
    do {                                                                                    \
        if (!(expression)) {                                                                \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
            ++g_failures;                                                                   \
        }                                                                                   \
    } while (false)

typedef std::vector<uint8_t> Bytes;

// The fake Channel. `in` is what the device has sent and the proxy has not
// read yet; `writes` holds one entry per aoahid_channel_write call.
std::mutex g_mutex;
std::condition_variable g_cv;
aoahid_channel g_channel;
aoahid_channel_options g_options;
aoahid_result g_open_result = AOAHID_OK;
aoahid_result g_read_error = AOAHID_OK;
std::deque<uint8_t> g_in;
std::vector<Bytes> g_writes;
// What the next aoahid_channel_write calls do, one entry per call: a count
// queues only that many bytes and reports a timeout, kWhole takes the call
// as usual. Empty means every call is taken whole.
const size_t kWhole = static_cast<size_t>(-1);
std::deque<size_t> g_write_script;
int g_opens = 0;
int g_closes = 0;
// The drain wait the proxy passed to the last aoahid_channel_open / aoahid_channel_close.
uint32_t g_drain_timeout_ms = 0;
uint32_t g_close_drain_timeout_ms = 0;
const uint32_t kDrainTimeoutMs = 1234;

void device_sends(const Bytes& bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_in.insert(g_in.end(), bytes.begin(), bytes.end());
    g_cv.notify_all();
}

void device_fails(aoahid_result error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_read_error = error;
    g_cv.notify_all();
}

void script_writes(std::initializer_list<size_t> steps) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_write_script.assign(steps);
}

size_t write_count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_writes.size();
}

Bytes write_at(size_t index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_writes[index];
}

int close_count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_closes;
}

template <typename Predicate> bool eventually(Predicate done) {
    for (int tries = 0; tries < 500; ++tries) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

void put_le32(Bytes& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<uint8_t>(value >> shift));
}

const uint32_t kWrite = 0x45545257u;  // "WRTE"

Bytes header(uint32_t command, uint32_t length, uint32_t magic) {
    Bytes out;
    put_le32(out, command);
    put_le32(out, 1);  // arg0
    put_le32(out, 2);  // arg1
    put_le32(out, length);
    put_le32(out, 0);  // data_check
    put_le32(out, magic);
    return out;
}

Bytes header(uint32_t length) { return header(kWrite, length, kWrite ^ 0xFFFFFFFFu); }

Bytes payload(size_t length, uint8_t first) {
    Bytes out(length);
    for (size_t i = 0; i < length; ++i) out[i] = static_cast<uint8_t>(first + i);
    return out;
}

Bytes joined(const Bytes& a, const Bytes& b) {
    Bytes out(a);
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

sockaddr_in loopback(uint16_t port) {
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return addr;
}

// A listening socket on a port the system picked.
socket_t listen_anywhere(uint16_t* port) {
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = loopback(0);
    socklen_t length = sizeof(addr);
    if (sock == INVALID_SOCKET || bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(sock, 1) != 0 || getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &length) != 0) {
        std::fprintf(stderr, "could not open a listening socket\n");
        std::exit(2);
    }
    *port = ntohs(addr.sin_port);
    return sock;
}

uint16_t free_port() {
    uint16_t port = 0;
    closesocket(listen_anywhere(&port));
    return port;
}

socket_t connect_client(uint16_t port) {
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = loopback(port);
    if (sock == INVALID_SOCKET || connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "could not connect to the proxy\n");
        std::exit(2);
    }
    // A proxy that never answers fails the test instead of hanging it.
#ifdef _WIN32
    DWORD timeout = 5000;
#else
    timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
#endif
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return sock;
}

bool send_bytes(socket_t sock, const Bytes& bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
        const auto n = send(sock, reinterpret_cast<const char*>(bytes.data() + sent),
                            static_cast<io_len_t>(bytes.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Reads up to `want` bytes; shorter when the proxy closes the connection or
// nothing arrives in time.
Bytes receive(socket_t sock, size_t want) {
    Bytes out(want);
    size_t got = 0;
    while (got < want) {
        const auto n = recv(sock, reinterpret_cast<char*>(out.data() + got), static_cast<io_len_t>(want - got), 0);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    out.resize(got);
    return out;
}

bool can_connect(uint16_t port) {
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr = loopback(port);
    const bool connected =
        sock != INVALID_SOCKET && connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (sock != INVALID_SOCKET) closesocket(sock);
    return connected;
}

bool closed_by_proxy(socket_t sock) {
    char byte = 0;
    return recv(sock, &byte, 1, 0) == 0;
}

// The proxy serves one client at a time and takes up to ~100 ms to notice
// that the previous one left. A client's own packet reaching the device shows
// that its session has started, so what the device sends next is for it.
socket_t connect_and_sync(uint16_t port) {
    const size_t before = write_count();
    socket_t client = connect_client(port);
    CHECK(send_bytes(client, header(0)));
    CHECK(eventually([&] { return write_count() == before + 1; }));
    return client;
}

aoahid_device* fake_device() {
    static int storage = 0;
    return reinterpret_cast<aoahid_device*>(&storage);
}

void test_start_failures() {
    aoahid_adb_proxy_context* proxy = reinterpret_cast<aoahid_adb_proxy_context*>(&g_channel);
    CHECK(aoahid_adb_proxy_start(nullptr, 6555, kDrainTimeoutMs, &proxy) == AOAHID_ADB_PROXY_ERR_ARGUMENT);
    CHECK(proxy == nullptr);
    CHECK(aoahid_adb_proxy_start(fake_device(), 6555, kDrainTimeoutMs, nullptr) == AOAHID_ADB_PROXY_ERR_ARGUMENT);
    CHECK(g_opens == 0);
    // A zero wait is refused before anything is opened.
    CHECK(aoahid_adb_proxy_start(fake_device(), 6555, 0, &proxy) == AOAHID_ADB_PROXY_ERR_ARGUMENT);
    CHECK(proxy == nullptr && g_opens == 0);

    g_open_result = AOAHID_ERR_BUSY;
    proxy = reinterpret_cast<aoahid_adb_proxy_context*>(&g_channel);
    CHECK(aoahid_adb_proxy_start(fake_device(), free_port(), kDrainTimeoutMs, &proxy) == AOAHID_ADB_PROXY_ERR_INTERFACE);
    CHECK(proxy == nullptr);
    CHECK(close_count() == 0);
    g_open_result = AOAHID_OK;

    // A port something else listens on: the Channel that was opened is closed again.
    uint16_t taken = 0;
    socket_t holder = listen_anywhere(&taken);
    CHECK(aoahid_adb_proxy_start(fake_device(), taken, kDrainTimeoutMs, &proxy) == AOAHID_ADB_PROXY_ERR_BIND);
    CHECK(proxy == nullptr);
    CHECK(close_count() == 1);
    CHECK(g_drain_timeout_ms == kDrainTimeoutMs && g_close_drain_timeout_ms == kDrainTimeoutMs);
    closesocket(holder);

    aoahid_adb_proxy_stop(nullptr, kDrainTimeoutMs);
}

void test_channel_options() {
    CHECK(g_options.struct_size == sizeof(aoahid_channel_options));
    CHECK(g_options.interface_class == 0xFF);
    CHECK(g_options.interface_subclass == 0x42);
    CHECK(g_options.interface_protocol == 0x01);
    CHECK(g_options.read_mode == AOAHID_CHANNEL_READ_REQUEST);
    CHECK(g_options.transfer_bytes == 1024 * 1024);
    CHECK(g_options.zero_length_termination == 1);
}

// TCP -> USB: whatever the TCP segmentation, the header is one write and the
// payload the next.
void test_tcp_to_usb(uint16_t port) {
    const size_t before = write_count();
    socket_t client = connect_client(port);
    const Bytes first = payload(5, 0x10);
    const Bytes big = payload(70000, 0x20);
    Bytes stream = joined(header(5), first);
    stream = joined(stream, header(0));
    stream = joined(stream, joined(header(70000), big));
    CHECK(send_bytes(client, Bytes(stream.begin(), stream.begin() + 7)));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(send_bytes(client, Bytes(stream.begin() + 7, stream.end())));

    CHECK(eventually([&] { return write_count() == before + 5; }));
    if (write_count() == before + 5) {
        CHECK(write_at(before) == header(5));
        CHECK(write_at(before + 1) == first);
        CHECK(write_at(before + 2) == header(0));
        CHECK(write_at(before + 3) == header(70000));
        CHECK(write_at(before + 4) == big);
    }
    closesocket(client);
}

// USB -> TCP: a packet arriving in pieces reaches the client whole.
void test_usb_to_tcp(uint16_t port) {
    socket_t client = connect_and_sync(port);
    const Bytes packet = joined(header(300), payload(300, 0x30));
    device_sends(Bytes(packet.begin(), packet.begin() + 10));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    device_sends(Bytes(packet.begin() + 10, packet.begin() + 100));
    device_sends(Bytes(packet.begin() + 100, packet.end()));
    CHECK(receive(client, packet.size()) == packet);

    const Bytes empty = header(0);
    device_sends(empty);
    CHECK(receive(client, empty.size()) == empty);
    closesocket(client);
}

// A client header that is not an ADB packet ends the session and never
// reaches the device.
void test_bad_client_header(uint16_t port) {
    const size_t before = write_count();
    socket_t client = connect_client(port);
    CHECK(send_bytes(client, header(kWrite, 4, 0x12345678u)));
    CHECK(closed_by_proxy(client));
    closesocket(client);

    client = connect_client(port);
    CHECK(send_bytes(client, header(1024 * 1024 + 1)));
    CHECK(closed_by_proxy(client));
    closesocket(client);
    CHECK(write_count() == before);
}

// A packet the previous client left half received is finished and dropped, so
// the next client starts on a header.
void test_reconnect_mid_packet(uint16_t port) {
    socket_t first = connect_and_sync(port);
    const Bytes stale = joined(header(8), payload(8, 0x40));
    device_sends(Bytes(stale.begin(), stale.begin() + 27));
    CHECK(eventually([] {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_in.empty();
    }));
    closesocket(first);

    socket_t second = connect_and_sync(port);
    const Bytes fresh = joined(header(4), payload(4, 0x50));
    device_sends(Bytes(stale.begin() + 27, stale.end()));
    device_sends(fresh);
    CHECK(receive(second, fresh.size()) == fresh);
    closesocket(second);
}

// A client that leaves in the middle of a header or of a payload has sent
// nothing to the device: a header goes out only once its payload is in.
void test_client_leaves_mid_packet(uint16_t port) {
    const size_t before = write_count();
    socket_t client = connect_client(port);
    const Bytes cut = header(8);
    CHECK(send_bytes(client, Bytes(cut.begin(), cut.begin() + 10)));
    closesocket(client);

    client = connect_client(port);
    CHECK(send_bytes(client, joined(header(8), payload(3, 0x60))));
    closesocket(client);

    // The next client's own packet is the first thing the device gets.
    client = connect_and_sync(port);
    CHECK(write_count() == before + 1);
    CHECK(write_at(before) == header(0));
    closesocket(client);
}

// A write that times out with part of the data queued goes on with the rest.
void test_write_timeout_resumes(uint16_t port) {
    const size_t before = write_count();
    socket_t client = connect_client(port);
    const Bytes head = header(40);
    const Bytes body = payload(40, 0x70);
    script_writes({5, kWhole, 10, kWhole});
    CHECK(send_bytes(client, joined(head, body)));
    CHECK(eventually([&] { return write_count() == before + 4; }));
    if (write_count() == before + 4) {
        CHECK(write_at(before) == Bytes(head.begin(), head.begin() + 5));
        CHECK(write_at(before + 1) == Bytes(head.begin() + 5, head.end()));
        CHECK(write_at(before + 2) == Bytes(body.begin(), body.begin() + 10));
        CHECK(write_at(before + 3) == Bytes(body.begin() + 10, body.end()));
    }
    closesocket(client);
}

// A timeout with every byte queued leaves only the zero-length packet unsent,
// which cannot be sent alone: the session ends and nothing is written twice.
void test_write_timeout_at_end(uint16_t port) {
    const size_t before = write_count();
    socket_t client = connect_client(port);
    const Bytes body = payload(64, 0x80);
    script_writes({kWhole, body.size()});
    CHECK(send_bytes(client, joined(header(64), body)));
    CHECK(closed_by_proxy(client));
    closesocket(client);
    CHECK(write_count() == before + 2);
    if (write_count() == before + 2) CHECK(write_at(before + 1) == body);

    client = connect_and_sync(port);
    closesocket(client);
}

// A device header that claims more than an ADB packet can hold means the
// framing is lost: the session ends and the proxy stops serving.
void test_bad_device_header(uint16_t port) {
    socket_t client = connect_and_sync(port);
    device_sends(header(1024 * 1024 + 1));
    CHECK(closed_by_proxy(client));
    closesocket(client);
    CHECK(eventually([&] { return !can_connect(port); }));
}

// A failed USB read ends the session and the proxy stops serving: the port is
// closed, so a new client is refused instead of accepted and never answered.
void test_channel_lost(uint16_t port) {
    socket_t client = connect_and_sync(port);
    device_fails(AOAHID_ERR_NO_DEVICE);
    CHECK(closed_by_proxy(client));
    closesocket(client);
    CHECK(eventually([&] { return !can_connect(port); }));
}

}  // namespace

extern "C" {

aoahid_result AOAHID_CALL aoahid_channel_open(aoahid_device*, const aoahid_channel_options* options,
                                              uint32_t drain_timeout_ms,
                                              aoahid_channel** out_channel) {
    std::lock_guard<std::mutex> lock(g_mutex);
    *out_channel = nullptr;
    if (g_open_result != AOAHID_OK) return g_open_result;
    ++g_opens;
    g_drain_timeout_ms = drain_timeout_ms;
    g_options = *options;
    *out_channel = &g_channel;
    return AOAHID_OK;
}

aoahid_result AOAHID_CALL aoahid_channel_close(aoahid_channel*, uint32_t drain_timeout_ms) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_closes;
    g_close_drain_timeout_ms = drain_timeout_ms;
    return AOAHID_OK;
}

aoahid_result AOAHID_CALL aoahid_channel_write(aoahid_channel*, const uint8_t* data, size_t length,
                                               size_t* out_written, uint32_t) {
    std::lock_guard<std::mutex> lock(g_mutex);
    size_t take = length;
    aoahid_result result = AOAHID_OK;
    if (!g_write_script.empty()) {
        if (g_write_script.front() != kWhole) {
            take = g_write_script.front();
            result = AOAHID_ERR_TIMEOUT;
        }
        g_write_script.pop_front();
    }
    g_writes.push_back(Bytes(data, data + take));
    *out_written = take;
    return result;
}

aoahid_result AOAHID_CALL aoahid_channel_read(aoahid_channel*, uint8_t* buffer, size_t capacity,
                                              size_t* out_received, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(g_mutex);
    *out_received = 0;
    g_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                  [] { return !g_in.empty() || g_read_error != AOAHID_OK; });
    if (g_read_error != AOAHID_OK) return g_read_error;
    if (g_in.empty()) return AOAHID_ERR_TIMEOUT;
    size_t count = g_in.size() < capacity ? g_in.size() : capacity;
    for (size_t i = 0; i < count; ++i) {
        buffer[i] = g_in.front();
        g_in.pop_front();
    }
    *out_received = count;
    g_cv.notify_all();
    return AOAHID_OK;
}

}  // extern "C"

int main() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 2;
#endif
    test_start_failures();

    const uint16_t port = free_port();
    aoahid_adb_proxy_context* proxy = nullptr;
    CHECK(aoahid_adb_proxy_start(fake_device(), port, kDrainTimeoutMs, &proxy) == AOAHID_ADB_PROXY_OK);
    CHECK(proxy != nullptr);
    if (proxy) {
        test_channel_options();
        test_tcp_to_usb(port);
        test_usb_to_tcp(port);
        test_bad_client_header(port);
        test_reconnect_mid_packet(port);
        test_client_leaves_mid_packet(port);
        test_write_timeout_resumes(port);
        test_write_timeout_at_end(port);
        test_channel_lost(port);
        const int closes = close_count();
        aoahid_adb_proxy_stop(proxy, kDrainTimeoutMs);
        CHECK(close_count() == closes + 1);
        CHECK(g_close_drain_timeout_ms == kDrainTimeoutMs);

        // The port is free again once stop returns.
        device_fails(AOAHID_OK);
        proxy = nullptr;
        CHECK(aoahid_adb_proxy_start(fake_device(), port, kDrainTimeoutMs, &proxy) == AOAHID_ADB_PROXY_OK);
        if (proxy) test_bad_device_header(port);
        aoahid_adb_proxy_stop(proxy, kDrainTimeoutMs);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    if (g_failures == 0) std::printf("all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
