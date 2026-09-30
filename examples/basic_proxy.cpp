// Minimal host app: open the first phone with libaoahid and expose its ADB
// interface on 127.0.0.1:6555. The phone is opened in whatever USB mode it is
// in; switching to AOA accessory mode is the application's own choice
// (aoahid_accessory_start) and not needed by the proxy. USB debugging must be on.
//
// Order: `adb kill-server` -> run this -> `adb connect 127.0.0.1:6555`
// (see docs/USAGE.md). HID code would go where the example waits for Enter.
#include <cstdio>
#include <cstring>

#include "aoahid.h"
#include "aoahid_adb_proxy.h"

static const uint16_t kPort = 6555;

static aoahid_device_options device_options() {
    aoahid_device_options o;
    std::memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.control_timeout_ms = 500;
    o.send_timeout_ms = 500;
    o.descriptor_fragment_bytes = 4096;
    o.transfer_pool_slots = 4;
    o.maximum_report_bytes = 1024;
    o.close_drain_timeout_ms = 1000;
    o.aoa_descriptor_wire_policy_bytes = 4096;
    o.linux_descriptor_policy_bytes = 4096;
    o.linux_hid_fields_per_report_policy = 256;
    o.linux_hid_global_stack_depth_policy = 4;
    o.linux_hid_usages_policy = 12288;
    o.linux_hid_report_data_bits_policy = 65528;
    o.linux_hid_report_size_bits_policy = 256;
    o.target_ep0_data_policy_bytes = 4096;
    o.host_control_buffer_policy_bytes = 4096;
    o.interface_claim_policy = AOAHID_INTERFACE_CLAIM_NONE;
    o.interface_number = -1;
    return o;
}

int main() {
    aoahid_context_options co;
    std::memset(&co, 0, sizeof(co));
    co.struct_size = sizeof(co);
    co.event_mode = AOAHID_EVENT_INTERNAL_THREAD;  // required by the proxy
    co.log_level = AOAHID_LOG_DISABLED;
    aoahid_context* ctx = nullptr;
    if (aoahid_context_create(&co, &ctx) != AOAHID_OK) return 1;

    aoahid_discovery* d = nullptr;
    if (aoahid_discover(ctx, 500, &d) != AOAHID_OK || aoahid_discovery_count(d) == 0) {
        std::fprintf(stderr, "no AOA-capable device found\n");
        aoahid_discovery_destroy(d);
        aoahid_context_destroy(ctx);
        return 1;
    }
    const aoahid_device_info* phone = aoahid_discovery_get(d, 0);
    const aoahid_device_options dopt = device_options();
    aoahid_device* dev = nullptr;
    aoahid_result r = aoahid_device_open(ctx, phone, &dopt, &dev);
    aoahid_discovery_destroy(d);
    if (r != AOAHID_OK) {
        std::fprintf(stderr, "device open failed: %d\n", static_cast<int>(r));
        aoahid_context_destroy(ctx);
        return 1;
    }

    aoahid_adb_proxy_context* proxy = nullptr;
    int pr = aoahid_adb_proxy_start(dev, kPort, &proxy);
    if (pr != 0) {
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
        aoahid_adb_proxy_stop(proxy);
        std::printf("Stopped. Run `adb kill-server` so adb sees the phone over USB again.\n");
    }

    aoahid_device_close(dev);
    aoahid_context_destroy(ctx);
    return pr == 0 ? 0 : 1;
}
