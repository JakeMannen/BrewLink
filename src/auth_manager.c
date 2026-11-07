/**
 * @file auth_manager.c
 * @brief External Client Pairing, Network Broadcast Discovery, Token Auth, and HMAC-SHA256
 * @details Zero-allocation cryptographic helpers, GPIO 0 (BOOT button) 3s hold monitor,
 *          and UDP (:53540) + MQTT pairing discovery broadcaster.
 */

#include "auth_manager.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "config.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
static const char *TAG = "AUTH_MGR";
#else
static uint32_t s_mock_time_ms = 1000;
static uint32_t s_mock_rand_seed = 0x12345678;
#endif

static char     s_active_pin[AUTH_PAIRING_PIN_LEN + 1] = {0};
static uint32_t s_pin_expires_at_ms = 0;
static bool     s_window_open = false;

static char     s_test_api_token[CONFIG_STORE_TOKEN_MAX_LEN] = {0};
static char     s_test_topic_key[CONFIG_STORE_TOPIC_KEY_MAX_LEN] = {0};
static char     s_test_client_name[CONFIG_STORE_CLIENT_NAME_MAX_LEN] = {0};
static bool     s_test_paired = false;
static paired_client_t s_test_clients[CONFIG_STORE_MAX_PAIRED_CLIENTS];

#ifdef ESP_PLATFORM
static SemaphoreHandle_t s_auth_mutex = NULL;
static int               s_udp_sock = -1;
#endif

static uint32_t get_now_ms(void) {
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
#else
    return s_mock_time_ms;
#endif
}

static uint32_t get_random_u32(void) {
#ifdef ESP_PLATFORM
    return esp_random();
#else
    s_mock_rand_seed = s_mock_rand_seed * 1664525UL + 1013904223UL;
    return s_mock_rand_seed;
#endif
}

static void generate_hex_bytes(char *out_hex, size_t num_bytes) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < num_bytes; i++) {
        uint8_t b = (uint8_t)(get_random_u32() & 0xFF);
        out_hex[i * 2]     = hex[(b >> 4) & 0x0F];
        out_hex[i * 2 + 1] = hex[b & 0x0F];
    }
    out_hex[num_bytes * 2] = '\0';
}

// =============================================================================
// Zero-Allocation FIPS 180-4 SHA-256 & RFC 2104 HMAC-SHA256
// =============================================================================
typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t  buffer[64];
} sha256_ctx_t;

