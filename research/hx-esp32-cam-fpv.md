# Competitive Teardown & Roadmap: `RomanLut/hx-esp32-cam-fpv`

- **Reference Repository:** https://github.com/RomanLut/hx-esp32-cam-fpv (MIT License; forked from `jeanlemotan/esp32-cam-fpv`)
- **Snapshot Analyzed:** `master` branch (Release `v0.6.5`, 1,641 commits, inspected Nov 2026)
- **Purpose:** Give future agents and contributors an exact, source-verified engineering breakdown of **why `hx-esp32-cam-fpv` currently outperforms `esp32-bot/esp32-cam` on raw ESP32-CAM FPS/latency/resilience**, where our architecture already holds unique advantages, and the concrete milestone roadmap to **beat it**.

---

## 1. Benchmark Targets to Beat (AI-Thinker ESP32-CAM + OV2640)

Verified from `RomanLut/hx-esp32-cam-fpv` (`README.md`, `doc/latency.md`, `doc/wifi.md`, `doc/image_resolution_selection.md`, `components/air/main.cpp`):

| Metric | `hx-esp32-cam-fpv` (ESP32 + OV2640) | `hx-esp32-cam-fpv` (ESP32-S3 + OV5640) | Target for `esp32-bot/esp32-cam` to Beat |
|---|---|---|---|
| **Resolutions & FPS (stock XCLK 12 MHz)** | `640×360` 30 fps, `640×480` 30 fps, `800×456` 30 fps, `800×600` 30 fps, `1024×576` 12 fps, `1280×720` 12 fps | `640×360` 30/50 fps, `800×456` 30/50 fps, `1024×576` 30 fps, `1280×720` 30 fps | **Sustained `800×456` @ 30–40 fps** on ESP32 + OV2640 without frame drops |
| **Overclocked OV2640 FPS (XCLK 16 MHz)** | **`640×360` 40 fps, `640×480` 40 fps, `800×456` 40 fps** | — | Optional 40 fps mode with thermal guard |
| **Glass-to-Glass Latency** | **90–110 ms** (measured on RPi Zero 2W + 60 Hz TV; author notes ~40–80 ms should be achievable) | 90–110 ms | **< 65 ms** glass-to-glass over direct Wi-Fi / Web Serial |
| **Effective Air Throughput** | ~8–10 Mbps @ 24 Mbps / MCS3 26 Mbps PHY (up to ~14 Mbps on-field; ESP32 ceiling ~23 Mbps) *including* FEC 6/12 | Up to ~29 Mbps ceiling | Higher **perceptual quality (SSIM/PSNR) at equal Mbps** via adaptive DCT temporal coding + lightweight FEC |
| **Packet Loss Resilience** | Reed-Solomon FEC `k=6, n=12` (recovers 50% packet loss per block, zero single-packet frame drops) | `6/12` (2.4 GHz) or `6/8` (5.8 GHz on C5) | Zero frame drop up to **15–25% packet loss** without doubling bandwidth |
| **Air Unit Power Draw** | `< 300 mA @ 5 V` (~1.5 W) | `< 300 mA @ 5 V` | `< 300 mA` streaming + **cool-idle / sleep** (`< 60 mA` idle, `~10 µA` deep sleep) |
| **Ground Station Requirement** | Requires **RTL8812AU** monitor-mode USB dongle + Linux/Android/Quest for Raw Broadcast; APFPV SoftAP fallback for ground vehicles | Same | **Zero special hardware** (any browser via `balax-remote` GitHub Pages + optional direct SoftAP / Web Serial) |

---

## 2. How `hx-esp32-cam-fpv` Works Under the Hood (Source-Verified)

Inspecting `components/esp32-camera/driver/cam_hal.c`, `components/air/main.cpp`, `components/air/wifi.cpp`, `components/common/fec_codec.cpp`, and `air_firmware_esp32cam/sdkconfig.defaults` reveals **six concrete engineering choices** that drive its performance:

### 2.1 Sub-Frame DMA Streaming & Zero-PSRAM Hot Path (`cam_hal.c` + `main.cpp`)
- **What standard `esp32-camera` does (and what we currently do):** Allocates 2 full JPEG framebuffers in **external SPI PSRAM** (`CAMERA_FB_IN_PSRAM`), waits for the entire frame to finish capturing (`esp_camera_fb_get()`), then reads it back across the SPI PSRAM cache (paying Rev 1 `[CPU-3.9]` cache overhead and full-frame capture latency of `16–33 ms`).
- **What `hx-esp32-cam-fpv` does:**
  - Forks `esp32-camera/driver/cam_hal.c` to add `data_available_callback(void* cam_obj, const uint8_t* data, size_t count, bool last)` directly inside the DMA completion path.
  - Configures `.fb_location = CAMERA_FB_IN_DRAM` (`main.cpp:3352`), `.fb_count = 2`, `.grab_mode = CAMERA_GRAB_LATEST`.
  - Streams each DMA chunk immediately into `s_fec_encoder` → `FEC Encoder task` → `Wifi TX task` **while the OV2640 is still scanning the rest of the frame** (like HDZero). This saves **10–20 ms** of latency and eliminates PSRAM bus stalls on the hot path.
  - **OV2640 trailing-garbage workaround (`main.cpp:2813–2948`):** When VSYNC fires near the end of an OV2640 JPEG frame, DMA can deliver an extra trailing block filled with zeros/garbage. `hx` checks whether a DMA chunk starts with **16 consecutive zero bytes** to discard trailing garbage in O(1) time without scanning the JPEG stream byte-by-byte on the ESP32.

### 2.2 OV2640 Custom 16:9 SVGA Windowing & 16 MHz Overclock (`main.cpp:1734–1791`)
- Standard `esp32-camera` `FRAMESIZE_P_HD` (`800×448`) switches OV2640 into **UXGA sensor mode**, capping it at **12–13 fps**.
- `hx-esp32-cam-fpv` stays in **`OV2640_MODE_SVGA` (mode 1)** and crops vertically to 16:9 using `set_res_raw`:
  ```cpp
  // 800x456 @ 30 fps (40 fps @ 16 MHz XCLK)
  s->set_res_raw(s, 1 /*OV2640_MODE_SVGA*/, 0, 0, 0, 0, 72, 800, 600 - 144, 800, 456, false, false);
  // 640x360 @ 30 fps (40 fps @ 16 MHz XCLK) — SVGA 800x456 window scaled by DSP
  s->set_res_raw(s, 1 /*OV2640_MODE_SVGA*/, 0, 0, 0, 0, 72, 800, 600 - 144, 640, 360, false, false);
  ```
- When `ov2640HighFPS` is enabled for `VGA`, `VGA16`, or `SVGA16`, it raises XCLK from **12 MHz → 16 MHz** (`s->set_xclk(s, LEDC_TIMER_0, 16)`), pushing OV2640 from 30 fps to **40 fps** (silicon-dependent).

### 2.3 Three-Factor Closed-Loop Adaptive JPEG Compression (`K1 × K2 × K3`) + Nonlinear LUT (`main.cpp:2600–2800`)
- Boots at **`jpeg_quality = 63`** (smallest frames) so startup never overflows queues.
- Every completed frame updates three multiplicative coefficients (`0.05..1.0`):
  1. **`K1` (Bandwidth Budget):** Computes `targetFrameSize = (0.5 * wifiPhyRate * fec_k / fec_n) / fps` (capped by SD DVR write speed if recording) and scales `K1 *= targetFrameSize / video_full_frame_size`.
  2. **`K2` (Queue Capacity Margin):** Ensures `video_full_frame_size` stays below `0.7 * (wlanQueueCapacity * fec_k / fec_n + targetFrameSize)` (assuming the next frame can jump +30% in scene complexity). Drops fast on overshoot, recovers **10× slower**.
  3. **`K3` (Real-Time Wi-Fi Queue Backpressure — the most important factor):** Samples `min` and `max` outgoing Wi-Fi queue occupancy during each frame:
     - Queue overflow → immediately snaps **`K3 = 0`** (instant quality drop rather than dropping frames).
     - `min > 10%` or `max > 60%` → forces `QS` up by at least 1 step.
     - `max < 40%` → gently recovers quality (`+ (40 - max) / 10000`).
