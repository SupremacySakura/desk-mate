#include "accessory_io.hpp"

#include <android/log.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#if defined(__ANDROID__) || defined(__linux__)
#include <sys/eventfd.h>
#define DESKMATE_HAS_EVENTFD 1
#else
#define DESKMATE_HAS_EVENTFD 0
#endif

#define LOG_TAG "DeskMateAccessory"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace deskmate::android {
namespace {

constexpr std::size_t kMinReadSize = 1024;

}  // namespace

AccessoryIo::AccessoryIo(int descriptor) : descriptor_(descriptor) {
  if (descriptor_ >= 0) {
    const int flags = fcntl(descriptor_, F_GETFL, 0);
    if (flags >= 0) {
      (void)fcntl(descriptor_, F_SETFL, flags | O_NONBLOCK);
    }
  }
#if DESKMATE_HAS_EVENTFD
  wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#else
  int wake_pipe[2]{-1, -1};
  if (::pipe(wake_pipe) == 0) {
    for (int& fd : wake_pipe) {
      const int flags = fcntl(fd, F_GETFL, 0);
      if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      }
      (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    wake_fd_ = wake_pipe[0];
    wake_write_fd_ = wake_pipe[1];
  }
#endif
  staging_.resize(read_size_.load(std::memory_order_relaxed));
}

AccessoryIo::~AccessoryIo() {
  if (wake_fd_ >= 0) {
    close(wake_fd_);
    wake_fd_ = -1;
  }
  if (wake_write_fd_ >= 0) {
    close(wake_write_fd_);
    wake_write_fd_ = -1;
  }
}

void AccessoryIo::wake() {
#if DESKMATE_HAS_EVENTFD
  if (wake_fd_ >= 0) {
    const std::uint64_t value = 1;
    const ssize_t written = ::write(wake_fd_, &value, sizeof(value));
    (void)written;
  }
#else
  if (wake_write_fd_ >= 0) {
    const std::uint64_t value = 1;
    const ssize_t written = ::write(wake_write_fd_, &value, sizeof(value));
    (void)written;
  }
#endif
}

void AccessoryIo::cancel() {
  cancelled_.store(true, std::memory_order_release);
  wake();
}

void AccessoryIo::note_poll(int revents) {
  // POLLIN authorizes exactly one message read. The accessory driver blocks
  // inside read() when no message is staged even with O_NONBLOCK, so a stale
  // permit would freeze the worker.
  if ((revents & POLLIN) != 0 && staged_offset_ == staged_length_) {
    read_permits_.store(1, std::memory_order_release);
  }
  if ((revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
    hung_up_.store(true, std::memory_order_release);
  }
}

deskmate::protocol::IoResult AccessoryIo::read(
    std::span<std::uint8_t> destination) {
  if (descriptor_ < 0 || destination.empty()) {
    return {descriptor_ < 0 ? deskmate::protocol::IoStatus::Closed
                            : deskmate::protocol::IoStatus::WouldBlock,
            0};
  }
  if (staged_offset_ < staged_length_) {
    const std::size_t take =
        std::min(destination.size(), staged_length_ - staged_offset_);
    std::memcpy(destination.data(), staging_.data() + staged_offset_, take);
    staged_offset_ += take;
    if (staged_offset_ == staged_length_) {
      staged_offset_ = 0;
      staged_length_ = 0;
    }
    return {deskmate::protocol::IoStatus::Ok, take};
  }
  if (read_permits_.load(std::memory_order_acquire) <= 0) {
    read_would_block_.fetch_add(1, std::memory_order_relaxed);
    return {deskmate::protocol::IoStatus::WouldBlock, 0};
  }
  read_permits_.fetch_sub(1, std::memory_order_acq_rel);

  for (;;) {
    const std::size_t attempt = read_size_.load(std::memory_order_relaxed);
    const bool direct = destination.size() >= attempt;
    if (!direct && staging_.size() != attempt) {
      staging_.resize(attempt);
    }
    std::uint8_t* target = direct ? destination.data() : staging_.data();
    const ssize_t bytes = ::read(descriptor_, target, attempt);
    if (bytes > 0) {
      bytes_read_.fetch_add(static_cast<std::uint64_t>(bytes),
                            std::memory_order_relaxed);
      if (direct) {
        return {deskmate::protocol::IoStatus::Ok,
                static_cast<std::size_t>(bytes)};
      }
      staged_length_ = static_cast<std::size_t>(bytes);
      const std::size_t take =
          std::min(staged_length_, destination.size());
      std::memcpy(destination.data(), staging_.data(), take);
      staged_offset_ = take;
      if (staged_offset_ == staged_length_) {
        staged_offset_ = 0;
        staged_length_ = 0;
      }
      return {deskmate::protocol::IoStatus::Ok, take};
    }
    if (bytes == 0) {
      return {deskmate::protocol::IoStatus::WouldBlock, 0};
    }
    const int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK) {
      read_would_block_.fetch_add(1, std::memory_order_relaxed);
      return {deskmate::protocol::IoStatus::WouldBlock, 0};
    }
    if (error == EINTR) {
      if (cancelled_.load(std::memory_order_acquire)) {
        return {deskmate::protocol::IoStatus::Closed, 0};
      }
      continue;
    }
    if (error == EINVAL && read_size_.load(std::memory_order_relaxed) > kMinReadSize) {
      const std::size_t next =
          std::max(kMinReadSize, read_size_.load(std::memory_order_relaxed) / 2U);
      read_size_.store(next, std::memory_order_release);
      if (staging_.size() != next) {
        staging_.resize(next);
      }
      continue;
    }
    if (error == EIO || error == ENODEV || error == EPIPE) {
      hung_up_.store(true, std::memory_order_release);
    }
    read_errors_.fetch_add(1, std::memory_order_relaxed);
    LOGE("read failed errno=%d (%s)", error, std::strerror(error));
    return {deskmate::protocol::IoStatus::Error, 0};
  }
}

deskmate::protocol::IoResult AccessoryIo::write(
    std::span<const std::uint8_t> source) {
  if (descriptor_ < 0) {
    return {deskmate::protocol::IoStatus::Closed, 0};
  }
  if (source.empty()) {
    return {deskmate::protocol::IoStatus::Ok, 0};
  }
  const ssize_t bytes = ::write(descriptor_, source.data(), source.size());
  if (bytes > 0) {
    bytes_written_.fetch_add(static_cast<std::uint64_t>(bytes),
                             std::memory_order_relaxed);
    if (static_cast<std::size_t>(bytes) < source.size()) {
      partial_writes_.fetch_add(1, std::memory_order_relaxed);
    }
    return {deskmate::protocol::IoStatus::Ok, static_cast<std::size_t>(bytes)};
  }
  if (bytes < 0) {
    const int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) {
      write_would_block_.fetch_add(1, std::memory_order_relaxed);
      return {deskmate::protocol::IoStatus::WouldBlock, 0};
    }
    write_errors_.fetch_add(1, std::memory_order_relaxed);
    LOGE("write failed errno=%d (%s)", error, std::strerror(error));
    return {deskmate::protocol::IoStatus::Error, 0};
  }
  return {deskmate::protocol::IoStatus::WouldBlock, 0};
}

AccessoryIo::Stats AccessoryIo::stats() const {
  Stats copy;
  copy.bytes_read = bytes_read_.load(std::memory_order_relaxed);
  copy.bytes_written = bytes_written_.load(std::memory_order_relaxed);
  copy.read_errors = read_errors_.load(std::memory_order_relaxed);
  copy.write_errors = write_errors_.load(std::memory_order_relaxed);
  copy.read_would_block = read_would_block_.load(std::memory_order_relaxed);
  copy.write_would_block = write_would_block_.load(std::memory_order_relaxed);
  copy.partial_writes = partial_writes_.load(std::memory_order_relaxed);
  copy.read_buffer_bytes = read_size_.load(std::memory_order_relaxed);
  return copy;
}

}  // namespace deskmate::android
