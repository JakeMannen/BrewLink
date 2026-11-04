# Rule: Embedded Hardware & FreeRTOS Constraints

This rule defines mandatory hardware constraints, dual-core allocation, and memory discipline for the ESP32 (Adafruit HUZZAH32).

---

## 1. Target Hardware Specifications
*   **SoC**: ESP-WROOM-32 (Xtensa Dual-Core 32-bit LX6 @ 240 MHz).
*   **Memory**: 520 KB internal SRAM, 4 MB external SPI Flash.
*   **Coexistence Constraint**: Running simultaneous Bluetooth (NimBLE) and WiFi (LwIP + MQTT + TLS) strains internal SRAM. Typical free heap in coexistence is ~120 KB to 160 KB.

---

## 2. Core Pinning & Task Allocation
The ESP32 is a dual-core SoC. Workloads must be strictly partitioned by core using `xTaskCreatePinnedToCore()`:

*   **Core 0 (Protocol Stacks)**:
    *   NimBLE Host & Controller tasks.
    *   WiFi Driver, LwIP TCP/IP stack, and FreeRTOS system event loop.
    *   MQTT Client network worker task.
*   **Core 1 (Application & Relay)**:
    *   Telemetry Relay Dispatcher (`telemetry_relay_task`).
    *   Equipment protocol parsers and data sanitization.
    *   Local state machine, buttons, or display/LED drivers.

---

## 3. Memory & FreeRTOS Discipline
*   **Zero Dynamic Allocations in Fast Paths**:
    *   NEVER use `malloc()`, `calloc()`, or `strdup()` in periodic loops, BLE callbacks, or ISRs.
    *   Use pre-allocated static buffers or FreeRTOS queues sized during initialization.
*   **Task Stack Sizing**:
    *   Always specify explicit stack sizes via `xTaskCreatePinnedToCore()`.
    *   Minimum stack sizes: 4096 bytes for network/relay tasks, 3072 bytes for parser tasks.
*   **Watchdog Timers (WDT)**:
    *   Never write busy-wait loops (`while(1) {}`).
    *   Tasks must block on FreeRTOS primitives (`xQueueReceive()`, `xSemaphoreTake()`, `vTaskDelay()`) to yield CPU time to lower-priority tasks and the IDLE task.
*   **Heap Tracking**:
    *   Log heap milestones using `esp_get_free_heap_size()` and `esp_get_minimum_free_heap_size()` after establishing all 3 BLE connections and WiFi.
