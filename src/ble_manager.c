/**
 * @file ble_manager.c
 * @brief NimBLE Multi-Device BLE Central Subsystem Implementation
 */

#include "ble_manager.h"
#include "ble_compatibility.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "grainfather_g30.h"

static const char *TAG = "BLE_MGR";

// Grainfather G30 Service and Characteristic UUIDs (128-bit little-endian)
static const ble_uuid128_t s_g30_svc_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xd0, 0xcd, 0x00, 0x00);

static const ble_uuid128_t s_g30_notify_uuid =
    BLE_UUID128_INIT(0x31, 0x01, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xd1, 0xcd, 0x03, 0x00);

static const ble_uuid128_t s_g30_write_uuid =
    BLE_UUID128_INIT(0x31, 0x01, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xd2, 0xcd, 0x03, 0x00);

static QueueHandle_t s_telemetry_queue = NULL;
static QueueHandle_t s_ble_cmd_queue = NULL;

static ble_peer_device_t s_peers[BLE_MAX_SIMULTANEOUS_CONNECTIONS];
static ble_scan_entry_t  s_scan_entries[BLE_SCAN_MAX_RESULTS];
static uint16_t          s_scan_count = 0;
static bool              s_is_scanning = false;

static SemaphoreHandle_t s_ble_mutex = NULL;
static ble_peer_event_cb_t s_peer_cb = NULL;

// Forward declarations
static int ble_gap_event(struct ble_gap_event *event, void *arg);
static int ble_on_mtu(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg);
static int ble_on_disc_svc(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *service, void *arg);
static int ble_on_disc_chr(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg);
static int ble_on_cccd_write(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg);
static void ble_handle_notify_rx(uint16_t conn_handle, uint16_t attr_handle, struct os_mbuf *om);

static int find_peer_by_handle(uint16_t conn_handle) {
    for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        if (s_peers[i].connected && s_peers[i].conn_handle == conn_handle) {
            return i;
        }
    }
    return -1;
}

static int find_free_peer_slot(void) {
    for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        if (!s_peers[i].connected) {
            return i;
        }
    }
    return -1;
}

// -----------------------------------------------------------------------------
// BLE Outbound Command Worker Task (Pinned to Protocol Core 0)
// -----------------------------------------------------------------------------
static void ble_cmd_worker_task(void *pvParameters) {
    ble_outbound_cmd_packet_t cmd_pkt;
    ESP_LOGI(TAG, "BLE command worker task active on core %d", xPortGetCoreID());

    while (1) {
        if (xQueueReceive(s_ble_cmd_queue, &cmd_pkt, portMAX_DELAY) == pdTRUE) {
            uint16_t target_handle = cmd_pkt.conn_handle;
            uint16_t write_handle = 0;

            if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                int peer_idx = -1;
                if (target_handle != BLE_HS_CONN_HANDLE_NONE) {
                    peer_idx = find_peer_by_handle(target_handle);
                } else {
                    bool has_mac = false;
                    for (int i = 0; i < 6; i++) {
                        if (cmd_pkt.mac[i] != 0) {
                            has_mac = true;
                            break;
                        }
                    }
                    if (has_mac) {
                        for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
                            if (s_peers[i].connected && memcmp(s_peers[i].addr, cmd_pkt.mac, 6) == 0) {
                                peer_idx = i;
                                target_handle = s_peers[i].conn_handle;
                                break;
                            }
                        }
                    } else {
                        for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
                            if (s_peers[i].connected) {
                                peer_idx = i;
                                target_handle = s_peers[i].conn_handle;
                                break;
                            }
                        }
                    }
                }

                if (peer_idx >= 0 && s_peers[peer_idx].connected) {
                    write_handle = s_peers[peer_idx].write_val_handle;
                }
                xSemaphoreGive(s_ble_mutex);
            }

            if (target_handle != BLE_HS_CONN_HANDLE_NONE && write_handle != 0) {
                ESP_LOGI(TAG, "Writing BLE command to conn %d (handle 0x%04X): '%.*s'",
                         target_handle, write_handle, cmd_pkt.len, cmd_pkt.cmd_bytes);
                int rc = ble_gattc_write_no_rsp_flat(target_handle, write_handle,
                                                     cmd_pkt.cmd_bytes, cmd_pkt.len);
                if (rc != 0) {
                    ESP_LOGE(TAG, "ble_gattc_write_no_rsp_flat failed: rc=%d", rc);
                }
            } else {
                ESP_LOGW(TAG, "Cannot dispatch command; no active BLE connection handle or write handle (conn=%d, write_handle=0x%04X)",
                         target_handle, write_handle);
            }

            uint16_t delay_ms = cmd_pkt.delay_after_ms > 0 ? cmd_pkt.delay_after_ms : G30_WRITE_PACING_MS;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
}

