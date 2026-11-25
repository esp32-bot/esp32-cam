# Research Basis & Evidence Classification

Every technical statement in this document is explicitly tagged with its provenance:
- **[Verified upstream fact]** — confirmed directly in upstream source code or official documentation.
- **[Observed locally]** — verified on the development host or local repositories.
- **[Measured in host tests]** — measured on the host test harness (not ESP32 hardware).
- **[Measured in emulator]** — measured inside a simulator/emulator (not hardware timing).
- **[Hypothesis]** — engineering hypothesis to be tested empirically.
- **[Requires hardware validation]** — cannot be established without physical AI-Thinker ESP32-CAM + OV2640 (and BalaC/M5StickC Plus) hardware.

---

## 1. Local Machine & Repository Discovery

- **[Observed locally]** Development host: Apple M5 (`arm64`), 10 CPU cores, 16 GiB RAM, macOS 27.0 (Darwin 27.0.0), Apple Clang 21.0.0, CMake 4.4.3, Ninja 1.13.2, Python 3.15.2.
- **[Observed locally]** ESP-IDF `v6.1.1` installed with `xtensa-esp-elf-g++` (`esp-15.2.0_20251204`, GCC 15.2.0) targeting original `esp32`.
- **[Observed locally]** Host libraries available via Homebrew/SDK: `jpeg-turbo 3.2.1`, `zlib` (macOS SDK), `lz4 1.11.1`, `zstd 1.5.7`, `libpng 1.6.58`.
- **[Observed locally]** No local QEMU (`qemu-system-xtensa`), Wokwi CLI, Docker/Podman, PlatformIO, or Arduino CLI installation is present on the host.
- **[Observed locally]** Sibling `esp32-bot` repositories discovered (all containing active uncommitted work that was left untouched):
  - `balax` (branch `main`, ESP-IDF v6.1.1): M5StickC Plus (`ESP32-PICO-D4`, `MPU6886`, `AXP192`) self-balancing robot firmware. Contains hardware silicon reference docs (`docs/silicon/M5STICKC_PLUS.md`, `ESP32_CAM.md`, `OV2640.md`) and 6-DOF quaternion fusion (`main/imu_fusion.h`: body frame `+X` left, `+Y` up, `+Z` forward; Euler `pitch` + = leaning back, `roll` + = leaning right, `yaw` + = CCW).
  - `balax-remote` (branch `main`): Web remote & 3D telemetry PWA.
  - `joyx` (branch `main`): M5StickC Plus physical joystick + ESP-NOW companion.
  - `tools` (branch `main`): Shared ESP-IDF components (`m5stickc_plus`, `gfx`, `settings`, `console_commands`, `ota`) and CLI helpers.
  - Neither `esp32-cam` nor `balax-cam` existed locally or remotely in `@esp32-bot`.
  - None of the existing `esp32-bot` repositories contain a `LICENSE` file yet; therefore no `LICENSE` file is prematurely imposed here until project licensing is explicitly chosen. All code written in this repository is clean-room and avoids copying GPL code.

---

## 2. Upstream Camera Driver: `espressif/esp32-camera`

- **[Verified upstream fact]** Repository license is mixed per-file: core HAL/driver (`driver/cam_hal.c`, `driver/esp_camera.c`, `target/esp32/ll_cam.c`) is **Apache-2.0**; OpenMV-derived sensor and SCCB files (`sensors/ov2640.c`, `sensors/private_include/ov2640_regs.h`, `driver/sccb.c`) are **MIT**.
- **[Verified upstream fact]** `sensors/ov2640.c` implements `set_window(sensor_t*, ov2640_sensor_mode_t, offset_x, offset_y, max_x, max_y, w, h)` (invoked via `sensor->set_res_raw`), which divides `max_x`/`max_y`/`w`/`h` by 4 and writes DSP-bank registers `HSIZE`, `VSIZE`, `XOFFL`, `YOFFL`, `VHYX`, `TEST`, `ZMOW`, `ZMOH`, `ZMHH`.
- **[Verified upstream fact]** `set_window()` in `sensors/ov2640.c` also enters `R_BYPASS_DSP_BYPAS`, rewrites the entire mode register table (`ov2640_settings_to_cif/svga/uxga`), rewrites `CLKRC` and `R_DVP_SP`, re-enables DSP, calls `vTaskDelay(10 ms)`, and calls `set_pixformat()`. Consequently, calling `set_res_raw()` per frame is far too disruptive for frame-rate stabilization.
- **[Hypothesis / Requires hardware validation]** Updating only `{BANK_SEL=BANK_DSP, XOFFL, YOFFL, VHYX}` at VSYNC/frame boundaries (after configuring `HSIZE`/`VSIZE`/`ZMOW`/`ZMOH` once) may allow low-cost ROI shifting without a 10 ms DSP reset, or may induce JPEG tearing/corruption if latched mid-frame. This must be tested on physical OV2640 silicon.
- **[Verified upstream fact]** `driver/cam_hal.c` receives I2S0 parallel DMA buffers in `cam_task` via `CAM_IN_SUC_EOF_EVENT` and `CAM_VSYNC_EVENT`, copies chunks into frame buffers (`ll_cam_memcpy`), checks JPEG SOI (`0xFFD8`) and EOI (`0xFFD9`), and supports `fb_count = 2` with `CAMERA_GRAB_LATEST` to drop stale queued frames rather than building FIFO latency.
- **[Verified upstream fact]** On original AI-Thinker ESP32-CAM (`ESP32-S` / `ESP32-D0WDQ6` rev 1.x or `D0WD-V3`), PSRAM shares the SPI bus (`GPIO6–11`) with Flash via `GPIO16` (`CS#`) and `GPIO17` (`CLK`). Rev 1 silicon requires `CONFIG_SPIRAM_CACHE_WORKAROUND=y` (`[CPU-3.9]`). MicroSD must use 1-bit SDMMC (`GPIO2/14/15`) to avoid `GPIO4` flash LED strobing and `GPIO12` (`MTDI`) 1.8 V boot-strap hazards. `GPIO32` is an active-LOW power gate for the OV2640 2.8 V / 1.2 V LDOs.

