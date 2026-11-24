#include "deskmate/byte_io.hpp"
#include "deskmate/message_stream.hpp"
#include "deskmate/protocol.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

using deskmate::protocol::ErrorAction;
using deskmate::protocol::ErrorCode;
using deskmate::protocol::Header;
using deskmate::protocol::IoResult;
using deskmate::protocol::IoStatus;
using deskmate::protocol::MessageStreamDecoder;
using deskmate::protocol::MessageType;
using deskmate::protocol::Packet;
using deskmate::protocol::ProtocolError;
using deskmate::protocol::StreamDecoderConfig;
using deskmate::protocol::StreamEventKind;

std::string golden_directory{"testdata/golden"};

std::vector<std::uint8_t> encode_or_fail(const Packet& packet) {
  const auto encoded = deskmate::protocol::encode_packet(packet);
  if (!encoded) {
    std::cerr << "encode_packet failed: " << encoded.error().detail << '\n';
    return {};
  }
  return encoded.value();
}

Packet make_packet(MessageType type, std::uint32_t session_nonce,
                   std::uint32_t video_stream_id, std::uint32_t seq,
                   std::vector<std::uint8_t> payload, std::uint8_t fragment_index = 0,
                   std::uint8_t fragment_total = 1, std::uint16_t flags = 0,
                   std::uint64_t timestamp_us = 0) {
  Packet packet;
  packet.header.type = static_cast<std::uint8_t>(type);
  packet.header.flags = flags;
  packet.header.fragment_index = fragment_index;
  packet.header.fragment_total = fragment_total;
  packet.header.session_nonce = session_nonce;
  packet.header.video_stream_id = video_stream_id;
  packet.header.seq = seq;
  packet.header.timestamp_us = timestamp_us;
  packet.header.payload_len = static_cast<std::uint32_t>(payload.size());
  packet.payload = std::move(payload);
  return packet;
}

std::vector<std::uint8_t> concat(std::initializer_list<std::vector<std::uint8_t>> parts) {
  std::vector<std::uint8_t> result;
  for (const auto& part : parts) {
    result.insert(result.end(), part.begin(), part.end());
  }
  return result;
}

std::string_view trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return value;
}

