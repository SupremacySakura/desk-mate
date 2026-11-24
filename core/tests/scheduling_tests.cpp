#include "deskmate/byte_io.hpp"
#include "deskmate/clock.hpp"
#include "deskmate/message_stream.hpp"
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

std::vector<std::vector<std::uint8_t>> encoded_video_frame(
    std::uint32_t seq, std::vector<std::uint8_t> payload,
    std::uint16_t flags = 0,
    std::optional<std::uint32_t> recovery_id = std::nullopt) {
  deskmate::protocol::VideoFrameToSend frame;
  frame.session_nonce = 0x11223344;
  frame.video_stream_id = 0x55667788;
  frame.seq = seq;
  frame.timestamp_us = 1000 + seq;
  frame.flags = flags;
  frame.recovery_id = recovery_id;
  if (recovery_id.has_value()) {
    frame.flags |= deskmate::protocol::kFlagRecoveryIdr;
  }
  frame.payload = std::move(payload);
  auto fragments = deskmate::protocol::encode_video_fragments(frame, 32);
  if (!fragments) {
    std::cerr << "failed to encode test video frame\n";
    std::abort();
  }
  return std::move(fragments.value());
}

std::vector<std::vector<std::uint8_t>> encoded_keyframe(
    std::uint32_t seq, std::vector<std::uint8_t> payload) {
  return encoded_video_frame(seq, std::move(payload),
                             deskmate::protocol::kFlagKeyFrame);
}

void check_encoded_keyframe(const std::vector<std::uint8_t>& bytes,
                            std::vector<std::uint8_t> payload,
                            std::uint32_t recovery_id) {
  deskmate::protocol::MessageStreamDecoder decoder(
      deskmate::protocol::StreamDecoderConfig{0x11223344, 0x55667788});
  const auto events = decoder.feed(bytes);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind),
           static_cast<int>(deskmate::protocol::StreamEventKind::VideoFrame));
  CHECK(events[0].recovery_id.has_value());
  CHECK_EQ(*events[0].recovery_id, recovery_id);
  CHECK_EQ(events[0].payload, payload);
}

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
  const auto keyframe = encoded_keyframe(99, {0x80, 0x81});
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = keyframe,
               .keyframe = true,
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK(scheduler.idr_request().has_value());
  const std::size_t bytes_before_keyframe = io.bytes.size();
  scheduler.pump();
  CHECK(!scheduler.idr_request().has_value());
  CHECK_EQ(bytes_before_keyframe, 12U);
  check_encoded_keyframe(
      std::vector<std::uint8_t>(io.bytes.begin() +
                                    static_cast<std::ptrdiff_t>(bytes_before_keyframe),
                                io.bytes.end()),
      {0x80, 0x81}, 1);
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
               .fragments = encoded_keyframe(100, {0x30}),
               .keyframe = true,
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK(scheduler.idr_request().has_value());
  scheduler.pump();
  CHECK(!scheduler.idr_request().has_value());

  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x40}}}),
           deskmate::protocol::SchedulerQueueStatus::IdrRequired);
  CHECK_EQ(scheduler.idr_request()->recovery_id, 2U);

  scheduler.request_idr_from_peer(7);
  CHECK_EQ(scheduler.idr_request()->recovery_id, 7U);
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = encoded_keyframe(101, {0x50}),
               .keyframe = true,
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK(scheduler.idr_request().has_value());
  scheduler.pump();
  CHECK(!scheduler.idr_request().has_value());

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