---

## 3. Upstream FPV Projects: `jeanlemotan/esp32-cam-fpv` & `RomanLut/hx-esp32-cam-fpv`

- **[Verified upstream fact]** Both repositories are **MIT licensed**.
- **[Verified upstream fact]** `RomanLut/hx-esp32-cam-fpv` modifies `esp32-camera` (`driver/include/esp_camera.h` and `driver/cam_hal.c`) by adding `size_t (*data_available_callback)(void *cam_obj, const uint8_t *data, size_t count, bool last)` to `camera_config_t`. Inside `cam_task`, as DMA blocks arrive, the driver scans for `0xFFD8` (SOI) and streams chunks immediately to `data_available_callback` before EOI completes, reducing full-frame buffering latency and PSRAM pressure.
- **[Verified upstream fact]** Both projects transmit over raw 802.11 Wi-Fi packet injection (plus `APFPV` UDP mode in `hx-esp32-cam-fpv`) with Reed-Solomon FEC (`4/7` in `jeanlemotan`; default `k=6, n=12` or configurable `6/8`, `6/10` in `RomanLut`), dropping unrecoverable frames rather than stalling for TCP/ARQ retransmissions.
- **[Verified upstream fact]** Project-reported measurements (NOT verified by us on hardware): `hx-esp32-cam-fpv` reports 640×480 at 30 FPS (or 40 FPS with sensor overclocking) on original ESP32 + OV2640; `jeanlemotan/esp32-cam-fpv` reports up to 12 Mbps, 34–44 FPS around VGA/SVGA, and >90 FPS at ≤400×296.
- **[Requires hardware validation]** All FPS, latency, overclock stability, and Wi-Fi throughput figures on our target AI-Thinker ESP32-CAM hardware. Sensor overclocking is intentionally excluded from our default baseline.

---

## 4. Prior Art: Compressed-Domain JPEG / MJPEG Temporal Coding

Compressed-domain processing of JPEG/MJPEG quantized DCT coefficients is **well-established prior art** and is **not claimed as novel** here. Key literature includes:
- **[Verified upstream fact]** B. C. Smith & L. A. Rowe (1993, *IEEE CG&A*; 1996, *Real-Time Imaging*) and B. C. Smith (1994, *ACM MM*): algebraic operations (addition, subtraction, scaling, compositing, linear maps) directly on quantized JPEG/MJPEG DCT coefficients without full IDCT/RGB reconstruction.
- **[Verified upstream fact]** S.-F. Chang & D. G. Messerschmitt (1993, *IEEE ICASSP*; 1995, *IEEE JSAC*): compressed-domain DCT inverse motion compensation (MC-DCT) and inter/intra DCT transcoding.
- **[Verified upstream fact]** N. Merhav & V. Bhaskaran (1997, *IEEE TCSVT*), P. A. A. Assuncao & M. Ghanbari (1998, *IEEE TCSVT*), J. Song & B.-L. Yeo (2000, *IEEE TCSVT*): fast sparse-matrix MC-DCT and drift-controlled frequency-domain transcoding.
- **[Verified upstream fact]** U.-V. Koc & K. J. R. Liu (1998, *IEEE TIP* / *TCSVT*) and U.S. Patents 5,796,434 / 8,576,303 B2: DCT-domain motion estimation and background/foreground temporal differencing directly on 8×8 MJPEG DCT coefficients.

Accordingly, our compressed-domain temporal codec modes are documented and evaluated strictly as **experimental engineering prototypes** to determine empirically whether combining OV2640 hardware JPEG with lightweight ESP32 temporal coding improves the bitrate/quality/latency tradeoff over optimized MJPEG.

---

## 5. Key Hypotheses to Test

1. **[Hypothesis]** Generic lossless compression (e.g., `zlib`) applied directly to raw JPEG byte streams yields negligible or negative compression gain while adding CPU cost (control experiment).
2. **[Hypothesis]** On static, near-static, or smoothly stabilized scenes, quantized DCT-domain temporal coding (sparse changed-MCU signaling or DC/AC residual coding) can materially reduce transmitted bytes relative to independent JPEG frames, whereas on high-motion or noisy scenes it may offer little benefit or expand the stream (requiring automatic keyframe/pass-through fallback).
3. **[Hypothesis]** Mechanical/electronic stabilization (keeping scene blocks aligned across frames using BalaX IMU attitude telemetry) increases the fraction of unchanged 8×8 MCUs and reduces temporal delta size compared to unstabilized pitching video.
4. **[Hypothesis / Requires hardware validation]** On original ESP32 (240 MHz dual-core Xtensa LX6 with SPI PSRAM), lightweight MCU-level sparse/DC-AC temporal coding may fit inside the inter-frame budget at QVGA/CIF/VGA, or SPI PSRAM cache-miss stalls may make pass-through DMA streaming superior at higher resolutions.
