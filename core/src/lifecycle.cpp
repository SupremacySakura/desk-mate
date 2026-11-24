#include "deskmate/session.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

namespace deskmate::protocol {
namespace {

constexpr std::uint8_t kClockEncodingVersion = 1;
constexpr std::size_t kClockPayloadSize = 28;

void write_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    bytes.push_back(
        static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU));
  }
}

std::uint64_t read_u64(std::span<const std::uint8_t> bytes,
                       std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[offset + index])
             << (index * 8U);
  }
  return value;
}

ProtocolError clock_error(std::string_view detail) {
  return ProtocolError{ErrorCode::InvalidControlPayload,
                       ErrorAction::DiscardMessage, detail};
}

std::int64_t clamp_i64(__int128 value) {
  constexpr __int128 minimum =
      static_cast<__int128>(std::numeric_limits<std::int64_t>::min());
  constexpr __int128 maximum =
      static_cast<__int128>(std::numeric_limits<std::int64_t>::max());
  if (value < minimum) {
    return std::numeric_limits<std::int64_t>::min();
  }
  if (value > maximum) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return static_cast<std::int64_t>(value);
}

}  // namespace

struct SurfaceLease::State {
  mutable std::mutex mutex;
  SurfaceReferenceOps* reference_ops{};
  std::shared_ptr<SurfaceReferenceOps> owned_reference_ops;
  std::uint64_t generation{};
  bool available{};
  std::size_t reference_count{};
};

struct SurfaceOwner::State final : SurfaceLease::State {};

SurfaceLease::SurfaceLease(std::shared_ptr<State> state,
                           std::uint64_t generation)
    : state_(std::move(state)), generation_(generation), released_(false) {}

SurfaceLease::~SurfaceLease() { release(); }

SurfaceLease::SurfaceLease(SurfaceLease&& other) noexcept
    : state_(std::move(other.state_)),
      generation_(other.generation_),
      released_(other.released_) {
  other.released_ = true;
}

SurfaceLease& SurfaceLease::operator=(SurfaceLease&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  release();
  state_ = std::move(other.state_);
  generation_ = other.generation_;
  released_ = other.released_;
  other.released_ = true;
  return *this;
}

void SurfaceLease::release() noexcept {
  if (released_ || !state_) {
    released_ = true;
    return;
  }
  {
    const std::lock_guard lock(state_->mutex);
    if (state_->reference_count != 0) {
      --state_->reference_count;
    }
    if (state_->reference_ops != nullptr) {
      state_->reference_ops->remove_reference();
    }
    released_ = true;
  }
  state_.reset();
}

bool SurfaceLease::holds_reference() const noexcept {
  if (released_ || !state_) {
    return false;
  }
  const std::lock_guard lock(state_->mutex);
  return state_->reference_count != 0;
}

bool SurfaceLease::can_render() const noexcept {
  if (released_ || !state_) {
    return false;
  }
  const std::lock_guard lock(state_->mutex);
  return state_->available && state_->generation == generation_;
}

SurfaceOwner::SurfaceOwner(SurfaceReferenceOps* reference_ops)
    : state_(std::make_shared<State>()) {
  state_->reference_ops = reference_ops;
}

SurfaceOwner::SurfaceOwner(std::shared_ptr<SurfaceReferenceOps> reference_ops)
    : state_(std::make_shared<State>()) {
  state_->owned_reference_ops = std::move(reference_ops);
  state_->reference_ops = state_->owned_reference_ops.get();
}

SurfaceOwner::~SurfaceOwner() {
  const std::lock_guard lock(state_->mutex);
  state_->available = false;
  if (state_->owned_reference_ops == nullptr) {
    if (state_->reference_ops != nullptr) {
      for (std::size_t index = 0; index < state_->reference_count; ++index) {
        state_->reference_ops->remove_reference();
      }
    }
    state_->reference_count = 0;
    state_->reference_ops = nullptr;
  }
}

std::uint64_t SurfaceOwner::attach() {
  const std::lock_guard lock(state_->mutex);
  ++state_->generation;
  if (state_->generation == 0) {
    ++state_->generation;
  }
  state_->available = true;
  return state_->generation;
}

void SurfaceOwner::invalidate() noexcept {
  const std::lock_guard lock(state_->mutex);
  state_->available = false;
}

bool SurfaceOwner::available() const noexcept {
  const std::lock_guard lock(state_->mutex);
  return state_->available;
}

std::uint64_t SurfaceOwner::generation() const noexcept {
  const std::lock_guard lock(state_->mutex);
  return state_->generation;
}

std::size_t SurfaceOwner::reference_count() const noexcept {
  const std::lock_guard lock(state_->mutex);
  return state_->reference_count;
}

std::optional<SurfaceLease> SurfaceOwner::acquire() {
  const std::lock_guard lock(state_->mutex);
  if (!state_->available) {
    return std::nullopt;
  }
  if (state_->reference_ops != nullptr) {
    state_->reference_ops->add_reference();
  }
  ++state_->reference_count;
  return SurfaceLease(state_, state_->generation);
}

bool SurfaceOwner::can_render(std::uint64_t generation) const noexcept {
  const std::lock_guard lock(state_->mutex);
  return state_->available && state_->generation == generation;
}

