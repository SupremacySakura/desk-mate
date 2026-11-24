#include "ace/xcomponent/native_interface_xcomponent.h"
#include "napi/native_api.h"
#include "probe_engine.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

std::mutex g_surface_mutex;
std::string g_surface_key;

std::string SurfaceKey(void *window)
{
    return window == nullptr ? std::string() : std::to_string(reinterpret_cast<std::uintptr_t>(window));
}

bool UpdateSurface(void *window)
{
    const std::string key = SurfaceKey(window);
    if (key.empty()) {
        return false;
    }

    std::lock_guard lock(g_surface_mutex);
    if (key == g_surface_key) {
        return true;
    }
    if (!g_surface_key.empty()) {
        (void)deskmate::gate3::probe::ProbeEngine::Instance().SurfaceDestroyed(g_surface_key);
        g_surface_key.clear();
    }
    if (!deskmate::gate3::probe::ProbeEngine::Instance().SurfaceCreated(key)) {
        return false;
    }
    g_surface_key = key;
    return true;
}

void RemoveSurface(void *window)
{
    const std::string key = SurfaceKey(window);
    std::lock_guard lock(g_surface_mutex);
    if (key.empty() || key != g_surface_key) {
        return;
    }
    g_surface_key.clear();
    (void)deskmate::gate3::probe::ProbeEngine::Instance().SurfaceDestroyed(key);
}

void OnSurfaceCreated(OH_NativeXComponent *, void *window)
{
    (void)UpdateSurface(window);
}

void OnSurfaceChanged(OH_NativeXComponent *, void *window)
{
    (void)UpdateSurface(window);
}

void OnSurfaceDestroyed(OH_NativeXComponent *, void *window)
{
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
    if (napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(), &copied) != napi_ok) {
        return false;
    }
    output.assign(buffer.data(), copied);
    return true;
}

bool ReadStringArgument(napi_env env, napi_callback_info info, std::string &output)
{
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) {
        return false;
    }
    return ReadString(env, argv[0], output);
}

bool ReadBytesArgument(napi_env env, napi_callback_info info, std::vector<std::uint8_t> &output)
{
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) {
        return false;
    }

    bool is_typed_array = false;
    if (napi_is_typedarray(env, argv[0], &is_typed_array) == napi_ok && is_typed_array) {
        napi_typedarray_type type = napi_uint8_array;
        std::size_t length = 0;
        void *data = nullptr;
        napi_value array_buffer = nullptr;
        std::size_t byte_offset = 0;
        if (napi_get_typedarray_info(env, argv[0], &type, &length, &data, &array_buffer, &byte_offset) != napi_ok ||
            type != napi_uint8_array) {
            return false;
        }
        output.clear();
        if (length > 0) {
            if (data == nullptr) {
                return false;
            }
            const auto *bytes = static_cast<const std::uint8_t *>(data);
            output.assign(bytes, bytes + length);
        }
        return true;
    }

    bool is_array_buffer = false;
    if (napi_is_arraybuffer(env, argv[0], &is_array_buffer) == napi_ok && is_array_buffer) {
        void *data = nullptr;
        std::size_t length = 0;
        if (napi_get_arraybuffer_info(env, argv[0], &data, &length) != napi_ok) {
            return false;
        }
        output.clear();
        if (length > 0) {
            if (data == nullptr) {
                return false;
            }
            const auto *bytes = static_cast<const std::uint8_t *>(data);
            output.assign(bytes, bytes + length);
        }
        return true;
    }
    return false;
}

napi_value LoadStream(napi_env env, napi_callback_info info)
{
    std::vector<std::uint8_t> bytes;
    if (!ReadBytesArgument(env, info, bytes)) {
        return Boolean(env, false);
    }
    return Boolean(env, deskmate::gate3::probe::ProbeEngine::Instance().LoadStream(std::move(bytes)));
}

napi_value SurfaceCreated(napi_env env, napi_callback_info info)
{
    std::string surface_id;
    if (!ReadStringArgument(env, info, surface_id)) {
        return Boolean(env, false);
    }
    return Boolean(env, deskmate::gate3::probe::ProbeEngine::Instance().SurfaceCreated(surface_id));
}

napi_value SurfaceDestroyed(napi_env env, napi_callback_info info)
{
    std::string surface_id;
    if (!ReadStringArgument(env, info, surface_id)) {
        return Boolean(env, false);
    }
    return Boolean(env, deskmate::gate3::probe::ProbeEngine::Instance().SurfaceDestroyed(surface_id));
}

napi_value RunSuite(napi_env env, napi_callback_info)
{
    return Boolean(env, deskmate::gate3::probe::ProbeEngine::Instance().RunSuite());
}

napi_value BeginFreeze(napi_env env, napi_callback_info)
{
    return Boolean(env, deskmate::gate3::probe::ProbeEngine::Instance().BeginFreeze());
}

napi_value RecordLastGoodFrame(napi_env env, napi_callback_info info)
{
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    bool stable = false;
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1 ||
        napi_get_value_bool(env, argv[0], &stable) != napi_ok) {
        return Boolean(env, false);
    }
    return Boolean(env, deskmate::gate3::probe::ProbeEngine::Instance().RecordLastGoodFrame(stable));
}

napi_value ResumeAfterFreeze(napi_env env, napi_callback_info)
{
    return Boolean(env, deskmate::gate3::probe::ProbeEngine::Instance().ResumeAfterFreeze());
}

napi_value SetDeviceInfo(napi_env env, napi_callback_info info)
{
    std::string json;
    if (!ReadStringArgument(env, info, json)) {
        return Boolean(env, false);
    }
    return Boolean(env, deskmate::gate3::probe::ProbeEngine::Instance().SetDeviceInfo(std::move(json)));
}

napi_value SnapshotJson(napi_env env, napi_callback_info)
{
    const std::string json = deskmate::gate3::probe::ProbeEngine::Instance().SnapshotJson();
    napi_value result = nullptr;
    if (napi_create_string_utf8(env, json.data(), json.size(), &result) != napi_ok) {
        return nullptr;
    }
    return result;
}

void RegisterXComponent(napi_env env, napi_value exports)
{
    napi_value export_instance = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &export_instance) != napi_ok) {
        return;
    }
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, export_instance, &type) != napi_ok || type != napi_object) {
        return;
    }

    OH_NativeXComponent *component = nullptr;
    if (napi_unwrap(env, export_instance, reinterpret_cast<void **>(&component)) != napi_ok || component == nullptr) {
        return;
    }
    (void)OH_NativeXComponent_RegisterCallback(component, &g_xComponentCallback);
}

napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor properties[] = {
        {"loadStream", nullptr, LoadStream, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"surfaceCreated", nullptr, SurfaceCreated, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"surfaceDestroyed", nullptr, SurfaceDestroyed, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runSuite", nullptr, RunSuite, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"snapshotJson", nullptr, SnapshotJson, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"beginFreeze", nullptr, BeginFreeze, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"recordLastGoodFrame", nullptr, RecordLastGoodFrame, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resumeAfterFreeze", nullptr, ResumeAfterFreeze, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDeviceInfo", nullptr, SetDeviceInfo, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    if (napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]), properties) != napi_ok) {
        return exports;
    }
    RegisterXComponent(env, exports);
    return exports;
}

} // namespace

static napi_module g_entryModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void)
{
    napi_module_register(&g_entryModule);
}
