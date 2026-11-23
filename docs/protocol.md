# Wire Protocol Specification — `ECL1` (v1)

## 1. Design Goals

- Compact, fixed-size binary header (little-endian) suitable for ESP32 UDP datagrams and future raw 802.11 / FEC transports.
- Stateless per-packet integrity validation (IEEE CRC-32) so corrupted or truncated packets are rejected immediately.
- Zero dependency on ordered reliable TCP: lost packets drop only the affected frame (or trigger keyframe resync on missing temporal reference), never stalling the live stream.
- Explicit temporal dependency signaling (`reference_frame_id`, `FLAGS_KEYFRAME`, `codec_mode`).
- Optional passive camera-orientation metadata slot (used by `balax-cam` without coupling the transport to robot logic).

---

## 2. Packet Layout (`ECL1`, 32-Byte Header + Payload + 4-Byte Trailer CRC32)

All multi-byte integers are **little-endian**.

| Offset | Size | Field | Description |
|---:|---:|---|---|
| 0 | 2 | `magic` | `0x4C43` (`'C','L'` — Camera Livestream) |
| 2 | 1 | `version` | Protocol version = `1` |
| 3 | 1 | `flags` | Bit 0: `KEYFRAME`, Bit 1: `LAST_CHUNK`, Bit 2: `HAS_META_ATTITUDE`, Bit 3: `END_OF_STREAM` |
| 4 | 1 | `stream_id` | Logical stream identifier (`0..255`) |
| 5 | 1 | `codec_mode` | `0=PASS_THROUGH`, `1=FRAME_SKIP`, `2=GENERIC_ZLIB`, `3=DCT_DC_RESIDUAL`, `4=DCT_LOW_AC_RESIDUAL`, `5=DCT_FULL_RESIDUAL`, `6=DCT_SPARSE_BLOCK` |
| 6 | 2 | `chunk_id` | 0-based chunk index within this frame (`0 .. chunk_count - 1`) |
| 8 | 2 | `chunk_count` | Total chunks in this frame (`>= 1`, bounded by `kMaxChunksPerFrame = 512`) |
| 10 | 2 | `payload_len` | Payload bytes in this packet (`0 .. kMaxPayloadPerPacket`) |
| 12 | 4 | `frame_id` | Monotonic 32-bit frame counter (wrap-safe modular comparison) |
| 16 | 4 | `reference_frame_id` | Frame ID this delta depends on (`== frame_id` for keyframes / pass-through) |
| 20 | 4 | `timestamp_us_low` | Capture timestamp in microseconds (low 32 bits; wraps every ~71.6 min) |
| 24 | 2 | `width` | Frame width in pixels |
| 26 | 2 | `height` | Frame height in pixels |
| 28 | 2 | `meta_pitch_cdeg` | Optional passive pitch metadata in 0.01° (`0` if `HAS_META_ATTITUDE` unset) |
| 30 | 2 | `meta_roll_cdeg` | Optional passive roll metadata in 0.01° (`0` if `HAS_META_ATTITUDE` unset) |
| 32 | `payload_len` | `payload` | Chunk bytes of the codec frame payload |
| `32 + payload_len` | 4 | `crc32` | IEEE 802.3 CRC-32 over bytes `[0 .. 32 + payload_len - 1]` |

**Total overhead per packet:** `36 bytes` (32-byte header + 4-byte CRC-32).
At a conservative LAN/Wi-Fi UDP MTU of `1400 bytes` (safe below 1500-byte Ethernet MTU minus IPv4+UDP headers = 1472 bytes), maximum payload per packet is `1364 bytes` (**2.57% framing overhead**).

---

## 3. Sequence Wrap-Around & Reassembly Rules

1. **Modular frame ID ordering:** `frame_id_newer(a, b)` is defined as `static_cast<int32_t>(a - b) > 0`, handling `0xFFFFFFFF → 0x00000000` wrap-around seamlessly.
2. **Bounded reassembly slots:** The depacketizer maintains a small fixed ring of in-flight frames (default `kReassemblySlots = 4`), each with a fixed maximum payload capacity (e.g., `128 KB`). No dynamic per-packet allocation occurs.
3. **Deduplication & reordering:** Duplicate `(frame_id, chunk_id)` packets are ignored idempotently; out-of-order chunks within a frame are placed directly at `chunk_id * nominal_chunk_size` (or offset table via `chunk_id` + final chunk size tracking; every non-final chunk uses a consistent stride stored from chunk 0 or negotiated `max_payload`, and the receiver validates consistent total byte length).
   - *Implementation detail:* To handle arbitrary chunk sizes cleanly even when chunk 0 arrives *out of order* after chunk 1, all chunks `0 .. chunk_count - 2` MUST have identical payload length `chunk_stride` equal to the sender's `max_payload_bytes`, and only the final chunk (`chunk_id == chunk_count - 1`) may be shorter (`<= chunk_stride`). If a non-final chunk arrives first, its `payload_len` establishes `chunk_stride`; if only the final chunk arrives first in a multi-chunk frame, it is buffered in a dedicated tail slot until any non-final chunk establishes `chunk_stride`. This guarantees exact out-of-order reassembly even when chunk `N-1` arrives before chunk `0`!
4. **Frame expiration:** Whenever a newer frame completes or exceeds `max_frame_age` slots behind the newest seen `frame_id`, incomplete older frames are immediately evicted and counted as dropped. The receiver NEVER stalls waiting indefinitely for lost packets.
5. **Malformed packet rejection:** Any packet failing minimum length (`< 36 B`), magic/version check, `payload_len` vs. datagram length mismatch, `chunk_count == 0`, `chunk_id >= chunk_count`, `chunk_count > kMaxChunksPerFrame`, inconsistent non-final chunk stride, or CRC-32 mismatch is rejected and counted in receiver error counters.

---

## 4. Temporal Delta Payload Formats (Inside Reassembled Frame)

- **`PASS_THROUGH (0)`**: Raw hardware/source JPEG bytes (`0xFFD8 ... 0xFFD9`) unchanged. Byte-identical recovery under zero loss.
- **`FRAME_SKIP (1)`**: 4-byte marker (`'S','K','I','P'`). Decoder re-emits the referenced frame unchanged.
- **`GENERIC_ZLIB (2)`**: 4-byte uncompressed length (LE) + standard zlib-compressed raw JPEG bytes (host control experiment).
- **`DCT_DC_RESIDUAL (3)` / `DCT_LOW_AC_RESIDUAL (4)` / `DCT_FULL_RESIDUAL (5)` / `DCT_SPARSE_BLOCK (6)`**:
  - **Keyframe (`FLAGS_KEYFRAME` set)**: Raw valid JPEG bytes! This means keyframes are **100% standard JPEGs** — a passive observer or fallback decoder can display every keyframe directly without custom decoding, and both encoder and decoder initialize their quantized DCT reference state from the exact same baseline JPEG bytes.
  - **Delta frame (`FLAGS_KEYFRAME` clear)**: Compact binary header (`ECLD`, version, submode, `ac_limit`, `mcu_count`, `changed_mcu_count`, bitmask/varint-coded quantized coefficient residuals or updated MCU blocks) applied to the decoder's reference quantized DCT state, then re-emitted as a valid baseline JPEG via `jpeg_writer`.
  - **Loss safety:** Every delta frame carries `reference_frame_id`. If `decoder.last_valid_frame_id != input.reference_frame_id` or `dependency_depth > max_dependency_depth`, the decoder immediately rejects the delta (`MISSING_REFERENCE`), invalidates chain continuation, and waits cleanly for the next keyframe (or requests an immediate keyframe over the control backchannel when connected).
