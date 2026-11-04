#include <unity.h>
#include <string.h>
#include <stdint.h>
#include "config.h"

void setUp(void) {
    // Set up before each test
}

void tearDown(void) {
    // Clean up after each test
}

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

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_telemetry_packet_structure);
    RUN_TEST(test_max_simultaneous_connections_limit);
    return UNITY_END();
}
