#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jpeg_metrics.hpp"

namespace fs = std::filesystem;
using namespace ecl::host;

struct SceneSpec {
    const char* name;
    const char* description;
};

static const SceneSpec kScenes[] = {
    {"static_scene", "Static indoor robotics scene with zero motion"},
    {"small_pitch", "Smooth +/-3 deg vertical pitch oscillation (~8 px amplitude)"},
    {"smooth_pan", "Constant 1.5 px/frame horizontal camera pan"},
    {"large_motion", "Fast 18 px/frame horizontal + 6 px/frame vertical motion"},
    {"high_texture", "Static high-frequency textured scene with mild CMOS noise"},
    {"sensor_noise", "Static scene with realistic per-frame CMOS noise (sigma=3.5)"},
    {"scene_cut", "Abrupt scene cuts at frame 10 and frame 20"},
    {"repeated_frames", "Exact repeated frames (groups of 5 identical frames)"},
    {"brightness_change", "Global illumination ramp simulating AEC / flicker"},
};

static SceneParams params_for(const std::string& scene, int frame_idx, uint16_t w, uint16_t h) {
    SceneParams sp{};
    sp.width = w;
    sp.height = h;
    if (scene == "static_scene") {
        // identical scene
    } else if (scene == "small_pitch") {
        sp.pitch_px = 8.0 * std::sin(frame_idx * 0.28);
    } else if (scene == "smooth_pan") {
        sp.pan_px = frame_idx * 1.5;
    } else if (scene == "large_motion") {
        sp.pan_px = frame_idx * 18.0;
        sp.pitch_px = 14.0 * std::sin(frame_idx * 0.65);
    } else if (scene == "high_texture") {
        sp.high_texture = true;
        sp.noise_sigma = 1.2;
        sp.noise_seed = 1000ULL + static_cast<uint64_t>(frame_idx);
    } else if (scene == "sensor_noise") {
        sp.noise_sigma = 3.5;
        sp.noise_seed = 2000ULL + static_cast<uint64_t>(frame_idx);
    } else if (scene == "scene_cut") {
        sp.alt_scene = (frame_idx >= 10 && frame_idx < 20);
        sp.pan_px = (frame_idx % 10) * 0.5;
    } else if (scene == "repeated_frames") {
        const int group = frame_idx / 5;
        sp.pan_px = group * 6.0;
    } else if (scene == "brightness_change") {
        sp.brightness_scale = 0.85 + 0.30 * (static_cast<double>(frame_idx) / 29.0);
        sp.brightness_bias = -10.0 + 20.0 * (static_cast<double>(frame_idx) / 29.0);
    }
    return sp;
}

int main(int argc, char** argv) {
    std::string out_dir = "testdata/generated";
    std::string manifest_path = "testdata/manifests/default_corpus.json";
    uint16_t width = 320;
    uint16_t height = 240;
    int frames = 30;
    int quality = 75;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (a == "--manifest" && i + 1 < argc) manifest_path = argv[++i];
        else if (a == "--width" && i + 1 < argc) width = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--height" && i + 1 < argc) height = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--quality" && i + 1 < argc) quality = std::atoi(argv[++i]);
    }

    fs::create_directories(out_dir);
    fs::create_directories(fs::path(manifest_path).parent_path());

    std::ofstream mf(manifest_path);
    mf << "{\n";
    mf << "  \"width\": " << width << ",\n";
    mf << "  \"height\": " << height << ",\n";
    mf << "  \"frames_per_scene\": " << frames << ",\n";
    mf << "  \"jpeg_quality\": " << quality << ",\n";
    mf << "  \"scenes\": [\n";

    const size_t nscenes = sizeof(kScenes) / sizeof(kScenes[0]);
    for (size_t s = 0; s < nscenes; ++s) {
        const std::string sname = kScenes[s].name;
        const fs::path sdir = fs::path(out_dir) / sname;
        fs::create_directories(sdir);

        size_t total_bytes = 0;
        for (int f = 0; f < frames; ++f) {
            const auto sp = params_for(sname, f, width, height);
            const auto rgb = render_synthetic_scene(sp);
            const auto jpg = encode_rgb_to_jpeg(rgb, quality, 0, 0);
            total_bytes += jpg.size();

            char fname[64];
            std::snprintf(fname, sizeof(fname), "frame_%04d.jpg", f);
            std::ofstream out(sdir / fname, std::ios::binary);
            out.write(reinterpret_cast<const char*>(jpg.data()), static_cast<std::streamsize>(jpg.size()));
        }

        mf << "    {\"name\": \"" << sname << "\", \"description\": \"" << kScenes[s].description
           << "\", \"frames\": " << frames << ", \"avg_jpeg_bytes\": "
           << (static_cast<double>(total_bytes) / frames) << "}"
           << (s + 1 < nscenes ? "," : "") << "\n";
    }

    mf << "  ]\n}\n";
    std::printf("Generated %zu scenes (%d frames each, %ux%u Q=%d) into %s\n",
                nscenes, frames, width, height, quality, out_dir.c_str());
    return 0;
}
