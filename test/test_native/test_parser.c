#include <unity.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "g30_schema.h"
#include "grainfather_g30.h"
#include "ble_compatibility.h"
#include "auth_manager.h"
#include "equipment_manager.h"

// Include source directly for host testing harness
#include "../../src/grainfather_g30.c"
#include "../../src/ble_compatibility.c"
#include "../../src/auth_manager.c"
#include "../../src/equipment_manager.c"
#include "wifi_manager.h"
#include "../../src/wifi_manager.c"

void setUp(void) {
    // Set up before each test
}

void tearDown(void) {
    // Clean up after each test
}

// =============================================================================
// Existing Baseline Tests
// =============================================================================
void test_telemetry_packet_structure(void) {
    ble_telemetry_packet_t packet;
    memset(&packet, 0, sizeof(packet));

    packet.device_index = 1;
    packet.rssi = -65;
    packet.len = 4;
    packet.data[0] = 0xAA;
    packet.data[1] = 0xBB;
    packet.data[2] = 0xCC;
    packet.data[3] = 0xDD;

    TEST_ASSERT_EQUAL_UINT8(1, packet.device_index);
    TEST_ASSERT_EQUAL_INT8(-65, packet.rssi);
    TEST_ASSERT_EQUAL_UINT32(4, packet.len);
    TEST_ASSERT_EQUAL_HEX8(0xAA, packet.data[0]);
}

void test_max_simultaneous_connections_limit(void) {
    TEST_ASSERT_GREATER_OR_EQUAL_INT(3, BLE_MAX_SIMULTANEOUS_CONNECTIONS);
}

void test_esp_idf_version_constraint(void) {
    TEST_ASSERT_EQUAL_INT(6, BREWLINK_REQUIRED_IDF_MAJOR);
    TEST_ASSERT_EQUAL_INT(1, BREWLINK_REQUIRED_IDF_MINOR);
}

void test_ble_peer_and_scan_structures(void) {
    ble_peer_device_t peer;
    memset(&peer, 0, sizeof(peer));
    peer.conn_handle = 1;
    peer.addr_type = 0;
    peer.rssi = -70;
    peer.connected = true;
    peer.equipment_type = EQUIPMENT_TYPE_BOILER;
    strncpy(peer.device_name, "Grainfather G30", sizeof(peer.device_name) - 1);

    TEST_ASSERT_EQUAL_UINT16(1, peer.conn_handle);
    TEST_ASSERT_EQUAL_INT8(-70, peer.rssi);
    TEST_ASSERT_TRUE(peer.connected);
    TEST_ASSERT_EQUAL_STRING("Grainfather G30", peer.device_name);

    ble_scan_entry_t scan;
    memset(&scan, 0, sizeof(scan));
    scan.is_grainfather = true;
    scan.is_compatible = true;
    scan.equipment_type = EQUIPMENT_TYPE_BOILER;
    strncpy(scan.name, "Grainfather", sizeof(scan.name) - 1);
    strncpy(scan.model, "Grainfather G30 Connect", sizeof(scan.model) - 1);
    TEST_ASSERT_TRUE(scan.is_grainfather);
    TEST_ASSERT_TRUE(scan.is_compatible);
    TEST_ASSERT_EQUAL_STRING("Grainfather", scan.name);
    TEST_ASSERT_EQUAL_STRING("Grainfather G30 Connect", scan.model);
}

void test_wifi_ap_configuration_defines(void) {
    TEST_ASSERT_EQUAL_STRING("BrewLink-Setup", WIFI_AP_SSID_PREFIX);
    TEST_ASSERT_EQUAL_INT(1, WIFI_AP_CHANNEL);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, WIFI_AP_MAX_CONN);
    TEST_ASSERT_EQUAL_INT(80, WEB_SERVER_PORT);
}

void test_wifi_scan_deduplicate_keeps_strongest_rssi(void) {
    wifi_mgr_ap_record_t in_aps[5] = {
        {"MyMeshHome", -75, 3},
        {"NeighborWiFi", -80, 2},
        {"MyMeshHome", -42, 4},
        {"MyMeshHome", -60, 3},
        {"NeighborWiFi", -65, 2}
    };
    wifi_mgr_ap_record_t out_aps[10];
    memset(out_aps, 0, sizeof(out_aps));

    uint16_t count = wifi_manager_deduplicate_ap_records(in_aps, 5, out_aps, 10);
    TEST_ASSERT_EQUAL_UINT16(2, count);

    // Sorted descending by RSSI: -42 dBm ("MyMeshHome") before -65 dBm ("NeighborWiFi")
    TEST_ASSERT_EQUAL_STRING("MyMeshHome", out_aps[0].ssid);
    TEST_ASSERT_EQUAL_INT8(-42, out_aps[0].rssi);
    TEST_ASSERT_EQUAL_UINT8(4, out_aps[0].authmode);

    TEST_ASSERT_EQUAL_STRING("NeighborWiFi", out_aps[1].ssid);
    TEST_ASSERT_EQUAL_INT8(-65, out_aps[1].rssi);
    TEST_ASSERT_EQUAL_UINT8(2, out_aps[1].authmode);
}

void test_wifi_scan_deduplicate_filters_empty_hidden_ssids(void) {
    wifi_mgr_ap_record_t in_aps[4] = {
        {"", -30, 3},
        {"ValidNetwork", -70, 3},
        {"", -50, 3},
        {"\0", -40, 0}
    };
    wifi_mgr_ap_record_t out_aps[5];
    memset(out_aps, 0, sizeof(out_aps));

    uint16_t count = wifi_manager_deduplicate_ap_records(in_aps, 4, out_aps, 5);
    TEST_ASSERT_EQUAL_UINT16(1, count);
    TEST_ASSERT_EQUAL_STRING("ValidNetwork", out_aps[0].ssid);
    TEST_ASSERT_EQUAL_INT8(-70, out_aps[0].rssi);
}

void test_wifi_scan_deduplicate_sorts_descending_by_rssi(void) {
    wifi_mgr_ap_record_t in_aps[4] = {
        {"Weakest", -90, 1},
        {"Strongest", -35, 3},
        {"Medium", -65, 2},
        {"SecondStrongest", -45, 3}
    };
    wifi_mgr_ap_record_t out_aps[10];
    memset(out_aps, 0, sizeof(out_aps));

    uint16_t count = wifi_manager_deduplicate_ap_records(in_aps, 4, out_aps, 10);
    TEST_ASSERT_EQUAL_UINT16(4, count);

    TEST_ASSERT_EQUAL_STRING("Strongest", out_aps[0].ssid);
    TEST_ASSERT_EQUAL_INT8(-35, out_aps[0].rssi);

    TEST_ASSERT_EQUAL_STRING("SecondStrongest", out_aps[1].ssid);
    TEST_ASSERT_EQUAL_INT8(-45, out_aps[1].rssi);

    TEST_ASSERT_EQUAL_STRING("Medium", out_aps[2].ssid);
    TEST_ASSERT_EQUAL_INT8(-65, out_aps[2].rssi);

    TEST_ASSERT_EQUAL_STRING("Weakest", out_aps[3].ssid);
    TEST_ASSERT_EQUAL_INT8(-90, out_aps[3].rssi);
}

void test_wifi_scan_deduplicate_handles_null_and_zero_inputs(void) {
    wifi_mgr_ap_record_t aps[2] = {{"Net", -50, 1}, {"Net2", -60, 1}};
    wifi_mgr_ap_record_t out[2];

    TEST_ASSERT_EQUAL_UINT16(0, wifi_manager_deduplicate_ap_records(NULL, 2, out, 2));
    TEST_ASSERT_EQUAL_UINT16(0, wifi_manager_deduplicate_ap_records(aps, 2, NULL, 2));
    TEST_ASSERT_EQUAL_UINT16(0, wifi_manager_deduplicate_ap_records(aps, 0, out, 2));
    TEST_ASSERT_EQUAL_UINT16(0, wifi_manager_deduplicate_ap_records(aps, 2, out, 0));
}

void test_wifi_scan_deduplicate_respects_max_output_limit(void) {
    wifi_mgr_ap_record_t in_aps[5] = {
        {"Net1", -85, 1},
        {"Net2", -40, 2},
        {"Net3", -90, 1},
        {"Net4", -50, 3},
        {"Net5", -30, 2}
    };
    wifi_mgr_ap_record_t out_aps[3];
    memset(out_aps, 0, sizeof(out_aps));

    uint16_t count = wifi_manager_deduplicate_ap_records(in_aps, 5, out_aps, 3);
    TEST_ASSERT_EQUAL_UINT16(3, count);

    // The top 3 strongest unique networks must be retained and sorted
    TEST_ASSERT_EQUAL_STRING("Net5", out_aps[0].ssid);
    TEST_ASSERT_EQUAL_INT8(-30, out_aps[0].rssi);

    TEST_ASSERT_EQUAL_STRING("Net2", out_aps[1].ssid);
    TEST_ASSERT_EQUAL_INT8(-40, out_aps[1].rssi);

    TEST_ASSERT_EQUAL_STRING("Net4", out_aps[2].ssid);
    TEST_ASSERT_EQUAL_INT8(-50, out_aps[2].rssi);
}

