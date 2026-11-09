#ifndef BREWLINK_PLACEHOLDER_SCHEMA_H
#define BREWLINK_PLACEHOLDER_SCHEMA_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// TEMPORARY STUBS: not tied to any real equipment model. Each real fermenter/cooler model
// added in the future MUST get its own <model>_schema.h and replace these stubs.

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
} placeholder_fermenter_state_t;

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
} placeholder_cooler_state_t;

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_PLACEHOLDER_SCHEMA_H

