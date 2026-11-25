#include "ace/xcomponent/native_interface_xcomponent.h"
#include "deskmate/protocol.hpp"
#include "device_engine.hpp"
#include "napi/native_api.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3210
#define LOG_TAG "DeskMateSurface"

#include <hilog/log.h>

namespace {

std::mutex g_surface_mutex;
std::string g_surface_key;

std::string SurfaceKey(void *window)
{
    return window == nullptr
               ? std::string()
               : std::to_string(reinterpret_cast<std::uintptr_t>(window));
}

bool UpdateSurface(void *window)
{
    const std::string key = SurfaceKey(window);
    if (key.empty()) {
        OH_LOG_ERROR(LOG_APP, "surface callback carried a null window");
        return false;
    }
    std::lock_guard lock(g_surface_mutex);
    auto &engine = deskmate::device::DeviceEngine::Instance();
    if (key == g_surface_key) {
        return true;
    }
    OH_LOG_INFO(LOG_APP, "surface created key=%{public}s",
                key.c_str());
    if (!g_surface_key.empty()) {
        (void)engine.SurfaceDestroyed(g_surface_key);
        g_surface_key.clear();
    }
    if (!engine.SurfaceCreated(key)) {
        return false;
    }
    engine.SetNativeWindow(window);
    g_surface_key = key;
    OH_LOG_INFO(LOG_APP, "surface attached key=%{public}s", key.c_str());
    return true;
}

void RemoveSurface(void *window)
{
    const std::string key = SurfaceKey(window);
    std::lock_guard lock(g_surface_mutex);
    if (key.empty() || key != g_surface_key) {
        return;
    }
    OH_LOG_INFO(LOG_APP, "surface destroyed key=%{public}s", key.c_str());
    g_surface_key.clear();
    auto &engine = deskmate::device::DeviceEngine::Instance();
    (void)engine.SurfaceDestroyed(key);
    engine.SetNativeWindow(nullptr);
}

struct QueuedNativeTouch {
    std::uint8_t kind{0};
    std::uint8_t slot{0};
    std::uint16_t flags{0};
    float x{0.0F};
    float y{0.0F};
    float dx{0.0F};
    float dy{0.0F};
};

struct NativeTouchTracker {
    bool enabled{false};
    std::uint64_t width{0};
    std::uint64_t height{0};
    std::unordered_map<std::int32_t, std::uint8_t> slots;
    std::unordered_map<std::uint8_t, std::pair<float, float>> positions;
    int primary_slot{-1};
    int log_budget{12};
};

std::mutex g_touch_mutex;
NativeTouchTracker g_touch_tracker;

void OnSurfaceCreated(OH_NativeXComponent *component, void *window)
{
    OH_LOG_INFO(LOG_APP, "OnSurfaceCreated");
    (void)UpdateSurface(window);
    uint64_t width = 0;
    uint64_t height = 0;
    if (component != nullptr && window != nullptr &&
        OH_NativeXComponent_GetXComponentSize(component, window, &width, &height) ==
            OH_NATIVEXCOMPONENT_RESULT_SUCCESS &&
        width > 0 && height > 0) {
        std::lock_guard lock(g_touch_mutex);
        g_touch_tracker.width = width;
        g_touch_tracker.height = height;
    }
}

void OnSurfaceChanged(OH_NativeXComponent *component, void *window)
{
    OH_LOG_INFO(LOG_APP, "OnSurfaceChanged");
    (void)UpdateSurface(window);
    uint64_t width = 0;
    uint64_t height = 0;
    if (component != nullptr && window != nullptr &&
        OH_NativeXComponent_GetXComponentSize(component, window, &width, &height) ==
            OH_NATIVEXCOMPONENT_RESULT_SUCCESS &&
        width > 0 && height > 0) {
        std::lock_guard lock(g_touch_mutex);
        g_touch_tracker.width = width;
        g_touch_tracker.height = height;
    }
}

float ClampUnit(float value)
{
    if (!std::isfinite(value)) {
        return 0.0F;
    }
    return std::min(1.0F, std::max(0.0F, value));
}

int AllocateNativeSlotLocked(std::int32_t touch_id)
{
    if (const auto existing = g_touch_tracker.slots.find(touch_id);
        existing != g_touch_tracker.slots.end()) {
        return existing->second;
    }
    bool used[OH_NATIVE_XCOMPONENT_MAX_TOUCH_POINTS_NUMBER] = {};
    for (const auto &entry : g_touch_tracker.slots) {
        if (entry.second < OH_NATIVE_XCOMPONENT_MAX_TOUCH_POINTS_NUMBER) {
            used[entry.second] = true;
        }
    }
    for (std::uint8_t slot = 0;
         slot < OH_NATIVE_XCOMPONENT_MAX_TOUCH_POINTS_NUMBER; ++slot) {
        if (!used[slot]) {
            g_touch_tracker.slots[touch_id] = slot;
            return slot;
        }
    }
    return -1;
}

std::vector<QueuedNativeTouch> BuildReleaseAllTouchesLocked(bool cancel)
{
    std::vector<QueuedNativeTouch> packets;
    const std::uint8_t kind = static_cast<std::uint8_t>(
        cancel ? deskmate::protocol::InputKind::TouchCancel
               : deskmate::protocol::InputKind::TouchUp);
    for (const auto &entry : g_touch_tracker.slots) {
        const std::uint8_t slot = entry.second;
        float x = 0.5F;
        float y = 0.5F;
        if (const auto pos = g_touch_tracker.positions.find(slot);
            pos != g_touch_tracker.positions.end()) {
            x = pos->second.first;
            y = pos->second.second;
        }
        packets.push_back(QueuedNativeTouch{kind, slot, 0, x, y, 0.0F, 0.0F});
    }
    g_touch_tracker.slots.clear();
    g_touch_tracker.positions.clear();
    g_touch_tracker.primary_slot = -1;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        std::uint16_t flags = deskmate::protocol::kInputFlagMultiTouchFrame;
        if (i + 1 == packets.size()) {
            flags = static_cast<std::uint16_t>(
                flags | deskmate::protocol::kInputFlagFrameCommit);
        }
        packets[i].flags = flags;
    }
    return packets;
}