// =============================================================================
// Grainfather G30 Notification Parsing Tests (BLE -> Gateway / WiFi)
// =============================================================================
void test_g30_parse_temperature_notification(void) {
    g30_state_t state;
    memset(&state, 0, sizeof(state));

    const uint8_t notif_data[] = "X65.0,64.8";
    g30_notification_type_t type = G30_NOTIF_UNKNOWN;

    esp_err_t err = g30_parse_notification(notif_data, strlen((char*)notif_data), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(G30_NOTIF_TEMPERATURE, type);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 65.0f, state.target_temp_c);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 64.8f, state.current_temp_c);
}

void test_g30_parse_system_state_notification(void) {
    g30_state_t state;
    memset(&state, 0, sizeof(state));

    // Y{Heat},{Pump},{Auto},{Ramp},{InterMode},{InterCode},{Stage},{Delayed}
    const uint8_t notif_data[] = "Y1,1,1,0,1,3,3,0";
    g30_notification_type_t type = G30_NOTIF_UNKNOWN;

    esp_err_t err = g30_parse_notification(notif_data, strlen((char*)notif_data), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(G30_NOTIF_SYSTEM_STATE, type);
    TEST_ASSERT_TRUE(state.heat_active);
    TEST_ASSERT_TRUE(state.pump_active);
    TEST_ASSERT_TRUE(state.auto_mode);
    TEST_ASSERT_FALSE(state.ramp_active);
    TEST_ASSERT_TRUE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(3, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Mash step finished: Sparge alert", state.interaction.message);
    TEST_ASSERT_EQUAL_UINT8(3, state.stage.step_index);
}

void test_g30_parse_timer_notification(void) {
    g30_state_t state;
    memset(&state, 0, sizeof(state));

    // T{Active},{LeftMin},{TotalMin},{LeftSec}
    const uint8_t notif_data[] = "T1,45,60,2700";
    g30_notification_type_t type = G30_NOTIF_UNKNOWN;

    esp_err_t err = g30_parse_notification(notif_data, strlen((char*)notif_data), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(G30_NOTIF_TIMER, type);
    TEST_ASSERT_TRUE(state.timer.active);
    TEST_ASSERT_EQUAL_UINT32(2700, state.timer.remaining_seconds);
    TEST_ASSERT_EQUAL_UINT32(3600, state.timer.total_seconds);
}

void test_g30_parse_power_notification(void) {
    g30_state_t state;
    memset(&state, 0, sizeof(state));

    // W{PwrPct},{Paused},{StepMash},{Interrupted},{ManualPwr},{SpargeAlert}
    const uint8_t notif_data[] = "W85,1,1,0,0,0";
    g30_notification_type_t type = G30_NOTIF_UNKNOWN;

    esp_err_t err = g30_parse_notification(notif_data, strlen((char*)notif_data), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(G30_NOTIF_POWER_FLAGS, type);
    TEST_ASSERT_EQUAL_UINT8(85, state.heat_power_pct);
    TEST_ASSERT_TRUE(state.timer.paused);
}

void test_g30_parse_interaction_notification(void) {
    g30_state_t state;
    memset(&state, 0, sizeof(state));

    // I1 is ALWAYS "Add mash water & Press SET to start heating", even when current temp (25C) is within 1C of target (26C)
    state.target_temp_c = 26.0f;
    state.current_temp_c = 25.0f;
    const uint8_t notif_water[] = "I1";
    g30_notification_type_t type = G30_NOTIF_UNKNOWN;

    esp_err_t err = g30_parse_notification(notif_water, strlen((char*)notif_water), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(G30_NOTIF_INTERACTION, type);
    TEST_ASSERT_TRUE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(1, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Add mash water & Press SET to start heating", state.interaction.message);
    TEST_ASSERT_EQUAL_STRING("Add Strike Water", state.stage.step_name);

    // I2 is "Strike temperature reached: Add grain" with "Add Grain" badge
    state.current_temp_c = 26.0f;
    const uint8_t notif_grain[] = "I2";
    err = g30_parse_notification(notif_grain, strlen((char*)notif_grain), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_UINT8(2, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Strike temperature reached: Add grain", state.interaction.message);
    TEST_ASSERT_EQUAL_STRING("Add Grain", state.stage.step_name);

    // I3 is "Mash step finished: Sparge alert" with "Sparge" badge
    const uint8_t notif_sparge[] = "I3";
    err = g30_parse_notification(notif_sparge, strlen((char*)notif_sparge), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_UINT8(3, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Mash step finished: Sparge alert", state.interaction.message);
    TEST_ASSERT_EQUAL_STRING("Sparge", state.stage.step_name);
}

void test_g30_parse_boil_temp_notification(void) {
    g30_state_t state;
    memset(&state, 0, sizeof(state));

    const uint8_t notif_data[] = "C99.5";
    g30_notification_type_t type = G30_NOTIF_UNKNOWN;

    esp_err_t err = g30_parse_notification(notif_data, strlen((char*)notif_data), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(G30_NOTIF_BOIL_TEMP, type);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 99.5f, state.boil_temp_c);
}

// =============================================================================
// Grainfather G30 Command Encoding Tests (WiFi / Gateway -> BLE)
// =============================================================================
void test_g30_format_padded_cmd_exact_19_bytes(void) {
    char buf[G30_COMMAND_LEN + 1];
    esp_err_t err = g30_format_padded_cmd("P", buf);

    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_CHAR('P', buf[0]);
    for (int i = 1; i < 19; i++) {
        TEST_ASSERT_EQUAL_CHAR(' ', buf[i]);
    }
}

void test_g30_encode_pump_command(void) {
    char buf[G30_COMMAND_LEN + 1];
    g30_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = G30_CMD_ACTION_SET_PUMP;

    // Pump ON -> "L1" + 17 spaces
    cmd.params.pump_state = true;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("L1                 ", buf);

    // Pump OFF -> "L0" + 17 spaces
    cmd.params.pump_state = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("L0                 ", buf);
}

void test_g30_encode_heat_command(void) {
    char buf[G30_COMMAND_LEN + 1];
    g30_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = G30_CMD_ACTION_SET_HEAT;

    // Heat ON -> "K1" + 17 spaces
    cmd.params.heat.heat_state = true;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("K1                 ", buf);

    // Heat OFF -> "K0" + 17 spaces
    cmd.params.heat.heat_state = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("K0                 ", buf);
}

void test_g30_encode_target_temperature(void) {
    char buf[G30_COMMAND_LEN + 1];
    g30_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = G30_CMD_ACTION_SET_TARGET_TEMP;

    // Integer temp 65 -> "$65," + 15 spaces
    cmd.params.target_temp_c = 65.0f;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("$65,               ", buf);

    // Decimal temp 67.5 -> "$67.5," + 13 spaces
    cmd.params.target_temp_c = 67.5f;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("$67.5,             ", buf);
}

void test_g30_encode_timer_commands(void) {
    char buf[G30_COMMAND_LEN + 1];
    g30_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    // Set timer 60 min -> "S60" + 16 spaces
    cmd.action = G30_CMD_ACTION_SET_TIMER;
    cmd.params.timer_duration_min = 60;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("S60                ", buf);

    // Pause / Resume -> "G" + 18 spaces
    cmd.action = G30_CMD_ACTION_PAUSE_TIMER;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("G                  ", buf);

    // Cancel timer -> "C" + 18 spaces
    cmd.action = G30_CMD_ACTION_CANCEL_TIMER;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("C                  ", buf);
}

void test_g30_encode_extended_commands(void) {
    char buf[G30_COMMAND_LEN + 1];
    g30_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    cmd.action = G30_CMD_ACTION_TEMP_UP;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("U                  ", buf);

    cmd.action = G30_CMD_ACTION_TEMP_DOWN;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("D                  ", buf);

    cmd.action = G30_CMD_ACTION_DELAYED_HEAT;
    cmd.params.delayed_heat.minutes = 30;
    cmd.params.delayed_heat.seconds = 15;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("B30,15,            ", buf);

    cmd.action = G30_CMD_ACTION_DISCONNECT_MANUAL;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("Q0                 ", buf);

    cmd.action = G30_CMD_ACTION_DISCONNECT_AUTO;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("Q2                 ", buf);

    cmd.action = G30_CMD_ACTION_TOGGLE_TIMER;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("G                  ", buf);
}

void test_g30_parse_extended_state_flags(void) {
    g30_state_t state;
    g30_notification_type_t type;
    memset(&state, 0, sizeof(state));

    const char *y = "Y0,0,0,0,0,0,1,1";
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t *)y, strlen(y), &state, &type));
    TEST_ASSERT_TRUE(state.delayed_heat);

    const char *w = "W50,0,1,1,1,1";
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t *)w, strlen(w), &state, &type));
    TEST_ASSERT_EQUAL_UINT8(50, state.heat_power_pct);
    TEST_ASSERT_TRUE(state.interrupted);
    TEST_ASSERT_TRUE(state.manual_power);
    TEST_ASSERT_TRUE(state.sparge_alert);

    const char *e = "E7";
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t *)e, strlen(e), &state, &type));
    TEST_ASSERT_EQUAL_UINT8(7, state.error_code);
}
void test_g30_encode_recipe_commands(void) {
    char buf[G30_COMMAND_LEN + 1];
    g30_recipe_t recipe;
    memset(&recipe, 0, sizeof(recipe));
    recipe.boil_time_min = 75;
    recipe.mash_step_count = 2;
    recipe.mash_water_l = 16.5f;
    recipe.sparge_water_l = 14.0f;

    // Line 1: Header -> "R75,2,16.5,14.0,   " (19 bytes)
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_header(&recipe, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("R75,2,16.5,14.0,   ", buf);

    // Line 2: Recipe Flags -> "0,1,1,0,0,         "
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_flags(buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("0,1,1,0,0,         ", buf);

    // Line 3: Recipe Name -> "SAISON             "
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_name("Saison", buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("SAISON             ", buf);

    // Line 4: Boil Params -> "0,2,0,0,           "
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_boil_params(0, 2, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("0,2,0,0,           ", buf);

    // Line 5: Boil stop at 15 min remaining -> "15,                "
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_boil_stop(15, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("15,                ", buf);

    // Mash Step 1: 65C for 60 min -> "65:60              "
    g30_mash_step_t step1 = {.temp_c = 65.0f, .duration_min = 60};
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_mash_step(&step1, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("65:60              ", buf);
}

// =============================================================================
// Digital Twin Integration & Topic Hierarchy Tests
// =============================================================================
void test_mqtt_topic_hierarchy_definitions(void) {
    TEST_ASSERT_EQUAL_STRING("brewlink/", MQTT_TOPIC_GATEWAY_PREFIX);
    TEST_ASSERT_EQUAL_STRING("/status", MQTT_TOPIC_STATUS_SUFFIX);
    TEST_ASSERT_EQUAL_STRING("/equipment/", MQTT_TOPIC_EQUIPMENT_SUBDIR);
    TEST_ASSERT_EQUAL_STRING("/announce", MQTT_TOPIC_ANNOUNCE_SUFFIX);
    TEST_ASSERT_EQUAL_STRING("/telemetry", MQTT_TOPIC_TELEMETRY_SUFFIX);
    TEST_ASSERT_EQUAL_STRING("/snapshot", MQTT_TOPIC_SNAPSHOT_SUFFIX);
    TEST_ASSERT_EQUAL_STRING("/cmd", MQTT_TOPIC_CMD_SUFFIX);
    TEST_ASSERT_EQUAL_STRING("/cmd_ack", MQTT_TOPIC_CMD_ACK_SUFFIX);

    TEST_ASSERT_EQUAL_INT(0, MQTT_QOS_TELEMETRY);
    TEST_ASSERT_EQUAL_INT(1, MQTT_QOS_STATUS);
    TEST_ASSERT_EQUAL_INT(1, MQTT_QOS_COMMAND);
    TEST_ASSERT_EQUAL_INT(1, MQTT_QOS_SNAPSHOT);
    TEST_ASSERT_EQUAL_INT(1, MQTT_QOS_ANNOUNCE);
}

void test_telemetry_cadence_definitions(void) {
    TEST_ASSERT_EQUAL_UINT32(1000, TELEMETRY_CADENCE_THROTTLE_MS);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.2f, TELEMETRY_TEMP_DELTA_MIN_C);
}

// =============================================================================
// BLE Compatible Devices Sourcecode Validation Tests
// =============================================================================
void test_compatible_devices_registry_defined(void) {
    size_t count = 0;
    const ble_compatible_device_t *list = ble_compatibility_get_definitions(&count);

    TEST_ASSERT_NOT_NULL(list);
    // G30 is the sole compatible equipment in focus
    TEST_ASSERT_EQUAL_UINT(1, count);

    // Verify sole device is Grainfather G30 Connect
    TEST_ASSERT_EQUAL_STRING("Grainfather G30 Connect", list[0].model_name);
    TEST_ASSERT_EQUAL_INT(EQUIPMENT_TYPE_BOILER, list[0].equipment_type);
    TEST_ASSERT_EQUAL_STRING("Grain", list[0].name_prefix);
    TEST_ASSERT_EQUAL_HEX16(0xCDD0, list[0].svc_uuid16);
    TEST_ASSERT_TRUE(list[0].requires_pacing);
    TEST_ASSERT_EQUAL_UINT16(150, list[0].pacing_delay_ms);
}

void test_compatibility_match_grainfather_names(void) {
    const ble_compatible_device_t *match = NULL;

    TEST_ASSERT_TRUE(ble_compatibility_match_by_name("Grain", &match));
    TEST_ASSERT_NOT_NULL(match);
    TEST_ASSERT_EQUAL_STRING("Grainfather G30 Connect", match->model_name);

    match = NULL;
    TEST_ASSERT_TRUE(ble_compatibility_match_by_name("Grainfather", &match));
    TEST_ASSERT_NOT_NULL(match);
    TEST_ASSERT_EQUAL_STRING("Grainfather G30 Connect", match->model_name);

    match = NULL;
    TEST_ASSERT_TRUE(ble_compatibility_match_by_name("grain_cdd0", &match));
    TEST_ASSERT_NOT_NULL(match);
    TEST_ASSERT_EQUAL_STRING("Grainfather G30 Connect", match->model_name);

    match = NULL;
    TEST_ASSERT_TRUE(ble_compatibility_match_by_name("GRAINFATHER_G30", &match));
    TEST_ASSERT_NOT_NULL(match);
    TEST_ASSERT_EQUAL_STRING("Grainfather G30 Connect", match->model_name);
}

void test_compatibility_match_by_service_uuids(void) {
    const ble_compatible_device_t *match = NULL;

    // 16-bit 0xCDD0
    TEST_ASSERT_TRUE(ble_compatibility_match_by_uuid16(0xCDD0, &match));
    TEST_ASSERT_NOT_NULL(match);
    TEST_ASSERT_EQUAL_STRING("Grainfather G30 Connect", match->model_name);

    // 128-bit little endian 0000cdd0-0000-1000-8000-00805f9b34fb
    const uint8_t g30_u128[16] = {
        0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x10, 0x00, 0x00, 0xd0, 0xcd, 0x00, 0x00
    };
    match = NULL;
    TEST_ASSERT_TRUE(ble_compatibility_match_by_uuid128(g30_u128, &match));
    TEST_ASSERT_NOT_NULL(match);
    TEST_ASSERT_EQUAL_STRING("Grainfather G30 Connect", match->model_name);
}

void test_compatibility_rejects_non_g30_equipment(void) {
    const ble_compatible_device_t *match = NULL;

    // G30 is the sole compatible equipment in focus; all other brands are rejected
    TEST_ASSERT_FALSE(ble_compatibility_match_by_name("BrewZilla 35L", &match));
    TEST_ASSERT_FALSE(ble_compatibility_match_by_name("Tilt Red", &match));
    TEST_ASSERT_FALSE(ble_compatibility_match_by_name("Plaato_01", &match));
    TEST_ASSERT_FALSE(ble_compatibility_match_by_name("GF_GC_Chiller", &match));
    TEST_ASSERT_FALSE(ble_compatibility_match_by_name("RAPT_Chamber", &match));
}

void test_compatibility_rejects_incompatible_devices(void) {
    const ble_compatible_device_t *match = (const ble_compatible_device_t *)0x1234;

    // Random non-brewing devices must NOT match
    TEST_ASSERT_FALSE(ble_compatibility_match_by_name("iPhone (Alex)", &match));
    TEST_ASSERT_FALSE(ble_compatibility_match_by_name("Apple Watch", &match));
    TEST_ASSERT_FALSE(ble_compatibility_match_by_name("Samsung Smart TV", &match));
    TEST_ASSERT_FALSE(ble_compatibility_match_by_name("Fitbit Charge 5", &match));
    TEST_ASSERT_FALSE(ble_compatibility_match_by_name("JBL Flip 6", &match));
    TEST_ASSERT_FALSE(ble_compatibility_match_by_name("BLE-1234", &match));
    TEST_ASSERT_FALSE(ble_compatibility_match_by_name("", &match));
    TEST_ASSERT_FALSE(ble_compatibility_match_by_name(NULL, &match));

    // Incompatible 16-bit UUIDs
    TEST_ASSERT_FALSE(ble_compatibility_match_by_uuid16(0x180D, &match)); // Heart Rate Service
    TEST_ASSERT_FALSE(ble_compatibility_match_by_uuid16(0x180F, &match)); // Battery Service
    TEST_ASSERT_FALSE(ble_compatibility_match_by_uuid16(0, &match));

    // Incompatible 128-bit UUID
    const uint8_t random_u128[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    TEST_ASSERT_FALSE(ble_compatibility_match_by_uuid128(random_u128, &match));
    TEST_ASSERT_FALSE(ble_compatibility_match_by_uuid128(NULL, &match));
}

void test_compatibility_full_advertisement_check(void) {
    const ble_compatible_device_t *match = NULL;
    uint16_t u16_list[] = {0x180F, 0xCDD0};

    // Device with non-matching name but advertising G30 service UUID (0xCDD0) is recognized
    TEST_ASSERT_TRUE(ble_compatibility_is_device_compatible(
        "Generic BLE",
        u16_list, 2,
        NULL, 0,
        &match
    ));
    TEST_ASSERT_NOT_NULL(match);
    TEST_ASSERT_EQUAL_STRING("Grainfather G30 Connect", match->model_name);

    // Device with neither compatible name nor compatible UUID is rejected
    uint16_t non_brewing_u16[] = {0x180D, 0x180A};
    match = NULL;
    TEST_ASSERT_FALSE(ble_compatibility_is_device_compatible(
        "Smart Band",
        non_brewing_u16, 2,
        NULL, 0,
        &match
    ));
    TEST_ASSERT_NULL(match);
}

// =============================================================================
// G30 Web Controller & Recipe Conversion Validation Tests
// =============================================================================
void test_g30_web_recipe_conversion_and_encoding(void) {
    g30_recipe_t recipe;
    memset(&recipe, 0, sizeof(recipe));
    strncpy(recipe.name, "Newcastle Brown Ale", sizeof(recipe.name) - 1);
    recipe.boil_time_min = 60;
    recipe.mash_water_l = 14.4f;
    recipe.sparge_water_l = 9.8f;
    recipe.mash_step_count = 2;
    recipe.mash_steps[0].temp_c = 67.0f;
    recipe.mash_steps[0].duration_min = 60;
    recipe.mash_steps[1].temp_c = 76.0f;
    recipe.mash_steps[1].duration_min = 15;
    recipe.boil_addition_count = 2;
    recipe.boil_additions_min[0] = 60;
    recipe.boil_additions_min[1] = 20;

    char header[G30_COMMAND_LEN + 1];
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_header(&recipe, header));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(header));
    TEST_ASSERT_EQUAL_STRING("R60,2,14.4,9.8,    ", header);

    char flags[G30_COMMAND_LEN + 1];
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_flags(flags));
    TEST_ASSERT_EQUAL_STRING("0,1,1,0,0,         ", flags);

    char name[G30_COMMAND_LEN + 1];
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_name(recipe.name, name));
    TEST_ASSERT_EQUAL_STRING("NEWCASTLE BROWN ALE", name);

    char boil_params[G30_COMMAND_LEN + 1];
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_boil_params(0, recipe.boil_addition_count, boil_params));
    TEST_ASSERT_EQUAL_STRING("0,2,0,0,           ", boil_params);

    char boil1[G30_COMMAND_LEN + 1];
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_boil_stop(recipe.boil_additions_min[0], boil1));
    TEST_ASSERT_EQUAL_STRING("60,                ", boil1);

    char boil2[G30_COMMAND_LEN + 1];
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_boil_stop(recipe.boil_additions_min[1], boil2));
    TEST_ASSERT_EQUAL_STRING("20,                ", boil2);

    char step1[G30_COMMAND_LEN + 1];
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_mash_step(&recipe.mash_steps[0], step1));
    TEST_ASSERT_EQUAL_STRING("67:60              ", step1);

    char step2[G30_COMMAND_LEN + 1];
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_mash_step(&recipe.mash_steps[1], step2));
    TEST_ASSERT_EQUAL_STRING("76:15              ", step2);
}

