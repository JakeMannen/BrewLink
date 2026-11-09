/**
 * @file web_server.c
 * @brief Embedded Web Configuration, Equipment-Agnostic REST API, and WebSocket Server
 */

#include "web_server.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "cJSON.h"

#include "config.h"
#include "config_store.h"
#include "auth_manager.h"
#include "equipment_manager.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "ble_manager.h"
#include "grainfather_g30.h"
#include "g30_controller.h"
#include "web_page.h"

static const char *TAG = "WEB_SERVER";
static httpd_handle_t s_server = NULL;

#define MAX_WS_CLIENTS 4
static int s_ws_client_fds[MAX_WS_CLIENTS] = {-1, -1, -1, -1};

// Helper: parse MAC string ("XX:XX:XX:XX:XX:XX" or "XXXXXXXXXXXX") to 6-byte array
static bool parse_mac_string(const char *str, uint8_t *mac) {
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
        char hex_byte[3] = {clean[i * 2], clean[i * 2 + 1], '\0'};
        mac[i] = (uint8_t)strtoul(hex_byte, NULL, 16);
    }
    return true;
}

static const char *equipment_type_to_str(equipment_type_t type) {
    switch (type) {
        case EQUIPMENT_TYPE_BOILER:        return "boiler";
        case EQUIPMENT_TYPE_FERMENTER:     return "fermenter";
        case EQUIPMENT_TYPE_COOLER_HEATER: return "cooler";
        default:                           return "generic";
    }
}

static equipment_type_t parse_equipment_type_str(const char *str) {
    if (str == NULL || str[0] == '\0') return EQUIPMENT_TYPE_UNKNOWN;
    if (strcmp(str, "boiler") == 0)    return EQUIPMENT_TYPE_BOILER;
    if (strcmp(str, "fermenter") == 0) return EQUIPMENT_TYPE_FERMENTER;
    if (strcmp(str, "cooler") == 0 || strcmp(str, "chiller") == 0) return EQUIPMENT_TYPE_COOLER_HEATER;
    return EQUIPMENT_TYPE_UNKNOWN;
}

// Verify HTTP/WebSocket authentication via Authorization header, X-BrewLink-Token, or ?token=
static bool verify_external_request_auth(httpd_req_t *req) {
    bool is_paired = false;
    auth_manager_get_status(NULL, NULL, NULL, &is_paired, NULL);

    char hdr_buf[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr_buf, sizeof(hdr_buf)) == ESP_OK) {
        if (auth_manager_verify_token(hdr_buf)) {
            return true;
        }
        return false;
    }

    if (httpd_req_get_hdr_value_str(req, "X-BrewLink-Token", hdr_buf, sizeof(hdr_buf)) == ESP_OK) {
        if (auth_manager_verify_token(hdr_buf)) {
            return true;
        }
        return false;
    }

    char query_buf[160] = {0};
    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
        char token_val[96] = {0};
        if (httpd_query_key_value(query_buf, "token", token_val, sizeof(token_val)) == ESP_OK) {
            if (auth_manager_verify_token(token_val)) {
                return true;
            }
            return false;
        }
    }

    // If an external client has explicitly paired, require token on external endpoints
    if (is_paired) {
        return false;
    }
    return true;
}

// Helper: parse a cJSON recipe object into g30_recipe_t
static void parse_json_recipe_object(cJSON *rec, g30_recipe_t *out_recipe) {
    memset(out_recipe, 0, sizeof(g30_recipe_t));
    if (rec == NULL) return;

    cJSON *name_item = cJSON_GetObjectItem(rec, "name");
    cJSON *boil_item = cJSON_GetObjectItem(rec, "boil_time_min");
    cJSON *hop_stand_item = cJSON_GetObjectItem(rec, "hop_stand_min");
    cJSON *mash_w_item = cJSON_GetObjectItem(rec, "mash_water_l");
    cJSON *sparge_w_item = cJSON_GetObjectItem(rec, "sparge_water_l");
    cJSON *steps_item = cJSON_GetObjectItem(rec, "mash_steps");
    cJSON *hops_item = cJSON_GetObjectItem(rec, "boil_additions_min");
    cJSON *ferm_item = cJSON_GetObjectItem(rec, "fermentation_steps");
    cJSON *og_item = cJSON_GetObjectItem(rec, "target_og_sg");
    cJSON *fg_item = cJSON_GetObjectItem(rec, "target_fg_sg");

    if (name_item && cJSON_IsString(name_item) && name_item->valuestring != NULL) {
        strncpy(out_recipe->name, name_item->valuestring, sizeof(out_recipe->name) - 1);
    } else {
        strncpy(out_recipe->name, "Brew Recipe", sizeof(out_recipe->name) - 1);
    }

    out_recipe->boil_time_min = (boil_item && cJSON_IsNumber(boil_item)) ? (uint16_t)boil_item->valueint : 60;
    out_recipe->hop_stand_min = (hop_stand_item && cJSON_IsNumber(hop_stand_item) && hop_stand_item->valueint > 0) ? (uint16_t)hop_stand_item->valueint : 0;
    out_recipe->mash_water_l = (mash_w_item && cJSON_IsNumber(mash_w_item)) ? (float)mash_w_item->valuedouble : 16.5f;
    out_recipe->sparge_water_l = (sparge_w_item && cJSON_IsNumber(sparge_w_item)) ? (float)sparge_w_item->valuedouble : 14.0f;
    out_recipe->target_og_sg = (og_item && cJSON_IsNumber(og_item)) ? (float)og_item->valuedouble : 0.0f;
    out_recipe->target_fg_sg = (fg_item && cJSON_IsNumber(fg_item)) ? (float)fg_item->valuedouble : 0.0f;

    if (steps_item && cJSON_IsArray(steps_item)) {
        int count = cJSON_GetArraySize(steps_item);
        out_recipe->mash_step_count = count > MAX_MASH_STEPS ? MAX_MASH_STEPS : (uint8_t)count;
        for (int i = 0; i < out_recipe->mash_step_count; i++) {
            cJSON *s = cJSON_GetArrayItem(steps_item, i);
            cJSON *t = cJSON_GetObjectItem(s, "temp_c");
            cJSON *d = cJSON_GetObjectItem(s, "duration_min");
            out_recipe->mash_steps[i].temp_c = (t && cJSON_IsNumber(t)) ? (float)t->valuedouble : 65.0f;
            out_recipe->mash_steps[i].duration_min = (d && cJSON_IsNumber(d)) ? (uint16_t)d->valueint : 60;
        }
    }

    if (hops_item && cJSON_IsArray(hops_item)) {
        int count = cJSON_GetArraySize(hops_item);
        out_recipe->boil_addition_count = count > MAX_BOIL_ADDITIONS ? MAX_BOIL_ADDITIONS : (uint8_t)count;
        for (int i = 0; i < out_recipe->boil_addition_count; i++) {
            cJSON *h = cJSON_GetArrayItem(hops_item, i);
            out_recipe->boil_additions_min[i] = (h && cJSON_IsNumber(h)) ? (uint16_t)h->valueint : 0;
        }
    }

    if (ferm_item && cJSON_IsArray(ferm_item)) {
        int count = cJSON_GetArraySize(ferm_item);
        out_recipe->fermentation_step_count = count > MAX_FERMENTATION_STEPS ? MAX_FERMENTATION_STEPS : (uint8_t)count;
        for (int i = 0; i < out_recipe->fermentation_step_count; i++) {
            cJSON *f = cJSON_GetArrayItem(ferm_item, i);
            cJSON *t = cJSON_GetObjectItem(f, "temp_c");
            cJSON *dh = cJSON_GetObjectItem(f, "duration_hours");
            cJSON *dd = cJSON_GetObjectItem(f, "duration_days");
            out_recipe->fermentation_steps[i].temp_c = (t && cJSON_IsNumber(t)) ? (float)t->valuedouble : 20.0f;
            if (dh && cJSON_IsNumber(dh)) {
                out_recipe->fermentation_steps[i].duration_hours = (uint16_t)dh->valueint;
            } else if (dd && cJSON_IsNumber(dd)) {
                out_recipe->fermentation_steps[i].duration_hours = (uint16_t)(dd->valueint * 24);
            } else {
                out_recipe->fermentation_steps[i].duration_hours = 168;
            }
        }
    }
}

