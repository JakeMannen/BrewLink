/**
 * @file config_store.c
 * @brief Persistent Configuration Store (NVS) Implementation
 */

#include "config_store.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_random.h"
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

#define NVS_KEY_WIFI_SSID    "wifi_ssid"
#define NVS_KEY_WIFI_PASS    "wifi_pass"
#define NVS_KEY_MQTT_URI     "mqtt_uri"
#define NVS_KEY_MQTT_USER    "mqtt_user"
#define NVS_KEY_MQTT_PASS    "mqtt_pass"
#define NVS_KEY_MQTT_PREFIX  "mqtt_prefix"
#define NVS_KEY_API_TOKEN    "api_token"
#define NVS_KEY_TOPIC_KEY    "topic_key"
#define NVS_KEY_CLIENT_NAME  "client_name"
#define NVS_KEY_PAIRED_CLI   "paired_cli"
#define NVS_KEY_PAIRED_BLE   "paired_ble"

static brewlink_config_t s_cached_config;
static SemaphoreHandle_t s_config_mutex = NULL;
static bool s_initialized = false;

static void generate_random_hex(char *out_buf, size_t num_bytes) {
    static const char hex_chars[] = "0123456789abcdef";
    uint8_t rnd[32];
    if (num_bytes > sizeof(rnd)) {
        num_bytes = sizeof(rnd);
    }
    esp_fill_random(rnd, num_bytes);
    for (size_t i = 0; i < num_bytes; i++) {
        out_buf[i * 2]     = hex_chars[(rnd[i] >> 4) & 0x0F];
        out_buf[i * 2 + 1] = hex_chars[rnd[i] & 0x0F];
    }
    out_buf[num_bytes * 2] = '\0';
}

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

    required_size = sizeof(s_cached_config.api_token);
    if (nvs_get_str(handle, NVS_KEY_API_TOKEN, s_cached_config.api_token, &required_size) == ESP_OK &&
        strlen(s_cached_config.api_token) > 0) {
        s_cached_config.client_paired = true;
    }

    required_size = sizeof(s_cached_config.mqtt_topic_key);
    nvs_get_str(handle, NVS_KEY_TOPIC_KEY, s_cached_config.mqtt_topic_key, &required_size);

    required_size = sizeof(s_cached_config.paired_client_name);
    nvs_get_str(handle, NVS_KEY_CLIENT_NAME, s_cached_config.paired_client_name, &required_size);

    required_size = sizeof(s_cached_config.paired_clients);
    if (nvs_get_blob(handle, NVS_KEY_PAIRED_CLI, s_cached_config.paired_clients, &required_size) == ESP_OK) {
        bool any_active = false;
        for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_CLIENTS; i++) {
            if (s_cached_config.paired_clients[i].active) {
                any_active = true;
                strncpy(s_cached_config.api_token, s_cached_config.paired_clients[i].api_token, sizeof(s_cached_config.api_token) - 1);
                strncpy(s_cached_config.mqtt_topic_key, s_cached_config.paired_clients[i].mqtt_topic_key, sizeof(s_cached_config.mqtt_topic_key) - 1);
                strncpy(s_cached_config.paired_client_name, s_cached_config.paired_clients[i].client_name, sizeof(s_cached_config.paired_client_name) - 1);
            }
        }
        s_cached_config.client_paired = any_active;
    } else if (s_cached_config.client_paired && s_cached_config.api_token[0] != '\0') {
        s_cached_config.paired_clients[0].active = true;
        strncpy(s_cached_config.paired_clients[0].api_token, s_cached_config.api_token, CONFIG_STORE_TOKEN_MAX_LEN - 1);
        strncpy(s_cached_config.paired_clients[0].mqtt_topic_key, s_cached_config.mqtt_topic_key, CONFIG_STORE_TOPIC_KEY_MAX_LEN - 1);
        strncpy(s_cached_config.paired_clients[0].client_name,
                s_cached_config.paired_client_name[0] ? s_cached_config.paired_client_name : "External-Client-0001",
                CONFIG_STORE_CLIENT_NAME_MAX_LEN - 1);
    }

    required_size = sizeof(s_cached_config.paired_ble);
    if (nvs_get_blob(handle, NVS_KEY_PAIRED_BLE, s_cached_config.paired_ble, &required_size) == ESP_OK) {
        ESP_LOGI(TAG, "Loaded paired BLE peripheral table from NVS");
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

    // Check compile-time static API token fallback if not already paired in NVS
    if (!s_cached_config.client_paired) {
#ifdef BREWLINK_API_TOKEN
        if (strlen(BREWLINK_API_TOKEN) > 0) {
            strncpy(s_cached_config.api_token, BREWLINK_API_TOKEN, sizeof(s_cached_config.api_token) - 1);
            strncpy(s_cached_config.paired_client_name, "static-secrets-h", sizeof(s_cached_config.paired_client_name) - 1);
            s_cached_config.client_paired = true;
            ESP_LOGI(TAG, "Loaded static API token from secrets.h");
        }
#endif
    }

    // Ensure a default secret token exists in RAM/NVS so initial pairing or HMAC verification always has a key
    if (strlen(s_cached_config.api_token) == 0) {
        char rand_hex[57];
        generate_random_hex(rand_hex, 28);
        snprintf(s_cached_config.api_token, sizeof(s_cached_config.api_token), "bl_%s", rand_hex);
    }
    if (strlen(s_cached_config.mqtt_topic_key) == 0) {
        char rand_tk[17];
        generate_random_hex(rand_tk, 8);
        snprintf(s_cached_config.mqtt_topic_key, sizeof(s_cached_config.mqtt_topic_key), "tk_%s", rand_tk);
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

esp_err_t config_store_save_pairing(const char *api_token,
                                    const char *mqtt_topic_key,
                                    const char *client_name) {
    if (api_token == NULL || strlen(api_token) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const char *eff_name = (client_name != NULL && client_name[0] != '\0') ? client_name : "External-Client";

    // Add or update in paired_clients table
    int slot = -1;
    for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_CLIENTS; i++) {
        if (s_cached_config.paired_clients[i].active &&
            strcmp(s_cached_config.paired_clients[i].client_name, eff_name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_CLIENTS; i++) {
            if (!s_cached_config.paired_clients[i].active) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        slot = 0;
    }

    paired_client_t *c_entry = &s_cached_config.paired_clients[slot];
    memset(c_entry, 0, sizeof(paired_client_t));
    c_entry->active = true;
    strncpy(c_entry->client_name, eff_name, sizeof(c_entry->client_name) - 1);
    strncpy(c_entry->api_token, api_token, sizeof(c_entry->api_token) - 1);
    if (mqtt_topic_key != NULL && mqtt_topic_key[0] != '\0') {
        strncpy(c_entry->mqtt_topic_key, mqtt_topic_key, sizeof(c_entry->mqtt_topic_key) - 1);
    } else {
        strncpy(c_entry->mqtt_topic_key, s_cached_config.mqtt_topic_key, sizeof(c_entry->mqtt_topic_key) - 1);
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_config_mutex);
        ESP_LOGE(TAG, "Failed to open NVS for writing pairing credentials (err=0x%x)", err);
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_API_TOKEN, api_token);
    if (err == ESP_OK && mqtt_topic_key != NULL) {
        err = nvs_set_str(handle, NVS_KEY_TOPIC_KEY, mqtt_topic_key);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_CLIENT_NAME, eff_name);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, NVS_KEY_PAIRED_CLI, s_cached_config.paired_clients, sizeof(s_cached_config.paired_clients));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        strncpy(s_cached_config.api_token, api_token, sizeof(s_cached_config.api_token) - 1);
        s_cached_config.api_token[sizeof(s_cached_config.api_token) - 1] = '\0';

        if (mqtt_topic_key != NULL && strlen(mqtt_topic_key) > 0) {
            strncpy(s_cached_config.mqtt_topic_key, mqtt_topic_key, sizeof(s_cached_config.mqtt_topic_key) - 1);
            s_cached_config.mqtt_topic_key[sizeof(s_cached_config.mqtt_topic_key) - 1] = '\0';
        }
        strncpy(s_cached_config.paired_client_name, eff_name, sizeof(s_cached_config.paired_client_name) - 1);
        s_cached_config.paired_client_name[sizeof(s_cached_config.paired_client_name) - 1] = '\0';
        s_cached_config.client_paired = true;
        ESP_LOGI(TAG, "Persisted pairing token for external client '%s'", eff_name);
    }

    xSemaphoreGive(s_config_mutex);
    return err;
}

esp_err_t config_store_delete_paired_client(const char *client_name) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    bool removed = false;
    if (client_name == NULL || client_name[0] == '\0') {
        memset(s_cached_config.paired_clients, 0, sizeof(s_cached_config.paired_clients));
        removed = true;
    } else {
        for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_CLIENTS; i++) {
            if (s_cached_config.paired_clients[i].active &&
                strcmp(s_cached_config.paired_clients[i].client_name, client_name) == 0) {
                memset(&s_cached_config.paired_clients[i], 0, sizeof(paired_client_t));
                removed = true;
            }
        }
        if (!removed && s_cached_config.client_paired &&
            strcmp(s_cached_config.paired_client_name, client_name) == 0) {
            removed = true;
        }
    }

    if (!removed) {
        xSemaphoreGive(s_config_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    // Recompute active primary client state from remaining paired clients
    bool any_active = false;
    memset(s_cached_config.paired_client_name, 0, sizeof(s_cached_config.paired_client_name));
    for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_CLIENTS; i++) {
        if (s_cached_config.paired_clients[i].active) {
            any_active = true;
            strncpy(s_cached_config.api_token, s_cached_config.paired_clients[i].api_token, sizeof(s_cached_config.api_token) - 1);
            strncpy(s_cached_config.mqtt_topic_key, s_cached_config.paired_clients[i].mqtt_topic_key, sizeof(s_cached_config.mqtt_topic_key) - 1);
            strncpy(s_cached_config.paired_client_name, s_cached_config.paired_clients[i].client_name, sizeof(s_cached_config.paired_client_name) - 1);
        }
    }
    s_cached_config.client_paired = any_active;
    if (!any_active) {
        // Rotate internal token so deleted client tokens are immediately invalidated
        char rand_hex[57];
        generate_random_hex(rand_hex, 28);
        snprintf(s_cached_config.api_token, sizeof(s_cached_config.api_token), "bl_%s", rand_hex);
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        if (any_active) {
            nvs_set_str(handle, NVS_KEY_API_TOKEN, s_cached_config.api_token);
            nvs_set_str(handle, NVS_KEY_CLIENT_NAME, s_cached_config.paired_client_name);
        } else {
            nvs_erase_key(handle, NVS_KEY_API_TOKEN);
            nvs_erase_key(handle, NVS_KEY_CLIENT_NAME);
        }
        nvs_set_blob(handle, NVS_KEY_PAIRED_CLI, s_cached_config.paired_clients, sizeof(s_cached_config.paired_clients));
        nvs_commit(handle);
        nvs_close(handle);
    }

    xSemaphoreGive(s_config_mutex);
    return ESP_OK;
}

