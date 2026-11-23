# Real-Hardware Validation Matrix & Test Plan

All host measurements in `docs/benchmarks.md` are **host algorithmic benchmarks only**. Every item below **[Requires hardware validation]** on a physical AI-Thinker ESP32-CAM (OV2640) and M5StickC Plus + BalaC robot before claiming real-world FPS, latency, stability, or stabilization performance.

---

## 1. Pre-Flight Safety & Wiring Checklist

### AI-Thinker ESP32-CAM Standalone
- [ ] Power `5V` pin from a clean **5.0 V ≥ 1.0 A** supply (never a saggy USB hub or thin jumper wires; AMS1117-3.3 needs ≥ 4.5 V to avoid brownout resets).
- [ ] Verify common `GND` with FTDI / `ESP32-CAM-MB` programmer.
- [ ] Keep MicroSD removed while serial-flashing if `GPIO2` pull-up blocks ROM bootloader; always use **1-bit SDMMC (`GPIO2/14/15`)** at runtime and drive **`GPIO4` LOW** to prevent flash-LED strobing/overheating.
- [ ] Verify board antenna 0 Ω jumper position (PCB vs IPEX/U.FL).

### BalaX + ESP32-CAM Companion Wiring
- [ ] **Power:** Feed ESP32-CAM `5V` from BalaC base external 5 V / dedicated UBEC + common `GND`. **NEVER power ESP32-CAM from the M5StickC Plus 120 mAh internal battery or Grove 5 V boost!**
- [ ] **Mechanical:** Maintain an air gap between ESP32-CAM and M5StickC Plus (StickC Plus battery has no thermistor).
- [ ] **UART link (1-bit SD safe):**
  - M5StickC Plus Grove `GPIO32` (TX) → ESP32-CAM `GPIO3` (`U0RX`)
  - M5StickC Plus Grove `GPIO33` (RX) ← ESP32-CAM `GPIO1` (`U0TX`)
  - Common `GND`
  - Baud: `460800` or `921600` (start at `460800` for bring-up, verify zero CRC errors on scope/counter, then step to `921600`).
  - Disable default ESP-IDF UART0 console logs in `balax-cam` production build (`CONFIG_ESP_CONSOLE_NONE=y`) so boot/log text never corrupts binary attitude frames.

---

## 2. Phase 1 — ESP32-CAM Standalone Validation Matrix

| Test ID | Procedure | Metrics to Record | Pass Criteria |
|---|---|---|---|
| **HW-01** | Boot `firmware/esp32cam`, inspect serial boot log | Chip rev (`D0WDQ6` rev1 vs `D0WD-V3`), PSRAM detected (4 MB), PSRAM clock (40/80 MHz), OV2640 PID (`0x2642`) | Clean boot, PSRAM memtest OK, OV2640 probed |
| **HW-02** | Capture & dump 30 raw OV2640 JPEG frames per resolution (`320×240`, `400×296`, `640×360`, `640×480`, `800×600`) at `QS = 8, 12, 16, 20` | Actual OV2640 JFIF structure (subsampling 4:2:2 vs 4:2:0, DHT/DQT presence, DRI), frame size min/median/p95/max | All dumped frames parse cleanly in host `jpeg_parser` & `libjpeg-turbo` with 0 errors |
| **HW-03** | Capture-only FPS sweep (Wi-Fi TX disabled) across resolutions × `XCLK` (`12, 16, 20 MHz`) | Capture FPS, `EV-VSYNC-OVF` / `FB-OVF` count, free internal heap, free PSRAM | Zero DMA overruns at target `XCLK` |
| **HW-04** | Network-only UDP throughput test (synthetic 1400 B packets, TX cap `+13 dBm` vs default) | Sustained Mbps, packet loss %, RSSI, PCB temperature after 5 min | Stable link, zero brownouts |
| **HW-05** | Simultaneous capture + `CODEC_PASS_THROUGH` UDP stream | Delivered FPS, dropped frames, min internal heap, min PSRAM, glass-to-glass latency (LED/stopwatch test) | Stable baseline ≥ 15 min without OOM/reset |
| **HW-06** | Enable experimental codec modes (`FRAME_SKIP`, `DCT_DC_RESIDUAL`, `DCT_SPARSE_BLOCK`, `DCT_FULL_RESIDUAL`) on static vs moving scenes | On-ESP32 encode time (µs), actual wire bitrate, effective FPS, heap/PSRAM delta, visual PSNR/SSIM vs pass-through | Confirm whether ESP32 CPU/PSRAM cost stays within frame budget |
| **HW-07** | Artificial packet loss (shielding / receiver drop filter 1%, 5%, 10% + burst) | Recovery latency (frames to next keyframe), corrupted frames emitted | Zero permanent stream poisoning; bounded recovery |
| **HW-08** | *(Optional later opt-in)* DMA chunk-streaming & OV2640 clock tuning | End-to-end latency delta vs full-frame baseline, overflow rate | Measurable latency reduction with acceptable stability |