void FlushNativeTouchPackets(const std::vector<QueuedNativeTouch> &packets)
{
    auto &engine = deskmate::device::DeviceEngine::Instance();
    for (const auto &pkt : packets) {
        (void)engine.SendInput(pkt.kind, pkt.slot, pkt.flags, pkt.x, pkt.y,
                               pkt.dx, pkt.dy, 0);
    }
}

void OnSurfaceDestroyed(OH_NativeXComponent *, void *window)
{
    OH_LOG_INFO(LOG_APP, "OnSurfaceDestroyed");
    std::vector<QueuedNativeTouch> release;
    {
        std::lock_guard lock(g_touch_mutex);
        release = BuildReleaseAllTouchesLocked(true);
        g_touch_tracker.width = 0;
        g_touch_tracker.height = 0;
    }
    FlushNativeTouchPackets(release);
    RemoveSurface(window);
}

void DispatchTouchEvent(OH_NativeXComponent *component, void *window)
{
    if (component == nullptr || window == nullptr) {
        return;
    }
    OH_NativeXComponent_TouchEvent event{};
    if (OH_NativeXComponent_GetTouchEvent(component, window, &event) !=
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }

    uint64_t width = 0;
    uint64_t height = 0;
    const bool got_size =
        OH_NativeXComponent_GetXComponentSize(component, window, &width, &height) ==
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS;

    std::vector<QueuedNativeTouch> packets;
    bool should_log = false;
    std::size_t active_contacts_after = 0;
    {
        std::lock_guard lock(g_touch_mutex);
        if (got_size && width > 0 && height > 0) {
            g_touch_tracker.width = width;
            g_touch_tracker.height = height;
        }
        if (!g_touch_tracker.enabled || g_touch_tracker.width == 0 ||
            g_touch_tracker.height == 0) {
            return;
        }

        if (event.type == OH_NATIVEXCOMPONENT_CANCEL) {
            packets = BuildReleaseAllTouchesLocked(true);
        } else {
            const float inv_w =
                1.0F / static_cast<float>(g_touch_tracker.width);
            const float inv_h =
                1.0F / static_cast<float>(g_touch_tracker.height);

            struct RawPoint {
                std::int32_t id;
                float x;
                float y;
                OH_NativeXComponent_TouchEventType type;
                bool pressed;
            };
            std::vector<RawPoint> points;
            const std::uint32_t count = std::min<std::uint32_t>(
                event.numPoints, OH_NATIVE_XCOMPONENT_MAX_TOUCH_POINTS_NUMBER);
            if (count > 0) {
                points.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i) {
                    const auto &tp = event.touchPoints[i];
                    points.push_back(RawPoint{tp.id, tp.x, tp.y, tp.type, tp.isPressed});
                }
            } else {
                points.push_back(RawPoint{
                    event.id, event.x, event.y, event.type,
                    event.type == OH_NATIVEXCOMPONENT_DOWN ||
                        event.type == OH_NATIVEXCOMPONENT_MOVE});
            }

            std::unordered_map<std::int32_t,
                               std::tuple<std::uint8_t, float, float>>
                lifted;
            std::unordered_set<std::int32_t> present_ids;
            for (const auto &pt : points) {
                present_ids.insert(pt.id);
                // Some HarmonyOS builds report touchPoints[i].isPressed=false
                // during OH_NATIVEXCOMPONENT_MOVE; classify lift by event/point
                // type rather than letting !pressed turn MOVE into TouchUp.
                const bool is_lifted_point =
                    pt.type == OH_NATIVEXCOMPONENT_UP ||
                    pt.type == OH_NATIVEXCOMPONENT_CANCEL ||
                    (event.type == OH_NATIVEXCOMPONENT_UP && pt.id == event.id) ||
                    (pt.type == OH_NATIVEXCOMPONENT_UNKNOWN &&
                     event.type != OH_NATIVEXCOMPONENT_DOWN &&
                     event.type != OH_NATIVEXCOMPONENT_MOVE && !pt.pressed);
                if (is_lifted_point) {
                    const std::uint8_t kind = static_cast<std::uint8_t>(
                        pt.type == OH_NATIVEXCOMPONENT_CANCEL
                            ? deskmate::protocol::InputKind::TouchCancel
                            : deskmate::protocol::InputKind::TouchUp);
                    lifted[pt.id] = {kind, ClampUnit(pt.x * inv_w),
                                     ClampUnit(pt.y * inv_h)};
                }
            }
            if (event.type == OH_NATIVEXCOMPONENT_UP ||
                event.type == OH_NATIVEXCOMPONENT_MOVE) {
                for (const auto &entry : g_touch_tracker.slots) {
                    if (!present_ids.contains(entry.first) &&
                        !lifted.contains(entry.first)) {
                        float x = 0.5F;
                        float y = 0.5F;
                        if (const auto pos =
                                g_touch_tracker.positions.find(entry.second);
                            pos != g_touch_tracker.positions.end()) {
                            x = pos->second.first;
                            y = pos->second.second;
                        }
                        lifted[entry.first] = {
                            static_cast<std::uint8_t>(
                                deskmate::protocol::InputKind::TouchUp),
                            x, y};
                    }
                }
            }

            for (const auto &pt : points) {
                if (lifted.contains(pt.id)) {
                    continue;
                }
                const int slot_int = AllocateNativeSlotLocked(pt.id);
                if (slot_int < 0) {
                    continue;
                }
                const auto slot = static_cast<std::uint8_t>(slot_int);
                const float nx = ClampUnit(pt.x * inv_w);
                const float ny = ClampUnit(pt.y * inv_h);
                const auto prev = g_touch_tracker.positions.find(slot);
                if (prev == g_touch_tracker.positions.end()) {
                    g_touch_tracker.positions[slot] = {nx, ny};
                    if (g_touch_tracker.primary_slot < 0) {
                        g_touch_tracker.primary_slot = slot;
                    }
                    packets.push_back(QueuedNativeTouch{
                        static_cast<std::uint8_t>(
                            deskmate::protocol::InputKind::TouchDown),
                        slot, 0, nx, ny, 0.0F, 0.0F});
                } else {
                    const float dx = nx - prev->second.first;
                    const float dy = ny - prev->second.second;
                    g_touch_tracker.positions[slot] = {nx, ny};
                    packets.push_back(QueuedNativeTouch{
                        static_cast<std::uint8_t>(
                            deskmate::protocol::InputKind::TouchMove),
                        slot, 0, nx, ny, dx, dy});
                }
            }

            for (const auto &[touch_id, info] : lifted) {
                const auto slot_it = g_touch_tracker.slots.find(touch_id);
                if (slot_it == g_touch_tracker.slots.end()) {
                    continue;
                }
                const std::uint8_t slot = slot_it->second;
                g_touch_tracker.slots.erase(slot_it);
                g_touch_tracker.positions.erase(slot);
                if (g_touch_tracker.primary_slot == slot) {
                    int next_primary = -1;
                    for (const auto &pos_entry : g_touch_tracker.positions) {
                        if (next_primary < 0 || pos_entry.first < next_primary) {
                            next_primary = pos_entry.first;
                        }
                    }
                    g_touch_tracker.primary_slot = next_primary;
                }
                packets.push_back(QueuedNativeTouch{
                    std::get<0>(info), slot, 0, std::get<1>(info),
                    std::get<2>(info), 0.0F, 0.0F});
            }

            if (!packets.empty()) {
                const std::size_t active_count = std::min<std::size_t>(
                    OH_NATIVE_XCOMPONENT_MAX_TOUCH_POINTS_NUMBER,
                    g_touch_tracker.positions.size());
                active_contacts_after = active_count;
                for (std::size_t i = 0; i < packets.size(); ++i) {
                    const auto up_kind = static_cast<std::uint8_t>(
                        deskmate::protocol::InputKind::TouchUp);
                    const auto cancel_kind = static_cast<std::uint8_t>(
                        deskmate::protocol::InputKind::TouchCancel);
                    const bool is_up =
                        packets[i].kind == up_kind ||
                        packets[i].kind == cancel_kind;
                    const bool is_primary =
                        !is_up &&
                        packets[i].slot == g_touch_tracker.primary_slot;
                    std::uint16_t flags = static_cast<std::uint16_t>(
                        deskmate::protocol::kInputFlagMultiTouchFrame |
                        ((active_count & 0x0FU)
                         << deskmate::protocol::kInputFlagContactCountShift));
                    if (is_primary) {
                        flags = static_cast<std::uint16_t>(
                            flags | deskmate::protocol::kInputFlagPrimaryButton);
                    }
                    if (i + 1 == packets.size()) {
                        flags = static_cast<std::uint16_t>(
                            flags | deskmate::protocol::kInputFlagFrameCommit);
                    }
                    packets[i].flags = flags;
                }
            }
        }
        if (!packets.empty() && g_touch_tracker.log_budget > 0) {
            --g_touch_tracker.log_budget;
            should_log = true;
        }
    }

    if (should_log && !packets.empty()) {
        const int tp0_type =
            event.numPoints > 0 ? static_cast<int>(event.touchPoints[0].type) : -1;
        const int tp0_pressed =
            event.numPoints > 0 ? (event.touchPoints[0].isPressed ? 1 : 0) : -1;
        OH_LOG_INFO(LOG_APP,
                    "native touch evType=%{public}d numPts=%{public}u "
                    "tp0=(type=%{public}d pressed=%{public}d) "
                    "batch=%{public}zu active=%{public}zu "
                    "first=(kind=%{public}u slot=%{public}u x=%{public}.3f "
                    "y=%{public}.3f)",
                    static_cast<int>(event.type), event.numPoints, tp0_type,
                    tp0_pressed, packets.size(), active_contacts_after,
                    packets[0].kind, packets[0].slot, packets[0].x,
                    packets[0].y);
    }
    FlushNativeTouchPackets(packets);
}