esp_err_t config_store_get_paired_clients(paired_client_t out_clients[CONFIG_STORE_MAX_PAIRED_CLIENTS]) {
    if (out_clients == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memcpy(out_clients, s_cached_config.paired_clients, sizeof(s_cached_config.paired_clients));
    xSemaphoreGive(s_config_mutex);
    return ESP_OK;
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

esp_err_t config_store_save_paired_ble(const uint8_t addr[6],
                                       uint8_t addr_type,
                                       uint8_t equipment_type,
                                       const char *name,
                                       const char *model) {
    if (addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int slot = -1;
    for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_BLE; i++) {
        if (s_cached_config.paired_ble[i].active &&
            memcmp(s_cached_config.paired_ble[i].addr, addr, 6) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_BLE; i++) {
            if (!s_cached_config.paired_ble[i].active) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        slot = 0; // Overwrite oldest slot if full
    }

    paired_ble_device_t *entry = &s_cached_config.paired_ble[slot];
    memset(entry, 0, sizeof(paired_ble_device_t));
    entry->active = true;
    memcpy(entry->addr, addr, 6);
    entry->addr_type = addr_type;
    entry->equipment_type = equipment_type;
    if (name != NULL && name[0] != '\0') {
        strncpy(entry->name, name, sizeof(entry->name) - 1);
    }
    if (model != NULL && model[0] != '\0') {
        strncpy(entry->model, model, sizeof(entry->model) - 1);
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, NVS_KEY_PAIRED_BLE, s_cached_config.paired_ble, sizeof(s_cached_config.paired_ble));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    xSemaphoreGive(s_config_mutex);
    return err;
}

esp_err_t config_store_remove_paired_ble(const uint8_t addr[6]) {
    if (addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    bool changed = false;
    for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_BLE; i++) {
        if (s_cached_config.paired_ble[i].active &&
            memcmp(s_cached_config.paired_ble[i].addr, addr, 6) == 0) {
            memset(&s_cached_config.paired_ble[i], 0, sizeof(paired_ble_device_t));
            changed = true;
        }
    }

    esp_err_t err = ESP_OK;
    if (changed) {
        nvs_handle_t handle;
        err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_blob(handle, NVS_KEY_PAIRED_BLE, s_cached_config.paired_ble, sizeof(s_cached_config.paired_ble));
            if (err == ESP_OK) {
                err = nvs_commit(handle);
            }
            nvs_close(handle);
        }
    }

    xSemaphoreGive(s_config_mutex);
    return err;
}

esp_err_t config_store_get_paired_ble(paired_ble_device_t out_devices[CONFIG_STORE_MAX_PAIRED_BLE]) {
    if (out_devices == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memcpy(out_devices, s_cached_config.paired_ble, sizeof(s_cached_config.paired_ble));
    xSemaphoreGive(s_config_mutex);
    return ESP_OK;
}
