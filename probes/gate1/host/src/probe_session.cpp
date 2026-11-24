#include "gate1/probe_session.hpp"

#include "gate1/handshake.hpp"
#include "gate1/protocol.hpp"
#include "gate1/transfer_set.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <ostream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace gate1 {
namespace {

using Clock = std::chrono::steady_clock;

constexpr unsigned kHandshakeTransferTimeoutMs = 3000;
constexpr std::size_t kInTransferBytes = 65536;
constexpr std::size_t kMaxOutTransferBytes = 65536;

const char* transfer_status_name(libusb_transfer_status status)
{
    switch (status) {
        case LIBUSB_TRANSFER_COMPLETED:
            return "COMPLETED";
        case LIBUSB_TRANSFER_ERROR:
            return "ERROR";
        case LIBUSB_TRANSFER_TIMED_OUT:
            return "TIMED_OUT";
        case LIBUSB_TRANSFER_CANCELLED:
            return "CANCELLED";
        case LIBUSB_TRANSFER_STALL:
            return "STALL";
        case LIBUSB_TRANSFER_NO_DEVICE:
            return "NO_DEVICE";
        case LIBUSB_TRANSFER_OVERFLOW:
            return "OVERFLOW";
        default:
            return "UNKNOWN";
    }
}

std::uint64_t elapsed_us(Clock::time_point start, Clock::time_point end)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

std::uint64_t read_u64_le(const std::uint8_t* data)
{
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | data[i];
    }
    return value;
}

class DuplexProbe {
public:
    DuplexProbe(
        libusb_context* context,
        libusb_device_handle* handle,
        int interface_number,
        std::uint8_t endpoint_in,
        std::uint8_t endpoint_out,
        const ProbeOptions& options,
        std::ostream& log)
        : context_(context),
          handle_(handle),
          interface_number_(interface_number),
          endpoint_in_(endpoint_in),
          endpoint_out_(endpoint_out),
          options_(options),
          log_(log),
          in_buffer_(kInTransferBytes)
    {
        out_capacity_ = static_cast<std::size_t>(std::max(0, options_.out_transfer_bytes));
        out_capacity_ = std::min(out_capacity_, kMaxOutTransferBytes);
        out_buffer_.reserve(out_capacity_ + kFrameHeaderSize);
    }

    ProbeResult run()
    {
        if (!validate_options()) {
            finish(Clock::now());
            return result_;
        }
        if (!claim_interface()) {
            finish(Clock::now());
            return result_;
        }
        if (!handshake()) {
            release_interface();
            finish(Clock::now());
            return result_;
        }
        if (!start_transfers()) {
            wait_until_transfers_zero();
            if (result_.cleanup_safe) {
                release_interface();
            }
            finish(Clock::now());
            return result_;
        }

        const Clock::time_point started = Clock::now();
        const Clock::time_point deadline =
            started + std::chrono::seconds(options_.run_seconds);
        Clock::time_point next_progress = started + std::chrono::seconds(1);

        while (Clock::now() < deadline && !fatal_ && !stopping_) {
            if (!pump_events(50)) {
                break;
            }
            const Clock::time_point now = Clock::now();
            if (now >= next_progress) {
                print_progress(started, now);
                next_progress += std::chrono::seconds(1);
            }
            watch_heartbeat(now);
        }

        begin_cancellation();
        wait_until_transfers_zero();
        if (result_.cleanup_safe) {
            release_interface();
        }
        finish(Clock::now());
        return result_;
    }

private:
    enum class OutKind {
        kNone,
        kData,
        kHeartbeat,
        kStatsRequest,
        kZeroLength,
    };

