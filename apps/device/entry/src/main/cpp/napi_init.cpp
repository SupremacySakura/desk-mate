#include "ace/xcomponent/native_interface_xcomponent.h"
#include "device_engine.hpp"
#include "napi/native_api.h"

#include <cstdint>
#include <mutex>
#include <string>
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

void OnSurfaceCreated(OH_NativeXComponent *, void *window)
{
    OH_LOG_INFO(LOG_APP, "OnSurfaceCreated");
    (void)UpdateSurface(window);
}

void OnSurfaceChanged(OH_NativeXComponent *, void *window)
{
    OH_LOG_INFO(LOG_APP, "OnSurfaceChanged");
    (void)UpdateSurface(window);
}

void OnSurfaceDestroyed(OH_NativeXComponent *, void *window)
{
    OH_LOG_INFO(LOG_APP, "OnSurfaceDestroyed");
    RemoveSurface(window);
}

void DispatchTouchEvent(OH_NativeXComponent *, void *)
{
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
