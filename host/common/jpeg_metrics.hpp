#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <jpeglib.h>

namespace ecl::host {

struct RgbImage {
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<uint8_t> rgb;  // width * height * 3
};

struct QualityMetrics {
    bool exact_pixels = false;
    double mse = 0.0;
    double psnr_db = 0.0;
    double ssim = 0.0;
};

struct JpegErrorMgr {
    jpeg_error_mgr pub;
    bool failed = false;
};

inline void quiet_error_exit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    err->failed = true;
    throw std::runtime_error("libjpeg error");
}

inline void quiet_output_message(j_common_ptr) {}

// Encodes RGB24 image to baseline JPEG using libjpeg-turbo.
// subsampling: 0 = 4:2:0, 1 = 4:2:2, 2 = 4:4:4.
inline std::vector<uint8_t> encode_rgb_to_jpeg(const RgbImage& img,
                                               int quality = 75,
                                               int subsampling = 0,
                                               int restart_interval = 0) {
    jpeg_compress_struct cinfo{};
    JpegErrorMgr jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = quiet_error_exit;
    jerr.pub.output_message = quiet_output_message;

    unsigned char* out_buf = nullptr;
    unsigned long out_size = 0;

    jpeg_create_compress(&cinfo);
    try {
        jpeg_mem_dest(&cinfo, &out_buf, &out_size);
        cinfo.image_width = img.width;
        cinfo.image_height = img.height;
        cinfo.input_components = 3;
        cinfo.in_color_space = JCS_RGB;
        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, quality, TRUE);
        cinfo.dct_method = JDCT_ISLOW;
        cinfo.optimize_coding = FALSE;  // Standard Annex K Huffman tables (like OV2640)
        cinfo.restart_interval = static_cast<unsigned int>(restart_interval);

        if (subsampling == 0) {
            // 4:2:0
            cinfo.comp_info[0].h_samp_factor = 2;
            cinfo.comp_info[0].v_samp_factor = 2;
            cinfo.comp_info[1].h_samp_factor = 1;
            cinfo.comp_info[1].v_samp_factor = 1;
            cinfo.comp_info[2].h_samp_factor = 1;
            cinfo.comp_info[2].v_samp_factor = 1;
        } else if (subsampling == 1) {
            // 4:2:2 (horizontal)
            cinfo.comp_info[0].h_samp_factor = 2;
            cinfo.comp_info[0].v_samp_factor = 1;
            cinfo.comp_info[1].h_samp_factor = 1;
            cinfo.comp_info[1].v_samp_factor = 1;
            cinfo.comp_info[2].h_samp_factor = 1;
            cinfo.comp_info[2].v_samp_factor = 1;
        } else {
            // 4:4:4
            cinfo.comp_info[0].h_samp_factor = 1;
            cinfo.comp_info[0].v_samp_factor = 1;
            cinfo.comp_info[1].h_samp_factor = 1;
            cinfo.comp_info[1].v_samp_factor = 1;
            cinfo.comp_info[2].h_samp_factor = 1;
            cinfo.comp_info[2].v_samp_factor = 1;
        }

        jpeg_start_compress(&cinfo, TRUE);
        while (cinfo.next_scanline < cinfo.image_height) {
            const uint8_t* row =
                img.rgb.data() + static_cast<size_t>(cinfo.next_scanline) * img.width * 3u;
            JSAMPROW row_ptr = const_cast<JSAMPROW>(row);
            jpeg_write_scanlines(&cinfo, &row_ptr, 1);
        }
        jpeg_finish_compress(&cinfo);
    } catch (...) {
        jpeg_destroy_compress(&cinfo);
        if (out_buf) free(out_buf);
        return {};
    }

    std::vector<uint8_t> result(out_buf, out_buf + out_size);
    jpeg_destroy_compress(&cinfo);
    if (out_buf) free(out_buf);
    return result;
}

// Decodes baseline JPEG bytes into RGB24 image.
inline bool decode_jpeg_to_rgb(const uint8_t* data, size_t size, RgbImage* out) {
    if (!data || size < 4 || !out) return false;
    jpeg_decompress_struct dinfo{};
    JpegErrorMgr jerr{};
    dinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = quiet_error_exit;
    jerr.pub.output_message = quiet_output_message;

    jpeg_create_decompress(&dinfo);
    try {
        jpeg_mem_src(&dinfo, data, static_cast<unsigned long>(size));
        if (jpeg_read_header(&dinfo, TRUE) != JPEG_HEADER_OK) {
            jpeg_destroy_decompress(&dinfo);
            return false;
        }
        dinfo.out_color_space = JCS_RGB;
        dinfo.dct_method = JDCT_ISLOW;
        jpeg_start_decompress(&dinfo);
        out->width = static_cast<uint16_t>(dinfo.output_width);
        out->height = static_cast<uint16_t>(dinfo.output_height);
        out->rgb.resize(static_cast<size_t>(out->width) * out->height * 3u);
        while (dinfo.output_scanline < dinfo.output_height) {
            JSAMPROW row_ptr =
                out->rgb.data() + static_cast<size_t>(dinfo.output_scanline) * out->width * 3u;
            jpeg_read_scanlines(&dinfo, &row_ptr, 1);
        }
        jpeg_finish_decompress(&dinfo);
    } catch (...) {
        jpeg_destroy_decompress(&dinfo);
        return false;
    }
    jpeg_destroy_decompress(&dinfo);
    return true;
}

