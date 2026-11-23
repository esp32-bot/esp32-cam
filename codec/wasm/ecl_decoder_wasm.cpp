#include <cstdint>
#include <cstring>

#include "ecl/temporal_codec.hpp"

extern "C" {

void* ecl_dec_create(uint32_t max_frame_bytes, uint32_t max_blocks) {
    auto* ctx = new ecl::TemporalDecoderContext();
    ecl::TemporalCodecConfig cfg{};
    cfg.mode = ecl::CodecMode::DCT_SPARSE_BLOCK;
    cfg.max_dependency_depth = 60;
    cfg.max_frame_bytes = max_frame_bytes;
    cfg.max_blocks = max_blocks;
    cfg.enable_dct_buffers = true;
    if (!ctx->init(cfg)) {
        delete ctx;
        return nullptr;
    }
    return ctx;
}

void ecl_dec_destroy(void* ptr) {
    delete static_cast<ecl::TemporalDecoderContext*>(ptr);
}

void ecl_dec_reset(void* ptr) {
    if (ptr) static_cast<ecl::TemporalDecoderContext*>(ptr)->reset();
}

// Returns reconstructed JPEG byte length (>0), or negative DecodeStatus on error.
int32_t ecl_dec_decode(void* ptr,
                       uint32_t codec_mode,
                       uint32_t frame_id,
                       uint32_t ref_frame_id,
                       uint32_t is_keyframe,
                       uint32_t width,
                       uint32_t height,
                       const uint8_t* in_data,
                       uint32_t in_len,
                       uint8_t* out_buf,
                       uint32_t out_cap) {
    if (!ptr || !out_buf) return -100;
    auto* ctx = static_cast<ecl::TemporalDecoderContext*>(ptr);

    ecl::PacketizedFrame pf{};
    pf.mode = static_cast<ecl::CodecMode>(codec_mode);
    pf.frame_id = frame_id;
    pf.reference_frame_id = ref_frame_id;
    pf.is_keyframe = (is_keyframe != 0);
    pf.metadata.width = static_cast<uint16_t>(width);
    pf.metadata.height = static_cast<uint16_t>(height);
    pf.payload = {in_data, in_len};

    ecl::ReconstructedFrame out{};
    const ecl::DecodeStatus st = ctx->decode(pf, &out);
    if (st != ecl::DecodeStatus::OK) {
        return -static_cast<int32_t>(st);
    }
    if (!out.bytes.data || out.bytes.size == 0 || out.bytes.size > out_cap) {
        return -101;
    }
    std::memcpy(out_buf, out.bytes.data, out.bytes.size);
    return static_cast<int32_t>(out.bytes.size);
}

}  // extern "C"
