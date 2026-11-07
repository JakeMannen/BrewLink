/**
 * @file ble_manager.c
 * @brief NimBLE Multi-Device BLE Central Subsystem Implementation
 */

#include "ble_manager.h"
#include "ble_compatibility.h"
#include "config_store.h"
#include "equipment_manager.h"
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

// Grainfather G30 Service (16-bit 0xCDD0 and 128-bit little-endian) and Characteristic UUIDs
static const ble_uuid16_t s_g30_svc_uuid16 = BLE_UUID16_INIT(0xCDD0);

static const ble_uuid128_t s_g30_svc_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xd0, 0xcd, 0x00, 0x00);

static const ble_uuid128_t s_g30_notify_uuid =
    BLE_UUID128_INIT(0x31, 0x01, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xd1, 0xcd, 0x03, 0x00);

static const ble_uuid128_t s_g30_write_uuid =
    BLE_UUID128_INIT(0x31, 0x01, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xd2, 0xcd, 0x03, 0x00);

static QueueHandle_t s_telemetry_queue = NULL;
static QueueHandle_t s_ble_cmd_queue = NULL;

static ble_peer_device_t s_peers[BLE_MAX_SIMULTANEOUS_CONNECTIONS];
static uint16_t          s_svc_start_handles[BLE_MAX_SIMULTANEOUS_CONNECTIONS];
static uint16_t          s_svc_end_handles[BLE_MAX_SIMULTANEOUS_CONNECTIONS];
static ble_scan_entry_t  s_scan_entries[BLE_SCAN_MAX_RESULTS];
static uint16_t          s_scan_count = 0;
static bool              s_is_scanning = false;
static bool              s_is_connecting = false;
static bool              s_ble_synced = false;

static SemaphoreHandle_t         s_ble_mutex = NULL;
static ble_peer_event_cb_t       s_peer_cb = NULL;
static ble_cmd_seq_complete_cb_t s_seq_cb = NULL;

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

static bool is_addr_currently_connected_locked(const uint8_t addr[6]) {
    for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        if (s_peers[i].connected && memcmp(s_peers[i].addr, addr, 6) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_addr_disconnected_paired_device(const uint8_t addr[6], paired_ble_device_t *out_paired) {
    bool already_conn = false;
    if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        already_conn = is_addr_currently_connected_locked(addr);
        xSemaphoreGive(s_ble_mutex);
    }
    if (already_conn) {
        return false;
    }

    // 1. Check NVS paired BLE registry
    paired_ble_device_t paired[CONFIG_STORE_MAX_PAIRED_BLE];
    if (config_store_get_paired_ble(paired) == ESP_OK) {
        for (uint8_t i = 0; i < CONFIG_STORE_MAX_PAIRED_BLE; i++) {
            if (paired[i].active && memcmp(paired[i].addr, addr, 6) == 0) {
                if (out_paired != NULL) {
                    *out_paired = paired[i];
                }
                return true;
            }
        }
    }

    // 2. Also check in-memory equipment_manager paired slots
    for (uint8_t i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        equipment_slot_t slot;
        if (equipment_manager_get_slot(i, &slot) == ESP_OK &&
            slot.paired && !slot.connected && memcmp(slot.mac, addr, 6) == 0) {
            if (out_paired != NULL) {
                memset(out_paired, 0, sizeof(*out_paired));
                out_paired->active = true;
                memcpy(out_paired->addr, slot.mac, 6);
                out_paired->equipment_type = (uint8_t)slot.equipment_type;
                strncpy(out_paired->name, slot.name, sizeof(out_paired->name) - 1);
                strncpy(out_paired->model, slot.model, sizeof(out_paired->model) - 1);
            }
            return true;
        }
    }
    return false;
}

static bool has_disconnected_paired_devices(void) {
    paired_ble_device_t paired[CONFIG_STORE_MAX_PAIRED_BLE];
    bool any_disconnected = false;

    if (config_store_get_paired_ble(paired) == ESP_OK) {
        if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (uint8_t i = 0; i < CONFIG_STORE_MAX_PAIRED_BLE; i++) {
                if (paired[i].active && !is_addr_currently_connected_locked(paired[i].addr)) {
                    any_disconnected = true;
                    break;
                }
            }
            xSemaphoreGive(s_ble_mutex);
        }
    }
    if (any_disconnected) {
        return true;
    }

    for (uint8_t i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
        equipment_slot_t slot;
        if (equipment_manager_get_slot(i, &slot) == ESP_OK && slot.paired && !slot.connected) {
            return true;
        }
    }
    return false;
}