// Computes exact pixel match, MSE, PSNR (dB), and 8x8 windowed luminance SSIM.
inline QualityMetrics compute_quality(const RgbImage& a, const RgbImage& b) {
    QualityMetrics m{};
    if (a.width != b.width || a.height != b.height || a.rgb.size() != b.rgb.size() ||
        a.rgb.empty()) {
        return m;
    }

    double sum_sq = 0.0;
    bool exact = true;
    for (size_t i = 0; i < a.rgb.size(); ++i) {
        const int d = static_cast<int>(a.rgb[i]) - static_cast<int>(b.rgb[i]);
        if (d != 0) {
            exact = false;
            sum_sq += static_cast<double>(d * d);
        }
    }
    m.exact_pixels = exact;
    m.mse = sum_sq / static_cast<double>(a.rgb.size());
    if (exact) {
        m.psnr_db = 99.99;  // Cap display value for exact match (infinite PSNR)
        m.ssim = 1.0;
        return m;
    }
    m.psnr_db = 10.0 * std::log10((255.0 * 255.0) / m.mse);

    // Luminance 8x8 block SSIM (Wang et al. 2004 constants C1, C2)
    constexpr double kC1 = (0.01 * 255.0) * (0.01 * 255.0);
    constexpr double kC2 = (0.03 * 255.0) * (0.03 * 255.0);
    const int bx_cnt = a.width / 8;
    const int by_cnt = a.height / 8;
    double ssim_sum = 0.0;
    int windows = 0;

    auto luma = [](const RgbImage& img, int x, int y) -> double {
        const size_t idx = (static_cast<size_t>(y) * img.width + x) * 3u;
        return 0.299 * img.rgb[idx] + 0.587 * img.rgb[idx + 1] + 0.114 * img.rgb[idx + 2];
    };

    for (int by = 0; by < by_cnt; ++by) {
        for (int bx = 0; bx < bx_cnt; ++bx) {
            double mu_a = 0.0, mu_b = 0.0;
            double va = 0.0, vb = 0.0, cov = 0.0;
            double pa[64], pb[64];
            int p = 0;
            for (int dy = 0; dy < 8; ++dy) {
                for (int dx = 0; dx < 8; ++dx) {
                    const double la = luma(a, bx * 8 + dx, by * 8 + dy);
                    const double lb = luma(b, bx * 8 + dx, by * 8 + dy);
                    pa[p] = la;
                    pb[p] = lb;
                    mu_a += la;
                    mu_b += lb;
                    p++;
                }
            }
            mu_a /= 64.0;
            mu_b /= 64.0;
            for (int i = 0; i < 64; ++i) {
                const double da = pa[i] - mu_a;
                const double db = pb[i] - mu_b;
                va += da * da;
                vb += db * db;
                cov += da * db;
            }
            va /= 63.0;
            vb /= 63.0;
            cov /= 63.0;
            const double num = (2.0 * mu_a * mu_b + kC1) * (2.0 * cov + kC2);
            const double den = (mu_a * mu_a + mu_b * mu_b + kC1) * (va + vb + kC2);
            ssim_sum += num / den;
            windows++;
        }
    }
    m.ssim = (windows > 0) ? (ssim_sum / windows) : 0.0;
    return m;
}

// Deterministic synthetic scene renderer (indoor robotics room with horizon, walls,
// floor tiles, fiducial boxes, and optional pitch/pan/noise/brightness transforms).
struct SceneParams {
    uint16_t width = 320;
    uint16_t height = 240;
    double pan_px = 0.0;
    double pitch_px = 0.0;
    double roll_rad = 0.0;
    double brightness_scale = 1.0;
    double brightness_bias = 0.0;
    double noise_sigma = 0.0;
    uint64_t noise_seed = 0;
    bool high_texture = false;
    bool alt_scene = false;
};

