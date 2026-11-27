/**
 * @file ota_manager.c
 * @brief BrewLink Dual-OTA Firmware Update, Release Verification, and GitHub Release Checker
 */

#include "ota_manager.h"
#include "ota_signing_key.h"
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <errno.h>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "g30_controller.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"

static const char *TAG = "OTA_MGR";
#endif

// =============================================================================
// Little-Endian Binary & SHA-256 Helpers
// =============================================================================

static inline uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void copy_cstr_bounded(char *dst, size_t dst_len, const uint8_t *src, size_t max_src) {
    if (!dst || dst_len == 0) {
        return;
    }
    size_t n = 0;
    while (n < max_src && n + 1 < dst_len && src[n] != '\0') {
        dst[n] = (char)src[n];
        n++;
    }
    dst[n] = '\0';
}

// Minimal RFC 6234 SHA-256 implementation (shared cleanly across host tests and ESP32)
static inline uint32_t ota_rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32U - n));
}

static void ota_sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };

    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ota_rotr32(w[i - 15], 7) ^ ota_rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ota_rotr32(w[i - 2], 17) ^ ota_rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ota_rotr32(e, 6) ^ ota_rotr32(e, 11) ^ ota_rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = ota_rotr32(a, 2) ^ ota_rotr32(a, 13) ^ ota_rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

typedef struct {
    uint32_t state[8];
    uint64_t total_len;
    uint8_t block[64];
    size_t block_len;
} ota_sha256_ctx_t;

static void ota_sha256_init(ota_sha256_ctx_t *ctx) {
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
    ctx->total_len = 0;
    ctx->block_len = 0;
}

static void ota_sha256_update(ota_sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    if (!ctx || !data || len == 0) {
        return;
    }
    ctx->total_len += (uint64_t)len;
    size_t off = 0;
    if (ctx->block_len > 0) {
        size_t take = 64U - ctx->block_len;
        if (take > len) {
            take = len;
        }
        memcpy(ctx->block + ctx->block_len, data, take);
        ctx->block_len += take;
        off += take;
        if (ctx->block_len == 64U) {
            ota_sha256_transform(ctx->state, ctx->block);
            ctx->block_len = 0;
        }
    }
    while (off + 64U <= len) {
        ota_sha256_transform(ctx->state, data + off);
        off += 64U;
    }
    if (off < len) {
        ctx->block_len = len - off;
        memcpy(ctx->block, data + off, ctx->block_len);
    }
}

static void ota_sha256_final(ota_sha256_ctx_t *ctx, uint8_t out_digest[32]) {
    uint8_t pad[64];
    memset(pad, 0, sizeof(pad));
    if (ctx->block_len > 0) {
        memcpy(pad, ctx->block, ctx->block_len);
    }
    size_t rem = ctx->block_len;
    pad[rem++] = 0x80U;
    if (rem > 56U) {
        ota_sha256_transform(ctx->state, pad);
        memset(pad, 0, sizeof(pad));
    }
    uint64_t bit_len = ctx->total_len * 8ULL;
    for (int i = 0; i < 8; i++) {
        pad[56 + i] = (uint8_t)(bit_len >> ((7 - i) * 8));
    }
    ota_sha256_transform(ctx->state, pad);
    for (int i = 0; i < 8; i++) {
        out_digest[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out_digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out_digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out_digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static void ota_sha256_to_hex(const uint8_t digest[32], char out_hex[OTA_SHA256_HEX_LEN + 1]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2]     = hex[(digest[i] >> 4) & 0x0FU];
        out_hex[i * 2 + 1] = hex[digest[i] & 0x0FU];
    }
    out_hex[OTA_SHA256_HEX_LEN] = '\0';
}

static bool ota_const_time_eq_hex(const char *a, const char *b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(tolower((unsigned char)a[i]) ^ tolower((unsigned char)b[i]));
    }
    return diff == 0;
}

static void ota_compute_sha256(const uint8_t *data, size_t len, uint8_t out_digest[32]) {
    ota_sha256_ctx_t ctx;
    ota_sha256_init(&ctx);
    ota_sha256_update(&ctx, data, len);
    ota_sha256_final(&ctx, out_digest);
}

bool ota_manager_is_signature_required(void) {
    return (BREWLINK_OTA_SIGNING_PUBKEY_PEM[0] != '\0');
}

bool ota_validate_github_url(const char *url, bool is_api_url) {
    if (!url || url[0] == '\0') {
        return false;
    }
    size_t len = strlen(url);
    if (len >= OTA_URL_MAX_LEN || strncmp(url, "https://", 8) != 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 0x20U || c >= 0x7FU || c == '@' || c == '\\') {
            return false;
        }
    }
    if (strstr(url + 8, "..") != NULL) {
        return false;
    }

    const char *host_start = url + 8;
    const char *slash = strchr(host_start, '/');
    if (!slash || slash == host_start) {
        return false;
    }
    size_t host_len = (size_t)(slash - host_start);
    for (size_t i = 0; i < host_len; i++) {
        if (host_start[i] == ':') {
            return false; // Reject non-default ports
        }
    }
    const char *path = slash;

    if (is_api_url) {
        if (host_len != 14U || strncmp(host_start, "api.github.com", 14) != 0) {
            return false;
        }
        const char *expected_prefix = "/repos/" BREWLINK_GITHUB_REPO "/";
        return strncmp(path, expected_prefix, strlen(expected_prefix)) == 0;
    }

    if (host_len == 10U && strncmp(host_start, "github.com", 10) == 0) {
        const char *expected_prefix = "/" BREWLINK_GITHUB_REPO "/";
        return strncmp(path, expected_prefix, strlen(expected_prefix)) == 0;
    }

    if ((host_len == 29U && strncmp(host_start, "objects.githubusercontent.com", 29) == 0) ||
        (host_len == 36U && strncmp(host_start, "release-assets.githubusercontent.com", 36) == 0) ||
        (host_len == 37U && strncmp(host_start, "github-releases.githubusercontent.com", 37) == 0)) {
        return path[1] != '\0';
    }

    return false;
}

esp_err_t ota_parse_sha256sums(const char *sums_text, const char *asset_name,
                               char out_hex[OTA_SHA256_HEX_LEN + 1]) {
    if (!sums_text || !asset_name || asset_name[0] == '\0' || !out_hex) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *line = sums_text;
    while (*line != '\0') {
        while (*line == '\r' || *line == '\n' || *line == ' ' || *line == '\t') {
            line++;
        }
        if (*line == '\0') {
            break;
        }
        const char *eol = line;
        while (*eol != '\0' && *eol != '\r' && *eol != '\n') {
            eol++;
        }
        size_t line_len = (size_t)(eol - line);
        if (line_len > OTA_SHA256_HEX_LEN + 1U) {
            bool hex_ok = true;
            for (size_t i = 0; i < OTA_SHA256_HEX_LEN; i++) {
                if (!isxdigit((unsigned char)line[i])) {
                    hex_ok = false;
                    break;
                }
            }
            if (hex_ok && (line[OTA_SHA256_HEX_LEN] == ' ' || line[OTA_SHA256_HEX_LEN] == '\t')) {
                const char *fn = line + OTA_SHA256_HEX_LEN;
                while (fn < eol && (*fn == ' ' || *fn == '\t' || *fn == '*')) {
                    fn++;
                }
                const char *fn_end = eol;
                while (fn_end > fn && (fn_end[-1] == ' ' || fn_end[-1] == '\t')) {
                    fn_end--;
                }
                size_t fn_len = (size_t)(fn_end - fn);
                if (fn_len == strlen(asset_name) && strncmp(fn, asset_name, fn_len) == 0) {
                    for (size_t i = 0; i < OTA_SHA256_HEX_LEN; i++) {
                        out_hex[i] = (char)tolower((unsigned char)line[i]);
                    }
                    out_hex[OTA_SHA256_HEX_LEN] = '\0';
                    return ESP_OK;
                }
            }
        }
        line = eol;
    }
    out_hex[0] = '\0';
    return ESP_ERR_NOT_FOUND;
}

// =============================================================================
// SemVer Parsing & Comparison
// =============================================================================

typedef struct {
    unsigned long major;
    unsigned long minor;
    unsigned long patch;
    char prerelease[OTA_VERSION_MAX_LEN];
} parsed_semver_t;

