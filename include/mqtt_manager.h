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
typedef void (*mqtt_conn_status_cb_t)(bool connected);

/**
 * @brief Initialize MQTT manager
 * @param gateway_id 12-char hex Gateway ID (or NULL to auto-derive from MAC)
 * @param cmd_cb Callback for received MQTT commands
 * @param conn_cb Optional callback for broker connection/disconnection events
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t mqtt_manager_init(const char *gateway_id,
                            mqtt_inbound_cmd_cb_t cmd_cb,
                            mqtt_conn_status_cb_t conn_cb);

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
 * @param uri New broker URI (e.g. "mqtt://192.168.1.100:1883" or "mqtts://...")
 * @param username New username (optional)
 * @param password New password / token (optional)
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
 * @brief Get Gateway ID used for topics and authentication
 * @return Gateway ID string
 */
const char *mqtt_manager_get_gateway_id(void);

/**
 * @brief Publish payload to raw MQTT topic
 * @param topic Target topic
 * @param data Payload data
 * @param len Payload length (0 for null-terminated string)
 * @param qos Quality of Service (0, 1, or 2)
 * @param retain Retain flag
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t mqtt_manager_publish(const char *topic, const char *data, int len, int qos, int retain);

/**
 * @brief Publish gateway status
 * @param status_json JSON payload
 * @return ESP_OK on success
 */
esp_err_t mqtt_manager_publish_gateway_status(const char *status_json);

/**
 * @brief Publish equipment discovery announcement
 * @param eq_mac Equipment MAC string (12 chars hex or XX:XX:XX:XX:XX:XX)
 * @param manifest_json JSON payload
 * @return ESP_OK on success
 */
esp_err_t mqtt_manager_publish_equipment_announce(const char *eq_mac, const char *manifest_json);

/**
 * @brief Publish retained JSON catalog of all paired/connected BLE equipment devices
 * @param list_json JSON payload formatted by equipment_manager_format_equipment_list_json
 * @return ESP_OK on success
 */
esp_err_t mqtt_manager_publish_equipment_list(const char *list_json);

/**
 * @brief Publish equipment telemetry
 * @param eq_mac Equipment MAC string
 * @param telemetry_json JSON payload
 * @return ESP_OK on success
 */
esp_err_t mqtt_manager_publish_equipment_telemetry(const char *eq_mac, const char *telemetry_json);

/**
 * @brief Publish equipment state resynchronization snapshot
 * @param eq_mac Equipment MAC string
 * @param snapshot_json JSON payload
 * @return ESP_OK on success
 */
esp_err_t mqtt_manager_publish_equipment_snapshot(const char *eq_mac, const char *snapshot_json);

/**
 * @brief Publish command acknowledgment response
 * @param eq_mac Equipment MAC string
 * @param req_id Correlation request ID string
 * @param action Action name executed
 * @param status Status string ("success", "rejected", "error")
 * @param error_code Integer error code (0 for success)
 * @param duration_ms Duration in ms to process
 * @return ESP_OK on success
 */
esp_err_t mqtt_manager_publish_cmd_ack(const char *eq_mac, const char *req_id,
                                       const char *action, const char *status,
                                       int error_code, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_MQTT_MANAGER_H
