#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -z "${IDF_PATH:-}" ]]; then
  if [[ -f "${HOME}/esp/v6.1.1/esp-idf/export.sh" ]]; then
    # shellcheck disable=SC1091
    source "${HOME}/esp/v6.1.1/esp-idf/export.sh" >/dev/null 2>&1
  else
    echo "ERROR: IDF_PATH is not set. Source your ESP-IDF export.sh first." >&2
    exit 1
  fi
fi

build_one() {
  local name="$1"
  local dir="$2"
  local bin="$3"
  mkdir -p "${dir}/build"
  echo "[build_firmware] Building ${name}..."
  if idf.py -C "${dir}" build > "${dir}/build/idf-build.log" 2>&1; then
    local size
    size=$(wc -c < "${dir}/build/${bin}" | tr -d ' ')
    echo "[build_firmware] OK ${name}: ${dir}/build/${bin} (${size} bytes)"
  else
    echo "[build_firmware] FAILED ${name} (tail of ${dir}/build/idf-build.log):" >&2
    tail -n 40 "${dir}/build/idf-build.log" >&2
    exit 1
  fi
}

build_one "ota_stub (factory @ 0x20000, max 0x90000)" \
  "${ROOT_DIR}/firmware/ota_stub" "esp32cam_ota_stub.bin"

build_one "esp32cam (ota_0 @ 0xB0000, max 0x330000)" \
  "${ROOT_DIR}/firmware/esp32cam" "esp32_cam_fw.bin"
