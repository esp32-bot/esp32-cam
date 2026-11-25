// Ultra-minimal ESP32-CAM factory rescue / Wi-Fi OTA stub (lives in 576 KB `factory` partition).
// Pure C, -Os, 80 MHz, STA-only, IPv4-only, zero C++/mbedTLS TLS/esp_http_server/mdns component!
// Hardware safety:
//   - Cuts OV2640 power (GPIO32 HIGH) and flash LED (GPIO4 LOW) so board stays cool during OTA.
//   - Blinks red LED (GPIO33 active-LOW).
// Endpoints (raw LwIP TCP port 80, CORS enabled):
//   - GET  /api/status  -> JSON status (is_stub:true, running_partition:"factory", compressed_ota:true, sector_skip:true)
//   - POST /api/ota     -> Stream raw or zlib-compressed (`application/x-balax-zlib`) firmware to `ota_0`
//                          with 4 KB read-compare sector-skip, optional `X-Sha256` verification,
//                          and `esp_image_verify()`, then switches boot partition to `ota_0` & reboots.
//   - POST /api/reboot  -> Returns to valid `ota_0` immediately.
// Auto-returns to valid `ota_0` after 60 s idle (stays indefinitely if `ota_0` is invalid).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_image_format.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "miniz.h"
#include "nvs.h"
#include "nvs_flash.h"

#define PIN_FLASH_LED  4
#define PIN_CAM_PWDN  32
#define PIN_RED_LED   33
#define SECTOR_BYTES  4096

// --- Pure-C SHA-256 (for optional X-Sha256 raw-image integrity check) ---
typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    size_t buf_len;
    uint64_t total_len;
} c_sha256_t;

static const uint32_t SHA_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t rotr32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void sha_init(c_sha256_t* s) {
    s->h[0] = 0x6a09e667u; s->h[1] = 0xbb67ae85u; s->h[2] = 0x3c6ef372u; s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu; s->h[5] = 0x9b05688cu; s->h[6] = 0x1f83d9abu; s->h[7] = 0x5be0cd19u;
    s->buf_len = 0; s->total_len = 0;
}

static void sha_compress(c_sha256_t* s, const uint8_t blk[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)blk[i*4]<<24)|((uint32_t)blk[i*4+1]<<16)|((uint32_t)blk[i*4+2]<<8)|blk[i*4+3];
    for (int i = 16; i < 64; ++i)
        w[i] = w[i-16] + (rotr32(w[i-15],7)^rotr32(w[i-15],18)^(w[i-15]>>3)) + w[i-7] +
               (rotr32(w[i-2],17)^rotr32(w[i-2],19)^(w[i-2]>>10));
    uint32_t a=s->h[0], b=s->h[1], c=s->h[2], d=s->h[3], e=s->h[4], f=s->h[5], g=s->h[6], hh=s->h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = hh + (rotr32(e,6)^rotr32(e,11)^rotr32(e,25)) + ((e&f)^((~e)&g)) + SHA_K[i] + w[i];
        uint32_t t2 = (rotr32(a,2)^rotr32(a,13)^rotr32(a,22)) + ((a&b)^(a&c)^(b&c));
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=hh;
}

static void sha_update(c_sha256_t* s, const uint8_t* p, size_t n) {
    s->total_len += n;
    size_t pos = 0;
    if (s->buf_len) {
        size_t take = (64 - s->buf_len < n) ? (64 - s->buf_len) : n;
        memcpy(s->buf + s->buf_len, p, take);
        s->buf_len += take; pos += take;
        if (s->buf_len == 64) { sha_compress(s, s->buf); s->buf_len = 0; }
    }
    while (pos + 64 <= n) { sha_compress(s, p + pos); pos += 64; }
    if (pos < n) { s->buf_len = n - pos; memcpy(s->buf, p + pos, s->buf_len); }
}

static void sha_final(c_sha256_t* s, uint8_t out[32]) {
    uint64_t bits = s->total_len * 8;
    s->buf[s->buf_len++] = 0x80;
    if (s->buf_len > 56) { memset(s->buf + s->buf_len, 0, 64 - s->buf_len); sha_compress(s, s->buf); s->buf_len = 0; }
    memset(s->buf + s->buf_len, 0, 56 - s->buf_len);
    for (int i = 0; i < 8; ++i) s->buf[56 + i] = (uint8_t)(bits >> (56 - i * 8));
    sha_compress(s, s->buf);
    for (int i = 0; i < 8; ++i) {
        out[i*4]=(uint8_t)(s->h[i]>>24); out[i*4+1]=(uint8_t)(s->h[i]>>16);
        out[i*4+2]=(uint8_t)(s->h[i]>>8); out[i*4+3]=(uint8_t)s->h[i];
    }
}

