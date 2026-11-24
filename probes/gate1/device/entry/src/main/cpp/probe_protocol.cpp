#include "probe_protocol.hpp"

#include <algorithm>
#include <cstring>

namespace gate1 {
namespace {

std::uint16_t read_u16_le(const std::uint8_t* data)
{
    return static_cast<std::uint16_t>(data[0]) |
           static_cast<std::uint16_t>(data[1]) << 8;
}

std::uint32_t read_u32_le(const std::uint8_t* data)
{
    return static_cast<std::uint32_t>(data[0]) |
           static_cast<std::uint32_t>(data[1]) << 8 |
           static_cast<std::uint32_t>(data[2]) << 16 |
           static_cast<std::uint32_t>(data[3]) << 24;
}

void append_u16_le(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void append_u32_le(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

} // namespace

std::vector<std::uint8_t> encode_frame(
    FrameType type,
    std::uint16_t flags,
    std::uint32_t seq,
    const std::vector<std::uint8_t>& payload)
{
    if (payload.size() > kMaxFramePayload) {
        return {};
    }
    std::vector<std::uint8_t> encoded;
    encoded.reserve(kFrameHeaderSize + payload.size());
    encoded.insert(encoded.end(), std::begin(kMagic), std::end(kMagic));
    encoded.push_back(kProtocolVersion);
    encoded.push_back(static_cast<std::uint8_t>(type));
    append_u16_le(encoded, flags);
    append_u32_le(encoded, seq);
    append_u32_le(encoded, static_cast<std::uint32_t>(payload.size()));
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    return encoded;
}

std::vector<std::uint8_t> make_data_payload(std::uint32_t seq, std::uint32_t size)
{
    std::vector<std::uint8_t> payload;
    payload.reserve(size);
    const std::uint8_t seed = static_cast<std::uint8_t>((seq * 31U) & 0xFFU);
    for (std::uint32_t i = 0; i < size; ++i) {
        payload.push_back(static_cast<std::uint8_t>(
            seed ^ static_cast<std::uint8_t>((i * 17U) & 0xFFU) ^
            static_cast<std::uint8_t>((i >> 3) & 0xFFU)));
    }
    return payload;
}

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size)
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

bool FrameDecoder::push(
    const std::uint8_t* data,
    std::size_t size,
    std::vector<Frame>& frames,
    std::string& error)
{
    error.clear();
    if (size == 0) {
        return true;
    }
    if (data == nullptr) {
        error = "null data with non-zero size";
        return false;
    }
    buffer_.insert(buffer_.end(), data, data + size);

    while (buffer_.size() >= kFrameHeaderSize) {
        if (std::memcmp(buffer_.data(), kMagic, sizeof(kMagic)) != 0) {
            error = "bad frame magic";
            buffer_.clear();
            return false;
        }
        if (buffer_[4] != kProtocolVersion) {
            error = "unsupported frame version";
            buffer_.clear();
            return false;
        }
        const std::uint32_t payload_size = read_u32_le(buffer_.data() + 12);
        if (payload_size > kMaxFramePayload) {
            error = "frame payload exceeds limit";
            buffer_.clear();
            return false;
        }
        const std::size_t total = kFrameHeaderSize + payload_size;
        if (buffer_.size() < total) {
            break;
        }
        Frame frame;
        frame.type = static_cast<FrameType>(buffer_[5]);
        frame.flags = read_u16_le(buffer_.data() + 6);
        frame.seq = read_u32_le(buffer_.data() + 8);
        frame.payload.assign(
            buffer_.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderSize),
            buffer_.begin() + static_cast<std::ptrdiff_t>(total));
        frames.push_back(std::move(frame));
        buffer_.erase(
            buffer_.begin(),
            buffer_.begin() + static_cast<std::ptrdiff_t>(total));
    }
    return true;
}

void FrameDecoder::reset()
{
    buffer_.clear();
}

std::size_t FrameDecoder::buffered_bytes() const
{
    return buffer_.size();
}

} // namespace gate1