inline RgbImage render_synthetic_scene(const SceneParams& p) {
    RgbImage img;
    img.width = p.width;
    img.height = p.height;
    img.rgb.resize(static_cast<size_t>(p.width) * p.height * 3u);

    const double cx = p.width * 0.5;
    const double cy = p.height * 0.5;
    const double cos_r = std::cos(p.roll_rad);
    const double sin_r = std::sin(p.roll_rad);

    uint64_t rng = p.noise_seed ^ 0x9E3779B97F4A7C15ULL;
    auto next_noise = [&]() -> double {
        if (p.noise_sigma <= 0.0) return 0.0;
        // Fast Irwin-Hall approximate Gaussian from 4 uniform samples (deterministic).
        double s = 0.0;
        for (int k = 0; k < 4; ++k) {
            rng = (rng + 0x9E3779B97F4A7C15ULL);
            uint64_t z = rng;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            z ^= (z >> 31);
            s += static_cast<double>(z & 0xFFFFu) / 65535.0;
        }
        return (s - 2.0) * 1.73205 * p.noise_sigma;
    };

    auto clamp_u8 = [](double v) -> uint8_t {
        if (v < 0.0) return 0;
        if (v > 255.0) return 255;
        return static_cast<uint8_t>(v + 0.5);
    };

    for (int y = 0; y < p.height; ++y) {
        for (int x = 0; x < p.width; ++x) {
            const double dx = x - cx;
            const double dy = y - cy;
            const double wx = (dx * cos_r - dy * sin_r) + cx + p.pan_px;
            const double wy = (dx * sin_r + dy * cos_r) + cy + p.pitch_px;

            double r = 0, g = 0, b = 0;

            if (p.alt_scene) {
                // Distinct second scene for scene-cut tests (warm workshop stripes + circles)
                const double rad = std::hypot(wx - p.width * 0.65, wy - p.height * 0.45);
                if (rad < p.height * 0.28) {
                    r = 215; g = 90; b = 45;
                } else {
                    const int stripe = (static_cast<int>(std::floor((wx + wy) / 20.0)) & 1);
                    r = stripe ? 45 : 85;
                    g = stripe ? 110 : 145;
                    b = stripe ? 160 : 120;
                }
            } else {
                // Robotics lab horizon: ceiling/wall above horizon, tiled floor below
                const double horizon_y = p.height * 0.48;
                if (wy < horizon_y) {
                    // Wall with vertical doorframe, window, and AprilTag-like target
                    const double t = std::max(0.0, std::min(1.0, wy / horizon_y));
                    r = 165.0 + 35.0 * t;
                    g = 178.0 + 30.0 * t;
                    b = 195.0 + 20.0 * t;

                    // Doorframe
                    if (wx > p.width * 0.14 && wx < p.width * 0.34 && wy > p.height * 0.10) {
                        r = 95; g = 74; b = 56;
                        if (wx > p.width * 0.16 && wx < p.width * 0.32 && wy > p.height * 0.13) {
                            r = 140; g = 112; b = 84;
                        }
                    }
                    // High-contrast target poster on wall
                    if (wx > p.width * 0.58 && wx < p.width * 0.80 &&
                        wy > p.height * 0.16 && wy < p.height * 0.40) {
                        const int gx = static_cast<int>(std::floor((wx - p.width * 0.58) / 12.0));
                        const int gy = static_cast<int>(std::floor((wy - p.height * 0.16) / 12.0));
                        const bool dark = ((gx ^ gy) & 1) != 0;
                        r = dark ? 28 : 232;
                        g = dark ? 32 : 235;
                        b = dark ? 38 : 238;
                    }
                } else {
                    // Perspective-like floor tiles
                    const double depth = (wy - horizon_y + 8.0);
                    const double u = (wx - cx) * (48.0 / depth);
                    const double v = 1200.0 / depth;
                    const int tx = static_cast<int>(std::floor(u / 18.0));
                    const int ty = static_cast<int>(std::floor(v / 18.0));
                    const bool tile = ((tx + ty) & 1) == 0;
                    r = tile ? 132.0 : 96.0;
                    g = tile ? 128.0 : 92.0;
                    b = tile ? 120.0 : 88.0;
                }

                // High-texture overlay (fine multi-frequency sine + micro-grid)
                if (p.high_texture) {
                    const double tex =
                        38.0 * std::sin(wx * 0.55) * std::cos(wy * 0.55) +
                        24.0 * std::sin((wx + wy) * 0.9);
                    r += tex;
                    g += tex * 0.85;
                    b -= tex * 0.6;
                }
            }

            const double n = next_noise();
            r = r * p.brightness_scale + p.brightness_bias + n;
            g = g * p.brightness_scale + p.brightness_bias + n;
            b = b * p.brightness_scale + p.brightness_bias + n;

            const size_t idx = (static_cast<size_t>(y) * p.width + x) * 3u;
            img.rgb[idx + 0] = clamp_u8(r);
            img.rgb[idx + 1] = clamp_u8(g);
            img.rgb[idx + 2] = clamp_u8(b);
        }
    }
    return img;
}

}  // namespace ecl::host
