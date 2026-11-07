/**
 * @file auth_manager.h
 * @brief External Client Pairing, Network Broadcast Discovery, Token Auth, and HMAC-SHA256
 * @details Supports 60-second code-free push-button pairing triggered via physical BOOT button
 *          (GPIO 0 held 3s) or Web UI, automatic UDP broadcast discovery (:53540) + MQTT
 *          discovery announcement, constant-time HTTP/WebSocket Bearer token checks, and
 *          zero-allocation HMAC-SHA256 command authentication over MQTT.
 */

#ifndef BREWLINK_AUTH_MANAGER_H
#define BREWLINK_AUTH_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK                  0
#define ESP_FAIL                -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_TIMEOUT         0x107
#endif
#endif

#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_PAIRING_PIN_LEN            6
#define AUTH_PAIRING_WINDOW_SEC         60
#define AUTH_BOOT_BTN_HOLD_MS           3000
#define AUTH_HMAC_HEX_LEN               64
#define AUTH_DISCOVERY_UDP_PORT         53540
#define AUTH_DISCOVERY_INTERVAL_MS      2000

/**
 * @brief Initialize authentication manager, GPIO 0 (BOOT button) 3s hold monitor, and UDP broadcaster
 * @return ESP_OK on success
 */
esp_err_t auth_manager_init(void);

/**
 * @brief Open a 60-second pairing window and begin broadcasting pairing availability over UDP & MQTT
 * @param[out] out_pin Optional buffer (at least 7 bytes) to receive an optional 6-digit code
 * @param[out] out_ttl_sec Optional pointer to receive window duration in seconds (60)
 * @return ESP_OK on success
 */
esp_err_t auth_manager_start_pairing(char out_pin[AUTH_PAIRING_PIN_LEN + 1], uint32_t *out_ttl_sec);

/**
 * @brief Claim an active pairing session (code-free push-button or optional PIN) and issue credentials
 * @param[in] pin Optional 6-digit PIN (pass NULL or "" for code-free push-button claim during open window)
 * @param[in] client_name Name of external system (e.g. "BeerXML Vault")
 * @param[out] out_token Buffer (at least CONFIG_STORE_TOKEN_MAX_LEN) to receive "bl_..." API token
 * @param[out] out_topic_key Buffer (at least CONFIG_STORE_TOPIC_KEY_MAX_LEN) to receive "tk_..." MQTT key
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if pairing window is closed/expired,
 *         ESP_ERR_INVALID_ARG if a non-empty PIN was provided and does not match.
 */
esp_err_t auth_manager_claim_pin(const char *pin,
                                 const char *client_name,
                                 char out_token[CONFIG_STORE_TOKEN_MAX_LEN],
                                 char out_topic_key[CONFIG_STORE_TOPIC_KEY_MAX_LEN]);

/**
 * @brief Format the JSON discovery beacon broadcast on UDP :53540 and MQTT brewlink/discovery/{gw_id}
 * @param[in] gateway_id Gateway hardware ID string
 * @param[in] ip_str Gateway local IPv4 address string
 * @param[in] http_port Gateway HTTP/WebSocket port (80)
 * @param[out] out_json Output buffer for JSON string
 * @param[in] max_len Size of out_json buffer
 * @return ESP_OK if pairing window is open and JSON formatted, ESP_ERR_INVALID_STATE if window closed
 */
esp_err_t auth_manager_format_discovery_beacon(const char *gateway_id,
                                               const char *ip_str,
                                               uint16_t http_port,
                                               char *out_json,
                                               size_t max_len);

/**
 * @brief Get current pairing window status and paired client info
 * @param[out] out_window_open True if 60s pairing window is currently active
 * @param[out] out_pin Buffer (7 bytes) to receive active PIN if window is open
 * @param[out] out_remaining_sec Remaining seconds in active pairing window
 * @param[out] out_client_paired True if at least one external client is paired
 * @param[out] out_client_name Buffer to receive most recently paired client's unique name
 * @return ESP_OK on success
 */
esp_err_t auth_manager_get_status(bool *out_window_open,
                                  char out_pin[AUTH_PAIRING_PIN_LEN + 1],
                                  uint32_t *out_remaining_sec,
                                  bool *out_client_paired,
                                  char out_client_name[CONFIG_STORE_CLIENT_NAME_MAX_LEN]);

/**
 * @brief Retrieve all active paired external clients
 * @param[out] out_clients Buffer of CONFIG_STORE_MAX_PAIRED_CLIENTS entries
 * @param[out] out_count Optional pointer to receive count of active paired clients
 * @return ESP_OK on success
 */
esp_err_t auth_manager_get_paired_clients(paired_client_t out_clients[CONFIG_STORE_MAX_PAIRED_CLIENTS],
                                          uint8_t *out_count);

/**
 * @brief Delete/revoke a paired external client by its unique client_name (or pass NULL / "" to delete all)
 * @param[in] client_name Unique name of the paired external client (e.g. "Brewfather-A4F2"), or NULL to clear all
 * @return ESP_OK if deleted, ESP_ERR_NOT_FOUND if client_name was not found
 */
esp_err_t auth_manager_delete_paired_client(const char *client_name);

/**
 * @brief Compute lowercase hex HMAC-SHA256(key, message) with zero heap allocation
 * @param[in] key Null-terminated secret key string
 * @param[in] message Null-terminated message string
 * @param[out] out_hex Output buffer of at least 65 bytes (64 hex chars + null)
 * @return ESP_OK on success
 */
esp_err_t auth_manager_compute_hmac_sha256_hex(const char *key,
                                               const char *message,
                                               char out_hex[AUTH_HMAC_HEX_LEN + 1]);

/**
 * @brief Constant-time verification of a candidate Bearer / API token against active token
 * @param[in] candidate_token Token string (may optionally start with "Bearer ")
 * @return true if valid and matches active token, false otherwise
 */
bool auth_manager_verify_token(const char *candidate_token);

/**
 * @brief Verify an inbound MQTT command using HMAC-SHA256 signature or fallback token
 * @details Message signed is "{req_id}:{action}:{timestamp}" using api_token as key.
 * @param[in] req_id Request correlation ID
 * @param[in] action Command action string (e.g. "load_recipe")
 * @param[in] timestamp Unix timestamp or nonce integer from auth block
 * @param[in] signature_hex 64-char hex HMAC-SHA256 signature (or NULL)
 * @param[in] plain_token Optional fallback plain token (or NULL)
 * @return true if authenticated (or if no client has paired yet and open local mode is active), false if rejected
 */
bool auth_manager_verify_mqtt_command(const char *req_id,
                                      const char *action,
                                      uint32_t timestamp,
                                      const char *signature_hex,
                                      const char *plain_token);

/**
 * @brief Override active token and pairing state in memory (used by native unit tests)
 */
void auth_manager_set_test_credentials(const char *api_token, const char *topic_key, bool paired);

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_AUTH_MANAGER_H
