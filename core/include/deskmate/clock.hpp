#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace deskmate::protocol {

class MonotonicClock {
 public:
  MonotonicClock() = default;
  MonotonicClock(const MonotonicClock&) = delete;
  MonotonicClock& operator=(const MonotonicClock&) = delete;
  virtual ~MonotonicClock() = default;

  [[nodiscard]] virtual std::uint64_t now_ns() const noexcept = 0;
};

class SteadyClock final : public MonotonicClock {
 public:
  [[nodiscard]] std::uint64_t now_ns() const noexcept override {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }
};

class ManualClock final : public MonotonicClock {
 public:
  explicit ManualClock(std::uint64_t start_ns = 0) : now_ns_(start_ns) {}

  [[nodiscard]] std::uint64_t now_ns() const noexcept override { return now_ns_; }
  void advance_ns(std::uint64_t delta_ns) noexcept {
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    now_ns_ = delta_ns > maximum - now_ns_ ? maximum : now_ns_ + delta_ns;
  }

 private:
  std::uint64_t now_ns_{};
};

}  // namespace deskmate::protocol