void test_g30_web_control_actions_mapping(void) {
    char buf[G30_COMMAND_LEN + 1];
    g30_command_t cmd;

    // Pump control
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = G30_CMD_ACTION_SET_PUMP;
    cmd.params.pump_state = true;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("L1                 ", buf);

    cmd.params.pump_state = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("L0                 ", buf);

    // Heat control
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = G30_CMD_ACTION_SET_HEAT;
    cmd.params.heat.heat_state = true;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("K1                 ", buf);

    cmd.params.heat.heat_state = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("K0                 ", buf);

    // Set target temp decimal & integer
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = G30_CMD_ACTION_SET_TARGET_TEMP;
    cmd.params.target_temp_c = 65.5f;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("$65.5,             ", buf);

    cmd.params.target_temp_c = 100.0f;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("$100,              ", buf);

    // Acknowledge interaction prompt
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = G30_CMD_ACTION_ACK_INTERACTION;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("I                  ", buf);

    // Press SET command
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = G30_CMD_ACTION_PRESS_SET;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("T                  ", buf);

    // Pause/Resume timer command
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = G30_CMD_ACTION_PAUSE_TIMER;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("G                  ", buf);

    // Cancel timer command
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = G30_CMD_ACTION_CANCEL_TIMER;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("C                  ", buf);
}

