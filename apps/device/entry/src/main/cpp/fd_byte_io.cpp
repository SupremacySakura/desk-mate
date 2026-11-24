#include "fd_byte_io.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3210
#define LOG_TAG "DeskMateIo"

namespace deskmate::device {
namespace {

constexpr std::size_t kMinReadSize = 1024;
}  // namespace

FdByteIo::FdByteIo(int descriptor) : descriptor_(descriptor)
{
    // A blocking write() to a NAK'd OUT endpoint never returns and cannot be
    // interrupted by the eventfd, so the descriptor must be non-blocking
    // (probes/gate1/RUNBOOK.md 5.1 item 6). Read stays non-blocking too; the
    // worker drives it from poll().
    if (descriptor_ >= 0) {
        const int flags = fcntl(descriptor_, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(descriptor_, F_SETFL, flags | O_NONBLOCK);
        } else {
            note_error(std::string("F_GETFL failed: ") + std::strerror(errno));
        }
    }
    // The eventfd only carries wakeups; it is created before the descriptor is
    // ever handed to a session so the worker can never miss the first one.
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    read_buffer_.resize(read_size_.load());
    std::lock_guard lock(mutex_);
    stats_.read_buffer_bytes = read_size_.load();
}

FdByteIo::~FdByteIo()
{
    if (wake_fd_ >= 0) {
        close(wake_fd_);
        wake_fd_ = -1;
    }
}

void FdByteIo::note_error(const std::string& message)
{
    std::lock_guard lock(mutex_);
    last_error_ = message;
}

void FdByteIo::note_wakeup()
{
    std::lock_guard lock(mutex_);
    ++stats_.poll_wakeups;
}

void FdByteIo::note_poll_revents(std::uint32_t revents)
{
    // POLLIN means the driver has one whole message staged. The permit is
    // capped at one: POLLIN is level-triggered here and can be reported again
    // before the reader consumes it, and a second permit would authorise a
    // read() with nothing to collect - which blocks inside the driver and
    // freezes the worker.
    //
    // A permit is only meaningful when nothing is staged: bytes already read
    // out of the kernel are served from the staging buffer without touching the
    // descriptor, so a permit granted while staging is non-empty would survive
    // into the next read() and block it on an already-drained message.
    if ((revents & 0x01U) != 0 && staged_offset_ == staged_length_) {
        read_permits_.store(1);
    }
    std::lock_guard lock(mutex_);
    stats_.last_poll_revents = revents;
    if ((revents & 0x20U) != 0) {  // POLLHUP
        ++stats_.poll_hangups;
    }
    hung_up_.store((revents & 0x20U) != 0);
}

namespace {

std::uint32_t read_le32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (bytes.size() < offset + 4) {
        return 0;
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

// Control opcodes, duplicated here only for logging so a stalled handshake can
// be read straight out of hilog. Returns -1 when the chunk does not start with
// a complete control packet.
int control_opcode(std::span<const std::uint8_t> bytes)
{
    constexpr std::size_t kHeaderSize = 32;
    constexpr std::uint8_t kControlType = 0x04;
    if (bytes.size() < kHeaderSize + 1 || bytes[0] != 'D' || bytes[1] != 'M' ||
        bytes[3] != kControlType) {
        return -1;
    }
    const std::uint32_t payload_len = read_le32(bytes, 20);
    if (payload_len < 1 || bytes.size() < kHeaderSize + payload_len) {
        return -1;
    }
    return static_cast<int>(bytes[kHeaderSize]);
}

const char *opcode_name(int opcode)
{
    switch (opcode) {
        case 0x01:
            return "CLIENT_HELLO";
        case 0x02:
            return "STREAM_PREPARE";
        case 0x03:
            return "STREAM_READY";
        case 0x04:
            return "STREAM_COMMIT";
        case 0x05:
            return "STREAM_COMMITTED";
        case 0x06:
            return "PAUSE";
        case 0x07:
            return "PAUSE_ACK";
        case 0x08:
            return "RESUME";
        case 0x09:
            return "HEARTBEAT";
        case 0x0a:
            return "REQUEST_IDR";
        case 0x0b:
            return "PROTOCOL_ERROR";
        default:
            return "UNKNOWN";
    }
}

std::string hex_head(std::span<const std::uint8_t> bytes, std::size_t count)
{
    const std::size_t shown = std::min<std::size_t>(bytes.size(), count);
    std::string hex;
    char nibble[4] = {};
    for (std::size_t index = 0; index < shown; ++index) {
        std::snprintf(nibble, sizeof(nibble), "%02x", bytes[index]);
        hex += nibble;
        if (index + 1 != shown) {
            hex += ' ';
        }
    }
    return hex;
}

}  // namespace

// Prints the head of the first few inbound chunks. When the handshake stalls,
// this separates "the host never sent anything" from "the host sent bytes the
// decoder rejected" without a bus analyser.
void FdByteIo::trace_incoming(std::span<const std::uint8_t> bytes)
{
    const int opcode = control_opcode(bytes);
    if (opcode >= 0) {
        // Control traffic is rare and decides the handshake: log every packet.
        OH_LOG_INFO(LOG_APP,
                    "rx %{public}s opcode=%{public}d seq=%{public}u "
                    "nonce=0x%{public}x chunk=%{public}zu",
                    opcode_name(opcode), opcode, read_le32(bytes, 16),
                    read_le32(bytes, 8),
                    bytes.size());
        return;
    }
    if (trace_budget_ <= 0) {
        return;
    }
    --trace_budget_;
    OH_LOG_INFO(LOG_APP, "rx chunk=%{public}zu head=[%{public}s]",
                bytes.size(), hex_head(bytes, 24).c_str());
}

void FdByteIo::trace_outgoing(std::span<const std::uint8_t> bytes)
{
    const int opcode = control_opcode(bytes);
    if (opcode >= 0) {
        // This is the authoritative "the kernel accepted these bytes" proof
        // that READY / COMMIT_ACK actually left the device.
        OH_LOG_INFO(LOG_APP,
                    "tx %{public}s opcode=%{public}d seq=%{public}u "
                    "nonce=0x%{public}x bytes=%{public}zu",
                    opcode_name(opcode), opcode, read_le32(bytes, 16),
                    read_le32(bytes, 8), bytes.size());
        return;
    }
    if (tx_trace_budget_ <= 0) {
        return;
    }
    --tx_trace_budget_;
    OH_LOG_INFO(LOG_APP, "tx bytes=%{public}zu head=[%{public}s]",
                bytes.size(), hex_head(bytes, 24).c_str());
}

protocol::IoResult FdByteIo::read(std::span<std::uint8_t> destination)
{
    if (descriptor_ < 0) {
        return {protocol::IoStatus::Closed, 0};
    }
    if (destination.empty()) {
        return {protocol::IoStatus::WouldBlock, 0};
    }

