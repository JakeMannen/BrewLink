/**
 * @file equipment_manager.h
 * @brief Multi-Equipment Driver Registry and Unified State Dispatcher
 * @details Decouples BrewLink from being exclusive to Grainfather G30 by binding
 *          each connected BLE slot (up to BLE_MAX_SIMULTANEOUS_CONNECTIONS) to a
 *          polymorphic equipment_driver_t (Boilers, Fermenters, Coolers/Chillers).
 */

#ifndef BREWLINK_EQUIPMENT_MANAGER_H
#define BREWLINK_EQUIPMENT_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#else
#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK                  0
#define ESP_FAIL                -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_TIMEOUT         0x107
#endif
typedef void* QueueHandle_t;
#endif

#include "config.h"
#include "unified_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

// Capability bitmask flags
#define EQ_CAP_TEMP_READ            (1UL << 0)
#define EQ_CAP_TEMP_SET             (1UL << 1)
#define EQ_CAP_PUMP_CONTROL         (1UL << 2)
#define EQ_CAP_HEAT_CONTROL         (1UL << 3)
#define EQ_CAP_POWER_PCT            (1UL << 4)
#define EQ_CAP_TIMER_CONTROL        (1UL << 5)
#define EQ_CAP_MASH_RECIPE          (1UL << 6)
#define EQ_CAP_FERM_RECIPE          (1UL << 7)
#define EQ_CAP_INTERACTION_PROMPT   (1UL << 8)

typedef struct equipment_slot_s equipment_slot_t;

/**
 * @brief Callback used to enqueue an outbound BLE command packet
 */
typedef esp_err_t (*eq_enqueue_ble_pkt_fn_t)(const ble_outbound_cmd_packet_t *pkt);

/**
 * @brief Polymorphic Equipment Driver Interface
 */
typedef struct {
    const char      *driver_name;
    equipment_type_t equipment_type;
    uint32_t         capabilities_mask;

    void      (*init_state)(equipment_slot_t *slot);
    esp_err_t (*parse_notification)(equipment_slot_t *slot, const uint8_t *data, size_t len);
    esp_err_t (*dispatch_command)(equipment_slot_t *slot, const unified_command_t *cmd, eq_enqueue_ble_pkt_fn_t enqueue_fn);
    esp_err_t (*dispatch_recipe)(equipment_slot_t *slot, const char *req_id, const unified_recipe_t *recipe, eq_enqueue_ble_pkt_fn_t enqueue_fn);
    esp_err_t (*format_telemetry_json)(const equipment_slot_t *slot, uint32_t timestamp_ms, char *out_json, size_t max_len);
} equipment_driver_t;

/**
 * @brief Per-slot runtime state for a paired / connected BLE equipment device
 */
struct equipment_slot_s {
    bool                      paired;
    bool                      connected;
    uint8_t                   mac[6];
    uint16_t                  conn_handle;
    int8_t                    rssi;
    equipment_type_t          equipment_type;
    char                      name[32];
    char                      model[32];
    const equipment_driver_t *driver;

    union {
        unified_boiler_state_t        boiler;
        unified_fermenter_state_t     fermenter;
        unified_cooler_heater_state_t cooler;
    } state;

    union {
        unified_boiler_state_t        boiler;
        unified_fermenter_state_t     fermenter;
        unified_cooler_heater_state_t cooler;
    } prev_state;

    uint32_t                  last_telemetry_pub_ms;
};

/**
 * @brief Initialize the multi-equipment manager
 * @param ble_cmd_queue FreeRTOS queue for outbound BLE command packets (or NULL in host unit tests)
 * @return ESP_OK on success
 */
esp_err_t equipment_manager_init(QueueHandle_t ble_cmd_queue);

/**
 * @brief Register a custom packet enqueue override function (for native host unit testing)
 * @param fn Callback function receiving outbound BLE packets
 */