OH_NativeXComponent_Callback g_xComponentCallback = {
    OnSurfaceCreated,
    OnSurfaceChanged,
    OnSurfaceDestroyed,
    DispatchTouchEvent,
};

napi_value Boolean(napi_env env, bool value)
{
    napi_value result = nullptr;
    if (napi_get_boolean(env, value, &result) != napi_ok) {
        return nullptr;
    }
    return result;
}

napi_value String(napi_env env, const std::string &value)
{
    napi_value result = nullptr;
    if (napi_create_string_utf8(env, value.data(), value.size(), &result) !=
        napi_ok) {
        return nullptr;
    }
    return result;
}

bool ReadString(napi_env env, napi_value value, std::string &output)
{
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, value, &type) != napi_ok || type != napi_string) {
        return false;
    }
    std::size_t length = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
        return false;
    }
    std::vector<char> buffer(length + 1);
    std::size_t copied = 0;
    if (napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(),
                                   &copied) != napi_ok) {
        return false;
    }
    output.assign(buffer.data(), copied);
    return true;
}

napi_value StartSession(napi_env env, napi_callback_info info)
{
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    std::int32_t fd = -1;
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
        argc < 1 || napi_get_value_int32(env, argv[0], &fd) != napi_ok) {
        return Boolean(env, false);
    }
    return Boolean(env, deskmate::device::DeviceEngine::Instance().StartSession(fd));
}

