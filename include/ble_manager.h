/**
 * @file ble_manager.h
 * @brief NimBLE Multi-Device BLE Central Subsystem for BrewLink Gateway
 */

#ifndef BREWLINK_BLE_MANAGER_H
#define BREWLINK_BLE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "config.h"
#include "ble_compatibility.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_SCAN_MAX_RESULTS        16

/**
 * @brief Callback for peer device lifecycle events (connect / disconnect)
 * @param peer Pointer to peer device information
 * @param connected True if connected, false if disconnected
 */
typedef void (*ble_peer_event_cb_t)(const ble_peer_device_t *peer, bool connected);

/**
 * @brief Callback for outbound BLE command or multi-packet recipe sequence completion
 * @param mac 6-byte target equipment MAC address
 * @param req_id Correlation request ID
 * @param action Command action name (e.g. "load_recipe", "set_pump")
 * @param status ESP_OK when final packet in sequence is written, or ESP_ERR_INVALID_STATE if aborted mid-transfer
 */
typedef void (*ble_cmd_seq_complete_cb_t)(const uint8_t mac[6],
                                          const char *req_id,
                                          const char *action,
                                          esp_err_t status);

/**
 * @brief Initialize NimBLE Central host and register queues
 * @param telemetry_queue Queue for inbound telemetry packets
 * @param cmd_queue Queue for outbound commands
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t ble_manager_init(QueueHandle_t telemetry_queue, QueueHandle_t cmd_queue);

/**
 * @brief Register callback for peer connection/disconnection lifecycle events
 * @param cb Callback function
 * @return ESP_OK on success
 */
esp_err_t ble_manager_register_peer_cb(ble_peer_event_cb_t cb);

/**
 * @brief Register callback for outbound command sequence completion / abort events
 * @param cb Callback function
 * @return ESP_OK on success
 */
esp_err_t ble_manager_register_seq_cb(ble_cmd_seq_complete_cb_t cb);

/**
 * @brief Start active BLE discovery scan
 * @param duration_sec Scan duration in seconds (0 for continuous default)
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t ble_manager_start_scan(uint32_t duration_sec);

/**
 * @brief Stop active BLE scan
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t ble_manager_stop_scan(void);

/**
 * @brief Get copy of discovered compatible BLE peripherals from recent scan
 * @param[out] out_entries Buffer for scan entries
 * @param[in] max_entries Maximum entries to copy
 * @param[out] count Actual count copied
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t ble_manager_get_scan_results(ble_scan_entry_t *out_entries, uint16_t max_entries, uint16_t *count);

/**
 * @brief Check if a discovered peripheral MAC address belongs to a compatible device defined in sourcecode
 * @param addr 6-byte target MAC address
 * @return true if device is verified compatible, false otherwise
 */
bool ble_manager_is_addr_compatible(const uint8_t *addr);

/**
 * @brief Initiate connection to a target peripheral by address
 * @note Only peripherals defined as compatible in sourcecode are connectable.
 * @param addr 6-byte target MAC address
 * @param addr_type Address type (0 = Public, 1 = Random)
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if device is not compatible, or ESP error code.
 */
esp_err_t ble_manager_connect(const uint8_t *addr, uint8_t addr_type);

/**
 * @brief Terminate connection by connection handle
 * @param conn_handle Connection handle
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t ble_manager_disconnect(uint16_t conn_handle);

/**
 * @brief Terminate connection by MAC address
 * @param addr 6-byte MAC address
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t ble_manager_disconnect_by_addr(const uint8_t *addr);

/**
 * @brief Get snapshot of all currently connected BLE peer devices
 * @param[out] out_peers Destination buffer
 * @param[in] max_peers Maximum entries to copy
 * @param[out] count Actual count of connected peers
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t ble_manager_get_peers(ble_peer_device_t *out_peers, uint8_t max_peers, uint8_t *count);

/**
 * @brief Get active connected count
 * @return Number of active connections (0 to BLE_MAX_SIMULTANEOUS_CONNECTIONS)
 */
uint8_t ble_manager_get_connected_count(void);

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_BLE_MANAGER_H
