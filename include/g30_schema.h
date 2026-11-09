/**
 * @file g30_schema.h
 * @brief Data schema for the Grainfather G30 Connect ONLY.
 * @note Each supported equipment model owns its own schema header (<model>_schema.h).
 */
#ifndef BREWLINK_G30_SCHEMA_H
#define BREWLINK_G30_SCHEMA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "equipment_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Grainfather G30 Connect State Schema (G30-specific; not shared with other equipment)
// =============================================================================
#define MAX_STEP_NAME_LEN           24
#define MAX_INTERACTION_MSG_LEN     64

typedef struct {
    float    current_temp_c;
    float    target_temp_c;
    float    boil_temp_c;
    bool     heat_active;
    uint8_t  heat_power_pct;         // 0 - 100%
    bool     pump_active;
    bool     auto_mode;             // true = recipe / auto schedule, false = manual
    bool     ramp_active;           // true = heating to next step setpoint
    
    struct {
        bool     active;
        bool     paused;
        uint32_t remaining_seconds;
        uint32_t total_seconds;
    } timer;

    struct {
        uint8_t  step_index;
        char     step_name[MAX_STEP_NAME_LEN];
    } stage;

    struct {
        bool     required;
        uint8_t  code;
        char     message[MAX_INTERACTION_MSG_LEN];
    } interaction;

    uint32_t last_updated_ms;
} g30_state_t;

// =============================================================================
// Grainfather G30 Connect Recipe Definitions
// =============================================================================
#define MAX_MASH_STEPS              8
#define MAX_BOIL_ADDITIONS          8
#define MAX_FERMENTATION_STEPS      4
#define MAX_RECIPE_NAME_LEN         32

typedef struct {
    float    temp_c;
    uint16_t duration_min;
} g30_mash_step_t;

typedef struct {
    float    temp_c;
    uint16_t duration_hours;
} g30_fermentation_step_t;

typedef struct {
    char                        name[MAX_RECIPE_NAME_LEN];
    uint16_t                    boil_time_min;
    uint16_t                    hop_stand_min;
    float                       mash_water_l;
    float                       sparge_water_l;
    uint8_t                     mash_step_count;
    g30_mash_step_t         mash_steps[MAX_MASH_STEPS];
    uint8_t                     boil_addition_count;
    uint16_t                    boil_additions_min[MAX_BOIL_ADDITIONS]; // time remaining in boil
    uint8_t                     fermentation_step_count;
    g30_fermentation_step_t fermentation_steps[MAX_FERMENTATION_STEPS];
    float                       target_og_sg;
    float                       target_fg_sg;
} g30_recipe_t;

// =============================================================================
// Grainfather G30 Connect Downstream Command Actions (WiFi / Dashboard -> Gateway -> BLE)
// =============================================================================
typedef enum {
    G30_CMD_ACTION_NONE                 = 0,
    G30_CMD_ACTION_SET_PUMP             = 1,
    G30_CMD_ACTION_SET_HEAT             = 2,
    G30_CMD_ACTION_SET_TARGET_TEMP      = 3,
    G30_CMD_ACTION_SET_TIMER            = 4,
    G30_CMD_ACTION_PAUSE_TIMER          = 5,
    G30_CMD_ACTION_RESUME_TIMER         = 6,
    G30_CMD_ACTION_CANCEL_TIMER         = 7,
    G30_CMD_ACTION_ACK_INTERACTION      = 8,
    G30_CMD_ACTION_DISMISS_ALERT        = 9,
    G30_CMD_ACTION_LOAD_RECIPE          = 10,
    G30_CMD_ACTION_CANCEL_SESSION       = 11,
    G30_CMD_ACTION_PRESS_SET            = 12,
    G30_CMD_ACTION_DISMISS_SPARGE_ALERT = 13
} g30_command_action_t;

typedef struct {
    uint8_t          target_mac[6];
    uint8_t          target_slot;    // Optional 1-based slot index (1..3); 0 = not specified
    char             target_name[32];// Optional unique device name
    equipment_type_t equipment_type;
    g30_command_action_t action;
    char             req_id[16];
    union {
        bool             pump_state;
        struct {
            bool    heat_state;
            uint8_t power_pct;
        } heat;
        float            target_temp_c;
        uint16_t         timer_duration_min;
        g30_recipe_t recipe;
    } params;
} g30_command_t;

// Structure for outbound BLE commands queued to Core 0 NimBLE worker
#define BLE_G30_MAX_CMD_PAYLOAD     20

typedef struct {
    uint16_t conn_handle;
    uint8_t  mac[6];
    uint8_t  len;
    char     cmd_bytes[BLE_G30_MAX_CMD_PAYLOAD];
    uint16_t delay_after_ms;         // Minimum inter-packet pacing delay (e.g. 150 ms)
    char     req_id[16];             // Correlation ID for sequence completion tracking
    char     action[24];             // Action name for cmd_ack reporting
    uint8_t  seq_idx;                // 1-based packet index in command sequence (0 = untracked)
    uint8_t  seq_total;              // Total packets in sequence (when seq_idx == seq_total, emit completion)
} ble_outbound_cmd_packet_t;

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_G30_SCHEMA_H

