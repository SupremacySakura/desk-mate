#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gate1 {

using HandshakeClock = std::chrono::steady_clock;

enum class HandshakeWriteState {
    kCompleted,
    kTimedOut,
    kFailed,
};

struct HandshakeWriteAttempt {
    HandshakeWriteState state = HandshakeWriteState::kFailed;
    std::size_t transferred = 0;
    std::string error;
};

enum class HandshakeSendState {
    kIncomplete,
    kComplete,
    kFatal,
};

struct HandshakeSendOutcome {
    HandshakeSendState state = HandshakeSendState::kIncomplete;
    std::uint64_t bytes_transferred = 0;
    std::string error;
};

using HandshakeWriteFn = std::function<HandshakeWriteAttempt(
    const std::uint8_t* data,
    std::size_t size,
    unsigned timeout_ms)>;

bool handshake_initial_delay_elapsed(
    HandshakeClock::time_point delay_started,
    HandshakeClock::time_point now,
    std::chrono::milliseconds delay);

class HandshakeHelloSender {
public:
    explicit HandshakeHelloSender(std::vector<std::uint8_t> frame);

    HandshakeSendOutcome attempt(
        HandshakeClock::time_point now,
        HandshakeClock::time_point deadline,
        std::chrono::milliseconds max_transfer_timeout,
        std::chrono::milliseconds retry_interval,
        const HandshakeWriteFn& write);

    bool complete() const;
    std::size_t attempts() const;
    std::uint64_t bytes_transferred() const;
    HandshakeClock::time_point next_attempt() const;

private:
    std::vector<std::uint8_t> frame_;
    std::size_t offset_ = 0;
    std::size_t attempts_ = 0;
    std::uint64_t bytes_transferred_ = 0;
    HandshakeClock::time_point next_attempt_{};
};

} // namespace gate1
