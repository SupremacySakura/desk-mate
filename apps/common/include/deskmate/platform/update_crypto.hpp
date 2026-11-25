#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace deskmate::platform {

class Sha256Hasher {
 public:
  Sha256Hasher() noexcept;

  void update(std::span<const std::uint8_t> data) noexcept;
  [[nodiscard]] std::array<std::uint8_t, 32> finalize() noexcept;

 private:
  void process_block(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::uint64_t total_bytes_{0};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffer_used_{0};
};

[[nodiscard]] std::array<std::uint8_t, 32> sha256(
    std::span<const std::uint8_t> data) noexcept;

[[nodiscard]] bool sha256_file(const std::string& path,
                               std::array<std::uint8_t, 32>& out_hash,
                               std::uint64_t& out_size);

[[nodiscard]] std::array<std::uint8_t, 64> sha512(
    std::span<const std::uint8_t> data) noexcept;

[[nodiscard]] bool ed25519_verify(
    std::span<const std::uint8_t, 32> pubkey,
    std::span<const std::uint8_t> message,
    std::span<const std::uint8_t, 64> signature) noexcept;

[[nodiscard]] std::string hex_encode(std::span<const std::uint8_t> bytes);

[[nodiscard]] bool hex_decode(std::string_view hex,
                              std::vector<std::uint8_t>& out);

}  // namespace deskmate::platform