static int hex_nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int from_hex(const char* h, uint8_t* out, size_t n) {
    if (!h || strlen(h) < n * 2) return 0;
    for (size_t i = 0; i < n; ++i) {
        int a = hex_nib(h[i*2]), b = hex_nib(h[i*2+1]);
        if (a < 0 || b < 0) return 0;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return 1;
}

static uint32_t g_ip_be = 0;
static int64_t g_last_activity_us = 0;

static void init_cam_safe_gpios(void) {
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << PIN_FLASH_LED) | (1ULL << PIN_CAM_PWDN) | (1ULL << PIN_RED_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);
    gpio_set_level(PIN_FLASH_LED, 0);
    gpio_set_level(PIN_CAM_PWDN, 1);  // Active-LOW power gate -> HIGH = camera OFF
    gpio_set_level(PIN_RED_LED, 1);   // Active-LOW red LED -> HIGH = OFF
}

static void mdns_task(void* arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) vTaskDelete(NULL);
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET, .sin_port = htons(5353), .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    bind(s, (struct sockaddr*)&bind_addr, sizeof bind_addr);
    struct ip_mreq mreq = {
        .imr_multiaddr.s_addr = inet_addr("224.0.0.251"),
        .imr_interface.s_addr = htonl(INADDR_ANY),
    };
    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq);

    // Pre-built mDNS A-record answer for "balax-cam.local"
    static uint8_t pkt[43] = {
        0x00,0x00, 0x84,0x00, 0x00,0x00, 0x00,0x01, 0x00,0x00, 0x00,0x00,
        9,'b','a','l','a','x','-','c','a','m', 5,'l','o','c','a','l', 0,
        0x00,0x01, 0x80,0x01, 0x00,0x00,0x00,120, 0x00,0x04, 0,0,0,0
    };
    static const uint8_t target_q[] = {
        9,'b','a','l','a','x','-','c','a','m', 5,'l','o','c','a','l'
    };
    struct sockaddr_in mcast = {
        .sin_family = AF_INET, .sin_port = htons(5353), .sin_addr.s_addr = inet_addr("224.0.0.251")
    };
    uint8_t rx[256];
    for (;;) {
        int r = recv(s, rx, sizeof rx, 0);
        if (r > 22 && g_ip_be != 0 && (rx[2] & 0x80) == 0) {
            if (memmem(rx + 12, (size_t)(r - 12), target_q, sizeof target_q) != NULL) {
                memcpy(pkt + 39, &g_ip_be, 4);
                sendto(s, pkt, sizeof pkt, 0, (struct sockaddr*)&mcast, sizeof mcast);
            }
        }
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
        g_ip_be = e->ip_info.ip.addr;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_ip_be = 0;
        esp_wifi_connect();
    }
}

static void start_wifi_sta(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
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

static int ota0_is_valid(void) {
    const esp_partition_t* p =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!p) return 0;
    esp_partition_pos_t pos = { .offset = p->address, .size = p->size };
    esp_image_metadata_t meta;
    return esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta) == ESP_OK;
}

static void return_to_ota0(void) {
    const esp_partition_t* p =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (p && ota0_is_valid()) {
        esp_ota_set_boot_partition(p);
        esp_restart();
    }
}