static const uint32_t k_sha256[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROR32(x, 2) ^ ROR32(x, 13) ^ ROR32(x, 22))
#define EP1(x) (ROR32(x, 6) ^ ROR32(x, 11) ^ ROR32(x, 25))
#define SIG0(x) (ROR32(x, 7) ^ ROR32(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROR32(x, 17) ^ ROR32(x, 19) ^ ((x) >> 10))

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    int i;

    for (i = 0; i < 16; ++i) {
        m[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (; i < 64; ++i) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + k_sha256[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx) {
    ctx->state[0] = 0x6a09e667UL;
    ctx->state[1] = 0xbb67ae85UL;
    ctx->state[2] = 0x3c6ef372UL;
    ctx->state[3] = 0xa54ff53aUL;
    ctx->state[4] = 0x510e527fUL;
    ctx->state[5] = 0x9b05688cUL;
    ctx->state[6] = 0x1f83d9abUL;
    ctx->state[7] = 0x5be0cd19UL;
    ctx->bitcount = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        uint32_t idx = (uint32_t)((ctx->bitcount >> 3) & 0x3F);
        ctx->buffer[idx] = data[i];
        ctx->bitcount += 8;
        if (idx == 63) {
            sha256_transform(ctx, ctx->buffer);
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32]) {
    uint32_t idx = (uint32_t)((ctx->bitcount >> 3) & 0x3F);
    ctx->buffer[idx++] = 0x80;

    if (idx > 56) {
        while (idx < 64) {
            ctx->buffer[idx++] = 0x00;
        }
        sha256_transform(ctx, ctx->buffer);
        idx = 0;
    }
    while (idx < 56) {
        ctx->buffer[idx++] = 0x00;
    }

    for (int i = 0; i < 8; ++i) {
        ctx->buffer[63 - i] = (uint8_t)((ctx->bitcount >> (i * 8)) & 0xFF);
    }
    sha256_transform(ctx, ctx->buffer);

    for (int i = 0; i < 8; ++i) {
        hash[i * 4]     = (uint8_t)((ctx->state[i] >> 24) & 0xFF);
        hash[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFF);
        hash[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xFF);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xFF);
    }
}

esp_err_t auth_manager_compute_hmac_sha256_hex(const char *key,
                                               const char *message,
                                               char out_hex[AUTH_HMAC_HEX_LEN + 1]) {
    if (key == NULL || message == NULL || out_hex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t k_ipad[64];
    uint8_t k_opad[64];
    uint8_t tk[32];
    size_t key_len = strlen(key);
    const uint8_t *k_ptr = (const uint8_t *)key;

    if (key_len > 64) {
        sha256_ctx_t tctx;
        sha256_init(&tctx);
        sha256_update(&tctx, k_ptr, key_len);
        sha256_final(&tctx, tk);
        k_ptr = tk;
        key_len = 32;
    }

    memset(k_ipad, 0x36, sizeof(k_ipad));
    memset(k_opad, 0x5c, sizeof(k_opad));
    for (size_t i = 0; i < key_len; ++i) {
        k_ipad[i] ^= k_ptr[i];
        k_opad[i] ^= k_ptr[i];
    }

    uint8_t inner_digest[32];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, 64);
    sha256_update(&ctx, (const uint8_t *)message, strlen(message));
    sha256_final(&ctx, inner_digest);

    uint8_t outer_digest[32];
    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, 64);
    sha256_update(&ctx, inner_digest, 32);
    sha256_final(&ctx, outer_digest);

    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out_hex[i * 2]     = hex_chars[(outer_digest[i] >> 4) & 0x0F];
        out_hex[i * 2 + 1] = hex_chars[outer_digest[i] & 0x0F];
    }
    out_hex[64] = '\0';
    return ESP_OK;
}

// Constant-time string comparison (case-sensitive or case-insensitive)
static bool constant_time_equals(const char *a, const char *b, bool ignore_case) {
    if (a == NULL || b == NULL) {
        return false;
    }
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    if (len_a == 0 || len_a != len_b) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < len_a; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ignore_case) {
            ca = (unsigned char)tolower(ca);
            cb = (unsigned char)tolower(cb);
        }
        diff |= (ca ^ cb);
    }
    return (diff == 0);
}

static void get_active_credentials(char out_token[CONFIG_STORE_TOKEN_MAX_LEN],
                                   char out_topic_key[CONFIG_STORE_TOPIC_KEY_MAX_LEN],
                                   char out_client_name[CONFIG_STORE_CLIENT_NAME_MAX_LEN],
                                   bool *out_paired) {
#ifdef ESP_PLATFORM
    if (s_test_api_token[0] != '\0') {
        if (out_token) strncpy(out_token, s_test_api_token, CONFIG_STORE_TOKEN_MAX_LEN - 1);
        if (out_topic_key) strncpy(out_topic_key, s_test_topic_key, CONFIG_STORE_TOPIC_KEY_MAX_LEN - 1);
        if (out_client_name) strncpy(out_client_name, s_test_client_name, CONFIG_STORE_CLIENT_NAME_MAX_LEN - 1);
        if (out_paired) *out_paired = s_test_paired;
        return;
    }
    brewlink_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (config_store_get(&cfg) == ESP_OK) {
        if (out_token) {
            strncpy(out_token, cfg.api_token, CONFIG_STORE_TOKEN_MAX_LEN - 1);
            out_token[CONFIG_STORE_TOKEN_MAX_LEN - 1] = '\0';
        }
        if (out_topic_key) {
            strncpy(out_topic_key, cfg.mqtt_topic_key, CONFIG_STORE_TOPIC_KEY_MAX_LEN - 1);
            out_topic_key[CONFIG_STORE_TOPIC_KEY_MAX_LEN - 1] = '\0';
        }
        if (out_client_name) {
            strncpy(out_client_name, cfg.paired_client_name, CONFIG_STORE_CLIENT_NAME_MAX_LEN - 1);
            out_client_name[CONFIG_STORE_CLIENT_NAME_MAX_LEN - 1] = '\0';
        }
        if (out_paired) {
            *out_paired = cfg.client_paired;
        }
    }
#else
    if (out_token) {
        strncpy(out_token, s_test_api_token, CONFIG_STORE_TOKEN_MAX_LEN - 1);
        out_token[CONFIG_STORE_TOKEN_MAX_LEN - 1] = '\0';
    }
    if (out_topic_key) {
        strncpy(out_topic_key, s_test_topic_key, CONFIG_STORE_TOPIC_KEY_MAX_LEN - 1);
        out_topic_key[CONFIG_STORE_TOPIC_KEY_MAX_LEN - 1] = '\0';
    }
    if (out_client_name) {
        strncpy(out_client_name, s_test_client_name, CONFIG_STORE_CLIENT_NAME_MAX_LEN - 1);
        out_client_name[CONFIG_STORE_CLIENT_NAME_MAX_LEN - 1] = '\0';
    }
    if (out_paired) {
        *out_paired = s_test_paired;
    }
#endif
}