    bool validate_options()
    {
        if (options_.run_seconds <= 0) {
            fail("run duration must be positive");
        } else if (out_capacity_ < 512) {
            fail("out transfer size must be at least 512 bytes");
        } else if (options_.heartbeat_ms <= 0) {
            fail("heartbeat interval must be positive");
        } else if (options_.cancel_timeout_ms <= 0) {
            fail("cancel timeout must be positive");
        } else if (options_.handshake_initial_delay_ms < 0) {
            fail("handshake initial delay must not be negative");
        } else if (options_.handshake_timeout_ms <= 0) {
            fail("handshake timeout must be positive");
        } else if (options_.handshake_retry_ms <= 0) {
            fail("handshake retry interval must be positive");
        }
        return error_.empty();
    }

    bool claim_interface()
    {
        const int rc = libusb_claim_interface(handle_, interface_number_);
        if (rc != LIBUSB_SUCCESS) {
            fail(std::string("libusb_claim_interface failed: ") + libusb_error_name(rc));
            return false;
        }
        claimed_ = true;
        log_ << "[open] claimed interface " << interface_number_
             << ", bulk IN=0x" << std::hex << static_cast<int>(endpoint_in_)
             << " OUT=0x" << static_cast<int>(endpoint_out_) << std::dec << "\n";
        return true;
    }

    void release_interface()
    {
        if (!claimed_) {
            return;
        }
        const int rc = libusb_release_interface(handle_, interface_number_);
        if (rc != LIBUSB_SUCCESS) {
            result_.notes.push_back(
                std::string("libusb_release_interface failed: ") + libusb_error_name(rc));
        }
        claimed_ = false;
    }

