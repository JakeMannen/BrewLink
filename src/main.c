/**
 * @file main.c
 * @brief BrewLink Firmware Entrypoint
 * @details Multi-Device BLE Central (NimBLE) to WiFi/MQTT Bidirectional Relay for Adafruit HUZZAH32 (ESP-IDF)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
#include "unified_schema.h"
#include "grainfather_g30.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

static const char *TAG = "BREWLINK_MAIN";

// Global FreeRTOS Queue for telemetry packets from BLE to WiFi/MQTT relay (Upstream)
static QueueHandle_t s_telemetry_queue = NULL;

// Global FreeRTOS Queue for outbound BLE commands from WiFi/MQTT to BLE worker (Downstream)
static QueueHandle_t s_ble_cmd_queue = NULL;

// Active BLE peer table tracking connected equipment
static ble_peer_device_t s_connected_peers[BLE_MAX_SIMULTANEOUS_CONNECTIONS];

// Cached unified state for the Grainfather G30
static unified_boiler_state_t s_g30_state;

// MQTT Client Handle
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_connected = false;

// -----------------------------------------------------------------------------
// BLE Outbound Command Worker Task (FreeRTOS Core 0)
// -----------------------------------------------------------------------------
static void ble_cmd_worker_task(void *pvParameters) {
    ble_outbound_cmd_packet_t cmd_pkt;
    ESP_LOGI(TAG, "BLE outbound command worker started on core %d", xPortGetCoreID());

    while (1) {
        if (xQueueReceive(s_ble_cmd_queue, &cmd_pkt, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Transmitting BLE command to conn_handle %d: '%.*s'",
                     cmd_pkt.conn_handle, cmd_pkt.len, cmd_pkt.cmd_bytes);

            if (cmd_pkt.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                // In production, write_val_handle is resolved during GATT discovery of 0003cdd2
                uint16_t handle = s_connected_peers[0].write_val_handle != 0 ?
                                  s_connected_peers[0].write_val_handle : 0x000E;

                int rc = ble_gattc_write_flat(cmd_pkt.conn_handle, handle,
                                              cmd_pkt.cmd_bytes, G30_COMMAND_LEN,
                                              NULL, NULL);
                if (rc != 0) {
                    ESP_LOGE(TAG, "ble_gattc_write_flat failed: rc=%d", rc);
                }
            } else {
                ESP_LOGW(TAG, "Cannot send BLE command; peripheral not connected");
            }

            // Enforce mandatory inter-packet pacing (default 150 ms for Grainfather G30)
            uint16_t delay_ms = cmd_pkt.delay_after_ms > 0 ? cmd_pkt.delay_after_ms : G30_WRITE_PACING_MS;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
}

// -----------------------------------------------------------------------------
// Telemetry Relay Worker Task (FreeRTOS Core 1) - Upstream BLE -> WiFi
// -----------------------------------------------------------------------------
static void telemetry_relay_task(void *pvParameters) {
    ble_telemetry_packet_t packet;
    char topic[128];
    char payload_json[512];

    ESP_LOGI(TAG, "Telemetry relay task started on core %d", xPortGetCoreID());

    // Initialize default G30 boiler state
    memset(&s_g30_state, 0, sizeof(s_g30_state));
    s_g30_state.boil_temp_c = 100.0f;

    while (1) {
        if (xQueueReceive(s_telemetry_queue, &packet, portMAX_DELAY) == pdTRUE) {
            ESP_LOGD(TAG, "Relaying packet from device %d (len: %u, RSSI: %d)",
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
                // Fallback payload for raw or unparsed frames
                snprintf(payload_json, sizeof(payload_json),
                         "{\"dev\":%u,\"rssi\":%d,\"len\":%u,\"ts\":%lu}",
                         packet.device_index, packet.rssi, (unsigned int)packet.len, (unsigned long)packet.timestamp_ms);
            }

            if (s_mqtt_connected && s_mqtt_client != NULL) {
                esp_mqtt_client_publish(s_mqtt_client, topic, payload_json, 0, MQTT_QOS_TELEMETRY, 0);
            } else {
                ESP_LOGW(TAG, "MQTT not connected; dropping or buffering packet");
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Inbound Command Processing Helper (Downstream WiFi -> BLE)
// -----------------------------------------------------------------------------
static void process_inbound_command(const char *topic, const char *payload, int len) {
    ESP_LOGI(TAG, "Inbound command received on [%s]: %.*s", topic, len, payload);

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
        ESP_LOGW(TAG, "Unrecognized command action");
        return;
    }

    // Encode to G30 19-byte command buffer
    ble_outbound_cmd_packet_t outbound_pkt;
    memset(&outbound_pkt, 0, sizeof(outbound_pkt));
    outbound_pkt.conn_handle = s_connected_peers[0].conn_handle;
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
        ESP_LOGE(TAG, "Failed to encode command: err=0x%x", enc_err);
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
            // Subscribe to both gateway and per-device command topics
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_GATEWAY_CMD, MQTT_QOS_COMMAND);
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_CMD_WILDCARD, MQTT_QOS_COMMAND);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected from broker");
            s_mqtt_connected = false;
            break;

        case MQTT_EVENT_DATA: {
            char topic_buf[128];
            int topic_len = event->topic_len < (int)(sizeof(topic_buf) - 1) ? event->topic_len : (int)(sizeof(topic_buf) - 1);
            memcpy(topic_buf, event->topic, topic_len);
            topic_buf[topic_len] = '\0';

            process_inbound_command(topic_buf, event->data, event->data_len);
            break;
        }

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
    // Scanning begins when peer slots are available
}

// -----------------------------------------------------------------------------
// App Entrypoint
// -----------------------------------------------------------------------------
void app_main(void) {
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "BrewLink Multi-BLE Central & IoT Relay Starting");
    ESP_LOGI(TAG, "Target: Adafruit HUZZAH32 (ESP-WROOM-32)");
    ESP_LOGI(TAG, "Max Simultaneous BLE Connections: %d", BLE_MAX_SIMULTANEOUS_CONNECTIONS);
    ESP_LOGI(TAG, "Bidirectional BLE <-> WiFi Support Active");
    ESP_LOGI(TAG, "Supported Equipment: Grainfather G30 Connect");
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

    // 3. Create FreeRTOS Queues
    s_telemetry_queue = xQueueCreate(TELEMETRY_QUEUE_LENGTH, sizeof(ble_telemetry_packet_t));
    if (s_telemetry_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create telemetry queue!");
        return;
    }

    s_ble_cmd_queue = xQueueCreate(BLE_CMD_QUEUE_LENGTH, sizeof(ble_outbound_cmd_packet_t));
    if (s_ble_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create BLE command queue!");
        return;
    }

    // 4. Start Tasks
    // Telemetry Relay Worker Task on Core 1 (App / Parsing Core)
    xTaskCreatePinnedToCore(
        telemetry_relay_task,
        "telemetry_relay",
        RELAY_TASK_STACK_SIZE,
        NULL,
        RELAY_TASK_PRIORITY,
        NULL,
        APP_PROCESSING_TASK_CORE
    );

    // BLE Outbound Command Worker on Core 0 (Networking / Protocol Core)
    xTaskCreatePinnedToCore(
        ble_cmd_worker_task,
        "ble_cmd_worker",
        CMD_DISPATCH_TASK_STACK_SIZE,
        NULL,
        CMD_DISPATCH_TASK_PRIORITY,
        NULL,
        NETWORKING_TASK_CORE
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

    // 6. Configure MQTT Client with LWT
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .session.last_will = {
            .topic = MQTT_TOPIC_STATUS,
            .msg = "{\"status\":\"offline\"}",
            .msg_len = 20,
            .qos = MQTT_QOS_STATUS,
            .retain = 1,
        },
    };
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    // 7. Initialize NimBLE Host and Port
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_on_sync;

    // Start NimBLE Host Task on Protocol Core (Core 0)
    nimble_port_freertos_init(ble_host_task);
}
