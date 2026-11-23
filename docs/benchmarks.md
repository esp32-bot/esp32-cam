# Host Benchmarks & Empirical Analysis

> **Important Distinction:** Every measurement below is **`[Measured in host tests]`** on an Apple M5 (`arm64`, 10 cores, 16 GiB RAM, macOS 27.0, Apple Clang 21.0.0, `-O3` Release). These numbers measure **algorithmic compression ratio, reconstruction quality, loss-recovery behavior, and host complexity** — they are **NOT** ESP32 hardware FPS or Wi-Fi throughput measurements.

## Reproducing These Benchmarks

```sh
./scripts/run_all_tests.sh
```

---

## 1. 9-Scene × 7-Mode Codec Matrix (`320×240`, Q=75, 30 frames/scene, KF=15)

- **Equivalence classes:**
  - `byte`: exact byte-for-byte match against source JPEG (guaranteed for `PASS_THROUGH`, `FRAME_SKIP`, `GENERIC_ZLIB`; for `DCT_FULL_EXACT`, **decoded-pixel equivalence** `∞ dB PSNR / SSIM 1.0000` is guaranteed for any baseline JPEG, and byte-exactness occurred here only because both source and writer used Annex K tables).
  - `lossy`: closed-loop quantized DCT approximation (`DCT_DC_RES`, `DCT_LOW_AC15`, `DCT_SPARSE_T2` with coefficient deadzone `T = 2`).
- **`KF(fb)`**: total keyframes emitted (`fb` = adaptive fallbacks where delta exceeded raw JPEG size).

