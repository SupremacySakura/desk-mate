#include "deskmate/platform/session_profile.hpp"

#include <algorithm>

namespace deskmate::platform {
namespace {

using deskmate::protocol::CapabilitySet;
using deskmate::protocol::DisplayResolution;
using deskmate::protocol::PixelFormat;
using deskmate::protocol::VideoEncoder;

void append_resolution_unique(CapabilitySet& capabilities, std::uint16_t width,
                              std::uint16_t height, std::uint16_t refresh_hz) {
  if (width == 0 || height == 0 || refresh_hz == 0 ||
      capabilities.resolutions.size() >=
          deskmate::protocol::kMaxCapabilityItems) {
    return;
  }
  const DisplayResolution candidate{width, height, refresh_hz};
  if (std::find(capabilities.resolutions.begin(), capabilities.resolutions.end(),
                candidate) == capabilities.resolutions.end()) {
    capabilities.resolutions.push_back(candidate);
  }
}

CapabilitySet base_capabilities(std::uint16_t width, std::uint16_t height,
                                std::uint16_t refresh_hz) {
  CapabilitySet capabilities;
  capabilities.pixel_formats = {PixelFormat::Nv12};
  append_resolution_unique(capabilities, width, height, refresh_hz);
  if (refresh_hz > 90) {
    append_resolution_unique(capabilities, width, height, 90);
  }
  if (refresh_hz > 60) {
    append_resolution_unique(capabilities, width, height, 60);
  }
  return capabilities;
}

}  // namespace

CapabilitySet make_host_capabilities(std::uint16_t width, std::uint16_t height,
                                     std::uint16_t refresh_hz,
                                     bool allow_hevc) {
  CapabilitySet capabilities = base_capabilities(width, height, refresh_hz);
  capabilities.encoders =
      allow_hevc ? std::vector<VideoEncoder>{VideoEncoder::Hevc,
                                             VideoEncoder::H264}
                 : std::vector<VideoEncoder>{VideoEncoder::H264};
  return capabilities;
}

CapabilitySet make_device_capabilities(std::uint16_t width,
                                       std::uint16_t height,
                                       std::uint16_t refresh_hz,
                                       bool prefer_hevc) {
  CapabilitySet capabilities = base_capabilities(width, height, refresh_hz);
  capabilities.encoders =
      prefer_hevc ? std::vector<VideoEncoder>{VideoEncoder::Hevc,
                                              VideoEncoder::H264}
                  : std::vector<VideoEncoder>{VideoEncoder::H264};
  append_resolution_unique(capabilities, 2800, 1840, 120);
  append_resolution_unique(capabilities, 2800, 1840, 60);
  append_resolution_unique(capabilities, 1920, 1200, 60);
  append_resolution_unique(capabilities, 1280, 800, 60);
  return capabilities;
}

deskmate::protocol::HostSessionConfig make_host_session_config(
    std::uint32_t video_stream_id, std::uint32_t config_id,
    std::uint16_t width, std::uint16_t height, std::uint16_t refresh_hz,
    bool allow_hevc, bool accept_device_resolution) {
  deskmate::protocol::HostSessionConfig config;
  config.video_stream_id = video_stream_id;
  config.initial_config_id = config_id;
  config.capabilities =
      make_host_capabilities(width, height, refresh_hz, allow_hevc);
  config.accept_device_resolution = accept_device_resolution;
  config.limits.video_fragment_payload = kVideoFragmentPayload;
  config.limits.stage_timeout_ns = kAoaHandshakeTimeoutNs;
  config.limits.initial_idr_timeout_ns = kAoaInitialIdrTimeoutNs;
  config.scheduler.max_video_urbs = 8;
  config.scheduler.max_pending_video_bytes = 2U * 1024U * 1024U;
  config.scheduler.max_oldest_unsent_frame_age_ns = 250'000'000ULL;
  return config;
}

deskmate::protocol::DeviceSessionConfig make_device_session_config(
    std::uint32_t session_nonce, std::uint16_t width, std::uint16_t height,
    std::uint16_t refresh_hz, bool prefer_hevc) {
  deskmate::protocol::DeviceSessionConfig config;
  config.session_nonce = session_nonce;
  config.capabilities =
      make_device_capabilities(width, height, refresh_hz, prefer_hevc);
  config.limits.stage_timeout_ns = kAoaHandshakeTimeoutNs;
  config.limits.initial_idr_timeout_ns = kAoaInitialIdrTimeoutNs;
  return config;
}

}  // namespace deskmate::platform
