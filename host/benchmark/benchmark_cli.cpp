#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ecl/net_impairment.hpp"
#include "ecl/packetizer.hpp"
#include "ecl/temporal_codec.hpp"
#include "jpeg_metrics.hpp"

namespace fs = std::filesystem;
using namespace ecl;
using namespace ecl::host;

struct BenchConfig {
    const char* label;
    CodecMode mode;
    uint16_t sparse_thresh;
    uint8_t low_ac;
    bool allow_kf_fallback;
};

struct BenchResult {
    std::string scene;
    std::string label;
    size_t frames = 0;
    double input_bpf = 0;
    double output_bpf = 0;
    double wire_bpf = 0;
    double ratio = 1.0;
    double enc_us = 0;
    double dec_us = 0;
    double changed_pct = 0;
    double psnr_db = 0;
    double ssim = 0;
    uint64_t keyframes = 0;
    uint64_t kf_fallbacks = 0;
    bool all_exact_bytes = false;
    bool all_exact_pixels = false;
    size_t enc_mem_kb = 0;
};

struct SendCtx {
    NetworkImpairmentSimulator* sim = nullptr;
    uint64_t ts = 0;
};

static bool emit_cb(void* ctx, const uint8_t* data, size_t len) {
    auto* s = static_cast<SendCtx*>(ctx);
    s->sim->send(data, len, s->ts);
    return true;
}

static BenchResult run_bench(const std::string& scene_name,
                             const std::vector<std::vector<uint8_t>>& frames,
                             const BenchConfig& bc,
                             uint32_t kf_interval) {
    TemporalCodecConfig cfg{};
    cfg.mode = bc.mode;
    cfg.keyframe_interval = kf_interval;
    cfg.max_dependency_depth = kf_interval;
    cfg.max_frame_bytes = 131072;
    cfg.max_blocks = 8192;
    cfg.low_ac_count = bc.low_ac;
    cfg.sparse_block_threshold = bc.sparse_thresh;
    cfg.allow_keyframe_fallback = bc.allow_kf_fallback;
    cfg.enable_dct_buffers = (bc.mode != CodecMode::PASS_THROUGH &&
                              bc.mode != CodecMode::FRAME_SKIP &&
                              bc.mode != CodecMode::GENERIC_ZLIB);

    TemporalCodecContext enc;
    TemporalDecoderContext dec;
    Packetizer pkt({1, kDefaultMtuBytes});
    Depacketizer depkt;
    enc.init(cfg);
    dec.init(cfg);
    depkt.init({131072, 4, 3});

    NetworkImpairmentSimulator sim({});  // zero loss

    uint64_t enc_us = 0, dec_us = 0;
    uint64_t changed_sum = 0, total_sum = 0;
    uint64_t exact_bytes = 0, exact_pixels = 0, decoded = 0;
    double psnr_sum = 0.0, ssim_sum = 0.0;
    uint8_t wire_buf[kDefaultMtuBytes];

    for (uint32_t i = 0; i < frames.size(); ++i) {
        EncodedFrameView fv{};
        fv.frame_id = i;
        fv.capture_timestamp_us = i * 33333ULL;
        fv.data = frames[i].data();
        fv.size = frames[i].size();
        fv.metadata.width = 320;
        fv.metadata.height = 240;

        CodecOutput co{};
        const auto t0 = std::chrono::steady_clock::now();
        const CodecStatus cs = codec_encode(&enc, fv, &co);
        const auto t1 = std::chrono::steady_clock::now();
        enc_us += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        if (cs != CodecStatus::OK) continue;

        changed_sum += co.changed_blocks;
        total_sum += co.total_blocks;

        SendCtx sctx{&sim, fv.capture_timestamp_us};
        pkt.packetize(co, static_cast<uint32_t>(fv.capture_timestamp_us), fv.metadata,
                      wire_buf, sizeof(wire_buf), emit_cb, &sctx);
        for (const auto& p : sim.drain_all()) {
            PacketizedFrame pf{};
            if (!depkt.feed_packet(p.data(), p.size(), &pf)) continue;
            ReconstructedFrame rf{};
            const auto d0 = std::chrono::steady_clock::now();
            const DecodeStatus ds = codec_decode(&dec, pf, &rf);
            const auto d1 = std::chrono::steady_clock::now();
            dec_us += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(d1 - d0).count());
            if (ds != DecodeStatus::OK) continue;
            decoded++;

            if (rf.bytes.size == frames[i].size() &&
                std::memcmp(rf.bytes.data, frames[i].data(), frames[i].size()) == 0) {
                exact_bytes++;
                exact_pixels++;
                psnr_sum += 99.99;
                ssim_sum += 1.0;
            } else {
                RgbImage a{}, b{};
                if (decode_jpeg_to_rgb(frames[i].data(), frames[i].size(), &a) &&
                    decode_jpeg_to_rgb(rf.bytes.data, rf.bytes.size, &b)) {
                    const auto q = compute_quality(a, b);
                    if (q.exact_pixels) exact_pixels++;
                    psnr_sum += q.psnr_db;
                    ssim_sum += q.ssim;
                }
            }
        }
    }

    BenchResult r{};
    r.scene = scene_name;
    r.label = bc.label;
    r.frames = frames.size();
    r.input_bpf = static_cast<double>(enc.stats().input_bytes) / frames.size();
    r.output_bpf = static_cast<double>(enc.stats().output_bytes) / frames.size();
    r.wire_bpf = static_cast<double>(pkt.stats().wire_bytes) / frames.size();
    r.ratio = r.output_bpf / r.input_bpf;
    r.enc_us = static_cast<double>(enc_us) / frames.size();
    r.dec_us = decoded > 0 ? (static_cast<double>(dec_us) / decoded) : 0.0;
    r.changed_pct = total_sum > 0 ? (100.0 * changed_sum / total_sum) : 0.0;
    r.psnr_db = decoded > 0 ? (psnr_sum / decoded) : 0.0;
    r.ssim = decoded > 0 ? (ssim_sum / decoded) : 0.0;
    r.keyframes = enc.stats().keyframes_emitted;
    r.kf_fallbacks = enc.stats().keyframe_fallbacks;
    r.all_exact_bytes = (exact_bytes == frames.size());
    r.all_exact_pixels = (exact_pixels == frames.size());
    r.enc_mem_kb = enc.allocated_bytes() / 1024;
    return r;
}

