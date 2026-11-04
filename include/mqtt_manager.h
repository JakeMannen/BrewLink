/**
 * @file mqtt_manager.h
 * @brief MQTT Client Subsystem for BrewLink Gateway
 */

#ifndef BREWLINK_MQTT_MANAGER_H
#define BREWLINK_MQTT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mqtt_inbound_cmd_cb_t)(const char *topic, const char *payload, int len);

/**
 * @brief Initialize MQTT manager
 * @param cmd_cb Callback for received MQTT commands
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t mqtt_manager_init(mqtt_inbound_cmd_cb_t cmd_cb);

/**
 * @brief Start MQTT client
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t mqtt_manager_start(void);

/**
 * @brief Stop MQTT client
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t mqtt_manager_stop(void);

/**
 * @brief Reconfigure MQTT client with new broker settings
 * @param uri New broker URI (e.g. "mqtt://192.168.1.100:1883")
 * @param username New username (optional)
 * @param password New password (optional)
 * @param prefix New topic prefix (optional)
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t mqtt_manager_reconfigure(const char *uri, const char *username,
                                   const char *password, const char *prefix);

/**
 * @brief Check if MQTT client is currently connected to broker
 * @return True if connected, false otherwise.
 */
bool mqtt_manager_is_connected(void);

/**
 * @brief Publish payload to MQTT topic
 * @param topic Target topic
 * @param data Payload data
 * @param len Payload length (0 for null-terminated string)
 * @param qos Quality of Service (0, 1, or 2)
 * @param retain Retain flag
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t mqtt_manager_publish(const char *topic, const char *data, int len, int qos, int retain);

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_MQTT_MANAGER_H
