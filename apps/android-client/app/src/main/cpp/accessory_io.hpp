#pragma once

#include "deskmate/byte_io.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace deskmate::android {

// ByteIo over Android's UsbManager.openAccessory() descriptor.
//
// Java owns the ParcelFileDescriptor and closes it after requestStop(); this
// class only borrows the integer fd. Reads are gated by poll(POLLIN) because a
// blocking accessory read cannot be cancelled by a flag alone. Writes are
// non-blocking and report the exact prefix accepted by the kernel.
class AccessoryIo final : public deskmate::protocol::ByteIo {
 public:
  explicit AccessoryIo(int descriptor);
  ~AccessoryIo() override;

  AccessoryIo(const AccessoryIo&) = delete;
  AccessoryIo& operator=(const AccessoryIo&) = delete;

  [[nodiscard]] deskmate::protocol::IoResult read(
      std::span<std::uint8_t> destination) override;
  [[nodiscard]] deskmate::protocol::IoResult write(
      std::span<const std::uint8_t> source) override;
  void cancel() override;
  void wake();

  // Called by the worker after poll() reports readiness.
  void note_poll(int revents);
  [[nodiscard]] bool valid() const { return descriptor_ >= 0; }
  [[nodiscard]] bool hung_up() const { return hung_up_.load(); }
  [[nodiscard]] int wake_fd() const { return wake_fd_; }

  struct Stats {
    std::uint64_t bytes_read{};
    std::uint64_t bytes_written{};
    std::uint64_t read_errors{};
    std::uint64_t write_errors{};
    std::uint64_t read_would_block{};
    std::uint64_t write_would_block{};
    std::uint64_t partial_writes{};
    std::size_t read_buffer_bytes{};
  };
  [[nodiscard]] Stats stats() const;

 private:
  int descriptor_ = -1;
  int wake_fd_ = -1;
  // eventfd is Linux-only; the host contract test and any POSIX fallback use a
  // pipe, so the write side is tracked separately.
  int wake_write_fd_ = -1;
  std::atomic<bool> cancelled_{false};
  std::atomic<bool> hung_up_{false};
  std::atomic<int> read_permits_{0};
  std::atomic<std::size_t> read_size_{16U * 1024U};
  std::atomic<std::uint64_t> bytes_read_{0};
  std::atomic<std::uint64_t> bytes_written_{0};
  std::atomic<std::uint64_t> read_errors_{0};
  std::atomic<std::uint64_t> write_errors_{0};
  std::atomic<std::uint64_t> read_would_block_{0};
  std::atomic<std::uint64_t> write_would_block_{0};
  std::atomic<std::uint64_t> partial_writes_{0};
  mutable std::mutex mutex_;
  std::vector<std::uint8_t> staging_;
  std::size_t staged_offset_{};
  std::size_t staged_length_{};
};

}  // namespace deskmate::android
