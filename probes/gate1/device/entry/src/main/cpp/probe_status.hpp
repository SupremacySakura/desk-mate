#pragma once

#include <cstring>
#include <sstream>
#include <string>

namespace gate1 {

constexpr bool probe_reports_running(bool running, bool worker_done)
{
    return running && !worker_done;
}

inline std::string probe_error_context(
    int error_number,
    int fd,
    int revents,
    const char* mode)
{
    std::ostringstream context;
    context << "errno=" << error_number
            << " (" << std::strerror(error_number) << ")"
            << " fd=" << fd
            << " mode=" << (mode == nullptr ? "none" : mode)
            << " revents=0x" << std::hex << revents;
    return context.str();
}

} // namespace gate1