void test_g30_parse_initial_strike_water_prompt(void) {
    g30_state_t state;
    memset(&state, 0, sizeof(state));
    // Reproduces Augustiner Lagerbier Hell Clone scenario: Current temp = 25C, Step 1 = 26C
    state.target_temp_c = 26.0f;
    state.current_temp_c = 25.0f;
    state.heat_active = false;
    state.stage.step_index = 1;

    // Y notification received at session start with inter_m=1, inter_c=1 (or inter_m=0, inter_c=0):
    const uint8_t notif_data[] = "Y0,0,1,0,1,1,1,0";
    g30_notification_type_t type = G30_NOTIF_UNKNOWN;

    esp_err_t err = g30_parse_notification(notif_data, strlen((char*)notif_data), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_TRUE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(1, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Add mash water & Press SET to start heating", state.interaction.message);
    TEST_ASSERT_EQUAL_STRING("Add Strike Water", state.stage.step_name);

    // User presses Acknowledge -> G30 displays "Mash 1" and heats to 26C (Stage 1, Ramp 1):
    const uint8_t ramp_notif[] = "Y1,0,1,1,0,0,1,0";
    err = g30_parse_notification(ramp_notif, strlen((char*)ramp_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_FALSE(state.interaction.required);
    TEST_ASSERT_EQUAL_STRING("Heating Strike Water", state.stage.step_name);

    // When temperature reaches 26C, G30 alarms to add grain with InterMode=1, InterCode=2, Stage=1:
    const uint8_t temp_notif[] = "X26.0,26.0";
    err = g30_parse_notification(temp_notif, strlen((char*)temp_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);

    const uint8_t strike_alert[] = "Y1,0,1,0,1,2,1,0";
    err = g30_parse_notification(strike_alert, strlen((char*)strike_alert), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_TRUE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(2, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Strike temperature reached: Add grain", state.interaction.message);
    TEST_ASSERT_EQUAL_STRING("Add Grain", state.stage.step_name);
}

void test_g30_stage_name_mapping(void) {
    g30_state_t state;
    memset(&state, 0, sizeof(state));
    g30_notification_type_t type = G30_NOTIF_UNKNOWN;

    // Stage 0 -> Idle
    const uint8_t idle_notif[] = "Y0,0,1,0,0,0,0,0";
    esp_err_t err = g30_parse_notification(idle_notif, strlen((char*)idle_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("Idle", state.stage.step_name);

    // Stage 1 with no heat/ramp/timer -> Add Strike Water
    state.target_temp_c = 26.0f;
    state.current_temp_c = 25.0f;
    const uint8_t strike_water_notif[] = "Y0,0,1,0,0,0,1,0";
    err = g30_parse_notification(strike_water_notif, strlen((char*)strike_water_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("Add Strike Water", state.stage.step_name);

    // Stage 1 with ramp active -> Heating Strike Water
    const uint8_t heat_strike_notif[] = "Y1,0,1,1,0,0,1,0";
    err = g30_parse_notification(heat_strike_notif, strlen((char*)heat_strike_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("Heating Strike Water", state.stage.step_name);

    // Stage 1 with InterCode = 2 -> Add Grain
    const uint8_t add_grain_notif[] = "Y1,0,1,0,1,2,1,0";
    err = g30_parse_notification(add_grain_notif, strlen((char*)add_grain_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("Add Grain", state.stage.step_name);
    TEST_ASSERT_EQUAL_STRING("Strike temperature reached: Add grain", state.interaction.message);

    // Stage 1 after Add Grain acknowledged (ramp=0, timer running) -> Mash Step 1
    state.target_temp_c = 26.0f;
    state.current_temp_c = 26.0f;
    const uint8_t mash1_notif[] = "Y1,1,1,0,0,0,1,0";
    err = g30_parse_notification(mash1_notif, strlen((char*)mash1_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("Mash Step 1", state.stage.step_name);

    // Stage 2 -> Mash Step 2
    state.target_temp_c = 28.0f;
    state.current_temp_c = 28.0f;
    const uint8_t mash2_notif[] = "Y1,1,1,0,0,0,2,0";
    err = g30_parse_notification(mash2_notif, strlen((char*)mash2_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("Mash Step 2", state.stage.step_name);

    // Stage 3 -> Mash Step 3
    state.target_temp_c = 30.0f;
    state.current_temp_c = 30.0f;
    const uint8_t mash3_notif[] = "Y1,1,1,0,0,0,3,0";
    err = g30_parse_notification(mash3_notif, strlen((char*)mash3_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("Mash Step 3", state.stage.step_name);

    // Sparge Alert (inter_c = 3) -> Sparge
    const uint8_t sparge_notif[] = "Y0,0,1,0,1,3,4,0";
    err = g30_parse_notification(sparge_notif, strlen((char*)sparge_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("Sparge", state.stage.step_name);
    TEST_ASSERT_EQUAL_STRING("Mash step finished: Sparge alert", state.interaction.message);

    // Boil heating / ramp (target 100C, ramp active) -> Heating to Boil
    state.target_temp_c = 100.0f;
    state.current_temp_c = 85.0f;
    state.boil_temp_c = 100.0f;
    const uint8_t boil_ramp_notif[] = "Y1,0,1,1,0,0,4,0";
    err = g30_parse_notification(boil_ramp_notif, strlen((char*)boil_ramp_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("Heating to Boil", state.stage.step_name);

    // Boil active (target 100C, ramp inactive) -> Boiling
    state.current_temp_c = 100.0f;
    const uint8_t boil_active_notif[] = "Y1,0,1,0,0,0,4,0";
    err = g30_parse_notification(boil_active_notif, strlen((char*)boil_active_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("Boiling", state.stage.step_name);

    // Cooling alert (inter_c = 6) -> Cooling
    const uint8_t cooling_notif[] = "Y0,0,1,0,1,6,5,0";
    err = g30_parse_notification(cooling_notif, strlen((char*)cooling_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("Cooling", state.stage.step_name);
    TEST_ASSERT_EQUAL_STRING("Boil finished: Start cooling", state.interaction.message);

    // Manual mode -> Manual Mode
    const uint8_t manual_notif[] = "Y1,1,0,0,0,0,0,0";
    err = g30_parse_notification(manual_notif, strlen((char*)manual_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("Manual Mode", state.stage.step_name);
}

void test_g30_parse_timer_real_protocol_minutes_and_seconds(void) {
    g30_state_t state;
    memset(&state, 0, sizeof(state));
    g30_notification_type_t type = G30_NOTIF_UNKNOWN;

    // 1. Timer start on G30: T1,60,60,59 -> 59m 59s (3599s) remaining out of 60m (3600s)
    const uint8_t t_start[] = "T1,60,60,59";
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification(t_start, strlen((char*)t_start), &state, &type));
    TEST_ASSERT_TRUE(state.timer.active);
    TEST_ASSERT_EQUAL_UINT32(3600, state.timer.total_seconds);
    TEST_ASSERT_EQUAL_UINT32(3599, state.timer.remaining_seconds);

    // 2. Mid-timer on G30: T1,15,60,30 -> 14m 30s (870s) remaining out of 60m (3600s)
    const uint8_t t_mid[] = "T1,15,60,30";
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification(t_mid, strlen((char*)t_mid), &state, &type));
    TEST_ASSERT_TRUE(state.timer.active);
    TEST_ASSERT_EQUAL_UINT32(3600, state.timer.total_seconds);
    TEST_ASSERT_EQUAL_UINT32(870, state.timer.remaining_seconds);

    // 3. Final minute on G30: T1,1,60,45 -> 0m 45s (45s) remaining
    const uint8_t t_final[] = "T1,1,60,45";
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification(t_final, strlen((char*)t_final), &state, &type));
    TEST_ASSERT_TRUE(state.timer.active);
    TEST_ASSERT_EQUAL_UINT32(3600, state.timer.total_seconds);
    TEST_ASSERT_EQUAL_UINT32(45, state.timer.remaining_seconds);

    // 4. Timer inactive: T0,0,0,0 -> 0s remaining, 0s total
    const uint8_t t_off[] = "T0,0,0,0";
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification(t_off, strlen((char*)t_off), &state, &type));
    TEST_ASSERT_FALSE(state.timer.active);
    TEST_ASSERT_EQUAL_UINT32(0, state.timer.total_seconds);
    TEST_ASSERT_EQUAL_UINT32(0, state.timer.remaining_seconds);
}

void test_g30_master_recipe_full_lifecycle(void) {
    g30_state_t state;
    memset(&state, 0, sizeof(state));
    state.boil_temp_c = 100.0f;
    g30_notification_type_t type = G30_NOTIF_UNKNOWN;

    // Step 1: Augustiner Lagerbier Hell Clone uploaded -> Current temp 25.0C, Step 1 target 26.0C
    // G30 displays "Add 15.9l water, press set to continue" (Stage 1, InterCode 1)
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"X26.0,25.0", 10, &state, &type));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"Y0,0,1,0,1,1,1,0", 16, &state, &type));
    TEST_ASSERT_TRUE(state.auto_mode);
    TEST_ASSERT_TRUE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(1, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Add Strike Water", state.stage.step_name);
    TEST_ASSERT_EQUAL_STRING("Add mash water & Press SET to start heating", state.interaction.message);

    // Step 2: User presses Acknowledge ('I') -> G30 displays "Mash 1" and heats to 26.0C (Y1,0,1,1,0,0,1,0)
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"Y1,0,1,1,0,0,1,0", 16, &state, &type));
    TEST_ASSERT_FALSE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(0, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Heating Strike Water", state.stage.step_name);
    TEST_ASSERT_EQUAL_STRING("Normal operation", state.interaction.message);

    // Step 3: Water heats through 25.8C while still ramping on G30:
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"X26.0,25.8", 10, &state, &type));
    TEST_ASSERT_FALSE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(0, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Heating Strike Water", state.stage.step_name);

    // Step 4: G30 reaches 26.0C and displays "Add Grain" (Stage 1, InterMode 1, InterCode 2)
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"X26.0,26.0", 10, &state, &type));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"Y1,0,1,0,1,2,1,0", 16, &state, &type));
    TEST_ASSERT_TRUE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(2, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Add Grain", state.stage.step_name);
    TEST_ASSERT_EQUAL_STRING("Strike temperature reached: Add grain", state.interaction.message);

    // Step 5: User acknowledges ('I') -> G30 runs Mash Step 1 timer (2 min at 26C, Stage 1)
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"Y1,1,1,0,0,0,1,0", 16, &state, &type));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"T1,2,2,59", 9, &state, &type));
    TEST_ASSERT_FALSE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(0, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Mash Step 1", state.stage.step_name);
    TEST_ASSERT_TRUE(state.timer.active);
    TEST_ASSERT_EQUAL_UINT32(119, state.timer.remaining_seconds);

    // Step 5b: Mash Step 1 timer crosses 1:00 -> 0:59 without restarting
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"T1,2,2,0", 8, &state, &type));
    TEST_ASSERT_EQUAL_UINT32(60, state.timer.remaining_seconds);
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"T1,1,2,59", 9, &state, &type));
    TEST_ASSERT_EQUAL_UINT32(59, state.timer.remaining_seconds);
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"T1,1,2,1", 8, &state, &type));
    TEST_ASSERT_EQUAL_UINT32(1, state.timer.remaining_seconds);

    // Step 6: Mash Step 1 completes -> G30 ramps to Mash Step 2 (28.0C for 1 min, Stage 2)
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"X28.0,26.2", 10, &state, &type));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"Y1,1,1,1,0,0,2,0", 16, &state, &type));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"T0,1,1,0", 8, &state, &type));
    TEST_ASSERT_FALSE(state.interaction.required);
    TEST_ASSERT_TRUE(state.ramp_active);
    TEST_ASSERT_FALSE(state.timer.active);
    TEST_ASSERT_EQUAL_STRING("Mash Step 2", state.stage.step_name);

    // Step 7: Mash Step 2 reaches 28.0C and G30 starts 1-min timer (Stage 2)
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"X28.0,28.0", 10, &state, &type));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"Y1,1,1,0,0,0,2,0", 16, &state, &type));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"T1,1,1,59", 9, &state, &type));
    TEST_ASSERT_FALSE(state.ramp_active);
    TEST_ASSERT_TRUE(state.timer.active);
    TEST_ASSERT_EQUAL_STRING("Mash Step 2", state.stage.step_name);
    TEST_ASSERT_EQUAL_UINT32(59, state.timer.remaining_seconds);

    // Step 7b: Mash Step 2 completes -> G30 ramps to Mash Step 3 (30.0C for 1 min, Stage 3)
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"X30.0,28.1", 10, &state, &type));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"Y1,1,1,1,0,0,3,0", 16, &state, &type));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"T0,1,1,0", 8, &state, &type));
    TEST_ASSERT_EQUAL_STRING("Mash Step 3", state.stage.step_name);

    // Step 7c: Mash Step 3 reaches 30.0C and runs 1-min timer (Stage 3)
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"X30.0,30.0", 10, &state, &type));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"Y1,1,1,0,0,0,3,0", 16, &state, &type));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"T1,1,1,59", 9, &state, &type));
    TEST_ASSERT_EQUAL_STRING("Mash Step 3", state.stage.step_name);

    // Step 8: Mash Step 3 (Mash Out) finishes -> G30 displays SPARGE with sparge counter & "Set to skip to boil" (InterCode 4)
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"Y0,0,1,0,1,4,4,0", 16, &state, &type));
    TEST_ASSERT_TRUE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(4, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Sparge", state.stage.step_name);
    TEST_ASSERT_EQUAL_STRING("Sparge: Add sparge water & Press SET to skip to boil", state.interaction.message);

    // Step 9: User presses SET to skip to boil -> G30 ramps to Boil (100.0C)
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"X100.0,82.0", 11, &state, &type));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"Y1,0,1,1,0,0,4,0", 16, &state, &type));
    TEST_ASSERT_FALSE(state.interaction.required);
    TEST_ASSERT_EQUAL_STRING("Heating to Boil", state.stage.step_name);

    // Step 10: Boil timer running & timed hop addition alert (InterCode 5)
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"X100.0,100.0", 12, &state, &type));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"Y1,0,1,0,0,0,4,0", 16, &state, &type));
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"T1,16,75,0", 10, &state, &type));
    TEST_ASSERT_FALSE(state.interaction.required);
    TEST_ASSERT_EQUAL_STRING("Boiling", state.stage.step_name);
    TEST_ASSERT_EQUAL_UINT32(900, state.timer.remaining_seconds);

    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"I5", 2, &state, &type));
    TEST_ASSERT_TRUE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(5, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Boil addition alert: Add timed hops/adjuncts", state.interaction.message);

    // Verify dismiss_alert ('A') and dismiss_sparge_alert ('V') command encodings
    char cmd_buf[G30_COMMAND_LEN + 1];
    g30_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = G30_CMD_ACTION_DISMISS_ALERT;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, cmd_buf));
    TEST_ASSERT_EQUAL_STRING("A                  ", cmd_buf);

    cmd.action = G30_CMD_ACTION_DISMISS_SPARGE_ALERT;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, cmd_buf));
    TEST_ASSERT_EQUAL_STRING("V                  ", cmd_buf);

    // Step 12: Boil finished -> G30 triggers Cooling prompt (InterCode 6)
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_parse_notification((const uint8_t*)"Y0,0,1,0,1,6,5,0", 16, &state, &type));
    TEST_ASSERT_TRUE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(6, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Cooling", state.stage.step_name);
    TEST_ASSERT_EQUAL_STRING("Boil finished: Start cooling", state.interaction.message);
}

