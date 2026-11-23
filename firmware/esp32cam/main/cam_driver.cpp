#include "cam_driver.hpp"

#include "driver/gpio.h"
#include "esp_log.h"

namespace ecl_fw {
namespace {
const char* kTag = "ecl_cam";
}

void set_status_led(bool on) {
    // GPIO33 red status LED is active-LOW on AI-Thinker ESP32-CAM.
    gpio_set_level(static_cast<gpio_num_t>(AiThinkerPins::kRedLed), on ? 0 : 1);
}

esp_err_t init_ai_thinker_camera(const CamInitParams& params) {
    // Ensure blinding white Flash LED (GPIO4) stays strictly OFF and init red status LED.
    gpio_config_t led_cfg{};
    led_cfg.pin_bit_mask = (1ULL << AiThinkerPins::kFlashLed) | (1ULL << AiThinkerPins::kRedLed);
    led_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&led_cfg);
    gpio_set_level(static_cast<gpio_num_t>(AiThinkerPins::kFlashLed), 0);
    set_status_led(false);

    camera_config_t cfg{};
    cfg.pin_pwdn = AiThinkerPins::kPwdn;
    cfg.pin_reset = AiThinkerPins::kReset;
    cfg.pin_xclk = AiThinkerPins::kXclk;
    cfg.pin_sccb_sda = AiThinkerPins::kSiod;
    cfg.pin_sccb_scl = AiThinkerPins::kSioc;
    cfg.pin_d7 = AiThinkerPins::kY9;
    cfg.pin_d6 = AiThinkerPins::kY8;
    cfg.pin_d5 = AiThinkerPins::kY7;
    cfg.pin_d4 = AiThinkerPins::kY6;
    cfg.pin_d3 = AiThinkerPins::kY5;
    cfg.pin_d2 = AiThinkerPins::kY4;
    cfg.pin_d1 = AiThinkerPins::kY3;
    cfg.pin_d0 = AiThinkerPins::kY2;
    cfg.pin_vsync = AiThinkerPins::kVsync;
    cfg.pin_href = AiThinkerPins::kHref;
    cfg.pin_pclk = AiThinkerPins::kPclk;

    cfg.xclk_freq_hz = params.xclk_hz;
    cfg.ledc_timer = LEDC_TIMER_0;
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size = params.frame_size;
    cfg.jpeg_quality = params.jpeg_quality;
    cfg.fb_count = params.fb_count;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode = (params.fb_count > 1) ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;

    const esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        // Robotics-friendly ISP defaults (OV2640.md): enable BPC/WPC/LENC, keep AEC2 off.
        s->set_bpc(s, 1);
        s->set_wpc(s, 1);
        s->set_lenc(s, 1);
        s->set_aec2(s, 0);
        ESP_LOGI(kTag, "OV2640 ready: PID=0x%04X framesize=%d QS=%d XCLK=%d Hz",
                 s->id.PID, params.frame_size, params.jpeg_quality, params.xclk_hz);
    }
    return ESP_OK;
}

}  // namespace ecl_fw
