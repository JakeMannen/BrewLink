/**
 * @file config_store.h
 * @brief Persistent Configuration Store (NVS) for BrewLink Gateway
 * @details Thread-safe management of Wi-Fi credentials and MQTT broker parameters.
 */

#ifndef BREWLINK_CONFIG_STORE_H
#define BREWLINK_CONFIG_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_STORE_SSID_MAX_LEN        33
#define CONFIG_STORE_PASS_MAX_LEN        65
#define CONFIG_STORE_URI_MAX_LEN         128
#define CONFIG_STORE_USER_MAX_LEN        64
#define CONFIG_STORE_TOPIC_MAX_LEN       64

/**
 * @brief In-memory cached configuration snapshot
 */
typedef struct {
    char wifi_ssid[CONFIG_STORE_SSID_MAX_LEN];
    char wifi_password[CONFIG_STORE_PASS_MAX_LEN];
    bool wifi_configured;

    char mqtt_broker_uri[CONFIG_STORE_URI_MAX_LEN];
    char mqtt_username[CONFIG_STORE_USER_MAX_LEN];
    char mqtt_password[CONFIG_STORE_PASS_MAX_LEN];
    char mqtt_topic_prefix[CONFIG_STORE_TOPIC_MAX_LEN];
} brewlink_config_t;

/**
 * @brief Initialize the configuration storage subsystem
 * @details Reads settings from NVS, falls back to secrets.h if NVS is empty.
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t config_store_init(void);

/**
 * @brief Get a thread-safe copy of the active configuration
 * @param[out] out_cfg Pointer to destination struct
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_cfg is NULL.
 */
esp_err_t config_store_get(brewlink_config_t *out_cfg);

/**
 * @brief Persist Wi-Fi credentials to NVS and update cache
 * @param[in] ssid Wi-Fi network SSID (null-terminated)
 * @param[in] password Wi-Fi network password (null-terminated, can be empty for open networks)
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t config_store_save_wifi(const char *ssid, const char *password);

/**
 * @brief Persist MQTT broker configuration to NVS and update cache
 * @param[in] broker_uri Broker URI (e.g., "mqtt://192.168.1.100:1883")
 * @param[in] username Broker username (optional, NULL or empty if unauthenticated)
 * @param[in] password Broker password (optional, NULL or empty)
 * @param[in] topic_prefix Custom topic prefix (optional, NULL defaults to "brewlink/")
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t config_store_save_mqtt(const char *broker_uri, const char *username,
                                 const char *password, const char *topic_prefix);

/**
 * @brief Clear Wi-Fi credentials from NVS
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t config_store_clear_wifi(void);

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_CONFIG_STORE_H
