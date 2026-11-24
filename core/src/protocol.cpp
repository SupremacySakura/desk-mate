#include "deskmate/protocol.hpp"

#include <algorithm>

namespace deskmate::protocol {
namespace {

void write_u16(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
  bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void write_u32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
  }
}

void write_u64(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
  }
}

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(bytes[offset]) |
      (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U));
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
  }
  return value;
}

std::uint64_t read_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
  }
  return value;
}

ProtocolError error(ErrorCode code, ErrorAction action, std::string_view detail) {
  return ProtocolError{code, action, detail};
}

bool is_known_control_opcode(std::uint8_t value) {
  return value >= static_cast<std::uint8_t>(ControlOpcode::ClientHello) &&
         value <= static_cast<std::uint8_t>(ControlOpcode::ProtocolError);
}

}  // namespace

std::array<std::uint8_t, kWireHeaderSize> encode_header(const Header& header) {
  std::array<std::uint8_t, kWireHeaderSize> bytes{};
  const std::span<std::uint8_t> out(bytes);
  out[0] = header.magic[0];
  out[1] = header.magic[1];
  out[2] = header.version;
  out[3] = header.type;
  write_u16(out, 4, header.flags);
  out[6] = header.fragment_index;
  out[7] = header.fragment_total;
  write_u32(out, 8, header.session_nonce);
  write_u32(out, 12, header.video_stream_id);
  write_u32(out, 16, header.seq);
  write_u32(out, 20, header.payload_len);
  write_u64(out, 24, header.timestamp_us);
  return bytes;
}

Result<Header> decode_header(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kWireHeaderSize) {
    return Result<Header>::failure(
        error(ErrorCode::TruncatedHeader, ErrorAction::ResetStream, "header is shorter than 32 bytes"));
  }

  Header header;
  header.magic = {bytes[0], bytes[1]};
  header.version = bytes[2];
  header.type = bytes[3];
  header.flags = read_u16(bytes, 4);
  header.fragment_index = bytes[6];
  header.fragment_total = bytes[7];
  header.session_nonce = read_u32(bytes, 8);
  header.video_stream_id = read_u32(bytes, 12);
  header.seq = read_u32(bytes, 16);
  header.payload_len = read_u32(bytes, 20);
  header.timestamp_us = read_u64(bytes, 24);

  if (const auto validation = validate_header_structure(header); validation.has_value()) {
    return Result<Header>::failure(*validation);
  }
  return Result<Header>::success(header);
}

