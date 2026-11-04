/**
 * @file main.c
 * @brief BrewLink Firmware Entrypoint
 * @details Multi-Device BLE Central (NimBLE) to WiFi/MQTT Bidirectional Relay with Onboarding Web Server
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "config.h"
#include "unified_schema.h"
#include "grainfather_g30.h"
#include "config_store.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "ble_manager.h"
#include "web_server.h"

static const char *TAG = "BREWLINK_MAIN";

// Global FreeRTOS Queue for telemetry packets from BLE to WiFi/MQTT relay (Upstream)
static QueueHandle_t s_telemetry_queue = NULL;

// Global FreeRTOS Queue for outbound BLE commands from WiFi/MQTT to BLE worker (Downstream)
static QueueHandle_t s_ble_cmd_queue = NULL;

// Cached unified state for the Grainfather G30
static unified_boiler_state_t s_g30_state;

// -----------------------------------------------------------------------------
// Inbound Command Processing (Downstream WiFi/MQTT -> BLE)
// -----------------------------------------------------------------------------
static void process_inbound_command(const char *topic, const char *payload, int len) {
    ESP_LOGI(TAG, "Inbound MQTT command on [%s]: %.*s", topic, len, payload);

    unified_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.equipment_type = EQUIPMENT_TYPE_BOILER;

    // Lightweight parsing of JSON action verbs
    if (strstr(payload, "\"set_pump\"") != NULL) {
        cmd.action = CMD_ACTION_SET_PUMP;
        cmd.params.pump_state = (strstr(payload, "\"state\":true") != NULL ||
                                 strstr(payload, "\"state\": true") != NULL);
    } else if (strstr(payload, "\"set_heat\"") != NULL) {
        cmd.action = CMD_ACTION_SET_HEAT;
        cmd.params.heat.heat_state = (strstr(payload, "\"state\":true") != NULL ||
                                      strstr(payload, "\"state\": true") != NULL);
    } else if (strstr(payload, "\"set_target_temp\"") != NULL) {
        cmd.action = CMD_ACTION_SET_TARGET_TEMP;
        const char *temp_ptr = strstr(payload, "\"temp_c\":");
        if (temp_ptr) {
            cmd.params.target_temp_c = strtof(temp_ptr + 9, NULL);
        }
    } else if (strstr(payload, "\"set_timer\"") != NULL) {
        cmd.action = CMD_ACTION_SET_TIMER;
        const char *dur_ptr = strstr(payload, "\"duration_min\":");
        if (dur_ptr) {
            cmd.params.timer_duration_min = (uint16_t)strtoul(dur_ptr + 15, NULL, 10);
        }
    } else if (strstr(payload, "\"pause_timer\"") != NULL) {
        cmd.action = CMD_ACTION_PAUSE_TIMER;
    } else if (strstr(payload, "\"resume_timer\"") != NULL) {
        cmd.action = CMD_ACTION_RESUME_TIMER;
    } else if (strstr(payload, "\"cancel_timer\"") != NULL) {
        cmd.action = CMD_ACTION_CANCEL_TIMER;
    } else if (strstr(payload, "\"ack_interaction\"") != NULL) {
        cmd.action = CMD_ACTION_ACK_INTERACTION;
    } else if (strstr(payload, "\"dismiss_alert\"") != NULL) {
        cmd.action = CMD_ACTION_DISMISS_ALERT;
    } else if (strstr(payload, "\"cancel_session\"") != NULL) {
        cmd.action = CMD_ACTION_CANCEL_SESSION;
    } else {
        ESP_LOGW(TAG, "Unrecognized command action in MQTT payload");
        return;
    }

    // Encode to G30 19-byte command buffer
    ble_outbound_cmd_packet_t outbound_pkt;
    memset(&outbound_pkt, 0, sizeof(outbound_pkt));
    outbound_pkt.conn_handle = 0; // BLE worker will route to first active peer or handle 0
    outbound_pkt.len = G30_COMMAND_LEN;
    outbound_pkt.delay_after_ms = G30_WRITE_PACING_MS;

    esp_err_t enc_err = g30_encode_command(&cmd, outbound_pkt.cmd_bytes);
    if (enc_err == ESP_OK) {
        if (xQueueSend(s_ble_cmd_queue, &outbound_pkt, 0) != pdTRUE) {
            ESP_LOGE(TAG, "s_ble_cmd_queue full; command dropped");
        } else {
            ESP_LOGI(TAG, "Enqueued G30 BLE command: '%.*s'", G30_COMMAND_LEN, outbound_pkt.cmd_bytes);
        }
    } else {
        ESP_LOGE(TAG, "Failed to encode G30 command: err=0x%x", enc_err);
    }
}

// -----------------------------------------------------------------------------
// Wi-Fi Station Status Callback
// -----------------------------------------------------------------------------
static void on_wifi_sta_status_change(bool connected, const char *ip) {
    if (connected) {
        ESP_LOGI(TAG, "Wi-Fi Station active (IP: %s). Starting MQTT client...", ip);
        mqtt_manager_start();
    } else {
        ESP_LOGW(TAG, "Wi-Fi Station offline. Halting MQTT client...");
        mqtt_manager_stop();
    }
}

// -----------------------------------------------------------------------------
// Telemetry Relay Worker Task (FreeRTOS Core 1) - Upstream BLE -> WiFi/MQTT
// -----------------------------------------------------------------------------
static void telemetry_relay_task(void *pvParameters) {
    ble_telemetry_packet_t packet;
    char topic[128];
    char payload_json[512];

    ESP_LOGI(TAG, "Telemetry relay task active on core %d", xPortGetCoreID());

    // Initialize default G30 boiler state
    memset(&s_g30_state, 0, sizeof(s_g30_state));
    s_g30_state.boil_temp_c = 100.0f;

    while (1) {
        if (xQueueReceive(s_telemetry_queue, &packet, portMAX_DELAY) == pdTRUE) {
            ESP_LOGD(TAG, "Relaying telemetry packet from dev %d (len: %u, RSSI: %d)",
                     packet.device_index, (unsigned int)packet.len, packet.rssi);

            // Parse notification using Grainfather G30 protocol
            g30_notification_type_t notif_type = G30_NOTIF_UNKNOWN;
            esp_err_t err = g30_parse_notification(packet.data, packet.len, &s_g30_state, &notif_type);

            snprintf(topic, sizeof(topic), "%s%02X%02X%02X%02X%02X%02X%s",
                     MQTT_TOPIC_DEVICE_PREFIX,
                     packet.mac[0], packet.mac[1], packet.mac[2],
                     packet.mac[3], packet.mac[4], packet.mac[5],
                     MQTT_TOPIC_TELEMETRY_SUFFIX);

            if (err == ESP_OK) {
                // Unified Boiler Telemetry Schema (JSON)
                snprintf(payload_json, sizeof(payload_json),
                         "{\"device_id\":\"%02X%02X%02X%02X%02X%02X\","
                         "\"equipment_type\":\"boiler\",\"model\":\"Grainfather G30\","
                         "\"rssi\":%d,\"state\":{"
                         "\"temperature\":{\"current_c\":%.1f,\"target_c\":%.1f,\"boil_c\":%.1f},"
                         "\"heating\":{\"active\":%s,\"power_pct\":%u,\"ramping\":%s},"
                         "\"pump\":{\"active\":%s},"
                         "\"timer\":{\"active\":%s,\"paused\":%s,\"remaining_seconds\":%lu,\"total_seconds\":%lu},"
                         "\"stage\":{\"step_index\":%u,\"auto_mode\":%s},"
                         "\"interaction\":{\"required\":%s,\"code\":%u,\"message\":\"%s\"}"
                         "},\"timestamp_ms\":%lu}",
                         packet.mac[0], packet.mac[1], packet.mac[2],
                         packet.mac[3], packet.mac[4], packet.mac[5],
                         packet.rssi,
                         s_g30_state.current_temp_c, s_g30_state.target_temp_c, s_g30_state.boil_temp_c,
                         s_g30_state.heat_active ? "true" : "false",
                         s_g30_state.heat_power_pct,
                         s_g30_state.ramp_active ? "true" : "false",
                         s_g30_state.pump_active ? "true" : "false",
                         s_g30_state.timer.active ? "true" : "false",
                         s_g30_state.timer.paused ? "true" : "false",
                         (unsigned long)s_g30_state.timer.remaining_seconds,
                         (unsigned long)s_g30_state.timer.total_seconds,
                         s_g30_state.stage.step_index,
                         s_g30_state.auto_mode ? "true" : "false",
                         s_g30_state.interaction.required ? "true" : "false",
                         s_g30_state.interaction.code,
                         s_g30_state.interaction.message,
                         (unsigned long)packet.timestamp_ms);
            } else {
                // Fallback payload for raw frames
                snprintf(payload_json, sizeof(payload_json),
                         "{\"dev\":%u,\"rssi\":%d,\"len\":%u,\"ts\":%lu}",
                         packet.device_index, packet.rssi, (unsigned int)packet.len, (unsigned long)packet.timestamp_ms);
            }

            if (mqtt_manager_is_connected()) {
                mqtt_manager_publish(topic, payload_json, 0, MQTT_QOS_TELEMETRY, 0);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// App Entrypoint
// -----------------------------------------------------------------------------
void app_main(void) {
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "BrewLink Multi-BLE Central & IoT Gateway Starting");
    ESP_LOGI(TAG, "Target: Adafruit HUZZAH32 (ESP-WROOM-32)");
    ESP_LOGI(TAG, "Max Simultaneous BLE Connections: %d", BLE_MAX_SIMULTANEOUS_CONNECTIONS);
    ESP_LOGI(TAG, "SoftAP Onboarding & Web Configuration Server Active");
    ESP_LOGI(TAG, "=================================================");

    // 1. Initialize Non-Volatile Storage (NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize Persistent Configuration Store
    ESP_ERROR_CHECK(config_store_init());

    // 3. Create FreeRTOS Queues
    s_telemetry_queue = xQueueCreate(TELEMETRY_QUEUE_LENGTH, sizeof(ble_telemetry_packet_t));
    if (s_telemetry_queue == NULL) {
        ESP_LOGE(TAG, "Failed to allocate s_telemetry_queue");
        return;
    }

    s_ble_cmd_queue = xQueueCreate(BLE_CMD_QUEUE_LENGTH, sizeof(ble_outbound_cmd_packet_t));
    if (s_ble_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to allocate s_ble_cmd_queue");
        return;
    }

    // 4. Start Core 1 Telemetry Relay Task
    xTaskCreatePinnedToCore(
        telemetry_relay_task,
        "telemetry_relay",
        RELAY_TASK_STACK_SIZE,
        NULL,
        RELAY_TASK_PRIORITY,
        NULL,
        APP_PROCESSING_TASK_CORE
    );

    // 5. Initialize TCP/IP and Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 6. Initialize NimBLE Central Subsystem (starts host & command worker on Core 0)
    ESP_ERROR_CHECK(ble_manager_init(s_telemetry_queue, s_ble_cmd_queue));

    // 7. Initialize MQTT Subsystem
    ESP_ERROR_CHECK(mqtt_manager_init(process_inbound_command));

    // 8. Initialize Wi-Fi Subsystem (SoftAP broadcasting if no SSID connected)
    ESP_ERROR_CHECK(wifi_manager_init(on_wifi_sta_status_change));

    // 9. Start Embedded Web Configuration Server on port 80 (Core 0)
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "BrewLink Gateway initialization complete! Free Heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());
}