int hex_nibble(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

bool decode_hex(std::string_view text, std::vector<std::uint8_t>& output) {
  output.clear();
  if (text == "-") {
    return true;
  }
  if (text.size() % 2 != 0) {
    return false;
  }
  output.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    const int high = hex_nibble(text[index]);
    const int low = hex_nibble(text[index + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    output.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return true;
}

std::optional<ErrorCode> parse_error_code(std::string_view name) {
  if (name == "InvalidMagic") {
    return ErrorCode::InvalidMagic;
  }
  if (name == "UnsupportedVersion") {
    return ErrorCode::UnsupportedVersion;
  }
  if (name == "InvalidFragmentation") {
    return ErrorCode::InvalidFragmentation;
  }
  if (name == "EmptyVideoFragment") {
    return ErrorCode::EmptyVideoFragment;
  }
  if (name == "InvalidVideoStreamId") {
    return ErrorCode::InvalidVideoStreamId;
  }
  if (name == "InvalidSessionNonce") {
    return ErrorCode::InvalidSessionNonce;
  }
  if (name == "InvalidFlags") {
    return ErrorCode::InvalidFlags;
  }
  if (name == "PayloadTooLarge") {
    return ErrorCode::PayloadTooLarge;
  }
  return std::nullopt;
}

class BytePipe {
 public:
  explicit BytePipe(std::size_t read_chunk_size = 1024, std::size_t write_chunk_size = 1024)
      : read_chunk_size_(read_chunk_size), write_chunk_size_(write_chunk_size) {}

  IoResult read(std::span<std::uint8_t> destination) {
    if (read_offset_ == bytes_.size()) {
      bytes_.clear();
      read_offset_ = 0;
      return {closed_ ? IoStatus::Closed : IoStatus::WouldBlock, 0};
    }
    const std::size_t available = bytes_.size() - read_offset_;
    const std::size_t amount = std::min({destination.size(), available, read_chunk_size_});
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(read_offset_), amount,
                destination.begin());
    read_offset_ += amount;
    return {IoStatus::Ok, amount};
  }

  IoResult write(std::span<const std::uint8_t> source) {
    const std::size_t amount = std::min(source.size(), write_chunk_size_);
    bytes_.insert(bytes_.end(), source.begin(),
                  source.begin() + static_cast<std::ptrdiff_t>(amount));
    return {IoStatus::Ok, amount};
  }

  void close() { closed_ = true; }

 private:
  std::vector<std::uint8_t> bytes_;
  std::size_t read_offset_{};
  std::size_t read_chunk_size_;
  std::size_t write_chunk_size_;
  bool closed_{};
};

class SimplexByteIo final : public deskmate::protocol::ByteIo {
 public:
  SimplexByteIo(BytePipe& inbound, BytePipe& outbound) : inbound_(inbound), outbound_(outbound) {}

  IoResult read(std::span<std::uint8_t> destination) override {
    return inbound_.read(destination);
  }
  IoResult write(std::span<const std::uint8_t> source) override {
    return outbound_.write(source);
  }

 private:
  BytePipe& inbound_;
  BytePipe& outbound_;
};

bool write_all(deskmate::protocol::ByteIo& io, std::span<const std::uint8_t> bytes) {
  while (!bytes.empty()) {
    const IoResult result = io.write(bytes);
    if (result.status != IoStatus::Ok || result.bytes == 0 || result.bytes > bytes.size()) {
      return false;
    }
    bytes = bytes.subspan(result.bytes);
  }
  return true;
}

struct EchoHost {
  explicit EchoHost(deskmate::protocol::ByteIo& io, StreamDecoderConfig config,
                    std::size_t read_chunk = 64)
      : io(io), decoder(config), read_chunk_size(read_chunk) {}

  bool send_control(const deskmate::protocol::ControlMessage& message) {
    const auto payload = deskmate::protocol::encode_control_payload(message);
    if (!payload) {
      return false;
    }
    Packet packet;
    packet.header.type = static_cast<std::uint8_t>(MessageType::Control);
    packet.header.session_nonce = decoder.config().expected_session_nonce;
    packet.header.seq = next_seq++;
    packet.header.payload_len = static_cast<std::uint32_t>(payload.value().size());
    packet.payload = payload.value();
    const auto encoded = deskmate::protocol::encode_packet(packet);
    return encoded &&
           write_all(io, std::span<const std::uint8_t>(encoded.value()));
  }

  bool send_video(std::span<const std::uint8_t> frame, std::uint32_t seq,
                  std::uint64_t timestamp_us, std::size_t fragment_size) {
    if (fragment_size == 0 || frame.empty()) {
      return false;
    }
    const std::size_t fragment_total =
        (frame.size() + fragment_size - 1) / fragment_size;
    if (fragment_total > deskmate::protocol::kMaxVideoFragments) {
      return false;
    }
    for (std::size_t index = 0; index < fragment_total; ++index) {
      const std::size_t begin = index * fragment_size;
      const std::size_t end = std::min(frame.size(), begin + fragment_size);
      Packet packet;
      packet.header.type = static_cast<std::uint8_t>(MessageType::Video);
      packet.header.flags = deskmate::protocol::kFlagKeyFrame;
      packet.header.fragment_index = static_cast<std::uint8_t>(index);
      packet.header.fragment_total = static_cast<std::uint8_t>(fragment_total);
      packet.header.session_nonce = decoder.config().expected_session_nonce;
      packet.header.video_stream_id = decoder.config().expected_video_stream_id;
      packet.header.seq = seq;
      packet.header.timestamp_us = timestamp_us;
      packet.payload.assign(frame.begin() + static_cast<std::ptrdiff_t>(begin),
                            frame.begin() + static_cast<std::ptrdiff_t>(end));
      packet.header.payload_len = static_cast<std::uint32_t>(packet.payload.size());
      const auto encoded = deskmate::protocol::encode_packet(packet);
      if (!encoded || !write_all(io, std::span<const std::uint8_t>(encoded.value()))) {
        return false;
      }
    }
    return true;
  }

  void pump() {
    std::vector<std::uint8_t> buffer(read_chunk_size);
    while (true) {
      const IoResult result = io.read(buffer);
      if (result.status != IoStatus::Ok || result.bytes == 0) {
        return;
      }
      auto events =
          decoder.feed(std::span<const std::uint8_t>(buffer.data(), result.bytes));
      for (auto& event : events) {
        if (event.kind == StreamEventKind::Message &&
            event.header.type == static_cast<std::uint8_t>(MessageType::Control)) {
          echo_headers.push_back(event.header);
          echo_payloads.push_back(std::move(event.payload));
        } else if (event.kind == StreamEventKind::RejectedMessage ||
                   event.kind == StreamEventKind::StreamError) {
          errors.push_back(event.error);
        }
      }
    }
  }

  deskmate::protocol::ByteIo& io;
  MessageStreamDecoder decoder;
  std::size_t read_chunk_size;
  std::uint32_t next_seq{1};
  std::vector<Header> echo_headers;
  std::vector<std::vector<std::uint8_t>> echo_payloads;
  std::vector<ProtocolError> errors;
};

struct EchoDevice {
  explicit EchoDevice(deskmate::protocol::ByteIo& io, StreamDecoderConfig config,
                      std::size_t read_chunk = 64)
      : io(io), decoder(config), read_chunk_size(read_chunk) {}

  void pump() {
    std::vector<std::uint8_t> buffer(read_chunk_size);
    while (true) {
      const IoResult result = io.read(buffer);
      if (result.status != IoStatus::Ok || result.bytes == 0) {
        return;
      }
      auto events =
          decoder.feed(std::span<const std::uint8_t>(buffer.data(), result.bytes));
      for (auto& event : events) {
        if (event.kind == StreamEventKind::Message &&
            event.header.type == static_cast<std::uint8_t>(MessageType::Control)) {
          Packet echo;
          echo.header = event.header;
          echo.payload = std::move(event.payload);
          const auto encoded = deskmate::protocol::encode_packet(echo);
          if (!encoded ||
              !write_all(io, std::span<const std::uint8_t>(encoded.value()))) {
            io_failed = true;
          }
        } else if (event.kind == StreamEventKind::VideoFrame) {
          video_headers.push_back(event.header);
          video_payloads.push_back(std::move(event.payload));
        } else if (event.kind == StreamEventKind::RejectedMessage ||
                   event.kind == StreamEventKind::StreamError) {
          errors.push_back(event.error);
        }
      }
    }
  }

  deskmate::protocol::ByteIo& io;
  MessageStreamDecoder decoder;
  std::size_t read_chunk_size;
  bool io_failed{};
  std::vector<Header> video_headers;
  std::vector<std::vector<std::uint8_t>> video_payloads;
  std::vector<ProtocolError> errors;
};

void test_header_has_explicit_little_endian_layout() {
  Header header;
  header.version = 1;
  header.type = static_cast<std::uint8_t>(MessageType::Video);
  header.flags = 0x0001;
  header.fragment_index = 1;
  header.fragment_total = 2;
  header.session_nonce = 0x11223344;
  header.video_stream_id = 0x55667788;
  header.seq = 0x99AABBCC;
  header.payload_len = 0x123;
  header.timestamp_us = 0x0102030405060708;

  const std::array<std::uint8_t, 32> expected{
      0x44, 0x4D, 0x01, 0x01, 0x01, 0x00, 0x01, 0x02,
      0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55,
      0xCC, 0xBB, 0xAA, 0x99, 0x23, 0x01, 0x00, 0x00,
      0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
  };

  CHECK_EQ(deskmate::protocol::encode_header(header), expected);
}

void test_stream_handles_split_and_coalesced_messages() {
  constexpr std::uint32_t kSession = 0x10203040;
  const auto first = encode_or_fail(
      make_packet(MessageType::Control, kSession, 0, 7, {0x01, 0x02, 0x03}));
  const auto second = encode_or_fail(
      make_packet(MessageType::Clock, kSession, 0, 8, {0xAA, 0xBB, 0xCC, 0xDD}));
  CHECK(!first.empty());
  CHECK(!second.empty());

  MessageStreamDecoder decoder(StreamDecoderConfig{kSession, 0});
  auto events = decoder.feed(std::span<const std::uint8_t>(first).first(7));
  CHECK(events.empty());
  CHECK(!decoder.failed());

  const auto rest_and_second = concat(
      {std::vector<std::uint8_t>(first.begin() + 7, first.end()), second});
  events = decoder.feed(rest_and_second);
  CHECK_EQ(events.size(), 2U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::Message));
  CHECK_EQ(events[0].header.seq, 7U);
  CHECK_EQ(events[0].payload, std::vector<std::uint8_t>({0x01, 0x02, 0x03}));
  CHECK_EQ(static_cast<int>(events[1].kind), static_cast<int>(StreamEventKind::Message));
  CHECK_EQ(events[1].header.seq, 8U);
  CHECK_EQ(events[1].payload, std::vector<std::uint8_t>({0xAA, 0xBB, 0xCC, 0xDD}));
}

void test_unsupported_audio_is_discarded_and_stream_continues() {
  constexpr std::uint32_t kSession = 0x55667788;
  Header audio_header;
  audio_header.type = static_cast<std::uint8_t>(MessageType::Audio);
  audio_header.session_nonce = kSession;
  audio_header.seq = 1;
  audio_header.payload_len = 64;
  const auto audio = deskmate::protocol::encode_header(audio_header);

  const auto control = encode_or_fail(
      make_packet(MessageType::Control, kSession, 0, 2, {0x44, 0x55}));
  MessageStreamDecoder decoder(StreamDecoderConfig{kSession, 0});
  auto events = decoder.feed(audio);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind),
           static_cast<int>(StreamEventKind::RejectedMessage));
  CHECK_EQ(static_cast<int>(events[0].error.code),
           static_cast<int>(ErrorCode::UnsupportedMessageType));
  CHECK(!decoder.failed());

  std::vector<std::uint8_t> rest(64, 0);
  rest.insert(rest.end(), control.begin(), control.end());
  events = decoder.feed(rest);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::Message));
  CHECK_EQ(events[0].payload, std::vector<std::uint8_t>({0x44, 0x55}));
  CHECK(!decoder.failed());
}

