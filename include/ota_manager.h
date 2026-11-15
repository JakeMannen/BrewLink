/**
 * @file ota_manager.h
 * @brief BrewLink Dual-OTA Firmware Update, Release Verification, and GitHub Release Checker
 * @details Handles SemVer release comparison, ESP-IDF firmware binary and merged factory image
 *          validation (header magic, chip ID, esp_app_desc_t project identity, segment XOR checksum,
 *          and trailing SHA-256 verification), GitHub Release asset selection, HTTPS OTA download,
 *          and streaming chunked HTTP manual upload to the inactive OTA partition.
 */

#ifndef BREWLINK_OTA_MANAGER_H
#define BREWLINK_OTA_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK                    0
#define ESP_FAIL                  -1
#endif
#ifndef ESP_ERR_NO_MEM
#define ESP_ERR_NO_MEM            0x101
#endif
#ifndef ESP_ERR_INVALID_ARG
#define ESP_ERR_INVALID_ARG       0x102
#endif
#ifndef ESP_ERR_INVALID_STATE
#define ESP_ERR_INVALID_STATE     0x103
#endif
#ifndef ESP_ERR_INVALID_SIZE
#define ESP_ERR_INVALID_SIZE      0x104
#endif
#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND         0x105
#endif
#ifndef ESP_ERR_NOT_SUPPORTED
#define ESP_ERR_NOT_SUPPORTED     0x106
#endif
#ifndef ESP_ERR_TIMEOUT
#define ESP_ERR_TIMEOUT           0x107
#endif
#ifndef ESP_ERR_INVALID_RESPONSE
#define ESP_ERR_INVALID_RESPONSE  0x108
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_VERSION_MAX_LEN         32
#define OTA_PROJECT_NAME_MAX_LEN    32
#define OTA_TAG_MAX_LEN             48
#define OTA_URL_MAX_LEN             512
#define OTA_ASSET_NAME_MAX_LEN      128
#define OTA_PARTITION_LABEL_MAX_LEN 20
#define OTA_MESSAGE_MAX_LEN         160
#define OTA_NOTES_MAX_LEN           256

#define OTA_FACTORY_BOOTLOADER_OFF  0x1000U
#define OTA_FACTORY_PARTITIONS_OFF  0x8000U
#define OTA_FACTORY_APP_OFFSET      0x10000U
#define OTA_MIN_IMAGE_HEADER_SIZE   288U
#define OTA_MAX_IMAGE_SIZE          0x1E0000U

#define OTA_ESP_IMAGE_MAGIC         0xE9U
#define OTA_ESP_APP_DESC_MAGIC      0xABCD5432U
#define OTA_CHIP_ID_ESP32           0x0000U
#define OTA_EXPECTED_PROJECT_NAME   "BrewLink"

#define OTA_TASK_STACK_SIZE         8192
#define OTA_TASK_PRIORITY           5
#define OTA_HTTP_TIMEOUT_MS         20000

/**
 * @brief Detected firmware upload / release file format.
 */
typedef enum {
    OTA_FORMAT_UNKNOWN = 0,
    OTA_FORMAT_FIRMWARE_BIN,     /**< Standalone ESP-IDF application binary (-firmware.bin) */
    OTA_FORMAT_FACTORY_BIN,      /**< Merged 4MB flash image (-factory.bin, app @ 0x10000) */
    OTA_FORMAT_ELF,              /**< Debug ELF executable (-firmware.elf), rejected for OTA */
    OTA_FORMAT_ZIP,              /**< Release ZIP bundle (.zip), must be extracted first */
    OTA_FORMAT_BOOTLOADER_BIN,   /**< Bootloader binary (-bootloader.bin), rejected for OTA */
    OTA_FORMAT_PARTITIONS_BIN,   /**< Partition table binary (-partitions.bin), rejected for OTA */
    OTA_FORMAT_OTHER_ESP_IMAGE   /**< ESP image not matching BrewLink ESP32 application */
} ota_file_format_t;

/**
 * @brief Lifecycle state of the OTA subsystem.
 */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_UPLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_REBOOTING,
    OTA_STATE_ERROR
} ota_state_t;

/**
 * @brief Parsed metadata and validation outcome for a candidate firmware image.
 */
typedef struct {
    bool valid;
    ota_file_format_t format;
    size_t app_offset;
    size_t app_size;
    uint16_t chip_id;
    uint8_t segment_count;
    bool hash_appended;
    char version[OTA_VERSION_MAX_LEN];
    char project_name[OTA_PROJECT_NAME_MAX_LEN];
    char build_date[16];
    char build_time[16];
    char idf_ver[OTA_VERSION_MAX_LEN];
    char error[OTA_MESSAGE_MAX_LEN];
} ota_image_info_t;

/**
 * @brief Metadata for a GitHub Release discovered remotely or via browser assist.
 */
typedef struct {
    bool available;
    bool update_available;
    bool prerelease;
    char tag_name[OTA_TAG_MAX_LEN];
    char version[OTA_VERSION_MAX_LEN];
    char release_name[64];
    char published_at[32];
    char html_url[OTA_URL_MAX_LEN];
    char asset_name[OTA_ASSET_NAME_MAX_LEN];
    char download_url[OTA_URL_MAX_LEN];
    size_t asset_size;
    char notes[OTA_NOTES_MAX_LEN];
} ota_release_info_t;

