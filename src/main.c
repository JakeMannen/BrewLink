/**
 * @file main.c
 * @brief BrewLink Firmware Entrypoint
 * @details Multi-Device BLE Central (NimBLE) to WiFi/MQTT Relay for Adafruit HUZZAH32 (ESP-IDF)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

// NimBLE Stack Headers
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_esp.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

static const char *TAG = "BREWLINK_MAIN";

// Global FreeRTOS Queue for telemetry packets from BLE to WiFi/MQTT relay
static QueueHandle_t s_telemetry_queue = NULL;

// Active BLE peer table tracking connected equipment
static ble_peer_device_t s_connected_peers[BLE_MAX_SIMULTANEOUS_CONNECTIONS];

// MQTT Client Handle
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_connected = false;

// -----------------------------------------------------------------------------
// Telemetry Relay Worker Task (FreeRTOS Core 1)
// -----------------------------------------------------------------------------
static void telemetry_relay_task(void *pvParameters) {
    ble_telemetry_packet_t packet;
    char topic[128];
    char payload_json[256];

    ESP_LOGI(TAG, "Telemetry relay task started on core %d", xPortGetCoreID());

    while (1) {
        if (xQueueReceive(s_telemetry_queue, &packet, portMAX_DELAY) == pdTRUE) {
            ESP_LOGD(TAG, "Relaying packet from device %d (len: %u, RSSI: %d)",
                     packet.device_index, (unsigned int)packet.len, packet.rssi);

            // Construct topic: brewlink/device/0123456789AB/telemetry
            snprintf(topic, sizeof(topic), "%s%02X%02X%02X%02X%02X%02X/telemetry",
                     MQTT_TOPIC_TELEMETRY_PREFIX,
                     packet.mac[0], packet.mac[1], packet.mac[2],
                     packet.mac[3], packet.mac[4], packet.mac[5]);

            // Simple JSON payload
            snprintf(payload_json, sizeof(payload_json),
                     "{\"dev\":%u,\"rssi\":%d,\"len\":%u,\"ts\":%lu}",
                     packet.device_index, packet.rssi, (unsigned int)packet.len, (unsigned long)packet.timestamp_ms);

            if (s_mqtt_connected && s_mqtt_client != NULL) {
                esp_mqtt_client_publish(s_mqtt_client, topic, payload_json, 0, MQTT_QOS_TELEMETRY, 0);
            } else {
                ESP_LOGW(TAG, "MQTT not connected; dropping or buffering packet");
            }
        }
    }
}

// -----------------------------------------------------------------------------
// WiFi & MQTT Event Handlers
// -----------------------------------------------------------------------------
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi station starting; attempting connection to %s...", WIFI_SSID);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected; retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected! Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_mqtt_client != NULL) {
            esp_mqtt_client_start(s_mqtt_client);
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            s_mqtt_connected = true;
            esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_STATUS, "{\"status\":\"online\"}", 0, MQTT_QOS_STATUS, 1);
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_CMD, 1);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected from broker");
            s_mqtt_connected = false;
            break;
        default:
            break;
    }
}

// -----------------------------------------------------------------------------
// NimBLE Central Callbacks & Host Task
// -----------------------------------------------------------------------------
static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "NimBLE host task started on core %d", xPortGetCoreID());
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void) {
    ESP_LOGI(TAG, "NimBLE stack synchronized. Ready for multi-device scanning.");
    // Begin scanning for target peripheral equipment
}

// -----------------------------------------------------------------------------
// App Entrypoint
// -----------------------------------------------------------------------------
void app_main(void) {
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "BrewLink Multi-BLE Central & IoT Relay Starting");
    ESP_LOGI(TAG, "Target: Adafruit HUZZAH32 (ESP-WROOM-32)");
    ESP_LOGI(TAG, "Max Simultaneous BLE Connections: %d", BLE_MAX_SIMULTANEOUS_CONNECTIONS);
    ESP_LOGI(TAG, "=================================================");

    // 1. Initialize NVS (Non-Volatile Storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize Peer Tracking Table
    memset(s_connected_peers, 0, sizeof(s_connected_peers));
    for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        s_connected_peers[i].conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }

    // 3. Create FreeRTOS Queue for Telemetry Dispatch
    s_telemetry_queue = xQueueCreate(TELEMETRY_QUEUE_LENGTH, sizeof(ble_telemetry_packet_t));
    if (s_telemetry_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create telemetry queue!");
        return;
    }

    // 4. Start Telemetry Processing Task on Core 1
    xTaskCreatePinnedToCore(
        telemetry_relay_task,
        "telemetry_relay",
        RELAY_TASK_STACK_SIZE,
        NULL,
        RELAY_TASK_PRIORITY,
        NULL,
        APP_PROCESSING_TASK_CORE
    );

    // 5. Initialize TCP/IP and WiFi Stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 6. Configure MQTT Client
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    // 7. Initialize NimBLE Host and Port
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_on_sync;

    // Start NimBLE Host Task on Protocol Core (Core 0)
    nimble_port_freertos_init(ble_host_task);
}
