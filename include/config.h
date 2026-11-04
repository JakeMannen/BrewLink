#ifndef BREWLINK_CONFIG_H
#define BREWLINK_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// BLE Central Multi-Device Configuration
// =============================================================================
#define BLE_MAX_SIMULTANEOUS_CONNECTIONS    3
#define BLE_SCAN_INTERVAL_MS                100
#define BLE_SCAN_WINDOW_MS                  50
#define BLE_SCAN_DURATION_SEC               0    // Continuous active scanning

// Maximum telemetry payload size per notification packet
#define BLE_MAX_PAYLOAD_SIZE                128

// Structure tracking each connected BLE peripheral peer
typedef struct {
    uint16_t conn_handle;
    uint8_t  addr[6];
    int8_t   rssi;
    bool     connected;
    bool     services_discovered;
    uint16_t notify_val_handle;
    char     device_name[32];
} ble_peer_device_t;

// Structure for telemetry data queued from BLE to WiFi/MQTT relay
typedef struct {
    uint8_t  device_index;
    uint8_t  mac[6];
    int8_t   rssi;
    uint16_t handle;
    size_t   len;
    uint8_t  data[BLE_MAX_PAYLOAD_SIZE];
    uint32_t timestamp_ms;
} ble_telemetry_packet_t;

// =============================================================================
// FreeRTOS Queues & Core Pinning
// =============================================================================
#define TELEMETRY_QUEUE_LENGTH              32
#define NETWORKING_TASK_CORE                0    // Core 0: BLE, WiFi, MQTT, LwIP
#define APP_PROCESSING_TASK_CORE            1    // Core 1: Parsing, state machine, sensor logic

#define RELAY_TASK_STACK_SIZE               4096
#define RELAY_TASK_PRIORITY                 5

// =============================================================================
// MQTT Topics & QoS
// =============================================================================
#define MQTT_TOPIC_STATUS                   "brewlink/gateway/status"
#define MQTT_TOPIC_TELEMETRY_PREFIX         "brewlink/device/"
#define MQTT_TOPIC_CMD                      "brewlink/gateway/cmd"

#define MQTT_QOS_TELEMETRY                  0
#define MQTT_QOS_STATUS                     1

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_CONFIG_H
