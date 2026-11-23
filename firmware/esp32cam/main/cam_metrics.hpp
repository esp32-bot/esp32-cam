#pragma once

#include <cstdint>

namespace ecl_fw {

struct StreamMetrics {
    uint32_t frame_id = 0;
    uint32_t last_frame_bytes = 0;
    uint32_t last_codec_bytes = 0;
    uint32_t last_capture_us = 0;
    uint32_t last_codec_us = 0;
    uint32_t last_packetize_us = 0;
    uint32_t last_interval_us = 0;
    uint32_t dropped_frames = 0;
    uint32_t capture_errors = 0;
    uint32_t keyframes = 0;
    uint32_t last_changed_blocks = 0;
    uint32_t last_total_blocks = 0;
    uint64_t total_input_bytes = 0;
    uint64_t total_tx_bytes = 0;
    uint32_t free_internal_heap = 0;
    uint32_t min_internal_heap = 0;
    uint32_t free_psram = 0;
    uint32_t min_psram = 0;
};

}  // namespace ecl_fw
