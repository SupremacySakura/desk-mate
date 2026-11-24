#pragma once

namespace gate1 {

struct ResourceRelease {
    bool close_handle;
    bool exit_context;
};

constexpr ResourceRelease resource_release(bool handle_open, bool transfers_finished)
{
    const bool safe = !handle_open || transfers_finished;
    return ResourceRelease{safe, safe};
}

} // namespace gate1
