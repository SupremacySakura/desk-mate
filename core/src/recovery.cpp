#include "deskmate/recovery.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace deskmate::protocol {
namespace {

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

}  // namespace

DeviceRecoveryGate::DeviceRecoveryGate(MonotonicClock& clock,
                                       DeviceRecoveryConfig config)
    : clock_(clock), config_(config) {}

bool DeviceRecoveryGate::begin_recovery() {
  if (gated_) {
    return false;
  }
  gated_ = true;
  ++last_recovery_id_;
  if (last_recovery_id_ == 0) {
    ++last_recovery_id_;
  }
  recovery_id_ = last_recovery_id_;
  next_retry_ns_ = saturating_add(clock_.now_ns(), config_.retry_ns);
  return true;
}

RecoveryFrameAction DeviceRecoveryGate::observe_frame(
    std::uint16_t flags, std::optional<std::uint32_t> recovery_id) {
  if (!gated_) {
    return RecoveryFrameAction::Deliver;
  }
  if ((flags & kFlagKeyFrame) == 0 || !recovery_id.has_value() ||
      !recovery_id_.has_value() || *recovery_id != *recovery_id_) {
    return RecoveryFrameAction::Drop;
  }
  gated_ = false;
  next_retry_ns_.reset();
  return RecoveryFrameAction::Deliver;
}

std::optional<std::uint32_t> DeviceRecoveryGate::retry_if_due() {
  if (!gated_ || !recovery_id_.has_value() || !next_retry_ns_.has_value() ||
      clock_.now_ns() < *next_retry_ns_) {
    return std::nullopt;
  }
  next_retry_ns_ = saturating_add(clock_.now_ns(), config_.retry_ns);
  return recovery_id_;
}

void DeviceRecoveryGate::accept_unsolicited_recovery(
    std::uint32_t recovery_id) noexcept {
  gated_ = false;
  last_recovery_id_ = std::max(last_recovery_id_, recovery_id);
  recovery_id_.reset();
  next_retry_ns_.reset();
}

void DeviceRecoveryGate::reset() noexcept {
  gated_ = false;
  recovery_id_.reset();
  next_retry_ns_.reset();
}

std::vector<std::uint8_t> encode_recovery_id(std::uint32_t recovery_id) {
  std::vector<std::uint8_t> bytes(4);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] =
        static_cast<std::uint8_t>((recovery_id >> (index * 8U)) & 0xFFU);
  }
  return bytes;
}

Result<std::uint32_t> decode_recovery_id(
    std::span<const std::uint8_t> bytes) {
  if (bytes.size() != 4) {
    return Result<std::uint32_t>::failure(ProtocolError{
        ErrorCode::InvalidControlPayload, ErrorAction::DiscardMessage,
        "recovery ID body must be exactly four bytes"});
  }
  std::uint32_t recovery_id = 0;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    recovery_id |= static_cast<std::uint32_t>(bytes[index]) << (index * 8U);
  }
  if (recovery_id == 0) {
    return Result<std::uint32_t>::failure(ProtocolError{
        ErrorCode::InvalidControlPayload, ErrorAction::DiscardMessage,
        "recovery ID must be non-zero"});
  }
  return Result<std::uint32_t>::success(recovery_id);
}

}  // namespace deskmate::protocol