// =============================================================================
// Pairing, Token Auth, and MQTT HMAC-SHA256 Tests
// =============================================================================
void test_auth_pin_pairing_and_token_verification(void) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_init());

    char pin[AUTH_PAIRING_PIN_LEN + 1] = {0};
    uint32_t ttl_sec = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_start_pairing(pin, &ttl_sec));
    TEST_ASSERT_EQUAL_UINT32(6, strlen(pin));
    TEST_ASSERT_EQUAL_UINT32(AUTH_PAIRING_WINDOW_SEC, ttl_sec);

    // Reject wrong PIN
    char token[CONFIG_STORE_TOKEN_MAX_LEN] = {0};
    char topic_key[CONFIG_STORE_TOPIC_KEY_MAX_LEN] = {0};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, auth_manager_claim_pin("000000", "TestClient", token, topic_key));

    // Claim valid PIN
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_claim_pin(pin, "BrewFather Bridge", token, topic_key));
    TEST_ASSERT_TRUE(strncmp(token, "bl_", 3) == 0);
    TEST_ASSERT_TRUE(strncmp(topic_key, "tk_", 3) == 0);

    // Verify Bearer token and raw token
    char bearer_hdr[128];
    snprintf(bearer_hdr, sizeof(bearer_hdr), "Bearer %s", token);
    TEST_ASSERT_TRUE(auth_manager_verify_token(bearer_hdr));
    TEST_ASSERT_TRUE(auth_manager_verify_token(token));
    TEST_ASSERT_FALSE(auth_manager_verify_token("bl_invalid_token_12345"));
}