int main(int argc, char** argv) {
    std::string corpus_dir = "testdata/generated";
    std::string json_out;
    uint32_t kf_interval = 15;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--corpus" && i + 1 < argc) corpus_dir = argv[++i];
        else if (a == "--json" && i + 1 < argc) json_out = argv[++i];
        else if (a == "--kf" && i + 1 < argc) kf_interval = static_cast<uint32_t>(std::atoi(argv[++i]));
    }

    const std::vector<std::string> scene_order = {
        "static_scene", "repeated_frames", "sensor_noise", "brightness_change",
        "small_pitch", "smooth_pan", "large_motion", "high_texture", "scene_cut"
    };

    const std::vector<BenchConfig> configs = {
        {"PASS_THROUGH", CodecMode::PASS_THROUGH, 0, 15, true},
        {"FRAME_SKIP", CodecMode::FRAME_SKIP, 0, 15, true},
#ifdef ECL_ENABLE_ZLIB
        {"GENERIC_ZLIB", CodecMode::GENERIC_ZLIB, 0, 15, true},
#endif
        {"DCT_DC_RES", CodecMode::DCT_DC_RESIDUAL, 0, 0, true},
        {"DCT_LOW_AC15", CodecMode::DCT_LOW_AC_RESIDUAL, 0, 15, true},
        {"DCT_FULL_EXACT", CodecMode::DCT_FULL_RESIDUAL, 0, 63, true},
        {"DCT_SPARSE_T2", CodecMode::DCT_SPARSE_BLOCK, 2, 63, true},
    };

    std::vector<BenchResult> results;
    std::printf("| Scene | Mode | In B/f | Out B/f | Ratio | Enc us | Dec us | Chg%% | PSNR dB | SSIM | KF(fb) | Equiv |\n");
    std::printf("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|\n");

    for (const auto& sname : scene_order) {
        const fs::path sdir = fs::path(corpus_dir) / sname;
        if (!fs::exists(sdir)) continue;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(sdir)) {
            if (e.is_regular_file() && e.path().extension() == ".jpg") files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        if (files.empty()) continue;

        std::vector<std::vector<uint8_t>> frames;
        for (const auto& f : files) {
            std::ifstream in(f, std::ios::binary);
            frames.emplace_back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }

        for (const auto& bc : configs) {
            const auto r = run_bench(sname, frames, bc, kf_interval);
            const char* eq = r.all_exact_bytes ? "byte" : (r.all_exact_pixels ? "pixel" : "lossy");
            std::printf("| %s | %s | %.0f | %.0f | %.3f | %.0f | %.0f | %.1f%% | %.2f | %.4f | %llu(%llu) | %s |\n",
                        r.scene.c_str(), r.label.c_str(), r.input_bpf, r.output_bpf, r.ratio,
                        r.enc_us, r.dec_us, r.changed_pct, r.psnr_db, r.ssim,
                        static_cast<unsigned long long>(r.keyframes),
                        static_cast<unsigned long long>(r.kf_fallbacks), eq);
            results.push_back(r);
        }
    }

    if (!json_out.empty()) {
        std::ofstream jf(json_out);
        jf << "[\n";
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            jf << "  {\"scene\":\"" << r.scene << "\",\"mode\":\"" << r.label
               << "\",\"frames\":" << r.frames
               << ",\"input_bpf\":" << r.input_bpf
               << ",\"output_bpf\":" << r.output_bpf
               << ",\"wire_bpf\":" << r.wire_bpf
               << ",\"ratio\":" << r.ratio
               << ",\"enc_us\":" << r.enc_us
               << ",\"dec_us\":" << r.dec_us
               << ",\"changed_pct\":" << r.changed_pct
               << ",\"psnr_db\":" << r.psnr_db
               << ",\"ssim\":" << r.ssim
               << ",\"keyframes\":" << r.keyframes
               << ",\"kf_fallbacks\":" << r.kf_fallbacks
               << ",\"exact_bytes\":" << (r.all_exact_bytes ? "true" : "false")
               << ",\"exact_pixels\":" << (r.all_exact_pixels ? "true" : "false")
               << ",\"enc_mem_kb\":" << r.enc_mem_kb << "}"
               << (i + 1 < results.size() ? "," : "") << "\n";
        }
        jf << "]\n";
    }
    return 0;
}
