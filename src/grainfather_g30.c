/**
 * @file grainfather_g30.c
 * @brief Grainfather G30 Connect BLE Protocol Parser & Command Encoder
 * @details Conforms to BrewLink embedded constraints (no dynamic memory allocations)
 */

#include "grainfather_g30.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#endif

// =============================================================================
// Helper: Interaction Descriptions
// =============================================================================
const char* g30_get_interaction_message(uint8_t code) {
    switch (code) {
        case 0:
            return "Normal operation";
        case 1:
            return "Strike temperature reached: Add grain";
        case 2:
            return "Mash step finished: Sparge alert";
        case 3:
            return "Boil temperature reached: Add first hops";
        case 4:
            return "Boil addition alert: Add timed hops/adjuncts";
        case 5:
            return "Boil finished: Start cooling";
        default:
            return "User interaction required";
    }
}

// Resolve contextual interaction message taking into account temperature and heating status
static void g30_resolve_interaction_message(unified_boiler_state_t *state, uint8_t code) {
    if (state == NULL) {
        return;
    }

    // Determine if strike temperature has actually been reached
    bool temp_reached = (state->target_temp_c > 0.0f &&
                         state->current_temp_c >= (state->target_temp_c - 2.0f));

    // Initial cold water / strike water fill step (prior to heating)
    bool is_initial_strike_step = (state->auto_mode && state->stage.step_index <= 1 &&
                                   !state->heat_active && !temp_reached);

    if (code == 1 || is_initial_strike_step) {
        if (!temp_reached) {
            // Cold water -> Initial prompt: Add strike water & Press SET
            state->interaction.required = true;
            state->interaction.code = 1;
            if (strncmp(state->interaction.message, "Add ", 4) != 0) {
                strncpy(state->interaction.message, "Add mash water & Press SET to start heating",
                        sizeof(state->interaction.message) - 1);
                state->interaction.message[sizeof(state->interaction.message) - 1] = '\0';
            }
            strncpy(state->stage.step_name, "Add Strike Water", sizeof(state->stage.step_name) - 1);
            state->stage.step_name[sizeof(state->stage.step_name) - 1] = '\0';
            return;
        } else {
            // Strike temperature reached -> Prompt to add grain
            state->interaction.required = true;
            state->interaction.code = 1;
            strncpy(state->interaction.message, "Strike temperature reached: Add grain",
                    sizeof(state->interaction.message) - 1);
            state->interaction.message[sizeof(state->interaction.message) - 1] = '\0';
            strncpy(state->stage.step_name, "Mash Step 1", sizeof(state->stage.step_name) - 1);
            state->stage.step_name[sizeof(state->stage.step_name) - 1] = '\0';
            return;
        }
    }

    // Populate stage step name for other steps
    if (state->auto_mode) {
        if (state->stage.step_index <= 1) {
            if (state->ramp_active || state->heat_active) {
                strncpy(state->stage.step_name, "Heating Strike Water", sizeof(state->stage.step_name) - 1);
            } else {
                strncpy(state->stage.step_name, "Mash Step 1", sizeof(state->stage.step_name) - 1);
            }
        } else {
            snprintf(state->stage.step_name, sizeof(state->stage.step_name), "Mash Step %u", state->stage.step_index);
        }
        state->stage.step_name[sizeof(state->stage.step_name) - 1] = '\0';
    } else {
        strncpy(state->stage.step_name, "Manual Mode", sizeof(state->stage.step_name) - 1);
        state->stage.step_name[sizeof(state->stage.step_name) - 1] = '\0';
    }

    if (!state->interaction.required) {
        strncpy(state->interaction.message, "Normal operation", sizeof(state->interaction.message) - 1);
        state->interaction.message[sizeof(state->interaction.message) - 1] = '\0';
        return;
    }

    strncpy(state->interaction.message, g30_get_interaction_message(code),
            sizeof(state->interaction.message) - 1);
    state->interaction.message[sizeof(state->interaction.message) - 1] = '\0';
}
// =============================================================================
esp_err_t g30_format_padded_cmd(const char *cmd_str, char out_buf[G30_COMMAND_LEN + 1]) {
    if (cmd_str == NULL || out_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t in_len = strlen(cmd_str);
    if (in_len > G30_COMMAND_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Copy command bytes
    memcpy(out_buf, cmd_str, in_len);

    // Pad trailing bytes with spaces (ASCII 0x20)
    memset(out_buf + in_len, ' ', G30_COMMAND_LEN - in_len);
    out_buf[G30_COMMAND_LEN] = '\0';

    return ESP_OK;
}

// =============================================================================
// Command Encoder (Unified Command -> G30 19-Byte Command)
// =============================================================================
esp_err_t g30_encode_command(const unified_command_t *cmd, char out_buf[G30_COMMAND_LEN + 1]) {
    if (cmd == NULL || out_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char raw_cmd[G30_COMMAND_LEN + 1];
    memset(raw_cmd, 0, sizeof(raw_cmd));

    switch (cmd->action) {
        case CMD_ACTION_SET_PUMP:
            snprintf(raw_cmd, sizeof(raw_cmd), "%s", cmd->params.pump_state ? "L1" : "L0");
            break;

        case CMD_ACTION_SET_HEAT:
            snprintf(raw_cmd, sizeof(raw_cmd), "%s", cmd->params.heat.heat_state ? "K1" : "K0");
            break;

        case CMD_ACTION_SET_TARGET_TEMP:
            // Format: "$XX.X," or "$XX," - trailing comma is mandatory
            if ((int)(cmd->params.target_temp_c * 10) % 10 == 0) {
                snprintf(raw_cmd, sizeof(raw_cmd), "$%u,", (unsigned int)cmd->params.target_temp_c);
            } else {
                snprintf(raw_cmd, sizeof(raw_cmd), "$%.1f,", cmd->params.target_temp_c);
            }
            break;

        case CMD_ACTION_SET_TIMER:
            snprintf(raw_cmd, sizeof(raw_cmd), "S%u", cmd->params.timer_duration_min);
            break;

        case CMD_ACTION_PAUSE_TIMER:
        case CMD_ACTION_RESUME_TIMER:
            snprintf(raw_cmd, sizeof(raw_cmd), "G");
            break;

        case CMD_ACTION_CANCEL_TIMER:
            snprintf(raw_cmd, sizeof(raw_cmd), "C");
            break;

        case CMD_ACTION_ACK_INTERACTION:
            snprintf(raw_cmd, sizeof(raw_cmd), "I");
            break;

        case CMD_ACTION_DISMISS_ALERT:
            snprintf(raw_cmd, sizeof(raw_cmd), "A");
            break;

        case CMD_ACTION_PRESS_SET:
            snprintf(raw_cmd, sizeof(raw_cmd), "T");
            break;

        case CMD_ACTION_CANCEL_SESSION:
            snprintf(raw_cmd, sizeof(raw_cmd), "Q1");
            break;

        default:
            return ESP_ERR_NOT_SUPPORTED;
    }

    return g30_format_padded_cmd(raw_cmd, out_buf);
}

// =============================================================================
// Recipe Command Encoders
// =============================================================================

// Line 1: Main Header: R{boil_min},{step_count},{mash_water},{sparge_water},
esp_err_t g30_encode_recipe_header(const unified_recipe_t *recipe, char out_buf[G30_COMMAND_LEN + 1]) {
    if (recipe == NULL || out_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char raw_cmd[G30_COMMAND_LEN + 1];
    snprintf(raw_cmd, sizeof(raw_cmd), "R%u,%u,%.1f,%.1f,",
             recipe->boil_time_min,
             recipe->mash_step_count,
             recipe->mash_water_l,
             recipe->sparge_water_l);

    return g30_format_padded_cmd(raw_cmd, out_buf);
}

// Line 2: Recipe Flags: 0,1,1,0,0,
// (water_treat=0, sparge_counter=1, sparge_alert=1, delayed=0, skip_start=0)
esp_err_t g30_encode_recipe_flags(char out_buf[G30_COMMAND_LEN + 1]) {
    if (out_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return g30_format_padded_cmd("0,1,1,0,0,", out_buf);
}

// Line 3: Recipe Name (sanitized uppercase ASCII, truncated to 19 characters, space padded)
esp_err_t g30_encode_recipe_name(const char *name, char out_buf[G30_COMMAND_LEN + 1]) {
    if (out_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char clean_name[G30_COMMAND_LEN + 1];
    memset(clean_name, 0, sizeof(clean_name));

    if (name != NULL && name[0] != '\0') {
        int dst_idx = 0;
        for (int i = 0; dst_idx < G30_COMMAND_LEN && name[i] != '\0'; i++) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_') {
                clean_name[dst_idx++] = c;
            }
        }
        clean_name[dst_idx] = '\0';
    }

    if (clean_name[0] == '\0') {
        strncpy(clean_name, "BREW RECIPE", sizeof(clean_name) - 1);
    }

    return g30_format_padded_cmd(clean_name, out_buf);
}

// Line 4: Boil Parameters: {hop_stand_min},{boil_addition_count},0,0,
esp_err_t g30_encode_recipe_boil_params(uint16_t hop_stand_min, uint8_t boil_addition_count, char out_buf[G30_COMMAND_LEN + 1]) {
    if (out_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char raw_cmd[G30_COMMAND_LEN + 1];
    snprintf(raw_cmd, sizeof(raw_cmd), "%u,%u,0,0,", hop_stand_min, boil_addition_count);

    return g30_format_padded_cmd(raw_cmd, out_buf);
}

// Line 5..: Boil Addition Stop: {time_remaining_min},
esp_err_t g30_encode_recipe_boil_stop(uint16_t boil_time_remaining_min, char out_buf[G30_COMMAND_LEN + 1]) {
    if (out_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char raw_cmd[G30_COMMAND_LEN + 1];
    snprintf(raw_cmd, sizeof(raw_cmd), "%u,", boil_time_remaining_min);

    return g30_format_padded_cmd(raw_cmd, out_buf);
}

// Mash Step: {temp_c}:{duration_min}
esp_err_t g30_encode_recipe_mash_step(const unified_mash_step_t *step, char out_buf[G30_COMMAND_LEN + 1]) {
    if (step == NULL || out_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char raw_cmd[G30_COMMAND_LEN + 1];
    // Layout: {temp_c}:{duration_min}
    snprintf(raw_cmd, sizeof(raw_cmd), "%u:%u",
             (unsigned int)step->temp_c,
             step->duration_min);

    return g30_format_padded_cmd(raw_cmd, out_buf);
}

// =============================================================================
// Notification Parser (BLE Notification ASCII -> Unified State)
// =============================================================================
esp_err_t g30_parse_notification(const uint8_t *data, size_t len,
                                 unified_boiler_state_t *state,
                                 g30_notification_type_t *out_type) {
    if (data == NULL || len == 0 || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char buf[64];
    size_t copy_len = len < (sizeof(buf) - 1) ? len : (sizeof(buf) - 1);
    memcpy(buf, data, copy_len);
    buf[copy_len] = '\0';

    char prefix = buf[0];
    const char *payload = &buf[1];

    if (out_type != NULL) {
        *out_type = (g30_notification_type_t)prefix;
    }

    switch (prefix) {
        case G30_NOTIF_TEMPERATURE: { // X{Target},{Current}
            float target = 0.0f, current = 0.0f;
            if (sscanf(payload, "%f,%f", &target, &current) == 2) {
                state->target_temp_c = target;
                state->current_temp_c = current;
                g30_resolve_interaction_message(state, state->interaction.code);
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        }

        case G30_NOTIF_SYSTEM_STATE: { // Y{Heat},{Pump},{Auto},{Ramp},{InterMode},{InterCode},{Stage},{Delayed}
            int heat = 0, pump = 0, auto_m = 0, ramp = 0, inter_m = 0, inter_c = 0, stage = 0, del = 0;
            int count = sscanf(payload, "%d,%d,%d,%d,%d,%d,%d,%d",
                               &heat, &pump, &auto_m, &ramp, &inter_m, &inter_c, &stage, &del);
            if (count >= 6) {
                state->heat_active = (heat == 1);
                state->pump_active = (pump == 1);
                state->auto_mode = (auto_m == 1);
                state->ramp_active = (ramp == 1);
                state->interaction.required = (inter_m == 1);
                state->interaction.code = (uint8_t)inter_c;
                state->stage.step_index = (uint8_t)stage;
                g30_resolve_interaction_message(state, (uint8_t)inter_c);
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        }

        case G30_NOTIF_TIMER: { // T{Active},{LeftMin},{TotalMin},{LeftSec}
            int active = 0, left_min = 0, tot_min = 0, left_sec = 0;
            int count = sscanf(payload, "%d,%d,%d,%d", &active, &left_min, &tot_min, &left_sec);
            if (count >= 3) {
                state->timer.active = (active == 1);
                state->timer.remaining_seconds = (uint32_t)(left_sec > 0 ? left_sec : (left_min * 60));
                state->timer.total_seconds = (uint32_t)(tot_min * 60);
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        }

        case G30_NOTIF_POWER_FLAGS: { // W{PwrPct},{Paused},{StepMash},{Interrupted},{ManualPwr},{SpargeAlert}
            int pwr = 0, paused = 0;
            int count = sscanf(payload, "%d,%d", &pwr, &paused);
            if (count >= 2) {
                state->heat_power_pct = (uint8_t)pwr;
                state->timer.paused = (paused == 1);
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        }

        case G30_NOTIF_INTERACTION: { // I{InteractionCode}
            int code = 0;
            if (sscanf(payload, "%d", &code) == 1) {
                state->interaction.required = (code != 0);
                state->interaction.code = (uint8_t)code;
                g30_resolve_interaction_message(state, (uint8_t)code);
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        }

        case G30_NOTIF_BOIL_TEMP: { // C{BoilTemp}
            float boil_t = 100.0f;
            if (sscanf(payload, "%f", &boil_t) == 1) {
                state->boil_temp_c = boil_t;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        }

        case G30_NOTIF_ERROR: { // E{ErrorCode}
            // Error logged
            break;
        }

        default:
            return ESP_ERR_NOT_SUPPORTED;
    }

#ifdef ESP_PLATFORM
    state->last_updated_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
#endif

    return ESP_OK;
}
