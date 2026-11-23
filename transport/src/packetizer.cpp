#include "ecl/packetizer.hpp"

#include <algorithm>
#include <cstring>

namespace ecl {

size_t Packetizer::packetize(const CodecOutput& output,
                             uint32_t timestamp_us_low,
                             const FrameMetadata& meta,
                             uint8_t* wire_buf,
                             size_t wire_buf_capacity,
                             PacketEmitFn emit,
                             void* user_ctx) {
    if (!emit || !wire_buf || cfg_.max_mtu_bytes <= kPacketOverheadBytes) {
        stats_.failed_frames++;
        return 0;
    }

    const size_t max_payload = cfg_.max_mtu_bytes - kPacketOverheadBytes;
    if (wire_buf_capacity < cfg_.max_mtu_bytes) {
        stats_.failed_frames++;
        return 0;
    }

    const size_t total_size = output.bytes.size;
    const size_t chunk_count = (total_size == 0) ? 1u : ((total_size + max_payload - 1u) / max_payload);
    if (chunk_count == 0 || chunk_count > kMaxChunksPerFrame) {
        stats_.failed_frames++;
        return 0;
    }

    size_t emitted = 0;
    for (size_t i = 0; i < chunk_count; ++i) {
        const size_t offset = i * max_payload;
        const size_t remaining = (offset < total_size) ? (total_size - offset) : 0u;
        const size_t chunk_len = (remaining < max_payload) ? remaining : max_payload;

        PacketHeader hdr{};
        hdr.magic = kProtocolMagic;
        hdr.version = kProtocolVersion;
        hdr.flags = 0;
        if (output.is_keyframe) hdr.flags |= kFlagKeyframe;
        if (i + 1 == chunk_count) hdr.flags |= kFlagLastChunk;
        if (meta.has_attitude) hdr.flags |= kFlagHasMetaAttitude;
        hdr.stream_id = cfg_.stream_id;
        hdr.codec_mode = output.mode;
        hdr.chunk_id = static_cast<uint16_t>(i);
        hdr.chunk_count = static_cast<uint16_t>(chunk_count);
        hdr.payload_len = static_cast<uint16_t>(chunk_len);
        hdr.frame_id = output.frame_id;
        hdr.reference_frame_id = output.reference_frame_id;
        hdr.timestamp_us_low = timestamp_us_low;
        hdr.width = meta.width;
        hdr.height = meta.height;
        hdr.meta_pitch_cdeg = meta.pitch_cdeg;
        hdr.meta_roll_cdeg = meta.roll_cdeg;

        const uint8_t* ptr = (chunk_len > 0) ? (output.bytes.data + offset) : nullptr;
        const size_t wire_len = pack_packet(hdr, ptr, wire_buf, wire_buf_capacity);
        if (wire_len == 0 || !emit(user_ctx, wire_buf, wire_len)) {
            stats_.failed_frames++;
            return 0;
        }
        stats_.packets_emitted++;
        stats_.payload_bytes += chunk_len;
        stats_.wire_bytes += wire_len;
        emitted++;
    }

    stats_.frames_packetized++;
    return emitted;
}

Depacketizer::~Depacketizer() {
    for (size_t i = 0; i < kMaxSlots; ++i) {
        delete[] slots_[i].buf;
        delete[] slots_[i].tail_scratch;
        slots_[i].buf = nullptr;
        slots_[i].tail_scratch = nullptr;
    }
    delete[] output_buf_;
    output_buf_ = nullptr;
}

bool Depacketizer::init(DepacketizerConfig cfg) {
    for (size_t i = 0; i < kMaxSlots; ++i) {
        delete[] slots_[i].buf;
        delete[] slots_[i].tail_scratch;
        slots_[i].buf = nullptr;
        slots_[i].tail_scratch = nullptr;
    }
    delete[] output_buf_;
    output_buf_ = nullptr;
    allocated_bytes_ = 0;

    if (cfg.slot_count == 0 || cfg.slot_count > kMaxSlots || cfg.max_frame_bytes == 0) {
        return false;
    }
    cfg_ = cfg;

    output_buf_ = new (std::nothrow) uint8_t[cfg_.max_frame_bytes];
    if (!output_buf_) return false;
    allocated_bytes_ += cfg_.max_frame_bytes;

    for (size_t i = 0; i < cfg_.slot_count; ++i) {
        slots_[i].buf = new (std::nothrow) uint8_t[cfg_.max_frame_bytes];
        slots_[i].tail_scratch = new (std::nothrow) uint8_t[kDefaultMtuBytes];
        if (!slots_[i].buf || !slots_[i].tail_scratch) {
            return false;
        }
        allocated_bytes_ += cfg_.max_frame_bytes + kDefaultMtuBytes;
    }

    reset();
    return true;
}

void Depacketizer::clear_slot(Slot& s) {
    s.active = false;
    s.frame_id = 0;
    s.reference_frame_id = 0;
    s.timestamp_us_low = 0;
    s.codec_mode = CodecMode::PASS_THROUGH;
    s.flags = 0;
    s.chunk_count = 0;
    s.chunks_received = 0;
    s.chunk_stride = 0;
    s.last_chunk_len = 0;
    s.has_last_chunk = false;
    s.last_chunk_pending_copy = false;
    s.width = 0;
    s.height = 0;
    s.meta_pitch_cdeg = 0;
    s.meta_roll_cdeg = 0;
    std::memset(s.mask, 0, sizeof(s.mask));
}

void Depacketizer::reset() {
    for (size_t i = 0; i < cfg_.slot_count; ++i) {
        clear_slot(slots_[i]);
    }
    has_newest_id_ = false;
    newest_frame_id_ = 0;
    has_completed_id_ = false;
    last_completed_frame_id_ = 0;
    stats_ = {};
}

void Depacketizer::flush_incomplete() {
    for (size_t i = 0; i < cfg_.slot_count; ++i) {
        if (slots_[i].active) {
            clear_slot(slots_[i]);
            stats_.frames_expired++;
        }
    }
}

void Depacketizer::expire_old_slots(uint32_t newest_id) {
    for (size_t i = 0; i < cfg_.slot_count; ++i) {
        if (!slots_[i].active) continue;
        const int32_t age = frame_id_diff(newest_id, slots_[i].frame_id);
        if (age > static_cast<int32_t>(cfg_.max_frame_age)) {
            clear_slot(slots_[i]);
            stats_.frames_expired++;
        }
    }
}

Depacketizer::Slot* Depacketizer::find_or_allocate_slot(uint32_t frame_id) {
    Slot* free_slot = nullptr;
    Slot* oldest_slot = nullptr;

    for (size_t i = 0; i < cfg_.slot_count; ++i) {
        if (slots_[i].active && slots_[i].frame_id == frame_id) {
            return &slots_[i];
        }
        if (!slots_[i].active && !free_slot) {
            free_slot = &slots_[i];
        } else if (slots_[i].active) {
            if (!oldest_slot || frame_id_newer(oldest_slot->frame_id, slots_[i].frame_id)) {
                oldest_slot = &slots_[i];
            }
        }
    }

    if (free_slot) return free_slot;

    // Evict oldest active slot only if incoming frame is newer.
    if (oldest_slot && frame_id_newer(frame_id, oldest_slot->frame_id)) {
        clear_slot(*oldest_slot);
        stats_.frames_expired++;
        return oldest_slot;
    }
    return nullptr;
}

bool Depacketizer::feed_packet(const uint8_t* wire_data,
                               size_t wire_len,
                               PacketizedFrame* completed_out) {
    stats_.packets_received++;
    stats_.wire_bytes_received += wire_len;

    PacketHeader hdr{};
    ConstByteSpan payload{};
    const UnpackStatus st = unpack_packet(wire_data, wire_len, &hdr, &payload);
    if (st != UnpackStatus::OK) {
        stats_.packets_malformed++;
        if (st == UnpackStatus::CRC_MISMATCH) {
            stats_.packets_crc_error++;
        }
        return false;
    }

    if (hdr.payload_len > kDefaultMtuBytes) {
        stats_.packets_oversize++;
        return false;
    }

    stats_.packets_valid++;

    // Reject frames strictly older than (newest - max_frame_age) or already completed.
    if (has_completed_id_ && !frame_id_newer(hdr.frame_id, last_completed_frame_id_) &&
        hdr.frame_id == last_completed_frame_id_) {
        stats_.packets_duplicate++;
        return false;
    }
    if (has_newest_id_) {
        const int32_t diff_from_newest = frame_id_diff(newest_frame_id_, hdr.frame_id);
        if (diff_from_newest > static_cast<int32_t>(cfg_.max_frame_age)) {
            stats_.packets_too_old++;
            return false;
        }
        if (frame_id_newer(hdr.frame_id, newest_frame_id_)) {
            newest_frame_id_ = hdr.frame_id;
            expire_old_slots(newest_frame_id_);
        }
    } else {
        has_newest_id_ = true;
        newest_frame_id_ = hdr.frame_id;
    }

    Slot* slot = find_or_allocate_slot(hdr.frame_id);
    if (!slot) {
        stats_.packets_too_old++;
        return false;
    }

    if (!slot->active) {
        clear_slot(*slot);
        slot->active = true;
        slot->frame_id = hdr.frame_id;
        slot->reference_frame_id = hdr.reference_frame_id;
        slot->timestamp_us_low = hdr.timestamp_us_low;
        slot->codec_mode = hdr.codec_mode;
        slot->flags = hdr.flags & ~kFlagLastChunk;
        slot->chunk_count = hdr.chunk_count;
        slot->width = hdr.width;
        slot->height = hdr.height;
        slot->meta_pitch_cdeg = hdr.meta_pitch_cdeg;
        slot->meta_roll_cdeg = hdr.meta_roll_cdeg;
    } else {
        // Metadata consistency check across chunks of the same frame.
        if (slot->chunk_count != hdr.chunk_count || slot->codec_mode != hdr.codec_mode ||
            slot->reference_frame_id != hdr.reference_frame_id) {
            stats_.packets_malformed++;
            return false;
        }
    }

    const size_t word = hdr.chunk_id / 64u;
    const uint64_t bit = 1ull << (hdr.chunk_id % 64u);
    if ((slot->mask[word] & bit) != 0) {
        stats_.packets_duplicate++;
        return false;
    }

    const bool is_last = (hdr.chunk_id + 1u == hdr.chunk_count);
    if (hdr.chunk_count == 1u) {
        if (hdr.payload_len > cfg_.max_frame_bytes) {
            stats_.packets_oversize++;
            return false;
        }
        if (hdr.payload_len > 0) {
            std::memcpy(slot->buf, payload.data, hdr.payload_len);
        }
        slot->chunk_stride = hdr.payload_len;
        slot->last_chunk_len = hdr.payload_len;
        slot->has_last_chunk = true;
    } else if (!is_last) {
        if (hdr.payload_len == 0) {
            stats_.packets_malformed++;
            return false;
        }
        if (slot->chunk_stride == 0) {
            slot->chunk_stride = hdr.payload_len;
        } else if (slot->chunk_stride != hdr.payload_len) {
            stats_.packets_malformed++;
            return false;
        }
        const size_t offset = static_cast<size_t>(hdr.chunk_id) * slot->chunk_stride;
        if (offset + hdr.payload_len > cfg_.max_frame_bytes) {
            stats_.packets_oversize++;
            return false;
        }
        std::memcpy(slot->buf + offset, payload.data, hdr.payload_len);

        // If the final chunk arrived earlier before stride was known, place it now.
        if (slot->last_chunk_pending_copy) {
            const size_t tail_off = static_cast<size_t>(slot->chunk_count - 1u) * slot->chunk_stride;
            if (tail_off + slot->last_chunk_len > cfg_.max_frame_bytes) {
                stats_.packets_oversize++;
                clear_slot(*slot);
                return false;
            }
            if (slot->last_chunk_len > 0) {
                std::memcpy(slot->buf + tail_off, slot->tail_scratch, slot->last_chunk_len);
            }
            slot->last_chunk_pending_copy = false;
        }
    } else {
        // Multi-chunk final chunk.
        slot->last_chunk_len = hdr.payload_len;
        slot->has_last_chunk = true;
        if (slot->chunk_stride != 0) {
            const size_t tail_off = static_cast<size_t>(hdr.chunk_id) * slot->chunk_stride;
            if (tail_off + hdr.payload_len > cfg_.max_frame_bytes) {
                stats_.packets_oversize++;
                return false;
            }
            if (hdr.payload_len > 0) {
                std::memcpy(slot->buf + tail_off, payload.data, hdr.payload_len);
            }
        } else {
            if (hdr.payload_len > 0) {
                std::memcpy(slot->tail_scratch, payload.data, hdr.payload_len);
            }
            slot->last_chunk_pending_copy = true;
        }
    }

    slot->mask[word] |= bit;
    slot->chunks_received++;

    if (slot->chunks_received < slot->chunk_count) {
        return false;
    }

    // Frame complete! Compute total length and copy to output buffer.
    const size_t total_len = (slot->chunk_count == 1u)
                                 ? static_cast<size_t>(slot->last_chunk_len)
                                 : (static_cast<size_t>(slot->chunk_count - 1u) * slot->chunk_stride +
                                    static_cast<size_t>(slot->last_chunk_len));
    if (total_len > cfg_.max_frame_bytes) {
        stats_.packets_oversize++;
        clear_slot(*slot);
        return false;
    }

    if (total_len > 0) {
        std::memcpy(output_buf_, slot->buf, total_len);
    }

    if (completed_out) {
        completed_out->mode = slot->codec_mode;
        completed_out->frame_id = slot->frame_id;
        completed_out->reference_frame_id = slot->reference_frame_id;
        completed_out->timestamp_us_low = slot->timestamp_us_low;
        completed_out->is_keyframe = (slot->flags & kFlagKeyframe) != 0;
        completed_out->metadata.width = slot->width;
        completed_out->metadata.height = slot->height;
        completed_out->metadata.has_attitude = (slot->flags & kFlagHasMetaAttitude) != 0;
        completed_out->metadata.pitch_cdeg = slot->meta_pitch_cdeg;
        completed_out->metadata.roll_cdeg = slot->meta_roll_cdeg;
        completed_out->payload = ConstByteSpan{output_buf_, total_len};
    }

    const uint32_t finished_id = slot->frame_id;
    clear_slot(*slot);
    stats_.frames_completed++;

    if (!has_completed_id_ || frame_id_newer(finished_id, last_completed_frame_id_)) {
        has_completed_id_ = true;
        last_completed_frame_id_ = finished_id;
    }

    // Expire any older incomplete frames once a newer frame completes (never stall for old losses).
    for (size_t i = 0; i < cfg_.slot_count; ++i) {
        if (slots_[i].active && frame_id_newer(finished_id, slots_[i].frame_id)) {
            clear_slot(slots_[i]);
            stats_.frames_expired++;
        }
    }

    return true;
}

}  // namespace ecl
