# esp32-cam-livestream

Reusable, low-latency, bandwidth-efficient video livestream engine targeting the **AI-Thinker ESP32-CAM (ESP32-S + OV2640)** while keeping its core protocol, packetizer, bounded JPEG DCT parser, and experimental compressed-domain temporal codec **100% platform-independent** so the exact same C++17 source compiles and runs on both desktop hosts and ESP-IDF Xtensa ESP32.

> **Status & Honesty Note:** Host unit tests, 5,000-input parser fuzzing, synthetic benchmark suites, network-impairment replay, and ESP-IDF v6.1.1 Xtensa cross-compilation are all verified locally. **No real-hardware FPS, radio throughput, CPU load, thermal stability, or OV2640 register behavior is claimed here** — every hardware metric is explicitly deferred to [docs/hardware-validation.md](docs/hardware-validation.md).

## Architecture

| Layer | Location | Target | Role |
|---|---|---|---|
| **Protocol & Packetizer (`ECL1`)** | `transport/` | Host + ESP-IDF | Compact 32 B header + payload + 4 B IEEE CRC-32 trailer, zero-alloc sender packetizer, wrap-safe out-of-order bounded-slot receiver depacketizer, deterministic network-impairment simulator, POSIX UDP transport |
| **JPEG DCT Parser & Temporal Codec** | `codec/` | Host + ESP-IDF | Bounded zero-alloc baseline JPEG parser (SOI/EOI/DQT/DHT/Annex K fallback/DRI+RST/SOF0/SOS, 4:2:0 + 4:2:2 + 4:4:4 → quantized zigzag DCT blocks), standard JPEG writer, and closed-loop drift-free experimental temporal codec (`PASS_THROUGH` permanent fallback + `FRAME_SKIP`, `DCT_DC_RESIDUAL`, `DCT_LOW_AC_RESIDUAL`, `DCT_FULL_RESIDUAL`, `DCT_SPARSE_BLOCK`) |
| **Host Tooling & Benchmarks** | `host/` | Desktop (macOS/Linux) | Deterministic fixture generator (`libjpeg-turbo`), loss/burst/reorder replay CLI, compression & PSNR/SSIM benchmark harness, live UDP receiver CLI |
| **ESP32-CAM Main Firmware (`ota_0`)** | `firmware/esp32cam/` | ESP-IDF v6.1.1 (`esp32`) | AI-Thinker pin-safe OV2640 hardware-JPEG capture (`GPIO4` LOW, `GPIO32` active-LOW camera gate, `GPIO16/17` PSRAM untouched), dual-core capture/TX pipeline, `ECL1` UDP streaming, runtime codec-mode switch with `CODEC_PASS_THROUGH` default & compile-time `CONFIG_ECL_ENABLE_DCT_CODEC` opt-out |
| **Tiny Wi-Fi OTA Rescue Stub (`factory`)** | `firmware/ota_stub/` | ESP-IDF v6.1.1 (`esp32`) | Displayless/speakerless pure-C Wi-Fi HTTP OTA rescue stub (`549,728 B`, fits inside `0x90000` = 576 KB with 39 KB free) |

## Compact Tiny+Big Partition Layout (`4 MB` Flash)

Defined in [`firmware/partitions_cam_compact.csv`](firmware/partitions_cam_compact.csv) — modeled after `balax/partitions_compact.csv` and tightened by **64 KB** in `factory` because ESP32-CAM has no display, speaker, or PMIC:

| Partition | Type / SubType | Offset | Size | Purpose |
|---|---|---|---|---|
| `nvs` | `data/nvs` | `0x9000` | `24 KB` (`0x6000`) | Wi-Fi & runtime config |
| `otadata` | `data/ota` | `0xf000` | `8 KB` (`0x2000`) | Active boot slot selector (`factory` vs `ota_0`) |
| `phy_init` | `data/phy` | `0x11000` | `4 KB` (`0x1000`) | RF calibration |
| `factory` | `app/factory` | `0x20000` | **`576 KB` (`0x90000`)** | Tiny Wi-Fi OTA rescue stub (`esp32cam_ota_stub.bin` = 537 KB) |
| `ota_0` | `app/ota_0` | `0xB0000` | **`3.19 MB` (`0x330000`)** | Main livestream or `balax-cam` firmware (`esp32_cam_livestream_fw.bin` = 870 KB) |
| `telemetry` | `data/0x99` | `0x3E0000` | `64 KB` (`0x10000`) | Local diagnostics ring |
| `coredump` | `data/coredump` | `0x3F0000` | `64 KB` (`0x10000`) | Crash coredump storage |

