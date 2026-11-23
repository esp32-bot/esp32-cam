#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "ecl/jpeg_parser.hpp"
#include "jpeg_metrics.hpp"

using namespace ecl;

static void fuzz_one_input(const uint8_t* data, size_t size) {
    static JpegParserWorkspace ws{};
    static int16_t blocks[4096 * 64];
    JpegFrameInfo info{};
    (void)parse_jpeg_to_dct(data, size, &info, &ws, blocks, 4096);
}

#ifdef ECL_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz_one_input(data, size);
    return 0;
}
#else
int main(int argc, char** argv) {
    const int iterations = (argc > 1) ? std::atoi(argv[1]) : 10000;

    // Seed corpus from valid baseline JPEGs (4:2:0, 4:2:2, 4:4:4, DRI)
    std::vector<std::vector<uint8_t>> seeds;
    for (int sub = 0; sub < 3; ++sub) {
        host::SceneParams sp{};
        sp.width = 96;
        sp.height = 64;
        sp.high_texture = (sub == 1);
        seeds.push_back(host::encode_rgb_to_jpeg(host::render_synthetic_scene(sp), 70, sub, sub == 2 ? 4 : 0));
    }

    uint64_t rng = 0xF00DCAFE12345678ULL;
    auto next_u64 = [&]() -> uint64_t {
        uint64_t z = (rng += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    };

    for (const auto& s : seeds) {
        fuzz_one_input(s.data(), s.size());
    }

    for (int it = 0; it < iterations; ++it) {
        std::vector<uint8_t> buf = seeds[next_u64() % seeds.size()];
        const int mode = static_cast<int>(next_u64() % 6);
        if (mode == 0 && !buf.empty()) {
            // Bit flips
            const int nflips = 1 + static_cast<int>(next_u64() % 8);
            for (int f = 0; f < nflips; ++f) {
                buf[next_u64() % buf.size()] ^= static_cast<uint8_t>(1u << (next_u64() & 7u));
            }
        } else if (mode == 1 && !buf.empty()) {
            // Random truncation
            buf.resize(next_u64() % buf.size());
        } else if (mode == 2 && buf.size() > 8) {
            // Splice marker bytes 0xFF 0xXX
            const size_t pos = next_u64() % (buf.size() - 2);
            buf[pos] = 0xFF;
            buf[pos + 1] = static_cast<uint8_t>(next_u64() & 0xFFu);
        } else if (mode == 3) {
            // Completely random garbage
            buf.resize(next_u64() % 512);
            for (auto& b : buf) b = static_cast<uint8_t>(next_u64() & 0xFFu);
        } else if (mode == 4 && buf.size() > 20) {
            // Corrupt segment length fields
            const size_t pos = 2 + (next_u64() % (buf.size() - 6));
            buf[pos] = 0xFF;
            buf[pos + 1] = 0xFF;
        } else if (!buf.empty()) {
            // Overwrite slice with 0xFF (tests byte stuffing & marker loops)
            const size_t pos = next_u64() % buf.size();
            const size_t len = std::min<size_t>(32, buf.size() - pos);
            for (size_t k = 0; k < len; ++k) buf[pos + k] = 0xFF;
        }
        fuzz_one_input(buf.data(), buf.size());
    }

    std::printf("fuzz_jpeg_parser: %d mutated inputs completed safely (0 crashes/hangs)\n", iterations);
    return 0;
}
#endif
