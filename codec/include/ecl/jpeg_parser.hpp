#pragma once

#include <cstddef>
#include <cstdint>

namespace ecl {

constexpr size_t kMaxJpegComponents = 3;
constexpr size_t kMaxQuantTables = 4;
constexpr size_t kMaxHuffTables = 4;
constexpr uint16_t kMaxJpegDim = 2048;
constexpr size_t kMaxTotalBlocks = 65536;  // Hard upper bound for safety against malicious headers

struct JpegComponentInfo {
    uint8_t id = 0;
    uint8_t h_samp = 1;
    uint8_t v_samp = 1;
    uint8_t qt_id = 0;
    uint8_t dc_ht_id = 0;
    uint8_t ac_ht_id = 0;
};

struct JpegHuffmanTable {
    bool present = false;
    uint8_t counts[16]{};     // Number of codes of length 1..16
    uint8_t symbols[256]{};
    uint16_t num_symbols = 0;

    // Derived decode tables
    uint8_t fast_sym[256]{};  // 8-bit fast lookup symbol
    uint8_t fast_len[256]{};  // 0 if code > 8 bits or invalid
    int32_t max_code[17]{};
    int32_t val_offset[17]{};
};

struct JpegFrameInfo {
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t num_components = 0;
    uint8_t max_h_samp = 1;
    uint8_t max_v_samp = 1;
    uint16_t mcu_cols = 0;
    uint16_t mcu_rows = 0;
    uint32_t mcu_count = 0;
    uint8_t blocks_per_mcu = 0;
    uint32_t total_blocks = 0;
    uint16_t restart_interval = 0;
    bool has_dqt[kMaxQuantTables]{};
    uint8_t qtable[kMaxQuantTables][64]{};  // Stored in zigzag order as in DQT
    bool has_dht_dc[kMaxHuffTables]{};
    bool has_dht_ac[kMaxHuffTables]{};
    JpegComponentInfo comp[kMaxJpegComponents]{};
    size_t scan_data_offset = 0;
    size_t scan_data_size = 0;
};

enum class JpegParseStatus : uint8_t {
    OK = 0,
    INVALID_ARGUMENT = 1,
    MISSING_SOI = 2,
    TRUNCATED = 3,
    UNSUPPORTED_SOF = 4,
    INVALID_DIMENSIONS = 5,
    INVALID_DQT = 6,
    INVALID_DHT = 7,
    INVALID_SOS = 8,
    MISSING_TABLE = 9,
    BUFFER_TOO_SMALL = 10,
    ENTROPY_ERROR = 11,
};

// Reusable scratch workspace so zero heap allocation occurs per frame.
struct JpegParserWorkspace {
    JpegHuffmanTable dc_tables[kMaxHuffTables]{};
    JpegHuffmanTable ac_tables[kMaxHuffTables]{};
};

// Builds standard JPEG Annex K default Huffman tables (used as fallback if MJPEG omits DHT).
void init_annex_k_huffman_tables(JpegParserWorkspace* ws);

// Parses baseline JPEG markers (SOI, DQT, DHT, DRI, SOF0, SOS) and validates all bounds.
// Does NOT entropy-decode coefficients yet.
JpegParseStatus parse_jpeg_headers(const uint8_t* data,
                                   size_t size,
                                   JpegFrameInfo* info_out,
                                   JpegParserWorkspace* ws);

// Parses baseline JPEG and entropy-decodes all 8x8 quantized DCT blocks (in zigzag order,
// absolute DC at [0], AC at [1..63]) into caller-owned `blocks_out` (size >= total_blocks * 64).
// Zero heap allocations; safe against arbitrary untrusted/malformed input.
JpegParseStatus parse_jpeg_to_dct(const uint8_t* data,
                                  size_t size,
                                  JpegFrameInfo* info_out,
                                  JpegParserWorkspace* ws,
                                  int16_t* blocks_out,
                                  size_t max_blocks);

}  // namespace ecl
