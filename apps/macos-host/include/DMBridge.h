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
typedef void (*DMInputCallback)(void *context);
typedef void (*DMLinkIntentCallback)(uint64_t link_id, int32_t capture, int32_t input, void *context);
typedef void (*DMLinkInputCallback)(uint64_t link_id, void *context);

typedef struct DMBridgeConfig {
    const char *device_selector; /* NULL for auto-detect 12d1:5000 */
    int32_t width;
    int32_t height;
    int32_t refresh_hz;
    int32_t prefer_hevc;
    int32_t auto_match_device_resolution;
} DMBridgeConfig;

DMBridge *dm_bridge_create(const DMBridgeConfig *config);
void dm_bridge_destroy(DMBridge *bridge);

void dm_bridge_set_log(DMBridge *bridge, DMLogCallback callback, void *context);
void dm_bridge_set_intent_callback(DMBridge *bridge, DMIntentCallback callback,
                                   void *context);
void dm_bridge_set_input_callback(DMBridge *bridge, DMInputCallback callback,
                                  void *context);
void dm_bridge_set_link_intent_callback(DMBridge *bridge,
                                        DMLinkIntentCallback callback,
                                        void *context);
void dm_bridge_set_link_input_callback(DMBridge *bridge,
                                       DMLinkInputCallback callback,
                                       void *context);

/* Starts the USB/AOA/session worker. Returns 0 on success. */
int32_t dm_bridge_start(DMBridge *bridge);
/* Stops the worker and returns the device to its normal (HDC) mode. */
void dm_bridge_stop(DMBridge *bridge);
/* Updates whether new sessions auto-accept the connected device's resolution. */
void dm_bridge_set_auto_match_resolution(DMBridge *bridge, int32_t enabled);
/* Updates preferred host refresh rate (0 = auto/negotiated cap at 120 Hz). */
void dm_bridge_set_preferred_refresh_hz(DMBridge *bridge, int32_t refresh_hz);

/* Queues one Annex-B access unit. Safe to call from any thread. */
int32_t dm_bridge_submit_video(DMBridge *bridge, const uint8_t *data, size_t size,
                               uint64_t pts_us, int32_t keyframe);
int32_t dm_bridge_submit_video_for_link(DMBridge *bridge, uint64_t link_id,
                                        const uint8_t *data, size_t size,
                                        uint64_t pts_us, int32_t keyframe);

/* Drains accepted input events. Returns the number written. */
int32_t dm_bridge_take_input(DMBridge *bridge, DMInputEvent *out, int32_t capacity);
int32_t dm_bridge_take_input_for_link(DMBridge *bridge, uint64_t link_id,
                                      DMInputEvent *out, int32_t capacity);

/* Non-zero when the scheduler asked for a fresh IDR. */
int32_t dm_bridge_needs_keyframe(DMBridge *bridge);
int32_t dm_bridge_needs_keyframe_for_link(DMBridge *bridge, uint64_t link_id);
/* Non-zero recovery_id when the scheduler needs a fresh IDR; 0 otherwise. */
uint32_t dm_bridge_keyframe_request_id(DMBridge *bridge);
uint32_t dm_bridge_keyframe_request_id_for_link(DMBridge *bridge, uint64_t link_id);
/* Returns the negotiated stream profile parameters, or 0 if not yet negotiated.
   codec: 0 = none, 1 = h264, 2 = hevc. */
int32_t dm_bridge_negotiated_width(DMBridge *bridge);
int32_t dm_bridge_negotiated_height(DMBridge *bridge);
int32_t dm_bridge_negotiated_refresh_hz(DMBridge *bridge);
int32_t dm_bridge_negotiated_codec(DMBridge *bridge);
int32_t dm_bridge_negotiated_width_for_link(DMBridge *bridge, uint64_t link_id);
int32_t dm_bridge_negotiated_height_for_link(DMBridge *bridge, uint64_t link_id);
int32_t dm_bridge_negotiated_refresh_hz_for_link(DMBridge *bridge, uint64_t link_id);
int32_t dm_bridge_negotiated_codec_for_link(DMBridge *bridge, uint64_t link_id);

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

/* Updates the list of blocked device IDs; disconnects the active device if blocked. */
void dm_bridge_set_blocked_devices(DMBridge *bridge,
                                   const char *const *device_ids,
                                   int32_t count);
/* Disconnects the active USB accessory session if device_id is NULL or matches. */
void dm_bridge_disconnect_device(DMBridge *bridge, const char *device_id);
/* Disconnects the active USB accessory session matching link_id. */
void dm_bridge_disconnect_link(DMBridge *bridge, uint64_t link_id);
/* Scans currently attached matching USB devices and returns a heap-allocated JSON array; caller must free() it. */
char *dm_bridge_scan_devices(DMBridge *bridge);

#ifdef __cplusplus
}
#endif
