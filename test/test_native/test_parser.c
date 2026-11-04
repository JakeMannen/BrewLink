#include <unity.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "unified_schema.h"
#include "grainfather_g30.h"

// Include source directly for host testing harness
#include "../../src/grainfather_g30.c"

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
    scan.equipment_type = EQUIPMENT_TYPE_BOILER;
    strncpy(scan.name, "Grainfather", sizeof(scan.name) - 1);
    TEST_ASSERT_TRUE(scan.is_grainfather);
    TEST_ASSERT_EQUAL_STRING("Grainfather", scan.name);
}

void test_wifi_ap_configuration_defines(void) {
    TEST_ASSERT_EQUAL_STRING("BrewLink-Setup", WIFI_AP_SSID_PREFIX);
    TEST_ASSERT_EQUAL_INT(1, WIFI_AP_CHANNEL);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, WIFI_AP_MAX_CONN);
    TEST_ASSERT_EQUAL_INT(80, WEB_SERVER_PORT);
}

// =============================================================================
// Grainfather G30 Notification Parsing Tests (BLE -> Gateway / WiFi)
// =============================================================================
void test_g30_parse_temperature_notification(void) {
    unified_boiler_state_t state;
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
    unified_boiler_state_t state;
    memset(&state, 0, sizeof(state));

    // Y{Heat},{Pump},{Auto},{Ramp},{InterMode},{InterCode},{Stage},{Delayed}
    const uint8_t notif_data[] = "Y1,1,1,0,1,2,3,0";
    g30_notification_type_t type = G30_NOTIF_UNKNOWN;

    esp_err_t err = g30_parse_notification(notif_data, strlen((char*)notif_data), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(G30_NOTIF_SYSTEM_STATE, type);
    TEST_ASSERT_TRUE(state.heat_active);
    TEST_ASSERT_TRUE(state.pump_active);
    TEST_ASSERT_TRUE(state.auto_mode);
    TEST_ASSERT_FALSE(state.ramp_active);
    TEST_ASSERT_TRUE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(2, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Mash step finished: Sparge alert", state.interaction.message);
    TEST_ASSERT_EQUAL_UINT8(3, state.stage.step_index);
}

void test_g30_parse_timer_notification(void) {
    unified_boiler_state_t state;
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
    unified_boiler_state_t state;
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
    unified_boiler_state_t state;
    memset(&state, 0, sizeof(state));

    const uint8_t notif_data[] = "I1";
    g30_notification_type_t type = G30_NOTIF_UNKNOWN;

    esp_err_t err = g30_parse_notification(notif_data, strlen((char*)notif_data), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_INT(G30_NOTIF_INTERACTION, type);
    TEST_ASSERT_TRUE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(1, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Strike temperature reached: Add grain", state.interaction.message);
}

void test_g30_parse_boil_temp_notification(void) {
    unified_boiler_state_t state;
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
    unified_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = CMD_ACTION_SET_PUMP;

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
    unified_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = CMD_ACTION_SET_HEAT;

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
    unified_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = CMD_ACTION_SET_TARGET_TEMP;

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
    unified_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    // Set timer 60 min -> "S60" + 16 spaces
    cmd.action = CMD_ACTION_SET_TIMER;
    cmd.params.timer_duration_min = 60;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("S60                ", buf);

    // Pause / Resume -> "G" + 18 spaces
    cmd.action = CMD_ACTION_PAUSE_TIMER;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("G                  ", buf);

    // Cancel timer -> "C" + 18 spaces
    cmd.action = CMD_ACTION_CANCEL_TIMER;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("C                  ", buf);
}

void test_g30_encode_recipe_commands(void) {
    char buf[G30_COMMAND_LEN + 1];
    unified_recipe_t recipe;
    memset(&recipe, 0, sizeof(recipe));
    recipe.boil_time_min = 75;
    recipe.mash_step_count = 2;
    recipe.mash_water_l = 16.5f;
    recipe.sparge_water_l = 14.0f;

    // Header -> "R75,2,16.5,14.0,  " (19 bytes)
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_header(&recipe, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("R75,2,16.5,14.0,  ", buf);

    // Mash Step 1: 65C for 60 min -> "65:60              "
    unified_mash_step_t step1 = {.temp_c = 65.0f, .duration_min = 60};
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_mash_step(&step1, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("65:60              ", buf);

    // Boil stop at 15 min remaining -> "15                 "
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_recipe_boil_stop(15, buf));
    TEST_ASSERT_EQUAL_UINT32(19, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("15                 ", buf);
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
    RUN_TEST(test_g30_encode_recipe_commands);

    return UNITY_END();
}