// -----------------------------------------------------------------------------
// GATT Callbacks (Decoupled Handoff Pattern)
// -----------------------------------------------------------------------------
static void ble_handle_notify_rx(uint16_t conn_handle, uint16_t attr_handle, struct os_mbuf *om) {
    if (om == NULL) {
        return;
    }

    ble_telemetry_packet_t packet;
    memset(&packet, 0, sizeof(packet));

    if (xSemaphoreTake(s_ble_mutex, 0) == pdTRUE) {
        int peer_idx = find_peer_by_handle(conn_handle);
        if (peer_idx >= 0) {
            packet.device_index = (uint8_t)peer_idx;
            memcpy(packet.mac, s_peers[peer_idx].addr, 6);
            packet.rssi = s_peers[peer_idx].rssi;
        }
        xSemaphoreGive(s_ble_mutex);
    }

    packet.handle = attr_handle;
    packet.len = OS_MBUF_PKTLEN(om);
    if (packet.len > BLE_MAX_PAYLOAD_SIZE) {
        packet.len = BLE_MAX_PAYLOAD_SIZE;
    }
    os_mbuf_copydata(om, 0, packet.len, packet.data);
    packet.timestamp_ms = esp_log_timestamp();

    if (s_telemetry_queue != NULL) {
        xQueueSendToBack(s_telemetry_queue, &packet, 0);
    }
}

static int ble_on_cccd_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg) {
    if (error->status == 0) {
        ESP_LOGI(TAG, "CCCD subscription confirmed for conn %d", conn_handle);
    } else {
        ESP_LOGE(TAG, "CCCD subscription failed for conn %d: status=%d", conn_handle, error->status);
    }
    return 0;
}

static int ble_on_disc_chr(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == 0 && chr != NULL) {
        if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            int peer_idx = find_peer_by_handle(conn_handle);
            if (peer_idx >= 0) {
                if (ble_uuid_cmp(&chr->uuid.u, &s_g30_notify_uuid.u) == 0) {
                    s_peers[peer_idx].notify_val_handle = chr->val_handle;
                    ESP_LOGI(TAG, "Resolved G30 Notify characteristic handle: 0x%04X", chr->val_handle);
                } else if (ble_uuid_cmp(&chr->uuid.u, &s_g30_write_uuid.u) == 0) {
                    s_peers[peer_idx].write_val_handle = chr->val_handle;
                    ESP_LOGI(TAG, "Resolved G30 Write characteristic handle: 0x%04X", chr->val_handle);
                }
            }
            xSemaphoreGive(s_ble_mutex);
        }
    } else if (chr == NULL) {
        ESP_LOGI(TAG, "Characteristic discovery complete for conn %d", conn_handle);
        ble_peer_device_t peer_copy;
        bool newly_discovered = false;
        uint16_t notify_handle = 0;

        if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            int peer_idx = find_peer_by_handle(conn_handle);
            if (peer_idx >= 0) {
                notify_handle = s_peers[peer_idx].notify_val_handle;
                if (s_peers[peer_idx].notify_val_handle != 0 &&
                    s_peers[peer_idx].write_val_handle != 0 &&
                    !s_peers[peer_idx].services_discovered) {
                    s_peers[peer_idx].services_discovered = true;
                    peer_copy = s_peers[peer_idx];
                    newly_discovered = true;
                }
            }
            xSemaphoreGive(s_ble_mutex);
        }

        // Subscribe to CCCD now that characteristic discovery has completed
        if (notify_handle != 0) {
            uint8_t cccd_val[2] = {0x01, 0x00};
            uint16_t cccd_handle = notify_handle + 1;
            ESP_LOGI(TAG, "Subscribing to CCCD at handle 0x%04X for conn %d...", cccd_handle, conn_handle);
            int rc = ble_gattc_write_flat(conn_handle, cccd_handle, cccd_val, sizeof(cccd_val), ble_on_cccd_write, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to initiate CCCD subscription write: rc=%d", rc);
            }
        }

        if (newly_discovered && s_peer_cb != NULL) {
            s_peer_cb(&peer_copy, true);
        }
    }
    return 0;
}

