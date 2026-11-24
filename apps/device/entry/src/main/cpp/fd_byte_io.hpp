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
//   * writes must be non-blocking. `write()` reports exactly how many bytes the
//     kernel accepted and never claims bytes it did not hand over: the core
//     `flush()` contract pops a packet once `write()` reports its full size, so
//     a "pretend the rest is buffered" return silently loses control messages
//     (this is how STREAM_READY used to disappear).
//
// Because of the message-oriented read, callers must never see a short buffer
// truncate a host transfer. `read()` therefore always issues a full-size kernel
// read into an internal staging buffer and serves the caller's smaller request
// from it, keeping the remainder for the next call.
//
// `read()` is gated on poll(): the accessory descriptor is message-oriented and
// a read with no pending message blocks inside the driver even though the
// descriptor was put in O_NONBLOCK, so an unconditional read() wedges the worker
// after its first write (measured 2026-11-24: CLIENT_HELLO left the device, then
// the worker never consumed the Host's PREPARE). One POLLIN therefore permits
// exactly one message read, which is the semantic poll() reports here.
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

  struct Stats {
    std::uint64_t reads = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t zero_reads = 0;
    std::uint64_t read_would_block = 0;
    std::uint64_t read_errors = 0;
    std::uint64_t einval_shrinks = 0;
    std::uint64_t read_gated = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t write_would_block = 0;
    std::uint64_t write_partials = 0;
    std::uint64_t write_errors = 0;
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
  void trace_incoming(std::span<const std::uint8_t> bytes);
  void trace_outgoing(std::span<const std::uint8_t> bytes);

  int descriptor_ = -1;
  int wake_fd_ = -1;
  std::atomic<std::size_t> read_size_{16U * 1024U};
  // One POLLIN permits exactly one message read; refreshed by every poll().
  std::atomic<int> read_permits_{0};
  mutable std::mutex mutex_;
  // Worker-thread-only staging for the full-size kernel read.
  std::vector<std::uint8_t> read_buffer_;
  std::size_t staged_offset_ = 0;
  std::size_t staged_length_ = 0;
  // Bytes of the caller's last (partially written) span that the kernel has
  // not accepted yet. Never owns data: the core keeps the packet and retries.
  std::size_t pending_bytes_ = 0;
  int trace_budget_ = 8;
  int tx_trace_budget_ = 16;
  Stats stats_;
  std::string last_error_;
  std::atomic<bool> hung_up_{false};
  std::atomic<bool> cancelled_{false};
};

}  // namespace deskmate::device