void auth_manager_set_test_credentials(const char *api_token, const char *topic_key, bool paired) {
    memset(s_test_api_token, 0, sizeof(s_test_api_token));
    memset(s_test_topic_key, 0, sizeof(s_test_topic_key));
    memset(s_test_clients, 0, sizeof(s_test_clients));
    if (api_token != NULL) {
        strncpy(s_test_api_token, api_token, sizeof(s_test_api_token) - 1);
    }
    if (topic_key != NULL) {
        strncpy(s_test_topic_key, topic_key, sizeof(s_test_topic_key) - 1);
    }
    s_test_paired = paired;
    if (paired && api_token != NULL && api_token[0] != '\0') {
        s_test_clients[0].active = true;
        strncpy(s_test_clients[0].client_name, "Test-Client-0001", sizeof(s_test_clients[0].client_name) - 1);
        strncpy(s_test_clients[0].api_token, api_token, sizeof(s_test_clients[0].api_token) - 1);
        if (topic_key != NULL) {
            strncpy(s_test_clients[0].mqtt_topic_key, topic_key, sizeof(s_test_clients[0].mqtt_topic_key) - 1);
        }
    }
}

esp_err_t auth_manager_get_paired_clients(paired_client_t out_clients[CONFIG_STORE_MAX_PAIRED_CLIENTS],
                                          uint8_t *out_count) {
    if (out_clients == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_clients, 0, sizeof(paired_client_t) * CONFIG_STORE_MAX_PAIRED_CLIENTS);
#ifdef ESP_PLATFORM
    if (s_test_api_token[0] != '\0') {
        memcpy(out_clients, s_test_clients, sizeof(s_test_clients));
    } else {
        config_store_get_paired_clients(out_clients);
    }
#else
    memcpy(out_clients, s_test_clients, sizeof(s_test_clients));
#endif
    uint8_t count = 0;
    for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_CLIENTS; i++) {
        if (out_clients[i].active) {
            count++;
        }
    }
    if (out_count != NULL) {
        *out_count = count;
    }
    return ESP_OK;
}

esp_err_t auth_manager_delete_paired_client(const char *client_name) {
#ifdef ESP_PLATFORM
    if (s_auth_mutex != NULL) {
        xSemaphoreTake(s_auth_mutex, pdMS_TO_TICKS(200));
    }
    esp_err_t err = config_store_delete_paired_client(client_name);
    if (s_auth_mutex != NULL) {
        xSemaphoreGive(s_auth_mutex);
    }
    return err;
#else
    bool found = false;
    if (client_name == NULL || client_name[0] == '\0') {
        memset(s_test_clients, 0, sizeof(s_test_clients));
        memset(s_test_api_token, 0, sizeof(s_test_api_token));
        memset(s_test_topic_key, 0, sizeof(s_test_topic_key));
        memset(s_test_client_name, 0, sizeof(s_test_client_name));
        s_test_paired = false;
        return ESP_OK;
    }
    for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_CLIENTS; i++) {
        if (s_test_clients[i].active && strcmp(s_test_clients[i].client_name, client_name) == 0) {
            memset(&s_test_clients[i], 0, sizeof(paired_client_t));
            found = true;
        }
    }
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    // Re-sync legacy test fields to first remaining active client
    s_test_paired = false;
    memset(s_test_api_token, 0, sizeof(s_test_api_token));
    memset(s_test_topic_key, 0, sizeof(s_test_topic_key));
    memset(s_test_client_name, 0, sizeof(s_test_client_name));
    for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_CLIENTS; i++) {
        if (s_test_clients[i].active) {
            s_test_paired = true;
            strncpy(s_test_client_name, s_test_clients[i].client_name, sizeof(s_test_client_name) - 1);
            strncpy(s_test_api_token, s_test_clients[i].api_token, sizeof(s_test_api_token) - 1);
            strncpy(s_test_topic_key, s_test_clients[i].mqtt_topic_key, sizeof(s_test_topic_key) - 1);
            break;
        }
    }
    return ESP_OK;
