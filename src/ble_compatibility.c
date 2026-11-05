/**
 * @file ble_compatibility.c
 * @brief Compatible Bluetooth Low Energy Brewing Devices Registry and Validation
 */

#include "ble_compatibility.h"
#include <string.h>

/**
 * @brief Authoritative registry of compatible Bluetooth devices defined in sourcecode
 * @note Only devices in this table can be discovered as connectable or connected to by BrewLink.
 */
static const ble_compatible_device_t s_compatible_devices[] = {
    {
        .model_name      = "Grainfather G30 Connect",
        .equipment_type  = EQUIPMENT_TYPE_BOILER,
        .name_prefix     = "Grain",
        .svc_uuid16      = 0xCDD0,
        .svc_uuid128     = {
            0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
            0x00, 0x10, 0x00, 0x00, 0xd0, 0xcd, 0x00, 0x00
        },
        .notify_uuid128  = {
            0x31, 0x01, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
            0x00, 0x10, 0x00, 0x00, 0xd1, 0xcd, 0x03, 0x00
        },
        .write_uuid128   = {
            0x31, 0x01, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
            0x00, 0x10, 0x00, 0x00, 0xd2, 0xcd, 0x03, 0x00
        },
        .requires_pacing = true,
        .pacing_delay_ms = 150
    }
};

static const size_t s_compatible_device_count = sizeof(s_compatible_devices) / sizeof(s_compatible_devices[0]);

/**
 * @brief Case-insensitive prefix matcher (portable across standard C and ESP-IDF)
 */
static bool case_insensitive_starts_with(const char *str, const char *prefix) {
    if (str == NULL || prefix == NULL) {
        return false;
    }
    while (*prefix != '\0') {
        if (*str == '\0') {
            return false;
        }
        char c1 = *str;
        char c2 = *prefix;
        if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 + ('a' - 'A'));
        if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 + ('a' - 'A'));
        if (c1 != c2) {
            return false;
        }
        str++;
        prefix++;
    }
    return true;
}

const ble_compatible_device_t *ble_compatibility_get_definitions(size_t *count) {
    if (count != NULL) {
        *count = s_compatible_device_count;
    }
    return s_compatible_devices;
}

bool ble_compatibility_match_by_name(const char *name, const ble_compatible_device_t **out_device) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < s_compatible_device_count; i++) {
        if (s_compatible_devices[i].name_prefix != NULL &&
            case_insensitive_starts_with(name, s_compatible_devices[i].name_prefix)) {
            if (out_device != NULL) {
                *out_device = &s_compatible_devices[i];
            }
            return true;
        }
    }
    return false;
}

bool ble_compatibility_match_by_uuid16(uint16_t uuid16, const ble_compatible_device_t **out_device) {
    if (uuid16 == 0) {
        return false;
    }

    for (size_t i = 0; i < s_compatible_device_count; i++) {
        if (s_compatible_devices[i].svc_uuid16 != 0 &&
            s_compatible_devices[i].svc_uuid16 == uuid16) {
            if (out_device != NULL) {
                *out_device = &s_compatible_devices[i];
            }
            return true;
        }
    }
    return false;
}

bool ble_compatibility_match_by_uuid128(const uint8_t *uuid128, const ble_compatible_device_t **out_device) {
    if (uuid128 == NULL) {
        return false;
    }

    for (size_t i = 0; i < s_compatible_device_count; i++) {
        // Check if device defines a non-zero 128-bit service UUID
        bool has_uuid128 = false;
        for (int b = 0; b < BLE_UUID128_LEN; b++) {
            if (s_compatible_devices[i].svc_uuid128[b] != 0) {
                has_uuid128 = true;
                break;
            }
        }

        if (has_uuid128 && memcmp(s_compatible_devices[i].svc_uuid128, uuid128, BLE_UUID128_LEN) == 0) {
            if (out_device != NULL) {
                *out_device = &s_compatible_devices[i];
            }
            return true;
        }
    }
    return false;
}

bool ble_compatibility_is_device_compatible(
    const char *name,
    const uint16_t *uuids16,
    size_t num_uuids16,
    const uint8_t *uuids128,
    size_t num_uuids128,
    const ble_compatible_device_t **out_device)
{
    // 1. Check by advertised name prefix
    if (name != NULL && ble_compatibility_match_by_name(name, out_device)) {
        return true;
    }

    // 2. Check 16-bit service UUIDs
    if (uuids16 != NULL && num_uuids16 > 0) {
        for (size_t i = 0; i < num_uuids16; i++) {
            if (ble_compatibility_match_by_uuid16(uuids16[i], out_device)) {
                return true;
            }
        }
    }

    // 3. Check 128-bit service UUIDs
    if (uuids128 != NULL && num_uuids128 > 0) {
        for (size_t i = 0; i < num_uuids128; i++) {
            const uint8_t *curr_uuid = &uuids128[i * BLE_UUID128_LEN];
            if (ble_compatibility_match_by_uuid128(curr_uuid, out_device)) {
                return true;
            }
        }
    }

    if (out_device != NULL) {
        *out_device = NULL;
    }
    return false;
}