void test_invalid_header_is_rejected_before_payload_arrives() {
  Header invalid_magic;
  invalid_magic.magic = {'D', 'X'};
  invalid_magic.type = static_cast<std::uint8_t>(MessageType::Video);
  invalid_magic.session_nonce = 1;
  invalid_magic.video_stream_id = 1;
  invalid_magic.payload_len = 60000;

  MessageStreamDecoder decoder;
  auto events = decoder.feed(deskmate::protocol::encode_header(invalid_magic));
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::StreamError));
  CHECK_EQ(static_cast<int>(events[0].error.code),
           static_cast<int>(ErrorCode::InvalidMagic));
  CHECK(decoder.failed());

  Header oversized;
  oversized.type = static_cast<std::uint8_t>(MessageType::Video);
  oversized.session_nonce = 1;
  oversized.video_stream_id = 1;
  oversized.payload_len = 65537;
  MessageStreamDecoder second_decoder;
  events = second_decoder.feed(deskmate::protocol::encode_header(oversized));
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::StreamError));
  CHECK_EQ(static_cast<int>(events[0].error.code),
           static_cast<int>(ErrorCode::PayloadTooLarge));
  CHECK(second_decoder.failed());
}

void test_video_fragments_reassemble_out_of_order() {
  constexpr std::uint32_t kSession = 0x12345678;
  constexpr std::uint32_t kStream = 0x87654321;
  constexpr std::uint64_t kTimestamp = 0x0102030405060708;
  const std::vector<std::uint8_t> frame{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};

  const auto fragment_0 = encode_or_fail(make_packet(
      MessageType::Video, kSession, kStream, 10, {0x10, 0x11, 0x12}, 0, 3,
      deskmate::protocol::kFlagKeyFrame, kTimestamp));
  const auto fragment_1 = encode_or_fail(make_packet(
      MessageType::Video, kSession, kStream, 10, {0x13, 0x14}, 1, 3,
      deskmate::protocol::kFlagKeyFrame, kTimestamp));
  const auto fragment_2 = encode_or_fail(make_packet(
      MessageType::Video, kSession, kStream, 10, {0x15, 0x16, 0x17}, 2, 3,
      deskmate::protocol::kFlagKeyFrame, kTimestamp));

  MessageStreamDecoder decoder(StreamDecoderConfig{kSession, kStream});
  auto events = decoder.feed(fragment_2);
  CHECK(events.empty());
  events = decoder.feed(fragment_0);
  CHECK(events.empty());
  events = decoder.feed(fragment_1);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::VideoFrame));
  CHECK_EQ(events[0].header.seq, 10U);
  CHECK_EQ(events[0].header.timestamp_us, kTimestamp);
  CHECK_EQ(events[0].payload, frame);
  CHECK(!decoder.failed());
}

void test_video_sequence_gap_is_detected() {
  constexpr std::uint32_t kSession = 0x22223333;
  constexpr std::uint32_t kStream = 0x44445555;
  MessageStreamDecoder decoder(StreamDecoderConfig{kSession, kStream});

  auto events = decoder.feed(encode_or_fail(
      make_packet(MessageType::Video, kSession, kStream, 10, {0x01}, 0, 1,
                  deskmate::protocol::kFlagKeyFrame, 100)));
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::VideoFrame));

  events = decoder.feed(encode_or_fail(
      make_packet(MessageType::Video, kSession, kStream, 12, {0x02}, 0, 1,
                  deskmate::protocol::kFlagKeyFrame, 200)));
  CHECK_EQ(events.size(), 2U);
  CHECK_EQ(static_cast<int>(events[0].kind),
           static_cast<int>(StreamEventKind::RejectedMessage));
  CHECK_EQ(static_cast<int>(events[0].error.code),
           static_cast<int>(ErrorCode::SequenceDiscontinuity));
  CHECK_EQ(static_cast<int>(events[0].error.action),
           static_cast<int>(ErrorAction::RequestRecovery));
  CHECK_EQ(static_cast<int>(events[1].kind), static_cast<int>(StreamEventKind::VideoFrame));
}