#endif
}

// Helper: build a guaranteed unique "<presented_name>-<4_hex_suffix>" client name
static void format_unique_client_name(const char *presented_name,
                                      char out_name[CONFIG_STORE_CLIENT_NAME_MAX_LEN]) {
    const char *base = (presented_name != NULL && presented_name[0] != '\0')
                       ? presented_name
                       : "External-Client";

    paired_client_t existing[CONFIG_STORE_MAX_PAIRED_CLIENTS];
    auth_manager_get_paired_clients(existing, NULL);

    for (int attempt = 0; attempt < 32; attempt++) {
        uint16_t suffix = (uint16_t)(get_random_u32() & 0xFFFFU);
        snprintf(out_name, CONFIG_STORE_CLIENT_NAME_MAX_LEN, "%.25s-%04X", base, (unsigned int)suffix);

        bool collision = false;
        for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_CLIENTS; i++) {
            if (existing[i].active && strcmp(existing[i].client_name, out_name) == 0) {
                collision = true;
                break;
            }
        }
        if (!collision) {
            return;
        }
    }
}

esp_err_t auth_manager_format_discovery_beacon(const char *gateway_id,
                                               const char *ip_str,
                                               uint16_t http_port,
                                               char *out_json,
                                               size_t max_len) {
    if (out_json == NULL || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef ESP_PLATFORM
    if (s_auth_mutex != NULL) {
        xSemaphoreTake(s_auth_mutex, pdMS_TO_TICKS(100));
    }
#endif

    uint32_t now = get_now_ms();
    if (!s_window_open || (int32_t)(s_pin_expires_at_ms - now) <= 0) {
        s_window_open = false;
#ifdef ESP_PLATFORM
        if (s_auth_mutex != NULL) xSemaphoreGive(s_auth_mutex);
#endif
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t rem_sec = (uint32_t)((s_pin_expires_at_ms - now) / 1000UL);
    const char *gw = (gateway_id && gateway_id[0]) ? gateway_id : "BREWLINK0000";
    const char *ip = (ip_str && ip_str[0]) ? ip_str : "192.168.4.1";
    uint16_t port = (http_port > 0) ? http_port : 80;

    snprintf(out_json, max_len,
             "{\"event\":\"brewlink_pairing\",\"gateway_id\":\"%s\","
             "\"name\":\"BrewLink Gateway\",\"ip\":\"%s\",\"http_port\":%u,"
             "\"claim_url\":\"http://%s/api/pair/claim\","
             "\"ws_url\":\"ws://%s/ws\",\"remaining_sec\":%lu}",
             gw, ip, (unsigned int)port, ip, ip, (unsigned long)rem_sec);

#ifdef ESP_PLATFORM
    if (s_auth_mutex != NULL) {
        xSemaphoreGive(s_auth_mutex);
    }
#endif
    return ESP_OK;
}

#ifdef ESP_PLATFORM
static void broadcast_pairing_beacon_once(void) {
    wifi_mgr_status_t wifi_stat;
    memset(&wifi_stat, 0, sizeof(wifi_stat));
    wifi_manager_get_status(&wifi_stat);

    const char *ip_str = (wifi_stat.sta_connected && wifi_stat.sta_ip[0] != '\0')
                         ? wifi_stat.sta_ip
                         : "192.168.4.1";
    const char *gw_id = mqtt_manager_get_gateway_id();

    char beacon_json[320];
    if (auth_manager_format_discovery_beacon(gw_id, ip_str, WEB_SERVER_PORT,
                                             beacon_json, sizeof(beacon_json)) != ESP_OK) {
        return;
    }

    // 1. Send UDP broadcast datagram to 255.255.255.255:53540
    if (s_udp_sock < 0) {
        s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_udp_sock >= 0) {
            int broadcast_enable = 1;
            setsockopt(s_udp_sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));
        }
    }
    if (s_udp_sock >= 0) {
        struct sockaddr_in dest_addr;
        memset(&dest_addr, 0, sizeof(dest_addr));
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(AUTH_DISCOVERY_UDP_PORT);
        dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sendto(s_udp_sock, beacon_json, strlen(beacon_json), 0,
               (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    }

    // 2. Publish MQTT discovery announcement if broker is connected
    if (mqtt_manager_is_connected()) {
        char disc_topic[96];
        snprintf(disc_topic, sizeof(disc_topic), "brewlink/discovery/%s", gw_id);
        mqtt_manager_publish(disc_topic, beacon_json, 0, 0, 0);
    }
}
#endif

esp_err_t auth_manager_start_pairing(char out_pin[AUTH_PAIRING_PIN_LEN + 1], uint32_t *out_ttl_sec) {
#ifdef ESP_PLATFORM
    if (s_auth_mutex != NULL) {
        xSemaphoreTake(s_auth_mutex, pdMS_TO_TICKS(200));
    }
#endif

    uint32_t pin_val = 100000UL + (get_random_u32() % 900000UL);
    snprintf(s_active_pin, sizeof(s_active_pin), "%06lu", (unsigned long)pin_val);
    s_pin_expires_at_ms = get_now_ms() + (AUTH_PAIRING_WINDOW_SEC * 1000UL);
    s_window_open = true;

    if (out_pin != NULL) {
        memcpy(out_pin, s_active_pin, AUTH_PAIRING_PIN_LEN + 1);
    }
    if (out_ttl_sec != NULL) {
        *out_ttl_sec = AUTH_PAIRING_WINDOW_SEC;
    }

#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "======================================================");
    ESP_LOGI(TAG, "PAIRING MODE ACTIVE (%ds) - Broadcasting on UDP :%d",
             AUTH_PAIRING_WINDOW_SEC, AUTH_DISCOVERY_UDP_PORT);
    ESP_LOGI(TAG, "======================================================");
    if (s_auth_mutex != NULL) {
        xSemaphoreGive(s_auth_mutex);
    }
    broadcast_pairing_beacon_once();
#endif
    return ESP_OK;
}

esp_err_t auth_manager_claim_pin(const char *pin,
                                 const char *client_name,
                                 char out_token[CONFIG_STORE_TOKEN_MAX_LEN],
                                 char out_topic_key[CONFIG_STORE_TOPIC_KEY_MAX_LEN]) {
    if (out_token == NULL || out_topic_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef ESP_PLATFORM
    if (s_auth_mutex != NULL) {
        xSemaphoreTake(s_auth_mutex, pdMS_TO_TICKS(200));
    }
#endif

    uint32_t now = get_now_ms();
    if (!s_window_open || (int32_t)(s_pin_expires_at_ms - now) <= 0) {
        s_window_open = false;
        memset(s_active_pin, 0, sizeof(s_active_pin));
#ifdef ESP_PLATFORM
        if (s_auth_mutex != NULL) xSemaphoreGive(s_auth_mutex);
#endif
        return ESP_ERR_INVALID_STATE;
    }

    // If caller supplied a non-empty PIN, verify it; if NULL or empty string, allow
    // code-free push-button claim while the 60s pairing window is open.
    if (pin != NULL && pin[0] != '\0') {
        if (!constant_time_equals(pin, s_active_pin, false)) {
#ifdef ESP_PLATFORM
            ESP_LOGW(TAG, "Rejected pairing claim with invalid PIN");
            if (s_auth_mutex != NULL) xSemaphoreGive(s_auth_mutex);
#endif
            return ESP_ERR_INVALID_ARG;
        }
    }

    // Single-claim lockout: immediately close pairing window and issue credentials
    s_window_open = false;
    memset(s_active_pin, 0, sizeof(s_active_pin));

    char rand_token_hex[57];
    generate_hex_bytes(rand_token_hex, 28);
    snprintf(out_token, CONFIG_STORE_TOKEN_MAX_LEN, "bl_%s", rand_token_hex);

    char rand_tk_hex[17];
    generate_hex_bytes(rand_tk_hex, 8);
    snprintf(out_topic_key, CONFIG_STORE_TOPIC_KEY_MAX_LEN, "tk_%s", rand_tk_hex);

    char unique_cname[CONFIG_STORE_CLIENT_NAME_MAX_LEN] = {0};
    format_unique_client_name(client_name, unique_cname);

#ifdef ESP_PLATFORM
    config_store_save_pairing(out_token, out_topic_key, unique_cname);
    if (s_auth_mutex != NULL) {
        xSemaphoreGive(s_auth_mutex);
    }
#else
    strncpy(s_test_api_token, out_token, sizeof(s_test_api_token) - 1);
    strncpy(s_test_topic_key, out_topic_key, sizeof(s_test_topic_key) - 1);
    strncpy(s_test_client_name, unique_cname, sizeof(s_test_client_name) - 1);
    s_test_paired = true;

    int free_idx = -1;
    for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_CLIENTS; i++) {
        if (!s_test_clients[i].active) {
            free_idx = i;
            break;
        }
    }
    if (free_idx < 0) {
        for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_CLIENTS - 1; i++) {
            s_test_clients[i] = s_test_clients[i + 1];
        }
        free_idx = CONFIG_STORE_MAX_PAIRED_CLIENTS - 1;
    }
    memset(&s_test_clients[free_idx], 0, sizeof(paired_client_t));
    s_test_clients[free_idx].active = true;
    strncpy(s_test_clients[free_idx].client_name, unique_cname, sizeof(s_test_clients[free_idx].client_name) - 1);
    strncpy(s_test_clients[free_idx].api_token, out_token, sizeof(s_test_clients[free_idx].api_token) - 1);
    strncpy(s_test_clients[free_idx].mqtt_topic_key, out_topic_key, sizeof(s_test_clients[free_idx].mqtt_topic_key) - 1);
#endif

    return ESP_OK;
}

esp_err_t auth_manager_get_status(bool *out_window_open,
                                  char out_pin[AUTH_PAIRING_PIN_LEN + 1],
                                  uint32_t *out_remaining_sec,
                                  bool *out_client_paired,
                                  char out_client_name[CONFIG_STORE_CLIENT_NAME_MAX_LEN]) {
#ifdef ESP_PLATFORM
    if (s_auth_mutex != NULL) {
        xSemaphoreTake(s_auth_mutex, pdMS_TO_TICKS(200));
    }
#endif

    uint32_t now = get_now_ms();
    if (s_window_open && (int32_t)(s_pin_expires_at_ms - now) <= 0) {
        s_window_open = false;
        memset(s_active_pin, 0, sizeof(s_active_pin));
    }

    if (out_window_open != NULL) {
        *out_window_open = s_window_open;
    }
    if (out_pin != NULL) {
        if (s_window_open) {
            memcpy(out_pin, s_active_pin, AUTH_PAIRING_PIN_LEN + 1);
        } else {
            out_pin[0] = '\0';
        }
    }
    if (out_remaining_sec != NULL) {
        *out_remaining_sec = s_window_open ? (uint32_t)((s_pin_expires_at_ms - now) / 1000UL) : 0;
    }

#ifdef ESP_PLATFORM
    if (s_auth_mutex != NULL) {
        xSemaphoreGive(s_auth_mutex);
    }
#endif

    get_active_credentials(NULL, NULL, out_client_name, out_client_paired);
    return ESP_OK;
}

bool auth_manager_verify_token(const char *candidate_token) {
    if (candidate_token == NULL || candidate_token[0] == '\0') {
        return false;
    }

    // Strip optional "Bearer " prefix
    if (strncmp(candidate_token, "Bearer ", 7) == 0 || strncmp(candidate_token, "bearer ", 7) == 0) {
        candidate_token += 7;
        while (*candidate_token == ' ') {
            candidate_token++;
        }
    }

    paired_client_t clients[CONFIG_STORE_MAX_PAIRED_CLIENTS];
    uint8_t count = 0;
    auth_manager_get_paired_clients(clients, &count);
    for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_CLIENTS; i++) {
        if (clients[i].active && clients[i].api_token[0] != '\0') {
            if (constant_time_equals(candidate_token, clients[i].api_token, false)) {
                return true;
            }
        }
    }

    char active_token[CONFIG_STORE_TOKEN_MAX_LEN] = {0};
    get_active_credentials(active_token, NULL, NULL, NULL);
    if (active_token[0] == '\0') {
        return false;
    }

    return constant_time_equals(candidate_token, active_token, false);
}