void test_auth_code_free_push_button_pairing_and_discovery_beacon(void) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_init());

    // Before pairing window opens, discovery beacon fails with ESP_ERR_INVALID_STATE
    char beacon[384] = {0};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, auth_manager_format_discovery_beacon("C44F331A8B20", "192.168.1.50", 80, beacon, sizeof(beacon)));

    // Open 60-second push-button pairing window
    char pin[AUTH_PAIRING_PIN_LEN + 1] = {0};
    uint32_t ttl_sec = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_start_pairing(pin, &ttl_sec));
    TEST_ASSERT_EQUAL_UINT32(AUTH_PAIRING_WINDOW_SEC, ttl_sec);

    // Format discovery broadcast beacon (UDP :53540 / MQTT brewlink/discovery/<gw_id>)
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_format_discovery_beacon("C44F331A8B20", "192.168.1.50", 80, beacon, sizeof(beacon)));
    TEST_ASSERT_NOT_NULL(strstr(beacon, "\"event\":\"brewlink_pairing\""));
    TEST_ASSERT_NOT_NULL(strstr(beacon, "\"gateway_id\":\"C44F331A8B20\""));
    TEST_ASSERT_NOT_NULL(strstr(beacon, "\"claim_url\":\"http://192.168.1.50/api/pair/claim\""));
    TEST_ASSERT_NOT_NULL(strstr(beacon, "\"ws_url\":\"ws://192.168.1.50/ws\""));

    // Claim pairing WITHOUT a PIN (code-free physical push-button claim: pin == NULL)
    char token[CONFIG_STORE_TOKEN_MAX_LEN] = {0};
    char topic_key[CONFIG_STORE_TOPIC_KEY_MAX_LEN] = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_claim_pin(NULL, "Headless External Client", token, topic_key));
    TEST_ASSERT_TRUE(strncmp(token, "bl_", 3) == 0);
    TEST_ASSERT_TRUE(strncmp(topic_key, "tk_", 3) == 0);
    TEST_ASSERT_TRUE(auth_manager_verify_token(token));

    // Single-claim lockout: second claim attempt immediately fails because window closed on first claim
    char token2[CONFIG_STORE_TOKEN_MAX_LEN] = {0};
    char topic_key2[CONFIG_STORE_TOPIC_KEY_MAX_LEN] = {0};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, auth_manager_claim_pin(NULL, "Attacker Client", token2, topic_key2));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, auth_manager_format_discovery_beacon("C44F331A8B20", "192.168.1.50", 80, beacon, sizeof(beacon)));
}

void test_auth_hmac_sha256_and_mqtt_command_signing(void) {
    // RFC 4231 Test Case 2: Key = "Jefe", Data = "what do ya want for nothing?"
    const char *key = "Jefe";
    const char *msg = "what do ya want for nothing?";
    char hex_out[AUTH_HMAC_HEX_LEN + 1] = {0};

    esp_err_t err = auth_manager_compute_hmac_sha256_hex(key, msg, hex_out);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", hex_out);

    // Pair a client and test MQTT HMAC command verification
    auth_manager_init();
    char pin[AUTH_PAIRING_PIN_LEN + 1] = {0};
    char token[CONFIG_STORE_TOKEN_MAX_LEN] = {0};
    char topic_key[CONFIG_STORE_TOPIC_KEY_MAX_LEN] = {0};
    auth_manager_start_pairing(pin, NULL);
    auth_manager_claim_pin(pin, "MQTT Recipe Uploader", token, topic_key);

    const char *req_id = "req-4821";
    const char *action = "load_recipe";
    uint32_t ts = 1712678000;
    char signing_input[128];
    snprintf(signing_input, sizeof(signing_input), "%s:%s:%lu", req_id, action, (unsigned long)ts);
    char valid_sig[AUTH_HMAC_HEX_LEN + 1] = {0};
    auth_manager_compute_hmac_sha256_hex(token, signing_input, valid_sig);

    TEST_ASSERT_TRUE(auth_manager_verify_mqtt_command(req_id, action, ts, valid_sig, NULL));
    TEST_ASSERT_FALSE(auth_manager_verify_mqtt_command(req_id, "set_pump", ts, valid_sig, NULL));
    TEST_ASSERT_FALSE(auth_manager_verify_mqtt_command(req_id, action, ts + 1, valid_sig, NULL));
}

