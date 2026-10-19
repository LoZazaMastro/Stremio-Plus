// Public FFmpeg metadata and IEC61937 transport regression checks.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_dovi.h"
#include "player_iec61937.h"
extern "C" {
#include <libavutil/dovi_meta.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void metadata_tests() {
    size_t size = 0;
    auto* metadata = av_dovi_metadata_alloc(&size);
    require(metadata != nullptr, "FFmpeg metadata allocation failed");
    auto* header = av_dovi_get_header(metadata);
    header->vdr_rpu_profile = 0; header->bl_video_full_range_flag = 1;
    header->bl_bit_depth = 10; header->coef_log2_denom = 20;
    auto* mapping = av_dovi_get_mapping(metadata);
    auto* color = av_dovi_get_color(metadata);
    for (int c = 0; c < 3; ++c) {
        auto& curve = mapping->curves[c];
        curve.num_pivots = 2; curve.pivots[0] = 0; curve.pivots[1] = 1023;
        curve.mapping_idc[0] = AV_DOVI_MAPPING_POLYNOMIAL;
        curve.poly_order[0] = 1; curve.poly_coef[0][1] = 1 << 20;
        color->ycc_to_rgb_matrix[c * 3 + c] = {1,1};
        color->rgb_to_lms_matrix[c * 3 + c] = {1,1};
    }
    VideoDoviParams params{};
    require(video_dovi_parse_metadata(metadata, size, params), "valid Profile5 public metadata was rejected");
    require(params.coef[0][1] == 1 && params.lohi[0][1] == 1,
        "identity polynomial coefficients were not preserved");
    header->bl_video_full_range_flag = 0;
    require(!video_dovi_parse_metadata(metadata, size, params), "HDR10-compatible Profile8 was reshaped as Profile5");
    header->bl_video_full_range_flag = 1;
    const size_t original_offset = metadata->mapping_offset;
    metadata->mapping_offset = std::numeric_limits<size_t>::max();
    require(!video_dovi_parse_metadata(metadata, size, params), "overflowing offset was accepted");
    metadata->mapping_offset = original_offset + 1;
    require(!video_dovi_parse_metadata(metadata, size, params), "unaligned mapping was accepted");
    metadata->mapping_offset = original_offset;
    require(!video_dovi_parse_metadata(metadata, sizeof(AVDOVIMetadata), params), "truncated metadata was accepted");
    auto& curve = mapping->curves[0];
    curve.num_pivots = 10;
    require(!video_dovi_parse_metadata(metadata, size, params), "oversized piece table was accepted");
    curve.num_pivots = 2; curve.pivots[1] = 0;
    require(!video_dovi_parse_metadata(metadata, size, params), "unordered pivots were accepted");
    curve.pivots[1] = 1023; curve.poly_order[0] = 3;
    require(!video_dovi_parse_metadata(metadata, size, params), "unsupported polynomial order was accepted");
    curve.poly_order[0] = 1; curve.mapping_idc[0] = AV_DOVI_MAPPING_MMR; curve.mmr_order[0] = 3;
    curve.mmr_coef[0][0][0] = 1 << 20;
    require(video_dovi_parse_metadata(metadata, size, params) && params.coef[0][3] == 3 && params.mmr[0][0] == 1,
        "valid MMR coefficients were not transferred");
    curve.mmr_order[0] = 4;
    require(!video_dovi_parse_metadata(metadata, size, params), "unsupported MMR order was accepted");
    curve.mmr_order[0] = 3;
    auto* frame = av_frame_alloc();
    auto* side = av_frame_new_side_data(frame, AV_FRAME_DATA_DOVI_METADATA, size);
    std::memcpy(side->data, metadata, size);
    const auto held = video_dovi_from_frame(frame);
    require(bool(held), "AVFrame public side data was not extracted");
    av_frame_free(&frame); av_free(metadata);
    require(held->coef[0][3] == 3, "GPU metadata did not survive decoder frame release");
    require(!video_dovi_from_frame(nullptr), "null decoder frame produced metadata");
}
std::vector<uint8_t> read(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("fixture unavailable");
    return {std::istreambuf_iterator<char>(input), {}};
}
void bitstream_tests(const std::string& folder) {
    for (const auto codec : {pt::Codec::ac3, pt::Codec::eac3, pt::Codec::dts}) {
        const std::string name = codec == pt::Codec::ac3 ? "ac3" : codec == pt::Codec::eac3 ? "eac3" : "dts";
        const auto elementary = read(folder + '/' + name + ".bin");
        const auto reference = read(folder + '/' + name + ".spdif");
        pt::Packer packer(codec); size_t cursor = 0;
        const char* error = nullptr;
        std::vector<uint8_t> output;
        while (packer.next_burst(elementary, &cursor, &output, &error, true)) {
            require(cursor <= elementary.size(), "packer cursor escaped the input buffer");
            require(output.size() <= 1024 * 1024, "packer output exceeded fixture bound");
        }
        require(!error && cursor == elementary.size(), "elementary stream was not consumed");
        require(output == reference, "IEC61937 bytes differ from FFmpeg's independent muxer");
        require(pt::parse_frame(codec, elementary, std::numeric_limits<size_t>::max()).size == 0,
            "overflowing parser offset was accepted");
    }
    uint32_t random = 0x41d032fa;
    std::vector<uint8_t> payload(128);
    for (unsigned iteration = 0; iteration < 3000; ++iteration) {
        for (auto& byte : payload) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; byte = uint8_t(random); }
        for (auto codec : {pt::Codec::ac3, pt::Codec::eac3, pt::Codec::dts, pt::Codec::aac, pt::Codec::truehd}) {
            pt::Packer packer(codec); size_t cursor = 0; const char* error = nullptr;
            std::vector<uint8_t> output;
            for (unsigned n = 0; n < 64 && packer.next_burst(payload, &cursor, &output, &error, true); ++n)
                require(output.size() <= 64 * 61440, "malformed input caused unbounded transport output");
            require(cursor <= payload.size(), "malformed transport input escaped its bound");
        }
    }
    std::vector<uint8_t> output;
    bool rejected = false;
    try { pt::write_burst(1, 1, payload, 4, &output); } catch (const std::length_error&) { rejected = true; }
    require(rejected && output.empty(), "oversized payload was written past a carrier burst");
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try { metadata_tests(); bitstream_tests(argv[1]); }
    catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
    std::printf("{\"result\":\"PASS\",\"checks\":%d}\n", checks);
}
