#pragma once

#include <aoahid.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AOAHID_ADB_PROXY_VERSION_MAJOR 3
#define AOAHID_ADB_PROXY_VERSION_MINOR 1
#define AOAHID_ADB_PROXY_VERSION_PATCH 3

typedef struct aoahid_adb_proxy_context aoahid_adb_proxy_context;

/* What aoahid_adb_proxy_start returns. The numbers are stable. */
enum {
    AOAHID_ADB_PROXY_OK = 0,
    /* null argument */
    AOAHID_ADB_PROXY_ERR_ARGUMENT = -1,
    /* ADB interface unavailable (held by adb, USB debugging off, no ADB interface) */
    AOAHID_ADB_PROXY_ERR_INTERFACE = -2,
    /* socket setup failed */
    AOAHID_ADB_PROXY_ERR_SOCKET = -3,
    /* the port could not be bound (in use, reserved, or not permitted) */
    AOAHID_ADB_PROXY_ERR_BIND = -4,
    /* listen failed */
    AOAHID_ADB_PROXY_ERR_LISTEN = -5,
    /* out of memory, or no thread could be started */
    AOAHID_ADB_PROXY_ERR_RESOURCE = -6
};

/*
 * Claims the device's ADB interface (0xFF/0x42/0x01) as a Channel and serves it
 * on 127.0.0.1:tcp_port for `adb connect`, from background threads.
 *
 * Requirements:
 *  - The device's Context uses AOAHID_EVENT_INTERNAL_THREAD.
 *  - The device exposes an ADB interface (USB debugging on; current USB mode or
 *    accessory mode, e.g. 18d1:2d01).
 *  - No adb server holds that interface (run `adb kill-server` first).
 *  - Avoid ports 5555-5585 (adb's emulator scan range); 6555 is a good default.
 *
 * Returns AOAHID_ADB_PROXY_OK (0), or one of the AOAHID_ADB_PROXY_ERR_* values
 * above. *out_proxy is NULL on every failure.
 *
 * If reading from the device fails later (unplugged, or any USB read error),
 * the proxy stops serving and closes the port, so `adb connect` is refused.
 * aoahid_adb_proxy_stop must still be called to release the rest.
 */
int aoahid_adb_proxy_start(aoahid_device* device, uint16_t tcp_port, aoahid_adb_proxy_context** out_proxy);

/* Stops the proxy and releases the Channel and port. Returns within ~100 ms.
 * A packet whose header already reached the device is finished first, so it
 * takes up to ~1 s longer if the device has stopped reading, plus up to the
 * Device's close_drain_timeout_ms while the Channel's transfers are cancelled.
 * Call before closing the device. NULL is ignored. */
void aoahid_adb_proxy_stop(aoahid_adb_proxy_context* proxy);

#ifdef __cplusplus
}
#endif
