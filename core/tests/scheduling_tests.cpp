#include "deskmate/byte_io.hpp"
#include "deskmate/clock.hpp"
#include "deskmate/scheduler.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n'; \
      ++failures;                                                                         \
      return;                                                                             \
    }                                                                                     \
  } while (false)

#define CHECK_EQ(actual, expected)                                                       \
  do {                                                                                   \
    const auto& actual_value = (actual);                                                 \
    const auto& expected_value = (expected);                                             \
    if (!(actual_value == expected_value)) {                                             \
      std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK_EQ failed: " #actual          \
                << " != " #expected << '\n';                                             \
      ++failures;                                                                        \
      return;                                                                            \
    }                                                                                    \
  } while (false)

class CaptureIo final : public deskmate::protocol::ByteIo {
 public:
  explicit CaptureIo(std::size_t write_chunk_size = 1024)
      : write_chunk_size_(write_chunk_size) {}

  deskmate::protocol::IoResult read(
      std::span<std::uint8_t>) override {
    return {deskmate::protocol::IoStatus::WouldBlock, 0};
  }

  deskmate::protocol::IoResult write(
      std::span<const std::uint8_t> source) override {
    if (remaining_writes == 0) {
      return {deskmate::protocol::IoStatus::WouldBlock, 0};
    }
    if (remaining_writes != std::numeric_limits<std::size_t>::max()) {
      --remaining_writes;
    }
    const std::size_t amount = std::min(source.size(), write_chunk_size_);
    bytes.insert(bytes.end(), source.begin(),
                 source.begin() + static_cast<std::ptrdiff_t>(amount));
    return {deskmate::protocol::IoStatus::Ok, amount};
  }

  std::vector<std::uint8_t> bytes;
  std::size_t remaining_writes{std::numeric_limits<std::size_t>::max()};

 private:
  std::size_t write_chunk_size_;
};

void test_priority_message_jumps_between_video_fragments() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io;
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock, deskmate::protocol::SendSchedulerConfig{.max_video_urbs = 2});

  CHECK_EQ(scheduler.enqueue_video_frame(
               {{0x10}, {0x11}, {0x12}, {0x13}, {0x14}}),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  scheduler.pump();
  CHECK_EQ(io.bytes, std::vector<std::uint8_t>({0x10, 0x11}));

  scheduler.enqueue_priority({0xE0, 0xE1});
  scheduler.pump();
  CHECK_EQ(io.bytes,
           std::vector<std::uint8_t>({0x10, 0x11, 0xE0, 0xE1, 0x12, 0x13}));
}

void test_pending_bytes_drop_only_unstarted_frames_and_request_idr() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io;
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock,
      deskmate::protocol::SendSchedulerConfig{
          .max_video_urbs = 1,
          .max_pending_video_bytes = 20,
      });

  CHECK_EQ(scheduler.enqueue_video_frame(
               deskmate::protocol::ScheduledVideoFrame{
                   .fragments = {{0x10, 0x11, 0x12, 0x13},
                                 {0x20, 0x21, 0x22, 0x23},
                                 {0x30, 0x31, 0x32, 0x33}},
                   .keyframe = true,
               }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  scheduler.pump();
  CHECK_EQ(io.bytes,
           std::vector<std::uint8_t>({0x10, 0x11, 0x12, 0x13}));
  CHECK(!scheduler.idr_request().has_value());

  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x40, 0x41, 0x42, 0x43},
                             {0x50, 0x51, 0x52, 0x53}}}),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x60, 0x61, 0x62, 0x63},
                             {0x70, 0x71, 0x72, 0x73}}}),
           deskmate::protocol::SchedulerQueueStatus::IdrRequired);
  CHECK(scheduler.idr_request().has_value());
  CHECK_EQ(scheduler.idr_request()->recovery_id, 1U);

  scheduler.pump();
  scheduler.pump();
  CHECK_EQ(io.bytes,
           std::vector<std::uint8_t>({0x10, 0x11, 0x12, 0x13,
                                      0x20, 0x21, 0x22, 0x23,
                                      0x30, 0x31, 0x32, 0x33}));
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x80, 0x81}},
               .keyframe = true,
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK(!scheduler.idr_request().has_value());
  scheduler.pump();
  CHECK_EQ(io.bytes,
           std::vector<std::uint8_t>({0x10, 0x11, 0x12, 0x13,
                                      0x20, 0x21, 0x22, 0x23,
                                      0x30, 0x31, 0x32, 0x33,
                                      0x80, 0x81}));
}

void test_oldest_unsent_frame_age_drops_unstarted_frames() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io;
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock,
      deskmate::protocol::SendSchedulerConfig{
          .max_video_urbs = 1,
          .max_oldest_unsent_frame_age_ns = 10'000'000,
      });

  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x10}, {0x11}},
               .keyframe = true,
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  scheduler.pump();
  CHECK_EQ(io.bytes, std::vector<std::uint8_t>({0x10}));
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x20}, {0x21}},
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);

  clock.advance_ns(10'000'000);
  scheduler.pump();
  CHECK_EQ(io.bytes, std::vector<std::uint8_t>({0x10, 0x11}));
  CHECK(scheduler.idr_request().has_value());
  CHECK_EQ(scheduler.idr_request()->recovery_id, 1U);

  scheduler.pump();
  CHECK_EQ(io.bytes, std::vector<std::uint8_t>({0x10, 0x11}));
}

