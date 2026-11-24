#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gate1 {

constexpr std::size_t kFrameHeaderSize = 16;
constexpr std::size_t kMaxFramePayload = 65536;
constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::uint8_t kMagic[] = {'D', 'M', 'P', '1'};

enum class FrameType : std::uint8_t {
    kHello = 0x01,
    kHelloAck = 0x81,
    kConfig = 0x02,
    kConfigAck = 0x82,
    kStatsRequest = 0x03,
    kStatsResponse = 0x83,
    kHeartbeat = 0x04,
    kHeartbeatAck = 0x84,
    kData = 0x10,
    kDataEcho = 0x90,
    kZeroLengthAck = 0x91,
    kError = 0xEE,
};

struct Frame {
    FrameType type = FrameType::kError;
    std::uint16_t flags = 0;
    std::uint32_t seq = 0;
    std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> encode_frame(
    FrameType type,
    std::uint16_t flags,
    std::uint32_t seq,
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> make_data_payload(std::uint32_t seq, std::uint32_t size);
std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size);

class FrameDecoder {
public:
    bool push(
        const std::uint8_t* data,
        std::size_t size,
        std::vector<Frame>& frames,
        std::string& error);

    void note_zero_length_chunk();
    std::size_t zero_length_chunks() const;
    std::size_t buffered_bytes() const;
    void reset();

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t zero_length_chunks_ = 0;
};

} // namespace gate1