### Verified Xtensa Build Footprints (ESP-IDF v6.1.1)

- **`esp32cam_ota_stub.bin` (`factory`)**: `549,728 B` (`536.8 KB`, `0x86360`) — `39.2 KB` (`6.8%`) free in `0x90000` (`576 KB`), zero partition warnings.
- **`esp32_cam_livestream_fw.bin` (`ota_0`)**: `890,864 B` (`870.0 KB`, `0xD97F0`) — `73%` free in `0x330000` (`3.19 MB`).
  - Static DRAM: `58,623 B` (`32.4%` of `180,736 B`, `122,113 B` internal DRAM free + 4 MB PSRAM).
  - Static IRAM: `98,495 B` (`75.2%` of `131,072 B`, `32,577 B` free).
  - Shared engine size inside ESP32 image: `libcodec.a = 8,390 B` Flash (`0 B` static DRAM/IRAM), `libtransport.a = 1,004 B` Flash (`0 B` static DRAM/IRAM).

## Quickstart

### 1. Build & Run All Host Tests, Fuzz Smoke Test, Fixtures, and Benchmarks

Requires CMake, Ninja/Make, C++17 compiler, and `libjpeg-turbo` (plus `zlib` for the negative-control benchmark):

```sh
./scripts/run_all_tests.sh
```

Individual tools after building (`cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`):

```sh
ctest --test-dir build --output-on-failure
./build/ecl_generate_fixtures --out build/fixtures --frames 30 --width 320 --height 240 --quality 75
./build/ecl_benchmark --corpus build/fixtures/manifest.json --out build/benchmark_report.md
./build/ecl_replay --corpus build/fixtures/manifest.json --scene small_pitch --mode dct_sparse_t2 --loss 0.05 --kf 10
./build/ecl_receiver --port 5000 --dump-dir build/rx_frames
```

### 2. Cross-Compile Both ESP32-CAM Firmware Images

```sh
source ~/esp/v6.1.1/esp-idf/export.sh
./scripts/build_firmware.sh
```

Initial USB provisioning (flashes bootloader, partition table, `ota_data_initial`, `factory` stub at `0x20000`, and main app at `0xB0000`):

```sh
# Replace /dev/cu.usbserial-* with your USB-UART adapter; hold GPIO0 LOW while resetting.
esptool.py --chip esp32 -p /dev/cu.usbserial-XXXX -b 460800 \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x1000  firmware/esp32cam/build/bootloader/bootloader.bin \
  0x8000  firmware/esp32cam/build/partition_table/partition-table.bin \
  0xf000  firmware/esp32cam/build/ota_data_initial.bin \
  0x20000 firmware/ota_stub/build/esp32cam_ota_stub.bin \
  0xB0000 firmware/esp32cam/build/esp32_cam_livestream_fw.bin
```

Subsequent updates can be pushed over Wi-Fi directly into `ota_0` via the rescue stub (`POST http://esp32cam.local/ota`).

## Documentation

- [`docs/research.md`](docs/research.md) — Verified upstream facts, local observations, compressed-domain JPEG prior art, and explicit hardware hypotheses.
- [`docs/architecture.md`](docs/architecture.md) — Layer boundaries, API contracts, RAM/PSRAM/Flash budgets, and incremental DMA-to-DCT parser analysis.
- [`docs/protocol.md`](docs/protocol.md) — `ECL1` wire header, payload modes, fragmentation, CRC-32, and loss-recovery semantics.
- [`docs/benchmarks.md`](docs/benchmarks.md) — Reproducible host benchmark results across 9 synthetic robotics scenes and packet-loss impairment scenarios.
- [`docs/simulator-notes.md`](docs/simulator-notes.md) — Audited QEMU / Velxio / Wokwi capabilities and why simulator timing is not hardware truth.
- [`docs/hardware-validation.md`](docs/hardware-validation.md) — Pre-flight electrical safety, UART wiring, and step-by-step hardware validation matrix (`HW-01`–`HW-08`, `BC-01`–`BC-05`).
- [`docs/journal.md`](docs/journal.md) — Engineering decision log and bug/fix history.

## Consumer Repositories

- `esp32-bot/balax-cam` — BalaC / M5StickC Plus attitude-coupled robot camera consumer that links `esp32-cam-livestream/codec` and `esp32-cam-livestream/transport` directly without duplicating internals.

## License

License TBD pending maintainer decision across the `esp32-bot` organization (clean-room implementation; no GPL code copied).