    // Serve any bytes left over from an earlier full-size kernel read first.
    if (staged_offset_ < staged_length_) {
        const std::size_t available = staged_length_ - staged_offset_;
        const std::size_t take = std::min(available, destination.size());
        std::memcpy(destination.data(), read_buffer_.data() + staged_offset_,
                    take);
        staged_offset_ += take;
        if (staged_offset_ == staged_length_) {
            staged_offset_ = 0;
            staged_length_ = 0;
        }
        return {protocol::IoStatus::Ok, take};
    }

    // No POLLIN, no message: issuing read() here blocks inside the driver.
    if (read_permits_.load() <= 0) {
        std::lock_guard lock(mutex_);
        ++stats_.read_gated;
        return {protocol::IoStatus::WouldBlock, 0};
    }

    // One permit per message; EINVAL retries reuse it instead of asking poll()
    // again, because the message is still pending in the driver.
    read_permits_.fetch_sub(1);
    for (;;) {
        // Always read a full kernel-sized chunk: a shorter request would make
        // the driver discard the rest of the host's bulk transfer.
        const std::size_t attempt = read_size_.load();
        if (read_buffer_.size() != attempt) {
            read_buffer_.resize(attempt);
        }
        const ssize_t bytes =
            ::read(descriptor_, read_buffer_.data(), attempt);
        if (bytes > 0) {
            staged_length_ = static_cast<std::size_t>(bytes);
            staged_offset_ = 0;
            trace_incoming(std::span<const std::uint8_t>(
                read_buffer_.data(), staged_length_));
            std::lock_guard lock(mutex_);
            ++stats_.reads;
            stats_.bytes_read += static_cast<std::uint64_t>(bytes);
            const std::size_t take =
                std::min(staged_length_, destination.size());
            std::memcpy(destination.data(), read_buffer_.data(), take);
            staged_offset_ = take;
            if (staged_offset_ == staged_length_) {
                staged_offset_ = 0;
                staged_length_ = 0;
            }
            return {protocol::IoStatus::Ok, take};
        }
        staged_offset_ = 0;
        staged_length_ = 0;
        if (bytes == 0) {
            std::lock_guard lock(mutex_);
            ++stats_.zero_reads;
            return {protocol::IoStatus::WouldBlock, 0};
        }

        const int error = errno;
        {
            std::lock_guard lock(mutex_);
            stats_.last_read_errno = error;
        }
        if (error == EAGAIN || error == EWOULDBLOCK) {
            std::lock_guard lock(mutex_);
            ++stats_.read_would_block;
            return {protocol::IoStatus::WouldBlock, 0};
        }
        if (error == EINTR) {
            if (cancelled_.load()) {
                return {protocol::IoStatus::Closed, 0};
            }
            continue;
        }
        // 64 KiB requests are rejected with EINVAL while poll() still reports
        // POLLIN. Shrink and retry rather than failing the session.
        if (error == EINVAL && read_size_.load() > kMinReadSize) {
            const std::size_t next =
                std::max(kMinReadSize, read_size_.load() / 2U);
            read_size_.store(next);
            read_buffer_.resize(next);
            std::lock_guard lock(mutex_);
            ++stats_.einval_shrinks;
            stats_.read_buffer_bytes = next;
            OH_LOG_WARN(LOG_APP, "read EINVAL, buffer now %{public}zu", next);
            continue;
        }
        if (error == EIO || error == ENODEV || error == EPIPE) {
            hung_up_.store(true);
        }
        {
            std::lock_guard lock(mutex_);
            ++stats_.read_errors;
            last_error_ = std::string("read failed: ") + std::strerror(error);
        }
        OH_LOG_ERROR(LOG_APP, "read failed errno=%{public}d (%{public}s)",
                     error, std::strerror(error));
        return {protocol::IoStatus::Error, 0};
    }
}

protocol::IoResult FdByteIo::write(std::span<const std::uint8_t> source)
{
    if (descriptor_ < 0) {
        return {protocol::IoStatus::Closed, 0};
    }
    if (source.empty()) {
        return {protocol::IoStatus::Ok, 0};
    }

