/**
 * @file ble_compatibility.h
 * @brief Definition and Validation of Compatible Bluetooth Low Energy Brewing Devices
 * @details Ensures only verified, compatible brewing equipment defined in sourcecode
 *          can be discovered, listed as connectable, and connected by the BrewLink gateway.
 */

#ifndef BREWLINK_BLE_COMPATIBILITY_H
#define BREWLINK_BLE_COMPATIBILITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_log.h"
#else
#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK              0
#define ESP_FAIL            -1
#define ESP_ERR_NO_MEM      0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NOT_FOUND   0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#endif
#endif

#include "unified_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_UUID128_LEN     16

/**
 * @brief Specification structure for a compatible BLE brewing device defined in sourcecode
 */
typedef struct {
    const char          *model_name;        /**< Human-readable equipment model */
    equipment_type_t     equipment_type;    /**< Unified equipment classification */
    const char          *name_prefix;       /**< Advertising name prefix (case-insensitive) */
    uint16_t             svc_uuid16;        /**< 16-bit primary service UUID alias (or 0) */
    uint8_t              svc_uuid128[BLE_UUID128_LEN];    /**< 128-bit primary service UUID (little-endian) */
    uint8_t              notify_uuid128[BLE_UUID128_LEN]; /**< 128-bit notify characteristic UUID */
    uint8_t              write_uuid128[BLE_UUID128_LEN];  /**< 128-bit write characteristic UUID */
    bool                 requires_pacing;   /**< True if controller requires pacing between writes */
    uint16_t             pacing_delay_ms;   /**< Delay between writes in milliseconds */
} ble_compatible_device_t;

/**
 * @brief Retrieve the array of all compatible BLE devices defined in sourcecode
 * @param[out] count Pointer to receive the total count of compatible device definitions
 * @return Pointer to const array of compatible device definitions
 */
const ble_compatible_device_t *ble_compatibility_get_definitions(size_t *count);

/**
 * @brief Check if an advertised device name matches any compatible device defined in sourcecode
 * @param name Advertised device name string (can be NULL or empty)
 * @param[out] out_device Optional pointer to receive matched compatible device definition
 * @return true if device matches a compatible device in sourcecode, false otherwise
 */
bool ble_compatibility_match_by_name(const char *name, const ble_compatible_device_t **out_device);

/**
 * @brief Check if a 16-bit service UUID matches any compatible device defined in sourcecode
 * @param uuid16 16-bit service UUID
 * @param[out] out_device Optional pointer to receive matched compatible device definition
 * @return true if UUID matches a compatible device, false otherwise
 */
bool ble_compatibility_match_by_uuid16(uint16_t uuid16, const ble_compatible_device_t **out_device);

/**
 * @brief Check if a 128-bit service UUID matches any compatible device defined in sourcecode
 * @param uuid128 16-byte array containing little-endian 128-bit service UUID
 * @param[out] out_device Optional pointer to receive matched compatible device definition
 * @return true if UUID matches a compatible device, false otherwise
 */
bool ble_compatibility_match_by_uuid128(const uint8_t *uuid128, const ble_compatible_device_t **out_device);

/**
 * @brief Comprehensive compatibility check across name, 16-bit UUIDs, and 128-bit UUIDs
 * @param name Advertised device name (or NULL)
 * @param uuids16 Array of 16-bit service UUIDs in advertisement (or NULL)
 * @param num_uuids16 Number of 16-bit UUIDs
 * @param uuids128 Flat buffer of 128-bit service UUIDs (16 bytes each, or NULL)
 * @param num_uuids128 Number of 128-bit UUIDs
 * @param[out] out_device Optional pointer to receive matched compatible device definition
 * @return true if advertisement matches any compatible device defined in sourcecode
 */
bool ble_compatibility_is_device_compatible(
    const char *name,
    const uint16_t *uuids16,
    size_t num_uuids16,
    const uint8_t *uuids128,
    size_t num_uuids128,
    const ble_compatible_device_t **out_device
);

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_BLE_COMPATIBILITY_H