    HandshakeWriteAttempt write_hello_once(
        const std::uint8_t* data,
        std::size_t size,
        unsigned timeout_ms)
    {
        int transferred = 0;
        unsigned char* pointer = const_cast<unsigned char*>(data);
        const int rc = libusb_bulk_transfer(
            handle_,
            endpoint_out_,
            pointer,
            static_cast<int>(size),
            &transferred,
            timeout_ms);
        if (transferred < 0 ||
            static_cast<std::size_t>(transferred) > size) {
            return HandshakeWriteAttempt{
                HandshakeWriteState::kFailed,
                0,
                "handshake OUT returned an invalid transferred length"};
        }
        result_.out_bytes += static_cast<std::uint64_t>(transferred);
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            return HandshakeWriteAttempt{
                HandshakeWriteState::kTimedOut,
                static_cast<std::size_t>(transferred),
                {}};
        }
        if (rc != LIBUSB_SUCCESS) {
            return HandshakeWriteAttempt{
                HandshakeWriteState::kFailed,
                static_cast<std::size_t>(transferred),
                libusb_error_name(rc)};
        }
        return HandshakeWriteAttempt{
            HandshakeWriteState::kCompleted,
            static_cast<std::size_t>(transferred),
            {}};
    }

    bool handshake()
    {
        const Clock::time_point claimed_at = Clock::now();
        if (options_.handshake_initial_delay_ms > 0) {
            log_ << "[handshake] waiting "
                 << options_.handshake_initial_delay_ms
                 << " ms after claim before first HELLO; open Accessory and "
                    "start the Device worker now\n";
            while (!handshake_initial_delay_elapsed(
                claimed_at,
                Clock::now(),
                std::chrono::milliseconds(
                    options_.handshake_initial_delay_ms))) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            log_ << "[handshake] initial delay elapsed; starting HELLO\n";
        }
        const Clock::time_point deadline =
            Clock::now() + std::chrono::milliseconds(options_.handshake_timeout_ms);
        Clock::time_point next_hello = Clock::now();
        std::optional<HandshakeHelloSender> hello_sender;
        std::size_t hello_frames_sent = 0;
        std::vector<Frame> frames;
        while (Clock::now() < deadline) {
            const Clock::time_point now = Clock::now();
            if (!hello_sender.has_value() ||
                (hello_sender->complete() && now >= next_hello)) {
                const std::vector<std::uint8_t> hello = encode_frame(
                    FrameType::kHello,
                    1,
                    next_seq(),
                    {0x44, 0x4D, 0x47, 0x31});
                if (hello.empty()) {
                    fail("handshake HELLO encoding failed");
                    return false;
                }
                hello_sender.emplace(hello);
            }

            if (!hello_sender->complete() &&
                now >= hello_sender->next_attempt()) {
                const HandshakeSendOutcome outcome = hello_sender->attempt(
                    now,
                    deadline,
                    std::chrono::milliseconds(kHandshakeTransferTimeoutMs),
                    std::chrono::milliseconds(options_.handshake_retry_ms),
                    [this](
                        const std::uint8_t* data,
                        std::size_t size,
                        unsigned timeout_ms) {
                        return write_hello_once(data, size, timeout_ms);
                    });
                if (outcome.state == HandshakeSendState::kFatal) {
                    fail("handshake OUT failed: " + outcome.error);
                    return false;
                }
                if (outcome.state == HandshakeSendState::kComplete) {
                    ++hello_frames_sent;
                    next_hello = now + std::chrono::milliseconds(
                        options_.handshake_retry_ms);
                }
                if (hello_frames_sent == 1 &&
                    outcome.state == HandshakeSendState::kComplete) {
                    log_ << "[handshake] HELLO sent; waiting up to "
                         << options_.handshake_timeout_ms
                         << " ms for Device authorization/native start\n";
                } else if (outcome.state == HandshakeSendState::kComplete) {
                    log_ << "[handshake] HELLO retry " << hello_frames_sent
                         << "\n";
                } else if (outcome.bytes_transferred > 0) {
                    log_ << "[handshake] HELLO OUT timed out after "
                         << outcome.bytes_transferred
                         << " byte(s); retrying remaining bytes\n";
                } else {
                    log_ << "[handshake] HELLO OUT timed out; retrying\n";
                }
            }

            int transferred = 0;
            const Clock::time_point hello_wake =
                hello_sender->complete()
                    ? next_hello
                    : hello_sender->next_attempt();
            const Clock::time_point wake_at =
                std::min(hello_wake, deadline);
            const auto remaining_ms = std::max<long long>(
                0,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    wake_at - Clock::now()).count());
            const unsigned timeout = static_cast<unsigned>(
                std::clamp<long long>(remaining_ms, 1, 100));
            const int rc = libusb_bulk_transfer(
                handle_,
                endpoint_in_,
                in_buffer_.data(),
                static_cast<int>(in_buffer_.size()),
                &transferred,
                timeout);
            if (rc == LIBUSB_ERROR_TIMEOUT) {
                continue;
            }
            if (rc != LIBUSB_SUCCESS) {
                fail(std::string("handshake IN failed: ") + libusb_error_name(rc));
                return false;
            }
            result_.in_bytes += static_cast<std::uint64_t>(transferred);
            last_inbound_ = Clock::now();
            if (transferred == 0) {
                ++result_.zero_length_in_completed;
                continue;
            }
            std::string decode_error;
            if (!decoder_.push(
                    in_buffer_.data(),
                    static_cast<std::size_t>(transferred),
                    frames,
                    decode_error)) {
                fail("handshake frame decode failed: " + decode_error);
                return false;
            }
            for (const Frame& frame : frames) {
                if (frame.type == FrameType::kHelloAck) {
                    result_.handshake_ok = true;
                    log_ << "[handshake] HELLO_ACK seq=" << frame.seq
                         << " protocol=" << static_cast<int>(kProtocolVersion) << "\n";
                    return true;
                }
                if (frame.type == FrameType::kError) {
                    fail("device returned ERROR during handshake");
                    return false;
                }
            }
            frames.clear();
        }

        fail(
            "handshake timed out waiting for HELLO_ACK after " +
            std::to_string(options_.handshake_timeout_ms) +
            " ms (authorize/open on Device and start the native worker)");
        return false;
    }

    bool start_transfers()
    {
        in_transfer_ = libusb_alloc_transfer(0);
        out_transfer_ = libusb_alloc_transfer(0);
        if (in_transfer_ == nullptr || out_transfer_ == nullptr) {
            fail("libusb_alloc_transfer failed");
            return false;
        }

        in_id_ = transfers_.add(Direction::kIn);
        out_id_ = transfers_.add(Direction::kOut);
        if (!submit_out()) {
            transfers_.complete(out_id_);
            transfers_.complete(in_id_);
            return false;
        }
        if (!submit_in()) {
            stopping_ = true;
            transfers_.complete(in_id_);
            in_done_ = true;
            transfers_.mark_cancel_requested(Direction::kOut);
            cancel_started_ = Clock::now();
            out_cancel_rc_ = libusb_cancel_transfer(out_transfer_);
            return false;
        }
        last_heartbeat_sent_ = Clock::now();
        last_stats_request_ = Clock::now();
        return true;
    }

    bool submit_in()
    {
        libusb_fill_bulk_transfer(
            in_transfer_,
            handle_,
            endpoint_in_,
            in_buffer_.data(),
            static_cast<int>(in_buffer_.size()),
            &DuplexProbe::in_callback,
            this,
            0);
        const int rc = libusb_submit_transfer(in_transfer_);
        if (rc != LIBUSB_SUCCESS) {
            fail(std::string("submit bulk IN failed: ") + libusb_error_name(rc));
            in_active_ = false;
            return false;
        }
        in_active_ = true;
        return true;
    }

    bool submit_out()
    {
        if (!prepare_out_chunk()) {
            return false;
        }
        libusb_fill_bulk_transfer(
            out_transfer_,
            handle_,
            endpoint_out_,
            out_buffer_.data(),
            static_cast<int>(out_transfer_length_),
            &DuplexProbe::out_callback,
            this,
            0);
        const int rc = libusb_submit_transfer(out_transfer_);
        if (rc != LIBUSB_SUCCESS) {
            fail(std::string("submit bulk OUT failed: ") + libusb_error_name(rc));
            out_active_ = false;
            return false;
        }
        out_active_ = true;
        return true;
    }

    bool prepare_out_chunk()
    {
        const Clock::time_point now = Clock::now();
        if (now - last_heartbeat_sent_ >=
            std::chrono::milliseconds(options_.heartbeat_ms)) {
            const std::uint32_t seq = next_seq();
            const std::vector<std::uint8_t> encoded =
                encode_frame(FrameType::kHeartbeat, 0, seq, {});
            out_buffer_.assign(encoded.begin(), encoded.end());
            out_transfer_length_ = encoded.size();
            out_kind_ = OutKind::kHeartbeat;
            heartbeat_sent_at_[seq] = now;
            ++result_.heartbeats_sent;
            last_heartbeat_sent_ = now;
            return true;
        }

        if (options_.stats_ms > 0 &&
            now - last_stats_request_ >=
                std::chrono::milliseconds(options_.stats_ms)) {
            const std::vector<std::uint8_t> encoded =
                encode_frame(FrameType::kStatsRequest, 0, next_seq(), {});
            out_buffer_.assign(encoded.begin(), encoded.end());
            out_transfer_length_ = encoded.size();
            out_kind_ = OutKind::kStatsRequest;
            last_stats_request_ = now;
            return true;
        }

        if (options_.zlp_every_frames > 0 &&
            frames_since_zlp_ >=
                static_cast<std::uint64_t>(options_.zlp_every_frames)) {
            frames_since_zlp_ = 0;
            out_transfer_length_ = 0;
            out_kind_ = OutKind::kZeroLength;
            ++result_.zero_length_out_scheduled;
            log_ << "[zlp] scheduled zero-length bulk OUT\n";
            return true;
        }

        static constexpr std::array<std::uint32_t, 6> kPayloadSizes = {
            1, 7, 255, 1024, 4096, 8192};
        const std::uint32_t max_payload = out_capacity_ > kFrameHeaderSize
            ? static_cast<std::uint32_t>(out_capacity_ - kFrameHeaderSize)
            : 1U;
        const std::uint32_t chosen =
            kPayloadSizes[result_.data_frames_sent % kPayloadSizes.size()];
        const std::uint32_t payload_size = std::min(chosen, max_payload);
        const std::uint32_t seq = next_seq();
        const std::vector<std::uint8_t> payload =
            make_data_payload(seq, payload_size);
        const std::vector<std::uint8_t> encoded =
            encode_frame(FrameType::kData, 0, seq, payload);
        if (encoded.empty()) {
            fail("data frame encoding failed");
            return false;
        }
        expected_echoes_[seq] = fnv1a64(payload.data(), payload.size());
        out_buffer_.assign(encoded.begin(), encoded.end());
        out_transfer_length_ = encoded.size();
        ++result_.data_frames_sent;
        ++frames_since_zlp_;
        out_kind_ = OutKind::kData;
        return true;
    }

    static void LIBUSB_CALL in_callback(libusb_transfer* transfer)
    {
        auto* self = static_cast<DuplexProbe*>(transfer->user_data);
        self->handle_in_completion(transfer);
    }

    static void LIBUSB_CALL out_callback(libusb_transfer* transfer)
    {
        auto* self = static_cast<DuplexProbe*>(transfer->user_data);
        self->handle_out_completion(transfer);
    }

    void handle_in_completion(libusb_transfer* transfer)
    {
        in_active_ = false;
        result_.in_callback_status = static_cast<int>(transfer->status);
        if (stopping_) {
            result_.in_callback_after_cancel_us =
                elapsed_us(cancel_started_, Clock::now());
        }

        const int actual_length = transfer->actual_length;
        if (actual_length > 0) {
            result_.in_bytes += static_cast<std::uint64_t>(actual_length);
            last_inbound_ = Clock::now();
            std::vector<Frame> frames;
            std::string decode_error;
            if (!decoder_.push(
                    in_buffer_.data(),
                    static_cast<std::size_t>(actual_length),
                    frames,
                    decode_error)) {
                fail("bulk IN frame decode failed: " + decode_error);
            } else {
                for (const Frame& frame : frames) {
                    process_frame(frame);
                }
            }
        } else if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
            ++result_.zero_length_in_completed;
            last_inbound_ = Clock::now();
        }

        if (transfer->status != LIBUSB_TRANSFER_COMPLETED &&
            transfer->status != LIBUSB_TRANSFER_CANCELLED) {
            fail(std::string("bulk IN completed with ") +
                 transfer_status_name(transfer->status));
        }

        if (stopping_ || fatal_) {
            transfers_.complete(in_id_);
            in_done_ = true;
            return;
        }
        if (!submit_in()) {
            transfers_.complete(in_id_);
            in_done_ = true;
        }
    }

    void handle_out_completion(libusb_transfer* transfer)
    {
        out_active_ = false;
        result_.out_callback_status = static_cast<int>(transfer->status);
        if (stopping_) {
            result_.out_callback_after_cancel_us =
                elapsed_us(cancel_started_, Clock::now());
        }

        if (transfer->actual_length > 0) {
            result_.out_bytes += static_cast<std::uint64_t>(transfer->actual_length);
        }
        if (out_kind_ == OutKind::kZeroLength &&
            transfer->status == LIBUSB_TRANSFER_COMPLETED &&
            transfer->actual_length == 0) {
            ++result_.zero_length_out_completed;
        }

        if (transfer->status != LIBUSB_TRANSFER_COMPLETED &&
            transfer->status != LIBUSB_TRANSFER_CANCELLED) {
            fail(std::string("bulk OUT completed with ") +
                 transfer_status_name(transfer->status));
        } else if (transfer->status == LIBUSB_TRANSFER_COMPLETED &&
                   !stopping_ &&
                   transfer->actual_length != static_cast<int>(out_transfer_length_)) {
            fail("short bulk OUT transfer");
        }

        if (stopping_ || fatal_) {
            transfers_.complete(out_id_);
            out_done_ = true;
            return;
        }
        if (!submit_out()) {
            transfers_.complete(out_id_);
            out_done_ = true;
        }
    }

    void process_frame(const Frame& frame)
    {
        switch (frame.type) {
            case FrameType::kDataEcho: {
                ++result_.data_echoes_received;
                const auto expected = expected_echoes_.find(frame.seq);
                if (expected == expected_echoes_.end()) {
                    ++result_.unmatched_echoes;
                    result_.notes.push_back(
                        "unexpected DATA_ECHO seq=" + std::to_string(frame.seq));
                    return;
                }
                const std::uint64_t actual =
                    fnv1a64(frame.payload.data(), frame.payload.size());
                if (actual != expected->second) {
                    ++result_.echo_mismatches;
                }
                expected_echoes_.erase(expected);
                return;
            }
            case FrameType::kHeartbeatAck: {
                ++result_.heartbeat_acks;
                const auto sent = heartbeat_sent_at_.find(frame.seq);
                if (sent == heartbeat_sent_at_.end()) {
                    result_.notes.push_back(
                        "unexpected HEARTBEAT_ACK seq=" + std::to_string(frame.seq));
                    return;
                }
                const std::uint64_t rtt =
                    elapsed_us(sent->second, Clock::now());
                result_.heartbeat_last_rtt_us = rtt;
                result_.heartbeat_max_rtt_us =
                    std::max(result_.heartbeat_max_rtt_us, rtt);
                heartbeat_sent_at_.erase(sent);
                return;
            }
            case FrameType::kStatsResponse:
                decode_device_stats(frame.payload);
                return;
            case FrameType::kError:
                fail("device returned ERROR frame");
                return;
            case FrameType::kHelloAck:
                result_.notes.push_back("unexpected duplicate HELLO_ACK");
                return;
            default:
                result_.notes.push_back(
                    "ignored frame type " +
                    std::to_string(static_cast<int>(frame.type)));
                return;
        }
    }

    void decode_device_stats(const std::vector<std::uint8_t>& payload)
    {
        if (payload.size() != kDeviceStatsFieldCount * sizeof(std::uint64_t)) {
            result_.notes.push_back(
                "STATS_RSP has unexpected length " + std::to_string(payload.size()));
            return;
        }
        std::uint64_t values[kDeviceStatsFieldCount] = {};
        for (std::size_t i = 0; i < kDeviceStatsFieldCount; ++i) {
            values[i] = read_u64_le(payload.data() + i * sizeof(std::uint64_t));
        }
        result_.device_stats.zero_reads = values[0];
        result_.device_stats.poll_wakeups = values[1];
        result_.device_stats.bytes_read = values[2];
        result_.device_stats.frames_read = values[3];
        result_.device_stats.bytes_written = values[4];
        result_.device_stats.frames_written = values[5];
        result_.device_stats.write_errors = values[6];
        result_.device_stats.eventfd_wakeups = values[7];
        result_.device_stats.signal_wakeups = values[8];
        result_.device_stats.owner_close_wakeups = values[9];
        result_.device_stats.lifecycle_background = values[10];
        result_.device_stats.lifecycle_foreground = values[11];
        result_.device_stats.poll_timeouts = values[12];
        result_.device_stats.read_errors = values[13];
        result_.device_stats.zero_writes = values[14];
        result_.device_stats.parse_errors = values[15];
        result_.device_stats_seen = true;
    }

    bool pump_events(int timeout_ms)
    {
        timeval timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        const int rc = libusb_handle_events_timeout(context_, &timeout);
        if (rc != LIBUSB_SUCCESS) {
            fail(std::string("libusb event loop failed: ") + libusb_error_name(rc));
            return false;
        }
        return true;
    }

    void watch_heartbeat(Clock::time_point now)
    {
        if (now - last_inbound_ <=
            std::chrono::milliseconds(options_.lost_after_ms)) {
            heartbeat_loss_active_ = false;
            return;
        }
        if (heartbeat_loss_active_) {
            return;
        }
        heartbeat_loss_active_ = true;
        ++result_.heartbeat_loss_events;
        result_.notes.push_back(
            "no valid inbound frame for at least " +
            std::to_string(options_.lost_after_ms) + " ms");
        log_ << "[heartbeat] WARNING: no valid inbound frame for >= "
             << options_.lost_after_ms << " ms\n";
    }

    void begin_cancellation()
    {
        stopping_ = true;
        cancel_started_ = Clock::now();
        result_.cancellation_requested = true;
        transfers_.mark_cancel_requested(Direction::kIn);
        transfers_.mark_cancel_requested(Direction::kOut);

        Clock::time_point first_issue = cancel_started_;
        if (in_active_ && !in_done_) {
            in_cancel_rc_ = libusb_cancel_transfer(in_transfer_);
            result_.in_cancel_rc = in_cancel_rc_;
            first_issue = Clock::now();
        } else {
            result_.in_cancel_rc = std::numeric_limits<int>::min();
        }
        if (out_active_ && !out_done_) {
            out_cancel_rc_ = libusb_cancel_transfer(out_transfer_);
            result_.out_cancel_rc = out_cancel_rc_;
            result_.cancel_issue_gap_us = elapsed_us(first_issue, Clock::now());
        } else {
            result_.out_cancel_rc = std::numeric_limits<int>::min();
        }

        log_ << "[cancel] issued IN rc="
             << (result_.in_cancel_rc == std::numeric_limits<int>::min()
                     ? std::string("not-active")
                     : libusb_error_name(result_.in_cancel_rc))
             << ", OUT rc="
             << (result_.out_cancel_rc == std::numeric_limits<int>::min()
                     ? std::string("not-active")
                     : libusb_error_name(result_.out_cancel_rc))
             << "; active=" << transfers_.active() << "\n";
    }

    void wait_until_transfers_zero()
    {
        if (transfers_.ready_to_release()) {
            result_.active_on_release = 0;
            result_.cleanup_safe = true;
            free_transfers_if_safe();
            return;
        }
        const Clock::time_point deadline =
            Clock::now() + std::chrono::milliseconds(options_.cancel_timeout_ms);
        while (!transfers_.ready_to_release() && Clock::now() < deadline) {
            if (!pump_events(25)) {
                break;
            }
        }
        result_.active_on_release = transfers_.active();
        result_.cleanup_safe = transfers_.ready_to_release();
        result_.cancel_wait_ms = elapsed_us(cancel_started_, Clock::now()) / 1000U;
        if (!result_.cleanup_safe) {
            fail("active_transfers did not reach zero before cancel timeout; "
                 "transfers and handle intentionally left unfreed");
            log_ << "[cleanup] REFUSING to free " << transfers_.active()
                 << " active transfer(s)\n";
            return;
        }
        log_ << "[cleanup] active_transfers=0 after " << result_.cancel_wait_ms
             << " ms; safe to release interface\n";
        free_transfers_if_safe();
    }

    void free_transfers_if_safe()
    {
        if (!result_.cleanup_safe) {
            return;
        }
        if (in_transfer_ != nullptr) {
            libusb_free_transfer(in_transfer_);
            in_transfer_ = nullptr;
        }
        if (out_transfer_ != nullptr) {
            libusb_free_transfer(out_transfer_);
            out_transfer_ = nullptr;
        }
    }

    void print_progress(Clock::time_point started, Clock::time_point now)
    {
        const double seconds = std::chrono::duration<double>(now - started).count();
        log_ << "[run] t=" << seconds << "s"
             << " out=" << result_.out_bytes
             << " in=" << result_.in_bytes
             << " data_frames=" << result_.data_frames_sent
             << " echoes=" << result_.data_echoes_received
             << " mismatches=" << result_.echo_mismatches
             << " hb=" << result_.heartbeat_acks << "/" << result_.heartbeats_sent
             << " hb_rtt_us=" << result_.heartbeat_last_rtt_us
             << " zlp_out=" << result_.zero_length_out_completed
             << " zlp_in=" << result_.zero_length_in_completed
             << " device_stats=" << (result_.device_stats_seen ? "yes" : "no")
             << "\n";
    }

    void finish(Clock::time_point now)
    {
        result_.pending_echoes_at_stop = expected_echoes_.size();
        if (transfers_.ready_to_release()) {
            result_.cleanup_safe = true;
            result_.active_on_release = 0;
        }
        result_.echo_ok =
            result_.handshake_ok &&
            result_.data_echoes_received > 0 &&
            result_.echo_mismatches == 0 &&
            result_.unmatched_echoes == 0;
        result_.cancellation_ok =
            result_.cancellation_requested &&
            result_.cleanup_safe &&
            result_.active_on_release == 0 &&
            transfers_.cancel_observed(Direction::kIn) &&
            transfers_.cancel_observed(Direction::kOut);
        result_.error = error_;

        if (result_.handshake_ok && result_.echo_ok) {
            log_ << "[result] handshake=ok echo=ok echoes="
                 << result_.data_echoes_received << "\n";
        }
        if (result_.cancellation_requested) {
            log_ << "[result] cancel_wait_ms=" << result_.cancel_wait_ms
                 << " in_status="
                 << (result_.in_callback_status < 0
                         ? std::string("no-callback")
                         : transfer_status_name(
                               static_cast<libusb_transfer_status>(
                                   result_.in_callback_status)))
                 << " out_status="
                 << (result_.out_callback_status < 0
                         ? std::string("no-callback")
                         : transfer_status_name(
                               static_cast<libusb_transfer_status>(
                                   result_.out_callback_status)))
                 << " active_on_release=" << result_.active_on_release << "\n";
        }
        if (!error_.empty()) {
            log_ << "[error] " << error_ << "\n";
        }
        (void)now;
    }

    void fail(const std::string& message)
    {
        if (error_.empty()) {
            error_ = message;
        }
        fatal_ = true;
    }

    std::uint32_t next_seq()
    {
        ++next_seq_;
        if (next_seq_ == 0) {
            next_seq_ = 1;
        }
        return next_seq_;
    }

    libusb_context* context_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    int interface_number_ = -1;
    std::uint8_t endpoint_in_ = 0;
    std::uint8_t endpoint_out_ = 0;
    ProbeOptions options_;
    std::ostream& log_;
    ProbeResult result_;
    bool claimed_ = false;
    bool fatal_ = false;
    bool stopping_ = false;
    std::string error_;
    std::uint32_t next_seq_ = 0;

    FrameDecoder decoder_;
    std::vector<std::uint8_t> in_buffer_;
    std::vector<std::uint8_t> out_buffer_;
    std::size_t out_capacity_ = 0;
    std::size_t out_transfer_length_ = 0;
    OutKind out_kind_ = OutKind::kNone;
    std::unordered_map<std::uint32_t, std::uint64_t> expected_echoes_;
    std::unordered_map<std::uint32_t, Clock::time_point> heartbeat_sent_at_;
    Clock::time_point last_heartbeat_sent_{};
    Clock::time_point last_stats_request_{};
    Clock::time_point last_inbound_{};
    Clock::time_point cancel_started_{};
    bool heartbeat_loss_active_ = false;
    std::uint64_t frames_since_zlp_ = 0;

    libusb_transfer* in_transfer_ = nullptr;
    libusb_transfer* out_transfer_ = nullptr;
    TransferSet transfers_;
    std::uint64_t in_id_ = 0;
    std::uint64_t out_id_ = 0;
    bool in_active_ = false;
    bool out_active_ = false;
    bool in_done_ = false;
    bool out_done_ = false;
    int in_cancel_rc_ = 0;
    int out_cancel_rc_ = 0;
};

} // namespace

ProbeResult run_duplex_probe(
    libusb_context* context,
    libusb_device_handle* handle,
    int interface_number,
    std::uint8_t endpoint_in,
    std::uint8_t endpoint_out,
    const ProbeOptions& options,
    std::ostream& log)
{
    DuplexProbe probe(
        context,
        handle,
        interface_number,
        endpoint_in,
        endpoint_out,
        options,
        log);
    return probe.run();
}

} // namespace gate1