| Scene | Mode | In B/f | Out B/f | Ratio | Enc µs | Dec µs | Chg% | PSNR dB | SSIM | KF(fb) | Equiv |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `static_scene` | `PASS_THROUGH` | 9311 | 9311 | 1.000 | 0 | 0 | 0.0% | 99.99 | 1.0000 | 30(0) | byte |
| `static_scene` | `FRAME_SKIP` | 9311 | 624 | 0.067 | 18 | 0 | 0.0% | 99.99 | 1.0000 | 2(0) | byte |
| `static_scene` | `GENERIC_ZLIB` | 9311 | 8087 | 0.869 | 133 | 31 | 0.0% | 99.99 | 1.0000 | 30(0) | byte |
| `static_scene` | `DCT_FULL_EXACT` | 9311 | 671 | 0.072 | 132 | 141 | 6.7% | 99.99 | 1.0000 | 2(0) | pixel |
| `repeated_frames` | `PASS_THROUGH` | 9205 | 9205 | 1.000 | 0 | 0 | 0.0% | 99.99 | 1.0000 | 30(0) | byte |
| `repeated_frames` | `FRAME_SKIP` | 9205 | 1844 | 0.200 | 20 | 0 | 0.0% | 99.99 | 1.0000 | 6(0) | byte |
| `repeated_frames` | `DCT_FULL_EXACT` | 9205 | 1884 | 0.205 | 204 | 174 | 20.0% | 99.99 | 1.0000 | 6(5) | pixel |
| `repeated_frames` | `DCT_SPARSE_T2` | 9205 | 1609 | 0.175 | 188 | 216 | 11.3% | 53.63 | 0.9367 | 2(0) | lossy |
| `sensor_noise` | `PASS_THROUGH` | 11503 | 11503 | 1.000 | 0 | 0 | 0.0% | 99.99 | 1.0000 | 30(0) | byte |
| `sensor_noise` | `FRAME_SKIP` | 11503 | 11503 | 1.000 | 41 | 0 | 0.0% | 99.99 | 1.0000 | 30(0) | byte |
| `sensor_noise` | `GENERIC_ZLIB` | 11503 | 11223 | 0.976 | 214 | 41 | 0.0% | 99.99 | 1.0000 | 30(0) | byte |
| `sensor_noise` | `DCT_DC_RES` | 11503 | 1207 | 0.105 | 174 | 312 | 35.0% | 39.24 | 0.9023 | 2(0) | lossy |
| `sensor_noise` | `DCT_LOW_AC15` | 11503 | 6392 | 0.556 | 518 | 496 | 69.2% | 41.09 | 0.9646 | 2(0) | lossy |
| `sensor_noise` | `DCT_FULL_EXACT` | 11503 | 8478 | 0.737 | 698 | 561 | 69.2% | 99.99 | 1.0000 | 2(0) | pixel |
| `sensor_noise` | `DCT_SPARSE_T2` | 11503 | 905 | 0.079 | 220 | 305 | 8.7% | 39.16 | 0.9036 | 2(0) | lossy |
| `brightness_change` | `PASS_THROUGH` | 9286 | 9286 | 1.000 | 0 | 0 | 0.0% | 99.99 | 1.0000 | 30(0) | byte |
| `brightness_change` | `DCT_DC_RES` | 9286 | 1549 | 0.167 | 126 | 217 | 72.7% | 44.01 | 0.9929 | 2(0) | lossy |
| `brightness_change` | `DCT_LOW_AC15` | 9286 | 1983 | 0.214 | 176 | 240 | 73.7% | 46.55 | 0.9958 | 2(0) | lossy |
| `brightness_change` | `DCT_FULL_EXACT` | 9286 | 2074 | 0.223 | 264 | 240 | 73.7% | 99.99 | 1.0000 | 2(0) | pixel |
| `brightness_change` | `DCT_SPARSE_T2` | 9286 | 1389 | 0.150 | 222 | 215 | 41.4% | 43.38 | 0.9932 | 2(0) | lossy |
| `small_pitch` | `PASS_THROUGH` | 9285 | 9285 | 1.000 | 0 | 0 | 0.0% | 99.99 | 1.0000 | 30(0) | byte |
| `small_pitch` | `GENERIC_ZLIB` | 9285 | 8187 | 0.882 | 182 | 33 | 0.0% | 99.99 | 1.0000 | 30(0) | byte |
| `small_pitch` | `DCT_DC_RES` | 9285 | 1200 | 0.129 | 135 | 228 | 38.8% | 25.26 | 0.6721 | 2(0) | lossy |
| `small_pitch` | `DCT_LOW_AC15` | 9285 | 5421 | 0.584 | 311 | 375 | 43.2% | 31.93 | 0.9068 | 2(0) | lossy |
| `small_pitch` | `DCT_FULL_EXACT` | 9285 | 8105 | 0.873 | 493 | 317 | 62.5% | 99.99 | 1.0000 | 13(12) | pixel |
| `small_pitch` | `DCT_SPARSE_T2` | 9285 | 4774 | 0.514 | 352 | 346 | 30.7% | 35.13 | 0.9204 | 2(0) | lossy |
| `smooth_pan` | `PASS_THROUGH` | 9336 | 9336 | 1.000 | 0 | 0 | 0.0% | 99.99 | 1.0000 | 30(0) | byte |
| `smooth_pan` | `DCT_DC_RES` | 9336 | 1138 | 0.122 | 142 | 237 | 33.8% | 24.22 | 0.5968 | 2(0) | lossy |
| `smooth_pan` | `DCT_LOW_AC15` | 9336 | 5501 | 0.589 | 311 | 369 | 37.6% | 31.84 | 0.8998 | 2(0) | lossy |
| `smooth_pan` | `DCT_FULL_EXACT` | 9336 | 8421 | 0.902 | 510 | 464 | 37.6% | 99.99 | 1.0000 | 2(0) | pixel |
| `smooth_pan` | `DCT_SPARSE_T2` | 9336 | 4724 | 0.506 | 352 | 349 | 34.1% | 35.04 | 0.9076 | 2(0) | lossy |
| `large_motion` | `PASS_THROUGH` | 9056 | 9056 | 1.000 | 0 | 0 | 0.0% | 99.99 | 1.0000 | 30(0) | byte |
| `large_motion` | `DCT_DC_RES` | 9056 | 1779 | 0.196 | 146 | 244 | 71.1% | 26.40 | 0.5597 | 2(0) | lossy |
| `large_motion` | `DCT_LOW_AC15` | 9056 | 8313 | 0.918 | 366 | 373 | 79.2% | 44.76 | 0.8794 | 7(5) | lossy |
| `large_motion` | `DCT_FULL_EXACT` | 9056 | 9056 | 1.000 | 510 | 104 | 100.0% | 99.99 | 1.0000 | 30(29) | pixel |
| `large_motion` | `DCT_SPARSE_T2` | 9056 | 7162 | 0.791 | 426 | 405 | 50.1% | 34.98 | 0.8890 | 2(0) | lossy |
| `high_texture` | `PASS_THROUGH` | 21279 | 21279 | 1.000 | 0 | 0 | 0.0% | 99.99 | 1.0000 | 30(0) | byte |
| `high_texture` | `GENERIC_ZLIB` | 21279 | 20645 | 0.970 | 484 | 77 | 0.0% | 99.99 | 1.0000 | 30(0) | byte |
| `high_texture` | `DCT_FULL_EXACT` | 21279 | 5681 | 0.267 | 603 | 608 | 73.2% | 99.99 | 1.0000 | 2(0) | pixel |
| `high_texture` | `DCT_SPARSE_T2` | 21279 | 1469 | 0.069 | 315 | 420 | 6.7% | 42.00 | 0.9931 | 2(0) | lossy |
| `scene_cut` | `PASS_THROUGH` | 9483 | 9483 | 1.000 | 0 | 0 | 0.0% | 99.99 | 1.0000 | 30(0) | byte |
| `scene_cut` | `DCT_FULL_EXACT` | 9483 | 5136 | 0.542 | 395 | 324 | 35.0% | 99.99 | 1.0000 | 3(2) | pixel |
| `scene_cut` | `DCT_SPARSE_T2` | 9483 | 2549 | 0.269 | 268 | 246 | 27.8% | 38.28 | 0.9364 | 3(2) | lossy |

