#pragma once

#include "deskmate/session.hpp"

#include <cstddef>
#include <cstdint>

namespace deskmate::platform {

// A 12 KiB video payload plus the 32-byte header stays below the 16 KiB
// HarmonyOS accessory read buffer. This is a transport constraint, not a wire
// protocol change: the receiver still accepts any legal 1..65536-byte fragment.
inline constexpr std::size_t kVideoFragmentPayload = 12U * 1024U;
inline constexpr std::uint64_t kAoaHandshakeTimeoutNs = 20'000'000'000ULL;
inline constexpr std::uint64_t kAoaInitialIdrTimeoutNs = 10'000'000'000ULL;

[[nodiscard]] deskmate::protocol::CapabilitySet make_host_capabilities(
    std::uint16_t width, std::uint16_t height, std::uint16_t refresh_hz,
    bool allow_hevc);

[[nodiscard]] deskmate::protocol::CapabilitySet make_device_capabilities(
    std::uint16_t width, std::uint16_t height, std::uint16_t refresh_hz,
    bool prefer_hevc);

[[nodiscard]] deskmate::protocol::HostSessionConfig make_host_session_config(
    std::uint32_t video_stream_id, std::uint32_t config_id,
    std::uint16_t width, std::uint16_t height, std::uint16_t refresh_hz,
    bool allow_hevc, bool accept_device_resolution);

[[nodiscard]] deskmate::protocol::DeviceSessionConfig make_device_session_config(
    std::uint32_t session_nonce, std::uint16_t width, std::uint16_t height,
    std::uint16_t refresh_hz, bool prefer_hevc);

}  // namespace deskmate::platform
