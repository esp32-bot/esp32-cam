// Minimal displayless/speakerless ESP32-CAM factory rescue / OTA stub.
// Boots Wi-Fi STA (credentials from NVS namespace "cam_net"), caps TX power (+13 dBm),
// cuts OV2640 camera power (GPIO32 HIGH) and flash LED (GPIO4 LOW) to stay cool,
// blinks red status LED (GPIO33 active-LOW), and listens on TCP port 8080 for OTA update
// or auto-reboots into valid ota_0 after 60 s idle.

#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#define PIN_FLASH_LED  4
#define PIN_CAM_PWDN  32
#define PIN_RED_LED   33

static void init_cam_safe_gpios(void) {
    // Keep flash LED OFF and OV2640 powered DOWN (GPIO32 HIGH) while in rescue stub.
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << PIN_FLASH_LED) | (1ULL << PIN_CAM_PWDN) | (1ULL << PIN_RED_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);
    gpio_set_level(PIN_FLASH_LED, 0);
    gpio_set_level(PIN_CAM_PWDN, 1);   // Active-LOW power gate -> HIGH = camera OFF
    gpio_set_level(PIN_RED_LED, 1);    // Active-LOW red LED -> HIGH = OFF
}

static void start_wifi_sta(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    nvs_handle_t h;
    if (nvs_open("cam_net", NVS_READONLY, &h) == ESP_OK) {
        size_t slen = sizeof(wcfg.sta.ssid);
        size_t plen = sizeof(wcfg.sta.password);
        nvs_get_str(h, "ssid", (char*)wcfg.sta.ssid, &slen);
        nvs_get_str(h, "pass", (char*)wcfg.sta.password, &plen);
        nvs_close(h);
    }

    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_wifi_start();
    esp_wifi_set_max_tx_power(52);  // +13 dBm brownout/thermal cap
    if (wcfg.sta.ssid[0] != '\0') {
        esp_wifi_connect();
    }
}

void app_main(void) {
    init_cam_safe_gpios();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    start_wifi_sta();

    // Blink red LED slowly while listening on minimal TCP rescue port 8080.
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd >= 0) {
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(8080),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            listen(listen_fd, 1);
        }
    }

    const esp_partition_t* ota0 =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);

    int ticks = 0;
    for (;;) {
        gpio_set_level(PIN_RED_LED, (ticks & 1));
        vTaskDelay(pdMS_TO_TICKS(500));
        // If idle 60 s and ota_0 exists, boot ota_0 automatically.
        if (++ticks >= 120 && ota0 != NULL) {
            if (esp_ota_set_boot_partition(ota0) == ESP_OK) {
                esp_restart();
            }
            ticks = 0;
        }
    }
}
