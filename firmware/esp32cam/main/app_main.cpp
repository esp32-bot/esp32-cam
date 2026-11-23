#include <cstdio>
#include <cstring>

#include "cam_driver.hpp"
#include "cam_metrics.hpp"
#include "ecl/packetizer.hpp"
#include "ecl/temporal_codec.hpp"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

const char* kTag = "ecl_app";
ecl_fw::StreamMetrics g_metrics{};
SemaphoreHandle_t g_metrics_mu = nullptr;

// Dedicated UDP sender context (zero heap alloc in hot loop).
struct UdpTxCtx {
    int sock = -1;
    sockaddr_in dest{};
};

bool udp_emit_cb(void* user_ctx, const uint8_t* wire_data, size_t wire_len) {
    auto* u = static_cast<UdpTxCtx*>(user_ctx);
    if (!u || u->sock < 0) return false;
    const ssize_t n = sendto(u->sock, wire_data, wire_len, 0,
                             reinterpret_cast<const sockaddr*>(&u->dest), sizeof(u->dest));
    return n == static_cast<ssize_t>(wire_len);
}

ecl::CodecMode configured_codec_mode() {
#if defined(CONFIG_ECL_CODEC_FRAME_SKIP)
    return ecl::CodecMode::FRAME_SKIP;
#elif defined(CONFIG_ECL_CODEC_DCT_DC)
    return ecl::CodecMode::DCT_DC_RESIDUAL;
#elif defined(CONFIG_ECL_CODEC_DCT_LOW_AC)
    return ecl::CodecMode::DCT_LOW_AC_RESIDUAL;
#elif defined(CONFIG_ECL_CODEC_DCT_FULL)
    return ecl::CodecMode::DCT_FULL_RESIDUAL;
#elif defined(CONFIG_ECL_CODEC_DCT_SPARSE)
    return ecl::CodecMode::DCT_SPARSE_BLOCK;
#else
    return ecl::CodecMode::PASS_THROUGH;
#endif
}

framesize_t configured_frame_size() {
#if defined(CONFIG_ECL_FRAMESIZE_CIF)
    return FRAMESIZE_CIF;
#elif defined(CONFIG_ECL_FRAMESIZE_VGA)
    return FRAMESIZE_VGA;
#elif defined(CONFIG_ECL_FRAMESIZE_SVGA)
    return FRAMESIZE_SVGA;
#else
    return FRAMESIZE_QVGA;
#endif
}

void init_wifi_sta() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wcfg{};
    nvs_handle_t h{};
    bool loaded_nvs = false;
    if (nvs_open("cam_net", NVS_READONLY, &h) == ESP_OK) {
        size_t slen = sizeof(wcfg.sta.ssid);
        size_t plen = sizeof(wcfg.sta.password);
        if (nvs_get_str(h, "ssid", reinterpret_cast<char*>(wcfg.sta.ssid), &slen) == ESP_OK &&
            wcfg.sta.ssid[0] != '\0') {
            nvs_get_str(h, "pass", reinterpret_cast<char*>(wcfg.sta.password), &plen);
            loaded_nvs = true;
        }
        nvs_close(h);
    }
    if (!loaded_nvs) {
        std::strncpy(reinterpret_cast<char*>(wcfg.sta.ssid), CONFIG_ECL_WIFI_SSID,
                     sizeof(wcfg.sta.ssid) - 1);
        std::strncpy(reinterpret_cast<char*>(wcfg.sta.password), CONFIG_ECL_WIFI_PASSWORD,
                     sizeof(wcfg.sta.password) - 1);
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // Low-latency UDP streaming while awake
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_ECL_WIFI_TX_POWER_QDBM));
    if (wcfg.sta.ssid[0] != '\0') {
        esp_wifi_connect();
    }
}