- Maps `quality1 = 8 + 55 * (1 - K1 * K2 * K3)` through a **nonlinear `recode[]` lookup table** because OV2640 `QS` vs. compressed byte size is steep at low `QS` (`8..20`) and flat at high `QS` (`40..63`).
- **Sharpness backoff (`main.cpp:2796`):** Automatically reduces OV2640 ISP sharpness (`-2` if `QS > 50`, `-1` if `QS > 45`, `0` if `QS > 40`) to prevent mosquito noise/ringing from inflating JPEG size under congestion.

### 2.4 Connectionless Raw 802.11 Packet Injection & Backpressure-Aware APFPV UDP (`wifi.cpp`)
- **Primary mode (`Raw80211`):** Calls `esp_wifi_80211_tx(WIFI_IF_STA, ...)` directly with an explicit PHY rate via `esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_24M / MCS3_LGI)`. Zero TCP/IP stack, zero 802.11 MAC ACKs, zero retry stalls.
- **Zero-dongle fallback (`APFPV` SoftAP UDP, `wifi.cpp:558–735`):**
  - Hosts an **open SoftAP** (`WIFI_AUTH_OPEN`, `WIFI_CIPHER_TYPE_NONE`, `max_connection = 1`, `WIFI_PS_NONE`, UDP port `5600`, `SO_SNDBUF = 8 KB`, `SO_SNDTIMEO = 20 ms`).
  - Eliminates the home-router double-hop (`ESP32 → Router → Laptop`) and WPA2 crypto overhead.
  - Crucially, when `sendto()` returns `EAGAIN`, `EWOULDBLOCK`, `ENOMEM`, or `ENOBUFS`, `wifi_tx_proc` **keeps the packet in `s_wlan_outgoing_queue`** so queue occupancy climbs and immediately triggers `K3` adaptive quality backoff instead of silently dropping frames!

### 2.5 Block-Level Reed-Solomon FEC (`components/common/fec_codec.cpp`, `doc/fec.md`)
- Coalesces small chunks up to MTU (`AIR2GROUND_MAX_MTU`), groups into `k` data packets, and emits `n - k` Reed-Solomon parity packets (default `k=6, n=12` on 2.4 GHz; `6/8` on 5.8 GHz).
- Any 6 of 12 packets in a block reconstruct all 6 data packets without waiting for ARQ retransmissions.

### 2.6 Built-In Profiling & Ground-Station Post-Processing
- **On-device VCD profiler (`vcd_profiler.cpp`):** Records microsecond transitions for camera DMA, FEC encode, Wi-Fi TX, and queue overflows directly to a `.vcd` waveform file (viewable in GTKWave) so latency bottlenecks are directly observable.
- **Ground Station (`gs/`, `components_gs/`):** Uses `TurboJPEG` (1–7 ms decode), OpenGL ES **JPEG deblocking**, **fisheye lens distortion correction shader**, and **EIS**.

---

## 3. Gap Analysis: Why We Aren't Beating `hx-esp32-cam-fpv` Yet

### 3.1 Where `hx-esp32-cam-fpv` Currently Beats Us

