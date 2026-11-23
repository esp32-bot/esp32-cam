#pragma once

#include <cstddef>
#include <cstdint>

#include "ecl/jpeg_parser.hpp"

namespace ecl {

// Re-encodes quantized 8x8 DCT blocks (in zigzag order, absolute DC at [0], AC at [1..63])
// into a standard baseline JFIF/JPEG bitstream using Annex K Huffman tables.
// Zero dynamic allocations; writes strictly within `out_capacity`.
// Returns encoded byte length, or 0 on error / buffer too small.
size_t write_dct_to_jpeg(const JpegFrameInfo& info,
                         const int16_t* blocks,
                         size_t num_blocks,
                         uint8_t* out_buf,
                         size_t out_capacity);

}  // namespace ecl
