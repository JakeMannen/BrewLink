/**
 * @file wifi_manager.h
 * @brief Wi-Fi Connection & SoftAP Onboarding Subsystem for BrewLink Gateway
 */

#ifndef BREWLINK_WIFI_MANAGER_H
#define BREWLINK_WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

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

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MGR_SSID_MAX_LEN              33
#define WIFI_MGR_IP_STR_MAX_LEN            16
#define WIFI_MGR_SCAN_MAX_RAW_RECORDS      40
#define WIFI_MGR_SCAN_MAX_UNIQUE_RECORDS   20

/**
 * @brief Information for an individual scanned Wi-Fi access point
 */
typedef struct {
    char    ssid[WIFI_MGR_SSID_MAX_LEN];
    int8_t  rssi;
    uint8_t authmode;
} wifi_mgr_ap_record_t;

/**
 * @brief Operational status snapshot for Wi-Fi subsystem
 */
typedef struct {
    bool    sta_connected;
    char    sta_ssid[WIFI_MGR_SSID_MAX_LEN];
    char    sta_ip[WIFI_MGR_IP_STR_MAX_LEN];
    int8_t  sta_rssi;

    bool    ap_active;
    char    ap_ssid[WIFI_MGR_SSID_MAX_LEN];
    char    ap_ip[WIFI_MGR_IP_STR_MAX_LEN];
    uint8_t ap_clients_count;
} wifi_mgr_status_t;

/**
 * @brief Callback invoked when Wi-Fi Station connection status changes
 * @param connected True if connected and IP assigned, false if disconnected
 * @param ip Station IP string (valid only when connected is true)
 */
typedef void (*wifi_manager_sta_cb_t)(bool connected, const char *ip);

/**
 * @brief Initialize Wi-Fi subsystem in APSTA mode
 * @details If no SSID is saved or if connection fails, broadcasts setup SoftAP ("BrewLink-Setup-XXXX").
 * @param sta_cb Callback for Station IP and connection events
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t wifi_manager_init(wifi_manager_sta_cb_t sta_cb);

/**
 * @brief Initiate active scan for nearby Wi-Fi access points
 * @param[out] out_records Dynamically allocated array of scan records (caller frees via wifi_manager_scan_free)
 * @param[out] out_count Number of records found
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t wifi_manager_scan(wifi_mgr_ap_record_t **out_records, uint16_t *out_count);

/**
 * @brief Filter and deduplicate scanned Wi-Fi access points by SSID
 * @details Retains only unique non-empty SSIDs, keeping the record with the strongest RSSI,
 *          and sorts the results in descending order by RSSI (strongest signal first).
 * @param[in] in_records Array of input scan records
 * @param in_count Number of input records
 * @param[out] out_records Output buffer for deduplicated records (pre-allocated by caller)
 * @param max_out Maximum capacity of out_records
 * @return Number of unique records stored in out_records
 */
uint16_t wifi_manager_deduplicate_ap_records(const wifi_mgr_ap_record_t *in_records,
                                             uint16_t in_count,
                                             wifi_mgr_ap_record_t *out_records,
                                             uint16_t max_out);

/**
 * @brief Free dynamically allocated scan results
 * @param records Pointer to array returned by wifi_manager_scan
 */
void wifi_manager_scan_free(wifi_mgr_ap_record_t *records);

/**
 * @brief Connect to a new Wi-Fi network and persist credentials
 * @param ssid Target SSID (null-terminated)
 * @param password Network password (null-terminated, can be empty for open networks)
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/**
 * @brief Query current operational status of Station and SoftAP interfaces
 * @param[out] out_status Pointer to destination status structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_status is NULL.
 */
esp_err_t wifi_manager_get_status(wifi_mgr_status_t *out_status);

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_WIFI_MANAGER_H
