#include "ecl/protocol.hpp"

#include <cstring>

namespace ecl {
namespace {

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

}  // namespace

uint32_t crc32_ieee(const uint8_t* data, size_t len) {
    // 16-entry nibble table: only 64 bytes in ROM/DRAM and 4x faster than bitwise CRC.
    static constexpr uint32_t kTable[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        crc = (crc >> 4) ^ kTable[crc & 0x0Fu];
        crc = (crc >> 4) ^ kTable[crc & 0x0Fu];
    }
    return ~crc;
}

size_t pack_packet(const PacketHeader& hdr,
                   const uint8_t* payload,
                   uint8_t* out_buf,
                   size_t out_capacity) {
    if (!out_buf || hdr.chunk_count == 0 || hdr.chunk_count > kMaxChunksPerFrame ||
        hdr.chunk_id >= hdr.chunk_count) {
        return 0;
    }
    if (hdr.payload_len > 0 && !payload) {
        return 0;
    }
    const size_t total_len = kPacketHeaderBytes + hdr.payload_len + kPacketTrailerBytes;
    if (total_len > out_capacity) {
        return 0;
    }

    write_u16_le(out_buf + 0, kProtocolMagic);
    out_buf[2] = kProtocolVersion;
    out_buf[3] = hdr.flags;
    out_buf[4] = hdr.stream_id;
    out_buf[5] = static_cast<uint8_t>(hdr.codec_mode);
    write_u16_le(out_buf + 6, hdr.chunk_id);
    write_u16_le(out_buf + 8, hdr.chunk_count);
    write_u16_le(out_buf + 10, hdr.payload_len);
    write_u32_le(out_buf + 12, hdr.frame_id);
    write_u32_le(out_buf + 16, hdr.reference_frame_id);
    write_u32_le(out_buf + 20, hdr.timestamp_us_low);
    write_u16_le(out_buf + 24, hdr.width);
    write_u16_le(out_buf + 26, hdr.height);
    write_u16_le(out_buf + 28, static_cast<uint16_t>(hdr.meta_pitch_cdeg));
    write_u16_le(out_buf + 30, static_cast<uint16_t>(hdr.meta_roll_cdeg));

    if (hdr.payload_len > 0) {
        std::memcpy(out_buf + kPacketHeaderBytes, payload, hdr.payload_len);
    }

    const uint32_t crc = crc32_ieee(out_buf, kPacketHeaderBytes + hdr.payload_len);
    write_u32_le(out_buf + kPacketHeaderBytes + hdr.payload_len, crc);
    return total_len;
}

UnpackStatus unpack_packet(const uint8_t* wire_buf,
                           size_t wire_len,
                           PacketHeader* hdr,
                           ConstByteSpan* payload_out) {
    if (!wire_buf || !hdr || !payload_out || wire_len < kPacketOverheadBytes) {
        return UnpackStatus::TOO_SHORT;
    }

    const uint16_t magic = read_u16_le(wire_buf + 0);
    if (magic != kProtocolMagic) {
        return UnpackStatus::BAD_MAGIC;
    }
    const uint8_t version = wire_buf[2];
    if (version != kProtocolVersion) {
        return UnpackStatus::BAD_VERSION;
    }

    const uint16_t chunk_id = read_u16_le(wire_buf + 6);
    const uint16_t chunk_count = read_u16_le(wire_buf + 8);
    const uint16_t payload_len = read_u16_le(wire_buf + 10);

    if (wire_len != kPacketHeaderBytes + static_cast<size_t>(payload_len) + kPacketTrailerBytes) {
        return UnpackStatus::LENGTH_MISMATCH;
    }
    if (chunk_count == 0 || chunk_count > kMaxChunksPerFrame || chunk_id >= chunk_count) {
        return UnpackStatus::INVALID_CHUNKS;
    }

    const uint32_t expected_crc = read_u32_le(wire_buf + kPacketHeaderBytes + payload_len);
    const uint32_t actual_crc = crc32_ieee(wire_buf, kPacketHeaderBytes + payload_len);
    if (actual_crc != expected_crc) {
        return UnpackStatus::CRC_MISMATCH;
    }

    hdr->magic = magic;
    hdr->version = version;
    hdr->flags = wire_buf[3];
    hdr->stream_id = wire_buf[4];
    hdr->codec_mode = static_cast<CodecMode>(wire_buf[5]);
    hdr->chunk_id = chunk_id;
    hdr->chunk_count = chunk_count;
    hdr->payload_len = payload_len;
    hdr->frame_id = read_u32_le(wire_buf + 12);
    hdr->reference_frame_id = read_u32_le(wire_buf + 16);
    hdr->timestamp_us_low = read_u32_le(wire_buf + 20);
    hdr->width = read_u16_le(wire_buf + 24);
    hdr->height = read_u16_le(wire_buf + 26);
    hdr->meta_pitch_cdeg = static_cast<int16_t>(read_u16_le(wire_buf + 28));
    hdr->meta_roll_cdeg = static_cast<int16_t>(read_u16_le(wire_buf + 30));

    payload_out->data = wire_buf + kPacketHeaderBytes;
    payload_out->size = payload_len;
    return UnpackStatus::OK;
}

}  // namespace ecl
