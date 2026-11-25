#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace deskmate::platform {

struct NalUnit {
  std::size_t offset{};
  std::size_t size{};
};

[[nodiscard]] bool looks_like_annex_b(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::vector<NalUnit> split_annex_b(
    std::span<const std::uint8_t> bytes);

// Returns SPS/PPS (H.264) or VPS/SPS/PPS (HEVC) in Annex-B form. Both
// MediaCodec CSD and Media Foundation keyframe preambles use this helper.
[[nodiscard]] std::vector<std::uint8_t> collect_parameter_sets(
    std::span<const std::uint8_t> annex_b, bool hevc);

// Media Foundation encoders commonly emit AVCC length-prefixed NAL units.
// This accepts either representation and always returns Annex-B.
[[nodiscard]] std::vector<std::uint8_t> to_annex_b(
    std::span<const std::uint8_t> bytes, bool hevc);

[[nodiscard]] bool is_parameter_set(std::uint8_t nal_header, bool hevc);
[[nodiscard]] bool is_keyframe_nal(std::uint8_t nal_header, bool hevc);

}  // namespace deskmate::platform