void test_scheduled_recovery_keyframe_marks_wire_before_clearing_request() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io;
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock, deskmate::protocol::SendSchedulerConfig{.max_video_urbs = 2});

  deskmate::protocol::VideoFrameToSend encoded;
  encoded.session_nonce = 0x11223344;
  encoded.video_stream_id = 0x55667788;
  encoded.seq = 9;
  encoded.timestamp_us = 1234;
  encoded.flags = deskmate::protocol::kFlagKeyFrame;
  encoded.payload = {0xAA, 0xBB, 0xCC};
  const auto fragments =
      deskmate::protocol::encode_video_fragments(encoded, 32);
  CHECK(fragments);

  scheduler.request_idr_from_peer(7);
  CHECK(scheduler.idr_request().has_value());
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = fragments.value(),
               .keyframe = true,
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK(scheduler.idr_request().has_value());
  CHECK_EQ(scheduler.idr_request()->recovery_id, 7U);

  scheduler.pump();
  deskmate::protocol::MessageStreamDecoder decoder(
      deskmate::protocol::StreamDecoderConfig{encoded.session_nonce,
                                              encoded.video_stream_id});
  const auto events = decoder.feed(io.bytes);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind),
           static_cast<int>(deskmate::protocol::StreamEventKind::VideoFrame));
  CHECK(events[0].recovery_id.has_value());
  CHECK_EQ(*events[0].recovery_id, 7U);
  CHECK_EQ(events[0].payload, encoded.payload);
  CHECK((events[0].header.flags & deskmate::protocol::kFlagRecoveryIdr) != 0);
  CHECK(!scheduler.idr_request().has_value());
}

void test_recovery_request_marks_already_queued_keyframe() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io;
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock, deskmate::protocol::SendSchedulerConfig{.max_video_urbs = 4});

  std::vector<std::uint8_t> payload(70);
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<std::uint8_t>(index);
  }
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = encoded_keyframe(77, payload),
               .keyframe = true,
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK(!scheduler.idr_request().has_value());

  scheduler.request_idr_from_peer(7);
  CHECK(scheduler.idr_request().has_value());
  CHECK_EQ(scheduler.idr_request()->recovery_id, 7U);
  CHECK(io.bytes.empty());

  scheduler.pump();
  deskmate::protocol::MessageStreamDecoder decoder(
      deskmate::protocol::StreamDecoderConfig{0x11223344, 0x55667788});
  const auto events = decoder.feed(io.bytes);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind),
           static_cast<int>(deskmate::protocol::StreamEventKind::VideoFrame));
  CHECK_EQ(events[0].payload, payload);
  CHECK(events[0].recovery_id.has_value());
  CHECK_EQ(*events[0].recovery_id, 7U);
  CHECK((events[0].header.flags & deskmate::protocol::kFlagRecoveryIdr) != 0);
  CHECK(!scheduler.idr_request().has_value());

  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x99}},
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
}

void test_recovery_request_drops_unstarted_p_frames() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io;
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock, deskmate::protocol::SendSchedulerConfig{.max_video_urbs = 2});

  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = encoded_video_frame(50, {0x40, 0x41}),
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  scheduler.request_idr_from_peer(7);
  CHECK(scheduler.idr_request().has_value());
  CHECK_EQ(scheduler.idr_request()->recovery_id, 7U);

  const std::vector<std::uint8_t> payload{0x50, 0x51};
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = encoded_keyframe(51, payload),
               .keyframe = true,
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK(scheduler.idr_request().has_value());

  scheduler.pump();
  deskmate::protocol::MessageStreamDecoder decoder(
      deskmate::protocol::StreamDecoderConfig{0x11223344, 0x55667788});
  const auto events = decoder.feed(io.bytes);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(events[0].payload, payload);
  CHECK(events[0].recovery_id.has_value());
  CHECK_EQ(*events[0].recovery_id, 7U);
  CHECK(!scheduler.idr_request().has_value());
}

void test_unmarkable_scheduled_keyframes_keep_recovery_request() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io;
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock, deskmate::protocol::SendSchedulerConfig{.max_video_urbs = 2});

  scheduler.request_idr_from_peer(7);
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = encoded_video_frame(
                   70, {0x70, 0x71},
                   deskmate::protocol::kFlagKeyFrame, 5),
               .keyframe = true,
               .recovery_id = 5,
           }),
           deskmate::protocol::SchedulerQueueStatus::InvalidArgument);
  CHECK(scheduler.idr_request().has_value());
  CHECK_EQ(scheduler.idr_request()->recovery_id, 7U);
  scheduler.pump();
  CHECK(io.bytes.empty());

  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x80}},
           }),
           deskmate::protocol::SchedulerQueueStatus::IdrRequired);
  CHECK(scheduler.idr_request().has_value());
  CHECK_EQ(scheduler.idr_request()->recovery_id, 7U);
}

