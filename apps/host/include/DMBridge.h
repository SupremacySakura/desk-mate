#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DMBridge DMBridge;

typedef struct DMInputEvent {
    int32_t kind;
    int32_t pointer_id;
    int32_t flags;
    float x;
    float y;
    float dx;
    float dy;
    uint32_t keycode;
} DMInputEvent;

typedef void (*DMLogCallback)(const char *message, void *context);
typedef void (*DMIntentCallback)(int32_t capture, int32_t input, void *context);

typedef struct DMBridgeConfig {
    const char *device_selector; /* NULL for auto-detect 12d1:5000 */
    int32_t width;
    int32_t height;
    int32_t refresh_hz;
    int32_t prefer_hevc;
} DMBridgeConfig;

DMBridge *dm_bridge_create(const DMBridgeConfig *config);
void dm_bridge_destroy(DMBridge *bridge);

void dm_bridge_set_log(DMBridge *bridge, DMLogCallback callback, void *context);
void dm_bridge_set_intent_callback(DMBridge *bridge, DMIntentCallback callback,
                                   void *context);

/* Starts the USB/AOA/session worker. Returns 0 on success. */
int32_t dm_bridge_start(DMBridge *bridge);
/* Stops the worker and returns the device to its normal (HDC) mode. */
void dm_bridge_stop(DMBridge *bridge);

/* Queues one Annex-B access unit. Safe to call from any thread. */
int32_t dm_bridge_submit_video(DMBridge *bridge, const uint8_t *data, size_t size,
                               uint64_t pts_us, int32_t keyframe);

/* Drains accepted input events. Returns the number written. */
int32_t dm_bridge_take_input(DMBridge *bridge, DMInputEvent *out, int32_t capacity);

/* Non-zero when the scheduler asked for a fresh IDR. */
int32_t dm_bridge_needs_keyframe(DMBridge *bridge);

/* Host system events; values mirror deskmate::protocol::HostEvent. */
enum {
    DM_HOST_EVENT_SCREEN_LOCKED = 0,
    DM_HOST_EVENT_SCREEN_UNLOCKED = 1,
    DM_HOST_EVENT_WILL_SLEEP = 2,
    DM_HOST_EVENT_SCREEN_RECORDING_REVOKED = 3,
    DM_HOST_EVENT_ACCESSIBILITY_REVOKED = 4,
    DM_HOST_EVENT_DISPLAY_ERROR = 5,
};
int32_t dm_bridge_host_event(DMBridge *bridge, int32_t event);

/* Heap-allocated JSON snapshot; caller must free() it. */
char *dm_bridge_snapshot(DMBridge *bridge);

#ifdef __cplusplus
}
#endif