    // The descriptor is O_NONBLOCK and the caller keeps the unwritten tail:
    // report exactly what the kernel took. Claiming the full span while only
    // part of it reached the endpoint makes EndpointCore::flush() pop a control
    // message that was never sent (STREAM_READY vanished this way).
    const ssize_t bytes = ::write(descriptor_, source.data(), source.size());
    if (bytes > 0) {
        {
            std::lock_guard lock(mutex_);
            stats_.bytes_written += static_cast<std::uint64_t>(bytes);
            if (static_cast<std::size_t>(bytes) < source.size()) {
                ++stats_.write_partials;
            }
            pending_bytes_ = source.size() - static_cast<std::size_t>(bytes);
            stats_.pending_write_bytes = pending_bytes_;
        }
        trace_outgoing(std::span<const std::uint8_t>(
            source.data(), static_cast<std::size_t>(bytes)));
        return {protocol::IoStatus::Ok, static_cast<std::size_t>(bytes)};
    }
    if (bytes < 0) {
        const int error = errno;
        {
            std::lock_guard lock(mutex_);
            stats_.last_write_errno = error;
            if (error == EAGAIN || error == EWOULDBLOCK) {
                ++stats_.write_would_block;
                pending_bytes_ = source.size();
                stats_.pending_write_bytes = pending_bytes_;
                return {protocol::IoStatus::WouldBlock, 0};
            }
            if (error == EINTR) {
                // Retry by the caller; not an error, nothing was consumed.
                pending_bytes_ = source.size();
                stats_.pending_write_bytes = pending_bytes_;
                return {protocol::IoStatus::WouldBlock, 0};
            }
            ++stats_.write_errors;
            last_error_ = std::string("write failed: ") + std::strerror(error);
        }
        OH_LOG_ERROR(LOG_APP, "write failed errno=%{public}d (%{public}s)",
                     error, std::strerror(error));
        return {protocol::IoStatus::Error, 0};
    }
    {
        std::lock_guard lock(mutex_);
        pending_bytes_ = source.size();
        stats_.pending_write_bytes = pending_bytes_;
        ++stats_.write_would_block;
    }
    return {protocol::IoStatus::WouldBlock, 0};
}

void FdByteIo::cancel()
{
    cancelled_.store(true);
    const std::uint64_t value = 1;
    if (wake_fd_ >= 0) {
        const ssize_t written = ::write(wake_fd_, &value, sizeof(value));
        (void)written;
    }
}

FdByteIo::Stats FdByteIo::stats() const
{
    std::lock_guard lock(mutex_);
    Stats copy = stats_;
    copy.pending_write_bytes = pending_bytes_;
    copy.read_buffer_bytes = read_size_.load();
    return copy;
}

std::string FdByteIo::last_error() const
{
    std::lock_guard lock(mutex_);
    return last_error_;
}

}  // namespace deskmate::device
