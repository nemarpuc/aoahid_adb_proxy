// Minimal host app: open the first phone with libaoahid and expose its ADB
// interface on 127.0.0.1:6555. The phone is opened in whatever USB mode it is
// in; switching to AOA accessory mode is the application's own choice
// (aoahid_accessory_start) and not needed by the proxy. USB debugging must be on.
//
// Order: `adb kill-server` -> run this -> `adb connect 127.0.0.1:6555`
// (see docs/USAGE.md). HID code would go where the example waits for Enter.
#include <cstdio>

#include "aoahid.h"
#include "aoahid_adb_proxy.h"

static const uint16_t kPort = 6555;
// Every wait is an argument of the call (see "Timeouts" in libaoahid's docs/API.md); the
// library's constants are values that work with a current phone.
static const uint32_t kControlTimeoutMs = AOAHID_CONTROL_TIMEOUT_MS;
static const uint32_t kDrainTimeoutMs = AOAHID_DRAIN_TIMEOUT_MS;

int main() {
    aoahid_context_options co;
    aoahid_context_options_init(&co);  // AOAHID_EVENT_INTERNAL_THREAD, which the proxy requires
    aoahid_context* ctx = nullptr;
    if (aoahid_context_create(&co, &ctx) != AOAHID_OK) return 1;

    aoahid_discovery* d = nullptr;
    if (aoahid_discover(ctx, kControlTimeoutMs, &d) != AOAHID_OK || aoahid_discovery_count(d) == 0) {
        std::fprintf(stderr, "no AOA-capable device found\n");
        aoahid_discovery_destroy(d);
        aoahid_context_destroy(ctx, kControlTimeoutMs, kDrainTimeoutMs);
        return 1;
    }
    const aoahid_device_info* phone = aoahid_discovery_get(d, 0);
    aoahid_device_options dopt;
    aoahid_device_options_init(&dopt);
    aoahid_device* dev = nullptr;
    aoahid_result r = aoahid_device_open(ctx, phone, &dopt, &dev);
    aoahid_discovery_destroy(d);
    if (r != AOAHID_OK) {
        std::fprintf(stderr, "device open failed: %d\n", static_cast<int>(r));
        aoahid_context_destroy(ctx, kControlTimeoutMs, kDrainTimeoutMs);
        return 1;
    }

    aoahid_adb_proxy_context* proxy = nullptr;
    int pr = aoahid_adb_proxy_start(dev, kPort, kDrainTimeoutMs, &proxy);
    if (pr != AOAHID_ADB_PROXY_OK) {
#ifdef _WIN32
        std::fprintf(stderr,
                     "proxy start failed: %d (-2: run `adb kill-server` first, and check that "
                     "the phone's driver is WinUSB; see docs/USAGE.md)\n",
                     pr);
#else
        std::fprintf(stderr, "proxy start failed: %d (-2: run `adb kill-server` first)\n", pr);
#endif
    } else {
        std::printf("ADB proxy ready. Run: adb connect 127.0.0.1:%u\n", kPort);
        std::printf("Press Enter to stop.\n");
        std::getchar();  // your HID loop (aoahid_node_*) goes here
        aoahid_adb_proxy_stop(proxy, kDrainTimeoutMs);
        std::printf("Stopped. Run `adb kill-server` so adb sees the phone over USB again.\n");
    }

    aoahid_device_close(dev, kControlTimeoutMs, kDrainTimeoutMs);
    aoahid_context_destroy(ctx, kControlTimeoutMs, kDrainTimeoutMs);
    return pr == 0 ? 0 : 1;
}
