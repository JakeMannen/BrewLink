/**
 * @file web_server.c
 * @brief Embedded Web Configuration Server Implementation
 */

#include "web_server.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "cJSON.h"

#include "config.h"
#include "config_store.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "ble_manager.h"
#include "web_page.h"

static const char *TAG = "WEB_SERVER";
static httpd_handle_t s_server = NULL;

// Helper: parse MAC string "XX:XX:XX:XX:XX:XX" to 6-byte array
static bool parse_mac_string(const char *str, uint8_t *mac) {
    if (str == NULL || mac == NULL) return false;
    int values[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            mac[i] = (uint8_t)values[i];
        }
        return true;
    }
    return false;
}

static const char *equipment_type_to_str(equipment_type_t type) {
    switch (type) {
        case EQUIPMENT_TYPE_BOILER:    return "boiler";
        case EQUIPMENT_TYPE_FERMENTER: return "fermenter";
        case EQUIPMENT_TYPE_COOLER_HEATER: return "cooler";
        default:                       return "generic";
    }
}

// -----------------------------------------------------------------------------
// HTTP Handlers
// -----------------------------------------------------------------------------

// GET / - Web UI Root
static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_send(req, PAGE_INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// Captive Portal Redirect Handler
static esp_err_t captive_portal_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

// GET /api/status - System status overview
static esp_err_t api_status_get_handler(httpd_req_t *req) {
    wifi_mgr_status_t wifi_stat;
    wifi_manager_get_status(&wifi_stat);

    brewlink_config_t cfg;
    config_store_get(&cfg);

    cJSON *root = cJSON_CreateObject();

    // Wi-Fi object
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", wifi_stat.sta_connected);
    cJSON_AddStringToObject(wifi, "ssid", wifi_stat.sta_ssid);
    cJSON_AddStringToObject(wifi, "ip", wifi_stat.sta_ip);
    cJSON_AddNumberToObject(wifi, "rssi", wifi_stat.sta_rssi);
    cJSON_AddBoolToObject(wifi, "ap_active", wifi_stat.ap_active);
    cJSON_AddStringToObject(wifi, "ap_ssid", wifi_stat.ap_ssid);
    cJSON_AddStringToObject(wifi, "ap_ip", wifi_stat.ap_ip);
    cJSON_AddNumberToObject(wifi, "ap_clients", wifi_stat.ap_clients_count);

    // MQTT object
    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddBoolToObject(mqtt, "connected", mqtt_manager_is_connected());
    cJSON_AddStringToObject(mqtt, "broker_uri", cfg.mqtt_broker_uri);

    // BLE object
    cJSON *ble = cJSON_AddObjectToObject(root, "ble");
    cJSON_AddNumberToObject(ble, "connected_count", ble_manager_get_connected_count());
    cJSON_AddNumberToObject(ble, "max_connections", BLE_MAX_SIMULTANEOUS_CONNECTIONS);

    // System object
    cJSON *sys = cJSON_AddObjectToObject(root, "system");
    cJSON_AddNumberToObject(sys, "uptime_s", (double)(esp_timer_get_time() / 1000000ULL));
    cJSON_AddNumberToObject(sys, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(sys, "min_free_heap", esp_get_minimum_free_heap_size());

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    return ret;
}

// GET /api/wifi/scan - Trigger and return Wi-Fi AP scan
static esp_err_t api_wifi_scan_get_handler(httpd_req_t *req) {
    wifi_mgr_ap_record_t *records = NULL;
    uint16_t count = 0;

    esp_err_t err = wifi_manager_scan(&records, &count);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateArray();
    if (records != NULL) {
        for (uint16_t i = 0; i < count; i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "ssid", records[i].ssid);
            cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
            cJSON_AddNumberToObject(item, "auth", records[i].authmode);
            cJSON_AddItemToArray(root, item);
        }
        wifi_manager_scan_free(records);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    return ret;
}

// POST /api/wifi/connect - Connect to Wi-Fi SSID
static esp_err_t api_wifi_connect_post_handler(httpd_req_t *req) {
    char buf[256];
    int remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");

    if (!cJSON_IsString(ssid_item) || (ssid_item->valuestring == NULL)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'ssid'");
        return ESP_FAIL;
    }

    const char *ssid = ssid_item->valuestring;
    const char *pass = (pass_item && cJSON_IsString(pass_item)) ? pass_item->valuestring : "";

    ESP_LOGI(TAG, "Web request to connect to Wi-Fi: '%s'", ssid);
    wifi_manager_connect(ssid, pass);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"connecting\",\"message\":\"Attempting connection\"}", HTTPD_RESP_USE_STRLEN);
}

// GET /api/mqtt - Get MQTT config
static esp_err_t api_mqtt_get_handler(httpd_req_t *req) {
    brewlink_config_t cfg;
    config_store_get(&cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "broker_uri", cfg.mqtt_broker_uri);
    cJSON_AddStringToObject(root, "username", cfg.mqtt_username);
    cJSON_AddStringToObject(root, "topic_prefix", cfg.mqtt_topic_prefix);
    cJSON_AddBoolToObject(root, "connected", mqtt_manager_is_connected());

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    return ret;
}

// POST /api/mqtt - Update MQTT broker configuration
static esp_err_t api_mqtt_post_handler(httpd_req_t *req) {
    char buf[512];
    int remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *uri_item = cJSON_GetObjectItem(root, "broker_uri");
    cJSON *user_item = cJSON_GetObjectItem(root, "username");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");
    cJSON *prefix_item = cJSON_GetObjectItem(root, "topic_prefix");

    if (!cJSON_IsString(uri_item) || (uri_item->valuestring == NULL)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'broker_uri'");
        return ESP_FAIL;
    }

    const char *uri = uri_item->valuestring;
    const char *user = (user_item && cJSON_IsString(user_item)) ? user_item->valuestring : "";
    const char *pass = (pass_item && cJSON_IsString(pass_item)) ? pass_item->valuestring : "";
    const char *prefix = (prefix_item && cJSON_IsString(prefix_item)) ? prefix_item->valuestring : "brewlink/";

    mqtt_manager_reconfigure(uri, user, pass, prefix);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"MQTT settings updated\"}", HTTPD_RESP_USE_STRLEN);
}