static int ble_on_disc_svc(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg) {
    if (error->status == 0 && service != NULL) {
        if (ble_uuid_cmp(&service->uuid.u, &s_g30_svc_uuid.u) == 0) {
            ESP_LOGI(TAG, "Discovered G30 Primary Service (0xCDD0). Discovering characteristics...");
            if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                int peer_idx = find_peer_by_handle(conn_handle);
                if (peer_idx >= 0) {
                    s_peers[peer_idx].equipment_type = EQUIPMENT_TYPE_BOILER;
                }
                xSemaphoreGive(s_ble_mutex);
            }
            ble_gattc_disc_all_chrs(conn_handle, service->start_handle, service->end_handle, ble_on_disc_chr, NULL);
        }
    } else if (service == NULL) {
        // Discovery finished; verify if a compatible service was found
        if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            int peer_idx = find_peer_by_handle(conn_handle);
            if (peer_idx >= 0 && s_peers[peer_idx].equipment_type == EQUIPMENT_TYPE_UNKNOWN) {
                ESP_LOGW(TAG, "Connected device on handle %d offers no compatible brewing service; disconnecting", conn_handle);
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            xSemaphoreGive(s_ble_mutex);
        }
    }
    return 0;
}

static int ble_on_mtu(uint16_t conn_handle, const struct ble_gatt_error *error,
                      uint16_t mtu, void *arg) {
    if (error->status == 0) {
        ESP_LOGI(TAG, "MTU exchange completed for conn %d: MTU=%u", conn_handle, mtu);
        ble_gattc_disc_all_svcs(conn_handle, ble_on_disc_svc, NULL);
    } else {
        ESP_LOGW(TAG, "MTU exchange failed (status=%d); continuing with service discovery", error->status);
        ble_gattc_disc_all_svcs(conn_handle, ble_on_disc_svc, NULL);
    }
    return 0;
}

