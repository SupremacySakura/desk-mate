#include "deskmate/platform/aoa.hpp"

#include <algorithm>
#include <cctype>

namespace deskmate::platform {
namespace {

std::string trim(std::string value) {
  const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [&](unsigned char ch) { return !is_space(ch); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [&](unsigned char ch) { return !is_space(ch); })
                  .base(),
              value.end());
  return value;
}

std::string hex_byte(std::uint8_t value) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  text.push_back(kDigits[(value >> 4U) & 0x0FU]);
  text.push_back(kDigits[value & 0x0FU]);
  return text;
}

}  // namespace

bool is_aoa_accessory(std::uint16_t vendor_id,
                      std::uint16_t product_id) noexcept {
  return vendor_id == kAccessoryVendorId &&
         product_id >= kAccessoryProductIdMin &&
         product_id <= kAccessoryProductIdMax;
}

bool is_aoa_hdc_candidate(std::uint16_t vendor_id,
                          std::uint16_t product_id) noexcept {
  return vendor_id == kHdcVendorId && product_id == kHdcProductId;
}

std::string aoa_device_id(const AoaIdentity& identity) {
  const std::string serial = trim(identity.serial);
  if (!serial.empty()) {
    return "usb:" + serial;
  }
  if (!identity.port_path.empty()) {
    return "usb:port:" + identity.port_path;
  }
  return "usb:" + hex_byte(identity.bus) + ":" + hex_byte(identity.address);
}

std::string aoa_display_name(const AoaIdentity& identity) {
  const std::string product = trim(identity.product);
  if (!product.empty()) {
    return product;
  }
  const std::string serial = trim(identity.serial);
  if (!serial.empty()) {
    return serial;
  }
  return "DeskMate device";
}

std::string aoa_identity_key(const AoaIdentity& identity) {
  if (!identity.port_path.empty()) {
    return "port:" + identity.port_path;
  }
  return aoa_device_id(identity);
}

bool select_bulk_endpoints(std::span<const EndpointDescriptorView> endpoints,
                           EndpointPair& out) {
  constexpr std::uint8_t kBulkMask = 0x03U;
  constexpr std::uint8_t kBulkType = 0x02U;
  constexpr std::uint8_t kDirectionMask = 0x80U;
  for (const auto& descriptor : endpoints) {
    if ((descriptor.attributes & kBulkMask) != kBulkType) {
      continue;
    }
    const std::uint8_t interface_number = descriptor.interface_number;
    int candidate_in = -1;
    int candidate_out = -1;
    for (const auto& candidate : endpoints) {
      if (candidate.interface_number != interface_number ||
          (candidate.attributes & kBulkMask) != kBulkType) {
        continue;
      }
      if ((candidate.address & kDirectionMask) != 0) {
        if (candidate_in < 0) {
          candidate_in = candidate.address;
        }
      } else if (candidate_out < 0) {
        candidate_out = candidate.address;
      }
    }
    if (candidate_in >= 0 && candidate_out >= 0) {
      out.interface_number = interface_number;
      out.endpoint_in = static_cast<std::uint8_t>(candidate_in);
      out.endpoint_out = static_cast<std::uint8_t>(candidate_out);
      return true;
    }
  }
  return false;
}

std::vector<std::uint8_t> encode_aoa_string(std::string_view text) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(text.size() + 1);
  for (const char ch : text) {
    bytes.push_back(static_cast<std::uint8_t>(ch));
  }
  bytes.push_back(0);
  return bytes;
}

}  // namespace deskmate::platform
