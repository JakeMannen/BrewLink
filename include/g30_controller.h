/**
 * @file g30_controller.h
 * @brief Thread-Safe Grainfather G30 Controller Interface for BrewLink Gateway
 */

#ifndef BREWLINK_G30_CONTROLLER_H
#define BREWLINK_G30_CONTROLLER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "g30_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get a thread-safe snapshot of the live Grainfather G30 state and connection
 * @param[out] out_state Destination pointer for unified boiler state (can be NULL)
 * @param[out] out_connected Destination boolean: true if G30 is connected, false otherwise
 * @param[out] out_mac Destination 6-byte buffer for connected G30 MAC address (can be NULL)
 * @param[out] out_rssi Destination pointer for connected G30 RSSI (can be NULL)
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t g30_controller_get_state(g30_state_t *out_state,
                                   bool *out_connected,
                                   uint8_t out_mac[6],
                                   int8_t *out_rssi);

/**
 * @brief Thread-safe dispatch of a unified command to the connected Grainfather G30
 * @param cmd Pointer to unified command
 * @return ESP_OK on success,
 *         ESP_ERR_NOT_FOUND if no G30 controller is connected,
 *         ESP_ERR_NO_MEM if command queue is full,
 *         ESP_ERR_INVALID_ARG or ESP_ERR_NOT_SUPPORTED on encoding error.
 */
esp_err_t g30_controller_dispatch_command(const g30_command_t *cmd);

/**
 * @brief Thread-safe dispatch of an automated recipe sequence to the connected Grainfather G30
 * @param recipe Pointer to unified recipe
 * @return ESP_OK on success,
 *         ESP_ERR_NOT_FOUND if no G30 controller is connected,
 *         ESP_ERR_INVALID_ARG if recipe step counts or values are invalid,
 *         ESP_ERR_NO_MEM if command queue is full.
 */
esp_err_t g30_controller_dispatch_recipe(const g30_recipe_t *recipe);

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_G30_CONTROLLER_H