// Helper: serialize an equipment_slot_t to a cJSON object
static cJSON *serialize_equipment_slot_cjson(const equipment_slot_t *slot, uint8_t slot_num) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "slot", slot_num);
    cJSON_AddBoolToObject(item, "paired", (slot->paired || slot->connected));
    cJSON_AddBoolToObject(item, "connected", slot->connected);

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             slot->mac[0], slot->mac[1], slot->mac[2],
             slot->mac[3], slot->mac[4], slot->mac[5]);
    const char *model_str = slot->model[0] ? slot->model : "BrewLink Equipment";
    const char *name_str = slot->name[0] ? slot->name : model_str;
    cJSON_AddStringToObject(item, "name", name_str);
    cJSON_AddStringToObject(item, "model", model_str);
    cJSON_AddStringToObject(item, "equipment_type", equipment_type_to_str(slot->equipment_type));
    cJSON_AddStringToObject(item, "mac", mac_str);
    cJSON_AddNumberToObject(item, "rssi", slot->rssi);

    uint32_t caps = slot->driver ? slot->driver->capabilities_mask : 0;
    cJSON *cap_obj = cJSON_AddObjectToObject(item, "capabilities");
    cJSON_AddBoolToObject(cap_obj, "recipe_upload", ((caps & EQ_CAP_MASH_RECIPE) || (caps & EQ_CAP_FERM_RECIPE)));
    cJSON_AddBoolToObject(cap_obj, "mash_profile", (caps & EQ_CAP_MASH_RECIPE) != 0);
    cJSON_AddBoolToObject(cap_obj, "fermentation_profile", (caps & EQ_CAP_FERM_RECIPE) != 0);
    cJSON_AddBoolToObject(cap_obj, "pump_control", (caps & EQ_CAP_PUMP_CONTROL) != 0);
    cJSON_AddBoolToObject(cap_obj, "heat_control", (caps & EQ_CAP_HEAT_CONTROL) != 0);

    cJSON *st = cJSON_AddObjectToObject(item, "state");
    if (slot->equipment_type == EQUIPMENT_TYPE_BOILER) {
        const g30_state_t *b = &slot->state.boiler;
        cJSON_AddNumberToObject(st, "current_temp_c", (double)b->current_temp_c);
        cJSON_AddNumberToObject(st, "target_temp_c", (double)b->target_temp_c);
        cJSON_AddNumberToObject(st, "boil_temp_c", (double)b->boil_temp_c);
        cJSON_AddBoolToObject(st, "heat_active", b->heat_active);
        cJSON_AddNumberToObject(st, "heat_power_pct", b->heat_power_pct);
        cJSON_AddBoolToObject(st, "pump_active", b->pump_active);
        cJSON_AddBoolToObject(st, "auto_mode", b->auto_mode);
        cJSON_AddBoolToObject(st, "ramp_active", b->ramp_active);

        cJSON *tm = cJSON_AddObjectToObject(st, "timer");
        cJSON_AddBoolToObject(tm, "active", b->timer.active);
        cJSON_AddBoolToObject(tm, "paused", b->timer.paused);
        cJSON_AddNumberToObject(tm, "remaining_seconds", b->timer.remaining_seconds);
        cJSON_AddNumberToObject(tm, "total_seconds", b->timer.total_seconds);

        cJSON *sg = cJSON_AddObjectToObject(st, "stage");
        cJSON_AddNumberToObject(sg, "step_index", b->stage.step_index);
        cJSON_AddStringToObject(sg, "step_name", b->stage.step_name[0] ? b->stage.step_name : "Idle");

        cJSON *ia = cJSON_AddObjectToObject(st, "interaction");
        cJSON_AddBoolToObject(ia, "required", b->interaction.required);
        cJSON_AddNumberToObject(ia, "code", b->interaction.code);
        cJSON_AddStringToObject(ia, "message", b->interaction.message[0] ? b->interaction.message : "Normal operation");
    } else if (slot->equipment_type == EQUIPMENT_TYPE_FERMENTER) {
        const placeholder_fermenter_state_t *f = &slot->state.fermenter;
        cJSON_AddNumberToObject(st, "beer_temp_c", (double)f->beer_temp_c);
        cJSON_AddNumberToObject(st, "chamber_temp_c", (double)f->chamber_temp_c);
        cJSON_AddNumberToObject(st, "target_temp_c", (double)f->target_temp_c);
        cJSON_AddNumberToObject(st, "gravity_sg", (double)f->gravity_sg);
    } else if (slot->equipment_type == EQUIPMENT_TYPE_COOLER_HEATER) {
        const placeholder_cooler_state_t *c = &slot->state.cooler;
        cJSON_AddNumberToObject(st, "reservoir_temp_c", (double)c->reservoir_temp_c);
        cJSON_AddNumberToObject(st, "current_temp_c", (double)c->zones[0].current_temp_c);
        cJSON_AddNumberToObject(st, "target_temp_c", (double)c->zones[0].target_temp_c);
    }
    return item;
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

    bool pair_open = false;
    uint32_t pair_rem = 0;
    bool client_paired = false;
    char client_name[CONFIG_STORE_CLIENT_NAME_MAX_LEN] = {0};
    auth_manager_get_status(&pair_open, NULL, &pair_rem, &client_paired, client_name);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "gateway_id", mqtt_manager_get_gateway_id());

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

    // Pairing object
    cJSON *pair = cJSON_AddObjectToObject(root, "pairing");
    cJSON_AddBoolToObject(pair, "window_open", pair_open);
    cJSON_AddNumberToObject(pair, "remaining_sec", pair_rem);
    cJSON_AddBoolToObject(pair, "client_paired", client_paired);
    cJSON_AddStringToObject(pair, "client_name", client_name);

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

