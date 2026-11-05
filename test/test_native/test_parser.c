#include <unity.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "unified_schema.h"
#include "grainfather_g30.h"
#include "ble_compatibility.h"

// Include source directly for host testing harness
#include "../../src/grainfather_g30.c"
#include "../../src/ble_compatibility.c"

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
    unified_mash_step_t step1 = {.temp_c = 65.0f, .duration_min = 60};
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
    unified_recipe_t recipe;
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
    unified_command_t cmd;

    // Pump control
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = CMD_ACTION_SET_PUMP;
    cmd.params.pump_state = true;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("L1                 ", buf);

    cmd.params.pump_state = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("L0                 ", buf);

    // Heat control
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = CMD_ACTION_SET_HEAT;
    cmd.params.heat.heat_state = true;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("K1                 ", buf);

    cmd.params.heat.heat_state = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("K0                 ", buf);

    // Set target temp decimal & integer
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = CMD_ACTION_SET_TARGET_TEMP;
    cmd.params.target_temp_c = 65.5f;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("$65.5,             ", buf);

    cmd.params.target_temp_c = 100.0f;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("$100,              ", buf);

    // Acknowledge interaction prompt
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = CMD_ACTION_ACK_INTERACTION;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("I                  ", buf);

    // Press SET command
    memset(&cmd, 0, sizeof(cmd));
    cmd.action = CMD_ACTION_PRESS_SET;
    TEST_ASSERT_EQUAL_INT(ESP_OK, g30_encode_command(&cmd, buf));
    TEST_ASSERT_EQUAL_STRING("T                  ", buf);
}

void test_g30_parse_initial_strike_water_prompt(void) {
    unified_boiler_state_t state;
    memset(&state, 0, sizeof(state));
    state.target_temp_c = 67.0f;
    state.current_temp_c = 23.0f;
    state.heat_active = false;
    state.stage.step_index = 1;

    // Y notification received at session start: Y0,0,1,0,1,1,1,0
    const uint8_t notif_data[] = "Y0,0,1,0,1,1,1,0";
    g30_notification_type_t type = G30_NOTIF_UNKNOWN;

    esp_err_t err = g30_parse_notification(notif_data, strlen((char*)notif_data), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_TRUE(state.interaction.required);
    TEST_ASSERT_EQUAL_UINT8(1, state.interaction.code);
    TEST_ASSERT_EQUAL_STRING("Add mash water & Press SET to start heating", state.interaction.message);

    // When temperature heats up to 67C:
    const uint8_t temp_notif[] = "X67.0,67.0";
    err = g30_parse_notification(temp_notif, strlen((char*)temp_notif), &state, &type);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("Strike temperature reached: Add grain", state.interaction.message);
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

    return UNITY_END();
}