void test_auth_unique_client_names_and_deletion(void) {
    auth_manager_set_test_credentials(NULL, NULL, false);

    // Pair Client 1 presenting "Brewfather"
    char token1[CONFIG_STORE_TOKEN_MAX_LEN] = {0};
    char tk1[CONFIG_STORE_TOPIC_KEY_MAX_LEN] = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_start_pairing(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_claim_pin(NULL, "Brewfather", token1, tk1));

    // Pair Client 2 also presenting "Brewfather"
    char token2[CONFIG_STORE_TOKEN_MAX_LEN] = {0};
    char tk2[CONFIG_STORE_TOPIC_KEY_MAX_LEN] = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_start_pairing(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_claim_pin(NULL, "Brewfather", token2, tk2));

    paired_client_t clients[CONFIG_STORE_MAX_PAIRED_CLIENTS];
    uint8_t count = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_get_paired_clients(clients, &count));
    TEST_ASSERT_EQUAL_UINT8(2, count);
    TEST_ASSERT_TRUE(clients[0].active);
    TEST_ASSERT_TRUE(clients[1].active);

    // Both names start with "Brewfather-" followed by 4 hex chars and are guaranteed unique
    TEST_ASSERT_EQUAL_INT(0, strncmp(clients[0].client_name, "Brewfather-", 11));
    TEST_ASSERT_EQUAL_INT(0, strncmp(clients[1].client_name, "Brewfather-", 11));
    TEST_ASSERT_EQUAL_UINT32(15, strlen(clients[0].client_name));
    TEST_ASSERT_EQUAL_UINT32(15, strlen(clients[1].client_name));
    TEST_ASSERT_FALSE(strcmp(clients[0].client_name, clients[1].client_name) == 0);

    // Both clients' API tokens are valid simultaneously
    TEST_ASSERT_TRUE(auth_manager_verify_token(token1));
    TEST_ASSERT_TRUE(auth_manager_verify_token(token2));

    // Delete Client 1 by its unique name -> token1 is immediately revoked, token2 stays valid
    char deleted_name[CONFIG_STORE_CLIENT_NAME_MAX_LEN];
    strncpy(deleted_name, clients[0].client_name, sizeof(deleted_name) - 1);
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_delete_paired_client(deleted_name));

    TEST_ASSERT_FALSE(auth_manager_verify_token(token1));
    TEST_ASSERT_TRUE(auth_manager_verify_token(token2));

    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_get_paired_clients(clients, &count));
    TEST_ASSERT_EQUAL_UINT8(1, count);

    // Deleting a non-existent client returns ESP_ERR_NOT_FOUND
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND, auth_manager_delete_paired_client(deleted_name));

    // Delete remaining client
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_delete_paired_client(clients[1].client_name));
    TEST_ASSERT_FALSE(auth_manager_verify_token(token2));
    TEST_ASSERT_EQUAL_INT(ESP_OK, auth_manager_get_paired_clients(clients, &count));
    TEST_ASSERT_EQUAL_UINT8(0, count);
}

// =============================================================================
// Multi-Equipment Manager & Fail-Fast Recipe Upload Tests
// =============================================================================
static ble_outbound_cmd_packet_t s_captured_pkts[32];
static uint16_t s_captured_pkt_count = 0;

static esp_err_t mock_enqueue_ble_pkt(const ble_outbound_cmd_packet_t *pkt) {
    if (s_captured_pkt_count >= 32) {
        return ESP_ERR_NO_MEM;
    }
    s_captured_pkts[s_captured_pkt_count++] = *pkt;
    return ESP_OK;
}

void test_equipment_manager_fail_fast_when_disconnected(void) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_init(NULL));
    equipment_manager_set_enqueue_override(mock_enqueue_ble_pkt);
    s_captured_pkt_count = 0;

    g30_recipe_t recipe;
    memset(&recipe, 0, sizeof(recipe));
    strncpy(recipe.name, "Offline IPA", sizeof(recipe.name) - 1);
    recipe.boil_time_min = 60;
    recipe.mash_step_count = 1;
    recipe.mash_steps[0].temp_c = 65.0f;
    recipe.mash_steps[0].duration_min = 60;

    uint8_t resolved_mac[6] = {0};
    // No BLE device connected -> must fail fast with ESP_ERR_NOT_FOUND and queue 0 packets
    esp_err_t err = equipment_manager_dispatch_recipe(NULL, EQUIPMENT_TYPE_BOILER, "req-offline", &recipe, resolved_mac);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND, err);
    TEST_ASSERT_EQUAL_UINT16(0, s_captured_pkt_count);
}

void test_equipment_manager_multi_device_recipe_dispatch(void) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_init(NULL));
    equipment_manager_set_enqueue_override(mock_enqueue_ble_pkt);

    // Connect 1 Boiler (Grainfather G30) and 1 Fermenter simultaneously
    uint8_t g30_mac[6] = {0xC4, 0x4F, 0x33, 0x1A, 0x8B, 0x22};
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_on_peer_event(
        g30_mac, true, EQUIPMENT_TYPE_BOILER, "Grainfather G30 Connect", 1, -58));

    uint8_t ferm_mac[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_on_peer_event(
        ferm_mac, true, EQUIPMENT_TYPE_FERMENTER, "Generic BLE Fermenter", 2, -64));

    // 1. Dispatch Boiler Recipe (2 mash steps, 2 boil additions -> 4 + 2 + 2 = 8 BLE packets)
    s_captured_pkt_count = 0;
    g30_recipe_t recipe;
    memset(&recipe, 0, sizeof(recipe));
    strncpy(recipe.name, "West Coast IPA", sizeof(recipe.name) - 1);
    recipe.boil_time_min = 60;
    recipe.mash_water_l = 16.5f;
    recipe.sparge_water_l = 13.5f;
    recipe.mash_step_count = 2;
    recipe.mash_steps[0].temp_c = 65.0f;
    recipe.mash_steps[0].duration_min = 60;
    recipe.mash_steps[1].temp_c = 75.0f;
    recipe.mash_steps[1].duration_min = 10;
    recipe.boil_addition_count = 2;
    recipe.boil_additions_min[0] = 60;
    recipe.boil_additions_min[1] = 15;
    recipe.fermentation_step_count = 1;
    recipe.fermentation_steps[0].temp_c = 19.5f;
    recipe.fermentation_steps[0].duration_hours = 240;
    recipe.target_og_sg = 1.062f;

    uint8_t resolved_mac[6] = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_dispatch_recipe(g30_mac, EQUIPMENT_TYPE_BOILER, "req-g30-1", &recipe, resolved_mac));
    TEST_ASSERT_EQUAL_UINT16(8, s_captured_pkt_count);
    TEST_ASSERT_EQUAL_UINT16(1, s_captured_pkts[0].seq_idx);
    TEST_ASSERT_EQUAL_UINT16(8, s_captured_pkts[0].seq_total);
    TEST_ASSERT_EQUAL_UINT16(8, s_captured_pkts[7].seq_idx);
    TEST_ASSERT_EQUAL_UINT16(8, s_captured_pkts[7].seq_total);
    TEST_ASSERT_EQUAL_STRING("req-g30-1", s_captured_pkts[7].req_id);

    // 2. Dispatch Fermenter Profile to the connected Fermenter
    s_captured_pkt_count = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_dispatch_recipe(ferm_mac, EQUIPMENT_TYPE_FERMENTER, "req-ferm-1", &recipe, resolved_mac));
    TEST_ASSERT_EQUAL_UINT16(1, s_captured_pkt_count);
    TEST_ASSERT_EQUAL_STRING("F1:19.5:240", s_captured_pkts[0].cmd_bytes);

    // 3. List paired/connected Bluetooth devices as JSON without needing MACs
    char eq_list_json[1024] = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_format_equipment_list_json("C44F331A8B20", "req-list-1", eq_list_json, sizeof(eq_list_json)));
    TEST_ASSERT_NOT_NULL(strstr(eq_list_json, "\"event\":\"equipment_list\""));
    TEST_ASSERT_NOT_NULL(strstr(eq_list_json, "\"req_id\":\"req-list-1\""));
    TEST_ASSERT_NOT_NULL(strstr(eq_list_json, "\"count\":2"));
    TEST_ASSERT_NOT_NULL(strstr(eq_list_json, "\"slot\":1"));
    TEST_ASSERT_NOT_NULL(strstr(eq_list_json, "\"name\":\"Grainfather G30 Connect\""));
    TEST_ASSERT_NOT_NULL(strstr(eq_list_json, "\"slot\":2"));
    TEST_ASSERT_NOT_NULL(strstr(eq_list_json, "\"name\":\"Generic BLE Fermenter\""));

    // 4. Target equipment by 1-based slot index (slot 2 -> Fermenter) without knowing MAC address
    s_captured_pkt_count = 0;
    memset(resolved_mac, 0, sizeof(resolved_mac));
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_dispatch_recipe_to_slot(2, NULL, EQUIPMENT_TYPE_UNKNOWN, "req-slot-2", &recipe, resolved_mac));
    TEST_ASSERT_EQUAL_UINT16(1, s_captured_pkt_count);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ferm_mac, resolved_mac, 6);

    // 5. Disconnect G30 and verify subsequent upload fails fast with ESP_ERR_NOT_FOUND
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_on_peer_event(
        g30_mac, false, EQUIPMENT_TYPE_BOILER, "Grainfather G30 Connect", 1, -58));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND, equipment_manager_dispatch_recipe(g30_mac, EQUIPMENT_TYPE_BOILER, "req-g30-2", &recipe, resolved_mac));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND, equipment_manager_dispatch_recipe_to_slot(1, NULL, EQUIPMENT_TYPE_BOILER, "req-slot-1-offline", &recipe, resolved_mac));
}