// GET /api/ble/peers - Connected BLE devices
static esp_err_t api_ble_peers_get_handler(httpd_req_t *req) {
    ble_peer_device_t peers[BLE_MAX_SIMULTANEOUS_CONNECTIONS];
    uint8_t count = 0;

    ble_manager_get_peers(peers, BLE_MAX_SIMULTANEOUS_CONNECTIONS, &count);

    cJSON *root = cJSON_CreateArray();
    for (uint8_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "slot", i + 1);
        cJSON_AddNumberToObject(item, "conn_handle", peers[i].conn_handle);

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 peers[i].addr[0], peers[i].addr[1], peers[i].addr[2],
                 peers[i].addr[3], peers[i].addr[4], peers[i].addr[5]);
        cJSON_AddStringToObject(item, "addr", mac_str);
        cJSON_AddStringToObject(item, "name", peers[i].device_name);
        cJSON_AddNumberToObject(item, "rssi", peers[i].rssi);
        cJSON_AddStringToObject(item, "equipment_type", equipment_type_to_str(peers[i].equipment_type));
        cJSON_AddBoolToObject(item, "connected", peers[i].connected);

        cJSON_AddItemToArray(root, item);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    return ret;
}

// GET /api/ble/scan - Discovered BLE devices
static esp_err_t api_ble_scan_get_handler(httpd_req_t *req) {
    // Trigger active scan if needed
    ble_manager_start_scan(5);

    // Yield short delay to collect immediate advertisements
    vTaskDelay(pdMS_TO_TICKS(1500));

    ble_scan_entry_t entries[BLE_SCAN_MAX_RESULTS];
    uint16_t count = 0;
    ble_manager_get_scan_results(entries, BLE_SCAN_MAX_RESULTS, &count);

    cJSON *root = cJSON_CreateArray();
    for (uint16_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 entries[i].addr[0], entries[i].addr[1], entries[i].addr[2],
                 entries[i].addr[3], entries[i].addr[4], entries[i].addr[5]);
        cJSON_AddStringToObject(item, "addr", mac_str);
        cJSON_AddNumberToObject(item, "addr_type", entries[i].addr_type);
        cJSON_AddStringToObject(item, "name", entries[i].name);
        cJSON_AddNumberToObject(item, "rssi", entries[i].rssi);
        cJSON_AddBoolToObject(item, "is_grainfather", entries[i].is_grainfather);
        cJSON_AddBoolToObject(item, "is_compatible", entries[i].is_compatible);
        cJSON_AddStringToObject(item, "model", entries[i].model);
        cJSON_AddStringToObject(item, "equipment_type", equipment_type_to_str(entries[i].equipment_type));
        cJSON_AddBoolToObject(item, "is_connected", entries[i].is_connected);

        cJSON_AddItemToArray(root, item);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    return ret;
}

// POST /api/ble/connect - Connect to BLE device by MAC
static esp_err_t api_ble_connect_post_handler(httpd_req_t *req) {
    char buf[128];
    int remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *addr_item = cJSON_GetObjectItem(root, "addr");
    cJSON *type_item = cJSON_GetObjectItem(root, "addr_type");

    if (!cJSON_IsString(addr_item) || (addr_item->valuestring == NULL)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'addr'");
        return ESP_FAIL;
    }

    uint8_t mac[6];
    if (!parse_mac_string(addr_item->valuestring, mac)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid MAC format");
        return ESP_FAIL;
    }

    uint8_t addr_type = (type_item && cJSON_IsNumber(type_item)) ? (uint8_t)type_item->valueint : 0;
    esp_err_t err = ble_manager_connect(mac, addr_type);
    cJSON_Delete(root);

    if (err == ESP_ERR_NOT_SUPPORTED) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device is not a compatible equipment device defined in sourcecode");
        return ESP_FAIL;
    } else if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to initiate BLE connection");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"connecting\",\"message\":\"BLE connection initiated to compatible device\"}", HTTPD_RESP_USE_STRLEN);
}