void test_invalid_queued_keyframe_is_discarded_on_recovery() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io;
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock, deskmate::protocol::SendSchedulerConfig{.max_video_urbs = 2});

  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = encoded_video_frame(71, {0x71, 0x72}),
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x73}, {0x74}},
               .keyframe = true,
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = encoded_video_frame(72, {0x75, 0x76}),
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);

  scheduler.request_idr_from_peer(7);
  CHECK(scheduler.idr_request().has_value());
  CHECK_EQ(scheduler.idr_request()->recovery_id, 7U);
  scheduler.pump();
  CHECK(io.bytes.empty());
  CHECK(scheduler.idr_request().has_value());

  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x77}},
           }),
           deskmate::protocol::SchedulerQueueStatus::IdrRequired);
}

void test_recovery_request_does_not_drop_started_frame_fragments() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io;
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock, deskmate::protocol::SendSchedulerConfig{.max_video_urbs = 1});

  std::vector<std::uint8_t> payload(70);
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<std::uint8_t>(0x80U + index);
  }
  const auto fragments = encoded_video_frame(73, payload);
  CHECK_EQ(fragments.size(), 3U);
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = fragments,
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  scheduler.pump();
  CHECK_EQ(io.bytes, fragments[0]);

  scheduler.request_idr_from_peer(7);
  CHECK(scheduler.idr_request().has_value());
  scheduler.pump();
  scheduler.pump();

  std::vector<std::uint8_t> expected;
  for (const auto& fragment : fragments) {
    expected.insert(expected.end(), fragment.begin(), fragment.end());
  }
  CHECK_EQ(io.bytes, expected);
  CHECK(scheduler.idr_request().has_value());
  CHECK_EQ(scheduler.idr_request()->recovery_id, 7U);
}

void test_video_frame_enqueue_keeps_request_until_pump() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io;
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock, deskmate::protocol::SendSchedulerConfig{.max_video_urbs = 2});

  scheduler.request_idr_from_peer(7);
  CHECK(scheduler.idr_request().has_value());

  deskmate::protocol::VideoFrameToSend frame;
  frame.session_nonce = 0x11223344;
  frame.video_stream_id = 0x55667788;
  frame.seq = 80;
  frame.timestamp_us = 5000;
  frame.flags = deskmate::protocol::kFlagKeyFrame;
  frame.payload = {0x90, 0x91};
  CHECK_EQ(scheduler.enqueue_video_frame(std::move(frame), 32),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK(scheduler.idr_request().has_value());
  CHECK_EQ(scheduler.idr_request()->recovery_id, 7U);
  CHECK(io.bytes.empty());

  scheduler.pump();
  deskmate::protocol::MessageStreamDecoder decoder(
      deskmate::protocol::StreamDecoderConfig{0x11223344, 0x55667788});
  const auto events = decoder.feed(io.bytes);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(events[0].payload, std::vector<std::uint8_t>({0x90, 0x91}));
  CHECK(events[0].recovery_id.has_value());
  CHECK_EQ(*events[0].recovery_id, 7U);
  CHECK(!scheduler.idr_request().has_value());
}

