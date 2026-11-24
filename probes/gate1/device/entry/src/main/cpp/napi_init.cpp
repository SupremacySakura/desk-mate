#include "accessory_probe.hpp"

#include <napi/native_api.h>

#include <string>
#include <vector>

namespace {

gate1::AccessoryProbe& probe_instance()
{
    static gate1::AccessoryProbe* instance = new gate1::AccessoryProbe();
    return *instance;
}

napi_value make_string(napi_env env, const std::string& value)
{
    napi_value result = nullptr;
    napi_create_string_utf8(env, value.c_str(), value.size(), &result);
    return result;
}

napi_value make_undefined(napi_env env)
{
    napi_value result = nullptr;
    napi_get_undefined(env, &result);
    return result;
}

bool read_string(napi_env env, napi_value value, std::string& output)
{
    std::size_t length = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
        return false;
    }
    std::vector<char> buffer(length + 1, '\0');
    std::size_t copied = 0;
    if (napi_get_value_string_utf8(
            env,
            value,
            buffer.data(),
            buffer.size(),
            &copied) != napi_ok) {
        return false;
    }
    output.assign(buffer.data(), copied);
    return true;
}

napi_value start_probe(napi_env env, napi_callback_info info)
{
    std::size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
        argc < 2) {
        return make_string(env, "{\"ok\":false,\"message\":\"startProbe expects fd and mode\"}");
    }
    std::int32_t fd = 0;
    std::string mode;
    if (napi_get_value_int32(env, argv[0], &fd) != napi_ok ||
        !read_string(env, argv[1], mode)) {
        return make_string(env, "{\"ok\":false,\"message\":\"invalid startProbe arguments\"}");
    }
    return make_string(env, probe_instance().start(fd, mode));
}

napi_value stop_and_join_probe(napi_env env, napi_callback_info info)
{
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    std::string mode;
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok ||
        argc < 1 || !read_string(env, argv[0], mode)) {
        return make_string(env, "{\"ok\":false,\"message\":\"stopAndJoinProbe expects mode\"}");
    }
    return make_string(env, probe_instance().stop_and_join(mode));
}

napi_value begin_owner_close_stop(napi_env env, napi_callback_info info)
{
    (void)info;
    return make_string(env, probe_instance().begin_owner_close_stop());
}

napi_value finish_owner_close_join(napi_env env, napi_callback_info info)
{
    (void)info;
    return make_string(env, probe_instance().finish_owner_close_join());
}

napi_value get_stats_json(napi_env env, napi_callback_info info)
{
    (void)info;
    return make_string(env, probe_instance().stats_json());
}

napi_value note_lifecycle(napi_env env, napi_callback_info info)
{
    std::size_t argc = 1;
    napi_value argv[1] = {nullptr};
    std::string event;
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) == napi_ok &&
        argc >= 1 && read_string(env, argv[0], event)) {
        probe_instance().note_lifecycle(event);
    }
    return make_undefined(env);
}

napi_value is_running(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value result = nullptr;
    napi_get_boolean(env, probe_instance().running(), &result);
    return result;
}

napi_value init(napi_env env, napi_value exports)
{
    napi_property_descriptor properties[] = {
        {"startProbe", nullptr, start_probe, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopAndJoinProbe", nullptr, stop_and_join_probe, nullptr, nullptr, nullptr,
         napi_default, nullptr},
        {"beginOwnerCloseStop", nullptr, begin_owner_close_stop, nullptr, nullptr, nullptr,
         napi_default, nullptr},
        {"finishOwnerCloseJoin", nullptr, finish_owner_close_join, nullptr, nullptr,
         nullptr, napi_default, nullptr},
        {"getStatsJson", nullptr, get_stats_json, nullptr, nullptr, nullptr, napi_default,
         nullptr},
        {"noteLifecycle", nullptr, note_lifecycle, nullptr, nullptr, nullptr, napi_default,
         nullptr},
        {"isRunning", nullptr, is_running, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(
        env,
        exports,
        sizeof(properties) / sizeof(properties[0]),
        properties);
    return exports;
}

} // namespace

static napi_module g_gate1_probe_module = {
    1,
    0,
    __FILE__,
    init,
    "gate1_probe",
    nullptr,
    {0},
};

extern "C" __attribute__((constructor)) void RegisterGate1ProbeModule(void)
{
    napi_module_register(&g_gate1_probe_module);
}
