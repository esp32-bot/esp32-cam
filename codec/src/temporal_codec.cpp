#include "ecl/temporal_codec.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include "ecl/jpeg_writer.hpp"

#ifdef ECL_ENABLE_ZLIB
#include <zlib.h>
#endif

namespace ecl {
namespace {

constexpr uint8_t kDeltaMagic[4] = {'E', 'C', 'L', 'D'};
constexpr uint8_t kSkipMagic[4] = {'S', 'K', 'I', 'P'};
constexpr uint8_t kDeltaVersion = 1;
constexpr size_t kDeltaHeaderBytes = 16;

inline uint64_t fnv1a_64(const uint8_t* data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

inline void write_u16_le(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

inline void write_u32_le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Compact bit-level Exp-Golomb coder for sparse quantized DCT residuals (zero lookup tables).
class DeltaBitWriter {
  public:
    DeltaBitWriter(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

    bool ok() const { return !overflow_; }

    void put_bit(uint32_t b) {
        if (overflow_) return;
        cur_ = static_cast<uint8_t>((cur_ << 1) | (b & 1u));
        bit_cnt_++;
        if (bit_cnt_ == 8) {
            if (pos_ >= cap_) {
                overflow_ = true;
                return;
            }
            buf_[pos_++] = cur_;
            cur_ = 0;
            bit_cnt_ = 0;
        }
    }

    void put_bits(uint32_t val, int nbits) {
        for (int i = nbits - 1; i >= 0; --i) {
            put_bit((val >> i) & 1u);
        }
    }

    // Unsigned 0-order Exp-Golomb: 0->1, 1->010, 2->011, 3->00100, ...
    void put_ue(uint32_t v) {
        const uint32_t code_num = v + 1u;
        int len = 0;
        uint32_t tmp = code_num;
        while (tmp > 1u) {
            tmp >>= 1;
            len++;
        }
        for (int i = 0; i < len; ++i) put_bit(0);
        put_bits(code_num, len + 1);
    }

    // Signed 0-order Exp-Golomb: 0->0, +1->1, -1->2, +2->3, -2->4, ...
    void put_se(int32_t s) {
        const uint32_t u = (s > 0) ? static_cast<uint32_t>(2 * s - 1)
                                   : static_cast<uint32_t>(-2 * s);
        put_ue(u);
    }

    // Non-zero signed Exp-Golomb (for AC residual != 0): +1->0, -1->1, +2->2, -2->3
    void put_se_nonzero(int32_t s) {
        const uint32_t u = (s > 0) ? static_cast<uint32_t>(2 * s - 2)
                                   : static_cast<uint32_t>(-2 * s - 1);
        put_ue(u);
    }

    size_t finish() {
        if (overflow_) return 0;
        if (bit_cnt_ > 0) {
            if (pos_ >= cap_) {
                overflow_ = true;
                return 0;
            }
            buf_[pos_++] = static_cast<uint8_t>(cur_ << (8 - bit_cnt_));
            cur_ = 0;
            bit_cnt_ = 0;
        }
        return pos_;
    }

  private:
    uint8_t* buf_ = nullptr;
    size_t cap_ = 0;
    size_t pos_ = 0;
    uint8_t cur_ = 0;
    int bit_cnt_ = 0;
    bool overflow_ = false;
};

class DeltaBitReader {
  public:
    DeltaBitReader(const uint8_t* buf, size_t size) : buf_(buf), size_(size) {}

    bool ok() const { return !error_; }

    uint32_t get_bit() {
        if (error_) return 0;
        if (bit_cnt_ == 0) {
            if (pos_ >= size_) {
                error_ = true;
                return 0;
            }
            cur_ = buf_[pos_++];
            bit_cnt_ = 8;
        }
        bit_cnt_--;
        return (cur_ >> bit_cnt_) & 1u;
    }

    uint32_t get_bits(int nbits) {
        uint32_t v = 0;
        for (int i = 0; i < nbits; ++i) {
            v = (v << 1) | get_bit();
        }
        return v;
    }

    uint32_t get_ue() {
        int zeros = 0;
        while (!error_ && get_bit() == 0) {
            zeros++;
            if (zeros > 24) {
                error_ = true;
                return 0;
            }
        }
        if (error_) return 0;
        const uint32_t suffix = (zeros > 0) ? get_bits(zeros) : 0u;
        return ((1u << zeros) | suffix) - 1u;
    }

    int32_t get_se() {
        const uint32_t u = get_ue();
        if (error_) return 0;
        if (u == 0) return 0;
        if (u & 1u) return static_cast<int32_t>((u + 1u) >> 1);
        return -static_cast<int32_t>(u >> 1);
    }

    int32_t get_se_nonzero() {
        const uint32_t u = get_ue();
        if (error_) return 0;
        if ((u & 1u) == 0) return static_cast<int32_t>((u >> 1) + 1u);
        return -static_cast<int32_t>((u >> 1) + 1u);
    }

  private:
    const uint8_t* buf_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
    uint8_t cur_ = 0;
    int bit_cnt_ = 0;
    bool error_ = false;
};

inline bool is_dct_mode(CodecMode m) {
    return m == CodecMode::DCT_DC_RESIDUAL || m == CodecMode::DCT_LOW_AC_RESIDUAL ||
           m == CodecMode::DCT_FULL_RESIDUAL || m == CodecMode::DCT_SPARSE_BLOCK;
}

}  // namespace

// -------------------- TemporalCodecContext --------------------

TemporalCodecContext::~TemporalCodecContext() {
    delete[] out_buf_;
    delete[] ref_blocks_;
    delete[] cur_blocks_;
}

bool TemporalCodecContext::init(const TemporalCodecConfig& cfg) {
    delete[] out_buf_;
    delete[] ref_blocks_;
    delete[] cur_blocks_;
    out_buf_ = nullptr;
    ref_blocks_ = nullptr;
    cur_blocks_ = nullptr;
    allocated_bytes_ = 0;
    initialized_ = false;

    if (cfg.max_frame_bytes < 64) return false;
    cfg_ = cfg;

    out_buf_ = new (std::nothrow) uint8_t[cfg_.max_frame_bytes];
    if (!out_buf_) return false;
    allocated_bytes_ += cfg_.max_frame_bytes;

    if (cfg_.enable_dct_buffers && cfg_.max_blocks > 0) {
        const size_t coeff_count = cfg_.max_blocks * 64u;
        ref_blocks_ = new (std::nothrow) int16_t[coeff_count];
        cur_blocks_ = new (std::nothrow) int16_t[coeff_count];
        if (!ref_blocks_ || !cur_blocks_) {
            delete[] ref_blocks_;
            delete[] cur_blocks_;
            ref_blocks_ = nullptr;
            cur_blocks_ = nullptr;
            return false;
        }
        allocated_bytes_ += coeff_count * sizeof(int16_t) * 2u;
    }

    initialized_ = true;
    reset();
    return true;
}

void TemporalCodecContext::reset() {
    has_reference_ = false;
    force_keyframe_ = true;
    ref_frame_id_ = 0;
    chain_depth_ = 0;
    ref_hash_ = 0;
    ref_raw_size_ = 0;
    std::memset(&ref_info_, 0, sizeof(ref_info_));
    stats_ = {};
}

bool TemporalCodecContext::dqt_and_layout_match(const JpegFrameInfo& a,
                                                const JpegFrameInfo& b) const {
    if (a.width != b.width || a.height != b.height || a.num_components != b.num_components ||
        a.mcu_count != b.mcu_count || a.blocks_per_mcu != b.blocks_per_mcu ||
        a.total_blocks != b.total_blocks) {
        return false;
    }
    for (uint8_t c = 0; c < a.num_components; ++c) {
        if (a.comp[c].h_samp != b.comp[c].h_samp || a.comp[c].v_samp != b.comp[c].v_samp ||
            a.comp[c].qt_id != b.comp[c].qt_id) {
            return false;
        }
    }
    for (uint8_t q = 0; q < kMaxQuantTables; ++q) {
        if (a.has_dqt[q] != b.has_dqt[q]) return false;
        if (a.has_dqt[q] && std::memcmp(a.qtable[q], b.qtable[q], 64) != 0) return false;
    }
    return true;
}

CodecStatus TemporalCodecContext::emit_pass_through_keyframe(const EncodedFrameView& frame,
                                                             CodecMode reported_mode,
                                                             bool update_dct_ref,
                                                             CodecOutput* out) {
    if (frame.size > cfg_.max_frame_bytes) {
        return CodecStatus::BUFFER_TOO_SMALL;
    }
    if (frame.size > 0) {
        std::memcpy(out_buf_, frame.data, frame.size);
    }

    uint32_t total_blocks = 0;
    if (update_dct_ref && ref_blocks_) {
        JpegFrameInfo info{};
        const JpegParseStatus st =
            parse_jpeg_to_dct(frame.data, frame.size, &info, &parser_ws_, ref_blocks_, cfg_.max_blocks);
        if (st == JpegParseStatus::OK) {
            ref_info_ = info;
            total_blocks = info.total_blocks;
            has_reference_ = true;
        } else {
            has_reference_ = false;
        }
    } else if (reported_mode == CodecMode::FRAME_SKIP) {
        ref_hash_ = fnv1a_64(frame.data, frame.size);
        ref_raw_size_ = frame.size;
        has_reference_ = true;
    }

    ref_frame_id_ = frame.frame_id;
    chain_depth_ = 0;
    force_keyframe_ = false;

    out->mode = reported_mode;
    out->frame_id = frame.frame_id;
    out->reference_frame_id = frame.frame_id;
    out->is_keyframe = true;
    out->changed_blocks = total_blocks;
    out->total_blocks = total_blocks;
    out->bytes = ConstByteSpan{out_buf_, frame.size};

    stats_.frames_encoded++;
    stats_.keyframes_emitted++;
    stats_.input_bytes += frame.size;
    stats_.output_bytes += frame.size;
    return CodecStatus::OK;
}

CodecStatus TemporalCodecContext::encode(const EncodedFrameView& frame, CodecOutput* out) {
    if (!initialized_ || !out || !frame.data || frame.size == 0) {
        return CodecStatus::INVALID_INPUT;
    }

    // 1. Permanent zero-overhead fallback: PASS_THROUGH
    if (cfg_.mode == CodecMode::PASS_THROUGH) {
        return emit_pass_through_keyframe(frame, CodecMode::PASS_THROUGH, false, out);
    }

    // 2. Host negative-control experiment: GENERIC_ZLIB on raw JPEG bytes
    if (cfg_.mode == CodecMode::GENERIC_ZLIB) {
#ifdef ECL_ENABLE_ZLIB
        if (cfg_.max_frame_bytes < 16) return CodecStatus::BUFFER_TOO_SMALL;
        uLongf dest_len = static_cast<uLongf>(cfg_.max_frame_bytes - 4);
        const int zres =
            compress2(out_buf_ + 4, &dest_len, frame.data, static_cast<uLong>(frame.size), 6);
        if (zres != Z_OK) {
            return CodecStatus::BUFFER_TOO_SMALL;
        }
        write_u32_le(out_buf_, static_cast<uint32_t>(frame.size));
        const size_t total = 4u + static_cast<size_t>(dest_len);
        out->mode = CodecMode::GENERIC_ZLIB;
        out->frame_id = frame.frame_id;
        out->reference_frame_id = frame.frame_id;
        out->is_keyframe = true;
        out->changed_blocks = 0;
        out->total_blocks = 0;
        out->bytes = ConstByteSpan{out_buf_, total};
        stats_.frames_encoded++;
        stats_.keyframes_emitted++;
        stats_.input_bytes += frame.size;
        stats_.output_bytes += total;
        return CodecStatus::OK;
#else
        return CodecStatus::UNSUPPORTED_MODE;
#endif
    }

    const bool need_periodic_kf =
        force_keyframe_ || !has_reference_ ||
        (cfg_.keyframe_interval > 0 && chain_depth_ + 1 >= cfg_.keyframe_interval) ||
        (cfg_.max_dependency_depth > 0 && chain_depth_ >= cfg_.max_dependency_depth);

    // 3. Simple temporal baseline: FRAME_SKIP (exact duplicate frame hash skip)
    if (cfg_.mode == CodecMode::FRAME_SKIP) {
        const uint64_t h = fnv1a_64(frame.data, frame.size);
        if (!need_periodic_kf && h == ref_hash_ && frame.size == ref_raw_size_) {
            std::memcpy(out_buf_, kSkipMagic, 4);
            out->mode = CodecMode::FRAME_SKIP;
            out->frame_id = frame.frame_id;
            out->reference_frame_id = ref_frame_id_;
            out->is_keyframe = false;
            out->changed_blocks = 0;
            out->total_blocks = 0;
            out->bytes = ConstByteSpan{out_buf_, 4};
            ref_frame_id_ = frame.frame_id;
            chain_depth_++;
            stats_.frames_encoded++;
            stats_.deltas_emitted++;
            stats_.skip_frames++;
            stats_.input_bytes += frame.size;
            stats_.output_bytes += 4;
            return CodecStatus::OK;
        }
        return emit_pass_through_keyframe(frame, CodecMode::FRAME_SKIP, false, out);
    }

    // 4. Experimental compressed-domain DCT temporal modes
    if (!is_dct_mode(cfg_.mode)) {
        return CodecStatus::UNSUPPORTED_MODE;
    }
    if (!ref_blocks_ || !cur_blocks_) {
        return CodecStatus::UNSUPPORTED_MODE;
    }

    if (need_periodic_kf) {
        return emit_pass_through_keyframe(frame, cfg_.mode, true, out);
    }

    // Parse current frame into cur_blocks_.
    JpegFrameInfo cur_info{};
    const JpegParseStatus pst =
        parse_jpeg_to_dct(frame.data, frame.size, &cur_info, &parser_ws_, cur_blocks_, cfg_.max_blocks);
    if (pst != JpegParseStatus::OK) {
        // Safe fallback to pass-through keyframe if JPEG uses unsupported features.
        stats_.keyframe_fallbacks++;
        return emit_pass_through_keyframe(frame, cfg_.mode, false, out);
    }

    if (!dqt_and_layout_match(ref_info_, cur_info)) {
        // Resolution, subsampling, or quantization table changed -> must emit keyframe.
        return emit_pass_through_keyframe(frame, cfg_.mode, true, out);
    }

    // Determine spectral limit and change threshold per mode.
    uint8_t max_coeff_idx = 64;
    uint16_t diff_thresh = 0;
    if (cfg_.mode == CodecMode::DCT_DC_RESIDUAL) {
        max_coeff_idx = 1;
    } else if (cfg_.mode == CodecMode::DCT_LOW_AC_RESIDUAL) {
        const uint8_t lim = std::min<uint8_t>(cfg_.low_ac_count, 63);
        max_coeff_idx = static_cast<uint8_t>(1 + lim);
    } else if (cfg_.mode == CodecMode::DCT_FULL_RESIDUAL) {
        max_coeff_idx = 64;
    } else if (cfg_.mode == CodecMode::DCT_SPARSE_BLOCK) {
        max_coeff_idx = 64;
        diff_thresh = cfg_.sparse_block_threshold;
    }

    // Write delta header placeholder.
    if (cfg_.max_frame_bytes < kDeltaHeaderBytes + 64) {
        return CodecStatus::BUFFER_TOO_SMALL;
    }
    std::memcpy(out_buf_, kDeltaMagic, 4);
    out_buf_[4] = kDeltaVersion;
    out_buf_[5] = static_cast<uint8_t>(cfg_.mode);
    out_buf_[6] = max_coeff_idx;
    out_buf_[7] = cur_info.blocks_per_mcu;
    write_u32_le(out_buf_ + 8, cur_info.mcu_count);

    DeltaBitWriter bw(out_buf_ + kDeltaHeaderBytes, cfg_.max_frame_bytes - kDeltaHeaderBytes);

    uint32_t changed_mcus = 0;
    uint32_t changed_blocks = 0;
    const uint8_t bpm = cur_info.blocks_per_mcu;

    // First pass per MCU: decide if MCU changed (closed-loop: unchanged MCUs leave ref_blocks_ untouched!).
    // Write 1-bit per MCU mask, then payload for changed MCUs? To stay single-buffer and fast,
    // we pack the MCU bitmask in fixed bytes right after the 16-byte header!
    const size_t mask_bytes = (cur_info.mcu_count + 7u) / 8u;
    if (kDeltaHeaderBytes + mask_bytes + 16 > cfg_.max_frame_bytes) {
        return CodecStatus::BUFFER_TOO_SMALL;
    }
    uint8_t* mcu_mask = out_buf_ + kDeltaHeaderBytes;
    std::memset(mcu_mask, 0, mask_bytes);

    DeltaBitWriter payload_bw(out_buf_ + kDeltaHeaderBytes + mask_bytes,
                              cfg_.max_frame_bytes - kDeltaHeaderBytes - mask_bytes);

    for (uint32_t mcu = 0; mcu < cur_info.mcu_count; ++mcu) {
        const size_t base_blk = static_cast<size_t>(mcu) * bpm;
        bool mcu_changed = false;

        for (uint8_t b = 0; b < bpm; ++b) {
            const int16_t* cur = cur_blocks_ + (base_blk + b) * 64u;
            const int16_t* ref = ref_blocks_ + (base_blk + b) * 64u;
            for (uint8_t k = 0; k < max_coeff_idx; ++k) {
                const int32_t d = static_cast<int32_t>(cur[k]) - static_cast<int32_t>(ref[k]);
                const uint32_t ad = static_cast<uint32_t>(d < 0 ? -d : d);
                if (ad > diff_thresh) {
                    mcu_changed = true;
                    break;
                }
            }
            if (mcu_changed) break;
        }

        if (!mcu_changed) {
            continue;
        }

        mcu_mask[mcu >> 3] |= static_cast<uint8_t>(1u << (mcu & 7u));
        changed_mcus++;

        for (uint8_t b = 0; b < bpm; ++b) {
            const int16_t* cur = cur_blocks_ + (base_blk + b) * 64u;
            int16_t* ref = ref_blocks_ + (base_blk + b) * 64u;

            bool blk_changed = false;
            for (uint8_t k = 0; k < max_coeff_idx; ++k) {
                const int32_t d = static_cast<int32_t>(cur[k]) - static_cast<int32_t>(ref[k]);
                const uint32_t ad = static_cast<uint32_t>(d < 0 ? -d : d);
                if (ad > diff_thresh) {
                    blk_changed = true;
                    break;
                }
            }

            if (!blk_changed) {
                payload_bw.put_bit(0);  // Block unchanged inside changed MCU
                continue;
            }

            changed_blocks++;
            payload_bw.put_bit(1);  // Block changed

            // DC residual
            const int32_t dc_diff = static_cast<int32_t>(cur[0]) - static_cast<int32_t>(ref[0]);
            payload_bw.put_se(dc_diff);

            if (max_coeff_idx > 1) {
                // Sparse AC residuals (1 .. max_coeff_idx - 1) using RLE + nonzero signed Exp-Golomb
                uint32_t zero_run = 0;
                for (uint8_t k = 1; k < max_coeff_idx; ++k) {
                    int32_t d = static_cast<int32_t>(cur[k]) - static_cast<int32_t>(ref[k]);
                    const uint32_t ad = static_cast<uint32_t>(d < 0 ? -d : d);
                    if (ad <= diff_thresh) {
                        zero_run++;
                    } else {
                        payload_bw.put_bit(1);  // Non-zero entry follows
                        payload_bw.put_ue(zero_run);
                        payload_bw.put_se_nonzero(d);
                        zero_run = 0;
                    }
                }
                if (zero_run > 0) {
                    payload_bw.put_bit(0);  // EOB (only needed when trailing zeros remain)
                }
            }
        }

        if (!payload_bw.ok()) {
            break;
        }
    }

    const size_t bit_bytes = payload_bw.finish();
    const size_t delta_size = kDeltaHeaderBytes + mask_bytes + bit_bytes;
    write_u32_le(out_buf_ + 12, changed_mcus);

    // Optional adaptive fallback: if delta overflowed or is larger than original JPEG, emit keyframe!
    if (!payload_bw.ok() || (cfg_.allow_keyframe_fallback && delta_size >= frame.size)) {
        stats_.keyframe_fallbacks++;
        // Update ref_blocks_ to full current frame and emit raw JPEG keyframe.
        std::memcpy(ref_blocks_, cur_blocks_, static_cast<size_t>(cur_info.total_blocks) * 64u * sizeof(int16_t));
        ref_info_ = cur_info;
        if (frame.size > cfg_.max_frame_bytes) return CodecStatus::BUFFER_TOO_SMALL;
        std::memcpy(out_buf_, frame.data, frame.size);
        ref_frame_id_ = frame.frame_id;
        chain_depth_ = 0;
        has_reference_ = true;
        out->mode = cfg_.mode;
        out->frame_id = frame.frame_id;
        out->reference_frame_id = frame.frame_id;
        out->is_keyframe = true;
        out->changed_blocks = cur_info.total_blocks;
        out->total_blocks = cur_info.total_blocks;
        out->bytes = ConstByteSpan{out_buf_, frame.size};
        stats_.frames_encoded++;
        stats_.keyframes_emitted++;
        stats_.input_bytes += frame.size;
        stats_.output_bytes += frame.size;
        return CodecStatus::OK;
    }

    // Closed-loop reference update: apply EXACTLY the deltas we transmitted to `ref_blocks_`
    // so encoder and decoder references remain 100% bit-identical (zero drift!).
    for (uint32_t mcu = 0; mcu < cur_info.mcu_count; ++mcu) {
        if ((mcu_mask[mcu >> 3] & (1u << (mcu & 7u))) == 0) continue;
        const size_t base_blk = static_cast<size_t>(mcu) * bpm;
        for (uint8_t b = 0; b < bpm; ++b) {
            const int16_t* cur = cur_blocks_ + (base_blk + b) * 64u;
            int16_t* ref = ref_blocks_ + (base_blk + b) * 64u;
            bool blk_changed = false;
            for (uint8_t k = 0; k < max_coeff_idx; ++k) {
                const int32_t d = static_cast<int32_t>(cur[k]) - static_cast<int32_t>(ref[k]);
                const uint32_t ad = static_cast<uint32_t>(d < 0 ? -d : d);
                if (ad > diff_thresh) {
                    blk_changed = true;
                    break;
                }
            }
            if (!blk_changed) continue;
            ref[0] = cur[0];
            for (uint8_t k = 1; k < max_coeff_idx; ++k) {
                const int32_t d = static_cast<int32_t>(cur[k]) - static_cast<int32_t>(ref[k]);
                const uint32_t ad = static_cast<uint32_t>(d < 0 ? -d : d);
                if (ad > diff_thresh) {
                    ref[k] = cur[k];
                }
            }
        }
    }

    out->mode = cfg_.mode;
    out->frame_id = frame.frame_id;
    out->reference_frame_id = ref_frame_id_;
    out->is_keyframe = false;
    out->changed_blocks = changed_blocks;
    out->total_blocks = cur_info.total_blocks;
    out->bytes = ConstByteSpan{out_buf_, delta_size};

    ref_frame_id_ = frame.frame_id;
    chain_depth_++;
    stats_.frames_encoded++;
    stats_.deltas_emitted++;
    stats_.input_bytes += frame.size;
    stats_.output_bytes += delta_size;
    return CodecStatus::OK;
}

// -------------------- TemporalDecoderContext --------------------

TemporalDecoderContext::~TemporalDecoderContext() {
    delete[] out_buf_;
    delete[] ref_raw_buf_;
    delete[] ref_blocks_;
}

bool TemporalDecoderContext::init(const TemporalCodecConfig& cfg) {
    delete[] out_buf_;
    delete[] ref_raw_buf_;
    delete[] ref_blocks_;
    out_buf_ = nullptr;
    ref_raw_buf_ = nullptr;
    ref_blocks_ = nullptr;
    allocated_bytes_ = 0;
    initialized_ = false;

    if (cfg.max_frame_bytes < 64) return false;
    cfg_ = cfg;

    out_buf_ = new (std::nothrow) uint8_t[cfg_.max_frame_bytes];
    ref_raw_buf_ = new (std::nothrow) uint8_t[cfg_.max_frame_bytes];
    if (!out_buf_ || !ref_raw_buf_) return false;
    allocated_bytes_ += cfg_.max_frame_bytes * 2u;

    if (cfg_.enable_dct_buffers && cfg_.max_blocks > 0) {
        const size_t coeff_count = cfg_.max_blocks * 64u;
        ref_blocks_ = new (std::nothrow) int16_t[coeff_count];
        if (!ref_blocks_) return false;
        allocated_bytes_ += coeff_count * sizeof(int16_t);
    }

    initialized_ = true;
    reset();
    return true;
}

void TemporalDecoderContext::reset() {
    has_reference_ = false;
    has_dct_ref_ = false;
    ref_frame_id_ = 0;
    chain_depth_ = 0;
    ref_raw_size_ = 0;
    std::memset(&ref_info_, 0, sizeof(ref_info_));
    stats_ = {};
}

DecodeStatus TemporalDecoderContext::decode(const PacketizedFrame& input, ReconstructedFrame* out) {
    if (!initialized_ || !out || input.payload.empty()) {
        return DecodeStatus::INVALID_INPUT;
    }

    // 1. PASS_THROUGH: byte-identical recovery.
    if (input.mode == CodecMode::PASS_THROUGH) {
        if (input.payload.size > cfg_.max_frame_bytes) {
            return DecodeStatus::BUFFER_TOO_SMALL;
        }
        std::memcpy(out_buf_, input.payload.data, input.payload.size);
        out->frame_id = input.frame_id;
        out->reference_frame_id = input.frame_id;
        out->is_keyframe = true;
        out->exact_byte_match = true;
        out->width = input.metadata.width;
        out->height = input.metadata.height;
        out->bytes = ConstByteSpan{out_buf_, input.payload.size};
        stats_.frames_decoded++;
        stats_.keyframes_decoded++;
        return DecodeStatus::OK;
    }

    // 2. GENERIC_ZLIB control mode
    if (input.mode == CodecMode::GENERIC_ZLIB) {
#ifdef ECL_ENABLE_ZLIB
        if (input.payload.size < 5) return DecodeStatus::INVALID_INPUT;
        const uint32_t expected_len = read_u32_le(input.payload.data);
        if (expected_len == 0 || expected_len > cfg_.max_frame_bytes) {
            return DecodeStatus::BUFFER_TOO_SMALL;
        }
        uLongf dest_len = expected_len;
        const int zres = uncompress(out_buf_, &dest_len, input.payload.data + 4,
                                    static_cast<uLong>(input.payload.size - 4));
        if (zres != Z_OK || dest_len != expected_len) {
            stats_.parse_errors++;
            return DecodeStatus::INVALID_INPUT;
        }
        out->frame_id = input.frame_id;
        out->reference_frame_id = input.frame_id;
        out->is_keyframe = true;
        out->exact_byte_match = true;
        out->width = input.metadata.width;
        out->height = input.metadata.height;
        out->bytes = ConstByteSpan{out_buf_, expected_len};
        stats_.frames_decoded++;
        stats_.keyframes_decoded++;
        return DecodeStatus::OK;
#else
        return DecodeStatus::UNSUPPORTED_MODE;
#endif
    }

    // 3. Keyframe for FRAME_SKIP or any DCT temporal mode: payload is a standard JPEG!
    if (input.is_keyframe) {
        if (input.payload.size > cfg_.max_frame_bytes) {
            return DecodeStatus::BUFFER_TOO_SMALL;
        }
        std::memcpy(out_buf_, input.payload.data, input.payload.size);
        std::memcpy(ref_raw_buf_, input.payload.data, input.payload.size);
        ref_raw_size_ = input.payload.size;
        ref_frame_id_ = input.frame_id;
        chain_depth_ = 0;
        has_reference_ = true;
        has_dct_ref_ = false;

        uint16_t w = input.metadata.width;
        uint16_t h = input.metadata.height;
        if (is_dct_mode(input.mode) && ref_blocks_) {
            JpegFrameInfo info{};
            const JpegParseStatus pst = parse_jpeg_to_dct(
                input.payload.data, input.payload.size, &info, &parser_ws_, ref_blocks_, cfg_.max_blocks);
            if (pst == JpegParseStatus::OK) {
                ref_info_ = info;
                has_dct_ref_ = true;
                w = info.width;
                h = info.height;
            }
        }

        out->frame_id = input.frame_id;
        out->reference_frame_id = input.frame_id;
        out->is_keyframe = true;
        out->exact_byte_match = true;
        out->width = w;
        out->height = h;
        out->bytes = ConstByteSpan{out_buf_, input.payload.size};
        stats_.frames_decoded++;
        stats_.keyframes_decoded++;
        return DecodeStatus::OK;
    }

    // 4. Delta frame: enforce strict loss-safe reference verification & bounded depth!
    if (!has_reference_ || input.reference_frame_id != ref_frame_id_) {
        stats_.missing_reference_drops++;
        // Invalidate stale chain so subsequent deltas depending on this broken chain cannot apply.
        has_reference_ = false;
        has_dct_ref_ = false;
        return DecodeStatus::MISSING_REFERENCE;
    }
    if (cfg_.max_dependency_depth > 0 && chain_depth_ >= cfg_.max_dependency_depth) {
        stats_.depth_exceeded_drops++;
        has_reference_ = false;
        has_dct_ref_ = false;
        return DecodeStatus::DEPENDENCY_DEPTH_EXCEEDED;
    }

    // 4a. FRAME_SKIP delta
    if (input.mode == CodecMode::FRAME_SKIP) {
        if (input.payload.size != 4 || std::memcmp(input.payload.data, kSkipMagic, 4) != 0) {
            stats_.parse_errors++;
            return DecodeStatus::INVALID_INPUT;
        }
        std::memcpy(out_buf_, ref_raw_buf_, ref_raw_size_);
        ref_frame_id_ = input.frame_id;
        chain_depth_++;
        out->frame_id = input.frame_id;
        out->reference_frame_id = input.reference_frame_id;
        out->is_keyframe = false;
        out->exact_byte_match = true;
        out->width = input.metadata.width;
        out->height = input.metadata.height;
        out->bytes = ConstByteSpan{out_buf_, ref_raw_size_};
        stats_.frames_decoded++;
        stats_.deltas_decoded++;
        return DecodeStatus::OK;
    }

    // 4b. DCT temporal delta
    if (!is_dct_mode(input.mode)) {
        return DecodeStatus::UNSUPPORTED_MODE;
    }
    if (!has_dct_ref_ || !ref_blocks_) {
        stats_.missing_reference_drops++;
        has_reference_ = false;
        return DecodeStatus::MISSING_REFERENCE;
    }

    if (input.payload.size < kDeltaHeaderBytes) {
        stats_.parse_errors++;
        return DecodeStatus::INVALID_INPUT;
    }
    if (std::memcmp(input.payload.data, kDeltaMagic, 4) != 0 ||
        input.payload.data[4] != kDeltaVersion) {
        stats_.parse_errors++;
        return DecodeStatus::INVALID_INPUT;
    }

    const uint8_t max_coeff_idx = input.payload.data[6];
    const uint8_t bpm = input.payload.data[7];
    const uint32_t mcu_count = read_u32_le(input.payload.data + 8);
    if (max_coeff_idx == 0 || max_coeff_idx > 64 || bpm != ref_info_.blocks_per_mcu ||
        mcu_count != ref_info_.mcu_count) {
        stats_.parse_errors++;
        has_reference_ = false;
        has_dct_ref_ = false;
        return DecodeStatus::INVALID_INPUT;
    }

    const size_t mask_bytes = (mcu_count + 7u) / 8u;
    if (kDeltaHeaderBytes + mask_bytes > input.payload.size) {
        stats_.parse_errors++;
        has_reference_ = false;
        has_dct_ref_ = false;
        return DecodeStatus::INVALID_INPUT;
    }

    const uint8_t* mcu_mask = input.payload.data + kDeltaHeaderBytes;
    DeltaBitReader br(input.payload.data + kDeltaHeaderBytes + mask_bytes,
                      input.payload.size - kDeltaHeaderBytes - mask_bytes);

    for (uint32_t mcu = 0; mcu < mcu_count; ++mcu) {
        if ((mcu_mask[mcu >> 3] & (1u << (mcu & 7u))) == 0) continue;
        const size_t base_blk = static_cast<size_t>(mcu) * bpm;

        for (uint8_t b = 0; b < bpm; ++b) {
            const uint32_t blk_flag = br.get_bit();
            if (!br.ok()) {
                stats_.parse_errors++;
                has_reference_ = false;
                has_dct_ref_ = false;
                return DecodeStatus::JPEG_PARSE_ERROR;
            }
            if (blk_flag == 0) continue;

            int16_t* ref = ref_blocks_ + (base_blk + b) * 64u;
            const int32_t dc_diff = br.get_se();
            if (!br.ok()) {
                stats_.parse_errors++;
                has_reference_ = false;
                has_dct_ref_ = false;
                return DecodeStatus::JPEG_PARSE_ERROR;
            }
            int32_t new_dc = static_cast<int32_t>(ref[0]) + dc_diff;
            if (new_dc < -32768) new_dc = -32768;
            if (new_dc > 32767) new_dc = 32767;
            ref[0] = static_cast<int16_t>(new_dc);

            if (max_coeff_idx > 1) {
                uint32_t k = 1;
                while (k < max_coeff_idx) {
                    const uint32_t has_nz = br.get_bit();
                    if (!br.ok()) {
                        stats_.parse_errors++;
                        has_reference_ = false;
                        has_dct_ref_ = false;
                        return DecodeStatus::JPEG_PARSE_ERROR;
                    }
                    if (has_nz == 0) break;  // EOB
                    const uint32_t zrun = br.get_ue();
                    const int32_t diff = br.get_se_nonzero();
                    if (!br.ok() || zrun >= max_coeff_idx || k + zrun >= max_coeff_idx) {
                        stats_.parse_errors++;
                        has_reference_ = false;
                        has_dct_ref_ = false;
                        return DecodeStatus::JPEG_PARSE_ERROR;
                    }
                    k += zrun;
                    int32_t nv = static_cast<int32_t>(ref[k]) + diff;
                    if (nv < -32768) nv = -32768;
                    if (nv > 32767) nv = 32767;
                    ref[k] = static_cast<int16_t>(nv);
                    k++;
                }
            }
        }
    }

    // Re-encode updated quantized DCT reference into a valid standard baseline JPEG!
    const size_t jpg_len =
        write_dct_to_jpeg(ref_info_, ref_blocks_, ref_info_.total_blocks, out_buf_, cfg_.max_frame_bytes);
    if (jpg_len == 0) {
        stats_.parse_errors++;
        return DecodeStatus::BUFFER_TOO_SMALL;
    }

    ref_frame_id_ = input.frame_id;
    chain_depth_++;
    out->frame_id = input.frame_id;
    out->reference_frame_id = input.reference_frame_id;
    out->is_keyframe = false;
    out->exact_byte_match = false;
    out->width = ref_info_.width;
    out->height = ref_info_.height;
    out->bytes = ConstByteSpan{out_buf_, jpg_len};
    stats_.frames_decoded++;
    stats_.deltas_decoded++;
    return DecodeStatus::OK;
}

}  // namespace ecl