static const char* find_hdr(const char* hdrs, const char* key, char* out, size_t cap) {
    const char* p = strcasestr(hdrs, key);
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') ++p;
    size_t i = 0;
    while (*p && *p != '\r' && *p != '\n' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    return out;
}

static void send_http(int fd, const char* status, const char* ctype, const char* body) {
    static char buf[512];
    int n = snprintf(buf, sizeof buf,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                     "Access-Control-Allow-Headers: Content-Type, X-Sha256\r\n"
                     "Connection: close\r\n\r\n%s",
                     status, ctype, (unsigned)strlen(body), body);
    send(fd, buf, (size_t)n, 0);
}

static void handle_client(int fd) {
    static char hdr[1024];
    size_t hlen = 0;
    char* body_start = NULL;
    while (hlen + 1 < sizeof hdr) {
        int r = recv(fd, hdr + hlen, sizeof hdr - 1 - hlen, 0);
        if (r <= 0) return;
        hlen += (size_t)r;
        hdr[hlen] = 0;
        body_start = strstr(hdr, "\r\n\r\n");
        if (body_start) { body_start += 4; break; }
    }
    if (!body_start) return;

    if (strncmp(hdr, "OPTIONS ", 8) == 0) {
        send_http(fd, "204 No Content", "text/plain", "");
        return;
    }

    if (strncmp(hdr, "GET /api/status", 15) == 0) {
        char ip_str[20] = "0.0.0.0";
        if (g_ip_be != 0) {
            const uint8_t* b = (const uint8_t*)&g_ip_be;
            snprintf(ip_str, sizeof ip_str, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        }
        char json[280];
        snprintf(json, sizeof json,
                 "{\"ok\":true,\"is_stub\":true,\"power_mode\":\"ota_stub\",\"camera_on\":false,"
                 "\"cpu_mhz\":80,\"running_partition\":\"factory\",\"compressed_ota\":true,"
                 "\"sector_skip\":true,\"ota0_valid\":%s,\"ip\":\"%s\"}",
                 ota0_is_valid() ? "true" : "false", ip_str);
        send_http(fd, "200 OK", "application/json", json);
        return;
    }

    if (strncmp(hdr, "POST /api/reboot", 16) == 0) {
        send_http(fd, "200 OK", "application/json", "{\"ok\":true,\"rebooting_to\":\"ota_0\"}");
        vTaskDelay(pdMS_TO_TICKS(150));
        return_to_ota0();
        return;
    }

    if (strncmp(hdr, "POST /api/ota", 13) != 0) {
        send_http(fd, "404 Not Found", "text/plain", "esp32cam ota stub");
        return;
    }

    char clen_s[24] = {0}, ctype[64] = {0}, sha_hex[72] = {0};
    if (!find_hdr(hdr, "Content-Length:", clen_s, sizeof clen_s)) {
        send_http(fd, "411 Length Required", "text/plain", "Missing Content-Length");
        return;
    }
    find_hdr(hdr, "Content-Type:", ctype, sizeof ctype);
    int check_sha = 0;
    uint8_t exp_sha[32];
    if (find_hdr(hdr, "X-Sha256:", sha_hex, sizeof sha_hex)) {
        if (!from_hex(sha_hex, exp_sha, 32)) {
            send_http(fd, "400 Bad Request", "text/plain", "Malformed X-Sha256");
            return;
        }
        check_sha = 1;
    }

    g_last_activity_us = esp_timer_get_time();

    const esp_partition_t* ota0 =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!ota0) {
        send_http(fd, "500 Internal Server Error", "text/plain", "No ota_0 partition");
        return;
    }

    size_t content_len = (size_t)strtoul(clen_s, NULL, 10);
    if (content_len == 0 || content_len > ota0->size) {
        send_http(fd, "400 Bad Request", "text/plain", "Invalid image size");
        return;
    }

    int compressed = (strstr(ctype, "application/x-balax-zlib") != NULL);
    uint8_t* pending = malloc(SECTOR_BYTES);
    uint8_t* existing = malloc(SECTOR_BYTES);
    tinfl_decompressor* inf = compressed ? malloc(sizeof(tinfl_decompressor)) : NULL;
    uint8_t* dict = compressed ? malloc(TINFL_LZ_DICT_SIZE) : NULL;
    if (!pending || !existing || (compressed && (!inf || !dict))) {
        free(pending); free(existing); free(inf); free(dict);
        send_http(fd, "500 Internal Server Error", "text/plain", "OOM");
        return;
    }
    if (compressed) tinfl_init(inf);

    c_sha256_t sha;
    if (check_sha) sha_init(&sha);

    size_t pend_len = 0, wofs = 0, wire_got = 0, raw_got = 0, dict_ofs = 0;
    uint32_t erased = 0, skipped = 0;
    int err = 0, done = 0;

    #define FLUSH_SEC() do { \
        if (pend_len > 0) { \
            if (pend_len < SECTOR_BYTES) memset(pending + pend_len, 0xFF, SECTOR_BYTES - pend_len); \
            if (wofs + SECTOR_BYTES > ota0->size) { err = 1; } \
            else if (esp_partition_read(ota0, wofs, existing, SECTOR_BYTES) == ESP_OK && \
                     memcmp(existing, pending, SECTOR_BYTES) == 0) { \
                ++skipped; \
            } else { \
                if (esp_partition_erase_range(ota0, wofs, SECTOR_BYTES) != ESP_OK || \
                    esp_partition_write(ota0, wofs, pending, SECTOR_BYTES) != ESP_OK) err = 1; \
                else ++erased; \
            } \
            wofs += SECTOR_BYTES; pend_len = 0; \
        } \
    } while (0)

    size_t first_len = hlen - (size_t)(body_start - hdr);
    uint8_t rx[1024];
    int led = 0;
    while (!err && wire_got < content_len) {
        const uint8_t* chunk = rx;
        size_t cl = 0;
        if (first_len > 0) {
            chunk = (const uint8_t*)body_start;
            cl = first_len; first_len = 0;
        } else {
            size_t want = content_len - wire_got;
            if (want > sizeof rx) want = sizeof rx;
            int r = recv(fd, rx, want, 0);
            if (r <= 0) { err = 1; break; }
            cl = (size_t)r;
        }
        wire_got += cl;
        if ((wire_got & 0x3FFF) < cl) {
            led ^= 1;
            gpio_set_level(PIN_RED_LED, led);
        }

        if (!compressed) {
            if (check_sha) sha_update(&sha, chunk, cl);
            for (size_t i = 0; i < cl && !err; ) {
                size_t take = SECTOR_BYTES - pend_len;
                if (take > cl - i) take = cl - i;
                memcpy(pending + pend_len, chunk + i, take);
                pend_len += take; i += take; raw_got += take;
                if (pend_len == SECTOR_BYTES) FLUSH_SEC();
            }
        } else {
            size_t ip = 0;
            uint32_t fl = TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_COMPUTE_ADLER32 |
                          (wire_got < content_len ? (uint32_t)TINFL_FLAG_HAS_MORE_INPUT : 0u);
            while (ip < cl && !done && !err) {
                size_t inb = cl - ip, outb = TINFL_LZ_DICT_SIZE - dict_ofs;
                tinfl_status st = tinfl_decompress(inf, chunk + ip, &inb, dict, dict + dict_ofs, &outb, fl);
                ip += inb;
                if (outb > 0 && check_sha) sha_update(&sha, dict + dict_ofs, outb);
                for (size_t i = 0; i < outb && !err; ) {
                    size_t take = SECTOR_BYTES - pend_len;
                    if (take > outb - i) take = outb - i;
                    memcpy(pending + pend_len, dict + dict_ofs + i, take);
                    pend_len += take; i += take; raw_got += take;
                    if (pend_len == SECTOR_BYTES) FLUSH_SEC();
                }
                dict_ofs = (dict_ofs + outb) & (TINFL_LZ_DICT_SIZE - 1);
                if (st == TINFL_STATUS_DONE) done = 1;
                else if (st < 0) err = 1;
                else if (st == TINFL_STATUS_NEEDS_MORE_INPUT && inb == 0 && outb == 0) break;
            }
        }
        g_last_activity_us = esp_timer_get_time();
    }
    if (!err) FLUSH_SEC();
    #undef FLUSH_SEC

    if (check_sha && !err) {
        uint8_t got_sha[32];
        sha_final(&sha, got_sha);
        if (memcmp(got_sha, exp_sha, 32) != 0) err = 1;
    }

    if (err || (compressed && !done) || !ota0_is_valid()) {
        free(pending); free(existing); free(inf); free(dict);
        gpio_set_level(PIN_RED_LED, 1);
        send_http(fd, "400 Bad Request", "text/plain", "OTA verification failed");
        return;
    }

    esp_ota_set_boot_partition(ota0);
    gpio_set_level(PIN_RED_LED, 1);
    char resp[220];
    snprintf(resp, sizeof resp,
             "{\"ok\":true,\"partition\":\"ota_0\",\"compressed\":%s,\"bytes\":%u,\"raw_bytes\":%u,"
             "\"sectors_erased\":%lu,\"sectors_skipped\":%lu}",
             compressed ? "true" : "false", (unsigned)wire_got, (unsigned)raw_got,
             (unsigned long)erased, (unsigned long)skipped);
    send_http(fd, "200 OK", "application/json", resp);
    free(pending); free(existing); free(inf); free(dict);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

void app_main(void) {
    init_cam_safe_gpios();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    int ota0_ok = ota0_is_valid();
    start_wifi_sta();
    xTaskCreate(mdns_task, "cam_mdns", 2560, NULL, 3, NULL);

    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int reuse = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(80), .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    bind(srv, (struct sockaddr*)&addr, sizeof addr);
    listen(srv, 2);

    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    g_last_activity_us = esp_timer_get_time();

    int blink = 0;
    for (;;) {
        int cli = accept(srv, NULL, NULL);
        if (cli >= 0) {
            struct timeval ctv = { .tv_sec = 15, .tv_usec = 0 };
            setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof ctv);
            handle_client(cli);
            close(cli);
        }
        blink ^= 1;
        gpio_set_level(PIN_RED_LED, blink);
        int64_t idle_us = esp_timer_get_time() - g_last_activity_us;
        if (ota0_ok && idle_us > 60LL * 1000 * 1000) {
            return_to_ota0();
        }
    }
}
