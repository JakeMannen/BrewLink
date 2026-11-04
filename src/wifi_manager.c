/**
 * @file wifi_manager.c
 * @brief Wi-Fi Connection & SoftAP Onboarding Subsystem Implementation
 */

#include "wifi_manager.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config.h"
#include "config_store.h"

static const char *TAG = "WIFI_MGR";

static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap = NULL;

static wifi_manager_sta_cb_t s_sta_callback = NULL;
static SemaphoreHandle_t s_wifi_mutex = NULL;

static wifi_mgr_status_t s_status;
static int s_retry_count = 0;
static bool s_scanning = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "Wi-Fi Station started");
                if (s_status.sta_ssid[0] != '\0') {
                    ESP_LOGI(TAG, "Connecting to SSID: '%s'...", s_status.sta_ssid);
                    esp_wifi_connect();
                }
                break;

            case WIFI_EVENT_STA_CONNECTED: {
                wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
                ESP_LOGI(TAG, "Connected to AP: '%s' (Channel %d)", event->ssid, event->channel);
                s_retry_count = 0;
                break;
            }

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "Wi-Fi Station disconnected (reason=%d)", event->reason);

                if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    s_status.sta_connected = false;
                    memset(s_status.sta_ip, 0, sizeof(s_status.sta_ip));
                    xSemaphoreGive(s_wifi_mutex);
                }

                if (s_sta_callback != NULL) {
                    s_sta_callback(false, NULL);
                }

                if (s_retry_count < WIFI_STA_MAX_RETRIES) {
                    s_retry_count++;
                    ESP_LOGI(TAG, "Retrying Wi-Fi connection (%d/%d)...", s_retry_count, WIFI_STA_MAX_RETRIES);
                    esp_wifi_connect();
                } else {
                    ESP_LOGW(TAG, "Exceeded max Wi-Fi reconnect attempts (%d). SoftAP '%s' is broadcasting for setup.",
                             WIFI_STA_MAX_RETRIES, s_status.ap_ssid);
                }
                break;
            }

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
                ESP_LOGI(TAG, "Client joined SoftAP: MAC=%02X:%02X:%02X:%02X:%02X:%02X AID=%d",
                         event->mac[0], event->mac[1], event->mac[2],
                         event->mac[3], event->mac[4], event->mac[5], event->aid);
                if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    s_status.ap_clients_count++;
                    xSemaphoreGive(s_wifi_mutex);
                }
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                ESP_LOGI(TAG, "Client left SoftAP: MAC=%02X:%02X:%02X:%02X:%02X:%02X AID=%d",
                         event->mac[0], event->mac[1], event->mac[2],
                         event->mac[3], event->mac[4], event->mac[5], event->aid);
                if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    if (s_status.ap_clients_count > 0) {
                        s_status.ap_clients_count--;
                    }
                    xSemaphoreGive(s_wifi_mutex);
                }
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            char ip_str[WIFI_MGR_IP_STR_MAX_LEN];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "Wi-Fi Station Got IP: %s", ip_str);

            if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                s_status.sta_connected = true;
                strncpy(s_status.sta_ip, ip_str, sizeof(s_status.sta_ip) - 1);

                wifi_ap_record_t ap_info;
                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                    s_status.sta_rssi = ap_info.rssi;
                }
                xSemaphoreGive(s_wifi_mutex);
            }

            if (s_sta_callback != NULL) {
                s_sta_callback(true, ip_str);
            }
        }
    }
}

