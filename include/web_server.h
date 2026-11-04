/**
 * @file web_server.h
 * @brief Embedded Web Configuration Server for BrewLink Gateway
 */

#ifndef BREWLINK_WEB_SERVER_H
#define BREWLINK_WEB_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start the HTTP configuration server on port 80
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t web_server_start(void);

/**
 * @brief Stop the HTTP configuration server
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t web_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_WEB_SERVER_H
