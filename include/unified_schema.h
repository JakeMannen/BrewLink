#ifndef BREWLINK_UNIFIED_SCHEMA_H
#define BREWLINK_UNIFIED_SCHEMA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Equipment Types Taxonomy
// =============================================================================
typedef enum {
    EQUIPMENT_TYPE_UNKNOWN       = 0,
    EQUIPMENT_TYPE_BOILER        = 1, // All-in-one brewing systems (Grainfather G30, BrewZilla)
    EQUIPMENT_TYPE_FERMENTER     = 2, // Conical fermenters, digital hydrometers (Tilt, Plaato)
    EQUIPMENT_TYPE_COOLER_HEATER = 3  // Glycol chillers, heating belts/jackets
} equipment_type_t;

#define EQUIPMENT_TYPE_GENERIC       EQUIPMENT_TYPE_UNKNOWN
#define EQUIPMENT_TYPE_COOLER        EQUIPMENT_TYPE_COOLER_HEATER

// =============================================================================
// Unified Boiler State Schema (e.g. Grainfather G30)
// =============================================================================
#define MAX_STEP_NAME_LEN           24
#define MAX_INTERACTION_MSG_LEN     48

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
} unified_boiler_state_t;

// =============================================================================
// Unified Fermenter State Schema (for future equipment expansion)
// =============================================================================
typedef struct {
    float    beer_temp_c;
    float    chamber_temp_c;
    float    target_temp_c;
    float    gravity_sg;             // e.g. 1.050
    bool     cooling_active;
    bool     heating_active;
    uint32_t fermentation_age_hours;
    uint32_t last_updated_ms;
} unified_fermenter_state_t;

// =============================================================================
// Unified Cooler / Heater State Schema (for future equipment expansion)
// =============================================================================
#define MAX_CHILLER_ZONES           4

typedef struct {
    float    reservoir_temp_c;
    uint8_t  active_zones_count;
    struct {
        float current_temp_c;
        float target_temp_c;
        bool  pump_running;
        bool  heating_active;
    } zones[MAX_CHILLER_ZONES];
    uint32_t last_updated_ms;
} unified_cooler_heater_state_t;

// =============================================================================
// Unified Recipe Definitions
// =============================================================================
#define MAX_MASH_STEPS              8
#define MAX_BOIL_ADDITIONS          8
#define MAX_RECIPE_NAME_LEN         32

typedef struct {
    float    temp_c;
    uint16_t duration_min;
} unified_mash_step_t;

typedef struct {
    char                name[MAX_RECIPE_NAME_LEN];
    uint16_t            boil_time_min;
    float               mash_water_l;
    float               sparge_water_l;
    uint8_t             mash_step_count;
    unified_mash_step_t mash_steps[MAX_MASH_STEPS];
    uint8_t             boil_addition_count;
    uint16_t            boil_additions_min[MAX_BOIL_ADDITIONS]; // time remaining in boil
} unified_recipe_t;

// =============================================================================
// Unified Downstream Command Actions (WiFi / Dashboard -> Gateway -> BLE)
// =============================================================================
typedef enum {
    CMD_ACTION_NONE             = 0,
    CMD_ACTION_SET_PUMP         = 1,
    CMD_ACTION_SET_HEAT         = 2,
    CMD_ACTION_SET_TARGET_TEMP  = 3,
    CMD_ACTION_SET_TIMER        = 4,
    CMD_ACTION_PAUSE_TIMER      = 5,
    CMD_ACTION_RESUME_TIMER     = 6,
    CMD_ACTION_CANCEL_TIMER     = 7,
    CMD_ACTION_ACK_INTERACTION  = 8,
    CMD_ACTION_DISMISS_ALERT    = 9,
    CMD_ACTION_LOAD_RECIPE      = 10,
    CMD_ACTION_CANCEL_SESSION   = 11
} command_action_t;

typedef struct {
    uint8_t          target_mac[6];
    equipment_type_t equipment_type;
    command_action_t action;
    char             req_id[16];
    union {
        bool             pump_state;
        struct {
            bool    heat_state;
            uint8_t power_pct;
        } heat;
        float            target_temp_c;
        uint16_t         timer_duration_min;
        unified_recipe_t recipe;
    } params;
} unified_command_t;

// Structure for outbound BLE commands queued to Core 0 NimBLE worker
#define BLE_G30_MAX_CMD_PAYLOAD     20

typedef struct {
    uint16_t conn_handle;
    uint8_t  mac[6];
    uint8_t  len;
    char     cmd_bytes[BLE_G30_MAX_CMD_PAYLOAD];
    uint16_t delay_after_ms;         // Minimum inter-packet pacing delay (e.g. 150 ms)
} ble_outbound_cmd_packet_t;

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_UNIFIED_SCHEMA_H