napi_value StopSession(napi_env env, napi_callback_info)
{
    return Boolean(env, deskmate::device::DeviceEngine::Instance().StopSession());
}

napi_value RequestStop(napi_env env, napi_callback_info)
{
    deskmate::device::DeviceEngine::Instance().RequestStop();
    return Boolean(env, true);
}

napi_value IsRunning(napi_env env, napi_callback_info)
{
    return Boolean(env, deskmate::device::DeviceEngine::Instance().Running());
}

napi_value SurfaceCreated(napi_env env, napi_callback_info info)
{
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    std::string surface_id;
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
        argc < 1 || !ReadString(env, argv[0], surface_id)) {
        return Boolean(env, false);
    }
    return Boolean(env,
                   deskmate::device::DeviceEngine::Instance().SurfaceCreated(surface_id));
}

napi_value SurfaceDestroyed(napi_env env, napi_callback_info info)
{
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    std::string surface_id;
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
        argc < 1 || !ReadString(env, argv[0], surface_id)) {
        return Boolean(env, false);
    }
    return Boolean(
        env, deskmate::device::DeviceEngine::Instance().SurfaceDestroyed(surface_id));
}

napi_value SendInput(napi_env env, napi_callback_info info)
{
    std::size_t argc = 8;
    napi_value argv[8] = {nullptr};
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
        argc < 8) {
        return Boolean(env, false);
    }
    std::int32_t kind = 0;
    std::int32_t pointer_id = 0;
    std::int32_t flags = 0;
    double x = 0;
    double y = 0;
    double dx = 0;
    double dy = 0;
    std::int64_t keycode = 0;
    if (napi_get_value_int32(env, argv[0], &kind) != napi_ok ||
        napi_get_value_int32(env, argv[1], &pointer_id) != napi_ok ||
        napi_get_value_int32(env, argv[2], &flags) != napi_ok ||
        napi_get_value_double(env, argv[3], &x) != napi_ok ||
        napi_get_value_double(env, argv[4], &y) != napi_ok ||
        napi_get_value_double(env, argv[5], &dx) != napi_ok ||
        napi_get_value_double(env, argv[6], &dy) != napi_ok ||
        napi_get_value_int64(env, argv[7], &keycode) != napi_ok) {
        return Boolean(env, false);
    }
    return Boolean(
        env, deskmate::device::DeviceEngine::Instance().SendInput(
                 static_cast<std::uint8_t>(kind),
                 static_cast<std::uint8_t>(pointer_id),
                 static_cast<std::uint16_t>(flags), static_cast<float>(x),
                 static_cast<float>(y), static_cast<float>(dx),
                 static_cast<float>(dy), static_cast<std::uint32_t>(keycode)));
}

