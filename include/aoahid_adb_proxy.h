#pragma once

#include <aoahid.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AOAHID_ADB_PROXY_VERSION_MAJOR 3
#define AOAHID_ADB_PROXY_VERSION_MINOR 0
#define AOAHID_ADB_PROXY_VERSION_PATCH 1

typedef struct aoahid_adb_proxy_context aoahid_adb_proxy_context;

/*
 * Claims the device's ADB interface (0xFF/0x42/0x01) as a Channel and serves it
 * on 127.0.0.1:tcp_port for `adb connect`, from background threads.
 *
 * Requirements:
 *  - The device's Context uses AOAHID_EVENT_INTERNAL_THREAD.
 *  - The device exposes an ADB interface (e.g. accessory + ADB, 18d1:2d01).
 *  - No adb server holds that interface (run `adb kill-server` first).
 *  - Avoid ports 5555-5585 (adb's emulator scan range); 6555 is a good default.
 *
 * Returns 0 on success, or:
 *  -1 null argument
 *  -2 ADB interface unavailable (held by adb, USB debugging off, no ADB interface)
 *  -3 socket setup failed
 *  -4 port in use
 *  -5 listen failed
 *  -6 out of memory, or no thread could be started
 */
int aoahid_adb_proxy_start(aoahid_device* device, uint16_t tcp_port, aoahid_adb_proxy_context** out_proxy);

/* Stops the proxy and releases the Channel and port. Returns within ~100 ms
 * (up to ~1 s if the device has stopped reading).
 * Call before closing the device. NULL is ignored. */
void aoahid_adb_proxy_stop(aoahid_adb_proxy_context* proxy);

#ifdef __cplusplus
}
#endif
