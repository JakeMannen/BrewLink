# Rule: GitHub Flow & Version Control

This rule defines the branching, commit message formatting, and pull request standards for the repository.

---

## 1. Branching Strategy
*   `main` is always stable, deployable, and compiles without warnings.
*   All work is developed on short-lived branches created directly from `main`:
    *   `feat/<feature-name>` (e.g., `feat/ble-multi-conn`, `feat/mqtt-relay`)
    *   `fix/<bug-name>` (e.g., `fix/heap-leak-discovery`, `fix/wifi-reconnect`)
    *   `chore/<task>` (e.g., `chore/devcontainer-tools`, `chore/ci-pipeline`)
    *   `refactor/<name>` (e.g., `refactor/queue-dispatcher`)

---

## 2. Conventional Commits
Use semantic, conventional commit messages:
*   `feat(ble): implement multi-device connection table`
*   `fix(relay): prevent queue overflow on WiFi drop`
*   `docs(agents): update devcontainer build guidelines`
*   `chore(deps): bump esp-idf platform in platformio.ini`

---

## 3. Pull Requests & CI
*   Every PR must use the checklist in `.github/pull_request_template.md`.
*   PRs must pass automated CI (`.github/workflows/ci.yml`) compiling `featheresp32`.
*   Document embedded testing notes: specify the number of BLE peripherals verified, RSSI levels, and heap stability before and after test.
*   Squash and merge into `main`.
