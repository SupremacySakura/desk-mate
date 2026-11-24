#pragma once

#include "deskmate/clock.hpp"
#include "deskmate/protocol.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace deskmate::protocol {

inline constexpr std::uint64_t kRecoveryRetryNs = 150'000'000ULL;

struct DeviceRecoveryConfig {
  std::uint64_t retry_ns{kRecoveryRetryNs};
};

enum class RecoveryFrameAction {
  Deliver,
  Drop,
};

class DeviceRecoveryGate {
 public:
  explicit DeviceRecoveryGate(MonotonicClock& clock,
                              DeviceRecoveryConfig config = {});

  [[nodiscard]] bool begin_recovery();
  [[nodiscard]] RecoveryFrameAction observe_frame(
      std::uint16_t flags, std::optional<std::uint32_t> recovery_id);
  [[nodiscard]] std::optional<std::uint32_t> retry_if_due();
  void accept_unsolicited_recovery(std::uint32_t recovery_id) noexcept;
  void reset() noexcept;

  [[nodiscard]] bool gated() const noexcept { return gated_; }
  [[nodiscard]] const std::optional<std::uint32_t>& recovery_id() const noexcept {
    return recovery_id_;
  }

 private:
  MonotonicClock& clock_;
  DeviceRecoveryConfig config_;
  bool gated_{};
  std::optional<std::uint32_t> recovery_id_;
  std::uint32_t last_recovery_id_{};
  std::optional<std::uint64_t> next_retry_ns_;
};

[[nodiscard]] std::vector<std::uint8_t> encode_recovery_id(
    std::uint32_t recovery_id);
[[nodiscard]] Result<std::uint32_t> decode_recovery_id(
    std::span<const std::uint8_t> bytes);

}  // namespace deskmate::protocol