Result<std::vector<std::uint8_t>> encode_packet(const Packet& packet) {
  const Header& header = packet.header;
  if (!is_supported_message_type(header.type)) {
    return Result<std::vector<std::uint8_t>>::failure(error(
        ErrorCode::UnsupportedMessageType, ErrorAction::RejectLocalArgument,
        "message type is not supported by this V1 implementation"));
  }
  if (header.payload_len != packet.payload.size()) {
    return Result<std::vector<std::uint8_t>>::failure(error(
        ErrorCode::InvalidLocalArgument, ErrorAction::RejectLocalArgument,
        "payload_len does not match the encoded payload"));
  }
  if (const auto validation = validate_header_structure(header); validation.has_value()) {
    return Result<std::vector<std::uint8_t>>::failure(*validation);
  }

  const auto encoded_header = encode_header(header);
  std::vector<std::uint8_t> bytes;
  bytes.reserve(encoded_header.size() + packet.payload.size());
  bytes.insert(bytes.end(), encoded_header.begin(), encoded_header.end());
  bytes.insert(bytes.end(), packet.payload.begin(), packet.payload.end());
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

Result<std::vector<std::vector<std::uint8_t>>> encode_video_fragments(
    const VideoFrameToSend& frame, std::size_t max_fragment_payload) {
  if (frame.payload.empty()) {
    return Result<std::vector<std::vector<std::uint8_t>>>::failure(
        error(ErrorCode::InvalidLocalArgument, ErrorAction::RejectLocalArgument,
              "video frame payload must not be empty"));
  }
  if (frame.session_nonce == 0 || frame.video_stream_id == 0) {
    return Result<std::vector<std::vector<std::uint8_t>>>::failure(
        error(ErrorCode::InvalidLocalArgument, ErrorAction::RejectLocalArgument,
              "video fragments require non-zero session_nonce and video_stream_id"));
  }
  if (max_fragment_payload == 0 || max_fragment_payload > kMaxPacketPayload) {
    return Result<std::vector<std::vector<std::uint8_t>>>::failure(
        error(ErrorCode::InvalidLocalArgument, ErrorAction::RejectLocalArgument,
              "fragment size must be in the range 1..65536"));
  }
  if ((frame.flags &
       static_cast<std::uint16_t>(~(kFlagKeyFrame | kFlagRecoveryIdr))) != 0) {
    return Result<std::vector<std::vector<std::uint8_t>>>::failure(
        error(ErrorCode::InvalidLocalArgument, ErrorAction::RejectLocalArgument,
              "video frame contains unknown flags"));
  }
  if (((frame.flags & kFlagRecoveryIdr) != 0) != frame.recovery_id.has_value()) {
    return Result<std::vector<std::vector<std::uint8_t>>>::failure(
        error(ErrorCode::InvalidLocalArgument, ErrorAction::RejectLocalArgument,
              "recovery_id and the recovery IDR flag must be provided together"));
  }

  const std::size_t prefix_size = frame.recovery_id.has_value() ? 4 : 0;
  if (frame.payload.size() > kMaxVideoFramePayload - prefix_size) {
    return Result<std::vector<std::vector<std::uint8_t>>>::failure(
        error(ErrorCode::PayloadTooLarge, ErrorAction::RejectLocalArgument,
              "video frame payload exceeds 4 MiB"));
  }
  const std::size_t wire_size = prefix_size + frame.payload.size();
  const std::size_t fragment_total =
      (wire_size + max_fragment_payload - 1) / max_fragment_payload;
  if (fragment_total > kMaxVideoFragments) {
    return Result<std::vector<std::vector<std::uint8_t>>>::failure(
        error(ErrorCode::InvalidLocalArgument, ErrorAction::RejectLocalArgument,
              "video frame requires more than 64 fragments"));
  }

  std::vector<std::uint8_t> wire_payload;
  wire_payload.reserve(wire_size);
  if (frame.recovery_id.has_value()) {
    for (std::size_t index = 0; index < 4; ++index) {
      wire_payload.push_back(
          static_cast<std::uint8_t>((*frame.recovery_id >> (index * 8U)) & 0xFFU));
    }
  }
  wire_payload.insert(wire_payload.end(), frame.payload.begin(), frame.payload.end());

  std::vector<std::vector<std::uint8_t>> fragments;
  fragments.reserve(fragment_total);
  for (std::size_t index = 0; index < fragment_total; ++index) {
    const std::size_t begin = index * max_fragment_payload;
    const std::size_t end = std::min(wire_size, begin + max_fragment_payload);
    Packet packet;
    packet.header.type = static_cast<std::uint8_t>(MessageType::Video);
    packet.header.flags = frame.flags;
    packet.header.fragment_index = static_cast<std::uint8_t>(index);
    packet.header.fragment_total = static_cast<std::uint8_t>(fragment_total);
    packet.header.session_nonce = frame.session_nonce;
    packet.header.video_stream_id = frame.video_stream_id;
    packet.header.seq = frame.seq;
    packet.header.timestamp_us = frame.timestamp_us;
    packet.payload.assign(
        wire_payload.begin() + static_cast<std::ptrdiff_t>(begin),
        wire_payload.begin() + static_cast<std::ptrdiff_t>(end));
    packet.header.payload_len = static_cast<std::uint32_t>(packet.payload.size());

    auto encoded = encode_packet(packet);
    if (!encoded) {
      return Result<std::vector<std::vector<std::uint8_t>>>::failure(encoded.error());
    }
    fragments.push_back(std::move(encoded.value()));
  }
  return Result<std::vector<std::vector<std::uint8_t>>>::success(std::move(fragments));
}

Result<std::vector<std::uint8_t>> encode_control_payload(const ControlMessage& message) {
  const auto opcode = static_cast<std::uint8_t>(message.opcode);
  if (!is_known_control_opcode(opcode)) {
    return Result<std::vector<std::uint8_t>>::failure(
        error(ErrorCode::UnsupportedControlOpcode, ErrorAction::RejectLocalArgument,
              "control opcode is not defined in protocol revision 1"));
  }
  if ((message.opcode == ControlOpcode::ClientHello ||
       message.opcode == ControlOpcode::StreamPrepare) &&
      message.proto_rev == 0) {
    return Result<std::vector<std::uint8_t>>::failure(
        error(ErrorCode::InvalidLocalArgument, ErrorAction::RejectLocalArgument,
              "CLIENT_HELLO and STREAM_PREPARE require a non-zero proto_rev"));
  }

  constexpr std::size_t kControlPrefixSize = 10;
  if (message.body.size() > kMaxNonVideoPayload - kControlPrefixSize) {
    return Result<std::vector<std::uint8_t>>::failure(
        error(ErrorCode::PayloadTooLarge, ErrorAction::RejectLocalArgument,
              "control payload exceeds 4096 bytes"));
  }

  std::vector<std::uint8_t> payload(kControlPrefixSize);
  const std::span<std::uint8_t> out(payload);
  out[0] = opcode;
  out[1] = 0;
  write_u32(out, 2, message.proto_rev);
  write_u32(out, 6, message.config_id);
  payload.insert(payload.end(), message.body.begin(), message.body.end());
  return Result<std::vector<std::uint8_t>>::success(std::move(payload));
}

Result<ControlMessage> decode_control_payload(std::span<const std::uint8_t> payload) {
  constexpr std::size_t kControlPrefixSize = 10;
  if (payload.size() > kMaxNonVideoPayload) {
    return Result<ControlMessage>::failure(
        error(ErrorCode::PayloadTooLarge, ErrorAction::DiscardMessage,
              "control payload exceeds 4096 bytes"));
  }
  if (payload.size() < kControlPrefixSize) {
    return Result<ControlMessage>::failure(
        error(ErrorCode::InvalidControlPayload, ErrorAction::DiscardMessage,
              "control payload is shorter than its fixed prefix"));
  }
  if (payload[1] != 0) {
    return Result<ControlMessage>::failure(
        error(ErrorCode::InvalidControlPayload, ErrorAction::DiscardMessage,
              "reserved control byte must be zero"));
  }
  if (!is_known_control_opcode(payload[0])) {
    return Result<ControlMessage>::failure(
        error(ErrorCode::UnsupportedControlOpcode, ErrorAction::DiscardMessage,
              "control opcode is not defined in protocol revision 1"));
  }

  ControlMessage message;
  message.opcode = static_cast<ControlOpcode>(payload[0]);
  message.proto_rev = read_u32(payload, 2);
  message.config_id = read_u32(payload, 6);
  message.body.assign(payload.begin() + static_cast<std::ptrdiff_t>(kControlPrefixSize),
                      payload.end());

  if ((message.opcode == ControlOpcode::ClientHello ||
       message.opcode == ControlOpcode::StreamPrepare) &&
      message.proto_rev == 0) {
    return Result<ControlMessage>::failure(
        error(ErrorCode::InvalidControlPayload, ErrorAction::DiscardMessage,
              "CLIENT_HELLO and STREAM_PREPARE require a non-zero proto_rev"));
  }
  return Result<ControlMessage>::success(std::move(message));
}

bool is_supported_message_type(std::uint8_t type) {
  return type == static_cast<std::uint8_t>(MessageType::Video) ||
         type == static_cast<std::uint8_t>(MessageType::Input) ||
         type == static_cast<std::uint8_t>(MessageType::Control) ||
         type == static_cast<std::uint8_t>(MessageType::Clock);
}

bool is_session_level_message(std::uint8_t type) {
  return type == static_cast<std::uint8_t>(MessageType::Control) ||
         type == static_cast<std::uint8_t>(MessageType::Clock);
}

std::optional<ProtocolError> validate_header_structure(const Header& header) {
  if (header.magic[0] != 'D' || header.magic[1] != 'M') {
    return error(ErrorCode::InvalidMagic, ErrorAction::ResetStream,
                 "wire magic must be the two bytes 0x44 0x4D");
  }
  if (header.version != kWireVersion) {
    return error(ErrorCode::UnsupportedVersion, ErrorAction::ResetStream,
                 "wire version is not supported");
  }
  if (header.payload_len > kMaxPacketPayload) {
    return error(ErrorCode::PayloadTooLarge, ErrorAction::ResetStream,
                 "packet payload exceeds 65536 bytes");
  }
  if (header.fragment_total == 0 || header.fragment_total > kMaxVideoFragments) {
    return error(ErrorCode::InvalidFragmentation, ErrorAction::ResetStream,
                 "fragment_total must be in the range 1..64");
  }
  if (header.fragment_index >= header.fragment_total) {
    return error(ErrorCode::InvalidFragmentation, ErrorAction::ResetStream,
                 "fragment_index must be smaller than fragment_total");
  }
  if (header.session_nonce == 0) {
    return error(ErrorCode::InvalidSessionNonce, ErrorAction::ResetStream,
                 "session_nonce must be non-zero");
  }

  if (!is_supported_message_type(header.type)) {
    return std::nullopt;
  }

  const bool is_video = header.type == static_cast<std::uint8_t>(MessageType::Video);
  if (is_video) {
    if ((header.flags & static_cast<std::uint16_t>(~(
                              kFlagKeyFrame | kFlagRecoveryIdr))) != 0) {
      return error(ErrorCode::InvalidFlags, ErrorAction::ResetStream,
                   "video packet contains unknown flags");
    }
    if (header.payload_len == 0) {
      return error(ErrorCode::EmptyVideoFragment, ErrorAction::ResetStream,
                   "video fragments must contain at least one byte");
    }
    if (header.video_stream_id == 0) {
      return error(ErrorCode::InvalidVideoStreamId, ErrorAction::ResetStream,
                   "video packets require a non-zero video_stream_id");
    }
    return std::nullopt;
  }

  if (header.fragment_total != 1 || header.fragment_index != 0) {
    return error(ErrorCode::InvalidFragmentation, ErrorAction::ResetStream,
                 "control, input, clock, and audio packets cannot be fragmented");
  }
  if (header.flags != 0) {
    return error(ErrorCode::InvalidFlags, ErrorAction::ResetStream,
                 "non-video packets must use zero flags");
  }
  if (header.payload_len > kMaxNonVideoPayload) {
    return error(ErrorCode::PayloadTooLarge, ErrorAction::ResetStream,
                 "non-video payload exceeds 4096 bytes");
  }
  if ((header.type == static_cast<std::uint8_t>(MessageType::Input)) &&
      header.video_stream_id == 0) {
    return error(ErrorCode::InvalidVideoStreamId, ErrorAction::ResetStream,
                 "input packets require a non-zero video_stream_id");
  }
  return std::nullopt;
}

}  // namespace deskmate::protocol