---

## 2. Packet-Loss Resilience (`small_pitch`, 30 frames, `KF = 10`, no FEC)

| Mode | Loss Rate | Wire Bytes | Decoded / 30 | Expired | Missing-Ref Drops | Avg PSNR dB | Avg SSIM |
|---|---:|---:|---:|---:|---:|---:|---:|
| `PASS_THROUGH` | 0% | 286,189 | 30 / 30 | 0 | 0 | 99.99 | 1.0000 |
| `PASS_THROUGH` | 2% | 286,189 | 28 / 30 | 2 | 0 | 99.99 | 1.0000 |
| `PASS_THROUGH` | 5% | 286,189 | 21 / 30 | 9 | 0 | 99.99 | 1.0000 |
| `DCT_FULL_EXACT` | 0% | 249,995 | 30 / 30 | 0 | 0 | 99.99 | 1.0000 |
| `DCT_FULL_EXACT` | 2% | 249,995 | 22 / 30 | 2 | 6 | 99.99 | 1.0000 |
| `DCT_FULL_EXACT` | 5% | 249,995 | 17 / 30 | 8 | 5 | 99.99 | 1.0000 |
| `DCT_SPARSE_T2` | 0% | 150,238 | 30 / 30 | 0 | 0 | 37.35 | 0.9160 |
| `DCT_SPARSE_T2` | 2% | 150,238 | 23 / 30 | 1 | 6 | 39.49 | 0.9224 |
| `DCT_SPARSE_T2` | 5% | 150,238 | 15 / 30 | 4 | 11 | 39.58 | 0.9253 |

---

## 3. Honest Takeaways

1. **Negative control (`GENERIC_ZLIB`) confirmed:** On realistic noisy or high-texture JPEGs (`sensor_noise`, `high_texture`), generic lossless compression saves only **2.4%–3.0%** while burning significant CPU.
2. **Exact lossless DCT coding (`DCT_FULL_EXACT`) is scene-dependent:** It gives **3.7×–13.9× reduction** on static, brightness-changing, or mildly noisy high-texture scenes, but **only 10%–13% reduction** on unstabilized pitching/panning scenes (`small_pitch`, `smooth_pan`) and **0% (pure fallback)** on `large_motion`.
3. **Perceptual deadzone (`DCT_SPARSE_T2`) is the standout experimental mode:** Filtering `±1..2` quantization jitter cuts `sensor_noise` by **12.7×** (`39.16 dB`), `high_texture` by **14.5×** (`42.00 dB`), and `small_pitch` / `smooth_pan` by **~2×** (`35.1 dB / 0.92 SSIM`).
4. **DC-only (`DCT_DC_RES`) is unsuitable for camera motion:** Despite tiny size, frozen high-frequency AC edges produce severe ghosting (`24–25 dB`, `0.60–0.67 SSIM`) under pitch/pan.
5. **Packet-loss tradeoff is real:** Without FEC or keyframe-request feedback, temporal dependencies drop more frames than independent MJPEG under 5% packet loss (`15/30` vs `21/30`), even though zero frames are corrupted and recovery is strictly bounded by `KF`.
