#ifndef BREWLINK_GRAINFATHER_G30_H
#define BREWLINK_GRAINFATHER_G30_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_log.h"
#else
#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK              0
#define ESP_FAIL            -1
#define ESP_ERR_NO_MEM      0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND   0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT     0x107
#endif
#endif

#include "g30_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Grainfather G30 Connect Bluetooth Identifiers
// =============================================================================
#define G30_DEVICE_NAME_PREFIX          "Grain"
#define G30_SERVICE_UUID128_STR         "0000cdd0-0000-1000-8000-00805f9b34fb"
#define G30_NOTIFY_CHR_UUID128_STR      "0003cdd1-0000-1000-8000-00805f9b0131"
#define G30_WRITE_CHR_UUID128_STR       "0003cdd2-0000-1000-8000-00805f9b0131"

// 16-bit UUID representations if using 16-bit aliases
#define G30_SERVICE_UUID16              0xCDD0
#define G30_NOTIFY_CHR_UUID16           0xCDD1
#define G30_WRITE_CHR_UUID16            0xCDD2

// Protocol invariants
#define G30_COMMAND_LEN                 19
#define G30_WRITE_PACING_MS             150

// Notification Type Codes
typedef enum {
    G30_NOTIF_UNKNOWN                   = 0,
    G30_NOTIF_TEMPERATURE               = 'X',
    G30_NOTIF_SYSTEM_STATE              = 'Y',
    G30_NOTIF_TIMER                     = 'T',
    G30_NOTIF_POWER_FLAGS               = 'W',
    G30_NOTIF_INTERACTION               = 'I',
    G30_NOTIF_BOIL_TEMP                 = 'C',
    G30_NOTIF_ERROR                     = 'E'
} g30_notification_type_t;

// =============================================================================
// API Functions
// =============================================================================

/**
 * @brief Parse an incoming ASCII notification buffer from characteristic 0003cdd1
 * @param data Pointer to raw BLE notification payload
 * @param len Length in bytes
 * @param[in,out] state Unified boiler state to update with parsed values
 * @param[out] out_type Detected notification type (or NULL if not needed)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG or ESP_ERR_NOT_SUPPORTED on parse failure
 */
esp_err_t g30_parse_notification(const uint8_t *data, size_t len,
                                 g30_state_t *state,
                                 g30_notification_type_t *out_type);

/**
 * @brief Format a string into a strictly padded 19-character G30 command buffer
 * @param cmd_str Input command string (null-terminated)
 * @param out_buf Buffer of at least 20 bytes (19 ASCII chars + null terminator)
 * @return ESP_OK on success, ESP_ERR_INVALID_SIZE if cmd_str > 19 chars
 */
esp_err_t g30_format_padded_cmd(const char *cmd_str, char out_buf[G30_COMMAND_LEN + 1]);

/**
 * @brief Encode a unified command struct into a 19-character G30 command
 * @param cmd Pointer to unified command
 * @param out_buf Buffer of at least 20 bytes
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if action not mapped
 */
esp_err_t g30_encode_command(const g30_command_t *cmd, char out_buf[G30_COMMAND_LEN + 1]);

/**
 * @brief Encode the recipe header command (Line 1): R{boil_min},{steps},{mash_water},{sparge_water},
 * @param recipe Pointer to recipe structure
 * @param out_buf Buffer of at least 20 bytes
 * @return ESP_OK on success
 */
esp_err_t g30_encode_recipe_header(const g30_recipe_t *recipe, char out_buf[G30_COMMAND_LEN + 1]);

/**
 * @brief Encode recipe configuration flags (Line 2): 0,1,1,0,0,
 * @details Flags: water_treat=0, sparge_counter=1, sparge_alert=1, delayed=0, skip_start=0
 * @param out_buf Buffer of at least 20 bytes
 * @return ESP_OK on success
 */
esp_err_t g30_encode_recipe_flags(char out_buf[G30_COMMAND_LEN + 1]);

/**
 * @brief Encode sanitized recipe name command (Line 3): 19-char uppercase ASCII space padded
 * @param name Recipe name string
 * @param out_buf Buffer of at least 20 bytes
 * @return ESP_OK on success
 */
esp_err_t g30_encode_recipe_name(const char *name, char out_buf[G30_COMMAND_LEN + 1]);

/**
 * @brief Encode boil parameters command (Line 4): {hop_stand_min},{boil_addition_count},0,0,
 * @param hop_stand_min Post-boil hop stand / whirlpool duration in minutes
 * @param boil_addition_count Number of unique boil additions to follow
 * @param out_buf Buffer of at least 20 bytes
 * @return ESP_OK on success
 */
esp_err_t g30_encode_recipe_boil_params(uint16_t hop_stand_min, uint8_t boil_addition_count, char out_buf[G30_COMMAND_LEN + 1]);

/**
 * @brief Encode a boil stop command (Line 5..): {time_left_min},
 * @param boil_time_remaining_min Boil time remaining in minutes
 * @param out_buf Buffer of at least 20 bytes
 * @return ESP_OK on success
 */
esp_err_t g30_encode_recipe_boil_stop(uint16_t boil_time_remaining_min, char out_buf[G30_COMMAND_LEN + 1]);

/**
 * @brief Encode a mash step command: {temp_c}:{duration_min}
 * @param step Pointer to mash step
 * @param out_buf Buffer of at least 20 bytes
 * @return ESP_OK on success
 */
esp_err_t g30_encode_recipe_mash_step(const g30_mash_step_t *step, char out_buf[G30_COMMAND_LEN + 1]);

/**
 * @brief Get a user-friendly description for a G30 interaction code
 * @param code Interaction code (0 - 5)
 * @return Static string message
 */
const char* g30_get_interaction_message(uint8_t code);

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_GRAINFATHER_G30_H
