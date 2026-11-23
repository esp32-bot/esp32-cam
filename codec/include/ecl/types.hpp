#pragma once

#include <cstddef>
#include <cstdint>

namespace ecl {

template <typename T>
struct Span {
    T* data = nullptr;
    size_t size = 0;

    constexpr bool empty() const { return size == 0 || data == nullptr; }
    constexpr T* begin() const { return data; }
    constexpr T* end() const { return data ? (data + size) : nullptr; }
    constexpr T& operator[](size_t i) const { return data[i]; }
};

using ByteSpan = Span<uint8_t>;
using ConstByteSpan = Span<const uint8_t>;

enum class CodecMode : uint8_t {
    PASS_THROUGH = 0,
    FRAME_SKIP = 1,
    GENERIC_ZLIB = 2,
    DCT_DC_RESIDUAL = 3,
    DCT_LOW_AC_RESIDUAL = 4,
    DCT_FULL_RESIDUAL = 5,
    DCT_SPARSE_BLOCK = 6,
};

inline const char* codec_mode_name(CodecMode mode) {
    switch (mode) {
        case CodecMode::PASS_THROUGH:        return "PASS_THROUGH";
        case CodecMode::FRAME_SKIP:          return "FRAME_SKIP";
        case CodecMode::GENERIC_ZLIB:        return "GENERIC_ZLIB";
        case CodecMode::DCT_DC_RESIDUAL:     return "DCT_DC_RESIDUAL";
        case CodecMode::DCT_LOW_AC_RESIDUAL: return "DCT_LOW_AC_RESIDUAL";
        case CodecMode::DCT_FULL_RESIDUAL:   return "DCT_FULL_RESIDUAL";
        case CodecMode::DCT_SPARSE_BLOCK:    return "DCT_SPARSE_BLOCK";
        default:                             return "UNKNOWN";
    }
}

struct FrameMetadata {
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t quality = 0;
    uint8_t flags = 0;
    bool has_attitude = false;
    int16_t pitch_cdeg = 0;
    int16_t roll_cdeg = 0;
};

struct EncodedFrameView {
    uint32_t frame_id = 0;
    uint64_t capture_timestamp_us = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
    FrameMetadata metadata{};
};

struct CodecOutput {
    CodecMode mode = CodecMode::PASS_THROUGH;
    uint32_t frame_id = 0;
    uint32_t reference_frame_id = 0;
    bool is_keyframe = true;
    uint32_t changed_blocks = 0;
    uint32_t total_blocks = 0;
    ConstByteSpan bytes{};
};

enum class CodecStatus : uint8_t {
    OK = 0,
    INVALID_INPUT = 1,
    BUFFER_TOO_SMALL = 2,
    UNSUPPORTED_MODE = 3,
    JPEG_PARSE_ERROR = 4,
    INTERNAL_ERROR = 5,
};

enum class DecodeStatus : uint8_t {
    OK = 0,
    MISSING_REFERENCE = 1,
    DEPENDENCY_DEPTH_EXCEEDED = 2,
    INVALID_INPUT = 3,
    BUFFER_TOO_SMALL = 4,
    UNSUPPORTED_MODE = 5,
    JPEG_PARSE_ERROR = 6,
};

struct PacketizedFrame {
    CodecMode mode = CodecMode::PASS_THROUGH;
    uint32_t frame_id = 0;
    uint32_t reference_frame_id = 0;
    uint32_t timestamp_us_low = 0;
    bool is_keyframe = true;
    FrameMetadata metadata{};
    ConstByteSpan payload{};
};

struct ReconstructedFrame {
    uint32_t frame_id = 0;
    uint32_t reference_frame_id = 0;
    bool is_keyframe = true;
    bool exact_byte_match = false;
    uint16_t width = 0;
    uint16_t height = 0;
    ConstByteSpan bytes{};
};

// Modular 32-bit sequence comparison: returns true if `a` is strictly newer than `b`.
inline constexpr bool frame_id_newer(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

inline constexpr int32_t frame_id_diff(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b);
}

}  // namespace ecl
