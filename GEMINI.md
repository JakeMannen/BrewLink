# Workspace Instructions for Gemini & Antigravity

This workspace operates under the authoritative rules and skills defined in [AGENTS.md](./AGENTS.md) and `.agents/`.

### 1. Mandatory Rules
*   [`01-embedded-constraints.md`](.agents/rules/01-embedded-constraints.md) — ESP-WROOM-32 specs, ESP-IDF 6.1 framework constraint, Core 0 / Core 1 task pinning, memory & heap discipline.
*   [`02-coding-standards.md`](.agents/rules/02-coding-standards.md) — ESP-IDF idioms, `esp_err_t` checking, tagged logging, thread safety, and mandatory post-edit build verification (`devcontainer exec pio run -e esp32dev -t build`).
*   [`03-security-secrets.md`](.agents/rules/03-security-secrets.md) — Secrets hygiene and `secrets.h` exclusion.
*   [`04-github-flow.md`](.agents/rules/04-github-flow.md) — Git safety rules (never push to main, never force push, new session/feature on branch from updated main), branching standards, conventional commits, and PR verification.
*   [`05-host-environment-protection.md`](.agents/rules/05-host-environment-protection.md) — Zero host installation policy (never install frameworks, addons, or tools on host; prompt user if blocked).

### 2. Operational Skills
*   [`vscode-platformio`](.agents/skills/vscode-platformio/SKILL.md) — VS Code Devcontainer, PlatformIO CLI builds, check, flash, monitor, size, and tests.
*   [`ble-multi-central`](.agents/skills/ble-multi-central/SKILL.md) — NimBLE Central connecting to >= 3 peripherals simultaneously, GATT discovery, and queue handoff.
*   [`outbound-relay`](.agents/skills/outbound-relay/SKILL.md) — WiFi station, MQTT client with LWT, HTTP client, and Core 1 dispatch task.
*   [`grainfather-g30-ble`](.agents/skills/grainfather-g30-ble/SKILL.md) — Grainfather G30 Connect BLE protocol, 19-byte padded commands, notification parsing, recipe sequence, and unified schemas.