/**
 * @brief Thread-safe snapshot of OTA status exposed via REST API and Web UI.
 */
typedef struct {
    ota_state_t state;
    uint8_t progress_pct;
    size_t bytes_written;
    size_t total_bytes;
    char current_version[OTA_VERSION_MAX_LEN];
    char running_partition[OTA_PARTITION_LABEL_MAX_LEN];
    char target_partition[OTA_PARTITION_LABEL_MAX_LEN];
    char message[OTA_MESSAGE_MAX_LEN];
    char last_error[OTA_MESSAGE_MAX_LEN];
    ota_release_info_t latest_release;
    int64_t last_check_time_s;
} ota_status_t;

// =============================================================================
// Pure / Host-Testable Verification & Parsing Functions
// =============================================================================

/**
 * @brief Strip an optional leading 'v'/'V' and whitespace from a SemVer string.
 */
esp_err_t ota_normalize_version(const char *raw_version, char *out_buf, size_t out_len);

/**
 * @brief Compare two SemVer strings (major.minor.patch[-prerelease]).
 * @return -1 if ver_a < ver_b, 0 if equal, 1 if ver_a > ver_b.
 */
int ota_compare_versions(const char *ver_a, const char *ver_b);

/**
 * @brief Convenience check: true when candidate_ver is strictly newer than current_ver.
 */
bool ota_is_newer_version(const char *candidate_ver, const char *current_ver);

/**
 * @brief Return a human-readable string identifier for a detected file format.
 */
const char *ota_file_format_to_str(ota_file_format_t format);

/**
 * @brief Return a lowercase string identifier for an OTA state.
 */
const char *ota_state_to_str(ota_state_t state);

/**
 * @brief Verify the file signature, ESP-IDF image header, chip ID, and esp_app_desc_t metadata.
 * @details Supports standalone firmware images (`-firmware.bin`) and merged flash images
 *          (`-factory.bin` containing a valid BrewLink application at offset 0x10000).
 *          Explicitly detects and rejects ELF, ZIP, bootloader, and partition-table binaries.
 */
esp_err_t ota_verify_firmware_header(const uint8_t *data, size_t len, ota_image_info_t *out_info);

/**
 * @brief Perform full header, segment-table, XOR checksum, and trailing SHA-256 verification
 *        on an in-memory image buffer (supports both -firmware.bin and -factory.bin).
 */
esp_err_t ota_verify_full_image_integrity(const uint8_t *data, size_t len, ota_image_info_t *out_info);

/**
 * @brief Parse a GitHub Releases API JSON response object (`releases/latest`) and select the
 *        preferred OTA asset (`*-firmware.bin`, falling back to `*-factory.bin`).
 */
esp_err_t ota_parse_github_release_json(const char *json_str, const char *current_ver,
                                        ota_release_info_t *out_release);

// =============================================================================
// Runtime ESP32 OTA Subsystem API
// =============================================================================

/**
 * @brief Initialize the OTA manager, record running app version/partition, and mark current
 *        slot valid if pending verification after an OTA reboot.
 */
esp_err_t ota_manager_init(void);

/**
 * @brief Return the current running BrewLink firmware version string.
 */
const char *ota_manager_get_current_version(void);

/**
 * @brief Return the label of the currently booted flash partition (e.g. "ota_0").
 */
const char *ota_manager_get_running_partition(void);

/**
 * @brief Copy a mutex-protected snapshot of the current OTA status.
 */
esp_err_t ota_manager_get_status(ota_status_t *out_status);

/**
 * @brief Query GitHub Releases over HTTPS (using mbedTLS cert bundle) and cache latest release info.
 * @param releases_url Custom API URL or NULL to use BREWLINK_GITHUB_RELEASES_API_URL.
 */
esp_err_t ota_manager_check_github_release(const char *releases_url, ota_release_info_t *out_release);

/**
 * @brief Spawn a Core 0 background task to download a release binary from @p download_url,
 *        stream it into the next OTA partition, verify integrity, set boot partition, and reboot.
 */
esp_err_t ota_manager_start_remote_update(const char *download_url);

/**
 * @brief Begin a streaming manual firmware upload session into the inactive OTA partition.
 */
esp_err_t ota_manager_begin_upload(size_t total_len, const uint8_t *first_chunk,
                                   size_t first_chunk_len, ota_image_info_t *out_info);

/**
 * @brief Write the next contiguous chunk of a manual firmware upload.
 */
esp_err_t ota_manager_write_upload_chunk(const uint8_t *data, size_t len);

/**
 * @brief Finalize upload (esp_ota_end + esp_ota_set_boot_partition) and optionally schedule reboot.
 */
esp_err_t ota_manager_finish_upload(bool schedule_reboot);

/**
 * @brief Abort an in-progress manual upload and clean up OTA state cleanly.
 */
esp_err_t ota_manager_abort_upload(const char *reason);

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_OTA_MANAGER_H