AccessoryFdOwner::AccessoryFdOwner(int descriptor,
                                   AccessoryFdCloser& closer) noexcept
    : descriptor_(descriptor), closer_(&closer), closed_(descriptor < 0) {}

AccessoryFdOwner::~AccessoryFdOwner() { close(); }

AccessoryFdOwner::AccessoryFdOwner(AccessoryFdOwner&& other) noexcept
    : descriptor_(other.descriptor_),
      closer_(other.closer_),
      closed_(other.closed_) {
  other.descriptor_ = -1;
  other.closer_ = nullptr;
  other.closed_ = true;
}

AccessoryFdOwner& AccessoryFdOwner::operator=(AccessoryFdOwner&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  close();
  descriptor_ = other.descriptor_;
  closer_ = other.closer_;
  closed_ = other.closed_;
  other.descriptor_ = -1;
  other.closer_ = nullptr;
  other.closed_ = true;
  return *this;
}

void AccessoryFdOwner::close() noexcept {
  if (closed_) {
    return;
  }
  closed_ = true;
  if (descriptor_ >= 0 && closer_ != nullptr) {
    closer_->close_fd(descriptor_);
  }
  descriptor_ = -1;
}

Result<std::vector<std::uint8_t>> encode_clock_message(
    const ClockMessage& message) {
  if (message.kind != ClockMessageKind::Request &&
      message.kind != ClockMessageKind::Response) {
    return Result<std::vector<std::uint8_t>>::failure(
        clock_error("clock message has an unknown kind"));
  }
  if (message.kind == ClockMessageKind::Request &&
      (message.t1_ns != 0 || message.t2_ns != 0)) {
    return Result<std::vector<std::uint8_t>>::failure(
        clock_error("clock requests must not contain device timestamps"));
  }

  std::vector<std::uint8_t> bytes;
  bytes.reserve(kClockPayloadSize);
  bytes.push_back(kClockEncodingVersion);
  bytes.push_back(static_cast<std::uint8_t>(message.kind));
  bytes.push_back(0);
  bytes.push_back(0);
  write_u64(bytes, message.t0_ns);
  write_u64(bytes, message.t1_ns);
  write_u64(bytes, message.t2_ns);
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

Result<ClockMessage> decode_clock_message(
    std::span<const std::uint8_t> bytes) {
  if (bytes.size() != kClockPayloadSize) {
    return Result<ClockMessage>::failure(
        clock_error("clock payload must be exactly 28 bytes"));
  }
  if (bytes[0] != kClockEncodingVersion || bytes[2] != 0 || bytes[3] != 0) {
    return Result<ClockMessage>::failure(
        clock_error("clock payload has an unsupported encoding"));
  }
  ClockMessage message;
  message.kind = static_cast<ClockMessageKind>(bytes[1]);
  if (message.kind != ClockMessageKind::Request &&
      message.kind != ClockMessageKind::Response) {
    return Result<ClockMessage>::failure(
        clock_error("clock payload has an unknown kind"));
  }
  message.t0_ns = read_u64(bytes, 4);
  message.t1_ns = read_u64(bytes, 12);
  message.t2_ns = read_u64(bytes, 20);
  if (message.kind == ClockMessageKind::Request &&
      (message.t1_ns != 0 || message.t2_ns != 0)) {
    return Result<ClockMessage>::failure(
        clock_error("clock request contains device timestamps"));
  }
  return Result<ClockMessage>::success(message);
}

bool ClockSyncEstimator::observe(const ClockSample& sample) {
  if (sample.t3_host_ns < sample.t0_host_ns ||
      sample.t2_device_ns < sample.t1_device_ns) {
    return false;
  }
  const std::uint64_t host_elapsed = sample.t3_host_ns - sample.t0_host_ns;
  const std::uint64_t device_elapsed =
      sample.t2_device_ns - sample.t1_device_ns;
  if (device_elapsed > host_elapsed) {
    return false;
  }
  const std::uint64_t rtt_ns = host_elapsed - device_elapsed;

  const __int128 forward =
      static_cast<__int128>(sample.t1_device_ns) - sample.t0_host_ns;
  const __int128 reverse =
      static_cast<__int128>(sample.t2_device_ns) - sample.t3_host_ns;
  const std::int64_t offset_ns = clamp_i64((forward + reverse) / 2);
  const std::uint64_t host_midpoint_ns =
      sample.t0_host_ns + host_elapsed / 2;

  std::int64_t drift_ppb = 0;
  if (first_host_midpoint_ns_.has_value() && first_offset_ns_.has_value() &&
      host_midpoint_ns > *first_host_midpoint_ns_) {
    const __int128 elapsed_host =
        static_cast<__int128>(host_midpoint_ns) - *first_host_midpoint_ns_;
    const __int128 offset_delta =
        static_cast<__int128>(offset_ns) - *first_offset_ns_;
    drift_ppb = clamp_i64((offset_delta * 1'000'000'000) / elapsed_host);
  } else if (!first_host_midpoint_ns_.has_value()) {
    first_host_midpoint_ns_ = host_midpoint_ns;
    first_offset_ns_ = offset_ns;
  }

  ++sample_count_;
  estimate_ = ClockEstimate{offset_ns, rtt_ns, drift_ppb, rtt_ns / 2,
                            sample_count_};
  return true;
}

}  // namespace deskmate::protocol