void test_retarget_video_stream_drains_partial_candidate_packet() {
  constexpr std::uint32_t kSession = 0x22223333;
  constexpr std::uint32_t kOldStream = 0x44445555;
  constexpr std::uint32_t kNewStream = 0x66667777;
  MessageStreamDecoder decoder(StreamDecoderConfig{kSession, kNewStream});

  const auto candidate_fragment = encode_or_fail(
      make_packet(MessageType::Video, kSession, kNewStream, 20, {0x70, 0x71}, 0,
                  1, deskmate::protocol::kFlagKeyFrame, 200));
  const auto old_fragment = encode_or_fail(
      make_packet(MessageType::Video, kSession, kOldStream, 21, {0x72}, 0, 1,
                  deskmate::protocol::kFlagKeyFrame, 300));
  const auto old_next_fragment = encode_or_fail(
      make_packet(MessageType::Video, kSession, kOldStream, 22, {0x73}, 0, 1,
                  deskmate::protocol::kFlagKeyFrame, 400));

  auto events = decoder.feed(std::span<const std::uint8_t>(candidate_fragment)
                                 .first(16));
  CHECK(events.empty());
  CHECK(!decoder.failed());

  // The candidate is abandoned while only part of its header has arrived. Its
  // remaining bytes never come; the next bytes on the wire already belong to
  // the old stream. The decoder must resynchronize instead of splicing the
  // leftover candidate header onto the old keyframe.
  decoder.retarget_video_stream(kOldStream);
  events = decoder.feed(old_fragment);

  CHECK(!decoder.failed());
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(events[0].kind, StreamEventKind::VideoFrame);
  CHECK_EQ(events[0].header.video_stream_id, kOldStream);
  CHECK_EQ(events[0].header.seq, 21U);
  CHECK_EQ(events[0].header.timestamp_us, 300U);
  CHECK_EQ(events[0].payload, std::vector<std::uint8_t>({0x72}));

  events = decoder.feed(old_next_fragment);
  CHECK(!decoder.failed());
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(events[0].kind, StreamEventKind::VideoFrame);
  CHECK_EQ(events[0].header.seq, 22U);
  CHECK_EQ(events[0].payload, std::vector<std::uint8_t>({0x73}));
}

void test_host_and_device_exchange_control_echo_over_fragmented_byte_io() {
  constexpr std::uint32_t kSession = 0xA1B2C3D4;
  constexpr std::uint32_t kStream = 0x01020304;
  const std::array<std::uint8_t, 2> magic{'D', 'M'};
  BytePipe host_to_device(3, 2);
  BytePipe device_to_host(5, 1);
  SimplexByteIo host_io(device_to_host, host_to_device);
  SimplexByteIo device_io(host_to_device, device_to_host);
  EchoHost host(host_io, StreamDecoderConfig{kSession, kStream}, 3);
  EchoDevice device(device_io, StreamDecoderConfig{kSession, kStream}, 7);

  CHECK(host.send_control(
      deskmate::protocol::ControlMessage{deskmate::protocol::ControlOpcode::ClientHello,
                                         deskmate::protocol::kProtocolRevision, 0, {0xCA, 0xFE}}));
  CHECK(host.send_control(
      deskmate::protocol::ControlMessage{deskmate::protocol::ControlOpcode::Heartbeat, 0, 0,
                                         {0x01}}));
  device.pump();
  host.pump();

  CHECK(device.errors.empty());
  CHECK(host.errors.empty());
  CHECK_EQ(host.echo_payloads.size(), 2U);
  CHECK_EQ(host.echo_headers[0].magic, magic);
  CHECK_EQ(host.echo_headers[0].version, 1U);
  CHECK_EQ(host.echo_headers[0].session_nonce, kSession);
  CHECK_EQ(host.echo_headers[0].seq, 1U);
  CHECK_EQ(host.echo_headers[1].seq, 2U);
  const auto first_control =
      deskmate::protocol::decode_control_payload(host.echo_payloads[0]);
  CHECK(first_control);
  CHECK_EQ(static_cast<int>(first_control.value().opcode),
           static_cast<int>(deskmate::protocol::ControlOpcode::ClientHello));
  CHECK_EQ(first_control.value().proto_rev, deskmate::protocol::kProtocolRevision);
}

void test_test_video_payload_is_reassembled_end_to_end() {
  constexpr std::uint32_t kSession = 0x10293847;
  constexpr std::uint32_t kStream = 0x56473829;
  constexpr std::uint64_t kTimestamp = 0x0102030405060708;
  const std::array<std::uint8_t, 2> magic{'D', 'M'};
  const std::vector<std::uint8_t> frame{
      0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
  };
  BytePipe host_to_device(2, 3);
  BytePipe device_to_host(4, 2);
  SimplexByteIo host_io(device_to_host, host_to_device);
  SimplexByteIo device_io(host_to_device, device_to_host);
  EchoHost host(host_io, StreamDecoderConfig{kSession, kStream}, 2);
  EchoDevice device(device_io, StreamDecoderConfig{kSession, kStream}, 3);

  CHECK(host.send_video(frame, 42, kTimestamp, 3));
  device.pump();
  host.pump();

  CHECK(device.errors.empty());
  CHECK(host.errors.empty());
  CHECK_EQ(device.video_payloads.size(), 1U);
  CHECK_EQ(device.video_headers[0].magic, magic);
  CHECK_EQ(device.video_headers[0].session_nonce, kSession);
  CHECK_EQ(device.video_headers[0].video_stream_id, kStream);
  CHECK_EQ(device.video_headers[0].seq, 42U);
  CHECK_EQ(device.video_headers[0].timestamp_us, kTimestamp);
  CHECK_EQ(device.video_payloads[0], frame);
}

void test_header_boundaries_are_rejected_before_payload_arrives() {
  auto make_header = [](MessageType type, std::uint32_t payload_len) {
    Header header;
    header.type = static_cast<std::uint8_t>(type);
    header.session_nonce = 1;
    header.video_stream_id =
        type == MessageType::Video || type == MessageType::Input ? 2 : 0;
    header.payload_len = payload_len;
    return header;
  };

  std::vector<std::pair<Header, ErrorCode>> cases;
  {
    Header header = make_header(MessageType::Control, 0);
    header.fragment_total = 0;
    cases.emplace_back(header, ErrorCode::InvalidFragmentation);
  }
  {
    Header header = make_header(MessageType::Control, 0);
    header.fragment_index = 1;
    header.fragment_total = 1;
    cases.emplace_back(header, ErrorCode::InvalidFragmentation);
  }
  {
    Header header = make_header(MessageType::Video, 1);
    header.fragment_total = 65;
    cases.emplace_back(header, ErrorCode::InvalidFragmentation);
  }
  {
    Header header = make_header(MessageType::Control, 0);
    header.fragment_total = 2;
    cases.emplace_back(header, ErrorCode::InvalidFragmentation);
  }
  {
    Header header = make_header(MessageType::Input, 1);
    header.fragment_total = 2;
    cases.emplace_back(header, ErrorCode::InvalidFragmentation);
  }
  {
    Header header = make_header(MessageType::Clock, 32);
    header.fragment_total = 2;
    cases.emplace_back(header, ErrorCode::InvalidFragmentation);
  }
  cases.emplace_back(make_header(MessageType::Video, 0), ErrorCode::EmptyVideoFragment);
  {
    Header header = make_header(MessageType::Video, 1);
    header.video_stream_id = 0;
    cases.emplace_back(header, ErrorCode::InvalidVideoStreamId);
  }
  {
    Header header = make_header(MessageType::Input, 1);
    header.video_stream_id = 0;
    cases.emplace_back(header, ErrorCode::InvalidVideoStreamId);
  }
  {
    Header header = make_header(MessageType::Control, 0);
    header.session_nonce = 0;
    cases.emplace_back(header, ErrorCode::InvalidSessionNonce);
  }
  {
    Header header = make_header(MessageType::Video, 1);
    header.flags = 0x0004;
    cases.emplace_back(header, ErrorCode::InvalidFlags);
  }
  {
    Header header = make_header(MessageType::Control, 0);
    header.flags = deskmate::protocol::kFlagKeyFrame;
    cases.emplace_back(header, ErrorCode::InvalidFlags);
  }
  cases.emplace_back(make_header(MessageType::Control, 4097), ErrorCode::PayloadTooLarge);
  cases.emplace_back(make_header(MessageType::Video, 65537), ErrorCode::PayloadTooLarge);

  for (const auto& [header, expected] : cases) {
    MessageStreamDecoder decoder;
    const auto events = decoder.feed(deskmate::protocol::encode_header(header));
    CHECK_EQ(events.size(), 1U);
    CHECK_EQ(static_cast<int>(events[0].kind),
             static_cast<int>(StreamEventKind::StreamError));
    CHECK_EQ(static_cast<int>(events[0].error.code), static_cast<int>(expected));
    CHECK(decoder.failed());
  }
}

