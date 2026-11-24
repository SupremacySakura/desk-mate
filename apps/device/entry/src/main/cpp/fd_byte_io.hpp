#pragma once

#include "deskmate/byte_io.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace deskmate::device {

// ByteIo over the ArkTS-owned accessory descriptor.
//
// The descriptor is never closed here (ADR 0004). Every behaviour below is
// pinned by the 2026-11-24 device measurements in probes/gate1/RUNBOOK.md 5.1:
//   * read() is message-oriented: a buffer smaller than one host bulk OUT
//     silently drops the remainder, so the buffer starts large and shrinks only
//     when the kernel rejects the size with EINVAL.
//   * 64 KiB reads fail with EINVAL while poll() still reports POLLIN, so the
//     size adapts down instead of treating that as a fatal read error.
//   * poll() ignores its timeout, so wakeups come from an eventfd instead.
//   * writes must be non-blocking with an output buffer and high-watermark
//     backpressure, otherwise a NAK'd OUT endpoint wedges the worker.
//
// Because of the message-oriented read, callers must never see a short buffer
// truncate a host transfer. `read()` therefore always issues a full-size kernel
// read into an internal staging buffer and serves the caller's smaller request
// from it, keeping the remainder for the next call.
class FdByteIo final : public protocol::ByteIo {
 public:
  explicit FdByteIo(int descriptor);
  ~FdByteIo() override;

  FdByteIo(const FdByteIo&) = delete;
  FdByteIo& operator=(const FdByteIo&) = delete;

  [[nodiscard]] protocol::IoResult read(
      std::span<std::uint8_t> destination) override;
  [[nodiscard]] protocol::IoResult write(
      std::span<const std::uint8_t> source) override;
  void cancel() override;

  [[nodiscard]] bool valid() const { return descriptor_ >= 0; }
  [[nodiscard]] bool hung_up() const { return hung_up_.load(); }
  [[nodiscard]] int wake_fd() const { return wake_fd_; }

  void note_wakeup();
  void note_poll_revents(std::uint32_t revents);
  void flush_pending();

  struct Stats {
    std::uint64_t reads = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t zero_reads = 0;
    std::uint64_t read_would_block = 0;
    std::uint64_t read_errors = 0;
    std::uint64_t einval_shrinks = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t write_would_block = 0;
    std::uint64_t write_errors = 0;
    std::uint64_t backpressure_stalls = 0;
    std::uint64_t poll_hangups = 0;
    std::uint64_t poll_wakeups = 0;
    std::size_t read_buffer_bytes = 0;
    std::size_t pending_write_bytes = 0;
    int last_read_errno = 0;
    int last_write_errno = 0;
    std::uint32_t last_poll_revents = 0;
  };

  [[nodiscard]] Stats stats() const;
  [[nodiscard]] std::string last_error() const;

 private:
  void note_error(const std::string& message);

  int descriptor_ = -1;
  int wake_fd_ = -1;
  std::atomic<std::size_t> read_size_{16U * 1024U};
  mutable std::mutex mutex_;
  // Worker-thread-only staging for the full-size kernel read.
  std::vector<std::uint8_t> read_buffer_;
  std::size_t staged_offset_ = 0;
  std::size_t staged_length_ = 0;
  std::deque<std::vector<std::uint8_t>> pending_;
  std::size_t pending_bytes_ = 0;
  Stats stats_;
  std::string last_error_;
  std::atomic<bool> hung_up_{false};
  std::atomic<bool> cancelled_{false};
};

}  // namespace deskmate::device