esp_err_t wifi_manager_init(wifi_manager_sta_cb_t sta_cb) {
    s_sta_callback = sta_cb;

    s_wifi_mutex = xSemaphoreCreateMutex();
    if (s_wifi_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_status, 0, sizeof(s_status));

    // Determine SoftAP SSID with MAC suffix: "BrewLink-Setup-XXXX"
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "%s-%02X%02X",
             WIFI_AP_SSID_PREFIX, mac[4], mac[5]);
    strncpy(s_status.ap_ip, "192.168.4.1", sizeof(s_status.ap_ip) - 1);

    // Initialize Station and AP network interfaces
    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Read stored configuration
    brewlink_config_t stored_cfg;
    config_store_get(&stored_cfg);

    // Prepare SoftAP configuration
    wifi_config_t ap_config;
    memset(&ap_config, 0, sizeof(ap_config));
    strncpy((char *)ap_config.ap.ssid, s_status.ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(s_status.ap_ssid);
    ap_config.ap.channel = WIFI_AP_CHANNEL;
    ap_config.ap.max_connection = WIFI_AP_MAX_CONN;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    // Use APSTA mode so AP is always available for onboarding/configuration
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    s_status.ap_active = true;

    if (stored_cfg.wifi_configured && strlen(stored_cfg.wifi_ssid) > 0) {
        wifi_config_t sta_config;
        memset(&sta_config, 0, sizeof(sta_config));
        strncpy((char *)sta_config.sta.ssid, stored_cfg.wifi_ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, stored_cfg.wifi_password, sizeof(sta_config.sta.password) - 1);
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

        strncpy(s_status.sta_ssid, stored_cfg.wifi_ssid, sizeof(s_status.sta_ssid) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        ESP_LOGI(TAG, "Configured Station for SSID: '%s'", s_status.sta_ssid);
    } else {
        ESP_LOGI(TAG, "No Wi-Fi credentials found. SoftAP '%s' active for onboarding at 192.168.4.1", s_status.ap_ssid);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi subsystem initialized. SoftAP '%s' is broadcasting.", s_status.ap_ssid);

    return ESP_OK;
}

esp_err_t wifi_manager_scan(wifi_mgr_ap_record_t **out_records, uint16_t *out_count) {
    if (out_records == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_scanning) {
        return ESP_ERR_INVALID_STATE;
    }
    s_scanning = true;

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE
    };

    ESP_LOGI(TAG, "Starting Wi-Fi scan...");
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: 0x%x", err);
        s_scanning = false;
        return err;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count == 0) {
        *out_records = NULL;
        *out_count = 0;
        s_scanning = false;
        return ESP_OK;
    }

    // Limit to 20 AP records to conserve heap
    if (ap_count > 20) {
        ap_count = 20;
    }

    wifi_ap_record_t *raw_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (raw_records == NULL) {
        s_scanning = false;
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, raw_records);
    if (err != ESP_OK) {
        free(raw_records);
        s_scanning = false;
        return err;
    }

    wifi_mgr_ap_record_t *records = malloc(sizeof(wifi_mgr_ap_record_t) * ap_count);
    if (records == NULL) {
        free(raw_records);
        s_scanning = false;
        return ESP_ERR_NO_MEM;
    }

    for (uint16_t i = 0; i < ap_count; i++) {
        strncpy(records[i].ssid, (char *)raw_records[i].ssid, sizeof(records[i].ssid) - 1);
        records[i].ssid[sizeof(records[i].ssid) - 1] = '\0';
        records[i].rssi = raw_records[i].rssi;
        records[i].authmode = raw_records[i].authmode;
    }

    free(raw_records);
    *out_records = records;
    *out_count = ap_count;
    s_scanning = false;

    ESP_LOGI(TAG, "Wi-Fi scan completed. Found %d networks.", ap_count);
    return ESP_OK;
}

void wifi_manager_scan_free(wifi_mgr_ap_record_t *records) {
    if (records != NULL) {
        free(records);
    }
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password) {
    if (ssid == NULL || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Connecting to new Wi-Fi network: '%s'...", ssid);

    // Save credentials to NVS
    esp_err_t err = config_store_save_wifi(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist credentials to NVS: 0x%x", err);
    }

    wifi_config_t sta_config;
    memset(&sta_config, 0, sizeof(sta_config));
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    if (password != NULL) {
        strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    }
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        strncpy(s_status.sta_ssid, ssid, sizeof(s_status.sta_ssid) - 1);
        s_status.sta_connected = false;
        s_retry_count = 0;
        xSemaphoreGive(s_wifi_mutex);
    }

    // Disconnect existing if active, reconfigure, and reconnect
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    return esp_wifi_connect();
}

esp_err_t wifi_manager_get_status(wifi_mgr_status_t *out_status) {
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_status.sta_connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_status.sta_rssi = ap_info.rssi;
        }
    }

    memcpy(out_status, &s_status, sizeof(wifi_mgr_status_t));
    xSemaphoreGive(s_wifi_mutex);
    return ESP_OK;
}