// -----------------------------------------------------------------------------
// Pairing REST Endpoints (/api/pair/*)
// -----------------------------------------------------------------------------

// GET /api/pair/status
static esp_err_t api_pair_status_get_handler(httpd_req_t *req) {
    bool open = false;
    char pin[AUTH_PAIRING_PIN_LEN + 1] = {0};
    uint32_t rem = 0;
    bool paired = false;
    char cname[CONFIG_STORE_CLIENT_NAME_MAX_LEN] = {0};
    auth_manager_get_status(&open, pin, &rem, &paired, cname);

    paired_client_t clients[CONFIG_STORE_MAX_PAIRED_CLIENTS];
    uint8_t count = 0;
    auth_manager_get_paired_clients(clients, &count);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "window_open", open);
    cJSON_AddStringToObject(root, "pin", open ? pin : "");
    cJSON_AddNumberToObject(root, "remaining_sec", rem);
    cJSON_AddBoolToObject(root, "client_paired", (paired || count > 0));
    cJSON_AddStringToObject(root, "client_name", cname);
    cJSON_AddStringToObject(root, "paired_client_name", cname);
    cJSON_AddNumberToObject(root, "count", count);

    cJSON *arr = cJSON_AddArrayToObject(root, "clients");
    for (int i = 0; i < CONFIG_STORE_MAX_PAIRED_CLIENTS; i++) {
        if (clients[i].active) {
            cJSON *c_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(c_obj, "client_name", clients[i].client_name);
            cJSON_AddStringToObject(c_obj, "mqtt_topic_key", clients[i].mqtt_topic_key);
            cJSON_AddItemToArray(arr, c_obj);
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    return ret;
}

// POST /api/pair/start - Open 60-second pairing window and generate 6-digit PIN
static esp_err_t api_pair_start_post_handler(httpd_req_t *req) {
    char pin[AUTH_PAIRING_PIN_LEN + 1] = {0};
    uint32_t ttl = 0;
    auth_manager_start_pairing(pin, &ttl);

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"pin\":\"%s\",\"expires_in_sec\":%lu}",
             pin, (unsigned long)ttl);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// POST /api/pair/claim - Claim pairing window (code-free push-button or optional 6-digit PIN) for persistent API Token & MQTT Topic Key
static esp_err_t api_pair_claim_post_handler(httpd_req_t *req) {
    char buf[256];
    int remaining = req->content_len;
    if (remaining < 0 || remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload size");
        return ESP_FAIL;
    }

    cJSON *root = NULL;
    const char *pin_str = NULL;
    const char *cname = "BeerXML-System";

    if (remaining > 0) {
        int ret = httpd_req_recv(req, buf, remaining);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
            return ESP_FAIL;
        }
        buf[ret] = '\0';

        root = cJSON_Parse(buf);
        if (root == NULL) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
        }

        cJSON *pin_item = cJSON_GetObjectItem(root, "pin");
        if (pin_item == NULL) {
            pin_item = cJSON_GetObjectItem(root, "pairing_code");
        }
        if (pin_item && cJSON_IsString(pin_item) && pin_item->valuestring != NULL) {
            pin_str = pin_item->valuestring;
        }

        cJSON *name_item = cJSON_GetObjectItem(root, "client_name");
        if (name_item && cJSON_IsString(name_item) && name_item->valuestring != NULL && name_item->valuestring[0] != '\0') {
            cname = name_item->valuestring;
        }
    }

    char token[CONFIG_STORE_TOKEN_MAX_LEN] = {0};
    char topic_key[CONFIG_STORE_TOPIC_KEY_MAX_LEN] = {0};

    esp_err_t err = auth_manager_claim_pin(pin_str, cname, token, topic_key);
    if (root) {
        cJSON_Delete(root);
    }

    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"error_code\":\"PAIRING_WINDOW_CLOSED\",\"message\":\"Pairing window is not open or has expired\"}", HTTPD_RESP_USE_STRLEN);
    } else if (err != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"error_code\":\"INVALID_PIN\",\"message\":\"Invalid pairing PIN\"}", HTTPD_RESP_USE_STRLEN);
    }

    wifi_mgr_status_t wifi_stat;
    wifi_manager_get_status(&wifi_stat);
    brewlink_config_t cfg;
    config_store_get(&cfg);

    const char *gw_id = mqtt_manager_get_gateway_id();
    const char *ip_str = wifi_stat.sta_connected ? wifi_stat.sta_ip : "192.168.4.1";

    char ws_url[96];
    snprintf(ws_url, sizeof(ws_url), "ws://%s/ws", ip_str);

    char eff_prefix[128];
    const char *base_p = cfg.mqtt_topic_prefix[0] ? cfg.mqtt_topic_prefix : MQTT_TOPIC_GATEWAY_PREFIX;
    size_t blen = strlen(base_p);
    snprintf(eff_prefix, sizeof(eff_prefix), "%s%s%s/", base_p, (blen > 0 && base_p[blen - 1] == '/') ? "" : "/", topic_key);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "paired");
    cJSON_AddStringToObject(resp, "gateway_id", gw_id);
    cJSON_AddStringToObject(resp, "client_name", cfg.paired_client_name);
    cJSON_AddStringToObject(resp, "api_token", token);
    cJSON_AddStringToObject(resp, "ws_url", ws_url);
    cJSON_AddStringToObject(resp, "mqtt_broker_uri", cfg.mqtt_broker_uri);
    cJSON_AddStringToObject(resp, "mqtt_topic_key", topic_key);
    cJSON_AddStringToObject(resp, "mqtt_topic_prefix", eff_prefix);

    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    esp_err_t sret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    return sret;
}

