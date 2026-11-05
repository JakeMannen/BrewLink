/**
 * @file main.c
 * @brief BrewLink Firmware Entrypoint
 * @details Multi-Device BLE Central (NimBLE) to WiFi/MQTT Bidirectional Relay with Onboarding Web Server
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "cJSON.h"

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

// Gateway identifier derived from ESP32 Base MAC
static char s_gateway_id[GATEWAY_ID_MAX_LEN] = {0};

// Cached unified state for the Grainfather G30
static unified_boiler_state_t s_g30_state;
static unified_boiler_state_t s_prev_state;
static uint32_t s_last_telemetry_pub_ms = 0;

// Helper: parse MAC string (e.g. "C44F331A8B22" or "C4:4F:33:1A:8B:22") into 6-byte array
static bool parse_hex_mac(const char *str, uint8_t mac[6]) {
    if (str == NULL || mac == NULL) return false;
    char clean[13] = {0};
    int c_idx = 0;
    for (int i = 0; str[i] != '\0' && c_idx < 12; i++) {
        if (isxdigit((unsigned char)str[i])) {
            clean[c_idx++] = (char)toupper((unsigned char)str[i]);
        }
    }
    if (c_idx != 12) return false;

    for (int i = 0; i < 6; i++) {
        char byte_hex[3] = {clean[i * 2], clean[i * 2 + 1], '\0'};
        mac[i] = (uint8_t)strtoul(byte_hex, NULL, 16);
    }
    return true;
}

// -----------------------------------------------------------------------------
// Equipment Capability Manifest & Announcements
// -----------------------------------------------------------------------------
static void publish_equipment_manifest(const ble_peer_device_t *peer, bool connected) {
    if (peer == NULL) return;

    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
             peer->addr[0], peer->addr[1], peer->addr[2],
             peer->addr[3], peer->addr[4], peer->addr[5]);

    char manifest[512];
    if (connected) {
        snprintf(manifest, sizeof(manifest),
                 "{\"event\":\"connected\",\"gateway_id\":\"%s\",\"mac\":\"%s\","
                 "\"equipment_type\":\"boiler\",\"model\":\"%s\",\"slot\":%d,\"rssi\":%d,"
                 "\"capabilities\":{\"temperature_read\":true,\"temperature_set\":true,"
                 "\"pump_control\":true,\"heat_control\":true,\"power_level_control\":true,"
                 "\"timer_control\":true,\"recipe_upload\":true,\"interaction_prompt\":true},"
                 "\"writable_ranges\":{\"target_temp_min_c\":20.0,\"target_temp_max_c\":100.0,"
                 "\"power_pct_min\":0,\"power_pct_max\":100},"
                 "\"timestamp_ms\":%lu}",
                 s_gateway_id, mac_str,
                 peer->device_name[0] ? peer->device_name : "Grainfather G30 Connect",
                 peer->conn_handle, peer->rssi,
                 (unsigned long)esp_log_timestamp());
    } else {
        snprintf(manifest, sizeof(manifest),
                 "{\"event\":\"disconnected\",\"gateway_id\":\"%s\",\"mac\":\"%s\","
                 "\"timestamp_ms\":%lu}",
                 s_gateway_id, mac_str,
                 (unsigned long)esp_log_timestamp());
    }

    if (mqtt_manager_is_connected()) {
        mqtt_manager_publish_equipment_announce(mac_str, manifest);
    }
}

// -----------------------------------------------------------------------------
// BLE Peer Lifecycle Callback
// -----------------------------------------------------------------------------
static void on_ble_peer_lifecycle_event(const ble_peer_device_t *peer, bool connected) {
    ESP_LOGI(TAG, "BLE peer lifecycle event: %02X:%02X:%02X:%02X:%02X:%02X (%s) -> %s",
             peer->addr[0], peer->addr[1], peer->addr[2],
             peer->addr[3], peer->addr[4], peer->addr[5],
             peer->device_name, connected ? "CONNECTED" : "DISCONNECTED");

    publish_equipment_manifest(peer, connected);
}

// -----------------------------------------------------------------------------
// Inbound Command Processing (Downstream WiFi/MQTT -> BLE with cmd_ack)
// -----------------------------------------------------------------------------
static void process_inbound_command(const char *topic, const char *payload, int len) {
    int64_t start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Inbound MQTT command on [%s]: %.*s", topic, len, payload);

    // Extract target MAC from topic if present (e.g. brewlink/{gw_id}/equipment/{MAC}/cmd)
    char target_mac_str[13] = {0};
    uint8_t target_mac[6] = {0};
    bool mac_resolved = false;

    const char *eq_marker = strstr(topic, "/equipment/");
    if (eq_marker != NULL) {
        const char *mac_start = eq_marker + 11;
        int m_idx = 0;
        while (mac_start[m_idx] != '\0' && mac_start[m_idx] != '/' && m_idx < 12) {
            target_mac_str[m_idx] = mac_start[m_idx];
            m_idx++;
        }
        target_mac_str[m_idx] = '\0';
        mac_resolved = parse_hex_mac(target_mac_str, target_mac);
    }

    // Allocate null-terminated JSON buffer for safe cJSON parsing
    char *json_buf = malloc(len + 1);
    if (json_buf == NULL) {
        ESP_LOGE(TAG, "Out of memory parsing command JSON");
        return;
    }
    memcpy(json_buf, payload, len);
    json_buf[len] = '\0';

    cJSON *root = cJSON_Parse(json_buf);
    free(json_buf);

    if (root == NULL) {
        ESP_LOGE(TAG, "Invalid JSON in command payload");
        mqtt_manager_publish_cmd_ack(target_mac_str, "unknown", "unknown", "error", 400, 0);
        return;
    }

    cJSON *req_id_item = cJSON_GetObjectItem(root, "req_id");
    const char *req_id = (req_id_item && cJSON_IsString(req_id_item)) ? req_id_item->valuestring : "req-unknown";

    cJSON *action_item = cJSON_GetObjectItem(root, "action");
    if (!action_item || !cJSON_IsString(action_item)) {
        ESP_LOGW(TAG, "Missing 'action' field in command");
        mqtt_manager_publish_cmd_ack(target_mac_str, req_id, "unknown", "rejected", 400, 0);
        cJSON_Delete(root);
        return;
    }
    const char *action_str = action_item->valuestring;
    cJSON *params = cJSON_GetObjectItem(root, "params");

    unified_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.equipment_type = EQUIPMENT_TYPE_BOILER;
    strncpy(cmd.req_id, req_id, sizeof(cmd.req_id) - 1);
    if (mac_resolved) {
        memcpy(cmd.target_mac, target_mac, 6);
    }

    bool handled = false;
    bool is_recipe = false;

    if (strcmp(action_str, "set_pump") == 0) {
        cmd.action = CMD_ACTION_SET_PUMP;
        cJSON *state = params ? cJSON_GetObjectItem(params, "state") : NULL;
        cmd.params.pump_state = state ? cJSON_IsTrue(state) : false;
        handled = true;
    } else if (strcmp(action_str, "set_heat") == 0) {
        cmd.action = CMD_ACTION_SET_HEAT;
        cJSON *state = params ? cJSON_GetObjectItem(params, "state") : NULL;
        cJSON *pwr = params ? cJSON_GetObjectItem(params, "power_pct") : NULL;
        cmd.params.heat.heat_state = state ? cJSON_IsTrue(state) : false;
        cmd.params.heat.power_pct = (pwr && cJSON_IsNumber(pwr)) ? (uint8_t)pwr->valueint : 100;
        handled = true;
    } else if (strcmp(action_str, "set_target_temp") == 0) {
        cmd.action = CMD_ACTION_SET_TARGET_TEMP;
        cJSON *temp = params ? cJSON_GetObjectItem(params, "temp_c") : NULL;
        cmd.params.target_temp_c = (temp && cJSON_IsNumber(temp)) ? (float)temp->valuedouble : 0.0f;
        handled = true;
    } else if (strcmp(action_str, "set_timer") == 0) {
        cmd.action = CMD_ACTION_SET_TIMER;
        cJSON *dur = params ? cJSON_GetObjectItem(params, "duration_min") : NULL;
        cmd.params.timer_duration_min = (dur && cJSON_IsNumber(dur)) ? (uint16_t)dur->valueint : 0;
        handled = true;
    } else if (strcmp(action_str, "pause_timer") == 0) {
        cmd.action = CMD_ACTION_PAUSE_TIMER;
        handled = true;
    } else if (strcmp(action_str, "resume_timer") == 0) {
        cmd.action = CMD_ACTION_RESUME_TIMER;
        handled = true;
    } else if (strcmp(action_str, "cancel_timer") == 0) {
        cmd.action = CMD_ACTION_CANCEL_TIMER;
        handled = true;
    } else if (strcmp(action_str, "ack_interaction") == 0) {
        cmd.action = CMD_ACTION_ACK_INTERACTION;
        handled = true;
    } else if (strcmp(action_str, "dismiss_alert") == 0) {
        cmd.action = CMD_ACTION_DISMISS_ALERT;
        handled = true;
    } else if (strcmp(action_str, "cancel_session") == 0) {
        cmd.action = CMD_ACTION_CANCEL_SESSION;
        handled = true;
    } else if (strcmp(action_str, "load_recipe") == 0) {
        cmd.action = CMD_ACTION_LOAD_RECIPE;
        is_recipe = true;
        handled = true;

        cJSON *rec = params ? cJSON_GetObjectItem(params, "recipe") : NULL;
        if (rec) {
            cJSON *name = cJSON_GetObjectItem(rec, "name");
            cJSON *boil_time = cJSON_GetObjectItem(rec, "boil_time_min");
            cJSON *mash_water = cJSON_GetObjectItem(rec, "mash_water_l");
            cJSON *sparge_water = cJSON_GetObjectItem(rec, "sparge_water_l");

            if (name && cJSON_IsString(name)) {
                strncpy(cmd.params.recipe.name, name->valuestring, sizeof(cmd.params.recipe.name) - 1);
            }
            cmd.params.recipe.boil_time_min = boil_time ? (uint16_t)boil_time->valueint : 60;
            cmd.params.recipe.mash_water_l = mash_water ? (float)mash_water->valuedouble : 0.0f;
            cmd.params.recipe.sparge_water_l = sparge_water ? (float)sparge_water->valuedouble : 0.0f;

            // Mash steps array
            cJSON *steps = cJSON_GetObjectItem(rec, "mash_steps");
            if (steps && cJSON_IsArray(steps)) {
                int count = cJSON_GetArraySize(steps);
                cmd.params.recipe.mash_step_count = count > MAX_MASH_STEPS ? MAX_MASH_STEPS : count;
                for (int i = 0; i < cmd.params.recipe.mash_step_count; i++) {
                    cJSON *step_item = cJSON_GetArrayItem(steps, i);
                    cJSON *t = cJSON_GetObjectItem(step_item, "temp_c");
                    cJSON *d = cJSON_GetObjectItem(step_item, "duration_min");
                    cmd.params.recipe.mash_steps[i].temp_c = t ? (float)t->valuedouble : 65.0f;
                    cmd.params.recipe.mash_steps[i].duration_min = d ? (uint16_t)d->valueint : 0;
                }
            }

            // Boil additions array
            cJSON *boil_adds = cJSON_GetObjectItem(rec, "boil_additions_min");
            if (boil_adds && cJSON_IsArray(boil_adds)) {
                int count = cJSON_GetArraySize(boil_adds);
                cmd.params.recipe.boil_addition_count = count > MAX_BOIL_ADDITIONS ? MAX_BOIL_ADDITIONS : count;
                for (int i = 0; i < cmd.params.recipe.boil_addition_count; i++) {
                    cJSON *add_item = cJSON_GetArrayItem(boil_adds, i);
                    cmd.params.recipe.boil_additions_min[i] = (uint16_t)add_item->valueint;
                }
            }
        }
    }

    cJSON_Delete(root);

    if (!handled) {
        ESP_LOGW(TAG, "Unrecognized command action '%s'", action_str);
        mqtt_manager_publish_cmd_ack(target_mac_str, req_id, action_str, "rejected", 404, 0);
        return;
    }

    // Dispatch to FreeRTOS queue for BLE worker
    ble_outbound_cmd_packet_t outbound_pkt;
    memset(&outbound_pkt, 0, sizeof(outbound_pkt));
    if (mac_resolved) {
        memcpy(outbound_pkt.mac, target_mac, 6);
    }
    outbound_pkt.conn_handle = 0;
    outbound_pkt.len = G30_COMMAND_LEN;
    outbound_pkt.delay_after_ms = G30_WRITE_PACING_MS;

    if (is_recipe) {
        // Enqueue recipe header
        esp_err_t err = g30_encode_recipe_header(&cmd.params.recipe, outbound_pkt.cmd_bytes);
        if (err == ESP_OK) {
            xQueueSend(s_ble_cmd_queue, &outbound_pkt, 0);
        }

        // Enqueue mash steps
        for (int i = 0; i < cmd.params.recipe.mash_step_count; i++) {
            err = g30_encode_recipe_mash_step(&cmd.params.recipe.mash_steps[i], outbound_pkt.cmd_bytes);
            if (err == ESP_OK) {
                xQueueSend(s_ble_cmd_queue, &outbound_pkt, 0);
            }
        }

        // Enqueue boil stops
        for (int i = 0; i < cmd.params.recipe.boil_addition_count; i++) {
            err = g30_encode_recipe_boil_stop(cmd.params.recipe.boil_additions_min[i], outbound_pkt.cmd_bytes);
            if (err == ESP_OK) {
                xQueueSend(s_ble_cmd_queue, &outbound_pkt, 0);
            }
        }

        uint32_t duration_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000ULL);
        mqtt_manager_publish_cmd_ack(target_mac_str, req_id, action_str, "success", 0, duration_ms);
        ESP_LOGI(TAG, "Enqueued complete recipe transfer for '%s'", cmd.params.recipe.name);
    } else {
        esp_err_t enc_err = g30_encode_command(&cmd, outbound_pkt.cmd_bytes);
        if (enc_err == ESP_OK) {
            if (xQueueSend(s_ble_cmd_queue, &outbound_pkt, 0) == pdTRUE) {
                uint32_t duration_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000ULL);
                mqtt_manager_publish_cmd_ack(target_mac_str, req_id, action_str, "success", 0, duration_ms);
                ESP_LOGI(TAG, "Enqueued G30 BLE command: '%.*s'", G30_COMMAND_LEN, outbound_pkt.cmd_bytes);
            } else {
                ESP_LOGE(TAG, "s_ble_cmd_queue full; command dropped");
                mqtt_manager_publish_cmd_ack(target_mac_str, req_id, action_str, "error", 503, 0);
            }
        } else {
            ESP_LOGE(TAG, "Failed to encode G30 command: err=0x%x", enc_err);
            mqtt_manager_publish_cmd_ack(target_mac_str, req_id, action_str, "error", enc_err, 0);
        }
    }
}

// -----------------------------------------------------------------------------
// MQTT Connection Status Callback & State Synchronization
// -----------------------------------------------------------------------------
static void on_mqtt_connection_status_change(bool connected) {
    if (connected) {
        ESP_LOGI(TAG, "MQTT broker connected! Publishing gateway status and equipment sync...");

        // 1. Publish Gateway Online Status
        wifi_mgr_status_t wifi_stat;
        wifi_manager_get_status(&wifi_stat);

        char status_json[320];
        snprintf(status_json, sizeof(status_json),
                 "{\"status\":\"online\",\"gateway_id\":\"%s\",\"firmware\":\"1.0.0\","
                 "\"hardware\":\"ESP-WROOM-32\",\"framework\":\"ESP-IDF v6.1\","
                 "\"ip_address\":\"%s\",\"rssi\":%d,\"max_slots\":%d,"
                 "\"active_peripherals\":%d,\"timestamp_ms\":%lu}",
                 s_gateway_id, wifi_stat.sta_ip, wifi_stat.sta_rssi,
                 BLE_MAX_SIMULTANEOUS_CONNECTIONS,
                 ble_manager_get_connected_count(),
                 (unsigned long)esp_log_timestamp());
        mqtt_manager_publish_gateway_status(status_json);

        // 2. Publish Manifest and Snapshot for all active peers
        ble_peer_device_t peers[BLE_MAX_SIMULTANEOUS_CONNECTIONS];
        uint8_t count = 0;
        if (ble_manager_get_peers(peers, BLE_MAX_SIMULTANEOUS_CONNECTIONS, &count) == ESP_OK) {
            for (uint8_t i = 0; i < count; i++) {
                publish_equipment_manifest(&peers[i], true);

                char mac_str[13];
                snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
                         peers[i].addr[0], peers[i].addr[1], peers[i].addr[2],
                         peers[i].addr[3], peers[i].addr[4], peers[i].addr[5]);

                char snapshot_json[512];
                snprintf(snapshot_json, sizeof(snapshot_json),
                         "{\"mac\":\"%s\",\"equipment_type\":\"boiler\",\"model\":\"%s\","
                         "\"rssi\":%d,\"state\":{"
                         "\"temperature\":{\"current_c\":%.1f,\"target_c\":%.1f,\"boil_c\":%.1f},"
                         "\"heating\":{\"active\":%s,\"power_pct\":%u,\"ramping\":%s},"
                         "\"pump\":{\"active\":%s},"
                         "\"timer\":{\"active\":%s,\"paused\":%s,\"remaining_seconds\":%lu,\"total_seconds\":%lu},"
                         "\"stage\":{\"step_index\":%u,\"auto_mode\":%s},"
                         "\"interaction\":{\"required\":%s,\"code\":%u,\"message\":\"%s\"}"
                         "},\"timestamp_ms\":%lu}",
                         mac_str, peers[i].device_name[0] ? peers[i].device_name : "Grainfather G30 Connect",
                         peers[i].rssi,
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
                         (unsigned long)esp_log_timestamp());

                mqtt_manager_publish_equipment_snapshot(mac_str, snapshot_json);
            }
        }
    } else {
        ESP_LOGW(TAG, "MQTT connection lost; offline state retained locally in RAM.");
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
    char payload_json[512];

    ESP_LOGI(TAG, "Telemetry relay task active on core %d", xPortGetCoreID());

    // Initialize default G30 boiler state
    memset(&s_g30_state, 0, sizeof(s_g30_state));
    memset(&s_prev_state, 0, sizeof(s_prev_state));
    s_g30_state.boil_temp_c = 100.0f;
    s_prev_state.boil_temp_c = 100.0f;

    while (1) {
        if (xQueueReceive(s_telemetry_queue, &packet, portMAX_DELAY) == pdTRUE) {
            ESP_LOGD(TAG, "Processing telemetry packet from dev %d (len: %u, RSSI: %d)",
                     packet.device_index, (unsigned int)packet.len, packet.rssi);

            // Parse notification using Grainfather G30 protocol
            g30_notification_type_t notif_type = G30_NOTIF_UNKNOWN;
            esp_err_t err = g30_parse_notification(packet.data, packet.len, &s_g30_state, &notif_type);

            char mac_str[13];
            snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
                     packet.mac[0], packet.mac[1], packet.mac[2],
                     packet.mac[3], packet.mac[4], packet.mac[5]);

            if (err == ESP_OK) {
                // Evaluate Hybrid Telemetry Cadence (Event-Driven + 1Hz Throttle)
                bool discrete_changed = (s_g30_state.pump_active != s_prev_state.pump_active) ||
                                        (s_g30_state.heat_active != s_prev_state.heat_active) ||
                                        (s_g30_state.heat_power_pct != s_prev_state.heat_power_pct) ||
                                        (s_g30_state.stage.step_index != s_prev_state.stage.step_index) ||
                                        (s_g30_state.auto_mode != s_prev_state.auto_mode) ||
                                        (s_g30_state.ramp_active != s_prev_state.ramp_active) ||
                                        (s_g30_state.interaction.required != s_prev_state.interaction.required) ||
                                        (s_g30_state.interaction.code != s_prev_state.interaction.code) ||
                                        (s_g30_state.timer.active != s_prev_state.timer.active) ||
                                        (s_g30_state.timer.paused != s_prev_state.timer.paused);

                uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                float temp_delta = fabsf(s_g30_state.current_temp_c - s_prev_state.current_temp_c);
                bool throttle_expired = (now_ms - s_last_telemetry_pub_ms >= TELEMETRY_CADENCE_THROTTLE_MS);
                bool temp_delta_exceeded = (temp_delta >= TELEMETRY_TEMP_DELTA_MIN_C);

                if (discrete_changed || throttle_expired || temp_delta_exceeded) {
                    snprintf(payload_json, sizeof(payload_json),
                             "{\"mac\":\"%s\",\"equipment_type\":\"boiler\",\"model\":\"Grainfather G30\","
                             "\"rssi\":%d,\"state\":{"
                             "\"temperature\":{\"current_c\":%.1f,\"target_c\":%.1f,\"boil_c\":%.1f},"
                             "\"heating\":{\"active\":%s,\"power_pct\":%u,\"ramping\":%s},"
                             "\"pump\":{\"active\":%s},"
                             "\"timer\":{\"active\":%s,\"paused\":%s,\"remaining_seconds\":%lu,\"total_seconds\":%lu},"
                             "\"stage\":{\"step_index\":%u,\"auto_mode\":%s},"
                             "\"interaction\":{\"required\":%s,\"code\":%u,\"message\":\"%s\"}"
                             "},\"timestamp_ms\":%lu}",
                             mac_str, packet.rssi,
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

                    if (mqtt_manager_is_connected()) {
                        mqtt_manager_publish_equipment_telemetry(mac_str, payload_json);
                    }

                    s_prev_state = s_g30_state;
                    s_last_telemetry_pub_ms = now_ms;
                }
            } else {
                // Fallback payload for raw or non-G30 frames
                snprintf(payload_json, sizeof(payload_json),
                         "{\"dev\":%u,\"mac\":\"%s\",\"rssi\":%d,\"len\":%u,\"ts\":%lu}",
                         packet.device_index, mac_str, packet.rssi,
                         (unsigned int)packet.len, (unsigned long)packet.timestamp_ms);

                if (mqtt_manager_is_connected()) {
                    mqtt_manager_publish_equipment_telemetry(mac_str, payload_json);
                }
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

    // 3. Derive Gateway Hardware ID
    uint8_t base_mac[6];
    if (esp_read_mac(base_mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_gateway_id, sizeof(s_gateway_id), "%02X%02X%02X%02X%02X%02X",
                 base_mac[0], base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
    } else {
        strncpy(s_gateway_id, "BREWLINK0000", sizeof(s_gateway_id) - 1);
    }
    ESP_LOGI(TAG, "BrewLink Gateway Hardware ID: %s", s_gateway_id);

    // 4. Create FreeRTOS Queues
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

    // 5. Start Core 1 Telemetry Relay Task
    xTaskCreatePinnedToCore(
        telemetry_relay_task,
        "telemetry_relay",
        RELAY_TASK_STACK_SIZE,
        NULL,
        RELAY_TASK_PRIORITY,
        NULL,
        APP_PROCESSING_TASK_CORE
    );

    // 6. Initialize TCP/IP and Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 7. Initialize NimBLE Central Subsystem (starts host & command worker on Core 0)
    ESP_ERROR_CHECK(ble_manager_init(s_telemetry_queue, s_ble_cmd_queue));
    ble_manager_register_peer_cb(on_ble_peer_lifecycle_event);

    // 8. Initialize MQTT Subsystem with Gateway ID & Callbacks
    ESP_ERROR_CHECK(mqtt_manager_init(s_gateway_id, process_inbound_command, on_mqtt_connection_status_change));

    // 9. Initialize Wi-Fi Subsystem (SoftAP broadcasting if no STA connected)
    ESP_ERROR_CHECK(wifi_manager_init(on_wifi_sta_status_change));

    // 10. Start Embedded Web Configuration Server on port 80 (Core 0)
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "BrewLink Gateway initialization complete! Free Heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());
}
