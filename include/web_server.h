/**
 * @file web_server.h
 * @brief Embedded Web Configuration, REST API, and Full-Duplex WebSocket Server
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
 * @brief Initialize and start the HTTP & WebSocket server on port 80
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t web_server_start(void);

/**
 * @brief Stop the HTTP & WebSocket server
 * @return ESP_OK on success, or ESP error code.
 */
esp_err_t web_server_stop(void);

/**
 * @brief Broadcast a JSON text frame asynchronously to all authenticated WebSocket clients (/ws)
 * @param json_str Null-terminated JSON string to push
 * @return ESP_OK on success
 */
esp_err_t web_server_ws_broadcast(const char *json_str);

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_WEB_SERVER_H