| Bottleneck | Current `esp32-cam` / `balax-cam` Behavior | Impact vs `hx-esp32-cam-fpv` |
|---|---|---|
| **1. Full-Frame PSRAM Capture** | `esp_camera_fb_get()` with `CAMERA_FB_IN_PSRAM` waits for the full JPEG before touching it. | **+15 to +33 ms latency** + PSRAM SPI cache misses (`[CPU-3.9]`) on every frame. |
| **2. No Forward Error Correction (FEC)** | `ECL1` (`transport/include/ecl/protocol.hpp`) sends raw chunks with CRC-32 only. Losing **1 of 10 chunks** discards the whole frame — and if `DCT_SPARSE_BLOCK` is active, stalls decoding until the next keyframe. | Visible stuttering/freezes under normal 2.4 GHz interference where `hx` runs smoothly. |
| **3. Home-Router STA Double-Hop & No Fixed PHY Rate** | STA connects to an external Wi-Fi router (2× airtime, router buffering/jitter, CSMA/CA contention, lwIP default rate control). | Adds **+10 to +40 ms jitter**, lowers sustainable throughput, and hurts consistency. |
| **4. CPU Cost of Compressed-Domain DCT Codec on High-Res Fast Motion** | Parsing Huffman + diffing DCT blocks + re-encoding on ESP32 (`codec/src/temporal_codec.cpp`) runs on the entire PSRAM frame on Core 1. Great for low-motion/low-bandwidth links, but adds CPU latency on fast 30–40 fps `800×456` streams where raw hardware JPEG fits in Wi-Fi airtime. | Caps high-res FPS when enabled unconditionally; needs **automatic bandwidth/motion mode switching**. |
| **5. Simpler Adaptive Quality Controller** | `balax-cam` `step_adaptive_quality` reacts to drops/congestion linearly without `hx`'s nonlinear `QS` LUT, 30% headroom predictor (`K2`), explicit TX-queue-depth watermarks (`K3`), or ISP sharpness backoff. | Slower convergence on sudden scene complexity spikes; more queue-drop frames. |
| **6. No End-to-End Microsecond Latency Telemetry** | We track FPS and kbps, but lack per-stage timing (`VSYNC → DMA done → encode → packetize → TX done → browser decode → canvas paint`). | Hard for agents to prove which stage causes latency. |

### 3.2 Where Our Architecture Already Beats `hx-esp32-cam-fpv` (Keep & Leverage!)

1. **Zero-Dongle Browser Ground Station (`balax-remote` on GitHub Pages):** Runs on any laptop/tablet/phone over Wi-Fi or 921600-baud Web Serial USB — no `RTL8812AU` monitor-mode adapter or Raspberry Pi/Radxa SBC required.
2. **Closed-Loop Compressed-Domain Temporal Codec (`codec/`):** `hx` itself notes in `README.md` that MJPEG wastes huge bandwidth. On low-to-moderate motion scenes, our `DCT_SPARSE_BLOCK` + `FRAME_SKIP` cuts bitrate by **50–90%** (verified in [`docs/benchmarks.md`](../docs/benchmarks.md)), letting us deliver **dramatically sharper image quality (lower `QS`) at the same Wi-Fi/USB bitrate** once paired with automatic pass-through switching.
3. **True IMU-Coupled Stabilization (`balax-cam`):** Hardware OV2640 window crop + receiver warp driven directly by `MPU6886` attitude beats vision-only EIS on latency and motion-blur/rolling-shutter immunity.
4. **Cool-by-Default Thermal Management:** 40 MHz DFS + `GPIO32` OV2640 power gate + thermal state machine avoids continuous 240 MHz thermal throttling/brownouts.
5. **100% Host-Testable & WASM-Portable Engine:** Exact C++17 `codec/` + `transport/` runs identically in desktop unit/fuzz tests, ESP-IDF firmware, and browser WASM.

---

## 4. Prioritized Roadmap for Future Agents to Beat `hx-esp32-cam-fpv`

> **Golden Rule:** Never guess. Complete **P0** first so every later milestone proves its exact millisecond latency, FPS, and SSIM/PSNR delta on real AI-Thinker ESP32-CAM hardware.

### P0 — Microsecond Pipeline Profiler & Glass-to-Glass Benchmark Harness
- **Goal:** Instrument every stage (`vsync_us`, `fb_ready_us`, `codec_us`, `tx_queue_us`, `tx_done_us`, `rx_reassemble_us`, `decode_us`, `paint_us`) and log rolling p50/p95 stats in `STATUS` JSON (and optionally export VCD/JSON traces).
- **Files:** `firmware/esp32cam/main/cam_metrics.hpp`, `firmware/esp32cam/main/app_main.cpp`, `balax-cam/firmware/esp32cam_balax/main/app_main.cpp`, `balax-remote/src/FpvCamPanel.tsx`.
- **Exit Criterion:** Repeatable LED/stopwatch glass-to-glass latency measurement recorded in `docs/hardware-validation.md` with per-stage millisecond breakdown.