napi_value NoteLifecycle(napi_env env, napi_callback_info info)
{
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    std::string event;
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
        argc < 1 || !ReadString(env, argv[0], event)) {
        return Boolean(env, false);
    }
    deskmate::device::DeviceEngine::Instance().NoteLifecycle(event);
    return Boolean(env, true);
}

napi_value SetPreferredCodec(napi_env env, napi_callback_info info)
{
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    std::string codec;
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
        argc < 1 || !ReadString(env, argv[0], codec)) {
        return Boolean(env, false);
    }
    deskmate::device::DeviceEngine::Instance().SetPreferredCodec(codec);
    return Boolean(env, true);
}

napi_value SetDisplayMetrics(napi_env env, napi_callback_info info)
{
    std::size_t argc = 3;
    napi_value argv[3] = {nullptr};
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
        argc < 3) {
        return Boolean(env, false);
    }
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t refresh_hz = 0;
    if (napi_get_value_int32(env, argv[0], &width) != napi_ok ||
        napi_get_value_int32(env, argv[1], &height) != napi_ok ||
        napi_get_value_int32(env, argv[2], &refresh_hz) != napi_ok) {
        return Boolean(env, false);
    }
    deskmate::device::DeviceEngine::Instance().SetDisplayMetrics(width, height,
                                                                 refresh_hz);
    return Boolean(env, true);
}

