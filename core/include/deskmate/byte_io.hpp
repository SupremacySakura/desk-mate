#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace deskmate::protocol {

enum class IoStatus {
  Ok,
  WouldBlock,
  Closed,
  Error,
};

struct IoResult {
  IoStatus status{IoStatus::Ok};
  std::size_t bytes{};
};

class ByteIo {
 public:
  ByteIo() = default;
  ByteIo(const ByteIo&) = delete;
  ByteIo& operator=(const ByteIo&) = delete;
  virtual ~ByteIo() = default;

  [[nodiscard]] virtual IoResult read(std::span<std::uint8_t> destination) = 0;
  [[nodiscard]] virtual IoResult write(std::span<const std::uint8_t> source) = 0;
  virtual void cancel() {}
};

}  // namespace deskmate::protocol
