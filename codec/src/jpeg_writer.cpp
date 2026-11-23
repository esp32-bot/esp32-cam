#include "ecl/jpeg_writer.hpp"

#include <cstring>

namespace ecl {
namespace {

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

struct EncHuffTable {
    uint16_t code[256]{};
    uint8_t size[256]{};
};

void build_enc_table(EncHuffTable* t, const uint8_t bits[16], const uint8_t* vals, size_t nvals) {
    std::memset(t, 0, sizeof(*t));
    uint32_t code = 0;
    size_t idx = 0;
    for (int len = 1; len <= 16; ++len) {
        for (uint8_t i = 0; i < bits[len - 1]; ++i) {
            if (idx < nvals) {
                const uint8_t sym = vals[idx++];
                t->code[sym] = static_cast<uint16_t>(code);
                t->size[sym] = static_cast<uint8_t>(len);
            }
            code++;
        }
        code <<= 1;
    }
}

struct EncTables {
    EncHuffTable dc[2];
    EncHuffTable ac[2];

    EncTables() {
        build_enc_table(&dc[0], kDcLumBits, kDcLumVal, sizeof(kDcLumVal));
        build_enc_table(&dc[1], kDcChrBits, kDcChrVal, sizeof(kDcChrVal));
        build_enc_table(&ac[0], kAcLumBits, kAcLumVal, sizeof(kAcLumVal));
        build_enc_table(&ac[1], kAcChrBits, kAcChrVal, sizeof(kAcChrVal));
    }
};

const EncTables& get_enc_tables() {
    static const EncTables kTables;
    return kTables;
}

class JpegByteBitWriter {
  public:
    JpegByteBitWriter(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

    bool ok() const { return !overflow_; }
    size_t size() const { return pos_; }

    void put_byte(uint8_t b) {
        if (pos_ >= cap_) {
            overflow_ = true;
            return;
        }
        buf_[pos_++] = b;
    }

    void put_u16_be(uint16_t v) {
        put_byte(static_cast<uint8_t>((v >> 8) & 0xFFu));
        put_byte(static_cast<uint8_t>(v & 0xFFu));
    }

    void put_bytes(const uint8_t* src, size_t n) {
        if (pos_ + n > cap_) {
            overflow_ = true;
            return;
        }
        std::memcpy(buf_ + pos_, src, n);
        pos_ += n;
    }

    void put_bits(uint32_t bits, int nbits) {
        if (overflow_ || nbits <= 0) return;
        bit_buf_ = (bit_buf_ << nbits) | (bits & ((1u << nbits) - 1u));
        bit_count_ += nbits;
        while (bit_count_ >= 8) {
            bit_count_ -= 8;
            const uint8_t b = static_cast<uint8_t>((bit_buf_ >> bit_count_) & 0xFFu);
            put_byte(b);
            if (b == 0xFF) {
                put_byte(0x00);
            }
        }
    }

    void flush_bits() {
        if (bit_count_ > 0) {
            const int pad = 8 - bit_count_;
            put_bits((1u << pad) - 1u, pad);
        }
    }

  private:
    uint8_t* buf_ = nullptr;
    size_t cap_ = 0;
    size_t pos_ = 0;
    uint32_t bit_buf_ = 0;
    int bit_count_ = 0;
    bool overflow_ = false;
};

inline int value_category(int32_t v) {
    uint32_t mag = static_cast<uint32_t>(v < 0 ? -v : v);
    int cat = 0;
    while (mag > 0) {
        mag >>= 1;
        cat++;
    }
    return cat;
}

inline uint32_t encode_magnitude_bits(int32_t v, int cat) {
    if (v >= 0) return static_cast<uint32_t>(v);
    return static_cast<uint32_t>(v + ((1 << cat) - 1));
}

void write_dht_segment(JpegByteBitWriter& w,
                       uint8_t tc_th,
                       const uint8_t bits[16],
                       const uint8_t* vals,
                       size_t nvals) {
    w.put_byte(0xFF);
    w.put_byte(0xC4);
    w.put_u16_be(static_cast<uint16_t>(2 + 1 + 16 + nvals));
    w.put_byte(tc_th);
    w.put_bytes(bits, 16);
    w.put_bytes(vals, nvals);
}

}  // namespace

size_t write_dct_to_jpeg(const JpegFrameInfo& info,
                         const int16_t* blocks,
                         size_t num_blocks,
                         uint8_t* out_buf,
                         size_t out_capacity) {
    if (!blocks || !out_buf || out_capacity < 64 || num_blocks < info.total_blocks ||
        info.width == 0 || info.height == 0 ||
        (info.num_components != 1 && info.num_components != 3)) {
        return 0;
    }

    const EncTables& enc = get_enc_tables();
    JpegByteBitWriter w(out_buf, out_capacity);

    // SOI
    w.put_byte(0xFF);
    w.put_byte(0xD8);

    // APP0 JFIF
    static constexpr uint8_t kJfif[18] = {
        0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00,
        0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00};
    w.put_bytes(kJfif, sizeof(kJfif));

    // DQT for all used quantization tables
    for (uint8_t qt = 0; qt < kMaxQuantTables; ++qt) {
        if (!info.has_dqt[qt]) continue;
        w.put_byte(0xFF);
        w.put_byte(0xDB);
        w.put_u16_be(67);
        w.put_byte(qt);
        w.put_bytes(info.qtable[qt], 64);
    }

    // SOF0
    w.put_byte(0xFF);
    w.put_byte(0xC0);
    w.put_u16_be(static_cast<uint16_t>(8 + 3 * info.num_components));
    w.put_byte(8);
    w.put_u16_be(info.height);
    w.put_u16_be(info.width);
    w.put_byte(info.num_components);
    for (uint8_t c = 0; c < info.num_components; ++c) {
        w.put_byte(info.comp[c].id);
        w.put_byte(static_cast<uint8_t>((info.comp[c].h_samp << 4) | (info.comp[c].v_samp & 0x0Fu)));
        w.put_byte(info.comp[c].qt_id);
    }

    // DHT (Annex K)
    write_dht_segment(w, 0x00, kDcLumBits, kDcLumVal, sizeof(kDcLumVal));
    write_dht_segment(w, 0x10, kAcLumBits, kAcLumVal, sizeof(kAcLumVal));
    if (info.num_components > 1) {
        write_dht_segment(w, 0x01, kDcChrBits, kDcChrVal, sizeof(kDcChrVal));
        write_dht_segment(w, 0x11, kAcChrBits, kAcChrVal, sizeof(kAcChrVal));
    }

    // SOS
    w.put_byte(0xFF);
    w.put_byte(0xDA);
    w.put_u16_be(static_cast<uint16_t>(6 + 2 * info.num_components));
    w.put_byte(info.num_components);
    for (uint8_t c = 0; c < info.num_components; ++c) {
        const uint8_t ht_idx = (c == 0) ? 0u : 1u;
        w.put_byte(info.comp[c].id);
        w.put_byte(static_cast<uint8_t>((ht_idx << 4) | ht_idx));
    }
    w.put_byte(0);   // Ss
    w.put_byte(63);  // Se
    w.put_byte(0);   // Ah/Al

    // Entropy-encode MCUs
    int32_t prev_dc[kMaxJpegComponents]{};
    uint32_t block_idx = 0;
    for (uint32_t mcu = 0; mcu < info.mcu_count; ++mcu) {
        for (uint8_t c = 0; c < info.num_components; ++c) {
            const uint8_t ht_idx = (c == 0) ? 0u : 1u;
            const EncHuffTable& dc_ht = enc.dc[ht_idx];
            const EncHuffTable& ac_ht = enc.ac[ht_idx];
            const uint8_t nblk = static_cast<uint8_t>(info.comp[c].h_samp * info.comp[c].v_samp);

            for (uint8_t b = 0; b < nblk; ++b) {
                const int16_t* blk = blocks + static_cast<size_t>(block_idx++) * 64u;
                // DC
                int32_t diff = static_cast<int32_t>(blk[0]) - prev_dc[c];
                prev_dc[c] = blk[0];
                if (diff < -2047) diff = -2047;
                if (diff > 2047) diff = 2047;
                const int dc_cat = value_category(diff);
                w.put_bits(dc_ht.code[dc_cat], dc_ht.size[dc_cat]);
                if (dc_cat > 0) {
                    w.put_bits(encode_magnitude_bits(diff, dc_cat), dc_cat);
                }

                // AC
                int zero_run = 0;
                for (int k = 1; k < 64; ++k) {
                    int32_t val = blk[k];
                    if (val == 0) {
                        zero_run++;
                        continue;
                    }
                    if (val < -1023) val = -1023;
                    if (val > 1023) val = 1023;
                    while (zero_run >= 16) {
                        w.put_bits(ac_ht.code[0xF0], ac_ht.size[0xF0]);
                        zero_run -= 16;
                    }
                    const int cat = value_category(val);
                    const uint8_t sym = static_cast<uint8_t>((zero_run << 4) | cat);
                    w.put_bits(ac_ht.code[sym], ac_ht.size[sym]);
                    w.put_bits(encode_magnitude_bits(val, cat), cat);
                    zero_run = 0;
                }
                if (zero_run > 0) {
                    w.put_bits(ac_ht.code[0x00], ac_ht.size[0x00]);
                }
                if (!w.ok()) return 0;
            }
        }
    }

    w.flush_bits();
    // EOI
    w.put_byte(0xFF);
    w.put_byte(0xD9);

    return w.ok() ? w.size() : 0;
}

}  // namespace ecl