// POST /api/pair/delete - Delete a paired external client by unique name (or all if omitted/empty)
static esp_err_t api_pair_delete_post_handler(httpd_req_t *req) {
    char buf[160] = {0};
    int remaining = req->content_len;
    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }

    char target_client[CONFIG_STORE_CLIENT_NAME_MAX_LEN] = {0};
    if (remaining > 0) {
        int ret = httpd_req_recv(req, buf, remaining);
        if (ret > 0) {
            buf[ret] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root != NULL) {
                cJSON *name_item = cJSON_GetObjectItem(root, "client_name");
                if (name_item && cJSON_IsString(name_item) && name_item->valuestring != NULL) {
                    strncpy(target_client, name_item->valuestring, sizeof(target_client) - 1);
                }
                cJSON_Delete(root);
            }
        }
    }

    esp_err_t err = auth_manager_delete_paired_client(target_client[0] ? target_client : NULL);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Paired external client not found\"}", HTTPD_RESP_USE_STRLEN);
    } else if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Paired external client deleted\"}", HTTPD_RESP_USE_STRLEN);
}

// -----------------------------------------------------------------------------
// Equipment-Agnostic REST Endpoints (/api/equipment/*)
// -----------------------------------------------------------------------------

// GET /api/equipment - List all paired/connected BLE equipment across all slots
static esp_err_t api_equipment_get_handler(httpd_req_t *req) {
    if (!verify_external_request_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"error_code\":\"UNAUTHORIZED\",\"message\":\"Valid Bearer api_token required\"}", HTTPD_RESP_USE_STRLEN);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "gateway_id", mqtt_manager_get_gateway_id());

    uint8_t paired_or_conn_cnt = 0;
    for (uint8_t i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        equipment_slot_t slot;
        if (equipment_manager_get_slot(i, &slot) == ESP_OK && (slot.paired || slot.connected)) {
            paired_or_conn_cnt++;
        }
    }
    cJSON_AddNumberToObject(root, "count", paired_or_conn_cnt);
    cJSON *eq_arr = cJSON_AddArrayToObject(root, "equipment");

    for (uint8_t i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        equipment_slot_t slot;
        if (equipment_manager_get_slot(i, &slot) == ESP_OK && (slot.paired || slot.connected)) {
            cJSON_AddItemToArray(eq_arr, serialize_equipment_slot_cjson(&slot, i + 1));
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    return ret;
}

// POST /api/equipment/recipe - Dispatch Unified Recipe to any connected equipment (Boiler, Fermenter, Cooler)
static esp_err_t api_equipment_recipe_post_handler(httpd_req_t *req) {
    if (!verify_external_request_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"error_code\":\"UNAUTHORIZED\",\"message\":\"Valid Bearer api_token required\"}", HTTPD_RESP_USE_STRLEN);
    }

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recipe payload size must be between 1 and 2048 bytes");
        return ESP_FAIL;
    }

    char *buf = malloc(remaining + 1);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *req_id_item = cJSON_GetObjectItem(root, "req_id");
    char req_id[16] = "http-recipe";
    if (req_id_item && cJSON_IsString(req_id_item) && req_id_item->valuestring != NULL) {
        strncpy(req_id, req_id_item->valuestring, sizeof(req_id) - 1);
    }

    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *scope = params ? params : root;

    uint8_t target_mac[6] = {0};
    cJSON *mac_item = cJSON_GetObjectItem(scope, "target_mac");
    if (mac_item == NULL) mac_item = cJSON_GetObjectItem(root, "target_mac");
    if (mac_item && cJSON_IsString(mac_item)) {
        parse_mac_string(mac_item->valuestring, target_mac);
    }

    cJSON *slot_item = cJSON_GetObjectItem(scope, "slot");
    if (slot_item == NULL) slot_item = cJSON_GetObjectItem(root, "slot");
    uint8_t target_slot = (slot_item && cJSON_IsNumber(slot_item) && slot_item->valueint >= 1)
                          ? (uint8_t)slot_item->valueint : 0;

    char target_name[32] = {0};
    cJSON *tname_item = cJSON_GetObjectItem(scope, "target_name");
    if (tname_item == NULL) tname_item = cJSON_GetObjectItem(root, "target_name");
    if (tname_item == NULL) tname_item = cJSON_GetObjectItem(scope, "equipment_name");
    if (tname_item == NULL) tname_item = cJSON_GetObjectItem(root, "equipment_name");
    if (tname_item && cJSON_IsString(tname_item) && tname_item->valuestring != NULL) {
        strncpy(target_name, tname_item->valuestring, sizeof(target_name) - 1);
    }

    cJSON *type_item = cJSON_GetObjectItem(scope, "equipment_type");
    if (type_item == NULL) type_item = cJSON_GetObjectItem(root, "equipment_type");
    equipment_type_t target_type = (type_item && cJSON_IsString(type_item))
                                   ? parse_equipment_type_str(type_item->valuestring)
                                   : EQUIPMENT_TYPE_BOILER;

    cJSON *rec_obj = cJSON_GetObjectItem(scope, "recipe");
    if (rec_obj == NULL) rec_obj = cJSON_GetObjectItem(root, "recipe");
    if (rec_obj == NULL) rec_obj = scope;

    g30_recipe_t recipe;
    parse_json_recipe_object(rec_obj, &recipe);
    cJSON_Delete(root);

    uint8_t resolved_mac[6] = {0};
    esp_err_t err = equipment_manager_dispatch_recipe_ex(target_slot,
                                                         target_name[0] ? target_name : NULL,
                                                         target_mac,
                                                         target_type,
                                                         req_id,
                                                         &recipe,
                                                         resolved_mac);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req,
            "{\"status\":\"error\",\"error_code\":\"EQUIPMENT_OFFLINE\","
            "\"message\":\"Target equipment is disconnected from BrewLink\"}",
            HTTPD_RESP_USE_STRLEN);
    } else if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req,
            "{\"status\":\"error\",\"error_code\":\"INVALID_RECIPE\","
            "\"message\":\"Recipe is missing required steps for the target equipment type\"}",
            HTTPD_RESP_USE_STRLEN);
    } else if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req,
            "{\"status\":\"error\",\"error_code\":\"QUEUE_FULL\","
            "\"message\":\"BLE command queue full; please retry\"}",
            HTTPD_RESP_USE_STRLEN);
    }

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             resolved_mac[0], resolved_mac[1], resolved_mac[2],
             resolved_mac[3], resolved_mac[4], resolved_mac[5]);

    char resp_buf[192];
    snprintf(resp_buf, sizeof(resp_buf),
             "{\"status\":\"ok\",\"req_id\":\"%s\",\"mac\":\"%s\","
             "\"message\":\"Recipe transfer queued to equipment\"}",
             req_id, mac_str);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp_buf, HTTPD_RESP_USE_STRLEN);
}