### P1 — Sub-Frame DMA Streaming (`CAMERA_FB_IN_DRAM` + Chunk Callback) for `PASS_THROUGH`
- **Goal:** Cut **15–30 ms** capture latency and remove PSRAM cache stalls when streaming hardware JPEG (`PASS_THROUGH` mode).
- **Design:**
  - Vendor/patch a minimal `esp32-camera` streaming hook (Apache-2.0 / MIT compatible) exposing `data_available_callback(chunk, len, is_last)` from `cam_hal.c`, and allocate `fb_location = CAMERA_FB_IN_DRAM` when in streaming-chunk mode.
  - Add OV2640 16-zero trailing-block filter (as in `hx` `main.cpp:2948`) to strip post-EOI DMA garbage in O(1).
  - Packetize each DMA chunk directly into `ECL1` packets on the fly (`chunk_count` finalized or signaled via `ECL1_FLAG_END_OF_FRAME` bit in `flags`, keeping backwards compatibility with fixed `chunk_count` frames!).
- **Files:** `transport/include/ecl/protocol.hpp`, `transport/src/packetizer.cpp`, `firmware/esp32cam/main/cam_driver.cpp`.
- **Exit Criterion:** Sustained `800×456` @ **30 fps** (12 MHz XCLK) and **40 fps** (16 MHz XCLK) on AI-Thinker ESP32-CAM with capture-to-first-packet latency **< 6 ms**.

### P2 — Adaptive Lightweight FEC in `ECL1` (Systematic XOR / Reed-Solomon)
- **Goal:** Beat `hx`'s `6/12` 100% bandwidth penalty by using **adaptive systematic FEC** (`0%` overhead on clean USB/LAN, `~15–33%` XOR/RS parity on lossy Wi-Fi, e.g. `k=6, n=8` or `k=8, n=10`).
- **Design:**
  - Add an `ECL1_FLAG_FEC_PARITY` flag and tiny FEC block header (`block_id`, `k`, `n`) in `transport/` (100% platform-independent C++17 + unit tests + impairment replay tests in `host/replay_cli.cpp` + browser decoder in `balax-remote/src/browserEcl.ts`).
  - Keep `codec/` completely untouched — FEC lives strictly in `transport/`.
  - **Extra win for temporal DCT mode:** Protecting keyframes and sparse DCT deltas with 20% FEC prevents multi-frame stalls without paying `hx`'s 100% `6/12` bitrate tax.
