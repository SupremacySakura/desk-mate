// Host-side contract test for the Android fd semantics. Compiled with the host
// toolchain by apps/common/tests/run_android_io_test.sh; the POSIX poll/read/
// write behavior is the same subset Android exposes.

#include "accessory_io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                   #condition);                                                \
      ++failures;                                                              \
      return;                                                                  \
    }                                                                          \
  } while (false)

struct Pipe {
  int read_fd{-1};
  int write_fd{-1};
  Pipe() {
    int fds[2]{};
    if (::pipe(fds) == 0) {
      read_fd = fds[0];
      write_fd = fds[1];
    }
  }
  ~Pipe() {
    if (read_fd >= 0) {
      ::close(read_fd);
    }
    if (write_fd >= 0) {
      ::close(write_fd);
    }
  }
};

void test_pollin_gates_read() {
  Pipe pipe;
  CHECK(pipe.read_fd >= 0);
  deskmate::android::AccessoryIo io(pipe.read_fd);
  std::array<std::uint8_t, 64> buffer{};
  const auto blocked = io.read(buffer);
  CHECK(blocked.status == deskmate::protocol::IoStatus::WouldBlock);
  const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};
  CHECK(::write(pipe.write_fd, payload.data(), payload.size()) == 4);
  pollfd fd{pipe.read_fd, POLLIN, 0};
  CHECK(::poll(&fd, 1, 1000) > 0);
  io.note_poll(fd.revents);
  const auto first = io.read(buffer);
  CHECK(first.status == deskmate::protocol::IoStatus::Ok);
  CHECK(first.bytes == 4U);
  CHECK(std::equal(payload.begin(), payload.end(), buffer.begin()));
  const auto empty = io.read(buffer);
  CHECK(empty.status == deskmate::protocol::IoStatus::WouldBlock);
}

void test_partial_write_reports_exact_prefix() {
  Pipe pipe;
  CHECK(pipe.read_fd >= 0);
  // Fill the pipe so the next non-blocking write only takes a prefix.
  const int flags = fcntl(pipe.write_fd, F_GETFL, 0);
  (void)fcntl(pipe.write_fd, F_SETFL, flags | O_NONBLOCK);
  std::vector<std::uint8_t> filler(1 << 16, 0xAA);
  while (::write(pipe.write_fd, filler.data(), filler.size()) > 0) {
  }
  deskmate::android::AccessoryIo io(pipe.write_fd);
  const std::vector<std::uint8_t> payload(4096, 0x5A);
  const auto result = io.write(payload);
  CHECK(result.status == deskmate::protocol::IoStatus::Ok ||
        result.status == deskmate::protocol::IoStatus::WouldBlock);
  CHECK(result.bytes <= payload.size());
}

void test_cancel_wakes_eventfd() {
  Pipe pipe;
  deskmate::android::AccessoryIo io(pipe.read_fd);
  CHECK(io.wake_fd() >= 0);
  io.cancel();
  pollfd fd{io.wake_fd(), POLLIN, 0};
  CHECK(::poll(&fd, 1, 1000) > 0);
  std::uint64_t value = 0;
  CHECK(::read(io.wake_fd(), &value, sizeof(value)) == sizeof(value));
}

}  // namespace

int main() {
  test_pollin_gates_read();
  test_partial_write_reports_exact_prefix();
  test_cancel_wakes_eventfd();
  if (failures != 0) {
    std::fprintf(stderr, "%d Android I/O test(s) failed\n", failures);
    return 1;
  }
  std::puts("all Android I/O tests passed");
  return 0;
}
