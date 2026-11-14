/**
 * @file equipment_manager.c
 * @brief Multi-Equipment Driver Registry and Unified State Dispatcher Implementation
 * @details Supports simultaneous Boilers (Grainfather G30), Fermenters, and Coolers/Chillers
 *          with zero dynamic heap allocation and strict fail-fast offline handling.
 */

#include "equipment_manager.h"
#include "grainfather_g30.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
static const char *TAG = "EQ_MGR";
static SemaphoreHandle_t s_eq_mutex = NULL;
#endif

static QueueHandle_t           s_ble_cmd_queue = NULL;
static eq_enqueue_ble_pkt_fn_t s_enqueue_override = NULL;
static equipment_slot_t        s_slots[BLE_MAX_SIMULTANEOUS_CONNECTIONS];

static void eq_lock(void) {
#ifdef ESP_PLATFORM
    if (s_eq_mutex != NULL) {
        xSemaphoreTake(s_eq_mutex, pdMS_TO_TICKS(200));
    }
#endif
}

static void eq_unlock(void) {
#ifdef ESP_PLATFORM
    if (s_eq_mutex != NULL) {
        xSemaphoreGive(s_eq_mutex);
    }
#endif
}

static bool is_mac_non_zero(const uint8_t mac[6]) {
    if (mac == NULL) return false;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) return true;
    }
    return false;
}

static bool str_eq_nocase(const char *a, const char *b) {
    if (a == NULL || b == NULL) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static bool is_slot_name_taken(int exclude_slot_idx, const char *candidate) {
    if (candidate == NULL || candidate[0] == '\0') return false;
    for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        if (i == exclude_slot_idx) continue;
        if ((s_slots[i].paired || s_slots[i].connected) &&
            s_slots[i].name[0] != '\0' &&
            str_eq_nocase(s_slots[i].name, candidate)) {
            return true;
        }
    }
    return false;
}

static void assign_unique_slot_name(int slot_idx,
                                    const uint8_t mac[6],
                                    const char *preferred_name,
                                    const char *model,
                                    char out_name[32]) {
    const char *base = "BrewLink Equipment";
    if (preferred_name != NULL && preferred_name[0] != '\0') {
        base = preferred_name;
    } else if (model != NULL && model[0] != '\0') {
        base = model;
    }

    char candidate[32] = {0};
    strncpy(candidate, base, sizeof(candidate) - 1);
    candidate[sizeof(candidate) - 1] = '\0';

    if (!is_slot_name_taken(slot_idx, candidate)) {
        strncpy(out_name, candidate, 31);
        out_name[31] = '\0';
        return;
    }

    // Disambiguate using last 2 bytes of MAC address (e.g. "Grainfather G30 Connect-8B22")
    if (mac != NULL) {
        snprintf(candidate, sizeof(candidate), "%.25s-%02X%02X", base, mac[4], mac[5]);
        if (!is_slot_name_taken(slot_idx, candidate)) {
            strncpy(out_name, candidate, 31);
            out_name[31] = '\0';
            return;
        }
    }

    // Fallback disambiguation using 1-based slot number
    unsigned int slot_num = (unsigned int)(((slot_idx >= 0 && slot_idx < 8) ? slot_idx : 0) + 1);
    snprintf(candidate, sizeof(candidate), "%.26s #%u", base, slot_num);
    strncpy(out_name, candidate, 31);
    out_name[31] = '\0';
}

static const char *action_enum_to_str(g30_command_action_t action) {
    switch (action) {
        case G30_CMD_ACTION_SET_PUMP:             return "set_pump";
        case G30_CMD_ACTION_SET_HEAT:             return "set_heat";
        case G30_CMD_ACTION_SET_TARGET_TEMP:      return "set_target_temp";
        case G30_CMD_ACTION_SET_TIMER:            return "set_timer";
        case G30_CMD_ACTION_PAUSE_TIMER:          return "pause_timer";
        case G30_CMD_ACTION_RESUME_TIMER:         return "resume_timer";
        case G30_CMD_ACTION_CANCEL_TIMER:         return "cancel_timer";
        case G30_CMD_ACTION_ACK_INTERACTION:      return "ack_interaction";
        case G30_CMD_ACTION_DISMISS_ALERT:        return "dismiss_alert";
        case G30_CMD_ACTION_LOAD_RECIPE:          return "load_recipe";
        case G30_CMD_ACTION_CANCEL_SESSION:       return "cancel_session";
        case G30_CMD_ACTION_PRESS_SET:            return "press_set";
        case G30_CMD_ACTION_DISMISS_SPARGE_ALERT: return "dismiss_sparge_alert";
        case G30_CMD_ACTION_TOGGLE_TIMER:         return "toggle_timer";
        case G30_CMD_ACTION_TEMP_UP:              return "temp_up";
        case G30_CMD_ACTION_TEMP_DOWN:            return "temp_down";
        case G30_CMD_ACTION_DELAYED_HEAT:         return "delayed_heat";
        case G30_CMD_ACTION_DISCONNECT_MANUAL:    return "disconnect_manual";
        case G30_CMD_ACTION_DISCONNECT_AUTO:      return "disconnect_auto";
        default:                              return "cmd";
    }
}

