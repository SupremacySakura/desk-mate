// Exercises the Windows-style link manager and Android-style device session
// against the shared core, without a live USB device.

#include "deskmate/platform/input_frames.hpp"
#include "deskmate/platform/session_profile.hpp"
#include "deskmate/windows/aoa_links.hpp"
#include "deskmate/windows/host_session.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
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

class SharedPipe {
 public:
  deskmate::protocol::IoResult read(std::span<std::uint8_t> destination) {
    std::lock_guard lock(mutex_);
    if (offset_ == bytes_.size()) {
      return {deskmate::protocol::IoStatus::WouldBlock, 0};
    }
    const std::size_t take =
        std::min(destination.size(), bytes_.size() - offset_);
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), take,
                destination.begin());
    offset_ += take;
    return {deskmate::protocol::IoStatus::Ok, take};
  }

  deskmate::protocol::IoResult write(std::span<const std::uint8_t> source) {
    std::lock_guard lock(mutex_);
    bytes_.insert(bytes_.end(), source.begin(), source.end());
    return {deskmate::protocol::IoStatus::Ok, source.size()};
  }

 private:
  std::mutex mutex_;
  std::vector<std::uint8_t> bytes_;
  std::size_t offset_{};
};

class FakeTransport final : public deskmate::windows::LinkTransport {
 public:
  explicit FakeTransport(std::shared_ptr<SharedPipe> pipe)
      : pipe_(std::move(pipe)) {}

  deskmate::protocol::IoResult read(
      std::span<std::uint8_t> destination) override {
    return pipe_->read(destination);
  }
  deskmate::protocol::IoResult write(
      std::span<const std::uint8_t> source) override {
    return pipe_->write(source);
  }
  [[nodiscard]] bool alive() const override { return true; }

 private:
  std::shared_ptr<SharedPipe> pipe_;
};

class FakeFactory final : public deskmate::windows::LinkFactory {
 public:
  std::unique_ptr<deskmate::windows::LinkSession> attach(
      std::unique_ptr<deskmate::windows::LinkTransport> transport,
      deskmate::platform::AoaIdentity identity,
      std::uint64_t link_id) override {
    return std::make_unique<deskmate::windows::SessionLink>(
        std::move(transport), std::move(identity), link_id, 0x12345678U, 1U,
        1920, 1200, 60, true);
  }
};

void test_link_manager_isolates_devices() {
  FakeFactory factory;
  deskmate::windows::LinkManager manager(factory);
  auto first = std::make_shared<SharedPipe>();
  auto second = std::make_shared<SharedPipe>();
  deskmate::platform::AoaIdentity identity;
  identity.product = "Tablet";
  identity.port_path = "1.1";
  const std::uint64_t a = manager.add(std::make_unique<FakeTransport>(first),
                                      identity);
  identity.port_path = "1.2";
  identity.product = "Phone";
  const std::uint64_t b = manager.add(std::make_unique<FakeTransport>(second),
                                      identity);
  CHECK(a != 0U);
  CHECK(b != 0U);
  CHECK(a != b);
  CHECK(manager.size() == 2U);
  CHECK(manager.live_ids().size() == 2U);
  manager.disconnect(a);
  CHECK(manager.size() == 1U);
  CHECK(manager.live_ids().front() == b);
  manager.stop_all();
  CHECK(manager.size() == 0U);
}

void test_host_session_link_reports_state() {
  auto pipe = std::make_shared<SharedPipe>();
  deskmate::platform::AoaIdentity identity;
  identity.product = "Android tablet";
  identity.port_path = "2.1";
  deskmate::windows::SessionLink link(std::make_unique<FakeTransport>(pipe),
                                      identity, 7U, 0xAABBCCDDU, 3U, 1920,
                                      1200, 60, true);
  CHECK(link.id() == 7U);
  CHECK(link.state() == deskmate::protocol::SessionState::AwaitingClientHello);
  CHECK(link.negotiated_summary() == "none");
  const auto stats = link.stats();
  CHECK(stats.frames_sent == 0U);
  CHECK(stats.input_events == 0U);
  link.stop();
}

// Regression: frames submitted by one link's encoder must reach only that
// link's session. The host used to broadcast every captured frame to all
// devices, which duplicated video when two tablets were attached.
void test_video_is_routed_to_a_single_link() {
  FakeFactory factory;
  deskmate::windows::LinkManager manager(factory);
  auto first_pipe = std::make_shared<SharedPipe>();
  auto second_pipe = std::make_shared<SharedPipe>();
  deskmate::platform::AoaIdentity identity;
  identity.port_path = "3.1";
  const std::uint64_t first =
      manager.add(std::make_unique<FakeTransport>(first_pipe), identity);
  identity.port_path = "3.2";
  const std::uint64_t second =
      manager.add(std::make_unique<FakeTransport>(second_pipe), identity);
  CHECK(first != 0U);
  CHECK(second != 0U);

  const auto only_first = manager.find(first);
  const auto only_second = manager.find(second);
  CHECK(only_first != nullptr);
  CHECK(only_second != nullptr);
  CHECK(only_first->id() == first);
  CHECK(only_second->id() == second);
  CHECK(manager.find(9999U) == nullptr);

  // Submitting to one link must not touch the other link's byte stream.
  only_first->submit_video(std::vector<std::uint8_t>{1, 2, 3}, 42U, true);
  manager.pump_all();
  CHECK(only_first->stats().frames_submitted == 1U);
  CHECK(only_second->stats().frames_submitted == 0U);
  manager.stop_all();
}

// A queued video frame is only handed to the session from pump(), so the
// encoder thread never races the USB pump.
void test_submit_video_does_not_touch_the_session_directly() {
  auto pipe = std::make_shared<SharedPipe>();
  deskmate::platform::AoaIdentity identity;
  identity.port_path = "4.1";
  deskmate::windows::SessionLink link(std::make_unique<FakeTransport>(pipe),
                                      identity, 11U, 0x01020304U, 2U, 1920,
                                      1200, 60, false);
  link.submit_video(std::vector<std::uint8_t>{9, 9, 9}, 5U, true);
  CHECK(link.stats().frames_submitted == 1U);
  // Still awaiting CLIENT_HELLO, so nothing can have been sent yet.
  CHECK(link.stats().frames_sent == 0U);
  CHECK(link.state() == deskmate::protocol::SessionState::AwaitingClientHello);
  link.pump();
  CHECK(link.stats().frames_sent == 0U);
  link.stop();
}

}  // namespace

int main() {
  test_link_manager_isolates_devices();
  test_host_session_link_reports_state();
  test_video_is_routed_to_a_single_link();
  test_submit_video_does_not_touch_the_session_directly();
  if (failures != 0) {
    std::fprintf(stderr, "%d AOA session test(s) failed\n", failures);
    return 1;
  }
  std::puts("all AOA session tests passed");
  return 0;
}