bool auth_manager_verify_mqtt_command(const char *req_id,
                                      const char *action,
                                      uint32_t timestamp,
                                      const char *signature_hex,
                                      const char *plain_token) {
    char active_token[CONFIG_STORE_TOKEN_MAX_LEN] = {0};
    bool is_paired = false;
    get_active_credentials(active_token, NULL, NULL, &is_paired);

    paired_client_t clients[CONFIG_STORE_MAX_PAIRED_CLIENTS];
    uint8_t count = 0;
    auth_manager_get_paired_clients(clients, &count);
    if (count > 0) {
        is_paired = true;
    }

    // 1. Check HMAC-SHA256 signature if provided
    if (signature_hex != NULL && signature_hex[0] != '\0') {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s:%s:%lu",
                 req_id ? req_id : "",
                 action ? action : "",
                 (unsigned long)timestamp);

        for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_CLIENTS; i++) {
            if (clients[i].active && clients[i].api_token[0] != '\0') {
                char expected_hex[AUTH_HMAC_HEX_LEN + 1];
                if (auth_manager_compute_hmac_sha256_hex(clients[i].api_token, msg, expected_hex) == ESP_OK) {
                    if (constant_time_equals(signature_hex, expected_hex, true)) {
                        return true;
                    }
                }
            }
        }

        if (active_token[0] != '\0') {
            char expected_hex[AUTH_HMAC_HEX_LEN + 1];
            if (auth_manager_compute_hmac_sha256_hex(active_token, msg, expected_hex) == ESP_OK) {
                if (constant_time_equals(signature_hex, expected_hex, true)) {
                    return true;
                }
            }
        }
        return false;
    }

    // 2. Check fallback plain token if provided
    if (plain_token != NULL && plain_token[0] != '\0') {
        return auth_manager_verify_token(plain_token);
    }

    // 3. If an external client has explicitly paired, require authentication on MQTT commands
    if (is_paired) {
        return false;
    }

    // Before first external pairing, allow local broker commands for backward compatibility
    return true;
}

