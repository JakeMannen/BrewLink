## Summary of Changes

<!-- Briefly describe the purpose of this PR and what changes were made -->

- **Type of Change**:
  - [ ] `feat`: New feature or hardware support
  - [ ] `fix`: Bug fix
  - [ ] `refactor`: Code refactoring without behavioral change
  - [ ] `docs`: Documentation updates
  - [ ] `chore`: Build system, devcontainer, or CI maintenance

---

## Hardware & Embedded Verification

<!-- Embedded software must specify test environments -->

- [ ] **Target Board**: Adafruit HUZZAH32 (ESP-WROOM-32)
- [ ] **Devcontainer Build**: Verified compiling cleanly via `pio run -e featheresp32`
- [ ] **BLE Multi-Device Connection**:
  - [ ] Tested simultaneous connection to >= 3 BLE peripherals
  - [ ] Verified GATT characteristic discovery and notification handling
  - [ ] Verified auto-reconnect on peripheral disconnect/power cycle
- [ ] **WiFi & Outbound Relay**:
  - [ ] Verified WiFi reconnection state machine
  - [ ] Verified MQTT message publishing / HTTP request delivery
- [ ] **Memory & Heap Sanity**:
  - Minimum free heap during full operation: `_____ bytes`
  - No memory leaks detected over sustained run

---

## GitHub Flow & Quality Checklist

- [ ] Branch branched directly from `main` and up to date
- [ ] Commits follow Conventional Commits format (e.g. `feat(ble): ...`, `fix(wifi): ...`)
- [ ] Secrets and credentials verified NOT committed (`secrets.h` gitignored)
- [ ] Code formatted with standard C/C++ style (`clang-format`)
