#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace deskmate::gate3 {

struct AnnexBNalUnit {
    std::size_t start_code_offset = 0;
    std::size_t header_offset = 0;
    std::size_t size = 0;
    std::uint8_t type = 0;
};

struct AnnexBAccessUnit {
    std::size_t offset = 0;
    std::size_t size = 0;
    bool keyframe = false;
    bool contains_sps = false;
    bool contains_pps = false;
    bool has_aud = false;
};

struct AnnexBStream {
    std::vector<std::uint8_t> bytes;
    std::vector<AnnexBNalUnit> nals;
    std::vector<AnnexBAccessUnit> access_units;

    std::span<const std::uint8_t> View(const AnnexBAccessUnit &access_unit) const;
};

// Parses H.264 Annex-B into independently submittable access units. Access
// units are delimited by AUD NALs when present, otherwise by the first VCL NAL
// carrying first_mb_in_slice == 0.
std::optional<AnnexBStream> ParseAnnexB(std::vector<std::uint8_t> bytes, std::string &error);

} // namespace deskmate::gate3