void equipment_manager_set_enqueue_override(eq_enqueue_ble_pkt_fn_t fn);

/**
 * @brief Notify equipment manager when a BLE peripheral connects or disconnects
 * @param mac 6-byte device MAC address
 * @param connected True on connect, false on disconnect
 * @param type Equipment type classification
 * @param model Device model/name string
 * @param conn_handle NimBLE connection handle
 * @param rssi Signal strength
 * @return ESP_OK on success
 */
esp_err_t equipment_manager_on_peer_event(const uint8_t mac[6],
                                          bool connected,
                                          equipment_type_t type,
                                          const char *model,
                                          uint16_t conn_handle,
                                          int8_t rssi);

/**
 * @brief Process an incoming BLE notification for a connected peer and format telemetry JSON if needed
 * @param mac 6-byte source MAC address
 * @param rssi Current RSSI
 * @param data Raw notification bytes
 * @param len Notification length
 * @param timestamp_ms Packet timestamp
 * @param[out] out_should_publish Set to true if state change or cadence warrants publishing telemetry
 * @param[out] out_json Buffer to receive formatted telemetry JSON
 * @param max_json_len Size of out_json buffer
 * @return ESP_OK on success
 */
esp_err_t equipment_manager_process_notification(const uint8_t mac[6],
                                                 int8_t rssi,
                                                 const uint8_t *data,
                                                 size_t len,
                                                 uint32_t timestamp_ms,
                                                 bool *out_should_publish,
                                                 char *out_json,
                                                 size_t max_json_len);

/**
 * @brief Dispatch a unified command to a target equipment device (fail-fast if disconnected)
 * @param cmd Pointer to unified command (uses cmd->target_mac or cmd->equipment_type)
 * @param[out] out_resolved_mac Optional 6-byte buffer to receive the resolved target MAC
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if device is disconnected, ESP_ERR_INVALID_STATE if rejected
 */
esp_err_t equipment_manager_dispatch_command(const unified_command_t *cmd, uint8_t out_resolved_mac[6]);

/**
 * @brief Dispatch a unified recipe to a target equipment device (fail-fast if disconnected)
 * @param target_mac Optional 6-byte target MAC (pass NULL or all-zeros to match by target_slot or target_type)
 * @param target_type Equipment type to target if target_mac is not specified (e.g. EQUIPMENT_TYPE_BOILER)
 * @param req_id Correlation request ID attached to the BLE packet sequence
 * @param recipe Pointer to unified recipe structure
 * @param[out] out_resolved_mac Optional 6-byte buffer to receive the resolved target MAC
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if target equipment is disconnected
 */
esp_err_t equipment_manager_dispatch_recipe(const uint8_t target_mac[6],
                                            equipment_type_t target_type,
                                            const char *req_id,
                                            const unified_recipe_t *recipe,
                                            uint8_t out_resolved_mac[6]);

/**
 * @brief Dispatch a unified recipe targeting a 1-based slot index (1..3), unique name, MAC, or equipment type
 * @param target_slot 1-based slot index (1..BLE_MAX_SIMULTANEOUS_CONNECTIONS), or 0 to match by name/MAC/type
 * @param target_name Optional unique device name (case-insensitive match)
 * @param target_mac Optional 6-byte target MAC
 * @param target_type Equipment type fallback
 * @param req_id Correlation request ID
 * @param recipe Pointer to unified recipe structure
 * @param[out] out_resolved_mac Optional 6-byte buffer to receive the resolved target MAC
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if target equipment is disconnected
 */
esp_err_t equipment_manager_dispatch_recipe_ex(uint8_t target_slot,
                                               const char *target_name,
                                               const uint8_t target_mac[6],
                                               equipment_type_t target_type,
                                               const char *req_id,
                                               const unified_recipe_t *recipe,
                                               uint8_t out_resolved_mac[6]);

