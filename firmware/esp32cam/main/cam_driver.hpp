#pragma once

#include <cstdint>
#include "esp_camera.h"
#include "esp_err.h"

namespace ecl_fw {

// AI-Thinker ESP32-CAM (Rev 1.6) verified pin map (docs/silicon/ESP32_CAM.md).
// GPIO16/17 are PSRAM CS#/CLK and MUST NEVER be touched.
// GPIO4 is the high-power white Flash LED (drive LOW at boot).
// GPIO32 is the active-LOW P-MOSFET gate for OV2640 2.8V/1.2V LDOs.
struct AiThinkerPins {
    static constexpr int kPwdn = 32;
    static constexpr int kReset = -1;
    static constexpr int kXclk = 0;
    static constexpr int kSiod = 26;
    static constexpr int kSioc = 27;
    static constexpr int kY9 = 35;
    static constexpr int kY8 = 34;
    static constexpr int kY7 = 39;
    static constexpr int kY6 = 36;
    static constexpr int kY5 = 21;
    static constexpr int kY4 = 19;
    static constexpr int kY3 = 18;
    static constexpr int kY2 = 5;
    static constexpr int kVsync = 25;
    static constexpr int kHref = 23;
    static constexpr int kPclk = 22;
    static constexpr int kFlashLed = 4;
    static constexpr int kRedLed = 33;
};

struct CamInitParams {
    framesize_t frame_size = FRAMESIZE_QVGA;
    int jpeg_quality = 12;
    int xclk_hz = 16000000;
    uint8_t fb_count = 2;
};

esp_err_t init_ai_thinker_camera(const CamInitParams& params);
void set_status_led(bool on);

}  // namespace ecl_fw
