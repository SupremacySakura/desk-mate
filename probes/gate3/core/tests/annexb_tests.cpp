#include "gate3/annexb.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

#ifndef GATE3_TESTDATA
#define GATE3_TESTDATA "probes/gate3/testdata/gate3-testsrc.h264"
#endif

int failures = 0;

void Expect(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

std::vector<std::uint8_t> ReadFile(const char *path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void TestStreamHasRepeatHeadersAndStartsOnAnIdr()
{
    auto bytes = ReadFile(GATE3_TESTDATA);
    Expect(!bytes.empty(), "test stream exists");
    std::string error;
    auto stream = deskmate::gate3::ParseAnnexB(std::move(bytes), error);
    Expect(stream.has_value(), error.c_str());
    if (!stream.has_value()) {
        return;
    }

    Expect(stream->nals.size() > 10, "stream contains multiple NAL units");
    Expect(stream->access_units.size() == 60, "stream contains 60 access units");
    Expect(stream->access_units.front().keyframe, "stream starts with an IDR");
    Expect(stream->access_units.front().contains_sps, "first IDR carries SPS");
    Expect(stream->access_units.front().contains_pps, "first IDR carries PPS");
    Expect(stream->access_units.front().has_aud, "generated access units carry AUD");

    int keyframe_count = 0;
    for (const auto &access_unit : stream->access_units) {
        if (access_unit.keyframe) {
            ++keyframe_count;
            Expect(access_unit.contains_sps, "each IDR carries SPS for resubmission");
            Expect(access_unit.contains_pps, "each IDR carries PPS for resubmission");
        }
    }
    Expect(keyframe_count == 2, "test stream contains two recoverable IDRs");
}

void ParserRejectsTruncatedStartCode()
{
    std::vector<std::uint8_t> bytes = {0x00, 0x00, 0x01};
    std::string error;
    const auto stream = deskmate::gate3::ParseAnnexB(std::move(bytes), error);
    Expect(!stream.has_value(), "truncated Annex-B stream is rejected");
    Expect(!error.empty(), "parse failure includes a reason");
}

void ParserRejectsMalformedSliceHeader()
{
    std::vector<std::uint8_t> bytes = {
        0x00, 0x00, 0x00, 0x01, 0x09, 0xF0,
        0x00, 0x00, 0x00, 0x01, 0x65,
    };
    std::string error;
    const auto stream = deskmate::gate3::ParseAnnexB(std::move(bytes), error);
    Expect(!stream.has_value(), "slice without a first_mb_in_slice field is rejected");
}

void AccessUnitViewIsContiguousAndBounded()
{
    auto bytes = ReadFile(GATE3_TESTDATA);
    std::string error;
    auto stream = deskmate::gate3::ParseAnnexB(std::move(bytes), error);
    Expect(stream.has_value(), "test stream parses for view checks");
    if (!stream.has_value()) {
        return;
    }
    std::size_t previous_end = 0;
    for (const auto &access_unit : stream->access_units) {
        const auto view = stream->View(access_unit);
        Expect(access_unit.offset >= previous_end, "access units do not overlap");
        Expect(access_unit.offset + access_unit.size <= stream->bytes.size(), "access unit is bounded");
        Expect(view.size() == access_unit.size, "access unit view has the recorded size");
        Expect(view.size() >= 5, "access unit contains start code and NAL payload");
        previous_end = access_unit.offset + access_unit.size;
    }
}

} // namespace

int main()
{
    TestStreamHasRepeatHeadersAndStartsOnAnIdr();
    ParserRejectsTruncatedStartCode();
    ParserRejectsMalformedSliceHeader();
    AccessUnitViewIsContiguousAndBounded();
    if (failures == 0) {
        std::puts("annex-b tests: PASS");
        return 0;
    }
    std::fprintf(stderr, "annex-b tests: %d failure(s)\n", failures);
    return 1;
}
