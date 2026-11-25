#include "deskmate/platform/annex_b.hpp"

#include <algorithm>

namespace deskmate::platform {
namespace {

constexpr std::uint8_t kH264Sps = 7;
constexpr std::uint8_t kH264Pps = 8;
constexpr std::uint8_t kH264Idr = 5;
constexpr std::uint8_t kHevcVps = 32;
constexpr std::uint8_t kHevcSps = 33;
constexpr std::uint8_t kHevcPps = 34;

std::size_t start_code_length(std::span<const std::uint8_t> bytes,
                              std::size_t offset) {
  if (offset + 3 <= bytes.size() && bytes[offset] == 0 &&
      bytes[offset + 1] == 0 && bytes[offset + 2] == 1) {
    return 3;
  }
  if (offset + 4 <= bytes.size() && bytes[offset] == 0 &&
      bytes[offset + 1] == 0 && bytes[offset + 2] == 0 &&
      bytes[offset + 3] == 1) {
    return 4;
  }
  return 0;
}

void append_start_code(std::vector<std::uint8_t>& out) {
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  out.push_back(1);
}

}  // namespace

bool looks_like_annex_b(std::span<const std::uint8_t> bytes) {
  return start_code_length(bytes, 0) != 0;
}

std::vector<NalUnit> split_annex_b(std::span<const std::uint8_t> bytes) {
  std::vector<NalUnit> units;
  std::size_t cursor = 0;
  std::size_t current_start = 0;
  bool in_nal = false;
  while (cursor + 3 <= bytes.size()) {
    const std::size_t prefix = start_code_length(bytes, cursor);
    if (prefix == 0) {
      ++cursor;
      continue;
    }
    if (in_nal) {
      const std::size_t end = cursor;
      if (end > current_start) {
        units.push_back(NalUnit{current_start, end - current_start});
      }
    }
    cursor += prefix;
    current_start = cursor;
    in_nal = true;
  }
  if (in_nal && current_start < bytes.size()) {
    units.push_back(NalUnit{current_start, bytes.size() - current_start});
  }
  return units;
}

bool is_parameter_set(std::uint8_t nal_header, bool hevc) {
  if (hevc) {
    const std::uint8_t type = static_cast<std::uint8_t>((nal_header >> 1U) & 0x3FU);
    return type == kHevcVps || type == kHevcSps || type == kHevcPps;
  }
  const std::uint8_t type = static_cast<std::uint8_t>(nal_header & 0x1FU);
  return type == kH264Sps || type == kH264Pps;
}

bool is_keyframe_nal(std::uint8_t nal_header, bool hevc) {
  if (hevc) {
    const std::uint8_t type = static_cast<std::uint8_t>((nal_header >> 1U) & 0x3FU);
    return type >= 16 && type <= 23;
  }
  return (nal_header & 0x1FU) == kH264Idr;
}

std::vector<std::uint8_t> collect_parameter_sets(
    std::span<const std::uint8_t> annex_b, bool hevc) {
  std::vector<std::uint8_t> out;
  for (const NalUnit& unit : split_annex_b(annex_b)) {
    if (unit.size == 0) {
      continue;
    }
    if (!is_parameter_set(annex_b[unit.offset], hevc)) {
      continue;
    }
    append_start_code(out);
    out.insert(out.end(), annex_b.begin() + static_cast<std::ptrdiff_t>(unit.offset),
               annex_b.begin() + static_cast<std::ptrdiff_t>(unit.offset + unit.size));
  }
  return out;
}

std::vector<std::uint8_t> to_annex_b(std::span<const std::uint8_t> bytes,
                                     bool hevc) {
  if (bytes.empty()) {
    return {};
  }
  if (looks_like_annex_b(bytes)) {
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
  }

  std::vector<std::uint8_t> out;
  std::size_t offset = 0;
  while (offset + 4 <= bytes.size()) {
    const std::uint32_t length =
        (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
        static_cast<std::uint32_t>(bytes[offset + 3]);
    offset += 4;
    if (length == 0 || offset + length > bytes.size()) {
      break;
    }
    append_start_code(out);
    out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
               bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
    offset += length;
  }
  if (out.empty()) {
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
  }
  (void)hevc;
  return out;
}

}  // namespace deskmate::platform