void test_recovery_id_is_stable_within_a_cycle_and_increments_after_idr() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io;
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock,
      deskmate::protocol::SendSchedulerConfig{
          .max_video_urbs = 1,
          .max_pending_video_bytes = 0,
      });

  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x10}}}),
           deskmate::protocol::SchedulerQueueStatus::IdrRequired);
  CHECK(scheduler.idr_request().has_value());
  CHECK_EQ(scheduler.idr_request()->recovery_id, 1U);

  clock.advance_ns(deskmate::protocol::kDefaultRecoveryRetryNs);
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x20}}}),
           deskmate::protocol::SchedulerQueueStatus::IdrRequired);
  CHECK_EQ(scheduler.idr_request()->recovery_id, 1U);

  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x30}},
               .keyframe = true,
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK(!scheduler.idr_request().has_value());
  scheduler.pump();

  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x40}}}),
           deskmate::protocol::SchedulerQueueStatus::IdrRequired);
  CHECK_EQ(scheduler.idr_request()->recovery_id, 2U);

  scheduler.request_idr_from_peer(7);
  CHECK_EQ(scheduler.idr_request()->recovery_id, 7U);
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x50}},
               .keyframe = true,
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK(!scheduler.idr_request().has_value());
  scheduler.pump();

  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x60}}}),
           deskmate::protocol::SchedulerQueueStatus::IdrRequired);
  CHECK_EQ(scheduler.idr_request()->recovery_id, 8U);
}

void test_priority_flush_finishes_active_fragment_without_starting_video() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io(2);
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock, deskmate::protocol::SendSchedulerConfig{.max_video_urbs = 2});

  io.remaining_writes = 1;
  CHECK_EQ(scheduler.enqueue_video_frame(
               {{0x10, 0x11, 0x12, 0x13}, {0x20}}),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  scheduler.pump();
  CHECK_EQ(io.bytes, std::vector<std::uint8_t>({0x10, 0x11}));

  io.remaining_writes = std::numeric_limits<std::size_t>::max();
  scheduler.enqueue_priority({0xE0});
  scheduler.pump_priority();
  CHECK_EQ(io.bytes,
           std::vector<std::uint8_t>({0x10, 0x11, 0x12, 0x13, 0xE0}));
  scheduler.pump();
  CHECK_EQ(io.bytes,
           std::vector<std::uint8_t>({0x10, 0x11, 0x12, 0x13, 0xE0, 0x20}));
}

void test_stopping_discards_active_and_queued_work_immediately() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io(2);
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock, deskmate::protocol::SendSchedulerConfig{.max_video_urbs = 2});

  io.remaining_writes = 1;
  CHECK_EQ(scheduler.enqueue_video_frame(
               {{0x10, 0x11, 0x12}, {0x20, 0x21}}),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  scheduler.pump();
  CHECK_EQ(io.bytes, std::vector<std::uint8_t>({0x10, 0x11}));

  scheduler.begin_stopping();
  CHECK(scheduler.stopping());
  CHECK(scheduler.idle());
  scheduler.pump();
  CHECK_EQ(io.bytes, std::vector<std::uint8_t>({0x10, 0x11}));

  scheduler.enqueue_priority({0xE0});
  CHECK_EQ(scheduler.enqueue_video_frame({{0x30}}),
           deskmate::protocol::SchedulerQueueStatus::Stopped);
  scheduler.pump();
  CHECK_EQ(io.bytes, std::vector<std::uint8_t>({0x10, 0x11}));
}

struct TestCase {
  const char* name;
  void (*run)();
};

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"priority message jumps between video fragments",
       test_priority_message_jumps_between_video_fragments},
      {"pending bytes drop only unstarted frames and request IDR",
       test_pending_bytes_drop_only_unstarted_frames_and_request_idr},
      {"oldest unsent frame age drops unstarted frames",
       test_oldest_unsent_frame_age_drops_unstarted_frames},
      {"recovery id stays stable per cycle and increments after IDR",
       test_recovery_id_is_stable_within_a_cycle_and_increments_after_idr},
      {"priority flush finishes active fragment without starting video",
       test_priority_flush_finishes_active_fragment_without_starting_video},
      {"stopping discards active and queued work immediately",
       test_stopping_discards_active_and_queued_work_immediately},
  };
  for (const auto& test : tests) {
    const int before = failures;
    test.run();
    std::cout << (failures == before ? "PASS " : "FAIL ") << test.name << '\n';
  }
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return 1;
  }
  std::cout << "All scheduling tests passed\n";
  return 0;
}