// Purge queued commands matching a disconnected/failed peer MAC and report aborted sequence if needed
static void purge_queued_commands_for_mac(const uint8_t mac[6], bool notify_abort) {
    if (s_ble_cmd_queue == NULL || mac == NULL) {
        return;
    }

    UBaseType_t waiting = uxQueueMessagesWaiting(s_ble_cmd_queue);
    if (waiting == 0) {
        return;
    }

    char aborted_req_id[16] = {0};
    char aborted_action[24] = {0};
    bool found_aborted_seq = false;

    for (UBaseType_t i = 0; i < waiting; i++) {
        ble_outbound_cmd_packet_t pkt;
        if (xQueueReceive(s_ble_cmd_queue, &pkt, 0) == pdTRUE) {
            if (memcmp(pkt.mac, mac, 6) == 0) {
                if (pkt.seq_total > 0 && !found_aborted_seq) {
                    strncpy(aborted_req_id, pkt.req_id, sizeof(aborted_req_id) - 1);
                    strncpy(aborted_action, pkt.action[0] ? pkt.action : "load_recipe", sizeof(aborted_action) - 1);
                    found_aborted_seq = true;
                }
            } else {
                // Re-queue packets belonging to other connected peers
                xQueueSendToBack(s_ble_cmd_queue, &pkt, 0);
            }
        }
    }

    if (notify_abort && found_aborted_seq && s_seq_cb != NULL) {
        ESP_LOGW(TAG, "Aborted in-flight BLE command sequence (req_id='%s') due to disconnect", aborted_req_id);
        s_seq_cb(mac, aborted_req_id, aborted_action, ESP_ERR_INVALID_STATE);
    }
}

