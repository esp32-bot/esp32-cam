#include <cstdio>
#include <cstdlib>
#define ECL_CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (0)
#include <cstdio>
#include <cstring>
#include <vector>

#include "ecl/jpeg_parser.hpp"
#include "ecl/jpeg_writer.hpp"
#include "jpeg_metrics.hpp"

using namespace ecl;
using namespace ecl::host;

namespace {

// Strips DHT segments from a valid JPEG to simulate OV2640/MJPEG frames without DHT.
std::vector<uint8_t> strip_dht_markers(const std::vector<uint8_t>& jpg) {
    std::vector<uint8_t> out;
    if (jpg.size() < 4) return out;
    out.push_back(jpg[0]);
    out.push_back(jpg[1]);
    size_t pos = 2;
    while (pos < jpg.size()) {
        if (jpg[pos] != 0xFF) break;
        const size_t mstart = pos;
        while (pos < jpg.size() && jpg[pos] == 0xFF) pos++;
        if (pos >= jpg.size()) break;
        const uint8_t marker = jpg[pos++];
        if (marker == 0xDA) {
            out.insert(out.end(), jpg.begin() + static_cast<std::ptrdiff_t>(mstart), jpg.end());
            break;
        }
        if (pos + 2 > jpg.size()) break;
        const uint16_t len = static_cast<uint16_t>((jpg[pos] << 8) | jpg[pos + 1]);
        const size_t seg_end = pos + len;
        if (seg_end > jpg.size()) break;
        if (marker != 0xC4) {
            out.insert(out.end(), jpg.begin() + static_cast<std::ptrdiff_t>(mstart),
                       jpg.begin() + static_cast<std::ptrdiff_t>(seg_end));
        }
        pos = seg_end;
    }
    return out;
}

void test_valid_corpus_roundtrip() {
    SceneParams sp{};
    sp.width = 320;
    sp.height = 240;
    sp.high_texture = true;
    RgbImage src = render_synthetic_scene(sp);

    JpegParserWorkspace ws{};
    std::vector<int16_t> blocks(8192 * 64);
    std::vector<uint8_t> rewritten(131072);

    const int subsamplings[3] = {0, 1, 2};  // 4:2:0, 4:2:2, 4:4:4
    const int rsts[2] = {0, 10};            // no DRI, DRI=10 MCUs

    for (int sub : subsamplings) {
        for (int rst : rsts) {
            auto jpg = encode_rgb_to_jpeg(src, 75, sub, rst);
            ECL_CHECK(!jpg.empty());

            JpegFrameInfo info{};
            const JpegParseStatus st =
                parse_jpeg_to_dct(jpg.data(), jpg.size(), &info, &ws, blocks.data(), 8192);
            ECL_CHECK(st == JpegParseStatus::OK);
            ECL_CHECK(info.width == 320 && info.height == 240);
            ECL_CHECK(info.restart_interval == rst);

            const size_t wlen =
                write_dct_to_jpeg(info, blocks.data(), info.total_blocks, rewritten.data(), rewritten.size());
            ECL_CHECK(wlen > 100);

            RgbImage dec_orig{}, dec_new{};
            ECL_CHECK(decode_jpeg_to_rgb(jpg.data(), jpg.size(), &dec_orig));
            ECL_CHECK(decode_jpeg_to_rgb(rewritten.data(), wlen, &dec_new));
            const QualityMetrics q = compute_quality(dec_orig, dec_new);
            ECL_CHECK(q.exact_pixels);
            ECL_CHECK(q.ssim == 1.0);
        }
    }

    // Test omitted-DHT (Annex K fallback)
    auto jpg_422 = encode_rgb_to_jpeg(src, 70, 1, 0);
    auto no_dht = strip_dht_markers(jpg_422);
    ECL_CHECK(no_dht.size() < jpg_422.size());
    JpegFrameInfo info{};
    ECL_CHECK(parse_jpeg_to_dct(no_dht.data(), no_dht.size(), &info, &ws, blocks.data(), 8192) ==
           JpegParseStatus::OK);
    const size_t wlen =
        write_dct_to_jpeg(info, blocks.data(), info.total_blocks, rewritten.data(), rewritten.size());
    ECL_CHECK(wlen > 100);
    RgbImage dec_orig{}, dec_new{};
    ECL_CHECK(decode_jpeg_to_rgb(jpg_422.data(), jpg_422.size(), &dec_orig));
    ECL_CHECK(decode_jpeg_to_rgb(rewritten.data(), wlen, &dec_new));
    ECL_CHECK(compute_quality(dec_orig, dec_new).exact_pixels);
}

void test_malformed_inputs_safe() {
    JpegParserWorkspace ws{};
    JpegFrameInfo info{};
    std::vector<int16_t> blocks(1024 * 64);

    // Empty / missing SOI
    const uint8_t bad1[4] = {0x00, 0x00, 0xFF, 0xD9};
    ECL_CHECK(parse_jpeg_to_dct(bad1, sizeof(bad1), &info, &ws, blocks.data(), 1024) ==
           JpegParseStatus::MISSING_SOI);

    // Huge dimensions in SOF0 (> kMaxJpegDim)
    const uint8_t huge_sof[] = {
        0xFF, 0xD8,
        0xFF, 0xC0, 0x00, 0x0B, 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x01, 0x11, 0x00,
        0xFF, 0xD9
    };
    ECL_CHECK(parse_jpeg_to_dct(huge_sof, sizeof(huge_sof), &info, &ws, blocks.data(), 1024) ==
           JpegParseStatus::INVALID_DIMENSIONS);

    // Truncated valid JPEG at every prefix length
    SceneParams sp{};
    sp.width = 64;
    sp.height = 64;
    auto jpg = encode_rgb_to_jpeg(render_synthetic_scene(sp), 70, 0, 0);
    for (size_t cut = 0; cut < jpg.size() / 2; ++cut) {
        const JpegParseStatus st =
            parse_jpeg_to_dct(jpg.data(), cut, &info, &ws, blocks.data(), 1024);
        ECL_CHECK(st != JpegParseStatus::OK);
    }

    // Buffer too small
    ECL_CHECK(parse_jpeg_to_dct(jpg.data(), jpg.size(), &info, &ws, blocks.data(), 2) ==
           JpegParseStatus::BUFFER_TOO_SMALL);
}

}  // namespace

int main() {
    test_valid_corpus_roundtrip();
    test_malformed_inputs_safe();
    std::printf("test_jpeg_parser: ALL PASSED\n");
    return 0;
}
