---
name: vscode-platformio
description: >-
  Build, check, flash, monitor, and analyze memory for the Adafruit HUZZAH32 ESP-IDF v6.1 firmware within the VS Code Devcontainer using PlatformIO.
---

# VS Code PlatformIO & Devcontainer Workflow Skill

This skill guides the agent through compiling, flashing, monitoring, testing, and debugging the BrewLink firmware (targeting ESP-IDF v6.1) inside the VS Code Devcontainer.

---

## 1. Environment Verification

1. Verify that commands run inside the devcontainer terminal as user `vscode`:
   ```bash
   whoami
   pio --version
   ```
2. Verify that PlatformIO Core is available on the `$PATH` (`/home/vscode/.platformio/penv/bin`).

---

## 2. Standard Build & Verification Procedures

### 2.1. Compile Firmware
To compile the `featheresp32` target:
```bash
pio run -e featheresp32
```

### 2.2. Static Code Analysis
Run static analysis with PlatformIO check:
```bash
pio check
```

### 2.3. Memory & Partition Inspection
To verify RAM (SRAM) and Flash consumption against available limits:
```bash
pio run -e featheresp32 -t size
```

### 2.4. Host Unit Testing
Run native unit tests (Unity framework) on desktop/container without hardware attached:
```bash
pio test -e native
```

### 2.5. ESP-IDF Menuconfig
Launch interactive menuconfig:
```bash
pio run -t menuconfig
```

---

## 3. Flashing & Hardware Communication

### 3.1. USB Passthrough from Host (Windows / WSL2)
If running on Windows host, attach the Adafruit Feather USB port (Silicon Labs CP2104) to WSL2/Docker:
```powershell
usbipd list
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>
```

### 3.2. Direct Flashing from Devcontainer
1. Check that the port is mounted:
   ```bash
   pio device list
   ```
2. Flash firmware to connected Feather:
   ```bash
   pio run -e featheresp32 -t upload
   ```
3. Open serial monitor at 115200 baud with backtrace decoding:
   ```bash
   pio device monitor -b 115200
   ```

### 3.3. When Flashing Hardware is Unavailable to Agent
If operating in an isolated sandbox, run compilation (`pio run -e featheresp32`) and instruct the user to execute the flash command from their VS Code terminal.

---

## 4. IntelliSense & VS Code Index Management

*   **Rule**: Do NOT manually create or edit `.vscode/c_cpp_properties.json`.
*   If header indexing warnings occur, regenerate the index via PlatformIO:
    ```bash
    pio init --ide vscode
    ```
    or trigger the VS Code Command Palette action: `> PlatformIO: Rebuild C/C++ Index`.

---

## 5. ESP-IDF Official Documentation & Architecture References

Consult the official Espressif documentation for architectural deep-dives, hardware driver APIs, cryptography, and component guidelines:

*   **Primary Documentation Portal**:
    *   [ESP-IDF Programming Guide (ESP32 Stable)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/index.html)
*   **API Reference & Guides**:
    *   [ESP-IDF API Reference](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/index.html) — Core system APIs (FreeRTOS, timers, heaps, logging), storage (NVS, partitions), networking, and peripherals.
    *   [ESP-IDF API Guides](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/index.html) — Technical architectural guides including Wi-Fi driver architecture, thread local storage, host testing, and multi-core mechanics.
*   **Security Guides**:
    *   [ESP-IDF Security Guides](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/security/index.html) — Comprehensive security guides covering Secure Boot, Flash Encryption, hardware crypto accelerators, anti-rollback protection, and Wi-Fi security (PMF / WPA3).
*   **Libraries & Frameworks**:
    *   [ESP-IDF Libraries and Frameworks](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/libraries-and-frameworks/index.html) — Middleware, protocols (ESP-TLS, Mbed TLS, MQTT, HTTP), and component integrations.