/**
 * @brief Dispatch a unified recipe targeting a 1-based slot index (1..3), MAC, or equipment type
 * @param target_slot 1-based slot index (1..BLE_MAX_SIMULTANEOUS_CONNECTIONS), or 0 to match by MAC/type
 * @param target_mac Optional 6-byte target MAC
 * @param target_type Equipment type fallback
 * @param req_id Correlation request ID
 * @param recipe Pointer to unified recipe structure
 * @param[out] out_resolved_mac Optional 6-byte buffer to receive the resolved target MAC
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if target equipment is disconnected
 */
esp_err_t equipment_manager_dispatch_recipe_to_slot(uint8_t target_slot,
                                                    const uint8_t target_mac[6],
                                                    equipment_type_t target_type,
                                                    const char *req_id,
                                                    const unified_recipe_t *recipe,
                                                    uint8_t out_resolved_mac[6]);

/**
 * @brief Register or pre-populate a paired BLE device slot (e.g. loaded from NVS at boot)
 * @details Guarantees a unique human-readable device name across all slots.
 * @param mac 6-byte BLE MAC address
 * @param type Equipment type classification
 * @param preferred_name Optional preferred name (if empty or colliding, a unique name is generated)
 * @param model Optional model name
 * @param[out] out_slot_idx Optional pointer to receive the 0-based slot index assigned
 * @return ESP_OK on success
 */
esp_err_t equipment_manager_register_paired_device(const uint8_t mac[6],
                                                   equipment_type_t type,
                                                   const char *preferred_name,
                                                   const char *model,
                                                   uint8_t *out_slot_idx);

/**
 * @brief Remove/unpair a BLE device from the equipment manager slot table
 * @param mac 6-byte BLE MAC address
 * @return ESP_OK on success
 */
esp_err_t equipment_manager_unpair_device(const uint8_t mac[6]);

/**
 * @brief Format a JSON list of all paired/connected BLE equipment devices across all slots
 * @param gateway_id Gateway hardware ID string
 * @param req_id Optional correlation request ID (if non-NULL, included in JSON response)
 * @param[out] out_json Output buffer to receive JSON string
 * @param max_len Size of out_json buffer
 * @return ESP_OK on success
 */
esp_err_t equipment_manager_format_equipment_list_json(const char *gateway_id,
                                                       const char *req_id,
                                                       char *out_json,
                                                       size_t max_len);

/**
 * @brief Get snapshot of a specific equipment slot by index (0 .. BLE_MAX_SIMULTANEOUS_CONNECTIONS-1)
 * @param slot_idx Slot index
 * @param[out] out_slot Destination struct copy
 * @return ESP_OK on success
 */
esp_err_t equipment_manager_get_slot(uint8_t slot_idx, equipment_slot_t *out_slot);

/**
 * @brief Find a connected equipment slot by MAC or equipment type
 * @param target_mac Optional 6-byte MAC (or NULL / all-zeros)
 * @param target_type Equipment type fallback
 * @param[out] out_slot Destination struct copy
 * @return ESP_OK if found and connected, ESP_ERR_NOT_FOUND otherwise
 */
esp_err_t equipment_manager_find_connected(const uint8_t target_mac[6],
                                           equipment_type_t target_type,
                                           equipment_slot_t *out_slot);

/**
 * @brief Format an equipment capability announcement JSON manifest
 * @param slot Pointer to equipment slot
 * @param gateway_id Gateway hardware ID string
 * @param connected True for connected manifest, false for disconnected manifest
 * @param timestamp_ms Timestamp in milliseconds
 * @param[out] out_json Output buffer
 * @param max_len Output buffer size
 * @return ESP_OK on success
 */
esp_err_t equipment_manager_format_announce_json(const equipment_slot_t *slot,
                                                 const char *gateway_id,
                                                 bool connected,
                                                 uint32_t timestamp_ms,
                                                 char *out_json,
                                                 size_t max_len);

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_EQUIPMENT_MANAGER_H
