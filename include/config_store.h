/**
 * @file config_store.h
 * @brief Persistent Configuration Store (NVS) for BrewLink Gateway
 * @details Thread-safe management of Wi-Fi credentials, MQTT broker parameters,
 *          and external client pairing secrets.
 */

#ifndef BREWLINK_CONFIG_STORE_H
#define BREWLINK_CONFIG_STORE_H

#include <stdint.h>
#include <stdbool.h>
#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL        -1
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_STORE_SSID_MAX_LEN        33
#define CONFIG_STORE_PASS_MAX_LEN        65
#define CONFIG_STORE_URI_MAX_LEN         128
#define CONFIG_STORE_USER_MAX_LEN        64
#define CONFIG_STORE_TOPIC_MAX_LEN       64
#define CONFIG_STORE_TOKEN_MAX_LEN       68
#define CONFIG_STORE_TOPIC_KEY_MAX_LEN   24
#define CONFIG_STORE_CLIENT_NAME_MAX_LEN 32
#define CONFIG_STORE_MAX_PAIRED_CLIENTS  8
#define CONFIG_STORE_MAX_PAIRED_BLE      3
#define CONFIG_STORE_BLE_NAME_MAX_LEN    32

/**
 * @brief Persistent record for a paired external client
 */
typedef struct {
    bool active;
    char client_name[CONFIG_STORE_CLIENT_NAME_MAX_LEN];
    char api_token[CONFIG_STORE_TOKEN_MAX_LEN];
    char mqtt_topic_key[CONFIG_STORE_TOPIC_KEY_MAX_LEN];
} paired_client_t;

/**
 * @brief Persistent record for a paired BLE peripheral device
 */
typedef struct {
    bool    active;
    uint8_t addr[6];
    uint8_t addr_type;
    uint8_t equipment_type;
    char    name[CONFIG_STORE_BLE_NAME_MAX_LEN];
    char    model[CONFIG_STORE_BLE_NAME_MAX_LEN];
} paired_ble_device_t;

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

    char api_token[CONFIG_STORE_TOKEN_MAX_LEN];
    char mqtt_topic_key[CONFIG_STORE_TOPIC_KEY_MAX_LEN];
    char paired_client_name[CONFIG_STORE_CLIENT_NAME_MAX_LEN];
    bool client_paired;

    paired_client_t     paired_clients[CONFIG_STORE_MAX_PAIRED_CLIENTS];
    paired_ble_device_t paired_ble[CONFIG_STORE_MAX_PAIRED_BLE];
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
 * @brief Persist external client pairing credentials to NVS and update cache
 * @param[in] api_token Generated 64-char hex API token (e.g. "bl_...")
 * @param[in] mqtt_topic_key Generated secret topic key (e.g. "tk_...")
 * @param[in] client_name Name of the paired external system
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t config_store_save_pairing(const char *api_token,
                                    const char *mqtt_topic_key,
                                    const char *client_name);

/**
 * @brief Delete a paired external client by its unique client_name (or pass NULL / "" to delete all)
 * @param[in] client_name Unique name of the paired external client to remove, or NULL to clear all
 * @return ESP_OK if deleted, ESP_ERR_NOT_FOUND if client_name was not found
 */
esp_err_t config_store_delete_paired_client(const char *client_name);

/**
 * @brief Retrieve all persisted paired external clients (up to CONFIG_STORE_MAX_PAIRED_CLIENTS)
 * @param[out] out_clients Array of CONFIG_STORE_MAX_PAIRED_CLIENTS entries
 * @return ESP_OK on success
 */
esp_err_t config_store_get_paired_clients(paired_client_t out_clients[CONFIG_STORE_MAX_PAIRED_CLIENTS]);

/**
 * @brief Clear Wi-Fi credentials from NVS
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t config_store_clear_wifi(void);

/**
 * @brief Save or update a paired BLE equipment device in NVS for automatic reconnection
 * @param addr 6-byte BLE MAC address
 * @param addr_type Address type (0 = Public, 1 = Random)
 * @param equipment_type Equipment type classification
 * @param name Unique device name
 * @param model Device model string
 * @return ESP_OK on success
 */
esp_err_t config_store_save_paired_ble(const uint8_t addr[6],
                                       uint8_t addr_type,
                                       uint8_t equipment_type,
                                       const char *name,
                                       const char *model);

/**
 * @brief Remove a paired BLE equipment device from NVS (e.g. when user explicitly disconnects/forgets)
 * @param addr 6-byte BLE MAC address
 * @return ESP_OK on success
 */
esp_err_t config_store_remove_paired_ble(const uint8_t addr[6]);

/**
 * @brief Retrieve all persisted paired BLE equipment devices (up to CONFIG_STORE_MAX_PAIRED_BLE)
 * @param[out] out_devices Array of CONFIG_STORE_MAX_PAIRED_BLE entries
 * @return ESP_OK on success
 */
esp_err_t config_store_get_paired_ble(paired_ble_device_t out_devices[CONFIG_STORE_MAX_PAIRED_BLE]);

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_CONFIG_STORE_H
