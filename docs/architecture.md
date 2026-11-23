# System Architecture — `esp32-cam-livestream`

## 1. Layered Boundaries

```
┌──────────────────────────────────────────────────────────┐
│  Camera Source (Platform-specific: ESP32-CAM or Host)    │
│  - AI-Thinker ESP32-CAM + OV2640 hardware JPEG           │
│  - Host deterministic JPEG directory / manifest source   │
│  - Zero knowledge of BalaC, MPU6886, or robot state      │
└───────────────────────────┬──────────────────────────────┘
                            │ EncodedFrameView (bytes + meta)
                            ▼
┌──────────────────────────────────────────────────────────┐
│  Temporal Codec Layer (Platform-independent C++17)       │
│  - Compiled identically on desktop host & Xtensa ESP32   │
│  - Zero knowledge of Wi-Fi, sockets, or robot            │
│  - Bounded scratch/reference buffers (no hot-path alloc) │
│  - Modes: PASS_THROUGH (permanent fallback), FRAME_SKIP, │
│    GENERIC_ZLIB (control), DCT_DC_RESIDUAL,              │
│    DCT_LOW_AC_RESIDUAL, DCT_FULL_RESIDUAL,               │
│    DCT_SPARSE_BLOCK                                      │
└───────────────────────────┬──────────────────────────────┘
                            │ CodecOutput (keyframe / delta)
                            ▼
┌──────────────────────────────────────────────────────────┐
│  Packet Transport Layer (Platform-independent framing)   │
│  - Compact versioned header (ECL1, 28 B) + IEEE CRC32    │
│  - MTU-bounded chunking + wrap-safe sequence handling    │
│  - Pluggable sender/receiver (UDP default; future raw    │
│    802.11 / ESP-NOW / FEC backend; host net impairment)  │
└───────────────────────────┬──────────────────────────────┘
                            │ Reassembled PacketizedFrame
                            ▼
┌──────────────────────────────────────────────────────────┐
│  Receiver & Reconstruction (Host / Companion App)        │
│  - Consumes live UDP OR deterministic saved recordings   │
│  - Loss-safe reference tracker (detects missing refs,    │
│    bounded dependency depth, fast keyframe resync)       │
│  - Reconstructs valid baseline JPEG for standard decoder │
└──────────────────────────────────────────────────────────┘
```

Robot-specific features (BalaX MPU6886 UART telemetry, attitude prediction, receiver geometric stabilization warp, experimental OV2640 ROI crop) live in the separate consumer repository **`esp32-bot/balax-cam`**, which depends on `ecl_codec` and `ecl_transport` as reusable CMake / ESP-IDF components without forking internals.

---

## 2. Core C++17 APIs (`codec/include/ecl/`)

- **`EncodedFrameView`**: Non-owning view of one encoded JPEG frame (`frame_id`, `capture_timestamp_us`, `data`, `size`, `FrameMetadata{width, height, quality, orientation_flags, attitude_seq, pitch_cdeg, roll_cdeg}`). Note that optional opaque attitude metadata fields are carried passively by the transport envelope so the codec itself never depends on robot math.
- **`CodecOutput`**: Result of `codec_encode(TemporalCodecContext*, const EncodedFrameView&, CodecOutput*)` referencing a caller/context-owned bounded buffer (`mode`, `frame_id`, `reference_frame_id`, `is_keyframe`, `changed_blocks`, `total_blocks`, `bytes`).
- **`codec_decode(TemporalDecoderContext*, const EncodedPayloadView&, ReconstructedFrame*)`**: Decodes keyframe or delta payload against the decoder's bounded reference state. If a delta references a missing or expired `reference_frame_id`, `codec_decode` returns `DecodeStatus::MISSING_REFERENCE` without corrupting output or poisoning future keyframe recovery.

---

## 3. Bounded Memory & Xtensa Port Discipline

To run safely on original ESP32 (520 KB internal SRAM, 4 MB SPI PSRAM):
- **Internal DRAM (`SRAM1`/`SRAM2`)**: Camera DMA descriptors & bounce buffers, Wi-Fi/lwIP buffers, FreeRTOS task stacks, small packetizer/UDP TX buffer (≤ 1.5 KB), and JPEG Huffman lookup tables (~8 KB).
- **External PSRAM (or static/reusable caller-allocated buffers)**:
  - Camera frame buffers (`fb_count = 2`, `CAMERA_GRAB_LATEST`).
  - Optional temporal reference coefficient buffer (only allocated when a DCT temporal mode is enabled!):
    - For QVGA `320×240` 4:2:0 (`20×15 = 300` MCUs, 6 blocks/MCU = 1,800 blocks × 64 × `int16_t` = **230.4 KB** in PSRAM; DC-only reference = **3.6 KB** in internal DRAM!).
    - For VGA `640×480` 4:2:0 (`40×30 = 1,200` MCUs, 7,200 blocks = **921.6 KB** in PSRAM; DC-only reference = **14.4 KB** in internal DRAM!).
  - Scratch output buffer sized once at initialization (`max_frame_bytes`), zero per-frame `malloc`/`free` in the hot loop.
- **Permanent fallback**: `CODEC_PASS_THROUGH` requires **0 bytes** of reference state and zero JPEG parsing CPU.

---

## 4. Streaming-From-DMA Investigation & Incremental Parser Path

Full-frame capture (`esp_camera_fb_get()` → codec → packetize → send) is the verified fallback baseline.

In `firmware/esp32cam/main/cam_driver.cpp`, we also document and design the experimental incremental chunk-streaming hook inspired by `hx-esp32-cam-fpv`:
```
I2S0 DMA EOF interrupt
  → cam_task chunk callback (2–4 KB DRAM DMA half-buffer)
      ├─ [PASS_THROUGH] → immediate MTU packetizer → UDP/Wi-Fi TX (zero full-frame wait)
      └─ [INCREMENTAL DCT] → streaming byte-feeder JPEG parser (SOI → headers → MCU-by-MCU)
                           → emit MCU delta packets per MCU row (requires only 1 MCU row scratch
                             + previous frame reference; no full current-frame JPEG buffer!)
```
Crucial architectural finding:
- Baseline JPEG entropy decoding is **strictly sequential per MCU** (with `DRI`/restart intervals resetting DC predictors). Therefore, **incremental MCU-by-MCU temporal encoding IS theoretically compatible with DMA chunk streaming** once JPEG headers (`DQT`, `DHT`, `SOF0`, `SOS`) arrive at the start of the frame — the encoder only needs the *previous* frame's reference coefficients, not the complete *current* frame buffer!
- However, if an adaptive encoder decides mid-frame to fall back to `PASS_THROUGH` (because a scene cut makes the delta larger than the raw JPEG), streaming-from-DMA cannot retroactively send earlier raw JPEG bytes unless they are also buffered or the switch happens on the next frame (via periodic/adaptive keyframe triggering).
- Therefore, we keep full-frame capture as the rock-solid default until real hardware benchmarks quantify DMA chunk latency vs. PSRAM contention.
