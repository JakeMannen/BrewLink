/**
 * @file config_store.c
 * @brief Persistent Configuration Store (NVS) Implementation
 */

#include "config_store.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

static const char *TAG = "CONFIG_STORE";
static const char *NVS_NAMESPACE = "brewlink_cfg";

#define NVS_KEY_WIFI_SSID   "wifi_ssid"
#define NVS_KEY_WIFI_PASS   "wifi_pass"
#define NVS_KEY_MQTT_URI    "mqtt_uri"
#define NVS_KEY_MQTT_USER   "mqtt_user"
#define NVS_KEY_MQTT_PASS   "mqtt_pass"
#define NVS_KEY_MQTT_PREFIX "mqtt_prefix"

static brewlink_config_t s_cached_config;
static SemaphoreHandle_t s_config_mutex = NULL;
static bool s_initialized = false;

static esp_err_t load_from_nvs(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No NVS configuration found or failed to open (err=0x%x). Checking compile-time defaults...", err);
        return err;
    }

    size_t required_size = sizeof(s_cached_config.wifi_ssid);
    err = nvs_get_str(handle, NVS_KEY_WIFI_SSID, s_cached_config.wifi_ssid, &required_size);
    if (err == ESP_OK && strlen(s_cached_config.wifi_ssid) > 0) {
        s_cached_config.wifi_configured = true;
        required_size = sizeof(s_cached_config.wifi_password);
        nvs_get_str(handle, NVS_KEY_WIFI_PASS, s_cached_config.wifi_password, &required_size);
        ESP_LOGI(TAG, "Loaded Wi-Fi SSID from NVS: '%s'", s_cached_config.wifi_ssid);
    } else {
        s_cached_config.wifi_configured = false;
    }

    required_size = sizeof(s_cached_config.mqtt_broker_uri);
    if (nvs_get_str(handle, NVS_KEY_MQTT_URI, s_cached_config.mqtt_broker_uri, &required_size) == ESP_OK) {
        ESP_LOGI(TAG, "Loaded MQTT Broker URI from NVS: '%s'", s_cached_config.mqtt_broker_uri);
    }

    required_size = sizeof(s_cached_config.mqtt_username);
    nvs_get_str(handle, NVS_KEY_MQTT_USER, s_cached_config.mqtt_username, &required_size);

    required_size = sizeof(s_cached_config.mqtt_password);
    nvs_get_str(handle, NVS_KEY_MQTT_PASS, s_cached_config.mqtt_password, &required_size);

    required_size = sizeof(s_cached_config.mqtt_topic_prefix);
    if (nvs_get_str(handle, NVS_KEY_MQTT_PREFIX, s_cached_config.mqtt_topic_prefix, &required_size) != ESP_OK ||
        strlen(s_cached_config.mqtt_topic_prefix) == 0) {
        strncpy(s_cached_config.mqtt_topic_prefix, "brewlink/", sizeof(s_cached_config.mqtt_topic_prefix) - 1);
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t config_store_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    s_config_mutex = xSemaphoreCreateMutex();
    if (s_config_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create config mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(&s_cached_config, 0, sizeof(s_cached_config));

    // Try reading from NVS
    esp_err_t err = load_from_nvs();
    if (err != ESP_OK || !s_cached_config.wifi_configured) {
        // Evaluate compile-time defaults from secrets.h / secrets.example.h
#ifdef WIFI_SSID
        if (strcmp(WIFI_SSID, "Your_WiFi_SSID") != 0 && strlen(WIFI_SSID) > 0) {
            strncpy(s_cached_config.wifi_ssid, WIFI_SSID, sizeof(s_cached_config.wifi_ssid) - 1);
#ifdef WIFI_PASSWORD
            strncpy(s_cached_config.wifi_password, WIFI_PASSWORD, sizeof(s_cached_config.wifi_password) - 1);
#endif
            s_cached_config.wifi_configured = true;
            ESP_LOGI(TAG, "Initialized default Wi-Fi SSID from header: '%s'", s_cached_config.wifi_ssid);
        } else {
            s_cached_config.wifi_configured = false;
            ESP_LOGI(TAG, "No Wi-Fi SSID configured. Gateway will activate SoftAP onboarding mode.");
        }
#endif

#ifdef MQTT_BROKER_URI
        if (strlen(s_cached_config.mqtt_broker_uri) == 0) {
            strncpy(s_cached_config.mqtt_broker_uri, MQTT_BROKER_URI, sizeof(s_cached_config.mqtt_broker_uri) - 1);
        }
#endif
#ifdef MQTT_USERNAME
        if (strlen(s_cached_config.mqtt_username) == 0) {
            strncpy(s_cached_config.mqtt_username, MQTT_USERNAME, sizeof(s_cached_config.mqtt_username) - 1);
        }
#endif
#ifdef MQTT_PASSWORD
        if (strlen(s_cached_config.mqtt_password) == 0) {
            strncpy(s_cached_config.mqtt_password, MQTT_PASSWORD, sizeof(s_cached_config.mqtt_password) - 1);
        }
#endif
        if (strlen(s_cached_config.mqtt_topic_prefix) == 0) {
            strncpy(s_cached_config.mqtt_topic_prefix, "brewlink/", sizeof(s_cached_config.mqtt_topic_prefix) - 1);
        }
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t config_store_get(brewlink_config_t *out_cfg) {
    if (out_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memcpy(out_cfg, &s_cached_config, sizeof(brewlink_config_t));
    xSemaphoreGive(s_config_mutex);
    return ESP_OK;
}

esp_err_t config_store_save_wifi(const char *ssid, const char *password) {
    if (ssid == NULL || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_config_mutex);
        ESP_LOGE(TAG, "Failed to open NVS for writing (err=0x%x)", err);
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_WIFI_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_WIFI_PASS, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        strncpy(s_cached_config.wifi_ssid, ssid, sizeof(s_cached_config.wifi_ssid) - 1);
        s_cached_config.wifi_ssid[sizeof(s_cached_config.wifi_ssid) - 1] = '\0';

        strncpy(s_cached_config.wifi_password, password ? password : "", sizeof(s_cached_config.wifi_password) - 1);
        s_cached_config.wifi_password[sizeof(s_cached_config.wifi_password) - 1] = '\0';

        s_cached_config.wifi_configured = true;
        ESP_LOGI(TAG, "Successfully persisted Wi-Fi credentials for SSID: '%s'", ssid);
    } else {
        ESP_LOGE(TAG, "Failed to persist Wi-Fi credentials to NVS: 0x%x", err);
    }

    xSemaphoreGive(s_config_mutex);
    return err;
}

esp_err_t config_store_save_mqtt(const char *broker_uri, const char *username,
                                 const char *password, const char *topic_prefix) {
    if (broker_uri == NULL || strlen(broker_uri) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_config_mutex);
        ESP_LOGE(TAG, "Failed to open NVS for writing MQTT settings (err=0x%x)", err);
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_MQTT_URI, broker_uri);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_MQTT_USER, username ? username : "");
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_MQTT_PASS, password ? password : "");
    }
    if (err == ESP_OK && topic_prefix && strlen(topic_prefix) > 0) {
        err = nvs_set_str(handle, NVS_KEY_MQTT_PREFIX, topic_prefix);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        strncpy(s_cached_config.mqtt_broker_uri, broker_uri, sizeof(s_cached_config.mqtt_broker_uri) - 1);
        strncpy(s_cached_config.mqtt_username, username ? username : "", sizeof(s_cached_config.mqtt_username) - 1);
        strncpy(s_cached_config.mqtt_password, password ? password : "", sizeof(s_cached_config.mqtt_password) - 1);
        if (topic_prefix && strlen(topic_prefix) > 0) {
            strncpy(s_cached_config.mqtt_topic_prefix, topic_prefix, sizeof(s_cached_config.mqtt_topic_prefix) - 1);
        }
        ESP_LOGI(TAG, "Successfully persisted MQTT settings: URI '%s'", broker_uri);
    } else {
        ESP_LOGE(TAG, "Failed to persist MQTT settings to NVS: 0x%x", err);
    }

    xSemaphoreGive(s_config_mutex);
    return err;
}

esp_err_t config_store_clear_wifi(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_key(handle, NVS_KEY_WIFI_SSID);
        nvs_erase_key(handle, NVS_KEY_WIFI_PASS);
        nvs_commit(handle);
        nvs_close(handle);
    }

    memset(s_cached_config.wifi_ssid, 0, sizeof(s_cached_config.wifi_ssid));
    memset(s_cached_config.wifi_password, 0, sizeof(s_cached_config.wifi_password));
    s_cached_config.wifi_configured = false;

    xSemaphoreGive(s_config_mutex);
    return ESP_OK;
}
