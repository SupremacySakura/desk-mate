#include "fd_byte_io.hpp"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace deskmate::device {
namespace {

constexpr std::size_t kMinReadSize = 1024;
constexpr std::size_t kOutputHighWatermark = 128U * 1024U;

}  // namespace

FdByteIo::FdByteIo(int descriptor) : descriptor_(descriptor)
{
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
    std::lock_guard lock(mutex_);
    stats_.last_poll_revents = revents;
    if ((revents & 0x20U) != 0) {  // POLLHUP
        ++stats_.poll_hangups;
    }
    hung_up_.store((revents & 0x20U) != 0);
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
        return {protocol::IoStatus::Error, 0};
    }
}

void FdByteIo::flush_pending()
{
    std::lock_guard lock(mutex_);
    while (!pending_.empty()) {
        auto& chunk = pending_.front();
        const ssize_t bytes =
            ::write(descriptor_, chunk.data(), chunk.size());
        if (bytes > 0) {
            stats_.bytes_written += static_cast<std::uint64_t>(bytes);
            pending_bytes_ -= static_cast<std::size_t>(bytes);
            if (static_cast<std::size_t>(bytes) == chunk.size()) {
                pending_.pop_front();
            } else {
                chunk.erase(chunk.begin(),
                            chunk.begin() + static_cast<std::ptrdiff_t>(bytes));
            }
            continue;
        }
        const int error = errno;
        stats_.last_write_errno = error;
        if (error == EAGAIN || error == EWOULDBLOCK) {
            ++stats_.write_would_block;
            return;
        }
        if (error == EINTR) {
            continue;
        }
        ++stats_.write_errors;
        last_error_ = std::string("write failed: ") + std::strerror(error);
        return;
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

    {
        std::lock_guard lock(mutex_);
        // Backpressure: refuse new bytes while the endpoint is NAKing, so the
        // session scheduler can drop whole GOPs instead of growing memory.
        if (pending_bytes_ >= kOutputHighWatermark) {
            ++stats_.backpressure_stalls;
            return {protocol::IoStatus::WouldBlock, 0};
        }
        pending_.emplace_back(source.begin(), source.end());
        pending_bytes_ += source.size();
    }
    flush_pending();

    std::lock_guard lock(mutex_);
    stats_.pending_write_bytes = pending_bytes_;
    if (!last_error_.empty() && stats_.write_errors > 0) {
        return {protocol::IoStatus::Error, 0};
    }
    return {protocol::IoStatus::Ok, source.size()};
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
