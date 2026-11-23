#pragma once

#include <cstddef>
#include <cstdint>

#include "ecl/types.hpp"

namespace ecl {

constexpr uint16_t kProtocolMagic = 0x4C43;  // 'C','L' little-endian
constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kPacketHeaderBytes = 32;
constexpr size_t kPacketTrailerBytes = 4;
constexpr size_t kPacketOverheadBytes = kPacketHeaderBytes + kPacketTrailerBytes;
constexpr size_t kDefaultMtuBytes = 1400;
constexpr size_t kDefaultMaxPayloadBytes = kDefaultMtuBytes - kPacketOverheadBytes;
constexpr uint16_t kMaxChunksPerFrame = 512;

enum PacketFlags : uint8_t {
    kFlagKeyframe = 1u << 0,
    kFlagLastChunk = 1u << 1,
    kFlagHasMetaAttitude = 1u << 2,
    kFlagEndOfStream = 1u << 3,
};

struct PacketHeader {
    uint16_t magic = kProtocolMagic;
    uint8_t version = kProtocolVersion;
    uint8_t flags = 0;
    uint8_t stream_id = 0;
    CodecMode codec_mode = CodecMode::PASS_THROUGH;
    uint16_t chunk_id = 0;
    uint16_t chunk_count = 1;
    uint16_t payload_len = 0;
    uint32_t frame_id = 0;
    uint32_t reference_frame_id = 0;
    uint32_t timestamp_us_low = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    int16_t meta_pitch_cdeg = 0;
    int16_t meta_roll_cdeg = 0;
};

enum class UnpackStatus : uint8_t {
    OK = 0,
    TOO_SHORT = 1,
    BAD_MAGIC = 2,
    BAD_VERSION = 3,
    LENGTH_MISMATCH = 4,
    INVALID_CHUNKS = 5,
    CRC_MISMATCH = 6,
};

uint32_t crc32_ieee(const uint8_t* data, size_t len);

// Serializes header + payload + CRC32 trailer into `out_buf`.
// Returns total wire packet size in bytes, or 0 if `out_buf` is too small or invalid.
size_t pack_packet(const PacketHeader& hdr,
                   const uint8_t* payload,
                   uint8_t* out_buf,
                   size_t out_capacity);

// Validates magic, version, length, chunk indices, and CRC32.
// On OK, populates `hdr` and sets `payload_out` pointing inside `wire_buf`.
UnpackStatus unpack_packet(const uint8_t* wire_buf,
                           size_t wire_len,
                           PacketHeader* hdr,
                           ConstByteSpan* payload_out);

}  // namespace ecl
