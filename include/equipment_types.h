#ifndef BREWLINK_EQUIPMENT_TYPES_H
#define BREWLINK_EQUIPMENT_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Equipment category taxonomy (routing/registry only, NOT a data schema).
 * @note Every supported equipment model MUST have its own dedicated schema header
 *       (e.g. g30_schema.h). There is no shared telemetry/command/recipe schema.
 */
typedef enum {
    EQUIPMENT_TYPE_UNKNOWN       = 0,
    EQUIPMENT_TYPE_BOILER        = 1, // Boiler models (Grainfather G30 ...)
    EQUIPMENT_TYPE_FERMENTER     = 2, // Fermenter models (future)
    EQUIPMENT_TYPE_COOLER_HEATER = 3  // Cooler/heater models (future)
} equipment_type_t;

#define EQUIPMENT_TYPE_GENERIC       EQUIPMENT_TYPE_UNKNOWN
#define EQUIPMENT_TYPE_COOLER        EQUIPMENT_TYPE_COOLER_HEATER

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_EQUIPMENT_TYPES_H
