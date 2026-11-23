#pragma once

#include <cstddef>
#include <cstdint>

#include "ecl/jpeg_parser.hpp"
#include "ecl/types.hpp"

namespace ecl {

struct TemporalCodecConfig {
    CodecMode mode = CodecMode::PASS_THROUGH;
    uint32_t keyframe_interval = 30;       // Force periodic keyframe every N frames
    uint32_t max_dependency_depth = 30;    // Hard cap on delta chain length
    size_t max_frame_bytes = 131072;       // Bounded output/scratch size (128 KB default)
    size_t max_blocks = 8192;              // Bounded DCT block capacity (VGA 4:2:0 = 7200 blocks)
    uint8_t low_ac_count = 15;             // Number of zigzag AC coeffs kept in DCT_LOW_AC_RESIDUAL
    uint16_t sparse_block_threshold = 0;   // Max abs coeff diff threshold for DCT_SPARSE_BLOCK (0 = exact)
    bool allow_keyframe_fallback = true;   // Emit raw keyframe if delta >= raw JPEG size
    bool enable_dct_buffers = true;        // Set false for zero-DCT-RAM builds (PASS_THROUGH/FRAME_SKIP only)
};

struct TemporalCodecStats {
    uint64_t frames_encoded = 0;
    uint64_t keyframes_emitted = 0;
    uint64_t deltas_emitted = 0;
    uint64_t keyframe_fallbacks = 0;
    uint64_t skip_frames = 0;
    uint64_t input_bytes = 0;
    uint64_t output_bytes = 0;
};

struct TemporalDecoderStats {
    uint64_t frames_decoded = 0;
    uint64_t keyframes_decoded = 0;
    uint64_t deltas_decoded = 0;
    uint64_t missing_reference_drops = 0;
    uint64_t depth_exceeded_drops = 0;
    uint64_t parse_errors = 0;
};

class TemporalCodecContext {
  public:
    TemporalCodecContext() = default;
    ~TemporalCodecContext();

    TemporalCodecContext(const TemporalCodecContext&) = delete;
    TemporalCodecContext& operator=(const TemporalCodecContext&) = delete;

    // Allocates all bounded buffers once. Zero dynamic allocation in `encode()`.
    bool init(const TemporalCodecConfig& cfg);
    void reset();

    void set_mode(CodecMode mode) { cfg_.mode = mode; force_keyframe_ = true; }
    CodecMode mode() const { return cfg_.mode; }
    void force_next_keyframe() { force_keyframe_ = true; }

    CodecStatus encode(const EncodedFrameView& frame, CodecOutput* out);

    const TemporalCodecStats& stats() const { return stats_; }
    size_t allocated_bytes() const { return allocated_bytes_; }

  private:
    bool dqt_and_layout_match(const JpegFrameInfo& a, const JpegFrameInfo& b) const;
    CodecStatus emit_pass_through_keyframe(const EncodedFrameView& frame,
                                           CodecMode reported_mode,
                                           bool update_dct_ref,
                                           CodecOutput* out);

    TemporalCodecConfig cfg_{};
    bool initialized_ = false;
    bool has_reference_ = false;
    bool force_keyframe_ = true;
    uint32_t ref_frame_id_ = 0;
    uint32_t chain_depth_ = 0;
    uint64_t ref_hash_ = 0;
    size_t ref_raw_size_ = 0;

    JpegFrameInfo ref_info_{};
    JpegParserWorkspace parser_ws_{};

    uint8_t* out_buf_ = nullptr;
    int16_t* ref_blocks_ = nullptr;
    int16_t* cur_blocks_ = nullptr;
    size_t allocated_bytes_ = 0;
    TemporalCodecStats stats_{};
};

class TemporalDecoderContext {
  public:
    TemporalDecoderContext() = default;
    ~TemporalDecoderContext();

    TemporalDecoderContext(const TemporalDecoderContext&) = delete;
    TemporalDecoderContext& operator=(const TemporalDecoderContext&) = delete;

    // Allocates all bounded buffers once. Zero dynamic allocation in `decode()`.
    bool init(const TemporalCodecConfig& cfg);
    void reset();

    DecodeStatus decode(const PacketizedFrame& input, ReconstructedFrame* out);

    bool has_valid_reference() const { return has_reference_; }
    uint32_t reference_frame_id() const { return ref_frame_id_; }
    const TemporalDecoderStats& stats() const { return stats_; }
    size_t allocated_bytes() const { return allocated_bytes_; }

  private:
    TemporalCodecConfig cfg_{};
    bool initialized_ = false;
    bool has_reference_ = false;
    uint32_t ref_frame_id_ = 0;
    uint32_t chain_depth_ = 0;
    size_t ref_raw_size_ = 0;
    bool has_dct_ref_ = false;

    JpegFrameInfo ref_info_{};
    JpegParserWorkspace parser_ws_{};

    uint8_t* out_buf_ = nullptr;
    uint8_t* ref_raw_buf_ = nullptr;
    int16_t* ref_blocks_ = nullptr;
    size_t allocated_bytes_ = 0;
    TemporalDecoderStats stats_{};
};

// Free-function convenience wrappers matching the narrow C-style API boundary.
inline CodecStatus codec_encode(TemporalCodecContext* ctx,
                                const EncodedFrameView& frame,
                                CodecOutput* out) {
    return ctx ? ctx->encode(frame, out) : CodecStatus::INVALID_INPUT;
}

inline DecodeStatus codec_decode(TemporalDecoderContext* ctx,
                                 const PacketizedFrame& input,
                                 ReconstructedFrame* out) {
    return ctx ? ctx->decode(input, out) : DecodeStatus::INVALID_INPUT;
}

}  // namespace ecl
