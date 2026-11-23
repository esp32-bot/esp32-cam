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

static CodecMode parse_mode(const std::string& s) {
    if (s == "PASS_THROUGH") return CodecMode::PASS_THROUGH;
    if (s == "FRAME_SKIP") return CodecMode::FRAME_SKIP;
    if (s == "GENERIC_ZLIB") return CodecMode::GENERIC_ZLIB;
    if (s == "DCT_DC_RESIDUAL") return CodecMode::DCT_DC_RESIDUAL;
    if (s == "DCT_LOW_AC_RESIDUAL") return CodecMode::DCT_LOW_AC_RESIDUAL;
    if (s == "DCT_FULL_RESIDUAL") return CodecMode::DCT_FULL_RESIDUAL;
    if (s == "DCT_SPARSE_BLOCK") return CodecMode::DCT_SPARSE_BLOCK;
    return CodecMode::PASS_THROUGH;
}

struct SendCtx {
    NetworkImpairmentSimulator* sim = nullptr;
    uint64_t send_time_us = 0;
};

static bool emit_to_sim(void* ctx, const uint8_t* data, size_t len) {
    auto* s = static_cast<SendCtx*>(ctx);
    s->sim->send(data, len, s->send_time_us);
    return true;
}

int main(int argc, char** argv) {
    std::string input_dir;
    std::string out_dir;
    CodecMode mode = CodecMode::PASS_THROUGH;
    double loss_rate = 0.0;
    double burst_enter = 0.0;
    double reorder_prob = 0.0;
    uint32_t kf_interval = 15;
    uint16_t sparse_thresh = 0;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--in" && i + 1 < argc) input_dir = argv[++i];
        else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (a == "--mode" && i + 1 < argc) mode = parse_mode(argv[++i]);
        else if (a == "--loss" && i + 1 < argc) loss_rate = std::atof(argv[++i]);
        else if (a == "--burst" && i + 1 < argc) burst_enter = std::atof(argv[++i]);
        else if (a == "--reorder" && i + 1 < argc) reorder_prob = std::atof(argv[++i]);
        else if (a == "--kf" && i + 1 < argc) kf_interval = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (a == "--thresh" && i + 1 < argc) sparse_thresh = static_cast<uint16_t>(std::atoi(argv[++i]));
    }

    if (input_dir.empty()) {
        std::fprintf(stderr, "Usage: ecl_replay --in <jpeg_dir> [--out <dir>] [--mode PASS_THROUGH|...] [--loss 0.0]\n");
        return 1;
    }

    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(input_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".jpg") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "No .jpg files found in %s\n", input_dir.c_str());
        return 1;
    }
    if (!out_dir.empty()) fs::create_directories(out_dir);

    TemporalCodecConfig cfg{};
    cfg.mode = mode;
    cfg.keyframe_interval = kf_interval;
    cfg.max_dependency_depth = kf_interval;
    cfg.max_frame_bytes = 262144;
    cfg.max_blocks = 16384;
    cfg.sparse_block_threshold = sparse_thresh;
    cfg.allow_keyframe_fallback = true;

    TemporalCodecContext enc;
    TemporalDecoderContext dec;
    Packetizer pkt({1, kDefaultMtuBytes});
    Depacketizer depkt;
    if (!enc.init(cfg) || !dec.init(cfg) || !depkt.init({262144, 4, 3})) {
        std::fprintf(stderr, "Failed to initialize pipeline\n");
        return 1;
    }

    ImpairmentConfig icfg{};
    icfg.random_loss_rate = loss_rate;
    icfg.burst_enter_prob = burst_enter;
    icfg.burst_stay_prob = (burst_enter > 0.0) ? 0.5 : 0.0;
    icfg.reorder_prob = reorder_prob;
    NetworkImpairmentSimulator sim(icfg);

    std::vector<std::vector<uint8_t>> orig_frames;
    orig_frames.reserve(files.size());
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        orig_frames.emplace_back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    uint64_t encode_us_total = 0;
    uint64_t decode_us_total = 0;
    uint64_t exact_byte_matches = 0;
    uint64_t exact_pixel_matches = 0;
    uint64_t decoded_ok = 0;
    double psnr_sum = 0.0;
    double ssim_sum = 0.0;
    uint64_t changed_blocks_sum = 0;
    uint64_t total_blocks_sum = 0;

    uint8_t wire_buf[kDefaultMtuBytes];
    auto process_packet = [&](const std::vector<uint8_t>& p) {
        PacketizedFrame pf{};
        if (!depkt.feed_packet(p.data(), p.size(), &pf)) return;
        ReconstructedFrame rf{};
        const auto t0 = std::chrono::steady_clock::now();
        const DecodeStatus ds = codec_decode(&dec, pf, &rf);
        const auto t1 = std::chrono::steady_clock::now();
        decode_us_total += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        if (ds != DecodeStatus::OK) return;

        decoded_ok++;
        const auto& src = orig_frames[rf.frame_id];
        if (rf.bytes.size == src.size() && std::memcmp(rf.bytes.data, src.data(), src.size()) == 0) {
            exact_byte_matches++;
            exact_pixel_matches++;
            psnr_sum += 99.99;
            ssim_sum += 1.0;
        } else {
            RgbImage a{}, b{};
            if (decode_jpeg_to_rgb(src.data(), src.size(), &a) &&
                decode_jpeg_to_rgb(rf.bytes.data, rf.bytes.size, &b)) {
                const auto q = compute_quality(a, b);
                if (q.exact_pixels) exact_pixel_matches++;
                psnr_sum += q.psnr_db;
                ssim_sum += q.ssim;
            }
        }
        if (!out_dir.empty()) {
            char name[64];
            std::snprintf(name, sizeof(name), "reconstructed_%04u.jpg", rf.frame_id);
            std::ofstream out(fs::path(out_dir) / name, std::ios::binary);
            out.write(reinterpret_cast<const char*>(rf.bytes.data), static_cast<std::streamsize>(rf.bytes.size));
        }
    };

    for (uint32_t i = 0; i < orig_frames.size(); ++i) {
        EncodedFrameView fv{};
        fv.frame_id = i;
        fv.capture_timestamp_us = i * 33333ULL;
        fv.data = orig_frames[i].data();
        fv.size = orig_frames[i].size();

        CodecOutput co{};
        const auto t0 = std::chrono::steady_clock::now();
        const CodecStatus cs = codec_encode(&enc, fv, &co);
        const auto t1 = std::chrono::steady_clock::now();
        encode_us_total += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        if (cs != CodecStatus::OK) continue;

        changed_blocks_sum += co.changed_blocks;
        total_blocks_sum += co.total_blocks;

        SendCtx sctx{&sim, fv.capture_timestamp_us};
        pkt.packetize(co, static_cast<uint32_t>(fv.capture_timestamp_us), fv.metadata,
                      wire_buf, sizeof(wire_buf), emit_to_sim, &sctx);
        for (const auto& p : sim.drain_ready(fv.capture_timestamp_us + 2000ULL)) {
            process_packet(p);
        }
    }
    for (const auto& p : sim.drain_all()) process_packet(p);
    depkt.flush_incomplete();

    const double ratio = enc.stats().input_bytes > 0
                             ? (static_cast<double>(enc.stats().output_bytes) / enc.stats().input_bytes)
                             : 1.0;
    const double avg_psnr = decoded_ok > 0 ? (psnr_sum / decoded_ok) : 0.0;
    const double avg_ssim = decoded_ok > 0 ? (ssim_sum / decoded_ok) : 0.0;

    std::printf(
        "{\n"
        "  \"mode\": \"%s\",\n"
        "  \"frames_in\": %zu,\n"
        "  \"frames_decoded\": %llu,\n"
        "  \"exact_byte_matches\": %llu,\n"
        "  \"exact_pixel_matches\": %llu,\n"
        "  \"input_bytes\": %llu,\n"
        "  \"codec_bytes\": %llu,\n"
        "  \"wire_bytes\": %llu,\n"
        "  \"compression_ratio\": %.4f,\n"
        "  \"keyframes\": %llu,\n"
        "  \"deltas\": %llu,\n"
        "  \"keyframe_fallbacks\": %llu,\n"
        "  \"missing_ref_drops\": %llu,\n"
        "  \"frames_expired\": %llu,\n"
        "  \"avg_encode_us\": %.1f,\n"
        "  \"avg_decode_us\": %.1f,\n"
        "  \"avg_psnr_db\": %.2f,\n"
        "  \"avg_ssim\": %.4f,\n"
        "  \"changed_blocks\": %llu,\n"
        "  \"total_blocks\": %llu,\n"
        "  \"encoder_mem_bytes\": %zu,\n"
        "  \"decoder_mem_bytes\": %zu\n"
        "}\n",
        codec_mode_name(mode),
        orig_frames.size(),
        static_cast<unsigned long long>(decoded_ok),
        static_cast<unsigned long long>(exact_byte_matches),
        static_cast<unsigned long long>(exact_pixel_matches),
        static_cast<unsigned long long>(enc.stats().input_bytes),
        static_cast<unsigned long long>(enc.stats().output_bytes),
        static_cast<unsigned long long>(pkt.stats().wire_bytes),
        ratio,
        static_cast<unsigned long long>(enc.stats().keyframes_emitted),
        static_cast<unsigned long long>(enc.stats().deltas_emitted),
        static_cast<unsigned long long>(enc.stats().keyframe_fallbacks),
        static_cast<unsigned long long>(dec.stats().missing_reference_drops),
        static_cast<unsigned long long>(depkt.stats().frames_expired),
        static_cast<double>(encode_us_total) / orig_frames.size(),
        decoded_ok > 0 ? (static_cast<double>(decode_us_total) / decoded_ok) : 0.0,
        avg_psnr,
        avg_ssim,
        static_cast<unsigned long long>(changed_blocks_sum),
        static_cast<unsigned long long>(total_blocks_sum),
        enc.allocated_bytes(),
        dec.allocated_bytes());

    return 0;
}
