// Minimal host app: open the first phone with libaoahid and expose its ADB
// interface on 127.0.0.1:6555. By default the phone stays in its current USB
// mode; `--accessory` switches it to AOA accessory+ADB mode first. USB
// debugging must be on in both cases.
//
// Order: `adb kill-server` -> run this -> `adb connect 127.0.0.1:6555`
// (see docs/USAGE.md). HID code would go where the example waits for Enter.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "aoahid.h"
#include "aoahid_adb_proxy.h"

static const uint16_t kPort = 6555;

static bool is_accessory(const aoahid_device_info* info) {
    return info->vendor_id == 0x18D1 && info->product_id >= 0x2D00 && info->product_id <= 0x2D05;
}

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

// Waits up to ~10 s for an accessory-mode device to (re)appear.
static aoahid_discovery* wait_accessory(aoahid_context* ctx, const aoahid_device_info** out) {
    for (int i = 0; i < 50; ++i) {
        aoahid_discovery* d = nullptr;
        if (aoahid_discover(ctx, 500, &d) == AOAHID_OK) {
            for (size_t k = 0; k < aoahid_discovery_count(d); ++k) {
                const aoahid_device_info* info = aoahid_discovery_get(d, k);
                if (is_accessory(info)) {
                    *out = info;
                    return d;
                }
            }
            aoahid_discovery_destroy(d);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return nullptr;
}

int main(int argc, char** argv) {
    const bool accessory = argc > 1 && std::strcmp(argv[1], "--accessory") == 0;

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
    if (accessory && !is_accessory(phone)) {
        aoahid_accessory_options ao;
        std::memset(&ao, 0, sizeof(ao));
        ao.struct_size = sizeof(ao);
        ao.strings.manufacturer = "aoahid";
        ao.strings.model = "adb_proxy";
        ao.strings.description = "aoahid_adb_proxy example";
        aoahid_result r = aoahid_accessory_start(ctx, phone, &ao);
        aoahid_discovery_destroy(d);
        d = nullptr;
        if (r != AOAHID_OK) {
            std::fprintf(stderr, "accessory start failed: %d\n", static_cast<int>(r));
            aoahid_context_destroy(ctx);
            return 1;
        }
        d = wait_accessory(ctx, &phone);
        if (!d) {
            std::fprintf(stderr, "device did not reappear in accessory mode\n");
            aoahid_context_destroy(ctx);
            return 1;
        }
    }
    if (accessory && phone->product_id != 0x2D01) {
        std::fprintf(stderr, "no ADB interface (enable USB debugging): %04x\n", phone->product_id);
        aoahid_discovery_destroy(d);
        aoahid_context_destroy(ctx);
        return 1;
    }

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
    }

    aoahid_device_close(dev);
    aoahid_context_destroy(ctx);
    return pr == 0 ? 0 : 1;
}
