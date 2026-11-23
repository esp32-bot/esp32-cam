#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release > build/host-build.log 2>&1
cmake --build build >> build/host-build.log 2>&1
ctest --test-dir build --output-on-failure

./build/ecl_generate_fixtures \
  --out testdata/generated \
  --manifest testdata/manifests/default_corpus.json \
  --width 320 --height 240 --frames 30 --quality 75

./build/ecl_replay --in testdata/generated/small_pitch --mode PASS_THROUGH --loss 0.0
./build/ecl_benchmark --corpus testdata/generated --kf 15 --json build/bench_results.json