esp_err_t ota_normalize_version(const char *raw_version, char *out_buf, size_t out_len) {
    if (!raw_version || !out_buf || out_len < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *p = raw_version;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == 'v' || *p == 'V') {
        p++;
    }

    size_t idx = 0;
    while (*p != '\0' && !isspace((unsigned char)*p) && idx + 1 < out_len) {
        out_buf[idx++] = *p++;
    }
    out_buf[idx] = '\0';

    if (idx == 0 || !isdigit((unsigned char)out_buf[0])) {
        out_buf[0] = '\0';
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static bool parse_semver(const char *ver_str, parsed_semver_t *out) {
    memset(out, 0, sizeof(*out));
    char norm[OTA_VERSION_MAX_LEN];
    if (ota_normalize_version(ver_str, norm, sizeof(norm)) != ESP_OK) {
        return false;
    }

    char *endptr = NULL;
    out->major = strtoul(norm, &endptr, 10);
    if (endptr == norm) {
        return false;
    }

    if (*endptr == '.') {
        const char *p = endptr + 1;
        out->minor = strtoul(p, &endptr, 10);
    }
    if (*endptr == '.') {
        const char *p = endptr + 1;
        out->patch = strtoul(p, &endptr, 10);
    }

    // Strip build metadata ('+...')
    char *plus = strchr(endptr, '+');
    if (plus) {
        *plus = '\0';
    }

    if (*endptr == '-') {
        snprintf(out->prerelease, sizeof(out->prerelease), "%s", endptr + 1);
    }
    return true;
}

static bool is_all_digits(const char *s, size_t len) {
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

static int compare_prerelease(const char *a, const char *b) {
    bool a_empty = (a[0] == '\0');
    bool b_empty = (b[0] == '\0');
    if (a_empty && b_empty) {
        return 0;
    }
    // Stable release (no prerelease) has higher precedence than prerelease
    if (a_empty) {
        return 1;
    }
    if (b_empty) {
        return -1;
    }

    const char *pa = a;
    const char *pb = b;
    while (*pa != '\0' && *pb != '\0') {
        const char *da = strchr(pa, '.');
        const char *db = strchr(pb, '.');
        size_t la = da ? (size_t)(da - pa) : strlen(pa);
        size_t lb = db ? (size_t)(db - pb) : strlen(pb);

        bool na = is_all_digits(pa, la);
        bool nb = is_all_digits(pb, lb);

        if (na && nb) {
            unsigned long va = strtoul(pa, NULL, 10);
            unsigned long vb = strtoul(pb, NULL, 10);
            if (va != vb) {
                return (va < vb) ? -1 : 1;
            }
        } else if (na != nb) {
            // Numeric identifiers always have lower precedence than non-numeric
            return na ? -1 : 1;
        } else {
            size_t min_l = (la < lb) ? la : lb;
            int cmp = strncmp(pa, pb, min_l);
            if (cmp != 0) {
                return (cmp < 0) ? -1 : 1;
            }
            if (la != lb) {
                return (la < lb) ? -1 : 1;
            }
        }

        pa = da ? (da + 1) : (pa + la);
        pb = db ? (db + 1) : (pb + lb);
    }

    if (*pa == '\0' && *pb == '\0') {
        return 0;
    }
    return (*pa == '\0') ? -1 : 1;
}

int ota_compare_versions(const char *ver_a, const char *ver_b) {
    parsed_semver_t a;
    parsed_semver_t b;
    bool ok_a = parse_semver(ver_a ? ver_a : "", &a);
    bool ok_b = parse_semver(ver_b ? ver_b : "", &b);

    if (!ok_a && !ok_b) {
        return 0;
    }
    if (!ok_a) {
        return -1;
    }
    if (!ok_b) {
        return 1;
    }

    if (a.major != b.major) {
        return (a.major < b.major) ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return (a.minor < b.minor) ? -1 : 1;
    }
    if (a.patch != b.patch) {
        return (a.patch < b.patch) ? -1 : 1;
    }
    return compare_prerelease(a.prerelease, b.prerelease);
}

bool ota_is_newer_version(const char *candidate_ver, const char *current_ver) {
    return ota_compare_versions(candidate_ver, current_ver) > 0;
}

const char *ota_file_format_to_str(ota_file_format_t format) {
    switch (format) {
        case OTA_FORMAT_FIRMWARE_BIN:   return "firmware.bin";
        case OTA_FORMAT_FACTORY_BIN:    return "factory.bin";
        case OTA_FORMAT_ELF:            return "firmware.elf";
        case OTA_FORMAT_ZIP:            return "zip";
        case OTA_FORMAT_BOOTLOADER_BIN: return "bootloader.bin";
        case OTA_FORMAT_PARTITIONS_BIN: return "partitions.bin";
        case OTA_FORMAT_OTHER_ESP_IMAGE:return "other_esp_image";
        default:                        return "unknown";
    }
}

const char *ota_state_to_str(ota_state_t state) {
    switch (state) {
        case OTA_STATE_IDLE:        return "idle";
        case OTA_STATE_CHECKING:    return "checking";
        case OTA_STATE_DOWNLOADING: return "downloading";
        case OTA_STATE_UPLOADING:   return "uploading";
        case OTA_STATE_VERIFYING:   return "verifying";
        case OTA_STATE_REBOOTING:   return "rebooting";
        case OTA_STATE_ERROR:       return "error";
        default:                    return "unknown";
    }
}

// =============================================================================
// ESP-IDF Image & Release Format Verification
// =============================================================================

static esp_err_t parse_app_header_at(const uint8_t *app, size_t app_len,
                                     size_t app_offset, ota_file_format_t fmt,
                                     ota_image_info_t *out_info) {
    if (app_len < OTA_MIN_IMAGE_HEADER_SIZE) {
        snprintf(out_info->error, sizeof(out_info->error),
                 "Image header truncated (%lu bytes, need >= %u)",
                 (unsigned long)app_len, (unsigned)OTA_MIN_IMAGE_HEADER_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    if (app[0] != OTA_ESP_IMAGE_MAGIC) {
        snprintf(out_info->error, sizeof(out_info->error),
                 "Invalid ESP-IDF image magic 0x%02x (expected 0xE9)", app[0]);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t desc_magic = read_u32_le(app + 32);
    if (desc_magic != OTA_ESP_APP_DESC_MAGIC) {
        if (app_offset == 0) {
            out_info->format = OTA_FORMAT_BOOTLOADER_BIN;
            snprintf(out_info->error, sizeof(out_info->error),
                     "Bootloader image detected (-bootloader.bin); only application firmware can be updated via OTA");
            return ESP_ERR_NOT_SUPPORTED;
        }
        snprintf(out_info->error, sizeof(out_info->error),
                 "Factory image missing esp_app_desc_t header at 0x%05x (magic=0x%08x)",
                 (unsigned)app_offset, (unsigned)desc_magic);
        return ESP_ERR_INVALID_ARG;
    }

    out_info->segment_count = app[1];
    out_info->chip_id = read_u16_le(app + 12);
    out_info->hash_appended = (app[23] == 1U);
    out_info->app_offset = app_offset;
    out_info->app_size = app_len;

    const uint8_t *desc = app + 32;
    copy_cstr_bounded(out_info->version, sizeof(out_info->version), desc + 16, 32);
    copy_cstr_bounded(out_info->project_name, sizeof(out_info->project_name), desc + 48, 32);
    copy_cstr_bounded(out_info->build_time, sizeof(out_info->build_time), desc + 80, 16);
    copy_cstr_bounded(out_info->build_date, sizeof(out_info->build_date), desc + 96, 16);
    copy_cstr_bounded(out_info->idf_ver, sizeof(out_info->idf_ver), desc + 112, 32);

    if (out_info->segment_count == 0 || out_info->segment_count > 16) {
        snprintf(out_info->error, sizeof(out_info->error),
                 "Invalid ESP image segment count (%u)", out_info->segment_count);
        return ESP_ERR_INVALID_ARG;
    }

    if (out_info->chip_id != OTA_CHIP_ID_ESP32) {
        out_info->format = OTA_FORMAT_OTHER_ESP_IMAGE;
        snprintf(out_info->error, sizeof(out_info->error),
                 "Wrong ESP chip target (chip_id=0x%04x, expected ESP32 0x0000)",
                 (unsigned)out_info->chip_id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (strcmp(out_info->project_name, OTA_EXPECTED_PROJECT_NAME) != 0) {
        out_info->format = OTA_FORMAT_OTHER_ESP_IMAGE;
        snprintf(out_info->error, sizeof(out_info->error),
                 "Firmware belongs to '%s', expected '%s'",
                 out_info->project_name[0] ? out_info->project_name : "(unknown)",
                 OTA_EXPECTED_PROJECT_NAME);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!out_info->hash_appended) {
        snprintf(out_info->error, sizeof(out_info->error),
                 "Firmware image missing mandatory trailing SHA-256 hash");
        return ESP_ERR_NOT_SUPPORTED;
    }

    out_info->format = fmt;
    out_info->valid = true;
    out_info->error[0] = '\0';
    return ESP_OK;
}

esp_err_t ota_verify_firmware_header(const uint8_t *data, size_t len, ota_image_info_t *out_info) {
    if (!data || !out_info || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->chip_id = 0xFFFFU;

    // 1. Reject ELF executables (-firmware.elf)
    if (len >= 4 && data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        out_info->format = OTA_FORMAT_ELF;
        snprintf(out_info->error, sizeof(out_info->error),
                 "ELF executable (-firmware.elf) is for debugging only; upload -firmware.bin, -factory.bin, or .zip");
        return ESP_ERR_NOT_SUPPORTED;
    }

    // 2. Reject raw ZIP bundles (must be extracted first)
    if (len >= 4 && data[0] == 'P' && data[1] == 'K' &&
        (data[2] == 0x03 || data[2] == 0x05) && (data[3] == 0x04 || data[3] == 0x06)) {
        out_info->format = OTA_FORMAT_ZIP;
        snprintf(out_info->error, sizeof(out_info->error),
                 "ZIP bundle detected; extract *-firmware.bin before sending to ESP32");
        return ESP_ERR_NOT_SUPPORTED;
    }

    // 3. Reject ESP-IDF partition table binary (-partitions.bin, entry magic 0x50AA LE)
    if (len >= 32 && data[0] == 0xAA && data[1] == 0x50) {
        out_info->format = OTA_FORMAT_PARTITIONS_BIN;
        snprintf(out_info->error, sizeof(out_info->error),
                 "Partition table binary (-partitions.bin) cannot be flashed via OTA");
        return ESP_ERR_NOT_SUPPORTED;
    }

    // 4. Standalone ESP32 image at offset 0 (-firmware.bin or -bootloader.bin)
    if (data[0] == OTA_ESP_IMAGE_MAGIC) {
        return parse_app_header_at(data, len, 0, OTA_FORMAT_FIRMWARE_BIN, out_info);
    }

    // 5. Merged 4MB factory image (-factory.bin): 0xFF padding at 0x0, bootloader @ 0x1000, app @ 0x10000
    bool has_ff_prefix = true;
    size_t check_ff = (len < 16U) ? len : 16U;
    for (size_t i = 0; i < check_ff; i++) {
        if (data[i] != 0xFFU) {
            has_ff_prefix = false;
            break;
        }
    }

    if (has_ff_prefix || (len > OTA_FACTORY_BOOTLOADER_OFF && data[OTA_FACTORY_BOOTLOADER_OFF] == OTA_ESP_IMAGE_MAGIC)) {
        if (len < OTA_FACTORY_APP_OFFSET + OTA_MIN_IMAGE_HEADER_SIZE) {
            snprintf(out_info->error, sizeof(out_info->error),
                     "Merged factory image truncated before app offset 0x10000");
            return ESP_ERR_INVALID_SIZE;
        }
        return parse_app_header_at(data + OTA_FACTORY_APP_OFFSET,
                                   len - OTA_FACTORY_APP_OFFSET,
                                   OTA_FACTORY_APP_OFFSET,
                                   OTA_FORMAT_FACTORY_BIN,
                                   out_info);
    }

    out_info->format = OTA_FORMAT_UNKNOWN;
    snprintf(out_info->error, sizeof(out_info->error),
             "Unrecognized file format (expected BrewLink -firmware.bin, -factory.bin, or .zip)");
    return ESP_ERR_INVALID_ARG;
}

esp_err_t ota_verify_full_image_integrity(const uint8_t *data, size_t len, ota_image_info_t *out_info) {
    esp_err_t err = ota_verify_firmware_header(data, len, out_info);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t *app = data + out_info->app_offset;
    size_t buf_len = len - out_info->app_offset;
    size_t pos = 24U;
    uint8_t xor_sum = 0xEFU;

    for (uint8_t i = 0; i < out_info->segment_count; i++) {
        if (pos + 8U > buf_len) {
            out_info->valid = false;
            snprintf(out_info->error, sizeof(out_info->error),
                     "Truncated segment header #%u", (unsigned)i);
            return ESP_ERR_INVALID_SIZE;
        }
        uint32_t seg_len = read_u32_le(app + pos + 4U);
        pos += 8U;
        if (seg_len > buf_len - pos) {
            out_info->valid = false;
            snprintf(out_info->error, sizeof(out_info->error),
                     "Truncated segment payload #%u (%lu bytes)", (unsigned)i, (unsigned long)seg_len);
            return ESP_ERR_INVALID_SIZE;
        }
        for (uint32_t j = 0; j < seg_len; j++) {
            xor_sum ^= app[pos + j];
        }
        pos += seg_len;
    }

    size_t checksum_idx = ((pos + 16U) & ~(size_t)15U) - 1U;
    if (checksum_idx >= buf_len) {
        out_info->valid = false;
        snprintf(out_info->error, sizeof(out_info->error),
                 "Image truncated before XOR checksum byte");
        return ESP_ERR_INVALID_SIZE;
    }

    if (app[checksum_idx] != xor_sum) {
        out_info->valid = false;
        snprintf(out_info->error, sizeof(out_info->error),
                 "Image XOR checksum mismatch (got 0x%02x, expected 0x%02x)",
                 app[checksum_idx], xor_sum);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t img_end = checksum_idx + 1U;
    if (out_info->hash_appended) {
        if (img_end + 32U > buf_len) {
            out_info->valid = false;
            snprintf(out_info->error, sizeof(out_info->error),
                     "Image truncated before trailing SHA-256 hash");
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t calc_sha[32];
        ota_compute_sha256(app, img_end, calc_sha);
        if (memcmp(calc_sha, app + img_end, 32U) != 0) {
            out_info->valid = false;
            snprintf(out_info->error, sizeof(out_info->error),
                     "Image trailing SHA-256 checksum mismatch");
            return ESP_ERR_INVALID_RESPONSE;
        }
        img_end += 32U;
    }

    if (img_end > OTA_MAX_IMAGE_SIZE) {
        out_info->valid = false;
        snprintf(out_info->error, sizeof(out_info->error),
                 "Application image (%lu bytes) exceeds %u-byte OTA partition",
                 (unsigned long)img_end, (unsigned)OTA_MAX_IMAGE_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    out_info->app_size = img_end;
    out_info->valid = true;
    out_info->error[0] = '\0';
    return ESP_OK;
}

// =============================================================================
// Portable Zero-Malloc GitHub Releases JSON Parser
// =============================================================================

static const char *json_skip_ws(const char *p) {
    while (p && *p != '\0' && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    return p;
}

static const char *json_read_string(const char *p, char *out_buf, size_t out_len) {
    if (!p || *p != '"') {
        return NULL;
    }
    p++;
    size_t idx = 0;
    while (*p != '\0' && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            if (*p == '\0') {
                return NULL;
            }
            char esc = *p++;
            switch (esc) {
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'u':
                    for (int k = 0; k < 4; k++) {
                        if (!isxdigit((unsigned char)*p)) {
                            return NULL;
                        }
                        p++;
                    }
                    c = ' ';
                    break;
                default: c = esc; break;
            }
        }
        if (out_buf && idx + 1 < out_len) {
            out_buf[idx++] = c;
        }
    }
    if (out_buf && out_len > 0) {
        out_buf[idx] = '\0';
    }
    if (*p != '"') {
        return NULL;
    }
    return p + 1;
}

static const char *json_skip_value(const char *p) {
    p = json_skip_ws(p);
    if (!p || *p == '\0') {
        return NULL;
    }
    if (*p == '"') {
        return json_read_string(p, NULL, 0);
    }
    if (*p == '{' || *p == '[') {
        char open_c = *p;
        char close_c = (open_c == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (*p != '\0' && depth > 0) {
            if (*p == '"') {
                p = json_read_string(p, NULL, 0);
                if (!p) {
                    return NULL;
                }
                continue;
            }
            if (*p == open_c) {
                depth++;
            } else if (*p == close_c) {
                depth--;
            }
            p++;
        }
        return (depth == 0) ? p : NULL;
    }

    // Primitive: number, true, false, null
    while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        p++;
    }
    return p;
}

static const char *json_read_bool(const char *p, bool *out_val) {
    p = json_skip_ws(p);
    if (strncmp(p, "true", 4) == 0) {
        *out_val = true;
        return p + 4;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out_val = false;
        return p + 5;
    }
    return json_skip_value(p);
}

static const char *json_read_size(const char *p, size_t *out_val) {
    p = json_skip_ws(p);
    if (!p || (!isdigit((unsigned char)*p))) {
        return json_skip_value(p);
    }
    char *endptr = NULL;
    unsigned long v = strtoul(p, &endptr, 10);
    *out_val = (size_t)v;
    return endptr;
}

static bool ends_with(const char *str, const char *suffix) {
    size_t ls = strlen(str);
    size_t lsf = strlen(suffix);
    if (ls < lsf) {
        return false;
    }
    return strcmp(str + ls - lsf, suffix) == 0;
}

static int score_asset_filename(const char *name) {
    if (!name || name[0] == '\0') {
        return 0;
    }
    if (ends_with(name, "-firmware.bin")) {
        return 100;
    }
    if (ends_with(name, "-factory.bin")) {
        return 50;
    }
    return 0;
}

static const char *parse_github_assets_array(const char *p, ota_release_info_t *rel) {
    p = json_skip_ws(p);
    if (!p || *p != '[') {
        return json_skip_value(p);
    }
    p++;
    int best_score = 0;
    char fw_sig_url[OTA_URL_MAX_LEN] = {0};
    char factory_sig_url[OTA_URL_MAX_LEN] = {0};

    while (*p != '\0') {
        p = json_skip_ws(p);
        if (*p == ']') {
            if (ends_with(rel->asset_name, "-firmware.bin") && fw_sig_url[0] != '\0') {
                snprintf(rel->sig_download_url, sizeof(rel->sig_download_url), "%s", fw_sig_url);
            } else if (ends_with(rel->asset_name, "-factory.bin") && factory_sig_url[0] != '\0') {
                snprintf(rel->sig_download_url, sizeof(rel->sig_download_url), "%s", factory_sig_url);
            }
            return p + 1;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '{') {
            p = json_skip_value(p);
            if (!p) {
                return NULL;
            }
            continue;
        }
        p++;

        char asset_name[OTA_ASSET_NAME_MAX_LEN] = {0};
        char asset_url[OTA_URL_MAX_LEN] = {0};
        size_t asset_size = 0;

        while (*p != '\0') {
            p = json_skip_ws(p);
            if (*p == '}') {
                p++;
                break;
            }
            if (*p == ',') {
                p++;
                continue;
            }
            char key[48] = {0};
            p = json_read_string(p, key, sizeof(key));
            if (!p) {
                return NULL;
            }
            p = json_skip_ws(p);
            if (*p != ':') {
                return NULL;
            }
            p = json_skip_ws(p + 1);

            if (strcmp(key, "name") == 0) {
                p = json_read_string(p, asset_name, sizeof(asset_name));
            } else if (strcmp(key, "browser_download_url") == 0) {
                p = json_read_string(p, asset_url, sizeof(asset_url));
            } else if (strcmp(key, "size") == 0) {
                p = json_read_size(p, &asset_size);
            } else {
                p = json_skip_value(p);
            }
            if (!p) {
                return NULL;
            }
        }

        if (asset_url[0] != '\0' && ota_validate_github_url(asset_url, false)) {
            int score = score_asset_filename(asset_name);
            if (score > best_score) {
                best_score = score;
                snprintf(rel->asset_name, sizeof(rel->asset_name), "%s", asset_name);
                snprintf(rel->download_url, sizeof(rel->download_url), "%s", asset_url);
                rel->asset_size = asset_size;
            } else if (ends_with(asset_name, "-firmware.bin.sig")) {
                snprintf(fw_sig_url, sizeof(fw_sig_url), "%s", asset_url);
            } else if (ends_with(asset_name, "-factory.bin.sig")) {
                snprintf(factory_sig_url, sizeof(factory_sig_url), "%s", asset_url);
            } else if (strcmp(asset_name, "SHA256SUMS.txt") == 0) {
                snprintf(rel->sha256sums_url, sizeof(rel->sha256sums_url), "%s", asset_url);
            }
        }
    }
    return NULL;
}

esp_err_t ota_parse_github_release_json(const char *json_str, const char *current_ver,
                                        ota_release_info_t *out_release) {
    if (!json_str || !out_release) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_release, 0, sizeof(*out_release));
    const char *p = json_skip_ws(json_str);
    if (!p || *p != '{') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    p++;

    bool saw_close = false;
    while (*p != '\0') {
        p = json_skip_ws(p);
        if (*p == '}') {
            saw_close = true;
            break;
        }
        if (*p == ',') {
            p++;
            continue;
        }

        char key[48] = {0};
        p = json_read_string(p, key, sizeof(key));
        if (!p) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        p = json_skip_ws(p);
        if (*p != ':') {
            return ESP_ERR_INVALID_RESPONSE;
        }
        p = json_skip_ws(p + 1);

        if (strcmp(key, "tag_name") == 0) {
            p = json_read_string(p, out_release->tag_name, sizeof(out_release->tag_name));
        } else if (strcmp(key, "name") == 0) {
            p = json_read_string(p, out_release->release_name, sizeof(out_release->release_name));
        } else if (strcmp(key, "published_at") == 0) {
            p = json_read_string(p, out_release->published_at, sizeof(out_release->published_at));
        } else if (strcmp(key, "html_url") == 0) {
            p = json_read_string(p, out_release->html_url, sizeof(out_release->html_url));
        } else if (strcmp(key, "body") == 0) {
            p = json_read_string(p, out_release->notes, sizeof(out_release->notes));
        } else if (strcmp(key, "prerelease") == 0) {
            p = json_read_bool(p, &out_release->prerelease);
        } else if (strcmp(key, "assets") == 0) {
            p = parse_github_assets_array(p, out_release);
        } else {
            p = json_skip_value(p);
        }

        if (!p) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (!saw_close) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (out_release->tag_name[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    // Sanitize html_url to prevent javascript: or off-origin link injection
    if (out_release->html_url[0] != '\0' && !ota_validate_github_url(out_release->html_url, false)) {
        out_release->html_url[0] = '\0';
    }

    if (ota_normalize_version(out_release->tag_name, out_release->version,
                              sizeof(out_release->version)) != ESP_OK) {
        snprintf(out_release->version, sizeof(out_release->version), "%.*s",
                 (int)(sizeof(out_release->version) - 1U), out_release->tag_name);
    }

    out_release->available = (out_release->download_url[0] != '\0');
    out_release->update_available = ota_is_newer_version(out_release->version,
                                                          current_ver ? current_ver : BREWLINK_FIRMWARE_VERSION);
    return ESP_OK;
}

// =============================================================================
// Runtime ESP32 Dual-OTA Manager Implementation
// =============================================================================

#ifdef ESP_PLATFORM

#define OTA_MAX_RELEASE_JSON_BYTES      32768U
#define OTA_DEFAULT_RELEASE_JSON_BYTES  24576U
#define OTA_FALLBACK_RELEASE_JSON_BYTES 16384U
#define OTA_MAX_REDIRECTS               5
#define OTA_MAX_DOWNLOAD_BYTES          ((int64_t)(OTA_FACTORY_APP_OFFSET + OTA_MAX_IMAGE_SIZE + 65536U))
#define OTA_MAX_SUMS_BYTES              4096U

typedef struct {
    bool active;
    bool ota_begun;
    bool factory_mode;
    bool boot_magic_checked;
    bool part_magic_checked;
    bool allow_downgrade;
    bool has_expected_sha256;
    bool has_signature;
    size_t expected_total;
    size_t stream_pos;
    size_t app_written;
    size_t header_buf_len;
    uint8_t header_buf[OTA_MIN_IMAGE_HEADER_SIZE];
    ota_sha256_ctx_t file_sha_ctx;
    char expected_sha256[OTA_SHA256_HEX_LEN + 1];
    uint8_t signature[OTA_MAX_SIG_BYTES];
    size_t signature_len;
    esp_ota_handle_t ota_handle;
    const esp_partition_t *update_part;
} ota_upload_session_t;

typedef struct {
    char url[OTA_URL_MAX_LEN];
} ota_check_task_args_t;

typedef struct {
    char download_url[OTA_URL_MAX_LEN];
    char asset_name[OTA_ASSET_NAME_MAX_LEN];
    char sig_url[OTA_URL_MAX_LEN];
    char sums_url[OTA_URL_MAX_LEN];
    bool force_reboot;
    bool allow_downgrade;
} ota_download_task_args_t;

static SemaphoreHandle_t s_ota_mutex = NULL;
static SemaphoreHandle_t s_upload_mutex = NULL;
static esp_timer_handle_t s_boot_confirm_timer = NULL;
static esp_timer_handle_t s_reboot_fallback_timer = NULL;
static ota_status_t s_status;
static ota_upload_session_t s_upload;
static int64_t s_last_check_attempt_us = 0;

// Pre-upload options staged for manual HTTP uploads (consumed under s_upload_mutex)
static bool s_next_upload_has_sig = false;
static uint8_t s_next_upload_sig[OTA_MAX_SIG_BYTES];
static size_t s_next_upload_sig_len = 0;
static bool s_next_upload_allow_downgrade = false;

static void ota_lock(void) {
    if (s_ota_mutex) {
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    }
}

static void ota_unlock(void) {
    if (s_ota_mutex) {
        xSemaphoreGive(s_ota_mutex);
    }
}

static void upload_lock(void) {
    if (s_upload_mutex) {
        xSemaphoreTake(s_upload_mutex, portMAX_DELAY);
    }
}

static void upload_unlock(void) {
    if (s_upload_mutex) {
        xSemaphoreGive(s_upload_mutex);
    }
}

static void set_status_error_locked(const char *msg) {
    s_status.state = OTA_STATE_ERROR;
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s", msg ? msg : "Unknown OTA error");
    snprintf(s_status.message, sizeof(s_status.message), "%s", s_status.last_error);
}

static void delayed_ota_reboot_task(void *pvParameters) {
    (void)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(1200));
    ESP_LOGI(TAG, "Rebooting into new firmware partition...");
    esp_restart();
}

static void reboot_fallback_timer_cb(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Fallback timer rebooting into new firmware partition...");
    esp_restart();
}

static void boot_confirm_timer_cb(void *arg) {
    (void)arg;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return;
    }
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "Confirmed newly booted OTA firmware v%s on '%s' after %ds stability window",
                     ota_manager_get_current_version(), running->label, OTA_BOOT_CONFIRM_DELAY_SEC);
        } else {
            ESP_LOGW(TAG, "Failed to confirm pending OTA partition '%s'", running->label);
        }
    }
}

static esp_err_t ota_verify_ecdsa_signature(const uint8_t digest[32],
                                            const uint8_t *sig, size_t sig_len) {
    if (!ota_manager_is_signature_required()) {
        return ESP_OK;
    }
    if (!sig || sig_len == 0 || sig_len > OTA_MAX_SIG_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    int ret = mbedtls_pk_parse_public_key(
        &pk,
        (const unsigned char *)BREWLINK_OTA_SIGNING_PUBKEY_PEM,
        strlen(BREWLINK_OTA_SIGNING_PUBKEY_PEM) + 1U
    );
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to parse embedded OTA signing public key (mbedtls=-0x%04x)", (unsigned)(-ret));
        mbedtls_pk_free(&pk);
        return ESP_FAIL;
    }

    ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, 32U, sig, sig_len);
    mbedtls_pk_free(&pk);
    if (ret != 0) {
        ESP_LOGE(TAG, "OTA ECDSA signature verification FAILED (mbedtls=-0x%04x)", (unsigned)(-ret));
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "OTA ECDSA-P256 firmware signature verified successfully");
    return ESP_OK;
}

static esp_err_t ota_open_validated_https(esp_http_client_handle_t client,
                                          bool is_api_url,
                                          int *out_status,
                                          int64_t *out_content_len,
                                          char *err_buf, size_t err_buf_len) {
    int redirects = 0;
    int status_code = 0;
    int64_t content_len = 0;
    esp_err_t err = ESP_OK;

    while (redirects <= OTA_MAX_REDIRECTS) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            if (err_buf && err_buf_len > 0) {
                snprintf(err_buf, err_buf_len, "HTTPS connection failed: %s", esp_err_to_name(err));
            }
            return err;
        }
        content_len = esp_http_client_fetch_headers(client);
        status_code = esp_http_client_get_status_code(client);

        if (status_code == 301 || status_code == 302 || status_code == 303 ||
            status_code == 307 || status_code == 308) {
            err = esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            if (err != ESP_OK) {
                if (err_buf && err_buf_len > 0) {
                    snprintf(err_buf, err_buf_len, "Redirect failed: %s", esp_err_to_name(err));
                }
                return err;
            }
            char redir_url[OTA_URL_MAX_LEN] = {0};
            if (esp_http_client_get_url(client, redir_url, sizeof(redir_url)) != ESP_OK ||
                !ota_validate_github_url(redir_url, is_api_url)) {
                ESP_LOGE(TAG, "Blocked untrusted OTA redirect URL");
                if (err_buf && err_buf_len > 0) {
                    snprintf(err_buf, err_buf_len, "Blocked untrusted or non-HTTPS redirect");
                }
                return ESP_ERR_NOT_SUPPORTED;
            }
            redirects++;
            continue;
        }
        break;
    }

    if (redirects > OTA_MAX_REDIRECTS) {
        esp_http_client_close(client);
        if (err_buf && err_buf_len > 0) {
            snprintf(err_buf, err_buf_len, "Too many HTTP redirects");
        }
        return ESP_FAIL;
    }

    if (out_status) *out_status = status_code;
    if (out_content_len) *out_content_len = content_len;
    return ESP_OK;
}

static esp_err_t ota_fetch_small_https_asset(const char *url, uint8_t *out_buf,
                                             size_t max_len, size_t *out_len) {
    if (!url || !out_buf || max_len == 0 || !out_len || !ota_validate_github_url(url, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "BrewLink-ESP32-OTA/" BREWLINK_FIRMWARE_VERSION,
        .disable_auto_redirect = true,
        .max_redirection_count = OTA_MAX_REDIRECTS,
        .buffer_size = 2048,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    int status_code = 0;
    int64_t content_len = 0;
    esp_err_t err = ota_open_validated_https(client, false, &status_code, &content_len, NULL, 0);
    if (err != ESP_OK || status_code != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    if (content_len > (int64_t)max_len) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t total = 0;
    int zero_reads = 0;
    while (total < max_len) {
        errno = 0;
        int r = esp_http_client_read(client, (char *)(out_buf + total), (int)(max_len - total));
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            if (content_len > 0 && total >= (size_t)content_len) {
                break;
            }
            int serr = esp_http_client_get_errno(client);
            if (serr == ECONNRESET || serr == ENOTCONN) {
                err = ESP_FAIL;
                break;
            }
            if (zero_reads++ > 10) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        zero_reads = 0;
        total += (size_t)r;
    }

    bool complete = esp_http_client_is_complete_data_received(client) ||
                    (content_len > 0 && total >= (size_t)content_len);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || !complete) {
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_SIZE;
    }
    *out_len = total;
    return ESP_OK;
}

static esp_err_t begin_flash_partition_with_header_locked(ota_image_info_t *out_info) {
    ota_image_info_t verified;
    ota_file_format_t expected_fmt = s_upload.factory_mode ? OTA_FORMAT_FACTORY_BIN : OTA_FORMAT_FIRMWARE_BIN;

    esp_err_t err = parse_app_header_at(s_upload.header_buf, OTA_MIN_IMAGE_HEADER_SIZE,
                                        s_upload.factory_mode ? OTA_FACTORY_APP_OFFSET : 0U,
                                        expected_fmt, &verified);
    if (out_info) {
        *out_info = verified;
    }
    if (err != ESP_OK) {
        return err;
    }

    // Anti-downgrade protection unless explicitly authorized
    const char *cur_ver = ota_manager_get_current_version();
    if (!s_upload.allow_downgrade && verified.version[0] != '\0' &&
        ota_compare_versions(verified.version, cur_ver) < 0) {
        snprintf(verified.error, sizeof(verified.error),
                 "Firmware downgrade blocked (v%s < running v%s)", verified.version, cur_ver);
        if (out_info) {
            *out_info = verified;
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    const esp_partition_t *next_part = esp_ota_get_next_update_partition(NULL);
    if (!next_part) {
        snprintf(verified.error, sizeof(verified.error), "No target OTA partition available (check partitions.csv)");
        if (out_info) {
            *out_info = verified;
        }
        return ESP_ERR_NOT_FOUND;
    }

    size_t expected_app_size = s_upload.expected_total;
    if (s_upload.factory_mode && expected_app_size > OTA_FACTORY_APP_OFFSET) {
        expected_app_size -= OTA_FACTORY_APP_OFFSET;
    }
    if (expected_app_size > next_part->size) {
        snprintf(verified.error, sizeof(verified.error),
                 "Image size (%lu B) exceeds partition '%s' (%lu B)",
                 (unsigned long)expected_app_size, next_part->label, (unsigned long)next_part->size);
        if (out_info) {
            *out_info = verified;
        }
        return ESP_ERR_INVALID_SIZE;
    }

    esp_ota_handle_t handle = 0;
    err = esp_ota_begin(next_part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        snprintf(verified.error, sizeof(verified.error), "esp_ota_begin failed: %s", esp_err_to_name(err));
        if (out_info) {
            *out_info = verified;
        }
        return err;
    }

    err = esp_ota_write(handle, s_upload.header_buf, OTA_MIN_IMAGE_HEADER_SIZE);
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        return err;
    }

    s_upload.ota_handle = handle;
    s_upload.update_part = next_part;
    s_upload.ota_begun = true;
    s_upload.app_written = OTA_MIN_IMAGE_HEADER_SIZE;

    ota_lock();
    snprintf(s_status.target_partition, sizeof(s_status.target_partition), "%s", next_part->label);
    snprintf(s_status.message, sizeof(s_status.message),
             "Verified %s v%s -> flashing %s...",
             ota_file_format_to_str(verified.format), verified.version, next_part->label);
    ota_unlock();

    ESP_LOGI(TAG, "OTA started (%s v%s -> partition '%s')",
             ota_file_format_to_str(verified.format), verified.version, next_part->label);
    return ESP_OK;
}

static esp_err_t process_upload_bytes_locked(const uint8_t *data, size_t len, ota_image_info_t *out_info) {
    ota_sha256_update(&s_upload.file_sha_ctx, data, len);

    size_t i = 0;
    while (i < len) {
        if (s_upload.factory_mode && s_upload.stream_pos < OTA_FACTORY_APP_OFFSET) {
            if (s_upload.stream_pos == OTA_FACTORY_BOOTLOADER_OFF && !s_upload.boot_magic_checked) {
                s_upload.boot_magic_checked = true;
                if (data[i] != OTA_ESP_IMAGE_MAGIC) {
                    ESP_LOGE(TAG, "Factory image missing bootloader 0xE9 magic at 0x1000 (got 0x%02x)", data[i]);
                    return ESP_ERR_INVALID_ARG;
                }
            }
            if (s_upload.stream_pos == OTA_FACTORY_PARTITIONS_OFF && !s_upload.part_magic_checked) {
                s_upload.part_magic_checked = true;
                if (data[i] != 0xAAU) {
                    ESP_LOGE(TAG, "Factory image missing partition table 0xAA magic at 0x8000 (got 0x%02x)", data[i]);
                    return ESP_ERR_INVALID_ARG;
                }
            }

            size_t to_skip = OTA_FACTORY_APP_OFFSET - s_upload.stream_pos;
            size_t avail = len - i;
            if (avail < to_skip) {
                if (s_upload.stream_pos < OTA_FACTORY_BOOTLOADER_OFF && s_upload.stream_pos + avail > OTA_FACTORY_BOOTLOADER_OFF) {
                    size_t step = OTA_FACTORY_BOOTLOADER_OFF - s_upload.stream_pos;
                    i += step;
                    s_upload.stream_pos += step;
                    continue;
                }
                if (s_upload.stream_pos < OTA_FACTORY_PARTITIONS_OFF && s_upload.stream_pos + avail > OTA_FACTORY_PARTITIONS_OFF) {
                    size_t step = OTA_FACTORY_PARTITIONS_OFF - s_upload.stream_pos;
                    i += step;
                    s_upload.stream_pos += step;
                    continue;
                }
                s_upload.stream_pos += avail;
                i += avail;
            } else {
                if (s_upload.stream_pos <= OTA_FACTORY_BOOTLOADER_OFF) {
                    size_t idx_boot = i + (OTA_FACTORY_BOOTLOADER_OFF - s_upload.stream_pos);
                    if (data[idx_boot] != OTA_ESP_IMAGE_MAGIC) {
                        return ESP_ERR_INVALID_ARG;
                    }
                    s_upload.boot_magic_checked = true;
                }
                if (s_upload.stream_pos <= OTA_FACTORY_PARTITIONS_OFF) {
                    size_t idx_part = i + (OTA_FACTORY_PARTITIONS_OFF - s_upload.stream_pos);
                    if (data[idx_part] != 0xAAU) {
                        return ESP_ERR_INVALID_ARG;
                    }
                    s_upload.part_magic_checked = true;
                }
                i += to_skip;
                s_upload.stream_pos += to_skip;
            }
            continue;
        }

        if (!s_upload.ota_begun) {
            size_t need = OTA_MIN_IMAGE_HEADER_SIZE - s_upload.header_buf_len;
            size_t copy_n = (len - i < need) ? (len - i) : need;
            memcpy(s_upload.header_buf + s_upload.header_buf_len, data + i, copy_n);
            s_upload.header_buf_len += copy_n;
            s_upload.stream_pos += copy_n;
            i += copy_n;

            if (s_upload.header_buf_len == OTA_MIN_IMAGE_HEADER_SIZE) {
                esp_err_t err = begin_flash_partition_with_header_locked(out_info);
                if (err != ESP_OK) {
                    return err;
                }
            }
            continue;
        }

        size_t chunk_len = len - i;
        if (!s_upload.update_part || chunk_len > s_upload.update_part->size ||
            s_upload.app_written > s_upload.update_part->size - chunk_len) {
            if (out_info) {
                snprintf(out_info->error, sizeof(out_info->error),
                         "Streamed firmware exceeds OTA partition size");
            }
            return ESP_ERR_INVALID_SIZE;
        }

        esp_err_t err = esp_ota_write(s_upload.ota_handle, data + i, chunk_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            return err;
        }
        s_upload.app_written += chunk_len;
        s_upload.stream_pos += chunk_len;
        i += chunk_len;
    }

    ota_lock();
    s_status.bytes_written = s_upload.app_written;
    if (s_upload.expected_total > 0) {
        size_t app_total = s_upload.factory_mode && s_upload.expected_total > OTA_FACTORY_APP_OFFSET
                           ? (s_upload.expected_total - OTA_FACTORY_APP_OFFSET)
                           : s_upload.expected_total;
        s_status.total_bytes = app_total;
        if (app_total > 0) {
            uint64_t pct = ((uint64_t)s_upload.app_written * 100ULL) / (uint64_t)app_total;
            s_status.progress_pct = (pct > 100ULL) ? 100U : (uint8_t)pct;
        }
    }
    ota_unlock();

    return ESP_OK;
}

esp_err_t ota_manager_init(void) {
    if (!s_ota_mutex) {
        s_ota_mutex = xSemaphoreCreateMutex();
        if (!s_ota_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_upload_mutex) {
        s_upload_mutex = xSemaphoreCreateMutex();
        if (!s_upload_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&s_status, 0, sizeof(s_status));
    memset(&s_upload, 0, sizeof(s_upload));
    s_status.signature_required = ota_manager_is_signature_required();

    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc && app_desc->version[0] != '\0') {
        snprintf(s_status.current_version, sizeof(s_status.current_version), "%s", app_desc->version);
    } else {
        snprintf(s_status.current_version, sizeof(s_status.current_version), "%s", BREWLINK_FIRMWARE_VERSION);
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        snprintf(s_status.running_partition, sizeof(s_status.running_partition), "%s", running->label);
    } else {
        snprintf(s_status.running_partition, sizeof(s_status.running_partition), "unknown");
    }

    const esp_partition_t *next_part = esp_ota_get_next_update_partition(NULL);
    if (next_part) {
        snprintf(s_status.target_partition, sizeof(s_status.target_partition), "%s", next_part->label);
    } else {
        snprintf(s_status.target_partition, sizeof(s_status.target_partition), "none");
    }

    s_status.state = OTA_STATE_IDLE;
    snprintf(s_status.message, sizeof(s_status.message), "Ready");
    ESP_LOGI(TAG, "OTA manager initialized: v%s running on '%s' (next target: '%s', sig_required=%s)",
             s_status.current_version, s_status.running_partition, s_status.target_partition,
             s_status.signature_required ? "yes" : "no");
    return ESP_OK;
}

esp_err_t ota_manager_schedule_boot_confirm(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return ESP_OK;
    }
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK ||
        ota_state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }

    if (!s_boot_confirm_timer) {
        const esp_timer_create_args_t args = {
            .callback = boot_confirm_timer_cb,
            .name = "ota_boot_ok",
        };
        esp_err_t err = esp_timer_create(&args, &s_boot_confirm_timer);
        if (err != ESP_OK) {
            return err;
        }
    }
    ESP_LOGI(TAG, "Pending OTA verification detected on '%s'; confirming in %ds if stable",
             running->label, OTA_BOOT_CONFIRM_DELAY_SEC);
    return esp_timer_start_once(s_boot_confirm_timer, (int64_t)OTA_BOOT_CONFIRM_DELAY_SEC * 1000000LL);
}

const char *ota_manager_get_current_version(void) {
    return s_status.current_version[0] ? s_status.current_version : BREWLINK_FIRMWARE_VERSION;
}

const char *ota_manager_get_running_partition(void) {
    return s_status.running_partition[0] ? s_status.running_partition : "unknown";
}

esp_err_t ota_manager_get_status(ota_status_t *out_status) {
    if (!out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    ota_lock();
    *out_status = s_status;
    ota_unlock();
    return ESP_OK;
}

static esp_err_t ota_perform_github_release_check(const char *releases_url,
                                                  ota_release_info_t *out_release,
                                                  bool state_already_checking) {
    const char *url = (releases_url && releases_url[0] != '\0')
                      ? releases_url
                      : BREWLINK_GITHUB_RELEASES_API_URL;

    if (!ota_validate_github_url(url, true)) {
        if (state_already_checking) {
            ota_lock();
            s_status.gateway_reachable = false;
            s_status.check_seq++;
            set_status_error_locked("Rejected untrusted release API URL");
            ota_unlock();
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (!state_already_checking) {
        ota_lock();
        if (s_status.state == OTA_STATE_CHECKING ||
            s_status.state == OTA_STATE_DOWNLOADING ||
            s_status.state == OTA_STATE_UPLOADING ||
            s_status.state == OTA_STATE_VERIFYING ||
            s_status.state == OTA_STATE_REBOOTING) {
            ota_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        s_status.state = OTA_STATE_CHECKING;
        s_status.last_error[0] = '\0';
        snprintf(s_status.message, sizeof(s_status.message), "Checking GitHub Releases...");
        ota_unlock();
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "BrewLink-ESP32-OTA/" BREWLINK_FIRMWARE_VERSION,
        .disable_auto_redirect = true,
        .max_redirection_count = OTA_MAX_REDIRECTS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ota_lock();
        s_status.gateway_reachable = false;
        s_status.check_seq++;
        set_status_error_locked("Failed to init HTTP client");
        ota_unlock();
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    int status_code = 0;
    int64_t content_len = 0;
    char open_err[OTA_MESSAGE_MAX_LEN] = {0};
    esp_err_t err = ota_open_validated_https(client, true, &status_code, &content_len, open_err, sizeof(open_err));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GitHub API open failed: %s", open_err);
        esp_http_client_cleanup(client);
        ota_lock();
        s_status.gateway_reachable = false;
        s_status.check_seq++;
        set_status_error_locked("Cannot reach GitHub Releases API (check gateway internet access)");
        ota_unlock();
        return err;
    }

    if (status_code != 200) {
        ESP_LOGW(TAG, "GitHub Releases API returned HTTP %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ota_lock();
        s_status.gateway_reachable = false;
        s_status.check_seq++;
        char msg[OTA_MESSAGE_MAX_LEN];
        snprintf(msg, sizeof(msg), "GitHub API returned HTTP %d", status_code);
        set_status_error_locked(msg);
        ota_unlock();
        return (status_code == 404) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    if (content_len > (int64_t)OTA_MAX_RELEASE_JSON_BYTES) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ota_lock();
        s_status.gateway_reachable = false;
        s_status.check_seq++;
        set_status_error_locked("GitHub Release response exceeds size limit");
        ota_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    // Allocate response buffer after TLS handshake so peak DRAM stays minimal
    size_t buf_cap = OTA_DEFAULT_RELEASE_JSON_BYTES;
    if (content_len > 0) {
        size_t wanted = (size_t)content_len + 1U;
        if (wanted < 2048U) {
            wanted = 2048U;
        }
        buf_cap = (wanted < OTA_MAX_RELEASE_JSON_BYTES) ? wanted : OTA_MAX_RELEASE_JSON_BYTES;
    }

    char *resp_buf = (char *)malloc(buf_cap);
    if (!resp_buf && buf_cap > OTA_FALLBACK_RELEASE_JSON_BYTES) {
        buf_cap = OTA_FALLBACK_RELEASE_JSON_BYTES;
        resp_buf = (char *)malloc(buf_cap);
    }
    if (!resp_buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ota_lock();
        s_status.gateway_reachable = false;
        s_status.check_seq++;
        set_status_error_locked("Out of heap allocating GitHub API buffer");
        ota_unlock();
        return ESP_ERR_NO_MEM;
    }

    size_t total_read = 0;
    int zero_reads = 0;
    while (total_read + 1U < buf_cap) {
        errno = 0;
        int r = esp_http_client_read(client, resp_buf + total_read,
                                     (int)(buf_cap - 1U - total_read));
        if (r < 0) {
            break;
        }
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            if (content_len > 0 && total_read >= (size_t)content_len) {
                break;
            }
            int serr = esp_http_client_get_errno(client);
            if (serr == ECONNRESET || serr == ENOTCONN) {
                break;
            }
            if (zero_reads++ < 10) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            break;
        }
        zero_reads = 0;
        total_read += (size_t)r;
        vTaskDelay(1);
    }
    resp_buf[total_read] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // Allocate parsed release struct on heap after TLS cleanup to keep stack usage low
    ota_release_info_t *parsed = (ota_release_info_t *)calloc(1, sizeof(ota_release_info_t));
    if (!parsed) {
        free(resp_buf);
        ota_lock();
        s_status.gateway_reachable = false;
        s_status.check_seq++;
        set_status_error_locked("Out of heap parsing release metadata");
        ota_unlock();
        return ESP_ERR_NO_MEM;
    }

    err = ota_parse_github_release_json(resp_buf, ota_manager_get_current_version(), parsed);
    free(resp_buf);

    if (err != ESP_OK) {
        free(parsed);
        ota_lock();
        s_status.gateway_reachable = false;
        s_status.check_seq++;
        set_status_error_locked("Failed to parse GitHub Release payload");
        ota_unlock();
        return err;
    }

    ota_lock();
    s_status.latest_release = *parsed;
    s_status.last_check_time_s = esp_timer_get_time() / 1000000LL;
    s_status.gateway_reachable = true;
    s_status.check_seq++;
    s_status.state = OTA_STATE_IDLE;
    s_status.last_error[0] = '\0';
    snprintf(s_status.message, sizeof(s_status.message),
             parsed->update_available ? "Update available: v%s" : "Up to date (latest: v%s)",
             parsed->version);
    ota_unlock();

    if (out_release) {
        *out_release = *parsed;
    }
    ESP_LOGI(TAG, "GitHub Release checked: tag=%s version=%s update=%s asset=%s",
             parsed->tag_name, parsed->version,
             parsed->update_available ? "yes" : "no", parsed->asset_name);
    free(parsed);
    return ESP_OK;
}

esp_err_t ota_manager_check_github_release(const char *releases_url, ota_release_info_t *out_release) {
    return ota_perform_github_release_check(releases_url, out_release, false);
}

static void ota_release_check_task(void *pvParameters) {
    ota_check_task_args_t *args = (ota_check_task_args_t *)pvParameters;
    const char *url = (args && args->url[0] != '\0') ? args->url : NULL;
    ota_perform_github_release_check(url, NULL, true);
    free(args);
    vTaskDelete(NULL);
}

esp_err_t ota_manager_start_release_check(const char *releases_url) {
    const char *effective_url = (releases_url && releases_url[0] != '\0')
                                ? releases_url
                                : BREWLINK_GITHUB_RELEASES_API_URL;
    if (!ota_validate_github_url(effective_url, true)) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t now_us = esp_timer_get_time();

    ota_lock();
    if (s_status.state == OTA_STATE_CHECKING ||
        s_status.state == OTA_STATE_DOWNLOADING ||
        s_status.state == OTA_STATE_UPLOADING ||
        s_status.state == OTA_STATE_VERIFYING ||
        s_status.state == OTA_STATE_REBOOTING) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_last_check_attempt_us > 0 &&
        (now_us - s_last_check_attempt_us) < ((int64_t)OTA_CHECK_COOLDOWN_SEC * 1000000LL)) {
        ota_unlock();
        return ESP_ERR_TIMEOUT;
    }

    ota_check_task_args_t *args = (ota_check_task_args_t *)calloc(1, sizeof(ota_check_task_args_t));
    if (!args) {
        ota_unlock();
        return ESP_ERR_NO_MEM;
    }
    snprintf(args->url, sizeof(args->url), "%s", effective_url);

    s_last_check_attempt_us = now_us;
    s_status.state = OTA_STATE_CHECKING;
    s_status.last_error[0] = '\0';
    snprintf(s_status.message, sizeof(s_status.message), "Checking GitHub Releases...");
    ota_unlock();

    BaseType_t res = xTaskCreatePinnedToCore(
        ota_release_check_task,
        "ota_chk_task",
        OTA_TASK_STACK_SIZE,
        args,
        OTA_TASK_PRIORITY,
        NULL,
        NETWORKING_TASK_CORE
    );
    if (res != pdPASS) {
        free(args);
        ota_lock();
        s_status.gateway_reachable = false;
        s_status.check_seq++;
        set_status_error_locked("Failed to create release check task");
        ota_unlock();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t ota_manager_set_upload_signature(const uint8_t *sig_der, size_t sig_len) {
    upload_lock();
    if (!sig_der || sig_len == 0) {
        s_next_upload_has_sig = false;
        s_next_upload_sig_len = 0;
        upload_unlock();
        return ESP_OK;
    }
    if (sig_len > OTA_MAX_SIG_BYTES) {
        upload_unlock();
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_next_upload_sig, sig_der, sig_len);
    s_next_upload_sig_len = sig_len;
    s_next_upload_has_sig = true;
    upload_unlock();
    return ESP_OK;
}

esp_err_t ota_manager_set_allow_downgrade(bool allow) {
    upload_lock();
    s_next_upload_allow_downgrade = allow;
    upload_unlock();
    return ESP_OK;
}

static esp_err_t ota_abort_upload_internal(const char *reason, bool only_if_active) {
    upload_lock();
    bool was_active = s_upload.active || s_upload.ota_begun;
    if (s_upload.ota_begun) {
        esp_ota_abort(s_upload.ota_handle);
    }
    memset(&s_upload, 0, sizeof(s_upload));
    s_next_upload_has_sig = false;
    s_next_upload_sig_len = 0;
    s_next_upload_allow_downgrade = false;

    ota_lock();
    if (!only_if_active || was_active ||
        s_status.state == OTA_STATE_DOWNLOADING ||
        s_status.state == OTA_STATE_UPLOADING ||
        s_status.state == OTA_STATE_VERIFYING) {
        if (s_status.state != OTA_STATE_CHECKING) {
            set_status_error_locked(reason ? reason : "OTA update aborted");
        }
    }
    ota_unlock();
    upload_unlock();

    ESP_LOGW(TAG, "OTA aborted: %s", reason ? reason : "aborted");
    return ESP_OK;
}

static esp_err_t ota_begin_upload_internal(size_t total_len, const uint8_t *first_chunk,
                                           size_t first_chunk_len, ota_image_info_t *out_info,
                                           bool from_remote_task,
                                           const char *expected_sha256_hex,
                                           const uint8_t *sig_der, size_t sig_len,
                                           bool allow_downgrade) {
    if (!first_chunk || first_chunk_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    upload_lock();
    ota_lock();
    bool busy = s_upload.active ||
                s_status.state == OTA_STATE_CHECKING ||
                s_status.state == OTA_STATE_VERIFYING ||
                s_status.state == OTA_STATE_REBOOTING ||
                (!from_remote_task && (s_status.state == OTA_STATE_DOWNLOADING ||
                                       s_status.state == OTA_STATE_UPLOADING));
    ota_unlock();
    if (busy) {
        upload_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    // Consume staged manual upload options if not called by remote download task
    bool eff_allow_downgrade = allow_downgrade;
    bool eff_has_sig = (sig_der != NULL && sig_len > 0 && sig_len <= OTA_MAX_SIG_BYTES);
    uint8_t eff_sig[OTA_MAX_SIG_BYTES];
    size_t eff_sig_len = 0;
    if (eff_has_sig) {
        memcpy(eff_sig, sig_der, sig_len);
        eff_sig_len = sig_len;
    } else if (!from_remote_task && s_next_upload_has_sig) {
        memcpy(eff_sig, s_next_upload_sig, s_next_upload_sig_len);
        eff_sig_len = s_next_upload_sig_len;
        eff_has_sig = true;
    }
    if (!from_remote_task && s_next_upload_allow_downgrade) {
        eff_allow_downgrade = true;
    }
    s_next_upload_has_sig = false;
    s_next_upload_sig_len = 0;
    s_next_upload_allow_downgrade = false;

    // Quick format check on first chunk before opening OTA
    ota_image_info_t probe;
    memset(&probe, 0, sizeof(probe));
    if (first_chunk_len >= 4) {
        if ((first_chunk[0] == 0x7F && first_chunk[1] == 'E' && first_chunk[2] == 'L' && first_chunk[3] == 'F') ||
            (first_chunk[0] == 'P' && first_chunk[1] == 'K') ||
            (first_chunk_len >= 32 && first_chunk[0] == 0xAA && first_chunk[1] == 0x50)) {
            esp_err_t verr = ota_verify_firmware_header(first_chunk, first_chunk_len, &probe);
            if (out_info) {
                *out_info = probe;
            }
            ota_lock();
            set_status_error_locked(probe.error);
            ota_unlock();
            upload_unlock();
            return verr;
        }
    }

    bool factory_mode = false;
    bool all_ff = true;
    size_t ff_check = (first_chunk_len < 16U) ? first_chunk_len : 16U;
    for (size_t i = 0; i < ff_check; i++) {
        if (first_chunk[i] != 0xFFU) {
            all_ff = false;
            break;
        }
    }

    if (all_ff) {
        if (total_len <= OTA_FACTORY_APP_OFFSET + OTA_MIN_IMAGE_HEADER_SIZE) {
            snprintf(probe.error, sizeof(probe.error),
                     "Merged factory image size (%lu B) too small before app offset 0x10000",
                     (unsigned long)total_len);
            if (out_info) {
                *out_info = probe;
            }
            ota_lock();
            set_status_error_locked(probe.error);
            ota_unlock();
            upload_unlock();
            return ESP_ERR_INVALID_SIZE;
        }
        factory_mode = true;
    } else if (first_chunk[0] != OTA_ESP_IMAGE_MAGIC) {
        esp_err_t verr = ota_verify_firmware_header(first_chunk, first_chunk_len, &probe);
        if (out_info) {
            *out_info = probe;
        }
        ota_lock();
        set_status_error_locked(probe.error);
        ota_unlock();
        upload_unlock();
        return verr;
    }

    memset(&s_upload, 0, sizeof(s_upload));
    s_upload.active = true;
    s_upload.factory_mode = factory_mode;
    s_upload.expected_total = total_len;
    s_upload.allow_downgrade = eff_allow_downgrade;
    ota_sha256_init(&s_upload.file_sha_ctx);
    if (expected_sha256_hex && strlen(expected_sha256_hex) == OTA_SHA256_HEX_LEN) {
        memcpy(s_upload.expected_sha256, expected_sha256_hex, OTA_SHA256_HEX_LEN + 1U);
        s_upload.has_expected_sha256 = true;
    }
    if (eff_has_sig) {
        memcpy(s_upload.signature, eff_sig, eff_sig_len);
        s_upload.signature_len = eff_sig_len;
        s_upload.has_signature = true;
    }

    ota_lock();
    if (!from_remote_task) {
        s_status.state = OTA_STATE_UPLOADING;
    }
    s_status.reboot_deferred = false;
    s_status.progress_pct = 0;
    s_status.bytes_written = 0;
    s_status.total_bytes = (factory_mode && total_len > OTA_FACTORY_APP_OFFSET)
                           ? (total_len - OTA_FACTORY_APP_OFFSET)
                           : total_len;
    s_status.last_error[0] = '\0';
    snprintf(s_status.message, sizeof(s_status.message),
             factory_mode ? "Streaming factory image (extracting app @ 0x10000)..." : "Verifying firmware header...");
    ota_unlock();

    esp_err_t err = process_upload_bytes_locked(first_chunk, first_chunk_len, out_info);
    upload_unlock();

    if (err != ESP_OK) {
        const char *reason = (out_info && out_info->error[0]) ? out_info->error : "Firmware header verification failed";
        ota_abort_upload_internal(reason, false);
        return err;
    }

    return ESP_OK;
}

esp_err_t ota_manager_begin_upload(size_t total_len, const uint8_t *first_chunk,
                                   size_t first_chunk_len, ota_image_info_t *out_info) {
    return ota_begin_upload_internal(total_len, first_chunk, first_chunk_len, out_info,
                                     false, NULL, NULL, 0, false);
}

esp_err_t ota_manager_write_upload_chunk(const uint8_t *data, size_t len) {
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    ota_image_info_t err_info;
    memset(&err_info, 0, sizeof(err_info));

    upload_lock();
    if (!s_upload.active) {
        upload_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = process_upload_bytes_locked(data, len, &err_info);
    upload_unlock();

    if (err != ESP_OK) {
        const char *reason = err_info.error[0] ? err_info.error : "Upload chunk write/verification failed";
        ota_abort_upload_internal(reason, false);
        return err;
    }
    return ESP_OK;
}

esp_err_t ota_manager_finish_upload(bool schedule_reboot) {
    upload_lock();
    if (!s_upload.active) {
        upload_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_upload.ota_begun) {
        upload_unlock();
        ota_abort_upload_internal("Upload ended before full firmware header received", false);
        return ESP_ERR_INVALID_SIZE;
    }

    ota_lock();
    s_status.state = OTA_STATE_VERIFYING;
    s_status.progress_pct = 100;
    snprintf(s_status.message, sizeof(s_status.message), "Verifying image checksum, SHA-256 & signature...");
    ota_unlock();

    uint8_t file_digest[32];
    char file_digest_hex[OTA_SHA256_HEX_LEN + 1];
    ota_sha256_final(&s_upload.file_sha_ctx, file_digest);
    ota_sha256_to_hex(file_digest, file_digest_hex);

    // 1. Verify against SHA256SUMS.txt if present
    if (s_upload.has_expected_sha256 &&
        !ota_const_time_eq_hex(file_digest_hex, s_upload.expected_sha256, OTA_SHA256_HEX_LEN)) {
        esp_ota_abort(s_upload.ota_handle);
        memset(&s_upload, 0, sizeof(s_upload));
        ota_lock();
        set_status_error_locked("Firmware SHA-256 mismatch against release SHA256SUMS.txt");
        ota_unlock();
        upload_unlock();
        ESP_LOGE(TAG, "SHA256SUMS mismatch!");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // 2. Verify ECDSA-P256 cryptographic signature if required (or if provided)
    if (ota_manager_is_signature_required() || s_upload.has_signature) {
        if (!s_upload.has_signature) {
            esp_ota_abort(s_upload.ota_handle);
            memset(&s_upload, 0, sizeof(s_upload));
            ota_lock();
            set_status_error_locked("Missing required ECDSA-P256 firmware signature (.sig)");
            ota_unlock();
            upload_unlock();
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t sig_err = ota_verify_ecdsa_signature(file_digest, s_upload.signature, s_upload.signature_len);
        if (sig_err != ESP_OK) {
            esp_ota_abort(s_upload.ota_handle);
            memset(&s_upload, 0, sizeof(s_upload));
            ota_lock();
            set_status_error_locked("Firmware cryptographic signature verification failed");
            ota_unlock();
            upload_unlock();
            return sig_err;
        }
    }

    // 3. ESP-IDF image checksum & trailing SHA-256 verification
    esp_err_t err = esp_ota_end(s_upload.ota_handle);
    if (err != ESP_OK) {
        memset(&s_upload, 0, sizeof(s_upload));
        char msg[OTA_MESSAGE_MAX_LEN];
        snprintf(msg, sizeof(msg), "Image verification failed (%s)", esp_err_to_name(err));
        ota_lock();
        set_status_error_locked(msg);
        ota_unlock();
        upload_unlock();
        ESP_LOGE(TAG, "%s", msg);
        return err;
    }

    const esp_partition_t *target_part = s_upload.update_part;
    const char *target_label = target_part ? target_part->label : "ota";
    err = esp_ota_set_boot_partition(target_part);
    memset(&s_upload, 0, sizeof(s_upload));

    if (err != ESP_OK) {
        char msg[OTA_MESSAGE_MAX_LEN];
        snprintf(msg, sizeof(msg), "Failed to set boot partition (%s)", esp_err_to_name(err));
        ota_lock();
        set_status_error_locked(msg);
        ota_unlock();
        upload_unlock();
        ESP_LOGE(TAG, "%s", msg);
        return err;
    }

    ota_lock();
    s_status.last_error[0] = '\0';
    if (schedule_reboot) {
        BaseType_t t_ok = xTaskCreate(delayed_ota_reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
        if (t_ok != pdPASS) {
            // Fallback to esp_timer one-shot reboot
            bool timer_ok = false;
            if (!s_reboot_fallback_timer) {
                const esp_timer_create_args_t targs = {
                    .callback = reboot_fallback_timer_cb,
                    .name = "ota_reboot_fb",
                };
                if (esp_timer_create(&targs, &s_reboot_fallback_timer) == ESP_OK) {
                    timer_ok = (esp_timer_start_once(s_reboot_fallback_timer, 1200000LL) == ESP_OK);
                }
            } else {
                timer_ok = (esp_timer_start_once(s_reboot_fallback_timer, 1200000LL) == ESP_OK);
            }
            if (timer_ok) {
                s_status.state = OTA_STATE_REBOOTING;
                snprintf(s_status.message, sizeof(s_status.message),
                         "Firmware verified on '%s'. Rebooting...", target_label);
            } else {
                s_status.state = OTA_STATE_IDLE;
                s_status.reboot_deferred = true;
                snprintf(s_status.message, sizeof(s_status.message),
                         "Firmware verified on '%s'. Manual reboot required.", target_label);
            }
        } else {
            s_status.state = OTA_STATE_REBOOTING;
            snprintf(s_status.message, sizeof(s_status.message),
                     "Firmware verified on '%s'. Rebooting...", target_label);
        }
    } else {
        s_status.state = OTA_STATE_IDLE;
        s_status.reboot_deferred = true;
        snprintf(s_status.message, sizeof(s_status.message),
                 "Firmware verified & staged on '%s'. Reboot when safe.", target_label);
    }
    ota_unlock();
    upload_unlock();

    ESP_LOGI(TAG, "OTA update committed to '%s' (reboot=%s)", target_label, schedule_reboot ? "yes" : "no");
    return ESP_OK;
}

esp_err_t ota_manager_abort_upload(const char *reason) {
    return ota_abort_upload_internal(reason, false);
}

static void ota_remote_download_task(void *pvParameters) {
    ota_download_task_args_t *args = (ota_download_task_args_t *)pvParameters;
    if (!args) {
        ota_abort_upload_internal("Internal OTA task parameter error", false);
        vTaskDelete(NULL);
        return;
    }

    // 1. Optional SHA256SUMS.txt fetch & parse
    char expected_sha256[OTA_SHA256_HEX_LEN + 1] = {0};
    if (args->sums_url[0] != '\0' && args->asset_name[0] != '\0') {
        uint8_t *sums_buf = (uint8_t *)calloc(1, OTA_MAX_SUMS_BYTES + 1U);
        if (sums_buf) {
            size_t sums_len = 0;
            if (ota_fetch_small_https_asset(args->sums_url, sums_buf, OTA_MAX_SUMS_BYTES, &sums_len) == ESP_OK) {
                sums_buf[sums_len] = '\0';
                if (ota_parse_sha256sums((const char *)sums_buf, args->asset_name, expected_sha256) == ESP_OK) {
                    ESP_LOGI(TAG, "Loaded expected SHA-256 from SHA256SUMS.txt for %s", args->asset_name);
                }
            }
            free(sums_buf);
        }
    }

    // 2. Fetch .sig asset if signature is required or available
    uint8_t sig_buf[OTA_MAX_SIG_BYTES];
    size_t sig_len = 0;
    if (args->sig_url[0] != '\0') {
        if (ota_fetch_small_https_asset(args->sig_url, sig_buf, sizeof(sig_buf), &sig_len) != ESP_OK) {
            sig_len = 0;
            if (ota_manager_is_signature_required()) {
                free(args);
                ota_abort_upload_internal("Failed to download required firmware signature (.sig)", false);
                vTaskDelete(NULL);
                return;
            }
        }
    } else if (ota_manager_is_signature_required()) {
        free(args);
        ota_abort_upload_internal("Release does not include required ECDSA signature (.sig)", false);
        vTaskDelete(NULL);
        return;
    }

    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) {
        free(args);
        ota_abort_upload_internal("Out of heap allocating download buffer", false);
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t cfg = {
        .url = args->download_url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "BrewLink-ESP32-OTA/" BREWLINK_FIRMWARE_VERSION,
        .disable_auto_redirect = true,
        .max_redirection_count = OTA_MAX_REDIRECTS,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(buf);
        free(args);
        ota_abort_upload_internal("Failed to init OTA download HTTP client", false);
        vTaskDelete(NULL);
        return;
    }

    int status_code = 0;
    int64_t content_len = 0;
    char open_err[OTA_MESSAGE_MAX_LEN] = {0};
    esp_err_t err = ota_open_validated_https(client, false, &status_code, &content_len, open_err, sizeof(open_err));

    if (err != ESP_OK || status_code != 200) {
        char msg[OTA_MESSAGE_MAX_LEN];
        if (err != ESP_OK) {
            snprintf(msg, sizeof(msg), "%s", open_err[0] ? open_err : esp_err_to_name(err));
        } else {
            snprintf(msg, sizeof(msg), "Release server returned HTTP %d", status_code);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(buf);
        free(args);
        ota_abort_upload_internal(msg, false);
        vTaskDelete(NULL);
        return;
    }

    if (content_len > OTA_MAX_DOWNLOAD_BYTES) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(buf);
        free(args);
        ota_abort_upload_internal("Remote firmware exceeds maximum allowed partition size", false);
        vTaskDelete(NULL);
        return;
    }

    size_t expected_total = (content_len > 0) ? (size_t)content_len : 0U;
    bool started = false;
    size_t total_rx = 0;
    int zero_reads = 0;

    while (1) {
        errno = 0;
        int r = esp_http_client_read(client, (char *)buf, 4096);
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            if (expected_total > 0 && total_rx >= expected_total) {
                break;
            }
            int serr = esp_http_client_get_errno(client);
            if (serr == ECONNRESET || serr == ENOTCONN) {
                err = ESP_FAIL;
                break;
            }
            zero_reads++;
            if (zero_reads > 15) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        zero_reads = 0;

        total_rx += (size_t)r;
        if ((int64_t)total_rx > OTA_MAX_DOWNLOAD_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        if (!started) {
            ota_image_info_t info;
            err = ota_begin_upload_internal(expected_total, buf, (size_t)r, &info,
                                            true,
                                            expected_sha256[0] ? expected_sha256 : NULL,
                                            sig_len > 0 ? sig_buf : NULL, sig_len,
                                            args->allow_downgrade);
            started = true;
        } else {
            err = ota_manager_write_upload_chunk(buf, (size_t)r);
        }
        if (err != ESP_OK) {
            break;
        }
        vTaskDelay(1);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buf);

    if (err != ESP_OK || !started || (expected_total > 0 && total_rx < expected_total)) {
        ota_lock();
        bool already_error = (s_status.state == OTA_STATE_ERROR);
        ota_unlock();
        if (!already_error) {
            ota_abort_upload_internal(err == ESP_ERR_TIMEOUT ? "Firmware download timed out" : "Incomplete firmware download", false);
        }
        free(args);
        vTaskDelete(NULL);
        return;
    }

    // Physical safety check: defer auto-reboot if 2000W Grainfather G30 heater/auto-mode is active
    bool schedule_reboot = true;
    g30_state_t g30_state;
    bool g30_connected = false;
    memset(&g30_state, 0, sizeof(g30_state));
    if (!args->force_reboot &&
        g30_controller_get_state(&g30_state, &g30_connected, NULL, NULL) == ESP_OK &&
        g30_connected && (g30_state.heat_active || g30_state.auto_mode)) {
        ESP_LOGW(TAG, "G30 boiler active (heat=%d, auto=%d, temp=%.1fC) -> staging OTA without auto-reboot!",
                 g30_state.heat_active, g30_state.auto_mode, g30_state.current_temp_c);
        schedule_reboot = false;
    }

    free(args);
    ota_manager_finish_upload(schedule_reboot);
    vTaskDelete(NULL);
}

esp_err_t ota_manager_start_remote_update_ex(const char *download_url, bool force_reboot, bool allow_downgrade) {
    ota_download_task_args_t *args = (ota_download_task_args_t *)calloc(1, sizeof(ota_download_task_args_t));
    if (!args) {
        return ESP_ERR_NO_MEM;
    }

    upload_lock();
    ota_lock();
    if (s_upload.active ||
        s_status.state == OTA_STATE_CHECKING ||
        s_status.state == OTA_STATE_DOWNLOADING ||
        s_status.state == OTA_STATE_UPLOADING ||
        s_status.state == OTA_STATE_VERIFYING ||
        s_status.state == OTA_STATE_REBOOTING) {
        ota_unlock();
        upload_unlock();
        free(args);
        return ESP_ERR_INVALID_STATE;
    }

    // Always prefer the gateway-verified cached release URL; if download_url passed, it MUST be allowlisted
    const char *eff_url = (download_url && download_url[0] != '\0')
                          ? download_url
                          : s_status.latest_release.download_url;
    if (!ota_validate_github_url(eff_url, false)) {
        ota_unlock();
        upload_unlock();
        free(args);
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(args->download_url, sizeof(args->download_url), "%s", eff_url);
    if (strcmp(eff_url, s_status.latest_release.download_url) == 0) {
        snprintf(args->asset_name, sizeof(args->asset_name), "%s", s_status.latest_release.asset_name);
        snprintf(args->sig_url, sizeof(args->sig_url), "%s", s_status.latest_release.sig_download_url);
        snprintf(args->sums_url, sizeof(args->sums_url), "%s", s_status.latest_release.sha256sums_url);
    }
    args->force_reboot = force_reboot;
    args->allow_downgrade = allow_downgrade;

    s_status.state = OTA_STATE_DOWNLOADING;
    s_status.reboot_deferred = false;
    s_status.progress_pct = 0;
    s_status.bytes_written = 0;
    s_status.total_bytes = 0;
    s_status.last_error[0] = '\0';
    snprintf(s_status.message, sizeof(s_status.message), "Starting HTTPS firmware download...");
    ota_unlock();
    upload_unlock();

    BaseType_t res = xTaskCreatePinnedToCore(
        ota_remote_download_task,
        "ota_dl_task",
        OTA_TASK_STACK_SIZE,
        args,
        OTA_TASK_PRIORITY,
        NULL,
        NETWORKING_TASK_CORE
    );
    if (res != pdPASS) {
        free(args);
        ota_lock();
        set_status_error_locked("Failed to create OTA download task");
        ota_unlock();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t ota_manager_start_remote_update(const char *download_url) {
    return ota_manager_start_remote_update_ex(download_url, false, false);
}

#else // !ESP_PLATFORM (Host Native Unit Test Stubs)

esp_err_t ota_manager_init(void) { return ESP_OK; }
esp_err_t ota_manager_schedule_boot_confirm(void) { return ESP_OK; }
const char *ota_manager_get_current_version(void) { return BREWLINK_FIRMWARE_VERSION; }
const char *ota_manager_get_running_partition(void) { return "ota_0"; }
esp_err_t ota_manager_get_status(ota_status_t *out_status) {
    if (!out_status) return ESP_ERR_INVALID_ARG;
    memset(out_status, 0, sizeof(*out_status));
    snprintf(out_status->current_version, sizeof(out_status->current_version), "%s", BREWLINK_FIRMWARE_VERSION);
    snprintf(out_status->running_partition, sizeof(out_status->running_partition), "ota_0");
    out_status->signature_required = ota_manager_is_signature_required();
    return ESP_OK;
}
esp_err_t ota_manager_check_github_release(const char *releases_url, ota_release_info_t *out_release) {
    (void)releases_url; (void)out_release; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ota_manager_start_release_check(const char *releases_url) {
    (void)releases_url; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ota_manager_start_remote_update(const char *download_url) {
    (void)download_url; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ota_manager_start_remote_update_ex(const char *download_url, bool force_reboot, bool allow_downgrade) {
    (void)download_url; (void)force_reboot; (void)allow_downgrade; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ota_manager_set_upload_signature(const uint8_t *sig_der, size_t sig_len) {
    (void)sig_der; (void)sig_len; return ESP_OK;
}
esp_err_t ota_manager_set_allow_downgrade(bool allow) {
    (void)allow; return ESP_OK;
}
esp_err_t ota_manager_begin_upload(size_t total_len, const uint8_t *first_chunk,
                                   size_t first_chunk_len, ota_image_info_t *out_info) {
    (void)total_len;
    return ota_verify_firmware_header(first_chunk, first_chunk_len, out_info);
}
esp_err_t ota_manager_write_upload_chunk(const uint8_t *data, size_t len) {
    (void)data; (void)len; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ota_manager_finish_upload(bool schedule_reboot) {
    (void)schedule_reboot; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ota_manager_abort_upload(const char *reason) {
    (void)reason; return ESP_OK;
}

#endif
