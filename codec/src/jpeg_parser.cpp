#include "ecl/jpeg_parser.hpp"

#include <cstring>

namespace ecl {
namespace {

// Standard JPEG Annex K Huffman tables (luminance & chrominance DC/AC).
constexpr uint8_t kDcLumBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
constexpr uint8_t kDcLumVal[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

constexpr uint8_t kDcChrBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
constexpr uint8_t kDcChrVal[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

constexpr uint8_t kAcLumBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125};
constexpr uint8_t kAcLumVal[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
    0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA};

constexpr uint8_t kAcChrBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119};
constexpr uint8_t kAcChrVal[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
    0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA};

inline uint16_t read_u16_be(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

bool build_huffman_table(JpegHuffmanTable* ht,
                         const uint8_t counts[16],
                         const uint8_t* symbols,
                         uint16_t num_symbols) {
    if (!ht || num_symbols > 256) return false;
    std::memset(ht, 0, sizeof(*ht));
    std::memcpy(ht->counts, counts, 16);
    if (num_symbols > 0) {
        std::memcpy(ht->symbols, symbols, num_symbols);
    }
    ht->num_symbols = num_symbols;

    uint32_t code = 0;
    uint16_t idx = 0;
    for (int len = 1; len <= 16; ++len) {
        ht->val_offset[len] = static_cast<int32_t>(idx) - static_cast<int32_t>(code);
        const uint8_t cnt = counts[len - 1];
        if (cnt > 0) {
            if (idx + cnt > num_symbols) return false;
            if (code + cnt > (1u << len)) return false;
            if (len <= 8) {
                for (uint8_t j = 0; j < cnt; ++j) {
                    const uint32_t c = code + j;
                    const uint8_t sym = symbols[idx + j];
                    const uint32_t shift = static_cast<uint32_t>(8 - len);
                    const uint32_t start = c << shift;
                    const uint32_t end = (c + 1u) << shift;
                    for (uint32_t k = start; k < end; ++k) {
                        ht->fast_sym[k] = sym;
                        ht->fast_len[k] = static_cast<uint8_t>(len);
                    }
                }
            }
            code += cnt;
            idx = static_cast<uint16_t>(idx + cnt);
            ht->max_code[len] = static_cast<int32_t>(code - 1u);
        } else {
            ht->max_code[len] = -1;
        }
        code <<= 1;
    }

    ht->present = true;
    return idx == num_symbols;
}

class BitReader {
  public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool ok() const { return !error_; }
    size_t pos() const { return pos_; }

    void reset_bit_buffer() {
        bit_buf_ = 0;
        bit_count_ = 0;
    }

    bool consume_restart_marker(uint8_t expected_rst_index) {
        reset_bit_buffer();
        // Skip optional 0xFF padding bytes before RSTm.
        while (pos_ < size_ && data_[pos_] != 0xFF) {
            // Trailing padding bits are already discarded; unexpected non-marker byte is an error.
            error_ = true;
            return false;
        }
        while (pos_ < size_ && data_[pos_] == 0xFF) {
            pos_++;
        }
        if (pos_ >= size_) {
            error_ = true;
            return false;
        }
        const uint8_t marker = data_[pos_++];
        if (marker != static_cast<uint8_t>(0xD0u + (expected_rst_index & 7u))) {
            error_ = true;
            return false;
        }
        hit_marker_ = false;
        padded_bytes_ = 0;
        return true;
    }

    void fill_bits() {
        while (bit_count_ <= 24) {
            if (hit_marker_ || pos_ >= size_) {
                // Pad with 0 bits after end of scan so remaining codes terminate safely or fail.
                bit_buf_ <<= 8;
                bit_count_ += 8;
                padded_bytes_++;
                if (padded_bytes_ > 8) {
                    error_ = true;
                }
                return;
            }
            const uint8_t b = data_[pos_++];
            if (b == 0xFF) {
                // Skip repeated 0xFF fill bytes.
                while (pos_ < size_ && data_[pos_] == 0xFF) {
                    pos_++;
                }
                if (pos_ >= size_) {
                    hit_marker_ = true;
                    bit_buf_ <<= 8;
                    bit_count_ += 8;
                    padded_bytes_++;
                    return;
                }
                const uint8_t next = data_[pos_++];
                if (next == 0x00) {
                    // Byte-stuffed literal 0xFF.
                    bit_buf_ = (bit_buf_ << 8) | 0xFFu;
                    bit_count_ += 8;
                } else {
                    // Reached a real marker (EOI or RSTm); rewind so caller can inspect it.
                    pos_ -= 2;
                    hit_marker_ = true;
                    bit_buf_ <<= 8;
                    bit_count_ += 8;
                    padded_bytes_++;
                    return;
                }
            } else {
                bit_buf_ = (bit_buf_ << 8) | b;
                bit_count_ += 8;
            }
        }
    }

    inline int decode_huffman(const JpegHuffmanTable& ht) {
        if (bit_count_ < 16) fill_bits();
        if (error_) return -1;

        const uint8_t top8 = static_cast<uint8_t>((bit_buf_ >> (bit_count_ - 8)) & 0xFFu);
        const uint8_t flen = ht.fast_len[top8];
        if (flen > 0) {
            bit_count_ -= flen;
            return ht.fast_sym[top8];
        }

        // Slow canonical decode for codes 9..16 bits.
        int32_t code = 0;
        for (int len = 1; len <= 16; ++len) {
            if (bit_count_ < 1) {
                fill_bits();
                if (error_) return -1;
            }
            bit_count_--;
            code = (code << 1) | static_cast<int32_t>((bit_buf_ >> bit_count_) & 1u);
            if (code <= ht.max_code[len]) {
                const int32_t idx = code + ht.val_offset[len];
                if (idx < 0 || idx >= ht.num_symbols) {
                    error_ = true;
                    return -1;
                }
                return ht.symbols[idx];
            }
        }
        error_ = true;
        return -1;
    }

    inline int32_t receive_extend(int nbits) {
        if (nbits == 0) return 0;
        if (nbits < 0 || nbits > 15) {
            error_ = true;
            return 0;
        }
        if (bit_count_ < nbits) {
            fill_bits();
            if (bit_count_ < nbits) {
                fill_bits();
            }
        }
        if (error_ || bit_count_ < nbits) {
            error_ = true;
            return 0;
        }
        bit_count_ -= nbits;
        const int32_t val =
            static_cast<int32_t>((bit_buf_ >> bit_count_) & ((1u << nbits) - 1u));
        const int32_t threshold = 1 << (nbits - 1);
        if (val < threshold) {
            return val + ((-1) << nbits) + 1;
        }
        return val;
    }

  private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
    uint32_t bit_buf_ = 0;
    int bit_count_ = 0;
    bool hit_marker_ = false;
    bool error_ = false;
    int padded_bytes_ = 0;
};

}  // namespace

void init_annex_k_huffman_tables(JpegParserWorkspace* ws) {
    if (!ws) return;
    std::memset(ws, 0, sizeof(*ws));
    build_huffman_table(&ws->dc_tables[0], kDcLumBits, kDcLumVal, sizeof(kDcLumVal));
    build_huffman_table(&ws->dc_tables[1], kDcChrBits, kDcChrVal, sizeof(kDcChrVal));
    build_huffman_table(&ws->ac_tables[0], kAcLumBits, kAcLumVal, sizeof(kAcLumVal));
    build_huffman_table(&ws->ac_tables[1], kAcChrBits, kAcChrVal, sizeof(kAcChrVal));
}

JpegParseStatus parse_jpeg_headers(const uint8_t* data,
                                   size_t size,
                                   JpegFrameInfo* info_out,
                                   JpegParserWorkspace* ws) {
    if (!data || !info_out || !ws) {
        return JpegParseStatus::INVALID_ARGUMENT;
    }
    if (size < 4) {
        return JpegParseStatus::TRUNCATED;
    }
    if (data[0] != 0xFF || data[1] != 0xD8) {
        return JpegParseStatus::MISSING_SOI;
    }

    std::memset(info_out, 0, sizeof(*info_out));
    std::memset(ws, 0, sizeof(*ws));

    bool saw_sof0 = false;
    bool saw_sos = false;
    size_t pos = 2;
    size_t marker_iters = 0;

    while (pos < size) {
        if (++marker_iters > 1024) {
            return JpegParseStatus::TRUNCATED;
        }
        if (data[pos] != 0xFF) {
            return JpegParseStatus::TRUNCATED;
        }
        while (pos < size && data[pos] == 0xFF) {
            pos++;
        }
        if (pos >= size) {
            return JpegParseStatus::TRUNCATED;
        }
        const uint8_t marker = data[pos++];

        if (marker == 0xD9) {
            // Premature EOI before SOS.
            return JpegParseStatus::INVALID_SOS;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;  // Standalone markers without length.
        }

        if (pos + 2 > size) {
            return JpegParseStatus::TRUNCATED;
        }
        const uint16_t seg_len = read_u16_be(data + pos);
        if (seg_len < 2 || pos + seg_len > size) {
            return JpegParseStatus::TRUNCATED;
        }
        const uint8_t* seg = data + pos + 2;
        const size_t seg_body = seg_len - 2;

        if (marker == 0xDB) {
            // DQT
            size_t off = 0;
            while (off < seg_body) {
                const uint8_t pq_tq = seg[off++];
                const uint8_t pq = pq_tq >> 4;
                const uint8_t tq = pq_tq & 0x0Fu;
                if (tq >= kMaxQuantTables || pq != 0) {
                    return JpegParseStatus::INVALID_DQT;
                }
                if (off + 64 > seg_body) {
                    return JpegParseStatus::INVALID_DQT;
                }
                std::memcpy(info_out->qtable[tq], seg + off, 64);
                info_out->has_dqt[tq] = true;
                off += 64;
            }
        } else if (marker == 0xC4) {
            // DHT
            size_t off = 0;
            while (off < seg_body) {
                if (off + 17 > seg_body) {
                    return JpegParseStatus::INVALID_DHT;
                }
                const uint8_t tc_th = seg[off++];
                const uint8_t tc = tc_th >> 4;
                const uint8_t th = tc_th & 0x0Fu;
                if (tc > 1 || th >= kMaxHuffTables) {
                    return JpegParseStatus::INVALID_DHT;
                }
                const uint8_t* counts = seg + off;
                off += 16;
                uint16_t total = 0;
                for (int i = 0; i < 16; ++i) total = static_cast<uint16_t>(total + counts[i]);
                if (total > 256 || off + total > seg_body) {
                    return JpegParseStatus::INVALID_DHT;
                }
                JpegHuffmanTable* target = (tc == 0) ? &ws->dc_tables[th] : &ws->ac_tables[th];
                if (!build_huffman_table(target, counts, seg + off, total)) {
                    return JpegParseStatus::INVALID_DHT;
                }
                if (tc == 0) info_out->has_dht_dc[th] = true;
                else info_out->has_dht_ac[th] = true;
                off += total;
            }
        } else if (marker == 0xDD) {
            // DRI
            if (seg_body != 2) return JpegParseStatus::TRUNCATED;
            info_out->restart_interval = read_u16_be(seg);
        } else if (marker == 0xC0) {
            // SOF0 (Baseline DCT)
            if (seg_body < 6) return JpegParseStatus::UNSUPPORTED_SOF;
            const uint8_t precision = seg[0];
            const uint16_t height = read_u16_be(seg + 1);
            const uint16_t width = read_u16_be(seg + 3);
            const uint8_t ncomp = seg[5];
            if (precision != 8 || (ncomp != 1 && ncomp != 3)) {
                return JpegParseStatus::UNSUPPORTED_SOF;
            }
            if (width == 0 || height == 0 || width > kMaxJpegDim || height > kMaxJpegDim) {
                return JpegParseStatus::INVALID_DIMENSIONS;
            }
            if (seg_body != static_cast<size_t>(6 + 3 * ncomp)) {
                return JpegParseStatus::UNSUPPORTED_SOF;
            }

            info_out->width = width;
            info_out->height = height;
            info_out->num_components = ncomp;
            info_out->max_h_samp = 1;
            info_out->max_v_samp = 1;
            uint8_t blocks_per_mcu = 0;

            for (uint8_t c = 0; c < ncomp; ++c) {
                const uint8_t cid = seg[6 + 3 * c + 0];
                const uint8_t hv = seg[6 + 3 * c + 1];
                const uint8_t qt = seg[6 + 3 * c + 2];
                const uint8_t hs = hv >> 4;
                const uint8_t vs = hv & 0x0Fu;
                if ((hs != 1 && hs != 2) || (vs != 1 && vs != 2) || qt >= kMaxQuantTables) {
                    return JpegParseStatus::UNSUPPORTED_SOF;
                }
                info_out->comp[c].id = cid;
                info_out->comp[c].h_samp = hs;
                info_out->comp[c].v_samp = vs;
                info_out->comp[c].qt_id = qt;
                if (hs > info_out->max_h_samp) info_out->max_h_samp = hs;
                if (vs > info_out->max_v_samp) info_out->max_v_samp = vs;
                blocks_per_mcu = static_cast<uint8_t>(blocks_per_mcu + hs * vs);
            }
            if (blocks_per_mcu == 0 || blocks_per_mcu > 10) {
                return JpegParseStatus::UNSUPPORTED_SOF;
            }
            info_out->blocks_per_mcu = blocks_per_mcu;
            const uint16_t mcu_w = static_cast<uint16_t>(info_out->max_h_samp * 8);
            const uint16_t mcu_h = static_cast<uint16_t>(info_out->max_v_samp * 8);
            info_out->mcu_cols = static_cast<uint16_t>((width + mcu_w - 1) / mcu_w);
            info_out->mcu_rows = static_cast<uint16_t>((height + mcu_h - 1) / mcu_h);
            const uint64_t mcu_count =
                static_cast<uint64_t>(info_out->mcu_cols) * static_cast<uint64_t>(info_out->mcu_rows);
            const uint64_t total_blocks = mcu_count * blocks_per_mcu;
            if (mcu_count == 0 || total_blocks > kMaxTotalBlocks) {
                return JpegParseStatus::INVALID_DIMENSIONS;
            }
            info_out->mcu_count = static_cast<uint32_t>(mcu_count);
            info_out->total_blocks = static_cast<uint32_t>(total_blocks);
            saw_sof0 = true;
        } else if (marker == 0xC1 || marker == 0xC2 || marker == 0xC3 ||
                   (marker >= 0xC5 && marker <= 0xCF && marker != 0xC4 && marker != 0xCC)) {
            return JpegParseStatus::UNSUPPORTED_SOF;
        } else if (marker == 0xDA) {
            // SOS
            if (!saw_sof0) return JpegParseStatus::INVALID_SOS;
            if (seg_body < 4) return JpegParseStatus::INVALID_SOS;
            const uint8_t ns = seg[0];
            if (ns != info_out->num_components || seg_body != static_cast<size_t>(1 + 2 * ns + 3)) {
                return JpegParseStatus::INVALID_SOS;
            }
            for (uint8_t i = 0; i < ns; ++i) {
                const uint8_t cs = seg[1 + 2 * i];
                const uint8_t td_ta = seg[1 + 2 * i + 1];
                const uint8_t td = td_ta >> 4;
                const uint8_t ta = td_ta & 0x0Fu;
                if (td >= kMaxHuffTables || ta >= kMaxHuffTables) {
                    return JpegParseStatus::INVALID_SOS;
                }
                bool matched = false;
                for (uint8_t c = 0; c < info_out->num_components; ++c) {
                    if (info_out->comp[c].id == cs) {
                        info_out->comp[c].dc_ht_id = td;
                        info_out->comp[c].ac_ht_id = ta;
                        matched = true;
                        break;
                    }
                }
                if (!matched) return JpegParseStatus::INVALID_SOS;
            }
            const uint8_t ss = seg[1 + 2 * ns + 0];
            const uint8_t se = seg[1 + 2 * ns + 1];
            const uint8_t ah_al = seg[1 + 2 * ns + 2];
            if (ss != 0 || se != 63 || ah_al != 0) {
                return JpegParseStatus::INVALID_SOS;
            }
            info_out->scan_data_offset = pos + seg_len;
            info_out->scan_data_size = size - info_out->scan_data_offset;
            saw_sos = true;
            break;
        }

        pos += seg_len;
    }

    if (!saw_sof0 || !saw_sos) {
        return JpegParseStatus::INVALID_SOS;
    }

    // If MJPEG frame omitted DHT markers (common on hardware MJPEG encoders), install Annex K defaults.
    bool any_dht = false;
    for (size_t i = 0; i < kMaxHuffTables; ++i) {
        if (info_out->has_dht_dc[i] || info_out->has_dht_ac[i]) any_dht = true;
    }
    if (!any_dht) {
        init_annex_k_huffman_tables(ws);
    }

    // Verify all referenced DQT and DHT tables exist.
    for (uint8_t c = 0; c < info_out->num_components; ++c) {
        if (!info_out->has_dqt[info_out->comp[c].qt_id]) {
            return JpegParseStatus::MISSING_TABLE;
        }
        if (!ws->dc_tables[info_out->comp[c].dc_ht_id].present ||
            !ws->ac_tables[info_out->comp[c].ac_ht_id].present) {
            return JpegParseStatus::MISSING_TABLE;
        }
    }

    return JpegParseStatus::OK;
}

JpegParseStatus parse_jpeg_to_dct(const uint8_t* data,
                                  size_t size,
                                  JpegFrameInfo* info_out,
                                  JpegParserWorkspace* ws,
                                  int16_t* blocks_out,
                                  size_t max_blocks) {
    if (!blocks_out) return JpegParseStatus::INVALID_ARGUMENT;

    const JpegParseStatus hdr_st = parse_jpeg_headers(data, size, info_out, ws);
    if (hdr_st != JpegParseStatus::OK) {
        return hdr_st;
    }
    if (info_out->total_blocks > max_blocks) {
        return JpegParseStatus::BUFFER_TOO_SMALL;
    }

    BitReader br(data + info_out->scan_data_offset, info_out->scan_data_size);
    int32_t dc_pred[kMaxJpegComponents]{};
    uint32_t block_idx = 0;
    uint16_t mcu_until_rst = info_out->restart_interval;
    uint8_t next_rst = 0;

    for (uint32_t mcu = 0; mcu < info_out->mcu_count; ++mcu) {
        for (uint8_t c = 0; c < info_out->num_components; ++c) {
            const JpegComponentInfo& comp = info_out->comp[c];
            const JpegHuffmanTable& dc_ht = ws->dc_tables[comp.dc_ht_id];
            const JpegHuffmanTable& ac_ht = ws->ac_tables[comp.ac_ht_id];
            const uint8_t nblk = static_cast<uint8_t>(comp.h_samp * comp.v_samp);

            for (uint8_t b = 0; b < nblk; ++b) {
                if (block_idx >= max_blocks) {
                    return JpegParseStatus::BUFFER_TOO_SMALL;
                }
                int16_t* blk = blocks_out + static_cast<size_t>(block_idx) * 64u;
                std::memset(blk, 0, 64 * sizeof(int16_t));

                // DC coefficient
                const int dc_cat = br.decode_huffman(dc_ht);
                if (dc_cat < 0 || dc_cat > 11 || !br.ok()) {
                    return JpegParseStatus::ENTROPY_ERROR;
                }
                const int32_t dc_diff = br.receive_extend(dc_cat);
                if (!br.ok()) return JpegParseStatus::ENTROPY_ERROR;
                dc_pred[c] += dc_diff;
                if (dc_pred[c] < -32768 || dc_pred[c] > 32767) {
                    return JpegParseStatus::ENTROPY_ERROR;
                }
                blk[0] = static_cast<int16_t>(dc_pred[c]);

                // AC coefficients (zigzag indices 1..63)
                int k = 1;
                while (k < 64) {
                    const int rs = br.decode_huffman(ac_ht);
                    if (rs < 0 || !br.ok()) {
                        return JpegParseStatus::ENTROPY_ERROR;
                    }
                    if (rs == 0x00) {
                        break;  // EOB
                    }
                    const int run = (rs >> 4) & 0x0F;
                    const int cat = rs & 0x0F;
                    if (cat == 0) {
                        if (run == 15) {
                            k += 16;  // ZRL (16 zeros)
                            if (k > 64) return JpegParseStatus::ENTROPY_ERROR;
                            continue;
                        }
                        return JpegParseStatus::ENTROPY_ERROR;
                    }
                    if (cat > 10) {
                        return JpegParseStatus::ENTROPY_ERROR;
                    }
                    k += run;
                    if (k >= 64) {
                        return JpegParseStatus::ENTROPY_ERROR;
                    }
                    const int32_t ac_val = br.receive_extend(cat);
                    if (!br.ok()) return JpegParseStatus::ENTROPY_ERROR;
                    blk[k] = static_cast<int16_t>(ac_val);
                    k++;
                }

                block_idx++;
            }
        }

        // Handle DRI restart markers between MCUs.
        if (info_out->restart_interval > 0 && (mcu + 1u) < info_out->mcu_count) {
            if (--mcu_until_rst == 0) {
                if (!br.consume_restart_marker(next_rst)) {
                    return JpegParseStatus::ENTROPY_ERROR;
                }
                next_rst = static_cast<uint8_t>((next_rst + 1u) & 7u);
                mcu_until_rst = info_out->restart_interval;
                std::memset(dc_pred, 0, sizeof(dc_pred));
            }
        }
    }

    return JpegParseStatus::OK;
}

}  // namespace ecl