void test_video_frame_with_other_recovery_id_keeps_request() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io;
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock, deskmate::protocol::SendSchedulerConfig{.max_video_urbs = 2});

  scheduler.request_idr_from_peer(7);
  CHECK(scheduler.idr_request().has_value());

  deskmate::protocol::VideoFrameToSend frame;
  frame.session_nonce = 0x11223344;
  frame.video_stream_id = 0x55667788;
  frame.seq = 81;
  frame.timestamp_us = 5001;
  frame.flags =
      deskmate::protocol::kFlagKeyFrame | deskmate::protocol::kFlagRecoveryIdr;
  frame.recovery_id = 5;
  frame.payload = {0x92};
  CHECK_EQ(scheduler.enqueue_video_frame(std::move(frame), 32),
           deskmate::protocol::SchedulerQueueStatus::InvalidArgument);
  CHECK(scheduler.idr_request().has_value());
  CHECK_EQ(scheduler.idr_request()->recovery_id, 7U);

  scheduler.pump();
  CHECK(io.bytes.empty());
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = {{0x93}},
           }),
           deskmate::protocol::SchedulerQueueStatus::IdrRequired);
}

void test_congestion_keeps_new_unstarted_gop_and_drops_old_unstarted_frames() {
  deskmate::protocol::ManualClock clock;
  CaptureIo io;
  deskmate::protocol::HostSendScheduler scheduler(
      io, clock,
      deskmate::protocol::SendSchedulerConfig{
          .max_video_urbs = 1,
          .max_pending_video_bytes = 80,
      });

  const std::vector<std::uint8_t> payload_a{0xA0, 0xA1};
  const std::vector<std::uint8_t> payload_c{0xC0, 0xC1};
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = encoded_keyframe(60, payload_a),
               .keyframe = true,
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  scheduler.pump();
  CHECK_EQ(io.bytes.size(), 34U);

  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = encoded_video_frame(61, {0xB0, 0xB1}),
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = encoded_keyframe(62, payload_c),
               .keyframe = true,
           }),
           deskmate::protocol::SchedulerQueueStatus::Queued);
  CHECK_EQ(scheduler.enqueue_video_frame(deskmate::protocol::ScheduledVideoFrame{
               .fragments = encoded_video_frame(63, {0xD0, 0xD1}),
           }),
           deskmate::protocol::SchedulerQueueStatus::IdrRequired);
  CHECK(scheduler.idr_request().has_value());

  scheduler.pump();
  deskmate::protocol::MessageStreamDecoder decoder(
      deskmate::protocol::StreamDecoderConfig{0x11223344, 0x55667788});
  const auto events = decoder.feed(io.bytes);
  CHECK_EQ(events.size(), 3U);
  CHECK_EQ(events[0].payload, payload_a);
  CHECK(!events[0].recovery_id.has_value());
  CHECK_EQ(static_cast<int>(events[1].kind),
           static_cast<int>(deskmate::protocol::StreamEventKind::RejectedMessage));
  CHECK_EQ(static_cast<int>(events[1].error.code),
           static_cast<int>(deskmate::protocol::ErrorCode::SequenceDiscontinuity));
  CHECK_EQ(events[2].payload, payload_c);
  CHECK(events[2].recovery_id.has_value());
  CHECK_EQ(*events[2].recovery_id, 1U);
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
      {"scheduled recovery keyframe marks wire before clearing request",
       test_scheduled_recovery_keyframe_marks_wire_before_clearing_request},
      {"recovery request marks already queued keyframe",
       test_recovery_request_marks_already_queued_keyframe},
      {"recovery request drops unstarted P frames",
       test_recovery_request_drops_unstarted_p_frames},
      {"unmarkable scheduled keyframes keep recovery request",
       test_unmarkable_scheduled_keyframes_keep_recovery_request},
      {"invalid queued keyframe is discarded on recovery",
       test_invalid_queued_keyframe_is_discarded_on_recovery},
      {"recovery request does not drop started frame fragments",
       test_recovery_request_does_not_drop_started_frame_fragments},
      {"video frame enqueue keeps request until pump",
       test_video_frame_enqueue_keeps_request_until_pump},
      {"video frame with other recovery id keeps request",
       test_video_frame_with_other_recovery_id_keeps_request},
      {"congestion keeps new unstarted GOP and drops old unstarted frames",
       test_congestion_keeps_new_unstarted_gop_and_drops_old_unstarted_frames},
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
