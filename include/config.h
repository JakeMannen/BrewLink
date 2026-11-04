#ifndef BREWLINK_CONFIG_H
#define BREWLINK_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "unified_schema.h"

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
    uint16_t         conn_handle;
    uint8_t          addr[6];
    uint8_t          addr_type;
    int8_t           rssi;
    bool             connected;
    bool             services_discovered;
    uint16_t         notify_val_handle;
    uint16_t         write_val_handle;
    equipment_type_t equipment_type;
    char             device_name[32];
} ble_peer_device_t;

// Structure for advertising BLE peripherals discovered during scanning
typedef struct {
    uint8_t          addr[6];
    uint8_t          addr_type;
    int8_t           rssi;
    char             name[32];
    bool             is_grainfather;
    equipment_type_t equipment_type;
    bool             is_connected;
} ble_scan_entry_t;

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
#define BLE_CMD_QUEUE_LENGTH                16
#define NETWORKING_TASK_CORE                0    // Core 0: BLE, WiFi, MQTT, LwIP
#define APP_PROCESSING_TASK_CORE            1    // Core 1: Parsing, state machine, dispatch

#define RELAY_TASK_STACK_SIZE               4096
#define RELAY_TASK_PRIORITY                 5

#define CMD_DISPATCH_TASK_STACK_SIZE        4096
#define CMD_DISPATCH_TASK_PRIORITY          5

// =============================================================================
// Wi-Fi Setup SoftAP & Web Server Configuration
// =============================================================================
#define WIFI_AP_SSID_PREFIX                 "BrewLink-Setup"
#define WIFI_AP_CHANNEL                     1
#define WIFI_AP_MAX_CONN                    4
#define WIFI_STA_MAX_RETRIES                5
#define WEB_SERVER_PORT                     80
#define WEB_SERVER_STACK_SIZE               8192

// =============================================================================
// MQTT Topics & QoS
// =============================================================================
#define MQTT_TOPIC_STATUS                   "brewlink/gateway/status"
#define MQTT_TOPIC_GATEWAY_CMD              "brewlink/gateway/cmd"
#define MQTT_TOPIC_DEVICE_PREFIX            "brewlink/device/"
#define MQTT_TOPIC_TELEMETRY_SUFFIX         "/telemetry"
#define MQTT_TOPIC_CMD_SUFFIX               "/cmd"
#define MQTT_TOPIC_CMD_WILDCARD             "brewlink/device/+/cmd"

#define MQTT_QOS_TELEMETRY                  0
#define MQTT_QOS_STATUS                     1
#define MQTT_QOS_COMMAND                    1

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_CONFIG_H