- **Files:** `transport/include/ecl/protocol.hpp`, `transport/src/fec.cpp` (new), `transport/tests/test_transport.cpp`, `balax-remote/src/browserEcl.ts`.
- **Exit Criterion:** Under simulated **15% random + burst packet loss**, frame delivery rate **> 98%** with **< 35% bandwidth overhead** (vs `hx`'s 100% overhead at `6/12`).

### P3 — Direct-Link Transport Options (Open SoftAP UDP + Optional Raw 802.11 / ESP-NOW)
- **Goal:** Eliminate home-router double-hop latency (+10–40 ms) while preserving zero-dongle compatibility.
- **Design:**
  - **Tier 1 (Zero extra hardware):** Add optional **Direct SoftAP Open UDP** mode (`WIFI_AUTH_OPEN`, `WIFI_CIPHER_TYPE_NONE`, `WIFI_PS_NONE`, `max_connection=1`, non-blocking `sendto` with explicit `EAGAIN/ENOMEM` backpressure queue) — works directly from any laptop/phone without a router.
  - **Tier 2 (Optional JoyX / ESP32-S3 USB Bridge or RTL8812AU):** Pluggable `Raw80211` / `ESP-NOW` transport backend (`esp_wifi_80211_tx` + `esp_wifi_config_80211_tx_rate(WIFI_PHY_RATE_24M / MCS3)`), so **JoyX** (already an ESP32 USB bridge!) can act as a tiny plug-and-play raw-802.11 / ESP-NOW receiver dongle for `balax-remote` via Web Serial!
- **Exit Criterion:** Direct SoftAP UDP cuts round-trip / one-way network latency to **< 10 ms p95** at 8–12 Mbps; optional JoyX raw-802.11 bridge eliminates Wi-Fi association drops completely.

### P4 — Queue-Depth-Coupled Adaptive Quality (`K1×K2×K3`) + Auto Codec Mode Switcher
- **Goal:** Beat `hx` on **image quality per Mbps** AND **zero queue overflows**.
- **Design:**
  - Implement a reusable, host-tested `AdaptiveRateController` in `transport/` (or `codec/`) featuring:
    - Nonlinear OV2640 `QS` LUT (`8..63`),
    - Multiplicative `K1` (bandwidth budget), `K2` (30% frame-growth headroom), `K3` (TX queue depth watermark + instant overflow circuit breaker),
    - OV2640 sharpness backoff at `QS > 40/45/50`.
  - **Auto Codec Switching (our secret weapon over `hx`):**
    - When link headroom is high & motion is fast → **`PASS_THROUGH` DMA streaming** (minimum latency, 30–40 fps `800×456`).
    - When link is constrained (e.g. 921600-baud USB UART, crowded 2.4 GHz, long range) or scene motion is low → automatically switch to **`DCT_SPARSE_BLOCK` / `FRAME_SKIP`** to deliver **2–4× higher visual quality** than `hx`'s raw MJPEG at the same bitrate!
- **Exit Criterion:** Zero TX queue overflows across 5-minute stress tests on crowded 2.4 GHz; **≥ +3 dB PSNR / +0.05 SSIM** over raw MJPEG at 2–4 Mbps constrained links.

### P5 — Browser WebGL Post-Processing (`balax-remote`)
- **Goal:** Match and beat `hx`'s OpenGL ES ground station directly inside the browser at ~0 ms CPU cost.
- **Design:**
  - Decode JPEG via `createImageBitmap()` (hardware-accelerated on modern phones/laptops, ~1–3 ms) and upload to a single WebGL2 fragment shader pass doing:
    1. **8×8 JPEG block-boundary deblocking filter** (huge perceptual upgrade at `QS = 25..50`),
    2. **Radial fisheye lens undistortion** (configurable per lens),
    3. **IMU attitude horizon stabilization warp** (`pitch`/`roll` from `ECL1` header).
- **Exit Criterion:** Decode-to-paint latency **< 8 ms** in Chrome/Safari with visibly cleaner `QS=35` video than raw canvas 2D JPEG rendering.

### P6 — Optional Next-Gen Silicon Path (`ESP32-S3` + `OV5640` / `ESP32-C5` 5 GHz)
- Once AI-Thinker `ESP32 + OV2640` beats `hx-esp32-cam-fpv`, the exact same `codec/` and `transport/` components compile cleanly for:
  - **XIAO ESP32-S3 Sense + OV5640** (`20 MHz` DMA, `800×456 @ 50 fps` or `1280×720 @ 30 fps`, better low-light/WDR, native USB-CDC at ~6–10 Mbps instead of 921600-baud CH340!).
  - **ESP32-C5 + OV5640** (clean 5.8 GHz band → lower FEC overhead `6/8` and far less interference).

---

## 5. Key Pitfalls Documented by `hx-esp32-cam-fpv` (Don't Repeat!)

1. **Don't rely on `FRAMESIZE_P_HD` on OV2640:** It selects UXGA mode internally (`12–13 fps`). Always use `set_res_raw(s, 1 /*SVGA*/, ...)` for 30–40 fps 16:9 modes (`640×360`, `800×456`).
2. **Don't start streaming at low `QS` (high quality):** Always start at high `QS` (`48–63`) and ramp quality up smoothly as queue headroom is proven, or the very first frames will jam the Wi-Fi TX queue.
3. **Don't blindly drop packets on `sendto() == ENOMEM/EAGAIN`:** Treat socket/driver backpressure as the primary feedback signal (`K3`) for the adaptive quality controller.
4. **Don't use long unshielded camera flex cables near RF/GPS:** 10–16 MHz parallel `PCLK`/`D0–D7` signals radiate strong broadband EMI (`doc/unsuccessful_attempts.md`).
5. **Channel selection matters on 2.4 GHz:** Small ESP32 PCB/ceramic antennas are best tuned around channels **3–7**; channel 11 often suffers steep attenuation.
6. **Licensing hygiene:** `RomanLut/hx-esp32-cam-fpv` is MIT-licensed (and `espressif/esp32-camera` is Apache-2.0 / MIT). Always preserve copyright/license headers if porting any MIT/Apache code, or re-implement cleanly inside `esp32-bot/esp32-cam`'s modular `codec/` and `transport/` layers.