// Helper: parse control JSON and populate g30_command_t
static bool parse_json_control_command(cJSON *root, g30_command_t *cmd) {
    memset(cmd, 0, sizeof(g30_command_t));

    cJSON *req_id_item = cJSON_GetObjectItem(root, "req_id");
    if (req_id_item && cJSON_IsString(req_id_item) && req_id_item->valuestring != NULL) {
        strncpy(cmd->req_id, req_id_item->valuestring, sizeof(cmd->req_id) - 1);
    } else {
        strncpy(cmd->req_id, "web-cmd", sizeof(cmd->req_id) - 1);
    }

    cJSON *action_item = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action_item) || action_item->valuestring == NULL) {
        return false;
    }

    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *scope = params ? params : root;

    cJSON *mac_item = cJSON_GetObjectItem(scope, "target_mac");
    if (mac_item == NULL) mac_item = cJSON_GetObjectItem(root, "target_mac");
    if (mac_item && cJSON_IsString(mac_item)) {
        parse_mac_string(mac_item->valuestring, cmd->target_mac);
    }

    cJSON *slot_item = cJSON_GetObjectItem(scope, "slot");
    if (slot_item == NULL) slot_item = cJSON_GetObjectItem(root, "slot");
    if (slot_item && cJSON_IsNumber(slot_item) && slot_item->valueint >= 1) {
        cmd->target_slot = (uint8_t)slot_item->valueint;
    }

    cJSON *tname_item = cJSON_GetObjectItem(scope, "target_name");
    if (tname_item == NULL) tname_item = cJSON_GetObjectItem(root, "target_name");
    if (tname_item == NULL) tname_item = cJSON_GetObjectItem(scope, "equipment_name");
    if (tname_item == NULL) tname_item = cJSON_GetObjectItem(root, "equipment_name");
    if (tname_item && cJSON_IsString(tname_item) && tname_item->valuestring != NULL) {
        strncpy(cmd->target_name, tname_item->valuestring, sizeof(cmd->target_name) - 1);
    }

    cJSON *type_item = cJSON_GetObjectItem(scope, "equipment_type");
    if (type_item == NULL) type_item = cJSON_GetObjectItem(root, "equipment_type");
    cmd->equipment_type = (type_item && cJSON_IsString(type_item))
                          ? parse_equipment_type_str(type_item->valuestring)
                          : EQUIPMENT_TYPE_BOILER;

    const char *action_str = action_item->valuestring;
    if (strcmp(action_str, "set_pump") == 0) {
        cmd->action = G30_CMD_ACTION_SET_PUMP;
        cJSON *st = cJSON_GetObjectItem(scope, "state");
        cmd->params.pump_state = (st && cJSON_IsTrue(st));
        return true;
    } else if (strcmp(action_str, "set_heat") == 0) {
        cmd->action = G30_CMD_ACTION_SET_HEAT;
        cJSON *st = cJSON_GetObjectItem(scope, "state");
        cJSON *pwr = cJSON_GetObjectItem(scope, "power_pct");
        cmd->params.heat.heat_state = (st && cJSON_IsTrue(st));
        cmd->params.heat.power_pct = (pwr && cJSON_IsNumber(pwr)) ? (uint8_t)pwr->valueint : 100;
        return true;
    } else if (strcmp(action_str, "set_target_temp") == 0) {
        cmd->action = G30_CMD_ACTION_SET_TARGET_TEMP;
        cJSON *temp = cJSON_GetObjectItem(scope, "temp_c");
        if (temp && cJSON_IsNumber(temp)) {
            cmd->params.target_temp_c = (float)temp->valuedouble;
            return true;
        }
    } else if (strcmp(action_str, "ack_interaction") == 0) {
        cmd->action = G30_CMD_ACTION_ACK_INTERACTION;
        return true;
    } else if (strcmp(action_str, "press_set") == 0) {
        cmd->action = G30_CMD_ACTION_PRESS_SET;
        return true;
    } else if (strcmp(action_str, "dismiss_alert") == 0) {
        cmd->action = G30_CMD_ACTION_DISMISS_ALERT;
        return true;
    } else if (strcmp(action_str, "dismiss_sparge_alert") == 0) {
        cmd->action = G30_CMD_ACTION_DISMISS_SPARGE_ALERT;
        return true;
    } else if (strcmp(action_str, "cancel_session") == 0) {
        cmd->action = G30_CMD_ACTION_CANCEL_SESSION;
        return true;
    } else if (strcmp(action_str, "toggle_timer") == 0 ||
               strcmp(action_str, "pause_timer") == 0 ||
               strcmp(action_str, "resume_timer") == 0) {
        cmd->action = G30_CMD_ACTION_PAUSE_TIMER;
        return true;
    } else if (strcmp(action_str, "cancel_timer") == 0) {
        cmd->action = G30_CMD_ACTION_CANCEL_TIMER;
        return true;
    } else if (strcmp(action_str, "set_timer") == 0) {
        cmd->action = G30_CMD_ACTION_SET_TIMER;
        cJSON *dur = cJSON_GetObjectItem(scope, "duration_min");
        if (dur && cJSON_IsNumber(dur) && dur->valueint > 0) {
            cmd->params.timer_duration_min = (uint16_t)dur->valueint;
            return true;
        }
    }
    return false;
}

// POST /api/equipment/control - Equipment-agnostic command endpoint
static esp_err_t api_equipment_control_post_handler(httpd_req_t *req) {
    if (!verify_external_request_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"error_code\":\"UNAUTHORIZED\",\"message\":\"Valid Bearer api_token required\"}", HTTPD_RESP_USE_STRLEN);
    }

    char buf[384];
    int remaining = req->content_len;
    if (remaining <= 0 || remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
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

    g30_command_t cmd;
    bool valid = parse_json_control_command(root, &cmd);
    cJSON_Delete(root);

    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid action or parameters");
        return ESP_FAIL;
    }

    uint8_t resolved_mac[6] = {0};
    esp_err_t err = equipment_manager_dispatch_command(&cmd, resolved_mac);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"error_code\":\"EQUIPMENT_OFFLINE\",\"message\":\"Target equipment is not connected\"}", HTTPD_RESP_USE_STRLEN);
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"Controller is master during an active recipe; manual timers and temperature steps are disabled\"}", HTTPD_RESP_USE_STRLEN);
    } else if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to dispatch command");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Command dispatched to equipment\"}", HTTPD_RESP_USE_STRLEN);
}