void test_equipment_manager_unique_names_and_power_cycle_persistence(void) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_init(NULL));
    equipment_manager_set_enqueue_override(mock_enqueue_ble_pkt);

    // Pair two identical "Grainfather G30 Connect" boilers with different MAC addresses
    uint8_t g30_mac_1[6] = {0xC4, 0x4F, 0x33, 0x1A, 0x8B, 0x22};
    uint8_t g30_mac_2[6] = {0xC4, 0x4F, 0x33, 0x1A, 0x8B, 0x23};

    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_on_peer_event(
        g30_mac_1, true, EQUIPMENT_TYPE_BOILER, "Grainfather G30 Connect", 1, -55));
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_on_peer_event(
        g30_mac_2, true, EQUIPMENT_TYPE_BOILER, "Grainfather G30 Connect", 2, -62));

    equipment_slot_t slot1, slot2;
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_get_slot(0, &slot1));
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_get_slot(1, &slot2));

    // Verify names are guaranteed unique across all paired slots
    TEST_ASSERT_EQUAL_STRING("Grainfather G30 Connect", slot1.name);
    TEST_ASSERT_EQUAL_STRING("Grainfather G30 Connect-8B23", slot2.name);
    TEST_ASSERT_TRUE(slot1.paired);
    TEST_ASSERT_TRUE(slot2.paired);

    // Verify targeting the second boiler specifically by its unique target_name
    s_captured_pkt_count = 0;
    g30_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = G30_CMD_ACTION_SET_PUMP;
    cmd.params.pump_state = true;
    strncpy(cmd.target_name, "Grainfather G30 Connect-8B23", sizeof(cmd.target_name) - 1);

    uint8_t resolved_mac[6] = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_dispatch_command(&cmd, resolved_mac));
    TEST_ASSERT_EQUAL_UINT16(1, s_captured_pkt_count);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(g30_mac_2, resolved_mac, 6);

    // Simulate power-cycling Boiler 2 (disconnect -> stays in paired list with connected: false, then reconnects to same slot & unique name)
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_on_peer_event(
        g30_mac_2, false, EQUIPMENT_TYPE_BOILER, "Grainfather G30 Connect", 2, -62));

    char eq_list_json[1024] = {0};
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_format_equipment_list_json("C44F331A8B20", NULL, eq_list_json, sizeof(eq_list_json)));
    TEST_ASSERT_NOT_NULL(strstr(eq_list_json, "\"count\":2"));
    TEST_ASSERT_NOT_NULL(strstr(eq_list_json, "\"slot\":2,\"paired\":true,\"connected\":false,\"name\":\"Grainfather G30 Connect-8B23\""));

    // Power Boiler 2 back on -> reconnects into slot 2 and retains its unique name
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_on_peer_event(
        g30_mac_2, true, EQUIPMENT_TYPE_BOILER, "Grainfather G30 Connect", 5, -59));
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_get_slot(1, &slot2));
    TEST_ASSERT_TRUE(slot2.connected);
    TEST_ASSERT_TRUE(slot2.paired);
    TEST_ASSERT_EQUAL_STRING("Grainfather G30 Connect-8B23", slot2.name);

    // Explicit unpair removes it from the slot
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_unpair_device(g30_mac_2));
    TEST_ASSERT_EQUAL_INT(ESP_OK, equipment_manager_get_slot(1, &slot2));
    TEST_ASSERT_FALSE(slot2.paired);
    TEST_ASSERT_FALSE(slot2.connected);
}

// =============================================================================
// Test Runner Entrypoint
// =============================================================================
int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // Baseline tests
    RUN_TEST(test_telemetry_packet_structure);
    RUN_TEST(test_max_simultaneous_connections_limit);
    RUN_TEST(test_esp_idf_version_constraint);
    RUN_TEST(test_ble_peer_and_scan_structures);
    RUN_TEST(test_wifi_ap_configuration_defines);
    RUN_TEST(test_wifi_scan_deduplicate_keeps_strongest_rssi);
    RUN_TEST(test_wifi_scan_deduplicate_filters_empty_hidden_ssids);
    RUN_TEST(test_wifi_scan_deduplicate_sorts_descending_by_rssi);
    RUN_TEST(test_wifi_scan_deduplicate_handles_null_and_zero_inputs);
    RUN_TEST(test_wifi_scan_deduplicate_respects_max_output_limit);

    // G30 Notification Parsing tests
    RUN_TEST(test_g30_parse_temperature_notification);
    RUN_TEST(test_g30_parse_system_state_notification);
    RUN_TEST(test_g30_parse_timer_notification);
    RUN_TEST(test_g30_parse_power_notification);
    RUN_TEST(test_g30_parse_interaction_notification);
    RUN_TEST(test_g30_parse_boil_temp_notification);

    // G30 Command Encoding tests
    RUN_TEST(test_g30_format_padded_cmd_exact_19_bytes);
    RUN_TEST(test_g30_encode_pump_command);
    RUN_TEST(test_g30_encode_heat_command);
    RUN_TEST(test_g30_encode_target_temperature);
    RUN_TEST(test_g30_encode_timer_commands);
    RUN_TEST(test_g30_encode_extended_commands);
    RUN_TEST(test_g30_parse_extended_state_flags);
    RUN_TEST(test_g30_encode_recipe_commands);

    // Topic Hierarchy & Cadence tests
    RUN_TEST(test_mqtt_topic_hierarchy_definitions);
    RUN_TEST(test_telemetry_cadence_definitions);

    // BLE Compatible Devices Sourcecode Validation tests
    RUN_TEST(test_compatible_devices_registry_defined);
    RUN_TEST(test_compatibility_match_grainfather_names);
    RUN_TEST(test_compatibility_match_by_service_uuids);
    RUN_TEST(test_compatibility_rejects_non_g30_equipment);
    RUN_TEST(test_compatibility_rejects_incompatible_devices);
    RUN_TEST(test_compatibility_full_advertisement_check);

    // G30 Web Controller & Recipe tests
    RUN_TEST(test_g30_web_recipe_conversion_and_encoding);
    RUN_TEST(test_g30_web_control_actions_mapping);
    RUN_TEST(test_g30_parse_initial_strike_water_prompt);
    RUN_TEST(test_g30_stage_name_mapping);
    RUN_TEST(test_g30_parse_timer_real_protocol_minutes_and_seconds);
    RUN_TEST(test_g30_master_recipe_full_lifecycle);

    // Pairing, Token Auth, MQTT HMAC-SHA256 & Multi-Equipment Manager tests
    RUN_TEST(test_auth_pin_pairing_and_token_verification);
    RUN_TEST(test_auth_code_free_push_button_pairing_and_discovery_beacon);
    RUN_TEST(test_auth_hmac_sha256_and_mqtt_command_signing);
    RUN_TEST(test_auth_unique_client_names_and_deletion);
    RUN_TEST(test_equipment_manager_fail_fast_when_disconnected);
    RUN_TEST(test_equipment_manager_multi_device_recipe_dispatch);
    RUN_TEST(test_equipment_manager_unique_names_and_power_cycle_persistence);

    return UNITY_END();
}
