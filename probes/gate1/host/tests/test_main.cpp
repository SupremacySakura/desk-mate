#include <cstdlib>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "gate1/cleanup.hpp"
#include "gate1/handshake.hpp"
#include "gate1/protocol.hpp"
#include "gate1/transfer_set.hpp"
#include "probe_status.hpp"

namespace {

int failures = 0;

void expect(bool condition, const char* expression, const char* test, int line)
{
    if (condition) {
        return;
    }
    std::cerr << "FAIL " << test << ":" << line << ": " << expression << "\n";
    ++failures;
}

#define EXPECT_TRUE(expr) expect((expr), #expr, __func__, __LINE__)
#define EXPECT_EQ(lhs, rhs) expect((lhs) == (rhs), #lhs " == " #rhs, __func__, __LINE__)

void decoder_waits_for_a_frame_split_across_chunks()
{
    const std::vector<std::uint8_t> encoded =
        gate1::encode_frame(gate1::FrameType::kData, 0, 7, {0x10, 0x20, 0x30});
    gate1::FrameDecoder decoder;
    std::vector<gate1::Frame> frames;
    std::string error;

    EXPECT_TRUE(decoder.push(encoded.data(), 5, frames, error));
    EXPECT_TRUE(frames.empty());
    EXPECT_TRUE(decoder.push(encoded.data() + 5, encoded.size() - 5, frames, error));

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(frames.size(), std::size_t{1});
    if (frames.size() == 1) {
        EXPECT_EQ(frames[0].type, gate1::FrameType::kData);
        EXPECT_EQ(frames[0].seq, std::uint32_t{7});
        EXPECT_TRUE(frames[0].payload == std::vector<std::uint8_t>({0x10, 0x20, 0x30}));
    }
}

void decoder_reassembles_coalesced_frames_and_records_zero_length_transfers()
{
    const std::vector<std::uint8_t> first =
        gate1::encode_frame(gate1::FrameType::kHeartbeat, 0, 11, {});
    const std::vector<std::uint8_t> second =
        gate1::encode_frame(gate1::FrameType::kDataEcho, 3, 12, {0xAA});
    std::vector<std::uint8_t> combined = first;
    combined.insert(combined.end(), second.begin(), second.begin() + 9);
    const std::vector<std::uint8_t> tail(second.begin() + 9, second.end());

    gate1::FrameDecoder decoder;
    std::vector<gate1::Frame> frames;
    std::string error;

    EXPECT_TRUE(decoder.push(combined.data(), combined.size(), frames, error));
    EXPECT_EQ(frames.size(), std::size_t{1});
    EXPECT_TRUE(decoder.push(nullptr, 0, frames, error));
    EXPECT_TRUE(decoder.push(tail.data(), tail.size(), frames, error));

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(decoder.zero_length_chunks(), std::size_t{1});
    EXPECT_EQ(frames.size(), std::size_t{2});
    if (frames.size() == 2) {
        EXPECT_EQ(frames[1].type, gate1::FrameType::kDataEcho);
        EXPECT_EQ(frames[1].flags, std::uint16_t{3});
        EXPECT_TRUE(frames[1].payload == std::vector<std::uint8_t>({0xAA}));
    }
}

void transfer_set_releases_only_after_both_cancelled_callbacks_arrive()
{
    gate1::TransferSet transfers;
    const std::uint64_t in_id = transfers.add(gate1::Direction::kIn);
    const std::uint64_t out_id = transfers.add(gate1::Direction::kOut);

    EXPECT_EQ(transfers.active(), std::size_t{2});
    EXPECT_TRUE(transfers.mark_cancel_requested(gate1::Direction::kIn));
    EXPECT_TRUE(transfers.mark_cancel_requested(gate1::Direction::kOut));
    EXPECT_EQ(transfers.active(), std::size_t{2});
    EXPECT_TRUE(!transfers.ready_to_release());

    transfers.complete(in_id);
    EXPECT_EQ(transfers.active(), std::size_t{1});
    EXPECT_TRUE(!transfers.ready_to_release());

    transfers.complete(out_id);
    EXPECT_EQ(transfers.active(), std::size_t{0});
    EXPECT_TRUE(transfers.ready_to_release());
    EXPECT_TRUE(transfers.cancel_observed(gate1::Direction::kIn));
    EXPECT_TRUE(transfers.cancel_observed(gate1::Direction::kOut));
}

void deterministic_payload_has_a_stable_checksum()
{
    const std::vector<std::uint8_t> payload = gate1::make_data_payload(7, 6);
    EXPECT_TRUE(payload == std::vector<std::uint8_t>({0xD9, 0xC8, 0xFB, 0xEA, 0x9D, 0x8C}));
    EXPECT_EQ(gate1::fnv1a64(payload.data(), payload.size()), std::uint64_t{0x6a3511fc2239713cULL});
}

void resource_release_requires_all_transfers_to_finish()
{
    const gate1::ResourceRelease safe = gate1::resource_release(true, true);
    EXPECT_TRUE(safe.close_handle);
    EXPECT_TRUE(safe.exit_context);

    const gate1::ResourceRelease unsafe = gate1::resource_release(true, false);
    EXPECT_TRUE(!unsafe.close_handle);
    EXPECT_TRUE(!unsafe.exit_context);

    const gate1::ResourceRelease no_handle = gate1::resource_release(false, false);
    EXPECT_TRUE(no_handle.close_handle);
    EXPECT_TRUE(no_handle.exit_context);
}

void cleanup_outcome_is_recorded_before_json_failure_can_return()
{
    std::ifstream source(GATE1_MAIN_SOURCE_PATH);
    std::ostringstream contents;
    contents << source.rdbuf();
    const std::string text = contents.str();
    const std::size_t cleanup = text.find(
        "outcome.cleanup_safe = result.cleanup_safe;");
    const std::size_t json_write = text.find("if (!write_json(", cleanup);

    EXPECT_TRUE(cleanup != std::string::npos);
    EXPECT_TRUE(json_write != std::string::npos);
    EXPECT_TRUE(cleanup < json_write);
    EXPECT_TRUE(text.find("gate1::resource_release(") != std::string::npos);
}

void handshake_hello_retries_out_timeouts_until_deadline()
{
    using Clock = gate1::HandshakeClock;
    const Clock::time_point start{};
    const Clock::time_point deadline = start + std::chrono::seconds(20);
    Clock::time_point now = start;
    gate1::HandshakeHelloSender sender({0x44, 0x4D, 0x47, 0x31});

    int attempts = 0;
    std::vector<std::size_t> attempted_sizes;
    std::vector<unsigned> attempted_timeouts;
    const auto write = [&](const std::uint8_t*, std::size_t size, unsigned timeout_ms) {
        ++attempts;
        attempted_sizes.push_back(size);
        attempted_timeouts.push_back(timeout_ms);
        now += std::chrono::seconds(3);
        if (attempts < 3) {
            return gate1::HandshakeWriteAttempt{
                gate1::HandshakeWriteState::kTimedOut, 0, {}};
        }
        return gate1::HandshakeWriteAttempt{
            gate1::HandshakeWriteState::kCompleted, size, {}};
    };

    gate1::HandshakeSendOutcome outcome;
    while (!sender.complete()) {
        outcome = sender.attempt(
            now,
            deadline,
            std::chrono::milliseconds(3000),
            std::chrono::milliseconds(1000),
            write);
        if (outcome.state == gate1::HandshakeSendState::kFatal) {
            break;
        }
        if (!sender.complete()) {
            now = sender.next_attempt();
        }
    }

    EXPECT_TRUE(outcome.state == gate1::HandshakeSendState::kComplete);
    EXPECT_TRUE(sender.complete());
    EXPECT_EQ(attempts, 3);
    EXPECT_EQ(sender.bytes_transferred(), std::uint64_t{4});
    EXPECT_TRUE(attempted_sizes == std::vector<std::size_t>({4, 4, 4}));
    EXPECT_TRUE(
        attempted_timeouts == std::vector<unsigned>({3000, 3000, 3000}));
}

void handshake_hello_retries_only_the_unwritten_suffix_after_timeout()
{
    using Clock = gate1::HandshakeClock;
    const Clock::time_point start{};
    const Clock::time_point deadline = start + std::chrono::seconds(20);
    gate1::HandshakeHelloSender sender({0x10, 0x20, 0x30, 0x40});
    Clock::time_point now = start;

    int attempts = 0;
    const auto write = [&](const std::uint8_t* data, std::size_t size, unsigned) {
        ++attempts;
        if (attempts == 1) {
            EXPECT_EQ(size, std::size_t{4});
            EXPECT_EQ(data[0], std::uint8_t{0x10});
            now += std::chrono::seconds(3);
            return gate1::HandshakeWriteAttempt{
                gate1::HandshakeWriteState::kTimedOut, 1, {}};
        }
        EXPECT_EQ(size, std::size_t{3});
        EXPECT_EQ(data[0], std::uint8_t{0x20});
        return gate1::HandshakeWriteAttempt{
            gate1::HandshakeWriteState::kCompleted, size, {}};
    };

    const gate1::HandshakeSendOutcome first = sender.attempt(
        now,
        deadline,
        std::chrono::milliseconds(3000),
        std::chrono::milliseconds(1000),
        write);
    EXPECT_TRUE(first.state == gate1::HandshakeSendState::kIncomplete);
    EXPECT_EQ(first.bytes_transferred, std::uint64_t{1});
    EXPECT_TRUE(sender.next_attempt() == now + std::chrono::milliseconds(1000));

    now = sender.next_attempt();
    const gate1::HandshakeSendOutcome second = sender.attempt(
        now,
        deadline,
        std::chrono::milliseconds(3000),
        std::chrono::milliseconds(1000),
        write);
    EXPECT_TRUE(second.state == gate1::HandshakeSendState::kComplete);
    EXPECT_EQ(second.bytes_transferred, std::uint64_t{3});
    EXPECT_EQ(sender.bytes_transferred(), std::uint64_t{4});
}

void handshake_hello_fails_at_the_handshake_deadline()
{
    using Clock = gate1::HandshakeClock;
    const Clock::time_point start{};
    const Clock::time_point deadline = start + std::chrono::seconds(2);
    Clock::time_point now = start;
    gate1::HandshakeHelloSender sender({0x44, 0x4D, 0x47, 0x31});

    const auto write = [&](const std::uint8_t*, std::size_t, unsigned timeout_ms) {
        EXPECT_EQ(timeout_ms, 2000U);
        now += std::chrono::milliseconds(timeout_ms);
        return gate1::HandshakeWriteAttempt{
            gate1::HandshakeWriteState::kTimedOut, 0, {}};
    };

    const gate1::HandshakeSendOutcome timed_out = sender.attempt(
        now,
        deadline,
        std::chrono::milliseconds(3000),
        std::chrono::milliseconds(1000),
        write);
    EXPECT_TRUE(timed_out.state == gate1::HandshakeSendState::kIncomplete);
    EXPECT_TRUE(now == deadline);

    const gate1::HandshakeSendOutcome expired = sender.attempt(
        now,
        deadline,
        std::chrono::milliseconds(3000),
        std::chrono::milliseconds(1000),
        write);
    EXPECT_TRUE(expired.state == gate1::HandshakeSendState::kFatal);
    EXPECT_TRUE(expired.error.find("deadline") != std::string::npos);
}

void handshake_hello_fails_immediately_for_non_timeout_errors()
{
    using Clock = gate1::HandshakeClock;
    const Clock::time_point start{};
    const Clock::time_point deadline = start + std::chrono::seconds(20);
    gate1::HandshakeHelloSender sender({0x44, 0x4D, 0x47, 0x31});
    int attempts = 0;
    const auto write = [&](const std::uint8_t*, std::size_t, unsigned) {
        ++attempts;
        return gate1::HandshakeWriteAttempt{
            gate1::HandshakeWriteState::kFailed,
            0,
            "LIBUSB_ERROR_NO_DEVICE"};
    };

    const gate1::HandshakeSendOutcome outcome = sender.attempt(
        start,
        deadline,
        std::chrono::milliseconds(3000),
        std::chrono::milliseconds(1000),
        write);
    EXPECT_TRUE(outcome.state == gate1::HandshakeSendState::kFatal);
    EXPECT_EQ(attempts, 1);
    EXPECT_TRUE(outcome.error.find("LIBUSB_ERROR_NO_DEVICE") != std::string::npos);
}

void device_stats_do_not_report_running_after_worker_exit()
{
    EXPECT_TRUE(gate1::probe_reports_running(true, false));
    EXPECT_TRUE(!gate1::probe_reports_running(true, true));
    EXPECT_TRUE(!gate1::probe_reports_running(false, false));
    EXPECT_TRUE(!gate1::probe_reports_running(false, true));
}

void handshake_initial_delay_keeps_the_first_hello_off_the_endpoint()
{
    using Clock = gate1::HandshakeClock;
    const Clock::time_point claimed = Clock::time_point{};
    const std::chrono::milliseconds delay(10000);

    EXPECT_TRUE(!gate1::handshake_initial_delay_elapsed(
        claimed, claimed, delay));
    EXPECT_TRUE(!gate1::handshake_initial_delay_elapsed(
        claimed, claimed + std::chrono::milliseconds(9999), delay));
    EXPECT_TRUE(gate1::handshake_initial_delay_elapsed(
        claimed, claimed + delay, delay));
    EXPECT_TRUE(gate1::handshake_initial_delay_elapsed(
        claimed, claimed, std::chrono::milliseconds(0)));
}

void device_error_diagnostic_preserves_errno_and_poll_revents()
{
    const std::string diagnostic =
        gate1::probe_error_context(5, 45, 0x10, "eventfd");
    EXPECT_TRUE(diagnostic.find("errno=5") != std::string::npos);
    EXPECT_TRUE(diagnostic.find("fd=45") != std::string::npos);
    EXPECT_TRUE(diagnostic.find("mode=eventfd") != std::string::npos);
    EXPECT_TRUE(diagnostic.find("revents=0x10") != std::string::npos);
}

} // namespace

int main()
{
    decoder_waits_for_a_frame_split_across_chunks();
    decoder_reassembles_coalesced_frames_and_records_zero_length_transfers();
    transfer_set_releases_only_after_both_cancelled_callbacks_arrive();
    deterministic_payload_has_a_stable_checksum();
    resource_release_requires_all_transfers_to_finish();
    cleanup_outcome_is_recorded_before_json_failure_can_return();
    handshake_hello_retries_out_timeouts_until_deadline();
    handshake_hello_retries_only_the_unwritten_suffix_after_timeout();
    handshake_hello_fails_at_the_handshake_deadline();
    handshake_hello_fails_immediately_for_non_timeout_errors();
    device_stats_do_not_report_running_after_worker_exit();
    handshake_initial_delay_keeps_the_first_hello_off_the_endpoint();
    device_error_diagnostic_preserves_errno_and_poll_revents();

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all tests passed\n";
    return EXIT_SUCCESS;
}