static esp_err_t default_enqueue_ble_pkt(const ble_outbound_cmd_packet_t *pkt) {
    if (pkt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_enqueue_override != NULL) {
        return s_enqueue_override(pkt);
    }
#ifdef ESP_PLATFORM
    if (s_ble_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_ble_cmd_queue, pkt, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "s_ble_cmd_queue full; packet dropped");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

// =============================================================================
// Driver 1: Grainfather G30 Connect Driver (EQUIPMENT_TYPE_BOILER)
// =============================================================================
static void g30_drv_init_state(equipment_slot_t *slot) {
    memset(&slot->state.boiler, 0, sizeof(g30_state_t));
    memset(&slot->prev_state.boiler, 0, sizeof(g30_state_t));
    slot->state.boiler.boil_temp_c = 100.0f;
    slot->prev_state.boiler.boil_temp_c = 100.0f;
    strncpy(slot->state.boiler.stage.step_name, "Manual Mode", sizeof(slot->state.boiler.stage.step_name) - 1);
    strncpy(slot->state.boiler.interaction.message, "Normal operation", sizeof(slot->state.boiler.interaction.message) - 1);
}

static esp_err_t g30_drv_parse_notification(equipment_slot_t *slot, const uint8_t *data, size_t len) {
    g30_notification_type_t notif_type = G30_NOTIF_UNKNOWN;
    return g30_parse_notification(data, len, &slot->state.boiler, &notif_type);
}

static esp_err_t g30_drv_dispatch_command(equipment_slot_t *slot,
                                          const g30_command_t *cmd,
                                          eq_enqueue_ble_pkt_fn_t enqueue_fn) {
    // Master Controller Rule: when a recipe is ongoing (auto_mode == true), G30 hardware is master.
    if (slot->state.boiler.auto_mode &&
        (cmd->action == G30_CMD_ACTION_SET_TIMER ||
         cmd->action == G30_CMD_ACTION_CANCEL_TIMER ||
         cmd->action == G30_CMD_ACTION_SET_TARGET_TEMP ||
         cmd->action == G30_CMD_ACTION_TEMP_UP ||
         cmd->action == G30_CMD_ACTION_TEMP_DOWN)) {
        return ESP_ERR_INVALID_STATE;
    }

    // The G30 only has a timer pause/resume TOGGLE (G); make pause/resume explicit.
    if ((cmd->action == G30_CMD_ACTION_PAUSE_TIMER && slot->state.boiler.timer.paused) ||
        (cmd->action == G30_CMD_ACTION_RESUME_TIMER && !slot->state.boiler.timer.paused)) {
        return ESP_ERR_INVALID_STATE;
    }

    g30_command_t effective_cmd = *cmd;
    if (effective_cmd.action == G30_CMD_ACTION_ACK_INTERACTION &&
        slot->state.boiler.interaction.code == 5) {
        effective_cmd.action = G30_CMD_ACTION_DISMISS_ALERT;
    }

    ble_outbound_cmd_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    memcpy(pkt.mac, slot->mac, 6);
    pkt.conn_handle = slot->conn_handle;
    pkt.len = G30_COMMAND_LEN;
    pkt.delay_after_ms = G30_WRITE_PACING_MS;
    strncpy(pkt.req_id, cmd->req_id[0] ? cmd->req_id : "cmd", sizeof(pkt.req_id) - 1);
    strncpy(pkt.action, action_enum_to_str(cmd->action), sizeof(pkt.action) - 1);
    pkt.seq_idx = 1;
    pkt.seq_total = 1;

    esp_err_t err = g30_encode_command(&effective_cmd, pkt.cmd_bytes);
    if (err != ESP_OK) {
        return err;
    }
    return enqueue_fn(&pkt);
}

static esp_err_t g30_drv_dispatch_recipe(equipment_slot_t *slot,
                                         const char *req_id,
                                         const g30_recipe_t *recipe,
                                         eq_enqueue_ble_pkt_fn_t enqueue_fn) {
    if (recipe == NULL || recipe->mash_step_count == 0 || recipe->mash_step_count > MAX_MASH_STEPS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (recipe->boil_addition_count > MAX_BOIL_ADDITIONS) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t total_pkts = (uint8_t)(4 + recipe->boil_addition_count + recipe->mash_step_count);
    uint8_t cur_seq = 1;

    ble_outbound_cmd_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    memcpy(pkt.mac, slot->mac, 6);
    pkt.conn_handle = slot->conn_handle;
    pkt.len = G30_COMMAND_LEN;
    pkt.delay_after_ms = G30_WRITE_PACING_MS;
    strncpy(pkt.req_id, (req_id && req_id[0]) ? req_id : "recipe", sizeof(pkt.req_id) - 1);
    strncpy(pkt.action, "load_recipe", sizeof(pkt.action) - 1);
    pkt.seq_total = total_pkts;

    // Line 1: Main Recipe Header (R{boil_min},{mash_steps},{mash_water},{sparge_water},)
    pkt.seq_idx = cur_seq++;
    esp_err_t err = g30_encode_recipe_header(recipe, pkt.cmd_bytes);
    if (err != ESP_OK) return err;
    err = enqueue_fn(&pkt);
    if (err != ESP_OK) return err;

    // Line 2: Recipe Configuration Flags (0,1,1,0,0,)
    pkt.seq_idx = cur_seq++;
    err = g30_encode_recipe_flags(pkt.cmd_bytes);
    if (err != ESP_OK) return err;
    err = enqueue_fn(&pkt);
    if (err != ESP_OK) return err;

    // Line 3: Recipe Name (sanitized uppercase ASCII, space padded)
    pkt.seq_idx = cur_seq++;
    err = g30_encode_recipe_name(recipe->name, pkt.cmd_bytes);
    if (err != ESP_OK) return err;
    err = enqueue_fn(&pkt);
    if (err != ESP_OK) return err;

    // Line 4: Boil Parameters ({hop_stand_min},{boil_addition_count},0,0,)
    pkt.seq_idx = cur_seq++;
    err = g30_encode_recipe_boil_params(recipe->hop_stand_min, recipe->boil_addition_count, pkt.cmd_bytes);
    if (err != ESP_OK) return err;
    err = enqueue_fn(&pkt);
    if (err != ESP_OK) return err;

    // Line 5..4+N: Boil Stops in descending order ({time_left_min},)
    for (int i = 0; i < recipe->boil_addition_count; i++) {
        pkt.seq_idx = cur_seq++;
        err = g30_encode_recipe_boil_stop(recipe->boil_additions_min[i], pkt.cmd_bytes);
        if (err != ESP_OK) return err;
        err = enqueue_fn(&pkt);
        if (err != ESP_OK) return err;
    }

    // Line 4+N+1..: Mash Steps in sequence ({temp_c}:{duration_min})
    for (int i = 0; i < recipe->mash_step_count; i++) {
        pkt.seq_idx = cur_seq++;
        err = g30_encode_recipe_mash_step(&recipe->mash_steps[i], pkt.cmd_bytes);
        if (err != ESP_OK) return err;
        err = enqueue_fn(&pkt);
        if (err != ESP_OK) return err;
    }

    // Update cached state to reflect initial strike water prompt
    slot->state.boiler.auto_mode = true;
    slot->state.boiler.stage.step_index = 1;
    slot->state.boiler.heat_active = false;
    slot->state.boiler.ramp_active = false;
    slot->state.boiler.timer.active = false;
    slot->state.boiler.timer.paused = false;
    slot->state.boiler.timer.remaining_seconds = 0;
    slot->state.boiler.timer.total_seconds = 0;
    slot->state.boiler.target_temp_c = recipe->mash_steps[0].temp_c;
    slot->state.boiler.interaction.required = true;
    slot->state.boiler.interaction.code = 1;
    if (recipe->mash_water_l > 0.0f) {
        snprintf(slot->state.boiler.interaction.message, sizeof(slot->state.boiler.interaction.message),
                 "Add %.1fL water & Press SET to start heating", recipe->mash_water_l);
    } else {
        strncpy(slot->state.boiler.interaction.message, "Add mash water & Press SET to start heating",
                sizeof(slot->state.boiler.interaction.message) - 1);
    }
    slot->state.boiler.interaction.message[sizeof(slot->state.boiler.interaction.message) - 1] = '\0';
    strncpy(slot->state.boiler.stage.step_name, "Add Strike Water", sizeof(slot->state.boiler.stage.step_name) - 1);
    slot->state.boiler.stage.step_name[sizeof(slot->state.boiler.stage.step_name) - 1] = '\0';

    return ESP_OK;
}

static esp_err_t g30_drv_format_telemetry_json(const equipment_slot_t *slot,
                                               uint32_t timestamp_ms,
                                               char *out_json,
                                               size_t max_len) {
    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
             slot->mac[0], slot->mac[1], slot->mac[2],
             slot->mac[3], slot->mac[4], slot->mac[5]);

    const g30_state_t *st = &slot->state.boiler;
    const char *model_str = slot->model[0] ? slot->model : "Grainfather G30";
    const char *name_str = slot->name[0] ? slot->name : model_str;
    snprintf(out_json, max_len,
             "{\"event\":\"telemetry\",\"mac\":\"%s\",\"name\":\"%s\",\"equipment_type\":\"boiler\",\"model\":\"%s\","
             "\"rssi\":%d,\"state\":{"
             "\"temperature\":{\"current_c\":%.1f,\"target_c\":%.1f,\"boil_c\":%.1f},"
             "\"heating\":{\"active\":%s,\"power_pct\":%u,\"ramping\":%s},"
             "\"pump\":{\"active\":%s},"
             "\"timer\":{\"active\":%s,\"paused\":%s,\"remaining_seconds\":%lu,\"total_seconds\":%lu},"
             "\"stage\":{\"step_index\":%u,\"step_name\":\"%s\",\"auto_mode\":%s},"
             "\"interaction\":{\"required\":%s,\"code\":%u,\"message\":\"%s\"},\"flags\":{\"delayed_heat\":%s,\"interrupted\":%s,\"manual_power\":%s,\"sparge_alert\":%s},\"error_code\":%u"
             "},\"timestamp_ms\":%lu}",
             mac_str, name_str, model_str,
             slot->rssi,
             st->current_temp_c, st->target_temp_c, st->boil_temp_c,
             st->heat_active ? "true" : "false",
             st->heat_power_pct,
             st->ramp_active ? "true" : "false",
             st->pump_active ? "true" : "false",
             st->timer.active ? "true" : "false",
             st->timer.paused ? "true" : "false",
             (unsigned long)st->timer.remaining_seconds,
             (unsigned long)st->timer.total_seconds,
             st->stage.step_index,
             st->stage.step_name[0] ? st->stage.step_name : "Idle",
             st->auto_mode ? "true" : "false",
             st->interaction.required ? "true" : "false",
             st->interaction.code,
             st->interaction.message,
             st->delayed_heat ? "true" : "false",
             st->interrupted ? "true" : "false",
             st->manual_power ? "true" : "false",
             st->sparge_alert ? "true" : "false",
             st->error_code,
             (unsigned long)timestamp_ms);
    return ESP_OK;
}

// =============================================================================
// Driver 2: Extensible Fermenter Driver (EQUIPMENT_TYPE_FERMENTER)
// =============================================================================
static void ferm_drv_init_state(equipment_slot_t *slot) {
    memset(&slot->state.fermenter, 0, sizeof(placeholder_fermenter_state_t));
    memset(&slot->prev_state.fermenter, 0, sizeof(placeholder_fermenter_state_t));
    slot->state.fermenter.target_temp_c = 20.0f;
    slot->state.fermenter.gravity_sg = 1.050f;
}

static esp_err_t ferm_drv_parse_notification(equipment_slot_t *slot, const uint8_t *data, size_t len) {
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    char buf[64];
    size_t cpy = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, data, cpy);
    buf[cpy] = '\0';

    float beer_t = 0.0f, sg = 0.0f;
    if (sscanf(buf, "F%f,%f", &beer_t, &sg) == 2) {
        slot->state.fermenter.beer_temp_c = beer_t;
        slot->state.fermenter.gravity_sg = sg;
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t ferm_drv_dispatch_command(equipment_slot_t *slot,
                                           const g30_command_t *cmd,
                                           eq_enqueue_ble_pkt_fn_t enqueue_fn) {
    if (cmd->action == G30_CMD_ACTION_SET_TARGET_TEMP) {
        slot->state.fermenter.target_temp_c = cmd->params.target_temp_c;
        ble_outbound_cmd_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        memcpy(pkt.mac, slot->mac, 6);
        pkt.conn_handle = slot->conn_handle;
        snprintf(pkt.cmd_bytes, sizeof(pkt.cmd_bytes), "$%.1f,", cmd->params.target_temp_c);
        pkt.len = (uint8_t)strlen(pkt.cmd_bytes);
        pkt.delay_after_ms = 100;
        strncpy(pkt.req_id, cmd->req_id[0] ? cmd->req_id : "cmd", sizeof(pkt.req_id) - 1);
        strncpy(pkt.action, "set_target_temp", sizeof(pkt.action) - 1);
        pkt.seq_idx = 1;
        pkt.seq_total = 1;
        return enqueue_fn(&pkt);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t ferm_drv_dispatch_recipe(equipment_slot_t *slot,
                                          const char *req_id,
                                          const g30_recipe_t *recipe,
                                          eq_enqueue_ble_pkt_fn_t enqueue_fn) {
    if (recipe == NULL || recipe->fermentation_step_count == 0 ||
        recipe->fermentation_step_count > MAX_FERMENTATION_STEPS) {
        return ESP_ERR_INVALID_ARG;
    }

    slot->state.fermenter.target_temp_c = recipe->fermentation_steps[0].temp_c;
    if (recipe->target_og_sg > 1.0f) {
        slot->state.fermenter.gravity_sg = recipe->target_og_sg;
    }

    ble_outbound_cmd_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    memcpy(pkt.mac, slot->mac, 6);
    pkt.conn_handle = slot->conn_handle;
    pkt.delay_after_ms = 100;
    strncpy(pkt.req_id, (req_id && req_id[0]) ? req_id : "ferm-rx", sizeof(pkt.req_id) - 1);
    strncpy(pkt.action, "load_recipe", sizeof(pkt.action) - 1);
    pkt.seq_total = recipe->fermentation_step_count;

    for (uint8_t i = 0; i < recipe->fermentation_step_count; i++) {
        pkt.seq_idx = (uint8_t)(i + 1);
        snprintf(pkt.cmd_bytes, sizeof(pkt.cmd_bytes), "F%u:%.1f:%u",
                 (unsigned int)(i + 1),
                 recipe->fermentation_steps[i].temp_c,
                 (unsigned int)recipe->fermentation_steps[i].duration_hours);
        pkt.len = (uint8_t)strlen(pkt.cmd_bytes);
        esp_err_t err = enqueue_fn(&pkt);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t ferm_drv_format_telemetry_json(const equipment_slot_t *slot,
                                                uint32_t timestamp_ms,
                                                char *out_json,
                                                size_t max_len) {
    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
             slot->mac[0], slot->mac[1], slot->mac[2],
             slot->mac[3], slot->mac[4], slot->mac[5]);

    const placeholder_fermenter_state_t *st = &slot->state.fermenter;
    const char *model_str = slot->model[0] ? slot->model : "BrewLink Fermenter";
    const char *name_str = slot->name[0] ? slot->name : model_str;
    snprintf(out_json, max_len,
             "{\"event\":\"telemetry\",\"mac\":\"%s\",\"name\":\"%s\",\"equipment_type\":\"fermenter\",\"model\":\"%s\","
             "\"rssi\":%d,\"state\":{"
             "\"beer_temp_c\":%.1f,\"chamber_temp_c\":%.1f,\"target_temp_c\":%.1f,"
             "\"gravity_sg\":%.3f,\"cooling_active\":%s,\"heating_active\":%s"
             "},\"timestamp_ms\":%lu}",
             mac_str, name_str, model_str,
             slot->rssi,
             st->beer_temp_c, st->chamber_temp_c, st->target_temp_c,
             st->gravity_sg,
             st->cooling_active ? "true" : "false",
             st->heating_active ? "true" : "false",
             (unsigned long)timestamp_ms);
    return ESP_OK;
}

// =============================================================================
// Driver 3: Extensible Cooler / Heater Driver (EQUIPMENT_TYPE_COOLER_HEATER)
// =============================================================================
static void cooler_drv_init_state(equipment_slot_t *slot) {
    memset(&slot->state.cooler, 0, sizeof(placeholder_cooler_state_t));
    memset(&slot->prev_state.cooler, 0, sizeof(placeholder_cooler_state_t));
    slot->state.cooler.active_zones_count = 1;
    slot->state.cooler.zones[0].target_temp_c = 4.0f;
}

static esp_err_t cooler_drv_parse_notification(equipment_slot_t *slot, const uint8_t *data, size_t len) {
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    char buf[64];
    size_t cpy = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, data, cpy);
    buf[cpy] = '\0';

    float cur_t = 0.0f, targ_t = 0.0f;
    if (sscanf(buf, "K%f,%f", &cur_t, &targ_t) == 2) {
        slot->state.cooler.zones[0].current_temp_c = cur_t;
        slot->state.cooler.zones[0].target_temp_c = targ_t;
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t cooler_drv_dispatch_command(equipment_slot_t *slot,
                                             const g30_command_t *cmd,
                                             eq_enqueue_ble_pkt_fn_t enqueue_fn) {
    if (cmd->action == G30_CMD_ACTION_SET_TARGET_TEMP) {
        slot->state.cooler.zones[0].target_temp_c = cmd->params.target_temp_c;
        ble_outbound_cmd_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        memcpy(pkt.mac, slot->mac, 6);
        pkt.conn_handle = slot->conn_handle;
        snprintf(pkt.cmd_bytes, sizeof(pkt.cmd_bytes), "$%.1f,", cmd->params.target_temp_c);
        pkt.len = (uint8_t)strlen(pkt.cmd_bytes);
        pkt.delay_after_ms = 100;
        strncpy(pkt.req_id, cmd->req_id[0] ? cmd->req_id : "cmd", sizeof(pkt.req_id) - 1);
        strncpy(pkt.action, "set_target_temp", sizeof(pkt.action) - 1);
        pkt.seq_idx = 1;
        pkt.seq_total = 1;
        return enqueue_fn(&pkt);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t cooler_drv_dispatch_recipe(equipment_slot_t *slot,
                                            const char *req_id,
                                            const g30_recipe_t *recipe,
                                            eq_enqueue_ble_pkt_fn_t enqueue_fn) {
    if (recipe == NULL || recipe->fermentation_step_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    slot->state.cooler.zones[0].target_temp_c = recipe->fermentation_steps[0].temp_c;
    ble_outbound_cmd_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    memcpy(pkt.mac, slot->mac, 6);
    pkt.conn_handle = slot->conn_handle;
    snprintf(pkt.cmd_bytes, sizeof(pkt.cmd_bytes), "$%.1f,", recipe->fermentation_steps[0].temp_c);
    pkt.len = (uint8_t)strlen(pkt.cmd_bytes);
    pkt.delay_after_ms = 100;
    strncpy(pkt.req_id, (req_id && req_id[0]) ? req_id : "cool-rx", sizeof(pkt.req_id) - 1);
    strncpy(pkt.action, "load_recipe", sizeof(pkt.action) - 1);
    pkt.seq_idx = 1;
    pkt.seq_total = 1;
    return enqueue_fn(&pkt);
}

static esp_err_t cooler_drv_format_telemetry_json(const equipment_slot_t *slot,
                                                  uint32_t timestamp_ms,
                                                  char *out_json,
                                                  size_t max_len) {
    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
             slot->mac[0], slot->mac[1], slot->mac[2],
             slot->mac[3], slot->mac[4], slot->mac[5]);

    const placeholder_cooler_state_t *st = &slot->state.cooler;
    const char *model_str = slot->model[0] ? slot->model : "BrewLink Chiller";
    const char *name_str = slot->name[0] ? slot->name : model_str;
    snprintf(out_json, max_len,
             "{\"event\":\"telemetry\",\"mac\":\"%s\",\"name\":\"%s\",\"equipment_type\":\"cooler\",\"model\":\"%s\","
             "\"rssi\":%d,\"state\":{"
             "\"reservoir_temp_c\":%.1f,\"current_temp_c\":%.1f,\"target_temp_c\":%.1f"
             "},\"timestamp_ms\":%lu}",
             mac_str, name_str, model_str,
             slot->rssi,
             st->reservoir_temp_c,
             st->zones[0].current_temp_c,
             st->zones[0].target_temp_c,
             (unsigned long)timestamp_ms);
    return ESP_OK;
}

// =============================================================================
// Authoritative Driver Table
// =============================================================================
static const equipment_driver_t s_drivers[] = {
    {
        .driver_name           = "Grainfather G30 Connect",
        .equipment_type        = EQUIPMENT_TYPE_BOILER,
        .capabilities_mask     = EQ_CAP_TEMP_READ | EQ_CAP_TEMP_SET | EQ_CAP_PUMP_CONTROL |
                                 EQ_CAP_HEAT_CONTROL | EQ_CAP_POWER_PCT | EQ_CAP_TIMER_CONTROL |
                                 EQ_CAP_MASH_RECIPE | EQ_CAP_INTERACTION_PROMPT,
        .init_state            = g30_drv_init_state,
        .parse_notification    = g30_drv_parse_notification,
        .dispatch_command      = g30_drv_dispatch_command,
        .dispatch_recipe       = g30_drv_dispatch_recipe,
        .format_telemetry_json = g30_drv_format_telemetry_json
    },
    {
        .driver_name           = "Generic BLE Fermenter",
        .equipment_type        = EQUIPMENT_TYPE_FERMENTER,
        .capabilities_mask     = EQ_CAP_TEMP_READ | EQ_CAP_TEMP_SET | EQ_CAP_FERM_RECIPE,
        .init_state            = ferm_drv_init_state,
        .parse_notification    = ferm_drv_parse_notification,
        .dispatch_command      = ferm_drv_dispatch_command,
        .dispatch_recipe       = ferm_drv_dispatch_recipe,
        .format_telemetry_json = ferm_drv_format_telemetry_json
    },
    {
        .driver_name           = "Generic BLE Glycol Chiller",
        .equipment_type        = EQUIPMENT_TYPE_COOLER_HEATER,
        .capabilities_mask     = EQ_CAP_TEMP_READ | EQ_CAP_TEMP_SET | EQ_CAP_FERM_RECIPE,
        .init_state            = cooler_drv_init_state,
        .parse_notification    = cooler_drv_parse_notification,
        .dispatch_command      = cooler_drv_dispatch_command,
        .dispatch_recipe       = cooler_drv_dispatch_recipe,
        .format_telemetry_json = cooler_drv_format_telemetry_json
    }
};

static const equipment_driver_t *resolve_driver(equipment_type_t type) {
    for (size_t i = 0; i < sizeof(s_drivers) / sizeof(s_drivers[0]); i++) {
        if (s_drivers[i].equipment_type == type) {
            return &s_drivers[i];
        }
    }
    return &s_drivers[0]; // Default to Boiler driver
}

static int find_slot_index_ex(uint8_t target_slot,
                              const char *target_name,
                              const uint8_t target_mac[6],
                              equipment_type_t target_type) {
    if (target_slot >= 1 && target_slot <= BLE_MAX_SIMULTANEOUS_CONNECTIONS) {
        int s_idx = (int)(target_slot - 1);
        return s_slots[s_idx].connected ? s_idx : -1;
    }

    bool has_mac = is_mac_non_zero(target_mac);
    if (has_mac) {
        for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
            if (s_slots[i].connected && memcmp(s_slots[i].mac, target_mac, 6) == 0) {
                return i;
            }
        }
        return -1;
    }

    if (target_name != NULL && target_name[0] != '\0') {
        for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
            if (s_slots[i].connected && str_eq_nocase(s_slots[i].name, target_name)) {
                return i;
            }
        }
        return -1;
    }

    for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        if (s_slots[i].connected &&
            (target_type == EQUIPMENT_TYPE_UNKNOWN || s_slots[i].equipment_type == target_type)) {
            return i;
        }
    }
    return -1;
}

static int find_slot_index(const uint8_t target_mac[6], equipment_type_t target_type) {
    return find_slot_index_ex(0, NULL, target_mac, target_type);
}

// =============================================================================
// Public API Implementation
// =============================================================================
esp_err_t equipment_manager_init(QueueHandle_t ble_cmd_queue) {
    s_ble_cmd_queue = ble_cmd_queue;
#ifdef ESP_PLATFORM
    if (s_eq_mutex == NULL) {
        s_eq_mutex = xSemaphoreCreateMutex();
        if (s_eq_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
#endif
    memset(s_slots, 0, sizeof(s_slots));
    return ESP_OK;
}

void equipment_manager_set_enqueue_override(eq_enqueue_ble_pkt_fn_t fn) {
    s_enqueue_override = fn;
}

esp_err_t equipment_manager_register_paired_device(const uint8_t mac[6],
                                                   equipment_type_t type,
                                                   const char *preferred_name,
                                                   const char *model,
                                                   uint8_t *out_slot_idx) {
    if (!is_mac_non_zero(mac)) {
        return ESP_ERR_INVALID_ARG;
    }

    eq_lock();
    int slot_idx = -1;
    for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        if ((s_slots[i].paired || s_slots[i].connected) && memcmp(s_slots[i].mac, mac, 6) == 0) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx < 0) {
        for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
            if (!s_slots[i].paired && !s_slots[i].connected) {
                slot_idx = i;
                break;
            }
        }
    }
    if (slot_idx < 0) {
        eq_unlock();
        return ESP_ERR_NO_MEM;
    }

    equipment_slot_t *slot = &s_slots[slot_idx];
    bool was_connected = slot->connected;
    if (!was_connected) {
        memset(slot, 0, sizeof(equipment_slot_t));
    }
    slot->paired = true;
    memcpy(slot->mac, mac, 6);
    slot->equipment_type = (type != EQUIPMENT_TYPE_UNKNOWN) ? type : EQUIPMENT_TYPE_BOILER;
    slot->driver = resolve_driver(slot->equipment_type);
    if (model != NULL && model[0] != '\0') {
        strncpy(slot->model, model, sizeof(slot->model) - 1);
    } else if (slot->model[0] == '\0') {
        strncpy(slot->model, slot->driver->driver_name, sizeof(slot->model) - 1);
    }

    char unique_name[32] = {0};
    assign_unique_slot_name(slot_idx, mac, preferred_name, slot->model, unique_name);
    strncpy(slot->name, unique_name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';

    if (!was_connected && slot->driver && slot->driver->init_state) {
        slot->driver->init_state(slot);
    }
    if (out_slot_idx != NULL) {
        *out_slot_idx = (uint8_t)slot_idx;
    }
    eq_unlock();
    return ESP_OK;
}

esp_err_t equipment_manager_unpair_device(const uint8_t mac[6]) {
    if (!is_mac_non_zero(mac)) {
        return ESP_ERR_INVALID_ARG;
    }

    eq_lock();
    for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        if ((s_slots[i].paired || s_slots[i].connected) && memcmp(s_slots[i].mac, mac, 6) == 0) {
            memset(&s_slots[i], 0, sizeof(equipment_slot_t));
            eq_unlock();
            return ESP_OK;
        }
    }
    eq_unlock();
    return ESP_ERR_NOT_FOUND;
}

esp_err_t equipment_manager_on_peer_event(const uint8_t mac[6],
                                          bool connected,
                                          equipment_type_t type,
                                          const char *model,
                                          uint16_t conn_handle,
                                          int8_t rssi) {
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    eq_lock();
    if (connected) {
        int slot_idx = -1;
        // 1. Check if this MAC already occupies a paired or connected slot
        for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
            if ((s_slots[i].paired || s_slots[i].connected) && memcmp(s_slots[i].mac, mac, 6) == 0) {
                slot_idx = i;
                break;
            }
        }
        // 2. Otherwise pick a completely free slot (!paired && !connected)
        if (slot_idx < 0) {
            for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
                if (!s_slots[i].connected && !s_slots[i].paired) {
                    slot_idx = i;
                    break;
                }
            }
        }
        // 3. Fallback: pick any disconnected slot
        if (slot_idx < 0) {
            for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
                if (!s_slots[i].connected) {
                    slot_idx = i;
                    break;
                }
            }
        }
        if (slot_idx < 0) {
            eq_unlock();
            return ESP_ERR_NO_MEM;
        }

        equipment_slot_t *slot = &s_slots[slot_idx];
        bool already_connected = (slot->connected && memcmp(slot->mac, mac, 6) == 0);
        char prev_name[32] = {0};
        int8_t prev_rssi = slot->rssi;
        if (memcmp(slot->mac, mac, 6) == 0 && slot->name[0] != '\0') {
            strncpy(prev_name, slot->name, sizeof(prev_name) - 1);
        }

        if (!already_connected) {
            memset(slot, 0, sizeof(equipment_slot_t));
        }
        slot->connected = true;
        slot->paired = true;
        memcpy(slot->mac, mac, 6);
        slot->conn_handle = conn_handle;
        slot->rssi = (rssi != 0) ? rssi : prev_rssi;
        slot->equipment_type = (type != EQUIPMENT_TYPE_UNKNOWN) ? type : EQUIPMENT_TYPE_BOILER;
        slot->driver = resolve_driver(slot->equipment_type);
        if (model != NULL && model[0] != '\0') {
            strncpy(slot->model, model, sizeof(slot->model) - 1);
        } else if (slot->model[0] == '\0') {
            strncpy(slot->model, slot->driver->driver_name, sizeof(slot->model) - 1);
        }

        char unique_name[32] = {0};
        assign_unique_slot_name(slot_idx, mac, prev_name[0] ? prev_name : slot->model, slot->model, unique_name);
        strncpy(slot->name, unique_name, sizeof(slot->name) - 1);
        slot->name[sizeof(slot->name) - 1] = '\0';

        if (!already_connected && slot->driver && slot->driver->init_state) {
            slot->driver->init_state(slot);
        }
    } else {
        for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
            if (s_slots[i].connected && memcmp(s_slots[i].mac, mac, 6) == 0) {
                s_slots[i].connected = false;
                break;
            }
        }
    }
    eq_unlock();
    return ESP_OK;
}

esp_err_t equipment_manager_process_notification(const uint8_t mac[6],
                                                 int8_t rssi,
                                                 const uint8_t *data,
                                                 size_t len,
                                                 uint32_t timestamp_ms,
                                                 bool *out_should_publish,
                                                 char *out_json,
                                                 size_t max_json_len) {
    if (mac == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_should_publish != NULL) {
        *out_should_publish = false;
    }

    eq_lock();
    int idx = find_slot_index(mac, EQUIPMENT_TYPE_UNKNOWN);
    if (idx < 0 || !s_slots[idx].connected || s_slots[idx].driver == NULL) {
        eq_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    equipment_slot_t *slot = &s_slots[idx];
    if (rssi != 0) {
        slot->rssi = rssi;
    }
    esp_err_t err = slot->driver->parse_notification(slot, data, len);
    if (err == ESP_OK) {
        bool should_pub = true;
        if (slot->equipment_type == EQUIPMENT_TYPE_BOILER) {
            const g30_state_t *cur = &slot->state.boiler;
            const g30_state_t *prv = &slot->prev_state.boiler;
            bool discrete_changed = (cur->pump_active != prv->pump_active) ||
                                    (cur->heat_active != prv->heat_active) ||
                                    (cur->heat_power_pct != prv->heat_power_pct) ||
                                    (cur->stage.step_index != prv->stage.step_index) ||
                                    (cur->auto_mode != prv->auto_mode) ||
                                    (cur->ramp_active != prv->ramp_active) ||
                                    (cur->interaction.required != prv->interaction.required) ||
                                    (cur->interaction.code != prv->interaction.code) ||
                                    (cur->timer.active != prv->timer.active) ||
                                    (cur->timer.paused != prv->timer.paused);

            float temp_delta = fabsf(cur->current_temp_c - prv->current_temp_c);
            bool throttle_expired = (timestamp_ms - slot->last_telemetry_pub_ms >= TELEMETRY_CADENCE_THROTTLE_MS);
            bool temp_delta_exceeded = (temp_delta >= TELEMETRY_TEMP_DELTA_MIN_C);
            should_pub = (discrete_changed || throttle_expired || temp_delta_exceeded);
            if (should_pub) {
                slot->prev_state.boiler = *cur;
                slot->last_telemetry_pub_ms = timestamp_ms;
            }
        }

        if (out_should_publish != NULL) {
            *out_should_publish = should_pub;
        }
        if (should_pub && out_json != NULL && max_json_len > 0) {
            slot->driver->format_telemetry_json(slot, timestamp_ms, out_json, max_json_len);
        }
    }
    eq_unlock();
    return err;
}

esp_err_t equipment_manager_dispatch_command(const g30_command_t *cmd, uint8_t out_resolved_mac[6]) {
    if (cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    eq_lock();
    int idx = find_slot_index_ex(cmd->target_slot, cmd->target_name, cmd->target_mac, cmd->equipment_type);
    if (idx < 0 || !s_slots[idx].connected || s_slots[idx].driver == NULL) {
        eq_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    equipment_slot_t *slot = &s_slots[idx];
    if (out_resolved_mac != NULL) {
        memcpy(out_resolved_mac, slot->mac, 6);
    }

    esp_err_t err = slot->driver->dispatch_command(slot, cmd, default_enqueue_ble_pkt);
    eq_unlock();
    return err;
}

esp_err_t equipment_manager_dispatch_recipe_ex(uint8_t target_slot,
                                               const char *target_name,
                                               const uint8_t target_mac[6],
                                               equipment_type_t target_type,
                                               const char *req_id,
                                               const g30_recipe_t *recipe,
                                               uint8_t out_resolved_mac[6]) {
    if (recipe == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    eq_lock();
    int idx = find_slot_index_ex(target_slot, target_name, target_mac, target_type);
    if (idx < 0 || !s_slots[idx].connected || s_slots[idx].driver == NULL) {
        eq_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    equipment_slot_t *slot = &s_slots[idx];
    if (out_resolved_mac != NULL) {
        memcpy(out_resolved_mac, slot->mac, 6);
    }

    esp_err_t err = slot->driver->dispatch_recipe(slot, req_id, recipe, default_enqueue_ble_pkt);
    eq_unlock();
    return err;
}

esp_err_t equipment_manager_dispatch_recipe_to_slot(uint8_t target_slot,
                                                    const uint8_t target_mac[6],
                                                    equipment_type_t target_type,
                                                    const char *req_id,
                                                    const g30_recipe_t *recipe,
                                                    uint8_t out_resolved_mac[6]) {
    return equipment_manager_dispatch_recipe_ex(target_slot, NULL, target_mac, target_type, req_id, recipe, out_resolved_mac);
}

esp_err_t equipment_manager_dispatch_recipe(const uint8_t target_mac[6],
                                            equipment_type_t target_type,
                                            const char *req_id,
                                            const g30_recipe_t *recipe,
                                            uint8_t out_resolved_mac[6]) {
    return equipment_manager_dispatch_recipe_ex(0, NULL, target_mac, target_type, req_id, recipe, out_resolved_mac);
}

esp_err_t equipment_manager_format_equipment_list_json(const char *gateway_id,
                                                       const char *req_id,
                                                       char *out_json,
                                                       size_t max_len) {
    if (out_json == NULL || max_len < 64) {
        return ESP_ERR_INVALID_ARG;
    }

    eq_lock();
    uint8_t paired_or_conn_cnt = 0;
    for (uint8_t i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        if (s_slots[i].paired || s_slots[i].connected) {
            paired_or_conn_cnt++;
        }
    }

    size_t pos = 0;
    int written = snprintf(out_json, max_len,
                           "{\"event\":\"equipment_list\"%s%s%s,\"gateway_id\":\"%s\",\"count\":%u,\"equipment\":[",
                           (req_id && req_id[0]) ? ",\"req_id\":\"" : "",
                           (req_id && req_id[0]) ? req_id : "",
                           (req_id && req_id[0]) ? "\"" : "",
                           gateway_id ? gateway_id : "BREWLINK",
                           paired_or_conn_cnt);
    if (written < 0 || (size_t)written >= max_len) {
        eq_unlock();
        return ESP_ERR_INVALID_SIZE;
    }
    pos = (size_t)written;

    bool first = true;
    for (uint8_t i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        const equipment_slot_t *slot = &s_slots[i];
        if (!slot->paired && !slot->connected) {
            continue;
        }

        char mac_colon[18];
        snprintf(mac_colon, sizeof(mac_colon), "%02X:%02X:%02X:%02X:%02X:%02X",
                 slot->mac[0], slot->mac[1], slot->mac[2],
                 slot->mac[3], slot->mac[4], slot->mac[5]);

        const char *type_str = "boiler";
        if (slot->equipment_type == EQUIPMENT_TYPE_FERMENTER) {
            type_str = "fermenter";
        } else if (slot->equipment_type == EQUIPMENT_TYPE_COOLER_HEATER) {
            type_str = "cooler";
        }

        uint32_t caps = slot->driver ? slot->driver->capabilities_mask : 0;
        const char *model_str = slot->model[0] ? slot->model : "BrewLink Equipment";
        const char *name_str = slot->name[0] ? slot->name : model_str;

        written = snprintf(out_json + pos, max_len - pos,
                           "%s{\"slot\":%u,\"paired\":%s,\"connected\":%s,\"name\":\"%s\",\"model\":\"%s\","
                           "\"equipment_type\":\"%s\",\"mac\":\"%s\",\"rssi\":%d,"
                           "\"capabilities\":{\"recipe_upload\":%s,\"mash_profile\":%s,"
                           "\"fermentation_profile\":%s,\"pump_control\":%s,\"heat_control\":%s}}",
                           first ? "" : ",",
                           (unsigned int)(i + 1),
                           (slot->paired || slot->connected) ? "true" : "false",
                           slot->connected ? "true" : "false",
                           name_str,
                           model_str,
                           type_str,
                           mac_colon,
                           slot->rssi,
                           ((caps & EQ_CAP_MASH_RECIPE) || (caps & EQ_CAP_FERM_RECIPE)) ? "true" : "false",
                           (caps & EQ_CAP_MASH_RECIPE) ? "true" : "false",
                           (caps & EQ_CAP_FERM_RECIPE) ? "true" : "false",
                           (caps & EQ_CAP_PUMP_CONTROL) ? "true" : "false",
                           (caps & EQ_CAP_HEAT_CONTROL) ? "true" : "false");
        if (written < 0 || pos + (size_t)written >= max_len - 2) {
            break;
        }
        pos += (size_t)written;
        first = false;
    }

    snprintf(out_json + pos, max_len - pos, "]}");
    eq_unlock();
    return ESP_OK;
}

esp_err_t equipment_manager_get_slot(uint8_t slot_idx, equipment_slot_t *out_slot) {
    if (slot_idx >= BLE_MAX_SIMULTANEOUS_CONNECTIONS || out_slot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    eq_lock();
    *out_slot = s_slots[slot_idx];
    eq_unlock();
    return ESP_OK;
}

esp_err_t equipment_manager_find_connected(const uint8_t target_mac[6],
                                           equipment_type_t target_type,
                                           equipment_slot_t *out_slot) {
    eq_lock();
    int idx = find_slot_index(target_mac, target_type);
    if (idx < 0 || !s_slots[idx].connected) {
        eq_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (out_slot != NULL) {
        *out_slot = s_slots[idx];
    }
    eq_unlock();
    return ESP_OK;
}

esp_err_t equipment_manager_format_announce_json(const equipment_slot_t *slot,
                                                 const char *gateway_id,
                                                 bool connected,
                                                 uint32_t timestamp_ms,
                                                 char *out_json,
                                                 size_t max_len) {
    if (slot == NULL || out_json == NULL || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
             slot->mac[0], slot->mac[1], slot->mac[2],
             slot->mac[3], slot->mac[4], slot->mac[5]);

    const char *type_str = "boiler";
    if (slot->equipment_type == EQUIPMENT_TYPE_FERMENTER) {
        type_str = "fermenter";
    } else if (slot->equipment_type == EQUIPMENT_TYPE_COOLER_HEATER) {
        type_str = "cooler";
    }

    uint32_t caps = slot->driver ? slot->driver->capabilities_mask : 0;
    const char *model_str = slot->model[0] ? slot->model : "BrewLink Equipment";
    const char *name_str = slot->name[0] ? slot->name : model_str;

    if (connected) {
        snprintf(out_json, max_len,
                 "{\"event\":\"connected\",\"gateway_id\":\"%s\",\"mac\":\"%s\",\"name\":\"%s\","
                 "\"equipment_type\":\"%s\",\"model\":\"%s\",\"slot\":%d,\"rssi\":%d,"
                 "\"capabilities\":{\"temperature_read\":%s,\"temperature_set\":%s,"
                 "\"pump_control\":%s,\"heat_control\":%s,\"power_level_control\":%s,"
                 "\"timer_control\":%s,\"recipe_upload\":%s,\"fermentation_profile\":%s,"
                 "\"interaction_prompt\":%s},"
                 "\"writable_ranges\":{\"target_temp_min_c\":2.0,\"target_temp_max_c\":100.0,"
                 "\"power_pct_min\":0,\"power_pct_max\":100},"
                 "\"timestamp_ms\":%lu}",
                 gateway_id ? gateway_id : "BREWLINK",
                 mac_str, name_str, type_str,
                 model_str,
                 slot->conn_handle, slot->rssi,
                 (caps & EQ_CAP_TEMP_READ) ? "true" : "false",
                 (caps & EQ_CAP_TEMP_SET) ? "true" : "false",
                 (caps & EQ_CAP_PUMP_CONTROL) ? "true" : "false",
                 (caps & EQ_CAP_HEAT_CONTROL) ? "true" : "false",
                 (caps & EQ_CAP_POWER_PCT) ? "true" : "false",
                 (caps & EQ_CAP_TIMER_CONTROL) ? "true" : "false",
                 ((caps & EQ_CAP_MASH_RECIPE) || (caps & EQ_CAP_FERM_RECIPE)) ? "true" : "false",
                 (caps & EQ_CAP_FERM_RECIPE) ? "true" : "false",
                 (caps & EQ_CAP_INTERACTION_PROMPT) ? "true" : "false",
                 (unsigned long)timestamp_ms);
    } else {
        snprintf(out_json, max_len,
                 "{\"event\":\"disconnected\",\"gateway_id\":\"%s\",\"mac\":\"%s\",\"name\":\"%s\","
                 "\"equipment_type\":\"%s\",\"timestamp_ms\":%lu}",
                 gateway_id ? gateway_id : "BREWLINK",
                 mac_str, name_str, type_str,
                 (unsigned long)timestamp_ms);
    }
    return ESP_OK;
}
