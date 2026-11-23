# Simulator & Emulation Assessment

## Summary Verdict

No local emulator on this host provides faithful OV2640 hardware JPEG timing, I²S0 DMA contention, SPI PSRAM cache-miss timing, or 2.4 GHz Wi-Fi throughput. Therefore:
1. **Host-native deterministic test & benchmark harness** is the primary correctness, fuzzing, protocol, and algorithmic-complexity verification layer.
2. **Real ESP-IDF v6.1.1 Xtensa cross-compilation** verifies ESP32 compilation, link-time memory/IRAM/DRAM/PSRAM sizing, and `esp32-camera` API integration.
3. **Real AI-Thinker ESP32-CAM hardware** is mandatory for all FPS, DMA overflow, PSRAM latency, Wi-Fi throughput, and OV2640 crop-register timing claims.

---

## 1. Local Availability Check (`Observed locally`)

| Tool | Local Status | Notes |
|---|---|---|
| Espressif `qemu-system-xtensa` | Not installed | Not present in `PATH` or `~/.espressif/tools/` |
| `qemu-system-x86_64` | Not installed | — |
| Velxio | Not installed | Inspected upstream GitHub repository (`davidmonterocrespo24/velxio`) |
| Wokwi CLI | Not installed | — |
| Docker / Podman | Not installed | — |
| ESP-IDF `v6.1.1` + `xtensa-esp32-elf-g++` | **Installed** | Used to cross-compile ESP32 firmware & codec |

---

## 2. Velxio Inspection (`Verified upstream fact`)

Inspected `davidmonterocrespo24/velxio` (`master` branch), specifically `docs/ESP32_EMULATION.md`, `docs/BUILD-QEMU.md`, `.gitmodules`, and release notes:
- Velxio emulates ESP32-CAM using `libqemu-xtensa` built on `lcgamboa/qemu` (`picsimlab-esp32` branch, QEMU 9.2 base) with Velxio-added devices: `hw/i2c/esp32_ov2640.c`, `hw/misc/esp32_i2s_cam.c`, and `hw/misc/velxio_camera_export.c`.
- Those QEMU device patches are licensed **GPL-2.0** and distributed as source tarballs gated behind `velxio.dev/api/pro/license/downloads/qemu-source-...` rather than committed in the public `velxio` repository.
- Functional behavior (per Velxio docs/examples): responds to SCCB chip-ID probing (`0x2642`) and bridges host browser webcam / synthetic frames into the emulated I²S camera path so `esp_camera_init()` and `esp_camera_fb_get()` work functionally.
- **What Velxio does NOT establish:**
  - It injects host-encoded JPEG/frame data rather than running a cycle-accurate OV2640 CMOS readout + hardware DSP JPEG engine.
  - It does not establish faithful OV2640 DSP crop-register (`XOFFL`/`YOFFL`/`VHYX`) mid-frame latching/tearing behavior or `QS` register quantization fidelity.
  - It does not model ESP32 SPI Flash/PSRAM bus contention or real 802.11 RF packet loss/throughput.
- **Licensing note:** Because QEMU and Velxio's QEMU additions are GPL-2.0, zero code from QEMU or Velxio is copied into `esp32-cam-livestream`.

---

## 3. Espressif QEMU & Wokwi (`Verified upstream fact`)

- **Espressif QEMU (`espressif/qemu`)**: Emulates dual-core Xtensa ESP32 CPU, UART, SPI Flash, basic PSRAM, and virtual Ethernet (`open_eth`), useful for protocol/crash/allocation smoke tests when installed, but lacks an OV2640 parallel I²S0 camera model and real Wi-Fi PHY timing.
- **Wokwi**: Supports custom chip C/WASM models (`wokwi-chip`) for I²C/SPI/GPIO peripherals, suitable for functional UART/SCCB register-write verification, but not cycle-accurate OV2640 DVP/DMA/PSRAM/Wi-Fi benchmarking.

---

## 4. Mandatory Host Harness Capabilities (`Measured in host tests`)

To maximize pre-hardware confidence, our native C++17 host harness tests:
1. Byte-identical zero-loss replay of real baseline JPEGs (`libjpeg-turbo` generated 9-scene corpus).
2. Deterministic packet impairment: random loss, Gilbert-Elliott burst loss, reordering, duplication, delay, MTU enforcement, and truncation.
3. Sequence number 32-bit wrap-around (`0xFFFFFFFD → 0x00000003`).
4. JPEG parser fuzzing and mutation testing (zero buffer overflows, zero unbounded allocations, zero infinite loops).
5. Full temporal codec benchmark matrix (bytes, ratio, encode/decode CPU µs, PSNR, SSIM, changed MCUs, bounded memory/allocs).
6. Synthetic BalaC IMU pitch/roll trajectories, UART packet framing/CRC/corruption, fresh-state overwrite, predictor error, receiver warp quality, and stabilization-vs-temporal-delta ablation.