void test_truncated_header_waits_for_more_bytes() {
  Header header;
  header.type = static_cast<std::uint8_t>(MessageType::Control);
  header.session_nonce = 0x01020304;
  header.seq = 3;
  header.payload_len = 2;
  const auto header_bytes = deskmate::protocol::encode_header(header);
  const std::vector<std::uint8_t> packet =
      concat({std::vector<std::uint8_t>(header_bytes.begin(), header_bytes.end()),
              {0xAA, 0xBB}});

  MessageStreamDecoder decoder(StreamDecoderConfig{0x01020304, 0});
  auto events = decoder.feed(std::span<const std::uint8_t>(packet).first(31));
  CHECK(events.empty());
  CHECK(!decoder.failed());
  events = decoder.feed(std::span<const std::uint8_t>(packet).subspan(31));
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::Message));
  CHECK_EQ(events[0].payload, std::vector<std::uint8_t>({0xAA, 0xBB}));

  const auto short_header = deskmate::protocol::decode_header(
      std::span<const std::uint8_t>(header_bytes).first(31));
  CHECK(!short_header);
  CHECK_EQ(static_cast<int>(short_header.error().code),
           static_cast<int>(ErrorCode::TruncatedHeader));
}

void test_stale_session_and_stream_are_discarded_without_disconnect() {
  constexpr std::uint32_t kSession = 0x11112222;
  constexpr std::uint32_t kStream = 0x33334444;
  MessageStreamDecoder decoder(StreamDecoderConfig{kSession, kStream});

  const auto wrong_session = encode_or_fail(
      make_packet(MessageType::Control, 0x99998888, 0, 1, {0x01}));
  auto events = decoder.feed(wrong_session);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind),
           static_cast<int>(StreamEventKind::RejectedMessage));
  CHECK_EQ(static_cast<int>(events[0].error.code),
           static_cast<int>(ErrorCode::SessionMismatch));
  CHECK(!decoder.failed());

  const auto wrong_stream = encode_or_fail(
      make_packet(MessageType::Video, kSession, 0xAAAAAAAA, 7, {0x02}, 0, 1,
                  deskmate::protocol::kFlagKeyFrame, 10));
  events = decoder.feed(wrong_stream);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].error.code),
           static_cast<int>(ErrorCode::VideoStreamMismatch));
  CHECK(!decoder.failed());

  events = decoder.feed(encode_or_fail(
      make_packet(MessageType::Input, kSession, 0xBBBBBBBB, 3, {0x05})));
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].error.code),
           static_cast<int>(ErrorCode::VideoStreamMismatch));
  CHECK(!decoder.failed());

  events = decoder.feed(encode_or_fail(
      make_packet(MessageType::Control, kSession, 0, 2, {0x03})));
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::Message));

  events = decoder.feed(encode_or_fail(
      make_packet(MessageType::Video, kSession, kStream, 8, {0x04}, 0, 1,
                  deskmate::protocol::kFlagKeyFrame, 20)));
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::VideoFrame));
}

void test_unknown_message_type_is_discarded_and_stream_continues() {
  constexpr std::uint32_t kSession = 0xABCDEF01;
  Header unknown;
  unknown.type = 0x7F;
  unknown.session_nonce = kSession;
  unknown.payload_len = 3;
  MessageStreamDecoder decoder(StreamDecoderConfig{kSession, 0});
  auto events = decoder.feed(deskmate::protocol::encode_header(unknown));
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].error.code),
           static_cast<int>(ErrorCode::UnsupportedMessageType));
  CHECK(!decoder.failed());

  std::vector<std::uint8_t> rest{0x01, 0x02, 0x03};
  const auto message = encode_or_fail(
      make_packet(MessageType::Control, kSession, 0, 9, {0x55}));
  rest.insert(rest.end(), message.begin(), message.end());
  events = decoder.feed(rest);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::Message));
  CHECK_EQ(events[0].payload, std::vector<std::uint8_t>({0x55}));
}

void test_control_message_can_interleave_between_video_fragments() {
  constexpr std::uint32_t kSession = 0x0A0B0C0D;
  constexpr std::uint32_t kStream = 0x0E0F1011;
  MessageStreamDecoder decoder(StreamDecoderConfig{kSession, kStream});
  const auto fragment_0 = encode_or_fail(
      make_packet(MessageType::Video, kSession, kStream, 50, {0x10, 0x11}, 0, 2,
                  deskmate::protocol::kFlagKeyFrame, 1000));
  const auto control = encode_or_fail(
      make_packet(MessageType::Control, kSession, 0, 4, {0x20}));
  const auto fragment_1 = encode_or_fail(
      make_packet(MessageType::Video, kSession, kStream, 50, {0x12}, 1, 2,
                  deskmate::protocol::kFlagKeyFrame, 1000));

  auto events = decoder.feed(fragment_0);
  CHECK(events.empty());
  const auto control_and_fragment = concat({control, fragment_1});
  events = decoder.feed(control_and_fragment);
  CHECK_EQ(events.size(), 2U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::Message));
  CHECK_EQ(events[0].payload, std::vector<std::uint8_t>({0x20}));
  CHECK_EQ(static_cast<int>(events[1].kind), static_cast<int>(StreamEventKind::VideoFrame));
  CHECK_EQ(events[1].payload, std::vector<std::uint8_t>({0x10, 0x11, 0x12}));
}

