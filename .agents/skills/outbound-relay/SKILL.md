---
name: outbound-relay
description: >-
  Implement, configure, and manage WiFi station mode, MQTT client with LWT, and HTTP REST relay from FreeRTOS queues on ESP32.
---

# Outbound Relay Subsystem Skill (WiFi, MQTT, HTTP)

This skill provides step-by-step guidance for configuring WiFi Station networking, establishing resilient MQTT telemetry publishing with LWT, and handling HTTP REST requests.

---

## 1. WiFi Station Management (`esp_wifi`)

### 1.1. Event Handler Pattern
Handle standard network lifecycle events non-blockingly:
```c
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi lost; scheduling non-blocking reconnect...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi connected with IP address");
        // Trigger MQTT client start
    }
}
```

---

## 2. MQTT Client Lifecycle (`esp-mqtt`)

### 2.1. Topics & QoS Hierarchy
*   **Gateway Status / LWT**: `brewlink/gateway/status` (QoS 1, retain = 1, `{"status":"online"}` / `{"status":"offline"}`).
*   **Device Telemetry**: `brewlink/device/<MAC_ADDRESS>/telemetry` (QoS 0 for high throughput).
*   **Downstream Commands**: `brewlink/gateway/cmd` (QoS 1).

### 2.2. Initialization with Last Will & Testament (LWT)
```c
esp_mqtt_client_config_t mqtt_cfg = {
    .broker.address.uri = MQTT_BROKER_URI,
    .credentials.username = MQTT_USERNAME,
    .credentials.authentication.password = MQTT_PASSWORD,
    .session.last_will = {
        .topic = MQTT_TOPIC_STATUS,
        .msg = "{\"status\":\"offline\"}",
        .msg_len = 20,
        .qos = 1,
        .retain = 1,
    },
};
```

---

## 3. Telemetry Relay Worker Task (Core 1)

Consume incoming packets from `s_telemetry_queue` and dispatch via MQTT or HTTP:

```c
static void telemetry_relay_task(void *pvParameters) {
    ble_telemetry_packet_t packet;
    char topic[128];
    char json_payload[256];

    while (1) {
        // Block until a BLE packet arrives
        if (xQueueReceive(s_telemetry_queue, &packet, portMAX_DELAY) == pdTRUE) {
            snprintf(topic, sizeof(topic), "%s%02X%02X%02X%02X%02X%02X/telemetry",
                     MQTT_TOPIC_TELEMETRY_PREFIX,
                     packet.mac[0], packet.mac[1], packet.mac[2],
                     packet.mac[3], packet.mac[4], packet.mac[5]);

            snprintf(json_payload, sizeof(json_payload),
                     "{\"rssi\":%d,\"len\":%u,\"ts\":%lu}",
                     packet.rssi, (unsigned int)packet.len, (unsigned long)packet.timestamp_ms);

            if (s_mqtt_connected) {
                esp_mqtt_client_publish(s_mqtt_client, topic, json_payload, 0, MQTT_QOS_TELEMETRY, 0);
            } else {
                ESP_LOGW(TAG, "MQTT offline; dropping packet to prevent memory exhaustion");
            }
        }
    }
}
```

---

## 4. HTTP REST Client (`esp_http_client`)

When sending telemetry via HTTP POST:
1. Initialize configuration:
   ```c
   esp_http_client_config_t config = {
       .url = HTTP_RELAY_ENDPOINT,
       .method = HTTP_METHOD_POST,
       .timeout_ms = 5000,
   };
   esp_http_client_handle_t client = esp_http_client_init(&config);
   ```
2. Set headers and payload:
   ```c
   esp_http_client_set_header(client, "Content-Type", "application/json");
   esp_http_client_set_post_field(client, json_payload, strlen(json_payload));
   ```
3. Execute and clean up:
   ```c
   esp_err_t err = esp_http_client_perform(client);
   esp_http_client_cleanup(client);
   ```
   **Rule**: Always call `esp_http_client_cleanup()` after request completion to prevent leaking LwIP sockets.

---

## 5. SoftAP Onboarding & Web Configuration Server

### 5.1. Fallback & AP Broadcast Rules
*   When no Wi-Fi credentials exist in NVS or if station connection attempts fail (`WIFI_STA_MAX_RETRIES`), activate SoftAP broadcast: `BrewLink-Setup-XXXX` (using last 4 hex characters of the MAC address).
*   Network mode: `WIFI_MODE_APSTA` allows the setup SoftAP to remain active while simultaneously performing scans for surrounding Wi-Fi networks and attempting station connections.
*   SoftAP IP: Default gateway `192.168.4.1`, open authentication for zero-friction user onboarding.

### 5.2. Web Configuration Server Endpoints (`esp_http_server`)
*   `GET /`: Serves embedded single-page application (SPA) in HTML5/CSS/JS with zero external CDN dependencies.
*   `GET /generate_204`, `/hotspot-detect.html`: Captive portal redirects returning `302 Found` to `http://192.168.4.1/`.
*   `GET /api/status`: System overview (Wi-Fi status, IP, MQTT broker status, BLE connected count, free heap, uptime).
*   `GET /api/wifi/scan`: Triggers active Wi-Fi AP scan and returns JSON array of discovered SSIDs, RSSIs, and auth modes.
*   `POST /api/wifi/connect`: Accepts `{"ssid":"...","password":"..."}`, saves to NVS via `config_store`, and connects.
*   `GET /api/mqtt`: Returns current MQTT broker URI and connection status.
*   `POST /api/mqtt`: Accepts `{"broker_uri":"...","username":"...","password":"...","topic_prefix":"..."}`, persists to NVS, and dynamically reconfigures MQTT client.
*   `GET /api/ble/peers`: Returns JSON list of connected BLE peripherals (MAC, name, equipment type, RSSI).
*   `GET /api/ble/scan`: Triggers active BLE discovery scan and returns discovered equipment (with Grainfather G30 auto-detection).
*   `POST /api/ble/connect`: Accepts `{"addr":"...","addr_type":0}` and establishes BLE connection.
*   `POST /api/ble/disconnect`: Accepts `{"conn_handle":X}` and terminates connection.
*   `POST /api/system/restart`: Schedules a graceful gateway reboot.