// POST /api/ble/disconnect - Disconnect BLE device
static esp_err_t api_ble_disconnect_post_handler(httpd_req_t *req) {
    char buf[128];
    int remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *handle_item = cJSON_GetObjectItem(root, "conn_handle");
    cJSON *addr_item = cJSON_GetObjectItem(root, "addr");

    if (handle_item && cJSON_IsNumber(handle_item)) {
        ble_manager_disconnect((uint16_t)handle_item->valueint);
    } else if (addr_item && cJSON_IsString(addr_item)) {
        uint8_t mac[6];
        if (parse_mac_string(addr_item->valuestring, mac)) {
            ble_manager_disconnect_by_addr(mac);
        }
    }

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"BLE disconnect initiated\"}", HTTPD_RESP_USE_STRLEN);
}

// POST /api/system/restart - Reboot ESP32
static void delayed_restart_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t api_system_restart_post_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Rebooting gateway...\"}", HTTPD_RESP_USE_STRLEN);

    xTaskCreate(delayed_restart_task, "restart_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Web Server Lifecycle
// -----------------------------------------------------------------------------
esp_err_t web_server_start(void) {
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.core_id = NETWORKING_TASK_CORE;
    config.stack_size = WEB_SERVER_STACK_SIZE;
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP configuration server on port %d...", config.server_port);
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: 0x%x", err);
        return err;
    }

    // URI Handlers
    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_root);

    httpd_uri_t uri_index = {
        .uri = "/index.html",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_index);

    // Captive portal probes
    httpd_uri_t uri_cp1 = {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_portal_handler};
    httpd_register_uri_handler(s_server, &uri_cp1);

    httpd_uri_t uri_cp2 = {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_portal_handler};
    httpd_register_uri_handler(s_server, &uri_cp2);

    httpd_uri_t uri_cp3 = {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_portal_handler};
    httpd_register_uri_handler(s_server, &uri_cp3);

    httpd_uri_t uri_cp4 = {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_portal_handler};
    httpd_register_uri_handler(s_server, &uri_cp4);

    // REST APIs
    httpd_uri_t uri_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_get_handler
    };
    httpd_register_uri_handler(s_server, &uri_status);

    httpd_uri_t uri_wifi_scan = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = api_wifi_scan_get_handler
    };
    httpd_register_uri_handler(s_server, &uri_wifi_scan);

    httpd_uri_t uri_wifi_conn = {
        .uri = "/api/wifi/connect",
        .method = HTTP_POST,
        .handler = api_wifi_connect_post_handler
    };
    httpd_register_uri_handler(s_server, &uri_wifi_conn);

    httpd_uri_t uri_mqtt_get = {
        .uri = "/api/mqtt",
        .method = HTTP_GET,
        .handler = api_mqtt_get_handler
    };
    httpd_register_uri_handler(s_server, &uri_mqtt_get);

    httpd_uri_t uri_mqtt_post = {
        .uri = "/api/mqtt",
        .method = HTTP_POST,
        .handler = api_mqtt_post_handler
    };
    httpd_register_uri_handler(s_server, &uri_mqtt_post);

    httpd_uri_t uri_ble_peers = {
        .uri = "/api/ble/peers",
        .method = HTTP_GET,
        .handler = api_ble_peers_get_handler
    };
    httpd_register_uri_handler(s_server, &uri_ble_peers);

    httpd_uri_t uri_ble_scan = {
        .uri = "/api/ble/scan",
        .method = HTTP_GET,
        .handler = api_ble_scan_get_handler
    };
    httpd_register_uri_handler(s_server, &uri_ble_scan);

    httpd_uri_t uri_ble_conn = {
        .uri = "/api/ble/connect",
        .method = HTTP_POST,
        .handler = api_ble_connect_post_handler
    };
    httpd_register_uri_handler(s_server, &uri_ble_conn);

    httpd_uri_t uri_ble_disconn = {
        .uri = "/api/ble/disconnect",
        .method = HTTP_POST,
        .handler = api_ble_disconnect_post_handler
    };
    httpd_register_uri_handler(s_server, &uri_ble_disconn);

    httpd_uri_t uri_sys_restart = {
        .uri = "/api/system/restart",
        .method = HTTP_POST,
        .handler = api_system_restart_post_handler
    };
    httpd_register_uri_handler(s_server, &uri_sys_restart);

    ESP_LOGI(TAG, "HTTP configuration server successfully registered all endpoints");
    return ESP_OK;
}

esp_err_t web_server_stop(void) {
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP configuration server stopped");
    }
    return ESP_OK;
}