// -----------------------------------------------------------------------------
// Two-Way WebSocket Handler (/ws)
// -----------------------------------------------------------------------------
static void register_ws_client_fd(int fd) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_client_fds[i] == fd) return;
    }
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_client_fds[i] == -1) {
            s_ws_client_fds[i] = fd;
            return;
        }
    }
    s_ws_client_fds[0] = fd;
}

static void remove_ws_client_fd(int fd) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_client_fds[i] == fd) {
            s_ws_client_fds[i] = -1;
        }
    }
}

static esp_err_t send_ws_text_to_req(httpd_req_t *req, const char *text) {
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)text;
    ws_pkt.len = strlen(text);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    return httpd_ws_send_frame(req, &ws_pkt);
}

esp_err_t web_server_ws_broadcast(const char *json_str) {
    if (s_server == NULL || json_str == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)json_str;
    ws_pkt.len = strlen(json_str);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        int fd = s_ws_client_fds[i];
        if (fd >= 0) {
            if (httpd_ws_get_fd_info(s_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
                if (httpd_ws_send_frame_async(s_server, fd, &ws_pkt) != ESP_OK) {
                    remove_ws_client_fd(fd);
                }
            } else {
                remove_ws_client_fd(fd);
            }
        }
    }
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        if (!verify_external_request_auth(req)) {
            ESP_LOGW(TAG, "Rejected unauthenticated WebSocket handshake on /ws");
            httpd_resp_set_status(req, "401 Unauthorized");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"status\":\"error\",\"error_code\":\"UNAUTHORIZED\"}", HTTPD_RESP_USE_STRLEN);
        }
        int fd = httpd_req_to_sockfd(req);
        register_ws_client_fd(fd);
        ESP_LOGI(TAG, "Authenticated WebSocket client connected (fd=%d)", fd);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    if (ws_pkt.len == 0 || ws_pkt.len > 2048) {
        return ESP_OK;
    }

    uint8_t *buf = malloc(ws_pkt.len + 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }
    buf[ws_pkt.len] = '\0';

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        cJSON *root = cJSON_Parse((const char *)buf);
        free(buf);
        if (root == NULL) {
            return send_ws_text_to_req(req, "{\"event\":\"cmd_ack\",\"status\":\"error\",\"error_code\":400,\"message\":\"Invalid JSON\"}");
        }

        cJSON *req_id_item = cJSON_GetObjectItem(root, "req_id");
        char req_id[16] = "ws-cmd";
        if (req_id_item && cJSON_IsString(req_id_item) && req_id_item->valuestring != NULL) {
            strncpy(req_id, req_id_item->valuestring, sizeof(req_id) - 1);
        }

        cJSON *action_item = cJSON_GetObjectItem(root, "action");
        const char *action_str = (action_item && cJSON_IsString(action_item)) ? action_item->valuestring : "";

        if (strcmp(action_str, "list_equipment") == 0) {
            cJSON_Delete(root);
            char list_frame[1024];
            if (equipment_manager_format_equipment_list_json(mqtt_manager_get_gateway_id(),
                                                             req_id,
                                                             list_frame,
                                                             sizeof(list_frame)) == ESP_OK) {
                return send_ws_text_to_req(req, list_frame);
            }
            return send_ws_text_to_req(req, "{\"event\":\"equipment_list\",\"count\":0,\"equipment\":[]}");
        } else if (strcmp(action_str, "load_recipe") == 0) {
            cJSON *params = cJSON_GetObjectItem(root, "params");
            cJSON *scope = params ? params : root;

            uint8_t target_mac[6] = {0};
            cJSON *mac_item = cJSON_GetObjectItem(scope, "target_mac");
            if (mac_item == NULL) mac_item = cJSON_GetObjectItem(root, "target_mac");
            if (mac_item && cJSON_IsString(mac_item)) {
                parse_mac_string(mac_item->valuestring, target_mac);
            }

            cJSON *slot_item = cJSON_GetObjectItem(scope, "slot");
            if (slot_item == NULL) slot_item = cJSON_GetObjectItem(root, "slot");
            uint8_t target_slot = (slot_item && cJSON_IsNumber(slot_item) && slot_item->valueint >= 1)
                                  ? (uint8_t)slot_item->valueint : 0;

            char target_name[32] = {0};
            cJSON *tname_item = cJSON_GetObjectItem(scope, "target_name");
            if (tname_item == NULL) tname_item = cJSON_GetObjectItem(root, "target_name");
            if (tname_item == NULL) tname_item = cJSON_GetObjectItem(scope, "equipment_name");
            if (tname_item == NULL) tname_item = cJSON_GetObjectItem(root, "equipment_name");
            if (tname_item && cJSON_IsString(tname_item) && tname_item->valuestring != NULL) {
                strncpy(target_name, tname_item->valuestring, sizeof(target_name) - 1);
            }

            cJSON *type_item = cJSON_GetObjectItem(scope, "equipment_type");
            if (type_item == NULL) type_item = cJSON_GetObjectItem(root, "equipment_type");
            equipment_type_t target_type = (type_item && cJSON_IsString(type_item))
                                           ? parse_equipment_type_str(type_item->valuestring)
                                           : EQUIPMENT_TYPE_BOILER;

            cJSON *rec_obj = cJSON_GetObjectItem(scope, "recipe");
            if (rec_obj == NULL) rec_obj = cJSON_GetObjectItem(root, "recipe");
            if (rec_obj == NULL) rec_obj = scope;

            g30_recipe_t recipe;
            parse_json_recipe_object(rec_obj, &recipe);
            cJSON_Delete(root);

            uint8_t resolved_mac[6] = {0};
            esp_err_t disp_err = equipment_manager_dispatch_recipe_ex(target_slot,
                                                                      target_name[0] ? target_name : NULL,
                                                                      target_mac,
                                                                      target_type,
                                                                      req_id,
                                                                      &recipe,
                                                                      resolved_mac);
            if (disp_err != ESP_OK) {
                char err_frame[256];
                snprintf(err_frame, sizeof(err_frame),
                         "{\"event\":\"cmd_ack\",\"req_id\":\"%s\",\"action\":\"load_recipe\","
                         "\"status\":\"error\",\"error_code\":%d,\"message\":\"%s\"}",
                         req_id, (int)disp_err,
                         (disp_err == ESP_ERR_NOT_FOUND) ? "Target equipment offline" : "Recipe invalid or queue full");
                return send_ws_text_to_req(req, err_frame);
            }
            // Note: Final "success" or mid-transfer "error" cmd_ack is broadcast by BLE sequence completion callback
            return ESP_OK;
        } else {
            g30_command_t cmd;
            bool ok = parse_json_control_command(root, &cmd);
            cJSON_Delete(root);
            if (!ok) {
                char err_frame[192];
                snprintf(err_frame, sizeof(err_frame),
                         "{\"event\":\"cmd_ack\",\"req_id\":\"%s\",\"status\":\"rejected\",\"error_code\":400}",
                         req_id);
                return send_ws_text_to_req(req, err_frame);
            }

            uint8_t resolved_mac[6] = {0};
            esp_err_t disp_err = equipment_manager_dispatch_command(&cmd, resolved_mac);
            if (disp_err != ESP_OK) {
                char err_frame[224];
                snprintf(err_frame, sizeof(err_frame),
                         "{\"event\":\"cmd_ack\",\"req_id\":\"%s\",\"status\":\"error\",\"error_code\":%d}",
                         req_id, (int)disp_err);
                return send_ws_text_to_req(req, err_frame);
            }
            return ESP_OK;
        }
    } else {
        free(buf);
    }
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Existing Wi-Fi, MQTT, BLE, and Legacy G30 Handlers
// -----------------------------------------------------------------------------

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
    if (remaining >= (int)sizeof(buf)) {
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
    if (remaining >= (int)sizeof(buf)) {
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

// GET /api/ble/peers - Paired and connected BLE devices
static esp_err_t api_ble_peers_get_handler(httpd_req_t *req) {
    ble_peer_device_t live_peers[BLE_MAX_SIMULTANEOUS_CONNECTIONS];
    uint8_t live_count = 0;
    ble_manager_get_peers(live_peers, BLE_MAX_SIMULTANEOUS_CONNECTIONS, &live_count);

    cJSON *root = cJSON_CreateArray();
    for (uint8_t i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        equipment_slot_t slot;
        if (equipment_manager_get_slot(i, &slot) == ESP_OK && (slot.paired || slot.connected)) {
            int8_t disp_rssi = slot.rssi;
            if (slot.connected) {
                for (uint8_t p = 0; p < live_count; p++) {
                    if (live_peers[p].connected && memcmp(live_peers[p].addr, slot.mac, 6) == 0 && live_peers[p].rssi != 0) {
                        disp_rssi = live_peers[p].rssi;
                        break;
                    }
                }
            }

            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "slot", i + 1);
            cJSON_AddNumberToObject(item, "conn_handle", slot.conn_handle);

            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     slot.mac[0], slot.mac[1], slot.mac[2],
                     slot.mac[3], slot.mac[4], slot.mac[5]);
            const char *model_str = slot.model[0] ? slot.model : "BrewLink Equipment";
            const char *name_str = slot.name[0] ? slot.name : model_str;
            cJSON_AddStringToObject(item, "addr", mac_str);
            cJSON_AddStringToObject(item, "name", name_str);
            cJSON_AddStringToObject(item, "model", model_str);
            cJSON_AddNumberToObject(item, "rssi", disp_rssi);
            cJSON_AddStringToObject(item, "equipment_type", equipment_type_to_str(slot.equipment_type));
            cJSON_AddBoolToObject(item, "paired", (slot.paired || slot.connected));
            cJSON_AddBoolToObject(item, "connected", slot.connected);

            cJSON_AddItemToArray(root, item);
        }
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
    ble_manager_start_scan(5);
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
    if (remaining >= (int)sizeof(buf)) {
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
    if (remaining >= (int)sizeof(buf)) {
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

    if (addr_item && cJSON_IsString(addr_item)) {
        uint8_t mac[6];
        if (parse_mac_string(addr_item->valuestring, mac)) {
            equipment_manager_unpair_device(mac);
            ble_manager_disconnect_by_addr(mac);
        }
    }
    if (handle_item && cJSON_IsNumber(handle_item)) {
        uint16_t h = (uint16_t)handle_item->valueint;
        for (uint8_t i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
            equipment_slot_t slot;
            if (equipment_manager_get_slot(i, &slot) == ESP_OK && slot.connected && slot.conn_handle == h) {
                equipment_manager_unpair_device(slot.mac);
                break;
            }
        }
        ble_manager_disconnect(h);
    }

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"BLE disconnect initiated\"}", HTTPD_RESP_USE_STRLEN);
}

// GET /api/g30/status - Get connected G30 status & telemetry
static esp_err_t api_g30_status_get_handler(httpd_req_t *req) {
    g30_state_t state;
    bool connected = false;
    uint8_t mac[6] = {0};
    int8_t rssi = 0;

    esp_err_t err = g30_controller_get_state(&state, &connected, mac, &rssi);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", connected);

    if (connected) {
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(root, "mac", mac_str);
        cJSON_AddStringToObject(root, "model", "Grainfather G30 Connect");
        cJSON_AddNumberToObject(root, "rssi", rssi);

        cJSON *st = cJSON_AddObjectToObject(root, "state");
        cJSON_AddNumberToObject(st, "current_temp_c", (double)state.current_temp_c);
        cJSON_AddNumberToObject(st, "target_temp_c", (double)state.target_temp_c);
        cJSON_AddNumberToObject(st, "boil_temp_c", (double)state.boil_temp_c);
        cJSON_AddBoolToObject(st, "heat_active", state.heat_active);
        cJSON_AddNumberToObject(st, "heat_power_pct", state.heat_power_pct);
        cJSON_AddBoolToObject(st, "pump_active", state.pump_active);
        cJSON_AddBoolToObject(st, "auto_mode", state.auto_mode);
        cJSON_AddBoolToObject(st, "ramp_active", state.ramp_active);

        cJSON *tm = cJSON_AddObjectToObject(st, "timer");
        cJSON_AddBoolToObject(tm, "active", state.timer.active);
        cJSON_AddBoolToObject(tm, "paused", state.timer.paused);
        cJSON_AddNumberToObject(tm, "remaining_seconds", state.timer.remaining_seconds);
        cJSON_AddNumberToObject(tm, "total_seconds", state.timer.total_seconds);

        cJSON *sg = cJSON_AddObjectToObject(st, "stage");
        cJSON_AddNumberToObject(sg, "step_index", state.stage.step_index);
        cJSON_AddStringToObject(sg, "step_name", state.stage.step_name[0] ? state.stage.step_name : (state.auto_mode ? "Recipe Step" : "Manual"));

        cJSON *ia = cJSON_AddObjectToObject(st, "interaction");
        cJSON_AddBoolToObject(ia, "required", state.interaction.required);
        cJSON_AddNumberToObject(ia, "code", state.interaction.code);
        cJSON_AddStringToObject(ia, "message", state.interaction.message[0] ? state.interaction.message : g30_get_interaction_message(state.interaction.code));
    } else {
        cJSON_AddStringToObject(root, "message", "No Grainfather G30 currently connected");
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    esp_err_t ret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    return ret;
}

// POST /api/g30/control - Manual pump, heat, target temp controls
static esp_err_t api_g30_control_post_handler(httpd_req_t *req) {
    char buf[256];
    int remaining = req->content_len;
    if (remaining <= 0 || remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
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

    g30_command_t cmd;
    bool valid = parse_json_control_command(root, &cmd);
    cJSON_Delete(root);

    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid action or parameters");
        return ESP_FAIL;
    }

    esp_err_t err = g30_controller_dispatch_command(&cmd);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Grainfather G30 is not connected");
        return ESP_FAIL;
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"G30 controller is master during an active recipe; manual timers and temperature steps are disabled\"}", HTTPD_RESP_USE_STRLEN);
    } else if (err == ESP_ERR_NO_MEM) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Command queue full; please retry");
        return ESP_FAIL;
    } else if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to execute G30 command");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Command dispatched to Grainfather G30\"}", HTTPD_RESP_USE_STRLEN);
}

