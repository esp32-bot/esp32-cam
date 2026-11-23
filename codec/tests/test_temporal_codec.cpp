#include <cstdio>
#include <cstdlib>
#define ECL_CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (0)
#include <cstdio>
#include <cstring>
#include <vector>

#include "ecl/packetizer.hpp"
#include "ecl/temporal_codec.hpp"
#include "jpeg_metrics.hpp"

using namespace ecl;
using namespace ecl::host;

namespace {

void test_all_modes_roundtrip() {
    // Small 10-frame sequence: static -> small pitch -> repeated -> brightness change
    std::vector<std::vector<uint8_t>> frames;
    for (int i = 0; i < 10; ++i) {
        SceneParams sp{};
        sp.width = 160;
        sp.height = 120;
        if (i == 0 || i == 1 || i == 2) {
            sp.pitch_px = 0.0;
        } else if (i < 6) {
            sp.pitch_px = (i - 2) * 1.5;
        } else if (i == 6) {
            sp.pitch_px = 4.5;
        } else {
            sp.pitch_px = 4.5;
            sp.brightness_bias = (i - 6) * 8.0;
        }
        frames.push_back(encode_rgb_to_jpeg(render_synthetic_scene(sp), 75, 0, 0));
    }

    const CodecMode modes[] = {
        CodecMode::PASS_THROUGH,
        CodecMode::FRAME_SKIP,
#ifdef ECL_ENABLE_ZLIB
        CodecMode::GENERIC_ZLIB,
#endif
        CodecMode::DCT_DC_RESIDUAL,
        CodecMode::DCT_LOW_AC_RESIDUAL,
        CodecMode::DCT_FULL_RESIDUAL,
        CodecMode::DCT_SPARSE_BLOCK,
    };

    for (CodecMode m : modes) {
        TemporalCodecConfig cfg{};
        cfg.mode = m;
        cfg.keyframe_interval = 10;
        cfg.max_dependency_depth = 10;
        cfg.max_frame_bytes = 65536;
        cfg.max_blocks = 2048;
        cfg.allow_keyframe_fallback = false;  // Force deltas to exercise decoder paths!

        TemporalCodecContext enc;
        TemporalDecoderContext dec;
        ECL_CHECK(enc.init(cfg));
        ECL_CHECK(dec.init(cfg));

        for (uint32_t i = 0; i < frames.size(); ++i) {
            EncodedFrameView fv{};
            fv.frame_id = i;
            fv.capture_timestamp_us = i * 33333ULL;
            fv.data = frames[i].data();
            fv.size = frames[i].size();
            fv.metadata.width = 160;
            fv.metadata.height = 120;

            CodecOutput co{};
            ECL_CHECK(codec_encode(&enc, fv, &co) == CodecStatus::OK);

            PacketizedFrame pf{};
            pf.mode = co.mode;
            pf.frame_id = co.frame_id;
            pf.reference_frame_id = co.reference_frame_id;
            pf.is_keyframe = co.is_keyframe;
            pf.metadata = fv.metadata;
            pf.payload = co.bytes;

            ReconstructedFrame rf{};
            ECL_CHECK(codec_decode(&dec, pf, &rf) == DecodeStatus::OK);
            ECL_CHECK(rf.frame_id == i);
            ECL_CHECK(!rf.bytes.empty());

            // Verify byte-exactness for PASS_THROUGH, FRAME_SKIP, GENERIC_ZLIB
            if (m == CodecMode::PASS_THROUGH || m == CodecMode::FRAME_SKIP ||
                m == CodecMode::GENERIC_ZLIB) {
                ECL_CHECK(rf.bytes.size == frames[i].size());
                ECL_CHECK(std::memcmp(rf.bytes.data, frames[i].data(), frames[i].size()) == 0);
            } else {
                // Verify valid JPEG decode + exact pixel match for lossless DCT_FULL_RESIDUAL & DCT_SPARSE_BLOCK (thresh=0)
                RgbImage orig{}, rec{};
                ECL_CHECK(decode_jpeg_to_rgb(frames[i].data(), frames[i].size(), &orig));
                ECL_CHECK(decode_jpeg_to_rgb(rf.bytes.data, rf.bytes.size, &rec));
                const QualityMetrics q = compute_quality(orig, rec);
                if (m == CodecMode::DCT_FULL_RESIDUAL || m == CodecMode::DCT_SPARSE_BLOCK) {
                    ECL_CHECK(q.exact_pixels);
                } else if (m == CodecMode::DCT_LOW_AC_RESIDUAL) {
                    ECL_CHECK(q.psnr_db > 22.0);
                } else {
                    ECL_CHECK(q.psnr_db > 14.0);
                }
            }
        }
    }
}

void test_loss_safe_reference_recovery() {
    // 15 frames, keyframe interval = 5 (keyframes at 0, 5, 10)
    std::vector<std::vector<uint8_t>> frames;
    for (int i = 0; i < 15; ++i) {
        SceneParams sp{};
        sp.width = 160;
        sp.height = 120;
        sp.pitch_px = i * 0.8;
        frames.push_back(encode_rgb_to_jpeg(render_synthetic_scene(sp), 75, 0, 0));
    }

    TemporalCodecConfig cfg{};
    cfg.mode = CodecMode::DCT_FULL_RESIDUAL;
    cfg.keyframe_interval = 5;
    cfg.max_dependency_depth = 5;
    cfg.max_frame_bytes = 65536;
    cfg.max_blocks = 2048;
    cfg.allow_keyframe_fallback = false;

    TemporalCodecContext enc;
    TemporalDecoderContext dec;
    ECL_CHECK(enc.init(cfg));
    ECL_CHECK(dec.init(cfg));

    // Drop frame 2 (delta right after keyframe 0), frame 5 (keyframe itself!), and frame 9 (just before keyframe 10).
    for (uint32_t i = 0; i < frames.size(); ++i) {
        EncodedFrameView fv{};
        fv.frame_id = i;
        fv.data = frames[i].data();
        fv.size = frames[i].size();
        fv.metadata.width = 160;
        fv.metadata.height = 120;

        CodecOutput co{};
        ECL_CHECK(codec_encode(&enc, fv, &co) == CodecStatus::OK);

        // Simulate packet loss on frames 2, 5, and 9
        if (i == 2 || i == 5 || i == 9) {
            continue;
        }

        PacketizedFrame pf{};
        pf.mode = co.mode;
        pf.frame_id = co.frame_id;
        pf.reference_frame_id = co.reference_frame_id;
        pf.is_keyframe = co.is_keyframe;
        pf.metadata = fv.metadata;
        pf.payload = co.bytes;

        ReconstructedFrame rf{};
        const DecodeStatus ds = codec_decode(&dec, pf, &rf);

        if (i == 0 || i == 1) {
            ECL_CHECK(ds == DecodeStatus::OK);
        } else if (i == 3 || i == 4) {
            // Frame 2 was lost -> frames 3 & 4 must fail cleanly with MISSING_REFERENCE!
            ECL_CHECK(ds == DecodeStatus::MISSING_REFERENCE);
        } else if (i >= 6 && i <= 8) {
            // Keyframe 5 was lost -> deltas 6, 7, 8 must NOT poison stream; must return MISSING_REFERENCE!
            ECL_CHECK(ds == DecodeStatus::MISSING_REFERENCE);
        } else if (i >= 10) {
            // Keyframe 10 arrives -> immediate exact recovery for 10, 11, 12, 13, 14!
            ECL_CHECK(ds == DecodeStatus::OK);
            RgbImage orig{}, rec{};
            ECL_CHECK(decode_jpeg_to_rgb(frames[i].data(), frames[i].size(), &orig));
            ECL_CHECK(decode_jpeg_to_rgb(rf.bytes.data, rf.bytes.size, &rec));
            ECL_CHECK(compute_quality(orig, rec).exact_pixels);
        }
    }
    ECL_CHECK(dec.stats().missing_reference_drops == 5);
    ECL_CHECK(dec.stats().frames_decoded == 7);
}

void test_corrupted_delta_does_not_poison_future_keyframe() {
    SceneParams sp{};
    sp.width = 160;
    sp.height = 120;
    auto f0 = encode_rgb_to_jpeg(render_synthetic_scene(sp), 75, 0, 0);
    sp.pitch_px = 2.0;
    auto f1 = encode_rgb_to_jpeg(render_synthetic_scene(sp), 75, 0, 0);

    TemporalCodecConfig cfg{};
    cfg.mode = CodecMode::DCT_FULL_RESIDUAL;
    cfg.keyframe_interval = 10;
    cfg.max_frame_bytes = 65536;
    cfg.max_blocks = 2048;

    TemporalDecoderContext dec;
    ECL_CHECK(dec.init(cfg));

    PacketizedFrame kf{};
    kf.mode = CodecMode::DCT_FULL_RESIDUAL;
    kf.frame_id = 0;
    kf.reference_frame_id = 0;
    kf.is_keyframe = true;
    kf.payload = ConstByteSpan{f0.data(), f0.size()};
    ReconstructedFrame rf{};
    ECL_CHECK(codec_decode(&dec, kf, &rf) == DecodeStatus::OK);

    // Feed a deliberately corrupted delta payload referencing frame 0
    uint8_t corrupt[32] = {'E', 'C', 'L', 'D', 1, 5, 64, 6, 0xFF, 0xFF, 0, 0, 1, 0, 0, 0};
    PacketizedFrame bad{};
    bad.mode = CodecMode::DCT_FULL_RESIDUAL;
    bad.frame_id = 1;
    bad.reference_frame_id = 0;
    bad.is_keyframe = false;
    bad.payload = ConstByteSpan{corrupt, sizeof(corrupt)};
    ECL_CHECK(codec_decode(&dec, bad, &rf) != DecodeStatus::OK);
    ECL_CHECK(!dec.has_valid_reference());

    // Next keyframe recovers cleanly!
    kf.frame_id = 2;
    kf.reference_frame_id = 2;
    kf.payload = ConstByteSpan{f1.data(), f1.size()};
    ECL_CHECK(codec_decode(&dec, kf, &rf) == DecodeStatus::OK);
    ECL_CHECK(dec.has_valid_reference());
    ECL_CHECK(rf.bytes.size == f1.size());
}

}  // namespace

int main() {
    test_all_modes_roundtrip();
    test_loss_safe_reference_recovery();
    test_corrupted_delta_does_not_poison_future_keyframe();
    std::printf("test_temporal_codec: ALL PASSED\n");
    return 0;
}
