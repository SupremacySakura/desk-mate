#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace deskmate::platform {

// USB IDs used by the AOA handshake. These values are shared by the macOS
// reference host and the Windows/Android ports; they are not protocol fields.
inline constexpr std::uint16_t kHdcVendorId = 0x12D1;
inline constexpr std::uint16_t kHdcProductId = 0x5000;
inline constexpr std::uint16_t kAccessoryVendorId = 0x18D1;
inline constexpr std::uint16_t kAccessoryProductIdMin = 0x2D00;
inline constexpr std::uint16_t kAccessoryProductIdMax = 0x2D05;

inline constexpr std::uint8_t kAoaGetProtocol = 51;
inline constexpr std::uint8_t kAoaSendString = 52;
inline constexpr std::uint8_t kAoaStart = 53;

[[nodiscard]] bool is_aoa_accessory(std::uint16_t vendor_id,
                                    std::uint16_t product_id) noexcept;
[[nodiscard]] bool is_aoa_hdc_candidate(std::uint16_t vendor_id,
                                        std::uint16_t product_id) noexcept;

struct AoaIdentity {
  std::uint8_t bus{};
  std::uint8_t address{};
  std::string port_path;
  std::string serial;
  std::string manufacturer;
  std::string product;
};

// Stable identity for a link. A serial is preferred; when an accessory omits
// it, the physical port path keeps two simultaneous devices distinct.
[[nodiscard]] std::string aoa_device_id(const AoaIdentity& identity);
[[nodiscard]] std::string aoa_display_name(const AoaIdentity& identity);
[[nodiscard]] std::string aoa_identity_key(const AoaIdentity& identity);

// Finds the first bulk IN/OUT pair in an active (or first) configuration.
// `descriptors` is a platform-neutral view so Windows and Android tests can
// reuse the selection logic without libusb or a live device.
struct EndpointPair {
  std::uint8_t interface_number{};
  std::uint8_t endpoint_in{};
  std::uint8_t endpoint_out{};
};

struct EndpointDescriptorView {
  std::uint8_t interface_number{};
  std::uint8_t attributes{};
  std::uint8_t address{};
};

[[nodiscard]] bool select_bulk_endpoints(
    std::span<const EndpointDescriptorView> endpoints, EndpointPair& out);

// AOA control requests (SEND_STRING 52) are vendor/device, null-terminated UTF-8 strings.
[[nodiscard]] std::vector<std::uint8_t> encode_aoa_string(std::string_view text);

}  // namespace deskmate::platform
