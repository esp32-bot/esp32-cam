#pragma once

#include <cstddef>
#include <cstdint>

#include "ecl/protocol.hpp"
#include "ecl/types.hpp"

namespace ecl {

using PacketEmitFn = bool (*)(void* user_ctx, const uint8_t* wire_data, size_t wire_len);

struct PacketizerConfig {
    uint8_t stream_id = 1;
    size_t max_mtu_bytes = kDefaultMtuBytes;
};

struct PacketizerStats {
    uint64_t frames_packetized = 0;
    uint64_t packets_emitted = 0;
    uint64_t payload_bytes = 0;
    uint64_t wire_bytes = 0;
    uint64_t failed_frames = 0;
};

class Packetizer {
  public:
    explicit Packetizer(PacketizerConfig cfg = {}) : cfg_(cfg) {}

    // Splits `output` into MTU-bounded ECL1 packets using caller-provided scratch `wire_buf`.
    // Zero heap allocations. Returns number of packets emitted (0 on failure).
    size_t packetize(const CodecOutput& output,
                     uint32_t timestamp_us_low,
                     const FrameMetadata& meta,
                     uint8_t* wire_buf,
                     size_t wire_buf_capacity,
                     PacketEmitFn emit,
                     void* user_ctx);

    const PacketizerStats& stats() const { return stats_; }
    void reset_stats() { stats_ = {}; }

  private:
    PacketizerConfig cfg_;
    PacketizerStats stats_{};
};

struct DepacketizerConfig {
    size_t max_frame_bytes = 131072;  // 128 KB bounded per slot
    size_t slot_count = 4;            // bounded reassembly ring
    uint32_t max_frame_age = 3;       // expire frames older than newest - max_frame_age
};

struct DepacketizerStats {
    uint64_t packets_received = 0;
    uint64_t packets_valid = 0;
    uint64_t packets_malformed = 0;
    uint64_t packets_crc_error = 0;
    uint64_t packets_duplicate = 0;
    uint64_t packets_too_old = 0;
    uint64_t packets_oversize = 0;
    uint64_t frames_completed = 0;
    uint64_t frames_expired = 0;
    uint64_t wire_bytes_received = 0;
};

class Depacketizer {
  public:
    static constexpr size_t kMaxSlots = 8;
    static constexpr size_t kMaskWords = (kMaxChunksPerFrame + 63) / 64;

    Depacketizer() = default;
    ~Depacketizer();

    Depacketizer(const Depacketizer&) = delete;
    Depacketizer& operator=(const Depacketizer&) = delete;

    // Allocates bounded reassembly buffers once. Returns false on invalid config or OOM.
    bool init(DepacketizerConfig cfg = {});
    void reset();

    // Feeds one raw wire packet. If this packet completes a frame, populates `*completed_out`
    // pointing to an internal output buffer valid until the next `feed_packet()` call, and returns true.
    bool feed_packet(const uint8_t* wire_data, size_t wire_len, PacketizedFrame* completed_out);

    // Flushes any remaining incomplete slots as expired (useful at end-of-stream).
    void flush_incomplete();

    const DepacketizerStats& stats() const { return stats_; }
    size_t allocated_bytes() const { return allocated_bytes_; }

  private:
    struct Slot {
        bool active = false;
        uint32_t frame_id = 0;
        uint32_t reference_frame_id = 0;
        uint32_t timestamp_us_low = 0;
        CodecMode codec_mode = CodecMode::PASS_THROUGH;
        uint8_t flags = 0;
        uint16_t chunk_count = 0;
        uint16_t chunks_received = 0;
        uint16_t chunk_stride = 0;
        uint16_t last_chunk_len = 0;
        bool has_last_chunk = false;
        bool last_chunk_pending_copy = false;
        uint16_t width = 0;
        uint16_t height = 0;
        int16_t meta_pitch_cdeg = 0;
        int16_t meta_roll_cdeg = 0;
        uint64_t mask[kMaskWords]{};
        uint8_t* buf = nullptr;
        uint8_t* tail_scratch = nullptr;
    };

    void clear_slot(Slot& s);
    void expire_old_slots(uint32_t newest_id);
    Slot* find_or_allocate_slot(uint32_t frame_id);

    DepacketizerConfig cfg_{};
    Slot slots_[kMaxSlots]{};
    uint8_t* output_buf_ = nullptr;
    size_t allocated_bytes_ = 0;
    bool has_newest_id_ = false;
    uint32_t newest_frame_id_ = 0;
    bool has_completed_id_ = false;
    uint32_t last_completed_frame_id_ = 0;
    DepacketizerStats stats_{};
};

}  // namespace ecl