// -----------------------------------------------------------------------------
// GAP Event Handler
// -----------------------------------------------------------------------------
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            struct ble_hs_adv_fields fields;
            memset(&fields, 0, sizeof(fields));
            int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);

            char dev_name[32] = {0};
            if (rc == 0 && fields.name != NULL && fields.name_len > 0) {
                size_t cpy_len = fields.name_len < sizeof(dev_name) - 1 ? fields.name_len : sizeof(dev_name) - 1;
                memcpy(dev_name, fields.name, cpy_len);
                dev_name[cpy_len] = '\0';
            }

            uint16_t adv_u16[8];
            size_t num_u16 = 0;
            if (rc == 0 && fields.uuids16 != NULL && fields.num_uuids16 > 0) {
                num_u16 = fields.num_uuids16 < 8 ? fields.num_uuids16 : 8;
                for (size_t i = 0; i < num_u16; i++) {
                    adv_u16[i] = fields.uuids16[i].value;
                }
            }

            uint8_t adv_u128[4 * BLE_UUID128_LEN];
            size_t num_u128 = 0;
            if (rc == 0 && fields.uuids128 != NULL && fields.num_uuids128 > 0) {
                num_u128 = fields.num_uuids128 < 4 ? fields.num_uuids128 : 4;
                for (size_t i = 0; i < num_u128; i++) {
                    memcpy(&adv_u128[i * BLE_UUID128_LEN], fields.uuids128[i].value, BLE_UUID128_LEN);
                }
            }

            const ble_compatible_device_t *compat = NULL;
            bool is_compat = ble_compatibility_is_device_compatible(
                dev_name,
                (num_u16 > 0) ? adv_u16 : NULL, num_u16,
                (num_u128 > 0) ? adv_u128 : NULL, num_u128,
                &compat
            );

            // Filter: ONLY connectable devices defined in sourcecode are recorded
            if (!is_compat || compat == NULL) {
                return 0;
            }

            bool is_g30 = (compat->equipment_type == EQUIPMENT_TYPE_BOILER &&
                           (strstr(compat->model_name, "Grainfather") != NULL));

            // Record in scan table
            if (xSemaphoreTake(s_ble_mutex, 0) == pdTRUE) {
                int found_idx = -1;
                for (int i = 0; i < s_scan_count; i++) {
                    if (memcmp(s_scan_entries[i].addr, event->disc.addr.val, 6) == 0) {
                        found_idx = i;
                        break;
                    }
                }

                if (found_idx >= 0) {
                    s_scan_entries[found_idx].rssi = event->disc.rssi;
                    if (dev_name[0] != '\0') {
                        strncpy(s_scan_entries[found_idx].name, dev_name, sizeof(s_scan_entries[found_idx].name) - 1);
                    }
                    s_scan_entries[found_idx].is_grainfather = is_g30;
                    s_scan_entries[found_idx].is_compatible = true;
                    s_scan_entries[found_idx].equipment_type = compat->equipment_type;
                    strncpy(s_scan_entries[found_idx].model, compat->model_name, sizeof(s_scan_entries[found_idx].model) - 1);
                } else if (s_scan_count < BLE_SCAN_MAX_RESULTS) {
                    ble_scan_entry_t *entry = &s_scan_entries[s_scan_count++];
                    memcpy(entry->addr, event->disc.addr.val, 6);
                    entry->addr_type = event->disc.addr.type;
                    entry->rssi = event->disc.rssi;
                    if (dev_name[0] != '\0') {
                        strncpy(entry->name, dev_name, sizeof(entry->name) - 1);
                    } else {
                        strncpy(entry->name, compat->model_name, sizeof(entry->name) - 1);
                    }
                    entry->is_grainfather = is_g30;
                    entry->is_compatible = true;
                    entry->equipment_type = compat->equipment_type;
                    strncpy(entry->model, compat->model_name, sizeof(entry->model) - 1);
                    entry->is_connected = (find_peer_by_handle(BLE_HS_CONN_HANDLE_NONE) != -1);
                }
                xSemaphoreGive(s_ble_mutex);
            }
            return 0;
        }

        case BLE_GAP_EVENT_CONNECT: {
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "BLE connected! conn_handle=%d", event->connect.conn_handle);
                if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    int slot = find_free_peer_slot();
                    if (slot >= 0) {
                        s_peers[slot].conn_handle = event->connect.conn_handle;
                        s_peers[slot].connected = true;

                        struct ble_gap_conn_desc desc;
                        if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                            memcpy(s_peers[slot].addr, desc.peer_id_addr.val, 6);
                            s_peers[slot].addr_type = desc.peer_id_addr.type;
                        }

                        // Check if device name was known from scan
                        for (int i = 0; i < s_scan_count; i++) {
                            if (memcmp(s_scan_entries[i].addr, s_peers[slot].addr, 6) == 0) {
                                strncpy(s_peers[slot].device_name, s_scan_entries[i].name, sizeof(s_peers[slot].device_name) - 1);
                                s_peers[slot].equipment_type = s_scan_entries[i].equipment_type;
                                break;
                            }
                        }
                        if (s_peers[slot].device_name[0] == '\0') {
                            snprintf(s_peers[slot].device_name, sizeof(s_peers[slot].device_name),
                                     "Equipment-%02X%02X", s_peers[slot].addr[4], s_peers[slot].addr[5]);
                        }
                    } else {
                        ESP_LOGW(TAG, "No free peer slot available for conn_handle %d", event->connect.conn_handle);
                    }
                    xSemaphoreGive(s_ble_mutex);
                }

                // Exchange MTU
                ble_gattc_exchange_mtu(event->connect.conn_handle, ble_on_mtu, NULL);
            } else {
                ESP_LOGE(TAG, "BLE connection establishment failed: status=%d", event->connect.status);
            }
            return 0;
        }

        case BLE_GAP_EVENT_DISCONNECT: {
            ESP_LOGW(TAG, "BLE disconnected! conn_handle=%d (reason=%d)",
                     event->disconnect.conn.conn_handle, event->disconnect.reason);

            ble_peer_device_t disc_peer;
            bool had_peer = false;

            if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                int slot = find_peer_by_handle(event->disconnect.conn.conn_handle);
                if (slot >= 0) {
                    disc_peer = s_peers[slot];
                    had_peer = true;
                    memset(&s_peers[slot], 0, sizeof(ble_peer_device_t));
                    s_peers[slot].conn_handle = BLE_HS_CONN_HANDLE_NONE;
                }
                xSemaphoreGive(s_ble_mutex);
            }

            if (had_peer && s_peer_cb != NULL) {
                s_peer_cb(&disc_peer, false);
            }
            return 0;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGI(TAG, "BLE discovery scan complete");
            s_is_scanning = false;
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX:
            ble_handle_notify_rx(event->notify_rx.conn_handle,
                                 event->notify_rx.attr_handle,
                                 event->notify_rx.om);
            return 0;

        default:
            return 0;
    }
}