// -----------------------------------------------------------------------------
// BLE Outbound Command Worker Task (Pinned to Protocol Core 0)
// -----------------------------------------------------------------------------
static void ble_cmd_worker_task(void *pvParameters) {
    ble_outbound_cmd_packet_t cmd_pkt;
    ESP_LOGI(TAG, "BLE command worker task active on core %d", xPortGetCoreID());

    while (1) {
        if (xQueueReceive(s_ble_cmd_queue, &cmd_pkt, pdMS_TO_TICKS(1500)) == pdTRUE) {
            uint16_t target_handle = cmd_pkt.conn_handle;
            uint16_t write_handle = 0;
            uint8_t resolved_mac[6] = {0};
            memcpy(resolved_mac, cmd_pkt.mac, 6);

            if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                int peer_idx = -1;
                if (target_handle != BLE_HS_CONN_HANDLE_NONE && target_handle != 0) {
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
                                memcpy(resolved_mac, s_peers[i].addr, 6);
                                break;
                            }
                        }
                    }
                }

                if (peer_idx >= 0 && s_peers[peer_idx].connected) {
                    write_handle = s_peers[peer_idx].write_val_handle;
                    memcpy(resolved_mac, s_peers[peer_idx].addr, 6);
                }
                xSemaphoreGive(s_ble_mutex);
            }

            if (target_handle != BLE_HS_CONN_HANDLE_NONE && write_handle != 0) {
                ESP_LOGI(TAG, "Writing BLE command [%u/%u] to conn %d (handle 0x%04X): '%.*s'",
                         cmd_pkt.seq_idx, cmd_pkt.seq_total,
                         target_handle, write_handle, cmd_pkt.len, cmd_pkt.cmd_bytes);
                int rc = ble_gattc_write_no_rsp_flat(target_handle, write_handle,
                                                     cmd_pkt.cmd_bytes, cmd_pkt.len);
                if (rc != 0) {
                    ESP_LOGE(TAG, "ble_gattc_write_no_rsp_flat failed: rc=%d", rc);
                    purge_queued_commands_for_mac(resolved_mac, false);
                    if (cmd_pkt.seq_total > 0 && s_seq_cb != NULL) {
                        s_seq_cb(resolved_mac, cmd_pkt.req_id,
                                 cmd_pkt.action[0] ? cmd_pkt.action : "cmd",
                                 ESP_ERR_INVALID_STATE);
                    }
                } else if (cmd_pkt.seq_total > 0 && cmd_pkt.seq_idx == cmd_pkt.seq_total) {
                    if (s_seq_cb != NULL) {
                        s_seq_cb(resolved_mac, cmd_pkt.req_id,
                                 cmd_pkt.action[0] ? cmd_pkt.action : "cmd",
                                 ESP_OK);
                    }
                }
            } else {
                ESP_LOGW(TAG, "Cannot dispatch command; peer disconnected mid-sequence (conn=%d, write_handle=0x%04X)",
                         target_handle, write_handle);
                purge_queued_commands_for_mac(resolved_mac, false);
                if (cmd_pkt.seq_total > 0 && s_seq_cb != NULL) {
                    s_seq_cb(resolved_mac, cmd_pkt.req_id,
                             cmd_pkt.action[0] ? cmd_pkt.action : "cmd",
                             ESP_ERR_INVALID_STATE);
                }
            }

            uint16_t delay_ms = cmd_pkt.delay_after_ms > 0 ? cmd_pkt.delay_after_ms : G30_WRITE_PACING_MS;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        } else {
            // 1. Refresh live HCI RSSI for all connected peers
            if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                for (int i = 0; i < BLE_MAX_SIMULTANEOUS_CONNECTIONS; i++) {
                    if (s_peers[i].connected && s_peers[i].conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                        int8_t live_rssi = 0;
                        if (ble_gap_conn_rssi(s_peers[i].conn_handle, &live_rssi) == 0 && live_rssi != 0) {
                            s_peers[i].rssi = live_rssi;
                        }
                    }
                }
                xSemaphoreGive(s_ble_mutex);
            }

            // 2. Periodic check: if any paired BLE equipment is currently disconnected, scan to auto-reconnect
            if (s_ble_synced && !ble_gap_disc_active() && !ble_gap_conn_active()) {
                s_is_scanning = false;
                s_is_connecting = false;
                if (has_disconnected_paired_devices()) {
                    ESP_LOGI(TAG, "Paired BLE equipment offline; starting background auto-reconnect scan...");
                    ble_manager_start_scan(5);
                }
            }
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

    int8_t live_rssi = 0;
    bool have_live_rssi = (ble_gap_conn_rssi(conn_handle, &live_rssi) == 0 && live_rssi != 0);

    ble_telemetry_packet_t packet;
    memset(&packet, 0, sizeof(packet));

    if (xSemaphoreTake(s_ble_mutex, 0) == pdTRUE) {
        int peer_idx = find_peer_by_handle(conn_handle);
        if (peer_idx >= 0) {
            if (have_live_rssi) {
                s_peers[peer_idx].rssi = live_rssi;
            }
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
        if (ble_uuid_cmp(&service->uuid.u, &s_g30_svc_uuid.u) == 0 ||
            ble_uuid_cmp(&service->uuid.u, &s_g30_svc_uuid16.u) == 0) {
            ESP_LOGI(TAG, "Discovered G30 Primary Service (0xCDD0, handles %u..%u)",
                     service->start_handle, service->end_handle);
            if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                int peer_idx = find_peer_by_handle(conn_handle);
                if (peer_idx >= 0) {
                    s_peers[peer_idx].equipment_type = EQUIPMENT_TYPE_BOILER;
                    s_svc_start_handles[peer_idx] = service->start_handle;
                    s_svc_end_handles[peer_idx] = service->end_handle;
                }
                xSemaphoreGive(s_ble_mutex);
            }
        }
    } else if (service == NULL) {
        // Service discovery finished; launch characteristic discovery sequentially
        uint16_t start_h = 0;
        uint16_t end_h = 0;
        bool has_compat_svc = false;

        if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            int peer_idx = find_peer_by_handle(conn_handle);
            if (peer_idx >= 0) {
                start_h = s_svc_start_handles[peer_idx];
                end_h = s_svc_end_handles[peer_idx];
                has_compat_svc = (s_peers[peer_idx].equipment_type != EQUIPMENT_TYPE_UNKNOWN);
            }
            xSemaphoreGive(s_ble_mutex);
        }

        if (start_h != 0 && end_h != 0) {
            ESP_LOGI(TAG, "Discovering G30 characteristics (handles %u..%u)...", start_h, end_h);
            ble_gattc_disc_all_chrs(conn_handle, start_h, end_h, ble_on_disc_chr, NULL);
        } else if (!has_compat_svc) {
            ESP_LOGW(TAG, "Connected device on handle %d offers no compatible brewing service; disconnecting", conn_handle);
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
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
            // 1. Check FIRST if this advertising peripheral is a paired device that is currently offline.
            //    This ensures we auto-reconnect immediately even if the current packet is an ADV_IND
            //    before SCAN_RSP (which carries the local name "Grain").
            paired_ble_device_t paired_dev;
            bool is_disconnected_paired = is_addr_disconnected_paired_device(event->disc.addr.val, &paired_dev);

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

            // Filter: ONLY connectable devices defined in sourcecode (or already paired) are recorded
            if ((!is_compat || compat == NULL) && !is_disconnected_paired) {
                return 0;
            }

            equipment_type_t eq_type = compat ? compat->equipment_type : (equipment_type_t)paired_dev.equipment_type;
            if (eq_type == EQUIPMENT_TYPE_UNKNOWN) {
                eq_type = EQUIPMENT_TYPE_BOILER;
            }
            const char *model_str = compat ? compat->model_name :
                                    (paired_dev.model[0] ? paired_dev.model : "Grainfather G30 Connect");
            bool is_g30 = (eq_type == EQUIPMENT_TYPE_BOILER && (strstr(model_str, "Grainfather") != NULL));

            // Record in scan table (including latest RSSI)
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
                    s_scan_entries[found_idx].equipment_type = eq_type;
                    strncpy(s_scan_entries[found_idx].model, model_str, sizeof(s_scan_entries[found_idx].model) - 1);
                } else if (s_scan_count < BLE_SCAN_MAX_RESULTS) {
                    ble_scan_entry_t *entry = &s_scan_entries[s_scan_count++];
                    memcpy(entry->addr, event->disc.addr.val, 6);
                    entry->addr_type = event->disc.addr.type;
                    entry->rssi = event->disc.rssi;
                    if (dev_name[0] != '\0') {
                        strncpy(entry->name, dev_name, sizeof(entry->name) - 1);
                    } else if (is_disconnected_paired && paired_dev.name[0] != '\0') {
                        strncpy(entry->name, paired_dev.name, sizeof(entry->name) - 1);
                    } else {
                        strncpy(entry->name, model_str, sizeof(entry->name) - 1);
                    }
                    entry->is_grainfather = is_g30;
                    entry->is_compatible = true;
                    entry->equipment_type = eq_type;
                    strncpy(entry->model, model_str, sizeof(entry->model) - 1);
                    entry->is_connected = (find_peer_by_handle(BLE_HS_CONN_HANDLE_NONE) != -1);
                }
                xSemaphoreGive(s_ble_mutex);
            }

            // Auto-reconnect if this advertising peripheral is a paired device that is currently offline
            if (is_disconnected_paired && !ble_gap_conn_active()) {
                ESP_LOGI(TAG, "Paired equipment '%s' (%02X:%02X:%02X:%02X:%02X:%02X, RSSI %d dBm) detected; auto-reconnecting...",
                         paired_dev.name[0] ? paired_dev.name : model_str,
                         event->disc.addr.val[0], event->disc.addr.val[1], event->disc.addr.val[2],
                         event->disc.addr.val[3], event->disc.addr.val[4], event->disc.addr.val[5],
                         event->disc.rssi);
                ble_gap_disc_cancel();
                s_is_scanning = false;
                s_is_connecting = true;
                int conn_rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr, 5000, NULL, ble_gap_event, NULL);
                if (conn_rc != 0) {
                    ESP_LOGW(TAG, "Auto-reconnect ble_gap_connect failed: rc=%d", conn_rc);
                    s_is_connecting = false;
                }
            }
            return 0;
        }

        case BLE_GAP_EVENT_CONNECT: {
            s_is_connecting = false;
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "BLE connected! conn_handle=%d", event->connect.conn_handle);
                ble_peer_device_t connected_peer;
                bool has_connected_peer = false;

                int8_t live_rssi = 0;
                bool have_live_rssi = (ble_gap_conn_rssi(event->connect.conn_handle, &live_rssi) == 0 && live_rssi != 0);

                if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    int slot = find_free_peer_slot();
                    if (slot >= 0) {
                        memset(&s_peers[slot], 0, sizeof(ble_peer_device_t));
                        s_svc_start_handles[slot] = 0;
                        s_svc_end_handles[slot] = 0;
                        s_peers[slot].conn_handle = event->connect.conn_handle;
                        s_peers[slot].connected = true;
                        s_peers[slot].rssi = have_live_rssi ? live_rssi : -60;

                        struct ble_gap_conn_desc desc;
                        if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                            memcpy(s_peers[slot].addr, desc.peer_id_addr.val, 6);
                            s_peers[slot].addr_type = desc.peer_id_addr.type;
                        }

                        // Check if device name and RSSI were known from scan
                        for (int i = 0; i < s_scan_count; i++) {
                            if (memcmp(s_scan_entries[i].addr, s_peers[slot].addr, 6) == 0) {
                                strncpy(s_peers[slot].device_name, s_scan_entries[i].name, sizeof(s_peers[slot].device_name) - 1);
                                s_peers[slot].equipment_type = s_scan_entries[i].equipment_type;
                                if (!have_live_rssi && s_scan_entries[i].rssi != 0) {
                                    s_peers[slot].rssi = s_scan_entries[i].rssi;
                                }
                                break;
                            }
                        }
                        if (s_peers[slot].device_name[0] == '\0') {
                            paired_ble_device_t paired_list[CONFIG_STORE_MAX_PAIRED_BLE];
                            if (config_store_get_paired_ble(paired_list) == ESP_OK) {
                                for (uint8_t p = 0; p < CONFIG_STORE_MAX_PAIRED_BLE; p++) {
                                    if (paired_list[p].active && memcmp(paired_list[p].addr, s_peers[slot].addr, 6) == 0) {
                                        strncpy(s_peers[slot].device_name,
                                                paired_list[p].model[0] ? paired_list[p].model : paired_list[p].name,
                                                sizeof(s_peers[slot].device_name) - 1);
                                        s_peers[slot].equipment_type = (equipment_type_t)paired_list[p].equipment_type;
                                        break;
                                    }
                                }
                            }
                        }
                        if (s_peers[slot].device_name[0] == '\0') {
                            strncpy(s_peers[slot].device_name, "Grainfather G30 Connect", sizeof(s_peers[slot].device_name) - 1);
                            s_peers[slot].equipment_type = EQUIPMENT_TYPE_BOILER;
                        }

                        connected_peer = s_peers[slot];
                        has_connected_peer = true;
                    } else {
                        ESP_LOGW(TAG, "No free peer slot available for conn_handle %d", event->connect.conn_handle);
                    }
                    xSemaphoreGive(s_ble_mutex);
                }

                // Immediately notify lifecycle callback so equipment slot & NVS pairing are persisted right away
                if (has_connected_peer && s_peer_cb != NULL) {
                    s_peer_cb(&connected_peer, true);
                }

                // Exchange MTU
                ble_gattc_exchange_mtu(event->connect.conn_handle, ble_on_mtu, NULL);
            } else {
                ESP_LOGE(TAG, "BLE connection establishment failed: status=%d", event->connect.status);
            }
            return 0;
        }

        case BLE_GAP_EVENT_DISCONNECT: {
            s_is_connecting = false;
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
                    s_svc_start_handles[slot] = 0;
                    s_svc_end_handles[slot] = 0;
                }
                xSemaphoreGive(s_ble_mutex);
            }

            if (had_peer) {
                purge_queued_commands_for_mac(disc_peer.addr, true);
                if (s_peer_cb != NULL) {
                    s_peer_cb(&disc_peer, false);
                }
            }

            // Immediately start auto-reconnect scan if paired equipment is now offline
            if (s_ble_synced && !ble_gap_disc_active() && !ble_gap_conn_active() && has_disconnected_paired_devices()) {
                ESP_LOGI(TAG, "Starting immediate auto-reconnect scan after disconnect...");
                ble_manager_start_scan(5);
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
    s_ble_synced = true;
    ESP_LOGI(TAG, "NimBLE host synchronized. Multi-central BLE ready.");
    if (has_disconnected_paired_devices()) {
        ESP_LOGI(TAG, "Paired BLE equipment found in NVS; initiating power-on auto-reconnect scan...");
        ble_manager_start_scan(15);
    }
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

esp_err_t ble_manager_register_seq_cb(ble_cmd_seq_complete_cb_t cb) {
    s_seq_cb = cb;
    return ESP_OK;
}

esp_err_t ble_manager_start_scan(uint32_t duration_sec) {
    if (ble_gap_disc_active() || s_is_scanning) {
        ble_gap_disc_cancel();
        s_is_scanning = false;
    }

    if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_scan_count = 0;
        memset(s_scan_entries, 0, sizeof(s_scan_entries));
        xSemaphoreGive(s_ble_mutex);
    }

    struct ble_gap_disc_params disc_params = {
        .filter_duplicates = 0,
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

    if (!compat) {
        paired_ble_device_t paired_list[CONFIG_STORE_MAX_PAIRED_BLE];
        if (config_store_get_paired_ble(paired_list) == ESP_OK) {
            for (uint8_t p = 0; p < CONFIG_STORE_MAX_PAIRED_BLE; p++) {
                if (paired_list[p].active && memcmp(paired_list[p].addr, addr, 6) == 0) {
                    compat = true;
                    break;
                }
            }
        }
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

    s_is_connecting = true;
    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &target_addr, 30000, NULL, ble_gap_event, NULL);
    if (rc != 0) {
        s_is_connecting = false;
        ESP_LOGE(TAG, "ble_gap_connect failed: rc=%d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ble_manager_disconnect(uint16_t conn_handle) {
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t peer_addr[6] = {0};
    bool has_addr = false;
    if (xSemaphoreTake(s_ble_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        int slot = find_peer_by_handle(conn_handle);
        if (slot >= 0) {
            memcpy(peer_addr, s_peers[slot].addr, 6);
            has_addr = true;
        }
        xSemaphoreGive(s_ble_mutex);
    }
    if (has_addr) {
        config_store_remove_paired_ble(peer_addr);
    }

    ESP_LOGI(TAG, "Terminating BLE connection handle %d...", conn_handle);
    int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_manager_disconnect_by_addr(const uint8_t *addr) {
    if (addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Explicitly unpair from NVS so it does not auto-reconnect
    config_store_remove_paired_ble(addr);

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

    return ESP_OK;
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
            int8_t live_rssi = 0;
            if (ble_gap_conn_rssi(s_peers[i].conn_handle, &live_rssi) == 0 && live_rssi != 0) {
                s_peers[i].rssi = live_rssi;
            }
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