void test_video_fragment_anomalies_are_detected_and_retired() {
  constexpr std::uint32_t kSession = 0xCAFEBABE;
  constexpr std::uint32_t kStream = 0x0BADF00D;
  MessageStreamDecoder decoder(StreamDecoderConfig{kSession, kStream});

  const auto fragment_0 = encode_or_fail(
      make_packet(MessageType::Video, kSession, kStream, 10, {0x10}, 0, 2,
                  deskmate::protocol::kFlagKeyFrame, 100));
  const auto fragment_1 = encode_or_fail(
      make_packet(MessageType::Video, kSession, kStream, 10, {0x11}, 1, 2,
                  deskmate::protocol::kFlagKeyFrame, 100));
  auto events = decoder.feed(fragment_0);
  CHECK(events.empty());
  events = decoder.feed(fragment_0);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].error.code),
           static_cast<int>(ErrorCode::DuplicateFragment));
  events = decoder.feed(fragment_1);
  CHECK(events.empty());

  const auto metadata_mismatch = encode_or_fail(
      make_packet(MessageType::Video, kSession, kStream, 11, {0x12}, 0, 2,
                  deskmate::protocol::kFlagKeyFrame, 200));
  const auto metadata_mismatch_2 = encode_or_fail(
      make_packet(MessageType::Video, kSession, kStream, 11, {0x13}, 1, 2,
                  deskmate::protocol::kFlagKeyFrame, 201));
  const auto metadata_mismatch_retry = encode_or_fail(
      make_packet(MessageType::Video, kSession, kStream, 11, {0x14}, 1, 2,
                  deskmate::protocol::kFlagKeyFrame, 200));
  events = decoder.feed(metadata_mismatch);
  CHECK(events.empty());
  events = decoder.feed(metadata_mismatch_2);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].error.code),
           static_cast<int>(ErrorCode::FragmentMetadataMismatch));
  events = decoder.feed(metadata_mismatch_retry);
  CHECK(events.empty());
}

void test_interleaved_frame_is_detected_and_new_frame_can_complete() {
  constexpr std::uint32_t kSession = 0x10101010;
  constexpr std::uint32_t kStream = 0x20202020;
  MessageStreamDecoder decoder(StreamDecoderConfig{kSession, kStream});

  const auto old_0 = encode_or_fail(
      make_packet(MessageType::Video, kSession, kStream, 20, {0x01}, 0, 2,
                  deskmate::protocol::kFlagKeyFrame, 100));
  const auto new_0 = encode_or_fail(
      make_packet(MessageType::Video, kSession, kStream, 21, {0x02}, 0, 2,
                  deskmate::protocol::kFlagKeyFrame, 200));
  const auto new_1 = encode_or_fail(
      make_packet(MessageType::Video, kSession, kStream, 21, {0x03}, 1, 2,
                  deskmate::protocol::kFlagKeyFrame, 200));

  auto events = decoder.feed(old_0);
  CHECK(events.empty());
  events = decoder.feed(new_0);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].error.code),
           static_cast<int>(ErrorCode::InterleavedFrame));
  events = decoder.feed(new_1);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::VideoFrame));
  CHECK_EQ(events[0].header.seq, 21U);
  CHECK_EQ(events[0].payload, std::vector<std::uint8_t>({0x02, 0x03}));
}

void test_recovery_idr_prefix_is_validated_and_stripped() {
  constexpr std::uint32_t kSession = 0x31415926;
  constexpr std::uint32_t kStream = 0x27182818;
  deskmate::protocol::VideoFrameToSend frame;
  frame.session_nonce = kSession;
  frame.video_stream_id = kStream;
  frame.seq = 30;
  frame.timestamp_us = 123456;
  frame.flags = deskmate::protocol::kFlagKeyFrame |
                deskmate::protocol::kFlagRecoveryIdr;
  frame.recovery_id = 0x11223344;
  frame.payload = {0xDE, 0xAD, 0xBE, 0xEF, 0x99};
  const auto fragments = deskmate::protocol::encode_video_fragments(frame, 2);
  CHECK(fragments);
  CHECK_EQ(fragments.value().size(), 5U);

  MessageStreamDecoder decoder(StreamDecoderConfig{kSession, kStream});
  std::vector<std::uint8_t> encoded;
  for (const auto& fragment : fragments.value()) {
    encoded.insert(encoded.end(), fragment.begin(), fragment.end());
  }
  const auto events = decoder.feed(encoded);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::VideoFrame));
  CHECK(events[0].recovery_id.has_value());
  CHECK_EQ(*events[0].recovery_id, 0x11223344U);
  CHECK_EQ(events[0].payload, frame.payload);

  const auto missing_prefix = encode_or_fail(
      make_packet(MessageType::Video, kSession, kStream, 31, {0x01, 0x02, 0x03}, 0, 1,
                  deskmate::protocol::kFlagKeyFrame |
                      deskmate::protocol::kFlagRecoveryIdr,
                  123));
  MessageStreamDecoder second_decoder(StreamDecoderConfig{kSession, kStream});
  const auto errors = second_decoder.feed(missing_prefix);
  CHECK_EQ(errors.size(), 1U);
  CHECK_EQ(static_cast<int>(errors[0].error.code),
           static_cast<int>(ErrorCode::RecoveryIdMissing));
}

