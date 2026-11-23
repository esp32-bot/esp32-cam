# Engineering Journal — `esp32-cam-livestream`

## Entry 1 — Milestone A: Discovery & Upstream Source Verification

### Machine & Local Environment Inspected
- Apple M5 (`arm64`), 10 cores, 16 GiB RAM, macOS 27.0, Apple Clang 21.0.0, CMake 4.4.3, Ninja 1.13.2.
- ESP-IDF `v6.1.1` installed locally with `xtensa-esp32-elf-g++` (`esp-15.2.0_20251204`).
- Host C/C++ dependencies installed: `jpeg-turbo 3.2.1`, `zlib`, `lz4 1.11.1`, `zstd 1.5.7`.
- No QEMU, Wokwi CLI, Docker/Podman, PlatformIO, or Arduino CLI installed locally.
- Mapped all 4 sibling `esp32-bot` repositories (`balax`, `balax-remote`, `joyx`, `tools`); all have active uncommitted work and were preserved untouched. Confirmed neither `esp32-cam-livestream` nor `balax-cam` existed locally or remotely.
- Reviewed BalaX silicon ground-truth docs (`M5STICKC_PLUS.md`, `ESP32_CAM.md`, `OV2640.md`) and `balax/main/imu_fusion.h` coordinate conventions (`+X` left, `+Y` up, `+Z` forward; `+pitch` leaning back, `+roll` leaning right).

### Upstream Source Verification
1. **`espressif/esp32-camera`** (`sensors/ov2640.c`, `driver/cam_hal.c`):
   - Confirmed `set_window` / `set_res_raw` writes DSP registers `HSIZE`, `VSIZE`, `XOFFL`, `YOFFL`, `VHYX`, `TEST`, `ZMOW`, `ZMOH`, `ZMHH` — **critical finding**: `set_window()` ALSO enters `R_BYPASS_DSP_BYPAS`, rewrites the full mode table, delays 10 ms, and calls `set_pixformat()`. Thus per-frame crop stabilization must NOT call `set_res_raw()` directly; an experimental path should test lightweight `{BANK_DSP, XOFFL, YOFFL, VHYX}` updates only (and validate on hardware for tearing).
   - Confirmed mixed Apache-2.0 (driver/HAL) + MIT (OpenMV-derived sensors/SCCB) licensing.
2. **`RomanLut/hx-esp32-cam-fpv` & `jeanlemotan/esp32-cam-fpv`** (both MIT):
   - Verified `hx-esp32-cam-fpv` adds `data_available_callback` to `camera_config_t` and streams DMA chunks from `cam_hal.c` upon detecting JPEG SOI (`0xFFD8`), plus uses raw 802.11 / APFPV UDP + Reed-Solomon FEC (`6/12` default).
3. **`davidmonterocrespo24/velxio`**:
   - Verified `docs/BUILD-QEMU.md`: ESP32-CAM QEMU devices (`esp32_ov2640.c`, `esp32_i2s_cam.c`, `velxio_camera_export.c`) are GPL-2.0 patches on `lcgamboa/qemu` distributed as gated tarballs and bridge host webcam/image frames rather than simulating cycle-accurate OV2640 JPEG hardware or PSRAM/Wi-Fi timing.
4. **Compressed-domain JPEG prior art**:
   - Documented Smith & Rowe (1993/1994/1996), Chang & Messerschmitt (1993/1995), Merhav & Bhaskaran (1997), Assuncao & Ghanbari (1998), Koc & Liu (1998), Song & Yeo (2000), and related patents. Our temporal DCT modes are explicitly framed as experimental prototypes, not novel inventions.

### Architectural Decisions Made
- Build two clean repositories: `esp32-cam-livestream` (standalone video engine) and `balax-cam` (robot consumer depending on `esp32-cam-livestream`).
- Keep `codec/` and `transport/` strictly platform-independent C++17 (no STL heap churn in hot paths, bounded buffers) so the exact same `.cpp`/`.hpp` files compile on Apple M5 host and ESP-IDF Xtensa `esp32`.
- Keep `CODEC_PASS_THROUGH` as the permanent zero-overhead fallback and make all temporal modes and sensor-crop stabilization opt-in feature flags.