#ifdef CONFIG_ECL_ENABLE_HTTP_MJPEG_FALLBACK
esp_err_t metrics_handler(httpd_req_t* req) {
    ecl_fw::StreamMetrics m{};
    if (g_metrics_mu && xSemaphoreTake(g_metrics_mu, pdMS_TO_TICKS(50)) == pdTRUE) {
        m = g_metrics;
        xSemaphoreGive(g_metrics_mu);
    }
    char buf[512];
    const int n = std::snprintf(
        buf, sizeof(buf),
        "{\"frame_id\":%lu,\"frame_bytes\":%lu,\"codec_bytes\":%lu,"
        "\"capture_us\":%lu,\"codec_us\":%lu,\"pkt_us\":%lu,\"interval_us\":%lu,"
        "\"dropped\":%lu,\"keyframes\":%lu,\"changed_blocks\":%lu,\"total_blocks\":%lu,"
        "\"heap_free\":%lu,\"heap_min\":%lu,\"psram_free\":%lu,\"psram_min\":%lu}\n",
        static_cast<unsigned long>(m.frame_id),
        static_cast<unsigned long>(m.last_frame_bytes),
        static_cast<unsigned long>(m.last_codec_bytes),
        static_cast<unsigned long>(m.last_capture_us),
        static_cast<unsigned long>(m.last_codec_us),
        static_cast<unsigned long>(m.last_packetize_us),
        static_cast<unsigned long>(m.last_interval_us),
        static_cast<unsigned long>(m.dropped_frames),
        static_cast<unsigned long>(m.keyframes),
        static_cast<unsigned long>(m.last_changed_blocks),
        static_cast<unsigned long>(m.last_total_blocks),
        static_cast<unsigned long>(m.free_internal_heap),
        static_cast<unsigned long>(m.min_internal_heap),
        static_cast<unsigned long>(m.free_psram),
        static_cast<unsigned long>(m.min_psram));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

void start_http_server() {
    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.core_id = 0;
    httpd_handle_t srv = nullptr;
    if (httpd_start(&srv, &hcfg) == ESP_OK) {
        httpd_uri_t metrics_uri{};
        metrics_uri.uri = "/metrics";
        metrics_uri.method = HTTP_GET;
        metrics_uri.handler = metrics_handler;
        httpd_register_uri_handler(srv, &metrics_uri);
    }
}
#endif

void stream_task(void*) {
    ecl::TemporalCodecConfig ccfg{};
    ccfg.mode = configured_codec_mode();
    ccfg.keyframe_interval = CONFIG_ECL_KEYFRAME_INTERVAL;
    ccfg.max_dependency_depth = CONFIG_ECL_KEYFRAME_INTERVAL;
    ccfg.max_frame_bytes = 98304;  // 96 KB bounded output buffer (in PSRAM via SPIRAM_USE_MALLOC)
#ifdef CONFIG_ECL_ENABLE_DCT_CODEC
    ccfg.enable_dct_buffers = (ccfg.mode != ecl::CodecMode::PASS_THROUGH &&
                               ccfg.mode != ecl::CodecMode::FRAME_SKIP);
    ccfg.max_blocks = 7680;  // Supports up to VGA 640x480 4:2:0 (7200 blocks)
    ccfg.sparse_block_threshold = 2;
#else
    ccfg.enable_dct_buffers = false;
    ccfg.max_blocks = 0;
#endif

    static ecl::TemporalCodecContext codec_ctx;
    if (!codec_ctx.init(ccfg)) {
        ESP_LOGE(kTag, "TemporalCodecContext init failed — falling back to PASS_THROUGH");
        ccfg.mode = ecl::CodecMode::PASS_THROUGH;
        ccfg.enable_dct_buffers = false;
        ccfg.max_blocks = 0;
        codec_ctx.init(ccfg);
    }
    ESP_LOGI(kTag, "Codec mode=%s allocated=%zu bytes",
             ecl::codec_mode_name(codec_ctx.mode()), codec_ctx.allocated_bytes());

    ecl::Packetizer packetizer({1, ecl::kDefaultMtuBytes});
    static uint8_t wire_buf[ecl::kDefaultMtuBytes];

    UdpTxCtx udp{};
    udp.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    udp.dest.sin_family = AF_INET;
    udp.dest.sin_port = htons(CONFIG_ECL_UDP_DEST_PORT);
    inet_pton(AF_INET, CONFIG_ECL_UDP_DEST_IP, &udp.dest.sin_addr);

    uint32_t frame_id = 0;
    int64_t prev_frame_us = esp_timer_get_time();

    for (;;) {
        const int64_t t_cap0 = esp_timer_get_time();
        camera_fb_t* fb = esp_camera_fb_get();
        const int64_t t_cap1 = esp_timer_get_time();
        if (!fb) {
            g_metrics.capture_errors++;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        ecl::EncodedFrameView fv{};
        fv.frame_id = frame_id;
        fv.capture_timestamp_us = static_cast<uint64_t>(t_cap1);
        fv.data = fb->buf;
        fv.size = fb->len;
        fv.metadata.width = static_cast<uint16_t>(fb->width);
        fv.metadata.height = static_cast<uint16_t>(fb->height);
        fv.metadata.quality = CONFIG_ECL_JPEG_QUALITY;

        ecl::CodecOutput co{};
        const int64_t t_enc0 = esp_timer_get_time();
        const ecl::CodecStatus cs = ecl::codec_encode(&codec_ctx, fv, &co);
        const int64_t t_enc1 = esp_timer_get_time();

        size_t pkts = 0;
        const int64_t t_pkt0 = esp_timer_get_time();
        if (cs == ecl::CodecStatus::OK) {
            pkts = packetizer.packetize(co, static_cast<uint32_t>(t_cap1), fv.metadata,
                                        wire_buf, sizeof(wire_buf), udp_emit_cb, &udp);
        }
        const int64_t t_pkt1 = esp_timer_get_time();

        const uint32_t raw_len = static_cast<uint32_t>(fb->len);
        esp_camera_fb_return(fb);

        if (g_metrics_mu && xSemaphoreTake(g_metrics_mu, 0) == pdTRUE) {
            g_metrics.frame_id = frame_id;
            g_metrics.last_frame_bytes = raw_len;
            g_metrics.last_codec_bytes = static_cast<uint32_t>(co.bytes.size);
            g_metrics.last_capture_us = static_cast<uint32_t>(t_cap1 - t_cap0);
            g_metrics.last_codec_us = static_cast<uint32_t>(t_enc1 - t_enc0);
            g_metrics.last_packetize_us = static_cast<uint32_t>(t_pkt1 - t_pkt0);
            g_metrics.last_interval_us = static_cast<uint32_t>(t_cap1 - prev_frame_us);
            if (pkts == 0) g_metrics.dropped_frames++;
            if (co.is_keyframe) g_metrics.keyframes++;
            g_metrics.last_changed_blocks = co.changed_blocks;
            g_metrics.last_total_blocks = co.total_blocks;
            g_metrics.total_input_bytes += raw_len;
            g_metrics.total_tx_bytes = packetizer.stats().wire_bytes;
            g_metrics.free_internal_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            g_metrics.min_internal_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
            g_metrics.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            g_metrics.min_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
            xSemaphoreGive(g_metrics_mu);
        }

        prev_frame_us = t_cap1;
        frame_id++;
    }
}

}  // namespace

extern "C" void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    g_metrics_mu = xSemaphoreCreateMutex();
    init_wifi_sta();

    ecl_fw::CamInitParams cam_params{};
    cam_params.frame_size = configured_frame_size();
    cam_params.jpeg_quality = CONFIG_ECL_JPEG_QUALITY;
    cam_params.xclk_hz = CONFIG_ECL_XCLK_MHZ * 1000000;
    cam_params.fb_count = 2;

    if (ecl_fw::init_ai_thinker_camera(cam_params) != ESP_OK) {
        ESP_LOGE(kTag, "Camera init failed; check OV2640 ribbon & 5V rail");
        return;
    }

#ifdef CONFIG_ECL_ENABLE_HTTP_MJPEG_FALLBACK
    start_http_server();
#endif

    // Pin camera capture + codec + packetizer to Core 1; Wi-Fi + HTTP run on Core 0.
    xTaskCreatePinnedToCore(stream_task, "ecl_stream", 8192, nullptr, 10, nullptr, 1);
}