void test_video_fragment_encoder_enforces_protocol_limits() {
  deskmate::protocol::VideoFrameToSend frame;
  frame.session_nonce = 1;
  frame.video_stream_id = 2;
  frame.seq = 3;
  frame.timestamp_us = 4;
  frame.flags = deskmate::protocol::kFlagKeyFrame;
  frame.payload = std::vector<std::uint8_t>(65, 0xAA);
  auto fragments = deskmate::protocol::encode_video_fragments(frame, 1);
  CHECK(!fragments);
  CHECK_EQ(static_cast<int>(fragments.error().code),
           static_cast<int>(ErrorCode::InvalidLocalArgument));

  frame.payload = std::vector<std::uint8_t>(64, 0xBB);
  fragments = deskmate::protocol::encode_video_fragments(frame, 1);
  CHECK(fragments);
  CHECK_EQ(fragments.value().size(), 64U);
  MessageStreamDecoder decoder(StreamDecoderConfig{1, 2});
  std::vector<std::uint8_t> encoded;
  for (const auto& fragment : fragments.value()) {
    const auto decoded = deskmate::protocol::decode_header(fragment);
    CHECK(decoded);
    CHECK_EQ(static_cast<unsigned>(decoded.value().fragment_total), 64U);
    encoded.insert(encoded.end(), fragment.begin(), fragment.end());
  }
  const auto events = decoder.feed(encoded);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::VideoFrame));
  CHECK_EQ(events[0].payload, frame.payload);

  frame.payload = {};
  CHECK(!deskmate::protocol::encode_video_fragments(frame, 1));
  frame.payload = {0x01};
  frame.video_stream_id = 0;
  CHECK(!deskmate::protocol::encode_video_fragments(frame, 1));
  frame.video_stream_id = 2;
  CHECK(!deskmate::protocol::encode_video_fragments(frame, 0));
  frame.flags = deskmate::protocol::kFlagRecoveryIdr;
  CHECK(!deskmate::protocol::encode_video_fragments(frame, 1));
  frame.recovery_id = 7;
  frame.flags = deskmate::protocol::kFlagKeyFrame;
  CHECK(!deskmate::protocol::encode_video_fragments(frame, 1));
}

void test_control_payload_round_trip_and_validation() {
  const std::array<deskmate::protocol::ControlOpcode, 11> opcodes{
      deskmate::protocol::ControlOpcode::ClientHello,
      deskmate::protocol::ControlOpcode::StreamPrepare,
      deskmate::protocol::ControlOpcode::StreamReady,
      deskmate::protocol::ControlOpcode::StreamCommit,
      deskmate::protocol::ControlOpcode::StreamCommitted,
      deskmate::protocol::ControlOpcode::Pause,
      deskmate::protocol::ControlOpcode::PauseAck,
      deskmate::protocol::ControlOpcode::Resume,
      deskmate::protocol::ControlOpcode::Heartbeat,
      deskmate::protocol::ControlOpcode::RequestIdr,
      deskmate::protocol::ControlOpcode::ProtocolError,
  };
  for (const auto opcode : opcodes) {
    deskmate::protocol::ControlMessage message;
    message.opcode = opcode;
    message.proto_rev = opcode == deskmate::protocol::ControlOpcode::ClientHello ||
                                opcode == deskmate::protocol::ControlOpcode::StreamPrepare
                            ? 9
                            : 0;
    message.config_id = 42;
    message.body = {0x01, 0x02, 0x03};
    const auto payload = deskmate::protocol::encode_control_payload(message);
    CHECK(payload);
    const auto decoded = deskmate::protocol::decode_control_payload(payload.value());
    CHECK(decoded);
    CHECK_EQ(static_cast<int>(decoded.value().opcode), static_cast<int>(opcode));
    CHECK_EQ(decoded.value().proto_rev, message.proto_rev);
    CHECK_EQ(decoded.value().config_id, message.config_id);
    CHECK_EQ(decoded.value().body, message.body);
  }

  deskmate::protocol::ControlMessage hello;
  hello.opcode = deskmate::protocol::ControlOpcode::ClientHello;
  hello.proto_rev = 0;
  CHECK(!deskmate::protocol::encode_control_payload(hello));

  hello.proto_rev = 1;
  auto encoded = deskmate::protocol::encode_control_payload(hello);
  CHECK(encoded);
  encoded.value()[1] = 1;
  const auto reserved = deskmate::protocol::decode_control_payload(encoded.value());
  CHECK(!reserved);
  CHECK_EQ(static_cast<int>(reserved.error().code),
           static_cast<int>(ErrorCode::InvalidControlPayload));

  std::vector<std::uint8_t> unknown_opcode(10, 0);
  unknown_opcode[0] = 0x7F;
  const auto unknown = deskmate::protocol::decode_control_payload(unknown_opcode);
  CHECK(!unknown);
  CHECK_EQ(static_cast<int>(unknown.error().code),
           static_cast<int>(ErrorCode::UnsupportedControlOpcode));

  hello.body.assign(4086, 0x5A);
  CHECK(deskmate::protocol::encode_control_payload(hello));
  hello.body.push_back(0x5A);
  CHECK(!deskmate::protocol::encode_control_payload(hello));

  const std::vector<std::uint8_t> oversized_payload(4097, 0);
  const auto oversized =
      deskmate::protocol::decode_control_payload(oversized_payload);
  CHECK(!oversized);
  CHECK_EQ(static_cast<int>(oversized.error().code),
           static_cast<int>(ErrorCode::PayloadTooLarge));
}

void test_payload_and_frame_length_boundaries() {
  constexpr std::uint32_t kSession = 0x12345678;
  constexpr std::uint32_t kStream = 0x87654321;

  std::vector<std::uint8_t> maximum_control(4096, 0x5A);
  const auto control_packet =
      encode_or_fail(make_packet(MessageType::Control, kSession, 0, 1, maximum_control));
  MessageStreamDecoder decoder(StreamDecoderConfig{kSession, kStream});
  auto events = decoder.feed(control_packet);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::Message));
  CHECK_EQ(events[0].payload.size(), 4096U);

  Packet oversized_control =
      make_packet(MessageType::Control, kSession, 0, 2,
                  std::vector<std::uint8_t>(4097, 0));
  const auto rejected_control = deskmate::protocol::encode_packet(oversized_control);
  CHECK(!rejected_control);
  CHECK_EQ(static_cast<int>(rejected_control.error().code),
           static_cast<int>(ErrorCode::PayloadTooLarge));

  constexpr std::size_t kMaximumFrame = 64U * 65536U;
  std::vector<std::uint8_t> maximum_frame(kMaximumFrame);
  for (std::size_t block = 0; block < 64; ++block) {
    std::fill_n(maximum_frame.begin() + static_cast<std::ptrdiff_t>(block * 65536U),
                65536U, static_cast<std::uint8_t>(block));
  }
  deskmate::protocol::VideoFrameToSend frame;
  frame.session_nonce = kSession;
  frame.video_stream_id = kStream;
  frame.seq = 3;
  frame.timestamp_us = 9;
  frame.flags = deskmate::protocol::kFlagKeyFrame;
  frame.payload = maximum_frame;
  const auto fragments = deskmate::protocol::encode_video_fragments(frame, 65536);
  CHECK(fragments);
  CHECK_EQ(fragments.value().size(), 64U);

  MessageStreamDecoder frame_decoder(StreamDecoderConfig{kSession, kStream});
  std::vector<std::uint8_t> encoded;
  encoded.reserve(kMaximumFrame + fragments.value().size() * 32U);
  for (const auto& fragment : fragments.value()) {
    encoded.insert(encoded.end(), fragment.begin(), fragment.end());
  }
  events = frame_decoder.feed(encoded);
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind), static_cast<int>(StreamEventKind::VideoFrame));
  CHECK_EQ(events[0].payload.size(), kMaximumFrame);
  CHECK_EQ(events[0].payload.front(), 0U);
  CHECK_EQ(events[0].payload.back(), 63U);

  frame.payload.push_back(0);
  const auto too_large = deskmate::protocol::encode_video_fragments(frame, 65536);
  CHECK(!too_large);
  CHECK_EQ(static_cast<int>(too_large.error().code),
           static_cast<int>(ErrorCode::PayloadTooLarge));
}

