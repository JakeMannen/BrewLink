/**
 * @file ota_manager.c
 * @brief BrewLink Dual-OTA Firmware Update, Release Verification, and GitHub Release Checker
 */

#include "ota_manager.h"
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
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

static void ota_compute_sha256(const uint8_t *data, size_t len, uint8_t out_digest[32]) {
    uint32_t state[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    uint8_t block[64];
    size_t offset = 0;

    while (offset + 64U <= len) {
        ota_sha256_transform(state, data + offset);
        offset += 64U;
    }

    size_t rem = len - offset;
    memset(block, 0, sizeof(block));
    if (rem > 0) {
        memcpy(block, data + offset, rem);
    }
    block[rem++] = 0x80U;
    if (rem > 56U) {
        ota_sha256_transform(state, block);
        memset(block, 0, sizeof(block));
    }

    uint64_t bit_len = (uint64_t)len * 8ULL;
    for (int i = 0; i < 8; i++) {
        block[56 + i] = (uint8_t)(bit_len >> ((7 - i) * 8));
    }
    ota_sha256_transform(state, block);

    for (int i = 0; i < 8; i++) {
        out_digest[i * 4]     = (uint8_t)(state[i] >> 24);
        out_digest[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        out_digest[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        out_digest[i * 4 + 3] = (uint8_t)(state[i]);
    }
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
                    for (int k = 0; k < 4 && *p != '\0'; k++) {
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

    while (*p != '\0') {
        p = json_skip_ws(p);
        if (*p == ']') {
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

        int score = score_asset_filename(asset_name);
        if (score > best_score && asset_url[0] != '\0') {
            best_score = score;
            snprintf(rel->asset_name, sizeof(rel->asset_name), "%s", asset_name);
            snprintf(rel->download_url, sizeof(rel->download_url), "%s", asset_url);
            rel->asset_size = asset_size;
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

    while (*p != '\0') {
        p = json_skip_ws(p);
        if (*p == '}') {
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

    if (out_release->tag_name[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
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

#define OTA_MAX_RELEASE_JSON_BYTES   32768U
#define OTA_MAX_REDIRECTS            5

typedef struct {
    bool active;
    bool ota_begun;
    bool factory_mode;
    bool boot_magic_checked;
    bool part_magic_checked;
    size_t expected_total;
    size_t stream_pos;
    size_t app_written;
    size_t header_buf_len;
    uint8_t header_buf[OTA_MIN_IMAGE_HEADER_SIZE];
    esp_ota_handle_t ota_handle;
    const esp_partition_t *update_part;
} ota_upload_session_t;

static SemaphoreHandle_t s_ota_mutex = NULL;
static ota_status_t s_status;
static ota_upload_session_t s_upload;
static char s_pending_url[OTA_URL_MAX_LEN];

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

static esp_err_t begin_flash_partition_with_header(ota_image_info_t *out_info) {
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

static esp_err_t process_upload_bytes(const uint8_t *data, size_t len, ota_image_info_t *out_info) {
    size_t i = 0;
    while (i < len) {
        if (s_upload.factory_mode && s_upload.stream_pos < OTA_FACTORY_APP_OFFSET) {
            // Validate bootloader magic @ 0x1000 and partitions magic @ 0x8000 as stream passes
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
                // Step byte-by-byte if crossing 0x1000 or 0x8000 checkpoints
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

        // Accumulate 288-byte application header before beginning flash erase/write
        if (!s_upload.ota_begun) {
            size_t need = OTA_MIN_IMAGE_HEADER_SIZE - s_upload.header_buf_len;
            size_t copy_n = (len - i < need) ? (len - i) : need;
            memcpy(s_upload.header_buf + s_upload.header_buf_len, data + i, copy_n);
            s_upload.header_buf_len += copy_n;
            s_upload.stream_pos += copy_n;
            i += copy_n;

            if (s_upload.header_buf_len == OTA_MIN_IMAGE_HEADER_SIZE) {
                esp_err_t err = begin_flash_partition_with_header(out_info);
                if (err != ESP_OK) {
                    return err;
                }
            }
            continue;
        }

        // Write remaining bytes directly to OTA partition
        size_t chunk_len = len - i;
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

    memset(&s_status, 0, sizeof(s_status));
    memset(&s_upload, 0, sizeof(s_upload));

    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc && app_desc->version[0] != '\0') {
        snprintf(s_status.current_version, sizeof(s_status.current_version), "%s", app_desc->version);
    } else {
        snprintf(s_status.current_version, sizeof(s_status.current_version), "%s", BREWLINK_FIRMWARE_VERSION);
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        snprintf(s_status.running_partition, sizeof(s_status.running_partition), "%s", running->label);
        esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
            ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                ESP_LOGI(TAG, "Confirmed newly booted OTA firmware v%s on '%s'",
                         s_status.current_version, running->label);
            } else {
                ESP_LOGW(TAG, "Failed to confirm pending OTA partition '%s'", running->label);
            }
        }
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
    ESP_LOGI(TAG, "OTA manager initialized: v%s running on '%s' (next target: '%s')",
             s_status.current_version, s_status.running_partition, s_status.target_partition);
    return ESP_OK;
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

esp_err_t ota_manager_check_github_release(const char *releases_url, ota_release_info_t *out_release) {
    const char *url = (releases_url && releases_url[0] != '\0')
                      ? releases_url
                      : BREWLINK_GITHUB_RELEASES_API_URL;

    ota_lock();
    if (s_status.state == OTA_STATE_DOWNLOADING ||
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

    char *resp_buf = (char *)malloc(OTA_MAX_RELEASE_JSON_BYTES);
    if (!resp_buf) {
        ota_lock();
        set_status_error_locked("Out of heap allocating GitHub API buffer");
        ota_unlock();
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "BrewLink-ESP32-OTA/" BREWLINK_FIRMWARE_VERSION,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(resp_buf);
        ota_lock();
        set_status_error_locked("Failed to init HTTP client");
        ota_unlock();
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GitHub API open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(resp_buf);
        ota_lock();
        set_status_error_locked("Cannot reach GitHub Releases API (check gateway internet access)");
        ota_unlock();
        return err;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    (void)content_len;

    size_t total_read = 0;
    while (total_read + 1 < OTA_MAX_RELEASE_JSON_BYTES) {
        int r = esp_http_client_read(client, resp_buf + total_read,
                                     (int)(OTA_MAX_RELEASE_JSON_BYTES - 1U - total_read));
        if (r <= 0) {
            break;
        }
        total_read += (size_t)r;
    }
    resp_buf[total_read] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status_code != 200) {
        ESP_LOGW(TAG, "GitHub Releases API returned HTTP %d", status_code);
        free(resp_buf);
        ota_lock();
        char msg[OTA_MESSAGE_MAX_LEN];
        snprintf(msg, sizeof(msg), "GitHub API returned HTTP %d", status_code);
        set_status_error_locked(msg);
        ota_unlock();
        return (status_code == 404) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    ota_release_info_t parsed;
    err = ota_parse_github_release_json(resp_buf, ota_manager_get_current_version(), &parsed);
    free(resp_buf);

    if (err != ESP_OK) {
        ota_lock();
        set_status_error_locked("Failed to parse GitHub Release payload");
        ota_unlock();
        return err;
    }

    ota_lock();
    s_status.latest_release = parsed;
    s_status.last_check_time_s = esp_timer_get_time() / 1000000LL;
    s_status.state = OTA_STATE_IDLE;
    s_status.last_error[0] = '\0';
    snprintf(s_status.message, sizeof(s_status.message),
             parsed.update_available ? "Update available: v%s" : "Up to date (latest: v%s)",
             parsed.version);
    ota_unlock();

    if (out_release) {
        *out_release = parsed;
    }
    ESP_LOGI(TAG, "GitHub Release checked: tag=%s version=%s update=%s asset=%s",
             parsed.tag_name, parsed.version,
             parsed.update_available ? "yes" : "no", parsed.asset_name);
    return ESP_OK;
}

esp_err_t ota_manager_begin_upload(size_t total_len, const uint8_t *first_chunk,
                                   size_t first_chunk_len, ota_image_info_t *out_info) {
    if (!first_chunk || first_chunk_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_lock();
    if (s_upload.active ||
        s_status.state == OTA_STATE_VERIFYING ||
        s_status.state == OTA_STATE_REBOOTING) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    ota_unlock();

    // Quick format check on first chunk before allocating/opening OTA
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
        return verr;
    }

    memset(&s_upload, 0, sizeof(s_upload));
    s_upload.active = true;
    s_upload.factory_mode = factory_mode;
    s_upload.expected_total = total_len;

    ota_lock();
    if (s_status.state != OTA_STATE_DOWNLOADING) {
        s_status.state = OTA_STATE_UPLOADING;
    }
    s_status.progress_pct = 0;
    s_status.bytes_written = 0;
    s_status.total_bytes = (factory_mode && total_len > OTA_FACTORY_APP_OFFSET)
                           ? (total_len - OTA_FACTORY_APP_OFFSET)
                           : total_len;
    s_status.last_error[0] = '\0';
    snprintf(s_status.message, sizeof(s_status.message),
             factory_mode ? "Streaming factory image (extracting app @ 0x10000)..." : "Verifying firmware header...");
    ota_unlock();

    esp_err_t err = process_upload_bytes(first_chunk, first_chunk_len, out_info);
    if (err != ESP_OK) {
        const char *reason = (out_info && out_info->error[0]) ? out_info->error : "Firmware header verification failed";
        ota_manager_abort_upload(reason);
        return err;
    }

    return ESP_OK;
}

esp_err_t ota_manager_write_upload_chunk(const uint8_t *data, size_t len) {
    if (!s_upload.active || !data) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0) {
        return ESP_OK;
    }

    ota_image_info_t err_info;
    memset(&err_info, 0, sizeof(err_info));
    esp_err_t err = process_upload_bytes(data, len, &err_info);
    if (err != ESP_OK) {
        const char *reason = err_info.error[0] ? err_info.error : "Upload chunk write/verification failed";
        ota_manager_abort_upload(reason);
        return err;
    }
    return ESP_OK;
}

esp_err_t ota_manager_finish_upload(bool schedule_reboot) {
    if (!s_upload.active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_upload.ota_begun) {
        ota_manager_abort_upload("Upload ended before full firmware header received");
        return ESP_ERR_INVALID_SIZE;
    }

    ota_lock();
    s_status.state = OTA_STATE_VERIFYING;
    s_status.progress_pct = 100;
    snprintf(s_status.message, sizeof(s_status.message), "Verifying image checksum & SHA-256...");
    ota_unlock();

    esp_err_t err = esp_ota_end(s_upload.ota_handle);
    if (err != ESP_OK) {
        s_upload.ota_begun = false;
        s_upload.active = false;
        char msg[OTA_MESSAGE_MAX_LEN];
        snprintf(msg, sizeof(msg), "Image verification failed (%s)", esp_err_to_name(err));
        ota_lock();
        set_status_error_locked(msg);
        ota_unlock();
        ESP_LOGE(TAG, "%s", msg);
        return err;
    }

    err = esp_ota_set_boot_partition(s_upload.update_part);
    const char *target_label = s_upload.update_part ? s_upload.update_part->label : "ota";
    s_upload.ota_begun = false;
    s_upload.active = false;

    if (err != ESP_OK) {
        char msg[OTA_MESSAGE_MAX_LEN];
        snprintf(msg, sizeof(msg), "Failed to set boot partition (%s)", esp_err_to_name(err));
        ota_lock();
        set_status_error_locked(msg);
        ota_unlock();
        ESP_LOGE(TAG, "%s", msg);
        return err;
    }

    ota_lock();
    s_status.last_error[0] = '\0';
    if (schedule_reboot) {
        s_status.state = OTA_STATE_REBOOTING;
        snprintf(s_status.message, sizeof(s_status.message),
                 "Firmware verified on '%s'. Rebooting...", target_label);
        xTaskCreate(delayed_ota_reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    } else {
        s_status.state = OTA_STATE_IDLE;
        snprintf(s_status.message, sizeof(s_status.message),
                 "Firmware verified on '%s'. Reboot required.", target_label);
    }
    ota_unlock();

    ESP_LOGI(TAG, "OTA update committed to '%s' (reboot=%s)", target_label, schedule_reboot ? "yes" : "no");
    return ESP_OK;
}

esp_err_t ota_manager_abort_upload(const char *reason) {
    if (s_upload.ota_begun) {
        esp_ota_abort(s_upload.ota_handle);
    }
    memset(&s_upload, 0, sizeof(s_upload));

    ota_lock();
    set_status_error_locked(reason ? reason : "OTA update aborted");
    ota_unlock();

    ESP_LOGW(TAG, "OTA aborted: %s", reason ? reason : "aborted");
    return ESP_OK;
}

static void ota_remote_download_task(void *pvParameters) {
    (void)pvParameters;
    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) {
        ota_manager_abort_upload("Out of heap allocating download buffer");
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t cfg = {
        .url = s_pending_url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "BrewLink-ESP32-OTA/" BREWLINK_FIRMWARE_VERSION,
        .max_redirection_count = OTA_MAX_REDIRECTS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(buf);
        ota_manager_abort_upload("Failed to init OTA download HTTP client");
        vTaskDelete(NULL);
        return;
    }

    int redirects = 0;
    int status_code = 0;
    int64_t content_len = 0;
    esp_err_t err = ESP_OK;

    while (redirects <= OTA_MAX_REDIRECTS) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            break;
        }
        content_len = esp_http_client_fetch_headers(client);
        status_code = esp_http_client_get_status_code(client);

        if (status_code == 301 || status_code == 302 || status_code == 303 ||
            status_code == 307 || status_code == 308) {
            esp_http_client_close(client);
            err = esp_http_client_set_redirection(client);
            if (err != ESP_OK) {
                break;
            }
            redirects++;
            continue;
        }
        break;
    }

    if (err != ESP_OK || status_code != 200) {
        char msg[OTA_MESSAGE_MAX_LEN];
        if (err != ESP_OK) {
            snprintf(msg, sizeof(msg), "Download connection failed: %s", esp_err_to_name(err));
        } else {
            snprintf(msg, sizeof(msg), "Release server returned HTTP %d", status_code);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(buf);
        ota_manager_abort_upload(msg);
        vTaskDelete(NULL);
        return;
    }

    size_t expected_total = (content_len > 0) ? (size_t)content_len : 0U;
    bool started = false;
    size_t total_rx = 0;

    while (1) {
        int r = esp_http_client_read(client, (char *)buf, 4096);
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            // Allow transient zero reads until transport reports complete or error
            if (expected_total > 0 && total_rx >= expected_total) {
                break;
            }
            break;
        }

        total_rx += (size_t)r;
        if (!started) {
            ota_image_info_t info;
            err = ota_manager_begin_upload(expected_total, buf, (size_t)r, &info);
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
        if (s_upload.active) {
            ota_manager_abort_upload("Incomplete firmware download");
        }
        vTaskDelete(NULL);
        return;
    }

    ota_manager_finish_upload(true);
    vTaskDelete(NULL);
}

esp_err_t ota_manager_start_remote_update(const char *download_url) {
    if (!download_url ||
        (strncmp(download_url, "https://", 8) != 0 && strncmp(download_url, "http://", 7) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(download_url) >= sizeof(s_pending_url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ota_lock();
    if (s_upload.active ||
        s_status.state == OTA_STATE_DOWNLOADING ||
        s_status.state == OTA_STATE_UPLOADING ||
        s_status.state == OTA_STATE_VERIFYING ||
        s_status.state == OTA_STATE_REBOOTING) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(s_pending_url, sizeof(s_pending_url), "%s", download_url);
    s_status.state = OTA_STATE_DOWNLOADING;
    s_status.progress_pct = 0;
    s_status.bytes_written = 0;
    s_status.total_bytes = 0;
    s_status.last_error[0] = '\0';
    snprintf(s_status.message, sizeof(s_status.message), "Starting HTTPS firmware download...");
    ota_unlock();

    BaseType_t res = xTaskCreatePinnedToCore(
        ota_remote_download_task,
        "ota_dl_task",
        OTA_TASK_STACK_SIZE,
        NULL,
        OTA_TASK_PRIORITY,
        NULL,
        NETWORKING_TASK_CORE
    );
    if (res != pdPASS) {
        ota_lock();
        set_status_error_locked("Failed to create OTA download task");
        ota_unlock();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

#else // !ESP_PLATFORM (Host Native Unit Test Stubs)

esp_err_t ota_manager_init(void) { return ESP_OK; }
const char *ota_manager_get_current_version(void) { return BREWLINK_FIRMWARE_VERSION; }
const char *ota_manager_get_running_partition(void) { return "ota_0"; }
esp_err_t ota_manager_get_status(ota_status_t *out_status) {
    if (!out_status) return ESP_ERR_INVALID_ARG;
    memset(out_status, 0, sizeof(*out_status));
    snprintf(out_status->current_version, sizeof(out_status->current_version), "%s", BREWLINK_FIRMWARE_VERSION);
    snprintf(out_status->running_partition, sizeof(out_status->running_partition), "ota_0");
    return ESP_OK;
}
esp_err_t ota_manager_check_github_release(const char *releases_url, ota_release_info_t *out_release) {
    (void)releases_url; (void)out_release; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t ota_manager_start_remote_update(const char *download_url) {
    (void)download_url; return ESP_ERR_NOT_SUPPORTED;
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