// -----------------------------------------------------------------------------
// NimBLE Host Task
// -----------------------------------------------------------------------------
static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "NimBLE host task running on core %d", xPortGetCoreID());
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void) {
    ESP_LOGI(TAG, "NimBLE host synchronized. Multi-central BLE ready.");
}

esp_err_t ble_manager_init(QueueHandle_t telemetry_queue, QueueHandle_t cmd_queue) {
    s_telemetry_queue = telemetry_queue;
    s_ble_cmd_queue = cmd_queue;

    s_ble_mutex = xSemaphoreCreateMutex();
    if (s_ble_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(s_peers, 0, sizeof(s_peers));
    for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        s_peers[i].conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }

    // Launch outbound BLE command worker on Core 0
    xTaskCreatePinnedToCore(
        ble_cmd_worker_task,
        "ble_cmd_worker",
        CMD_DISPATCH_TASK_STACK_SIZE,
        NULL,
        CMD_DISPATCH_TASK_PRIORITY,
        NULL,
        NETWORKING_TASK_CORE
    );

    // Initialize NimBLE port
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_on_sync;

    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

esp_err_t ble_manager_register_peer_cb(ble_peer_event_cb_t cb) {
    s_peer_cb = cb;
    return ESP_OK;
}

esp_err_t ble_manager_start_scan(uint32_t duration_sec) {
    if (s_is_scanning) {
        ble_gap_disc_cancel();
    }

    if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_scan_count = 0;
        memset(s_scan_entries, 0, sizeof(s_scan_entries));
        xSemaphoreGive(s_ble_mutex);
    }

    struct ble_gap_disc_params disc_params = {
        .filter_duplicates = 1,
        .passive = 0,
        .itvl = BLE_SCAN_INTERVAL_MS * 1000 / 625,
        .window = BLE_SCAN_WINDOW_MS * 1000 / 625,
    };

    int32_t duration_ms = (duration_sec > 0) ? (duration_sec * 1000) : BLE_HS_FOREVER;
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, duration_ms, &disc_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: rc=%d", rc);
        return ESP_FAIL;
    }

    s_is_scanning = true;
    ESP_LOGI(TAG, "Active BLE scanning started (duration=%ld ms)", (long)duration_ms);
    return ESP_OK;
}

