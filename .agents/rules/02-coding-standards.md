# Rule: Coding Standards & Error Handling

This rule enforces coding standards, error handling, thread safety, and logging idioms across the codebase.

---

## 1. ESP-IDF Idioms & Error Checking
*   Return `esp_err_t` on all fallible functions.
*   Check startup and critical initializations with `ESP_ERROR_CHECK()`.
*   Handle runtime failures gracefully (reconnect backoff, packet dropping with warning, state reset).

---

## 2. Structured Logging
*   Define a unique tag per module:
    ```c
    static const char *TAG = "MODULE_NAME";
    ```
*   Use ESP-IDF log macros: `ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE`, `ESP_LOGD`.
*   **Rule**: NEVER use raw `printf()` in production firmware.

---

## 3. Thread Safety & Synchronization
*   Shared data structures between tasks must be protected by FreeRTOS Mutexes (`SemaphoreHandle_t`) or passed by value through FreeRTOS Queues.
*   Never perform blocking operations or memory allocation in callback contexts (e.g. NimBLE GAP/GATT callbacks, WiFi event handlers).

---

## 4. Header Cleanliness
*   Use standard `#ifndef / #define` include guards in all headers.
*   Wrap C headers with C++ guards:
    ```c
    #ifdef __cplusplus
    extern "C" {
    #endif

    /* declarations */

    #ifdef __cplusplus
    }
    #endif
    ```

---

## 5. Post-Edit Compilation & Build Verification (MANDATORY)
*   **Always Run**: After finishing ESP code edits, always run:
    ```bash
    devcontainer exec pio run -e esp32dev -t build
    ```
*   **Fix Build Errors**: Immediately inspect compiler diagnostics and fix any syntax, type, header inclusion, or linker errors until the build succeeds cleanly before proceeding.