void test_golden_vectors() {
  std::ifstream manifest(golden_directory + "/manifest.txt");
  CHECK(manifest.is_open());

  std::size_t vector_count = 0;
  std::string line;
  while (std::getline(manifest, line)) {
    const std::string_view trimmed = trim(line);
    if (trimmed.empty() || trimmed.front() == '#') {
      continue;
    }

    std::array<std::string_view, 4> fields{};
    std::size_t field_index = 0;
    std::size_t field_start = 0;
    while (field_index < fields.size()) {
      const std::size_t separator = trimmed.find('|', field_start);
      if (separator == std::string_view::npos) {
        fields[field_index++] = trim(trimmed.substr(field_start));
        break;
      }
      fields[field_index++] = trim(trimmed.substr(field_start, separator - field_start));
      field_start = separator + 1;
    }
    CHECK_EQ(field_index, fields.size());

    std::vector<std::uint8_t> packet;
    std::vector<std::uint8_t> expected_payload;
    CHECK(decode_hex(fields[3], packet));
    CHECK(decode_hex(fields[2], expected_payload));

    MessageStreamDecoder decoder;
    std::vector<deskmate::protocol::StreamEvent> events;
    for (const std::uint8_t byte : packet) {
      auto received = decoder.feed(std::span<const std::uint8_t>(&byte, 1));
      for (auto& event : received) {
        events.push_back(std::move(event));
      }
    }

    ++vector_count;
    const std::string_view expectation = fields[1];
    if (expectation == "message") {
      CHECK(!decoder.failed());
      CHECK_EQ(events.size(), 1U);
      CHECK_EQ(static_cast<int>(events[0].kind),
               static_cast<int>(StreamEventKind::Message));
      CHECK_EQ(events[0].payload, expected_payload);
    } else if (expectation == "video") {
      CHECK(!decoder.failed());
      CHECK_EQ(events.size(), 1U);
      CHECK_EQ(static_cast<int>(events[0].kind),
               static_cast<int>(StreamEventKind::VideoFrame));
      CHECK_EQ(events[0].payload, expected_payload);
    } else if (expectation == "unsupported") {
      CHECK(!decoder.failed());
      CHECK_EQ(events.size(), 1U);
      CHECK_EQ(static_cast<int>(events[0].kind),
               static_cast<int>(StreamEventKind::RejectedMessage));
      CHECK_EQ(static_cast<int>(events[0].error.code),
               static_cast<int>(ErrorCode::UnsupportedMessageType));
    } else if (expectation == "incomplete") {
      CHECK(!decoder.failed());
      CHECK(events.empty());
    } else if (expectation.starts_with("error:")) {
      const auto expected_code = parse_error_code(expectation.substr(6));
      CHECK(expected_code.has_value());
      CHECK(decoder.failed());
      CHECK_EQ(events.size(), 1U);
      CHECK_EQ(static_cast<int>(events[0].kind),
               static_cast<int>(StreamEventKind::StreamError));
      CHECK_EQ(static_cast<int>(events[0].error.code),
               static_cast<int>(*expected_code));
    } else {
      CHECK(false);
    }
  }
  CHECK(vector_count >= 20U);
}

struct TestCase {
  const char* name;
  void (*run)();
};

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    golden_directory = argv[1];
  }
  const std::vector<TestCase> tests{
      {"header has explicit little-endian layout", test_header_has_explicit_little_endian_layout},
      {"stream handles split and coalesced messages", test_stream_handles_split_and_coalesced_messages},
      {"unsupported audio is discarded and stream continues",
       test_unsupported_audio_is_discarded_and_stream_continues},
      {"invalid header is rejected before payload arrives",
       test_invalid_header_is_rejected_before_payload_arrives},
      {"video fragments reassemble out of order", test_video_fragments_reassemble_out_of_order},
      {"video sequence gap is detected", test_video_sequence_gap_is_detected},
      {"retarget video stream drains partial candidate packet",
       test_retarget_video_stream_drains_partial_candidate_packet},
      {"host and device exchange control echo over fragmented byte I/O",
       test_host_and_device_exchange_control_echo_over_fragmented_byte_io},
      {"test video payload is reassembled end to end",
       test_test_video_payload_is_reassembled_end_to_end},
      {"header boundaries are rejected before payload arrives",
       test_header_boundaries_are_rejected_before_payload_arrives},
      {"truncated header waits for more bytes", test_truncated_header_waits_for_more_bytes},
      {"stale session and stream are discarded without disconnect",
       test_stale_session_and_stream_are_discarded_without_disconnect},
      {"control message can interleave between video fragments",
       test_control_message_can_interleave_between_video_fragments},
      {"unknown message type is discarded and stream continues",
       test_unknown_message_type_is_discarded_and_stream_continues},
      {"video fragment anomalies are detected and retired",
       test_video_fragment_anomalies_are_detected_and_retired},
      {"interleaved frame is detected and new frame can complete",
       test_interleaved_frame_is_detected_and_new_frame_can_complete},
      {"recovery IDR prefix is validated and stripped",
       test_recovery_idr_prefix_is_validated_and_stripped},
      {"video fragment encoder enforces protocol limits",
       test_video_fragment_encoder_enforces_protocol_limits},
      {"control payload round trip and validation",
       test_control_payload_round_trip_and_validation},
      {"payload and frame length boundaries", test_payload_and_frame_length_boundaries},
      {"golden vectors", test_golden_vectors},
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
  std::cout << "All tests passed\n";
  return 0;
}