napi_value SnapshotJson(napi_env env, napi_callback_info)
{
    return String(env,
                  deskmate::device::DeviceEngine::Instance().SnapshotJson());
}

napi_value SetTouchEnabled(napi_env env, napi_callback_info info)
{
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    bool enabled = false;
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
        argc < 1 || napi_get_value_bool(env, argv[0], &enabled) != napi_ok) {
        return Boolean(env, false);
    }
    std::vector<QueuedNativeTouch> release;
    {
        std::lock_guard lock(g_touch_mutex);
        g_touch_tracker.enabled = enabled;
        if (!enabled) {
            release = BuildReleaseAllTouchesLocked(true);
        } else {
            g_touch_tracker.log_budget = 12;
        }
    }
    FlushNativeTouchPackets(release);
    OH_LOG_INFO(LOG_APP, "native touch capture enabled=%{public}d",
                enabled ? 1 : 0);
    return Boolean(env, true);
}

void RegisterXComponent(napi_env env, napi_value exports)
{
    napi_value export_instance = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ,
                                &export_instance) != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "XComponent export object is missing");
        return;
    }
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, export_instance, &type) != napi_ok ||
        type != napi_object) {
        return;
    }
    OH_NativeXComponent *component = nullptr;
    if (napi_unwrap(env, export_instance, reinterpret_cast<void **>(&component)) !=
            napi_ok ||
        component == nullptr) {
        OH_LOG_ERROR(LOG_APP, "XComponent unwrap failed");
        return;
    }
    const int32_t result =
        OH_NativeXComponent_RegisterCallback(component, &g_xComponentCallback);
    OH_LOG_INFO(LOG_APP, "XComponent callback registration result=%{public}d",
                result);
}

napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor properties[] = {
        {"startSession", nullptr, StartSession, nullptr, nullptr, nullptr,
         napi_default, nullptr},
        {"stopSession", nullptr, StopSession, nullptr, nullptr, nullptr,
         napi_default, nullptr},
        {"requestStop", nullptr, RequestStop, nullptr, nullptr, nullptr,
         napi_default, nullptr},
        {"isRunning", nullptr, IsRunning, nullptr, nullptr, nullptr,
         napi_default, nullptr},
        {"surfaceCreated", nullptr, SurfaceCreated, nullptr, nullptr, nullptr,
         napi_default, nullptr},
        {"surfaceDestroyed", nullptr, SurfaceDestroyed, nullptr, nullptr,
         nullptr, napi_default, nullptr},
        {"sendInput", nullptr, SendInput, nullptr, nullptr, nullptr,
         napi_default, nullptr},
        {"noteLifecycle", nullptr, NoteLifecycle, nullptr, nullptr, nullptr,
         napi_default, nullptr},
        {"setPreferredCodec", nullptr, SetPreferredCodec, nullptr, nullptr,
         nullptr, napi_default, nullptr},
        {"setDisplayMetrics", nullptr, SetDisplayMetrics, nullptr, nullptr,
         nullptr, napi_default, nullptr},
        {"setTouchEnabled", nullptr, SetTouchEnabled, nullptr, nullptr, nullptr,
         napi_default, nullptr},
        {"snapshotJson", nullptr, SnapshotJson, nullptr, nullptr, nullptr,
         napi_default, nullptr},
    };
    if (napi_define_properties(env, exports,
                               sizeof(properties) / sizeof(properties[0]),
                               properties) != napi_ok) {
        return exports;
    }
    RegisterXComponent(env, exports);
    return exports;
}

}  // namespace

static napi_module g_deviceModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "deskmate_device",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterDeskMateDeviceModule(void)
{
    napi_module_register(&g_deviceModule);
}