#ifdef ESP_PLATFORM
// Background task on Core 1 polling GPIO 0 (BOOT button) for 3-second hold and broadcasting UDP/MQTT pairing beacons
#define BOOT_BUTTON_GPIO    GPIO_NUM_0

static void boot_button_monitor_task(void *pvParameters) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    uint32_t hold_ms = 0;
    uint32_t beacon_elapsed_ms = 0;
    bool triggered = false;

    while (1) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            hold_ms += 100;
            if (hold_ms >= AUTH_BOOT_BTN_HOLD_MS && !triggered) {
                triggered = true;
                ESP_LOGI(TAG, "BOOT button held for 3s -> activating 60s Pairing Broadcast Mode!");
                auth_manager_start_pairing(NULL, NULL);
                beacon_elapsed_ms = 0;
            }
        } else {
            hold_ms = 0;
            triggered = false;
        }

        beacon_elapsed_ms += 100;
        if (beacon_elapsed_ms >= AUTH_DISCOVERY_INTERVAL_MS) {
            beacon_elapsed_ms = 0;
            if (s_window_open) {
                broadcast_pairing_beacon_once();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif

esp_err_t auth_manager_init(void) {
#ifdef ESP_PLATFORM
    if (s_auth_mutex == NULL) {
        s_auth_mutex = xSemaphoreCreateMutex();
        if (s_auth_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xTaskCreatePinnedToCore(
        boot_button_monitor_task,
        "boot_btn_pair",
        4096,
        NULL,
        4,
        NULL,
        APP_PROCESSING_TASK_CORE
    );
#endif
    return ESP_OK;
}
