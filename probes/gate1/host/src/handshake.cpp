#include "gate1/handshake.hpp"

#include <algorithm>
#include <utility>

namespace gate1 {

bool handshake_initial_delay_elapsed(
    HandshakeClock::time_point delay_started,
    HandshakeClock::time_point now,
    std::chrono::milliseconds delay)
{
    return now >= delay_started + delay;
}

HandshakeHelloSender::HandshakeHelloSender(std::vector<std::uint8_t> frame)
    : frame_(std::move(frame))
{
}

HandshakeSendOutcome HandshakeHelloSender::attempt(
    HandshakeClock::time_point now,
    HandshakeClock::time_point deadline,
    std::chrono::milliseconds max_transfer_timeout,
    std::chrono::milliseconds retry_interval,
    const HandshakeWriteFn& write)
{
    HandshakeSendOutcome outcome;
    if (complete()) {
        outcome.state = HandshakeSendState::kComplete;
        return outcome;
    }
    if (now >= deadline) {
        outcome.state = HandshakeSendState::kFatal;
        outcome.error = "handshake OUT deadline expired";
        return outcome;
    }
    if (max_transfer_timeout.count() <= 0) {
        outcome.state = HandshakeSendState::kFatal;
        outcome.error = "handshake OUT transfer timeout must be positive";
        return outcome;
    }

    const auto remaining_deadline_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const auto timeout_ms = std::max<std::int64_t>(
        1,
        std::min<std::int64_t>(
            max_transfer_timeout.count(),
            remaining_deadline_ms.count()));

    ++attempts_;
    const std::size_t remaining = frame_.size() - offset_;
    const HandshakeWriteAttempt written = write(
        frame_.data() + offset_,
        remaining,
        static_cast<unsigned>(timeout_ms));

    if (written.transferred > remaining) {
        outcome.state = HandshakeSendState::kFatal;
        outcome.error = "handshake OUT returned an invalid transferred length";
        return outcome;
    }

    outcome.bytes_transferred = written.transferred;
    offset_ += written.transferred;
    bytes_transferred_ += written.transferred;

    if (written.state == HandshakeWriteState::kFailed) {
        outcome.state = HandshakeSendState::kFatal;
        outcome.error =
            written.error.empty() ? "handshake OUT failed" : written.error;
        return outcome;
    }
    if (offset_ == frame_.size()) {
        outcome.state = HandshakeSendState::kComplete;
        return outcome;
    }
    if (written.state == HandshakeWriteState::kTimedOut) {
        next_attempt_ =
            now + std::chrono::milliseconds(timeout_ms) + retry_interval;
        outcome.state = HandshakeSendState::kIncomplete;
        return outcome;
    }
    if (written.transferred == 0) {
        outcome.state = HandshakeSendState::kFatal;
        outcome.error = "handshake OUT made no progress";
        return outcome;
    }

    next_attempt_ = now;
    outcome.state = HandshakeSendState::kIncomplete;
    return outcome;
}

bool HandshakeHelloSender::complete() const
{
    return offset_ == frame_.size();
}

std::size_t HandshakeHelloSender::attempts() const
{
    return attempts_;
}

std::uint64_t HandshakeHelloSender::bytes_transferred() const
{
    return bytes_transferred_;
}

HandshakeClock::time_point HandshakeHelloSender::next_attempt() const
{
    return next_attempt_;
}

} // namespace gate1
