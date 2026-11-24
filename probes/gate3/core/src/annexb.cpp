#include "gate3/annexb.hpp"

#include <algorithm>
#include <limits>

namespace deskmate::gate3 {

namespace {

constexpr std::uint8_t kAud = 9;
constexpr std::uint8_t kSps = 7;
constexpr std::uint8_t kPps = 8;

bool IsVcl(std::uint8_t type)
{
    return type >= 1 && type <= 5;
}

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    bool ReadBit(bool &value)
    {
        if (byte_index_ >= bytes_.size()) {
            return false;
        }
        value = ((bytes_[byte_index_] >> (7 - bit_index_)) & 1U) != 0;
        if (++bit_index_ == 8) {
            bit_index_ = 0;
            ++byte_index_;
        }
        return true;
    }

    bool ReadUnsignedExpGolomb(std::uint32_t &value)
    {
        int leading_zeroes = 0;
        bool bit = false;
        while (true) {
            if (!ReadBit(bit)) {
                return false;
            }
            if (bit) {
                break;
            }
            if (++leading_zeroes > 31) {
                return false;
            }
        }

        std::uint32_t suffix = 0;
        for (int index = 0; index < leading_zeroes; ++index) {
            if (!ReadBit(bit)) {
                return false;
            }
            suffix = (suffix << 1U) | (bit ? 1U : 0U);
        }
        value = ((1U << leading_zeroes) - 1U) + suffix;
        return true;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t byte_index_ = 0;
    int bit_index_ = 0;
};

std::vector<std::uint8_t> RbspPrefix(std::span<const std::uint8_t> nal_payload)
{
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(std::min<std::size_t>(nal_payload.size(), 64));
    int zero_run = 0;
    for (std::uint8_t byte : nal_payload) {
        if (zero_run >= 2 && byte == 0x03) {
            zero_run = 0;
            continue;
        }
        rbsp.push_back(byte);
        if (rbsp.size() >= 64) {
            break;
        }
        if (byte == 0) {
            ++zero_run;
        } else {
            zero_run = 0;
        }
    }
    return rbsp;
}

bool ReadFirstMbInSlice(std::span<const std::uint8_t> nal, std::uint32_t &first_mb)
{
    if (nal.size() < 2) {
        return false;
    }
    const auto rbsp = RbspPrefix(nal.subspan(1));
    BitReader reader(rbsp);
    return reader.ReadUnsignedExpGolomb(first_mb);
}

struct RawNal {
    std::size_t start_code_offset = 0;
    std::size_t header_offset = 0;
    std::size_t payload_end = 0;
    std::uint8_t type = 0;
    std::uint32_t first_mb = std::numeric_limits<std::uint32_t>::max();
};

} // namespace

std::span<const std::uint8_t> AnnexBStream::View(const AnnexBAccessUnit &access_unit) const
{
    if (access_unit.offset > bytes.size() || access_unit.size > bytes.size() - access_unit.offset) {
        return {};
    }
    return std::span<const std::uint8_t>(bytes.data() + access_unit.offset, access_unit.size);
}

std::optional<AnnexBStream> ParseAnnexB(std::vector<std::uint8_t> bytes, std::string &error)
{
    std::vector<std::size_t> start_codes;
    for (std::size_t index = 0; index + 2 < bytes.size(); ++index) {
        if (bytes[index] != 0 || bytes[index + 1] != 0 || bytes[index + 2] != 1) {
            continue;
        }
        const std::size_t offset = index > 0 && bytes[index - 1] == 0 ? index - 1 : index;
        if (start_codes.empty() || start_codes.back() != offset) {
            start_codes.push_back(offset);
        }
        index += 2;
    }
    if (start_codes.empty()) {
        error = "Annex-B stream has no 00 00 01 start code";
        return std::nullopt;
    }

    std::vector<RawNal> raw_nals;
    raw_nals.reserve(start_codes.size());
    for (std::size_t index = 0; index < start_codes.size(); ++index) {
        const std::size_t start_code_size =
            start_codes[index] + 2 < bytes.size() && bytes[start_codes[index]] == 0 &&
                    bytes[start_codes[index] + 1] == 0 && bytes[start_codes[index] + 2] == 0 &&
                    bytes[start_codes[index] + 3] == 1
                ? 4
                : 3;
        const std::size_t header_offset = start_codes[index] + start_code_size;
        const std::size_t end = index + 1 < start_codes.size() ? start_codes[index + 1] : bytes.size();
        if (header_offset >= end) {
            error = "Annex-B NAL unit is empty";
            return std::nullopt;
        }
        RawNal nal;
        nal.start_code_offset = start_codes[index];
        nal.header_offset = header_offset;
        nal.payload_end = end;
        nal.type = bytes[header_offset] & 0x1FU;
        if (IsVcl(nal.type)) {
            const auto nal_bytes =
                std::span<const std::uint8_t>(bytes.data() + header_offset, end - header_offset);
            if (!ReadFirstMbInSlice(nal_bytes, nal.first_mb)) {
                error = "Annex-B slice header has no valid first_mb_in_slice";
                return std::nullopt;
            }
        }
        raw_nals.push_back(nal);
    }

    AnnexBStream stream;
    stream.bytes = std::move(bytes);
    stream.nals.reserve(raw_nals.size());
    for (const RawNal &nal : raw_nals) {
        stream.nals.push_back({nal.start_code_offset, nal.header_offset, nal.payload_end - nal.header_offset,
                               nal.type});
    }

    struct Group {
        std::size_t first_nal = 0;
        std::size_t last_nal = 0;
        bool has_vcl = false;
        bool keyframe = false;
        bool contains_sps = false;
        bool contains_pps = false;
        bool has_aud = false;
    };

    std::vector<Group> groups;
    Group current;
    bool current_started = false;
    const auto close_group = [&] {
        if (current_started && current.has_vcl) {
            groups.push_back(current);
        }
        current = {};
        current_started = false;
    };
    const auto start_group = [&](std::size_t nal_index) {
        current_started = true;
        current.first_nal = nal_index;
        current.last_nal = nal_index;
    };

    for (std::size_t index = 0; index < raw_nals.size(); ++index) {
        const RawNal &nal = raw_nals[index];
        if (nal.type == kAud) {
            close_group();
            start_group(index);
        } else if (!current_started) {
            start_group(index);
        } else if (!current.has_vcl) {
            current.last_nal = index;
        } else if (nal.type == kSps || nal.type == kPps) {
            close_group();
            start_group(index);
        } else if (IsVcl(nal.type) && nal.first_mb == 0) {
            close_group();
            start_group(index);
        } else {
            current.last_nal = index;
        }

        if (nal.type == kAud) {
            current.has_aud = true;
        } else if (nal.type == kSps) {
            current.contains_sps = true;
        } else if (nal.type == kPps) {
            current.contains_pps = true;
        } else if (IsVcl(nal.type)) {
            current.has_vcl = true;
            current.keyframe = current.keyframe || nal.type == 5;
        }
    }
    close_group();
    if (groups.empty()) {
        error = "Annex-B stream has no coded picture";
        return std::nullopt;
    }

    stream.access_units.reserve(groups.size());
    for (std::size_t index = 0; index < groups.size(); ++index) {
        const Group &group = groups[index];
        const std::size_t offset = raw_nals[group.first_nal].start_code_offset;
        const std::size_t end = index + 1 < groups.size()
                                    ? raw_nals[groups[index + 1].first_nal].start_code_offset
                                    : stream.bytes.size();
        stream.access_units.push_back({
            offset,
            end - offset,
            group.keyframe,
            group.contains_sps,
            group.contains_pps,
            group.has_aud,
        });
    }
    return stream;
}

} // namespace deskmate::gate3