esp_err_t ble_manager_stop_scan(void) {
    if (s_is_scanning) {
        ble_gap_disc_cancel();
        s_is_scanning = false;
        ESP_LOGI(TAG, "BLE scan cancelled");
    }
    return ESP_OK;
}

esp_err_t ble_manager_get_scan_results(ble_scan_entry_t *out_entries, uint16_t max_entries, uint16_t *count) {
    if (out_entries == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint16_t to_copy = (s_scan_count < max_entries) ? s_scan_count : max_entries;
    for (uint16_t i = 0; i < to_copy; i++) {
        memcpy(&out_entries[i], &s_scan_entries[i], sizeof(ble_scan_entry_t));
        // Update live connection state
        for (int p = 0; p < BLE_MAX_SIMULTANEOUS_CONNECTIONS; p++) {
            if (s_peers[p].connected && memcmp(s_peers[p].addr, out_entries[i].addr, 6) == 0) {
                out_entries[i].is_connected = true;
                break;
            }
        }
    }
    *count = to_copy;

    xSemaphoreGive(s_ble_mutex);
    return ESP_OK;
}

bool ble_manager_is_addr_compatible(const uint8_t *addr) {
    if (addr == NULL) {
        return false;
    }

    bool compat = false;
    if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < s_scan_count; i++) {
            if (memcmp(s_scan_entries[i].addr, addr, 6) == 0) {
                compat = s_scan_entries[i].is_compatible;
                break;
            }
        }
        xSemaphoreGive(s_ble_mutex);
    }
    return compat;
}

esp_err_t ble_manager_connect(const uint8_t *addr, uint8_t addr_type) {
    if (addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Enforce compatibility check: only devices verified as compatible defined in sourcecode can be connected
    if (!ble_manager_is_addr_compatible(addr)) {
        ESP_LOGW(TAG, "Connection rejected: %02X:%02X:%02X:%02X:%02X:%02X is not a compatible device defined in sourcecode",
                 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_is_scanning) {
        ble_gap_disc_cancel();
        s_is_scanning = false;
    }

    ble_addr_t target_addr;
    target_addr.type = addr_type;
    memcpy(target_addr.val, addr, 6);

    ESP_LOGI(TAG, "Initiating BLE connection to compatible device %02X:%02X:%02X:%02X:%02X:%02X (type=%d)...",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr_type);

    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &target_addr, 30000, NULL, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed: rc=%d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ble_manager_disconnect(uint16_t conn_handle) {
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Terminating BLE connection handle %d...", conn_handle);
    int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_manager_disconnect_by_addr(const uint8_t *addr) {
    if (addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
    if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
            if (s_peers[i].connected && memcmp(s_peers[i].addr, addr, 6) == 0) {
                handle = s_peers[i].conn_handle;
                break;
            }
        }
        xSemaphoreGive(s_ble_mutex);
    }

    if (handle != BLE_HS_CONN_HANDLE_NONE) {
        return ble_manager_disconnect(handle);
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t ble_manager_get_peers(ble_peer_device_t *out_peers, uint8_t max_peers, uint8_t *count) {
    if (out_peers == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t copied = 0;
    for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS && copied < max_peers; i++) {
        if (s_peers[i].connected) {
            memcpy(&out_peers[copied], &s_peers[i], sizeof(ble_peer_device_t));
            copied++;
        }
    }
    *count = copied;

    xSemaphoreGive(s_ble_mutex);
    return ESP_OK;
}

uint8_t ble_manager_get_connected_count(void) {
    uint8_t count = 0;
    if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
            if (s_peers[i].connected) {
                count++;
            }
        }
        xSemaphoreGive(s_ble_mutex);
    }
    return count;
}