// POST /api/g30/recipe - Enqueue BeerXML converted recipe to Grainfather G30
static esp_err_t api_g30_recipe_post_handler(httpd_req_t *req) {
    return api_equipment_recipe_post_handler(req);
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
    config.max_uri_handlers = 32;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP & WebSocket server on port %d...", config.server_port);
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: 0x%x", err);
        return err;
    }

    // URI Handlers
    httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_register_uri_handler(s_server, &uri_root);

    httpd_uri_t uri_index = {.uri = "/index.html", .method = HTTP_GET, .handler = root_get_handler};
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

    // System & Network APIs
    httpd_uri_t uri_status = {.uri = "/api/status", .method = HTTP_GET, .handler = api_status_get_handler};
    httpd_register_uri_handler(s_server, &uri_status);

    httpd_uri_t uri_wifi_scan = {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = api_wifi_scan_get_handler};
    httpd_register_uri_handler(s_server, &uri_wifi_scan);

    httpd_uri_t uri_wifi_conn = {.uri = "/api/wifi/connect", .method = HTTP_POST, .handler = api_wifi_connect_post_handler};
    httpd_register_uri_handler(s_server, &uri_wifi_conn);

    httpd_uri_t uri_mqtt_get = {.uri = "/api/mqtt", .method = HTTP_GET, .handler = api_mqtt_get_handler};
    httpd_register_uri_handler(s_server, &uri_mqtt_get);

    httpd_uri_t uri_mqtt_post = {.uri = "/api/mqtt", .method = HTTP_POST, .handler = api_mqtt_post_handler};
    httpd_register_uri_handler(s_server, &uri_mqtt_post);

    httpd_uri_t uri_ble_peers = {.uri = "/api/ble/peers", .method = HTTP_GET, .handler = api_ble_peers_get_handler};
    httpd_register_uri_handler(s_server, &uri_ble_peers);

    httpd_uri_t uri_ble_scan = {.uri = "/api/ble/scan", .method = HTTP_GET, .handler = api_ble_scan_get_handler};
    httpd_register_uri_handler(s_server, &uri_ble_scan);

    httpd_uri_t uri_ble_conn = {.uri = "/api/ble/connect", .method = HTTP_POST, .handler = api_ble_connect_post_handler};
    httpd_register_uri_handler(s_server, &uri_ble_conn);

    httpd_uri_t uri_ble_disconn = {.uri = "/api/ble/disconnect", .method = HTTP_POST, .handler = api_ble_disconnect_post_handler};
    httpd_register_uri_handler(s_server, &uri_ble_disconn);

    httpd_uri_t uri_sys_restart = {.uri = "/api/system/restart", .method = HTTP_POST, .handler = api_system_restart_post_handler};
    httpd_register_uri_handler(s_server, &uri_sys_restart);

    // Pairing Endpoints
    httpd_uri_t uri_pair_status = {.uri = "/api/pair/status", .method = HTTP_GET, .handler = api_pair_status_get_handler};
    httpd_register_uri_handler(s_server, &uri_pair_status);

    httpd_uri_t uri_pair_start = {.uri = "/api/pair/start", .method = HTTP_POST, .handler = api_pair_start_post_handler};
    httpd_register_uri_handler(s_server, &uri_pair_start);

    httpd_uri_t uri_pair_claim = {.uri = "/api/pair/claim", .method = HTTP_POST, .handler = api_pair_claim_post_handler};
    httpd_register_uri_handler(s_server, &uri_pair_claim);

    httpd_uri_t uri_pair_delete = {.uri = "/api/pair/delete", .method = HTTP_POST, .handler = api_pair_delete_post_handler};
    httpd_register_uri_handler(s_server, &uri_pair_delete);

    // Equipment-Agnostic Endpoints
    httpd_uri_t uri_eq_list = {.uri = "/api/equipment", .method = HTTP_GET, .handler = api_equipment_get_handler};
    httpd_register_uri_handler(s_server, &uri_eq_list);

    httpd_uri_t uri_eq_recipe = {.uri = "/api/equipment/recipe", .method = HTTP_POST, .handler = api_equipment_recipe_post_handler};
    httpd_register_uri_handler(s_server, &uri_eq_recipe);

    httpd_uri_t uri_eq_control = {.uri = "/api/equipment/control", .method = HTTP_POST, .handler = api_equipment_control_post_handler};
    httpd_register_uri_handler(s_server, &uri_eq_control);

    // Full-Duplex Two-Way WebSocket Endpoint (/ws)
    httpd_uri_t uri_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(s_server, &uri_ws);

    // Legacy Grainfather G30 Endpoints (Backward Compatibility)
    httpd_uri_t uri_g30_status = {.uri = "/api/g30/status", .method = HTTP_GET, .handler = api_g30_status_get_handler};
    httpd_register_uri_handler(s_server, &uri_g30_status);

    httpd_uri_t uri_g30_control = {.uri = "/api/g30/control", .method = HTTP_POST, .handler = api_g30_control_post_handler};
    httpd_register_uri_handler(s_server, &uri_g30_control);

    httpd_uri_t uri_g30_recipe = {.uri = "/api/g30/recipe", .method = HTTP_POST, .handler = api_g30_recipe_post_handler};
    httpd_register_uri_handler(s_server, &uri_g30_recipe);

    ESP_LOGI(TAG, "HTTP & WebSocket server registered all REST and /ws endpoints");
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
