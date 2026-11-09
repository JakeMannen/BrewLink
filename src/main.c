/**
 * @file main.c
 * @brief BrewLink Firmware Entrypoint
 * @details Multi-Device BLE Central (NimBLE) to WiFi/MQTT/WebSocket/HTTP Bidirectional Relay
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
#include "g30_schema.h"
#include "grainfather_g30.h"
#include "g30_controller.h"
#include "config_store.h"
#include "auth_manager.h"
#include "equipment_manager.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "ble_manager.h"
#include "web_server.h"

static const char *TAG = "BREWLINK_MAIN";

// Global FreeRTOS Queue for telemetry packets from BLE to WiFi/MQTT relay (Upstream)
static QueueHandle_t s_telemetry_queue = NULL;

// Global FreeRTOS Queue for outbound BLE commands from WiFi/MQTT to BLE worker (Downstream)
static QueueHandle_t s_ble_cmd_queue = NULL;
static SemaphoreHandle_t s_g30_state_mutex = NULL;

// Gateway identifier derived from ESP32 Base MAC
static char s_gateway_id[GATEWAY_ID_MAX_LEN] = {0};

// Cached unified state for the Grainfather G30 (kept synchronized with equipment_manager)
static g30_state_t s_g30_state;
static g30_state_t s_prev_state;
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

static const char *equip_type_to_str(equipment_type_t t) {
    switch (t) {
        case EQUIPMENT_TYPE_BOILER:        return "boiler";
        case EQUIPMENT_TYPE_FERMENTER:     return "fermenter";
        case EQUIPMENT_TYPE_COOLER_HEATER: return "cooler";
        default:                           return "unknown";
    }
}

static equipment_type_t parse_equip_type_str(const char *s) {
    if (s == NULL) return EQUIPMENT_TYPE_BOILER;
    if (strcmp(s, "fermenter") == 0) return EQUIPMENT_TYPE_FERMENTER;
    if (strcmp(s, "cooler") == 0 || strcmp(s, "cooler_heater") == 0 || strcmp(s, "chiller") == 0) {
        return EQUIPMENT_TYPE_COOLER_HEATER;
    }
    return EQUIPMENT_TYPE_BOILER;
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

    const char *eq_type_str = equip_type_to_str(peer->equipment_type);

    char manifest[768];
    if (connected) {
        snprintf(manifest, sizeof(manifest),
                 "{\"event\":\"connected\",\"gateway_id\":\"%s\",\"mac\":\"%s\","
                 "\"equipment_type\":\"%s\",\"model\":\"%s\",\"slot\":%d,\"rssi\":%d,"
                 "\"capabilities\":{\"temperature_read\":true,\"temperature_set\":true,"
                 "\"pump_control\":%s,\"heat_control\":%s,\"power_level_control\":%s,"
                 "\"timer_control\":%s,\"recipe_upload\":true,\"interaction_prompt\":%s},"
                 "\"writable_ranges\":{\"target_temp_min_c\":%.1f,\"target_temp_max_c\":%.1f,"
                 "\"power_pct_min\":0,\"power_pct_max\":100},"
                 "\"timestamp_ms\":%lu}",
                 s_gateway_id, mac_str, eq_type_str,
                 peer->device_name[0] ? peer->device_name : "Grainfather G30 Connect",
                 peer->conn_handle, peer->rssi,
                 (peer->equipment_type == EQUIPMENT_TYPE_BOILER) ? "true" : "false",
                 (peer->equipment_type != EQUIPMENT_TYPE_FERMENTER) ? "true" : "false",
                 (peer->equipment_type == EQUIPMENT_TYPE_BOILER) ? "true" : "false",
                 (peer->equipment_type == EQUIPMENT_TYPE_BOILER) ? "true" : "false",
                 (peer->equipment_type == EQUIPMENT_TYPE_BOILER) ? "true" : "false",
                 (peer->equipment_type == EQUIPMENT_TYPE_BOILER) ? 20.0f : -5.0f,
                 (peer->equipment_type == EQUIPMENT_TYPE_BOILER) ? 100.0f : 40.0f,
                 (unsigned long)esp_log_timestamp());
    } else {
        snprintf(manifest, sizeof(manifest),
                 "{\"event\":\"disconnected\",\"gateway_id\":\"%s\",\"mac\":\"%s\","
                 "\"equipment_type\":\"%s\",\"timestamp_ms\":%lu}",
                 s_gateway_id, mac_str, eq_type_str,
                 (unsigned long)esp_log_timestamp());
    }

    if (mqtt_manager_is_connected()) {
        mqtt_manager_publish_equipment_announce(mac_str, manifest);
    }
    web_server_ws_broadcast(manifest);
}

// -----------------------------------------------------------------------------
// BLE Peer Lifecycle & Sequence Completion Callbacks
// -----------------------------------------------------------------------------
static void broadcast_equipment_list(const char *req_id) {
    char list_json[1024];
    if (equipment_manager_format_equipment_list_json(s_gateway_id,
                                                     req_id,
                                                     list_json,
                                                     sizeof(list_json)) == ESP_OK) {
        if (mqtt_manager_is_connected()) {
            mqtt_manager_publish_equipment_list(list_json);
        }
        web_server_ws_broadcast(list_json);
    }
}

static void on_ble_peer_lifecycle_event(const ble_peer_device_t *peer, bool connected) {
    ESP_LOGI(TAG, "BLE peer lifecycle event: %02X:%02X:%02X:%02X:%02X:%02X (%s) -> %s",
             peer->addr[0], peer->addr[1], peer->addr[2],
             peer->addr[3], peer->addr[4], peer->addr[5],
             peer->device_name, connected ? "CONNECTED" : "DISCONNECTED");

    const char *model_str = peer->device_name[0] ? peer->device_name : "Grainfather G30 Connect";
    equipment_manager_on_peer_event(peer->addr,
                                    connected,
                                    peer->equipment_type,
                                    model_str,
                                    peer->conn_handle,
                                    peer->rssi);

    if (connected) {
        // Persist paired device and its guaranteed unique name in NVS for automatic power-on reconnection
        char unique_name[32] = {0};
        strncpy(unique_name, model_str, sizeof(unique_name) - 1);
        for (uint8_t i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
            equipment_slot_t slot;
            if (equipment_manager_get_slot(i, &slot) == ESP_OK &&
                (slot.paired || slot.connected) &&
                memcmp(slot.mac, peer->addr, 6) == 0) {
                if (slot.name[0] != '\0') {
                    strncpy(unique_name, slot.name, sizeof(unique_name) - 1);
                }
                break;
            }
        }
        config_store_save_paired_ble(peer->addr,
                                     peer->addr_type,
                                     (uint8_t)peer->equipment_type,
                                     unique_name,
                                     model_str);
    }

    publish_equipment_manifest(peer, connected);
    broadcast_equipment_list(NULL);
}

static void on_ble_cmd_seq_complete(const uint8_t mac[6],
                                    const char *req_id,
                                    const char *action,
                                    esp_err_t result) {
    char mac_str[13] = "000000000000";
    if (mac != NULL) {
        snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    const char *status_str = (result == ESP_OK) ? "success" : "error";

    if (mqtt_manager_is_connected()) {
        mqtt_manager_publish_cmd_ack(mac_str,
                                     req_id ? req_id : "req-0",
                                     action ? action : "cmd",
                                     status_str,
                                     (int)result,
                                     0);
    }

    char ws_ack[320];
    snprintf(ws_ack, sizeof(ws_ack),
             "{\"event\":\"cmd_ack\",\"mac\":\"%s\",\"req_id\":\"%s\","
             "\"action\":\"%s\",\"status\":\"%s\",\"error_code\":%d}",
             mac_str,
             req_id ? req_id : "req-0",
             action ? action : "cmd",
             status_str,
             (int)result);
    web_server_ws_broadcast(ws_ack);
}

// -----------------------------------------------------------------------------
// G30 Controller Public Bridge API (Delegates to Equipment Manager)
// -----------------------------------------------------------------------------
esp_err_t g30_controller_get_state(g30_state_t *out_state,
                                   bool *out_connected,
                                   uint8_t out_mac[6],
                                   int8_t *out_rssi) {
    if (out_connected == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    equipment_slot_t slot;
    memset(&slot, 0, sizeof(slot));
    bool found = false;

    ble_peer_device_t peers[BLE_MAX_SIMULTANEOUS_CONNECTIONS];
    uint8_t p_count = 0;
    ble_manager_get_peers(peers, BLE_MAX_SIMULTANEOUS_CONNECTIONS, &p_count);

    if (equipment_manager_find_connected(NULL, EQUIPMENT_TYPE_BOILER, &slot) == ESP_OK) {
        found = true;
        int8_t live_rssi = slot.rssi;
        for (uint8_t i = 0; i < p_count; i++) {
            if (peers[i].connected && memcmp(peers[i].addr, slot.mac, 6) == 0 && peers[i].rssi != 0) {
                live_rssi = peers[i].rssi;
                break;
            }
        }
        if (out_mac != NULL) {
            memcpy(out_mac, slot.mac, 6);
        }
        if (out_rssi != NULL) {
            *out_rssi = live_rssi;
        }
        if (out_state != NULL) {
            *out_state = slot.state.boiler;
        }
    }

    // Fallback to ble_manager peer list if slot not yet populated
    if (!found) {
        for (uint8_t i = 0; i < p_count; i++) {
            if (peers[i].connected && (peers[i].equipment_type == EQUIPMENT_TYPE_BOILER ||
                strncmp(peers[i].device_name, "Grain", 5) == 0)) {
                found = true;
                if (out_mac != NULL) {
                    memcpy(out_mac, peers[i].addr, 6);
                }
                if (out_rssi != NULL) {
                    *out_rssi = peers[i].rssi;
                }
                const char *m_str = peers[i].device_name[0] ? peers[i].device_name : "Grainfather G30 Connect";
                equipment_manager_on_peer_event(peers[i].addr,
                                                true,
                                                EQUIPMENT_TYPE_BOILER,
                                                m_str,
                                                peers[i].conn_handle,
                                                peers[i].rssi);
                config_store_save_paired_ble(peers[i].addr,
                                             peers[i].addr_type,
                                             (uint8_t)EQUIPMENT_TYPE_BOILER,
                                             m_str,
                                             m_str);
                break;
            }
        }
        if (out_state != NULL) {
            if (s_g30_state_mutex != NULL && xSemaphoreTake(s_g30_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                *out_state = s_g30_state;
                xSemaphoreGive(s_g30_state_mutex);
            } else {
                *out_state = s_g30_state;
            }
        }
    }

    *out_connected = found;
    return ESP_OK;
}

esp_err_t g30_controller_dispatch_command(const g30_command_t *cmd) {
    if (cmd == NULL || s_ble_cmd_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t resolved_mac[6] = {0};
    return equipment_manager_dispatch_command(cmd, resolved_mac);
}

esp_err_t g30_controller_dispatch_recipe(const g30_recipe_t *recipe) {
    if (recipe == NULL || s_ble_cmd_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = equipment_manager_dispatch_recipe(NULL,
                                                      EQUIPMENT_TYPE_BOILER,
                                                      "web-recipe",
                                                      recipe,
                                                      NULL);
    if (err == ESP_OK) {
        // Sync cached s_g30_state from equipment_manager
        bool connected = false;
        g30_controller_get_state(&s_g30_state, &connected, NULL, NULL);
    }
    return err;
}

// -----------------------------------------------------------------------------
// Inbound Command Processing (Downstream WiFi/MQTT -> BLE with Auth & cmd_ack)
// -----------------------------------------------------------------------------
static void process_inbound_command(const char *topic, const char *payload, int len) {
    ESP_LOGI(TAG, "Inbound MQTT command on [%s]: %.*s", topic, len, payload);

    // Extract target MAC from topic if present (e.g. brewlink/.../equipment/{MAC}/cmd)
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

    // Verify MQTT command signature or token (Option 1 security)
    cJSON *ts_item = cJSON_GetObjectItem(root, "timestamp");
    cJSON *sig_item = cJSON_GetObjectItem(root, "sig");
    cJSON *tok_item = cJSON_GetObjectItem(root, "token");
    uint32_t ts_val = (ts_item && cJSON_IsNumber(ts_item)) ? (uint32_t)ts_item->valuedouble : 0;
    const char *sig_str = (sig_item && cJSON_IsString(sig_item)) ? sig_item->valuestring : NULL;
    const char *tok_str = (tok_item && cJSON_IsString(tok_item)) ? tok_item->valuestring : NULL;

    if (!auth_manager_verify_mqtt_command(req_id, action_str, ts_val, sig_str, tok_str)) {
        ESP_LOGW(TAG, "Rejected unauthorized MQTT command '%s' (req_id=%s)", action_str, req_id);
        mqtt_manager_publish_cmd_ack(target_mac_str, req_id, action_str, "unauthorized", 401, 0);
        cJSON_Delete(root);
        return;
    }

    // Optional target_mac and equipment_type in payload
    cJSON *mac_item = cJSON_GetObjectItem(root, "target_mac");
    if (!mac_resolved && mac_item && cJSON_IsString(mac_item)) {
        mac_resolved = parse_hex_mac(mac_item->valuestring, target_mac);
        if (mac_resolved) {
            snprintf(target_mac_str, sizeof(target_mac_str), "%02X%02X%02X%02X%02X%02X",
                     target_mac[0], target_mac[1], target_mac[2],
                     target_mac[3], target_mac[4], target_mac[5]);
        }
    }

    cJSON *eq_type_item = cJSON_GetObjectItem(root, "equipment_type");
    equipment_type_t eq_type = parse_equip_type_str(
        (eq_type_item && cJSON_IsString(eq_type_item)) ? eq_type_item->valuestring : NULL);

    cJSON *params = cJSON_GetObjectItem(root, "params");

    if (strcmp(action_str, "list_equipment") == 0) {
        broadcast_equipment_list(req_id);
        mqtt_manager_publish_cmd_ack("GATEWAY", req_id, action_str, "success", 0, 0);
        cJSON_Delete(root);
        return;
    }

    cJSON *slot_item = params ? cJSON_GetObjectItem(params, "slot") : NULL;
    if (slot_item == NULL) slot_item = cJSON_GetObjectItem(root, "slot");
    uint8_t target_slot = (slot_item && cJSON_IsNumber(slot_item) && slot_item->valueint >= 1)
                          ? (uint8_t)slot_item->valueint : 0;

    cJSON *tname_item = params ? cJSON_GetObjectItem(params, "target_name") : NULL;
    if (tname_item == NULL) tname_item = cJSON_GetObjectItem(root, "target_name");
    if (tname_item == NULL && params) tname_item = cJSON_GetObjectItem(params, "equipment_name");
    if (tname_item == NULL) tname_item = cJSON_GetObjectItem(root, "equipment_name");

    g30_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.equipment_type = eq_type;
    cmd.target_slot = target_slot;
    if (tname_item && cJSON_IsString(tname_item)) {
        strncpy(cmd.target_name, tname_item->valuestring, sizeof(cmd.target_name) - 1);
    }
    strncpy(cmd.req_id, req_id, sizeof(cmd.req_id) - 1);
    if (mac_resolved) {
        memcpy(cmd.target_mac, target_mac, 6);
    }

    bool handled = false;
    bool is_recipe = false;

    if (strcmp(action_str, "set_pump") == 0) {
        cmd.action = G30_CMD_ACTION_SET_PUMP;
        cJSON *state = params ? cJSON_GetObjectItem(params, "state") : NULL;
        cmd.params.pump_state = state ? cJSON_IsTrue(state) : false;
        handled = true;
    } else if (strcmp(action_str, "set_heat") == 0) {
        cmd.action = G30_CMD_ACTION_SET_HEAT;
        cJSON *state = params ? cJSON_GetObjectItem(params, "state") : NULL;
        cJSON *pwr = params ? cJSON_GetObjectItem(params, "power_pct") : NULL;
        cmd.params.heat.heat_state = state ? cJSON_IsTrue(state) : false;
        cmd.params.heat.power_pct = (pwr && cJSON_IsNumber(pwr)) ? (uint8_t)pwr->valueint : 100;
        handled = true;
    } else if (strcmp(action_str, "set_target_temp") == 0) {
        cmd.action = G30_CMD_ACTION_SET_TARGET_TEMP;
        cJSON *temp = params ? cJSON_GetObjectItem(params, "temp_c") : NULL;
        cmd.params.target_temp_c = (temp && cJSON_IsNumber(temp)) ? (float)temp->valuedouble : 0.0f;
        handled = true;
    } else if (strcmp(action_str, "set_timer") == 0) {
        cmd.action = G30_CMD_ACTION_SET_TIMER;
        cJSON *dur = params ? cJSON_GetObjectItem(params, "duration_min") : NULL;
        cmd.params.timer_duration_min = (dur && cJSON_IsNumber(dur)) ? (uint16_t)dur->valueint : 0;
        handled = true;
    } else if (strcmp(action_str, "toggle_timer") == 0) {
        cmd.action = G30_CMD_ACTION_TOGGLE_TIMER;
        handled = true;
    } else if (strcmp(action_str, "pause_timer") == 0) {
        cmd.action = G30_CMD_ACTION_PAUSE_TIMER;
        handled = true;
    } else if (strcmp(action_str, "resume_timer") == 0) {
        cmd.action = G30_CMD_ACTION_RESUME_TIMER;
        handled = true;
    } else if (strcmp(action_str, "cancel_timer") == 0) {
        cmd.action = G30_CMD_ACTION_CANCEL_TIMER;
        handled = true;
    } else if (strcmp(action_str, "ack_interaction") == 0) {
        cmd.action = G30_CMD_ACTION_ACK_INTERACTION;
        handled = true;
    } else if (strcmp(action_str, "press_set") == 0) {
        cmd.action = G30_CMD_ACTION_PRESS_SET;
        handled = true;
    } else if (strcmp(action_str, "dismiss_alert") == 0) {
        cmd.action = G30_CMD_ACTION_DISMISS_ALERT;
        handled = true;
    } else if (strcmp(action_str, "dismiss_sparge_alert") == 0) {
        cmd.action = G30_CMD_ACTION_DISMISS_SPARGE_ALERT;
        handled = true;
    } else if (strcmp(action_str, "cancel_session") == 0) {
        cmd.action = G30_CMD_ACTION_CANCEL_SESSION;
        handled = true;
    } else if (strcmp(action_str, "temp_up") == 0) {
        cmd.action = G30_CMD_ACTION_TEMP_UP;
        handled = true;
    } else if (strcmp(action_str, "temp_down") == 0) {
        cmd.action = G30_CMD_ACTION_TEMP_DOWN;
        handled = true;
    } else if (strcmp(action_str, "disconnect_manual") == 0) {
        cmd.action = G30_CMD_ACTION_DISCONNECT_MANUAL;
        handled = true;
    } else if (strcmp(action_str, "disconnect_auto") == 0) {
        cmd.action = G30_CMD_ACTION_DISCONNECT_AUTO;
        handled = true;
    } else if (strcmp(action_str, "delayed_heat") == 0) {
        cmd.action = G30_CMD_ACTION_DELAYED_HEAT;
        cJSON *dm = params ? cJSON_GetObjectItem(params, "minutes") : NULL;
        cJSON *ds = params ? cJSON_GetObjectItem(params, "seconds") : NULL;
        if (dm && cJSON_IsNumber(dm) && dm->valueint >= 0 && dm->valueint <= 999) {
            cmd.params.delayed_heat.minutes = (uint16_t)dm->valueint;
            cmd.params.delayed_heat.seconds = (ds && cJSON_IsNumber(ds) && ds->valueint >= 0 && ds->valueint < 60) ? (uint8_t)ds->valueint : 0;
            handled = true;
        }
    } else if (strcmp(action_str, "load_recipe") == 0) {
        cmd.action = G30_CMD_ACTION_LOAD_RECIPE;
        is_recipe = true;
        handled = true;

        cJSON *rec = params ? cJSON_GetObjectItem(params, "recipe") : NULL;
        if (!rec && params) {
            rec = params;
        }
        if (rec) {
            cJSON *name = cJSON_GetObjectItem(rec, "name");
            cJSON *boil_time = cJSON_GetObjectItem(rec, "boil_time_min");
            cJSON *hop_stand = cJSON_GetObjectItem(rec, "hop_stand_min");
            cJSON *mash_water = cJSON_GetObjectItem(rec, "mash_water_l");
            cJSON *sparge_water = cJSON_GetObjectItem(rec, "sparge_water_l");

            if (name && cJSON_IsString(name)) {
                strncpy(cmd.params.recipe.name, name->valuestring, sizeof(cmd.params.recipe.name) - 1);
            }
            cmd.params.recipe.boil_time_min = boil_time ? (uint16_t)boil_time->valueint : 60;
            cmd.params.recipe.hop_stand_min = (hop_stand && cJSON_IsNumber(hop_stand) && hop_stand->valueint > 0) ? (uint16_t)hop_stand->valueint : 0;
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

            // Fermentation steps array
            cJSON *ferm_steps = cJSON_GetObjectItem(rec, "fermentation_steps");
            if (ferm_steps && cJSON_IsArray(ferm_steps)) {
                int count = cJSON_GetArraySize(ferm_steps);
                cmd.params.recipe.fermentation_step_count = count > MAX_FERMENTATION_STEPS ? MAX_FERMENTATION_STEPS : count;
                for (int i = 0; i < cmd.params.recipe.fermentation_step_count; i++) {
                    cJSON *f_item = cJSON_GetArrayItem(ferm_steps, i);
                    cJSON *t = cJSON_GetObjectItem(f_item, "temp_c");
                    cJSON *dh = cJSON_GetObjectItem(f_item, "duration_hours");
                    cJSON *dd = cJSON_GetObjectItem(f_item, "duration_days");
                    cmd.params.recipe.fermentation_steps[i].temp_c = t ? (float)t->valuedouble : 20.0f;
                    if (dh && cJSON_IsNumber(dh)) {
                        cmd.params.recipe.fermentation_steps[i].duration_hours = (uint16_t)dh->valueint;
                    } else if (dd && cJSON_IsNumber(dd)) {
                        cmd.params.recipe.fermentation_steps[i].duration_hours = (uint16_t)(dd->valueint * 24);
                    } else {
                        cmd.params.recipe.fermentation_steps[i].duration_hours = 168;
                    }
                }
            }
        }
    }

    if (!handled) {
        ESP_LOGW(TAG, "Unrecognized command action '%s'", action_str);
        mqtt_manager_publish_cmd_ack(target_mac_str, req_id, action_str, "rejected", 404, 0);
        cJSON_Delete(root);
        return;
    }

    // Dispatch via equipment_manager; fail-fast error ack is sent immediately if disconnected,
    // while success ack is emitted by on_ble_cmd_seq_complete once Core 0 writes the BLE packets.
    uint8_t resolved_mac[6] = {0};
    if (is_recipe) {
        esp_err_t err = equipment_manager_dispatch_recipe_ex(target_slot,
                                                             cmd.target_name[0] ? cmd.target_name : NULL,
                                                             mac_resolved ? target_mac : NULL,
                                                             eq_type,
                                                             req_id,
                                                             &cmd.params.recipe,
                                                             resolved_mac);
        if (err != ESP_OK) {
            mqtt_manager_publish_cmd_ack(target_mac_str, req_id, action_str, "error", (int)err, 0);
        }
    } else {
        esp_err_t err = equipment_manager_dispatch_command(&cmd, resolved_mac);
        if (err != ESP_OK) {
            mqtt_manager_publish_cmd_ack(target_mac_str, req_id, action_str, "error", (int)err, 0);
        }
    }

    cJSON_Delete(root);
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

        // 2. Publish Equipment List Catalog (retained on .../equipment)
        broadcast_equipment_list(NULL);

        // 3. Publish Manifest and Snapshot for all active peers
        ble_peer_device_t peers[BLE_MAX_SIMULTANEOUS_CONNECTIONS];
        uint8_t count = 0;
        if (ble_manager_get_peers(peers, BLE_MAX_SIMULTANEOUS_CONNECTIONS, &count) == ESP_OK) {
            for (uint8_t i = 0; i < count; i++) {
                publish_equipment_manifest(&peers[i], true);

                char mac_str[13];
                snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
                         peers[i].addr[0], peers[i].addr[1], peers[i].addr[2],
                         peers[i].addr[3], peers[i].addr[4], peers[i].addr[5]);

                g30_state_t b_state = s_g30_state;
                bool g30_conn = false;
                g30_controller_get_state(&b_state, &g30_conn, NULL, NULL);

                char snapshot_json[768];
                snprintf(snapshot_json, sizeof(snapshot_json),
                         "{\"mac\":\"%s\",\"equipment_type\":\"%s\",\"model\":\"%s\","
                         "\"rssi\":%d,\"state\":{"
                         "\"temperature\":{\"current_c\":%.1f,\"target_c\":%.1f,\"boil_c\":%.1f},"
                         "\"heating\":{\"active\":%s,\"power_pct\":%u,\"ramping\":%s},"
                         "\"pump\":{\"active\":%s},"
                         "\"timer\":{\"active\":%s,\"paused\":%s,\"remaining_seconds\":%lu,\"total_seconds\":%lu},"
                         "\"stage\":{\"step_index\":%u,\"auto_mode\":%s},"
                         "\"interaction\":{\"required\":%s,\"code\":%u,\"message\":\"%s\"},\"flags\":{\"delayed_heat\":%s,\"interrupted\":%s,\"manual_power\":%s,\"sparge_alert\":%s},\"error_code\":%u"
                         "},\"timestamp_ms\":%lu}",
                         mac_str,
                         equip_type_to_str(peers[i].equipment_type),
                         peers[i].device_name[0] ? peers[i].device_name : "Grainfather G30 Connect",
                         peers[i].rssi,
                         b_state.current_temp_c, b_state.target_temp_c, b_state.boil_temp_c,
                         b_state.heat_active ? "true" : "false",
                         b_state.heat_power_pct,
                         b_state.ramp_active ? "true" : "false",
                         b_state.pump_active ? "true" : "false",
                         b_state.timer.active ? "true" : "false",
                         b_state.timer.paused ? "true" : "false",
                         (unsigned long)b_state.timer.remaining_seconds,
                         (unsigned long)b_state.timer.total_seconds,
                         b_state.stage.step_index,
                         b_state.auto_mode ? "true" : "false",
                         b_state.interaction.required ? "true" : "false",
                         b_state.interaction.code,
                         b_state.interaction.message,
                         b_state.delayed_heat ? "true" : "false",
                         b_state.interrupted ? "true" : "false",
                         b_state.manual_power ? "true" : "false",
                         b_state.sparge_alert ? "true" : "false",
                         b_state.error_code,
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
// Telemetry Relay Worker Task (FreeRTOS Core 1) - Upstream BLE -> WiFi/MQTT/WS
// -----------------------------------------------------------------------------
static void telemetry_relay_task(void *pvParameters) {
    ble_telemetry_packet_t packet;
    char payload_json[768];

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

            // Route notification through multi-equipment manager first
            bool eq_should_pub = false;
            char eq_json[768] = {0};
            esp_err_t eq_err = equipment_manager_process_notification(packet.mac,
                                                                      packet.rssi,
                                                                      packet.data,
                                                                      packet.len,
                                                                      packet.timestamp_ms,
                                                                      &eq_should_pub,
                                                                      eq_json,
                                                                      sizeof(eq_json));

            // Sync local boiler state for G30 telemetry evaluation
            g30_notification_type_t notif_type = G30_NOTIF_UNKNOWN;
            esp_err_t err = ESP_FAIL;
            if (s_g30_state_mutex != NULL && xSemaphoreTake(s_g30_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                err = g30_parse_notification(packet.data, packet.len, &s_g30_state, &notif_type);
                xSemaphoreGive(s_g30_state_mutex);
            } else {
                err = g30_parse_notification(packet.data, packet.len, &s_g30_state, &notif_type);
            }
            (void)eq_err;

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
                                        (s_g30_state.timer.paused != s_prev_state.timer.paused) ||
                                        (s_g30_state.delayed_heat != s_prev_state.delayed_heat) ||
                                        (s_g30_state.sparge_alert != s_prev_state.sparge_alert) ||
                                        (s_g30_state.error_code != s_prev_state.error_code);

                uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                float temp_delta = fabsf(s_g30_state.current_temp_c - s_prev_state.current_temp_c);
                bool throttle_expired = (now_ms - s_last_telemetry_pub_ms >= TELEMETRY_CADENCE_THROTTLE_MS);
                bool temp_delta_exceeded = (temp_delta >= TELEMETRY_TEMP_DELTA_MIN_C);

                if (discrete_changed || throttle_expired || temp_delta_exceeded) {
                    snprintf(payload_json, sizeof(payload_json),
                             "{\"event\":\"telemetry\",\"mac\":\"%s\",\"equipment_type\":\"boiler\",\"model\":\"Grainfather G30\","
                             "\"rssi\":%d,\"state\":{"
                             "\"temperature\":{\"current_c\":%.1f,\"target_c\":%.1f,\"boil_c\":%.1f},"
                             "\"heating\":{\"active\":%s,\"power_pct\":%u,\"ramping\":%s},"
                             "\"pump\":{\"active\":%s},"
                             "\"timer\":{\"active\":%s,\"paused\":%s,\"remaining_seconds\":%lu,\"total_seconds\":%lu},"
                             "\"stage\":{\"step_index\":%u,\"auto_mode\":%s},"
                             "\"interaction\":{\"required\":%s,\"code\":%u,\"message\":\"%s\"},\"flags\":{\"delayed_heat\":%s,\"interrupted\":%s,\"manual_power\":%s,\"sparge_alert\":%s},\"error_code\":%u"
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
                             s_g30_state.delayed_heat ? "true" : "false",
                             s_g30_state.interrupted ? "true" : "false",
                             s_g30_state.manual_power ? "true" : "false",
                             s_g30_state.sparge_alert ? "true" : "false",
                             s_g30_state.error_code,
                             (unsigned long)packet.timestamp_ms);

                    if (mqtt_manager_is_connected()) {
                        mqtt_manager_publish_equipment_telemetry(mac_str, payload_json);
                    }
                    web_server_ws_broadcast(payload_json);

                    s_prev_state = s_g30_state;
                    s_last_telemetry_pub_ms = now_ms;
                }
            } else {
                // Fallback payload for raw or non-G30 frames
                snprintf(payload_json, sizeof(payload_json),
                         "{\"event\":\"telemetry\",\"dev\":%u,\"mac\":\"%s\",\"rssi\":%d,\"len\":%u,\"ts\":%lu}",
                         packet.device_index, mac_str, packet.rssi,
                         (unsigned int)packet.len, (unsigned long)packet.timestamp_ms);

                if (mqtt_manager_is_connected()) {
                    mqtt_manager_publish_equipment_telemetry(mac_str, payload_json);
                }
                web_server_ws_broadcast(payload_json);
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

    // 2. Initialize Persistent Configuration Store & Security / Pairing Manager
    ESP_ERROR_CHECK(config_store_init());
    ESP_ERROR_CHECK(auth_manager_init());

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

    // 4b. Create G30 State Mutex & Initialize Multi-Equipment Manager
    s_g30_state_mutex = xSemaphoreCreateMutex();
    if (s_g30_state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create s_g30_state_mutex");
        return;
    }
    ESP_ERROR_CHECK(equipment_manager_init(s_ble_cmd_queue));

    // Restore persisted paired BLE devices and their unique names from NVS
    paired_ble_device_t paired_ble[CONFIG_STORE_MAX_PAIRED_BLE];
    if (config_store_get_paired_ble(paired_ble) == ESP_OK) {
        for (uint8_t i = 0; i < CONFIG_STORE_MAX_PAIRED_BLE; i++) {
            if (paired_ble[i].active) {
                uint8_t slot_idx = 0;
                equipment_manager_register_paired_device(paired_ble[i].addr,
                                                         (equipment_type_t)paired_ble[i].equipment_type,
                                                         paired_ble[i].name,
                                                         paired_ble[i].model,
                                                         &slot_idx);
                ESP_LOGI(TAG, "Restored paired BLE device [slot %u]: '%s' (%02X:%02X:%02X:%02X:%02X:%02X)",
                         (unsigned int)(slot_idx + 1),
                         paired_ble[i].name,
                         paired_ble[i].addr[0], paired_ble[i].addr[1], paired_ble[i].addr[2],
                         paired_ble[i].addr[3], paired_ble[i].addr[4], paired_ble[i].addr[5]);
            }
        }
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
    ble_manager_register_seq_cb(on_ble_cmd_seq_complete);

    // 8. Initialize MQTT Subsystem with Gateway ID & Callbacks
    ESP_ERROR_CHECK(mqtt_manager_init(s_gateway_id, process_inbound_command, on_mqtt_connection_status_change));

    // 9. Initialize Wi-Fi Subsystem (SoftAP broadcasting if no STA connected)
    ESP_ERROR_CHECK(wifi_manager_init(on_wifi_sta_status_change));

    // 10. Start Embedded Web Configuration Server on port 80 (Core 0)
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "BrewLink Gateway initialization complete! Free Heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());
}