---

## 3. Phase 2 — BalaC / `balax-cam` Integration Matrix

| Test ID | Procedure | Metrics to Record | Pass Criteria |
|---|---|---|---|
| **BC-01** | Verify M5StickC Plus revision (`K016-P` vs Plus2), `MPU6886` `WHO_AM_I = 0x19`, and Grove `G32/G33` UART signal integrity | UART RX packets/sec, CRC error count, dropped/overwritten count | 100 Hz telemetry received with 0 CRC errors |
| **BC-02** | Timestamp alignment calibration (sharp mechanical pitch impulse visible in camera + IMU trace) | Measured attitude-to-exposure latency offset $\Delta t_{\text{cal}}$ (ms) | Repeatable $\Delta t_{\text{cal}}$ within ±5 ms |
| **BC-03** | `STABILIZATION_RECEIVER_WARP` while BalaC balances / pitches ±5° | Residual horizon tilt (deg), receiver warp CPU time (ms) | Visible horizon stabilization with 0 camera disruption |
| **BC-04** | `STABILIZATION_OV2640_CROP_EXPERIMENTAL` at 1 Hz → 5 Hz → 15 Hz → frame rate (writing only `XOFFL`/`YOFFL`/`VHYX` at VSYNC vs `set_res_raw`) | Corrupt/torn JPEG count, SCCB I²C write duration (µs), FPS impact | Document exact safe rate or confirm tearing failure mode |
| **BC-05** | Temporal compression A/B on real balancing robot (unstabilized vs stabilized) | Changed MCUs %, delta bytes/frame, bitrate | Empirically confirm or refute Hypothesis #3 on real silicon |

---

## 4. Expected Failure Modes & Mitigations

1. **Brownout reset loops on Wi-Fi TX spikes:** Power rail sagging below 4.5 V at `5V` input → use dedicated 5 V ≥ 1 A supply + 470 µF electrolytic decoupling cap across `5V`/`GND`.
2. **`EV-VSYNC-OVF` / torn JPEGs:** `PCLK` too fast for I²S0 DMA + PSRAM + Wi-Fi → drop `XCLK` from 20 MHz to 16 MHz or 12 MHz, raise `QS` (`12 → 16`).
3. **OV2640 JFIF header quirks (e.g., omitted DHT or 4:2:2 subsampling):** Our `jpeg_parser` explicitly supports standard Annex K fallback Huffman tables when `DHT` is omitted, and handles `4:2:0`, `4:2:2`, and `4:4:4` chroma subsampling.
4. **PSRAM cache corruption on Rev 1 ESP32-D0WDQ6 (`[CPU-3.9]`):** Enabled `CONFIG_SPIRAM_CACHE_WORKAROUND=y` in `sdkconfig.defaults`.
5. **OV2640 crop register writes tearing JPEG frames:** Kept behind `STABILIZATION_OV2640_CROP_EXPERIMENTAL` opt-in flag; `STABILIZATION_RECEIVER_WARP` is the permanent safe default.
6. **PSRAM cache-miss latency making full DCT temporal coding slower than raw pass-through on ESP32:** `CODEC_PASS_THROUGH` is always available as a runtime/compile-time fallback, and `DCT_DC_RESIDUAL` / `FRAME_SKIP` avoid full 64-coeff PSRAM sweeps.
