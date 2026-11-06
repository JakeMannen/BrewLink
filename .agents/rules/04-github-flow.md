# Rule: GitHub Flow & Version Control

This rule defines the branching, commit message formatting, and pull request standards for the repository.

---

## 1. Core Git Safety Rules (MANDATORY FOR AGENTS)
*   **Never push to `main`**: Direct pushes to `main` are strictly forbidden. All code changes reach `main` exclusively via Pull Requests.
*   **Never force push**: Force pushing (`git push --force`, `-f`, or `--force-with-lease`) is strictly prohibited under all circumstances. Remote history must never be rewritten.
*   **Always start a new session/feature in a new branch from updated `main`**: Unless explicitly stated otherwise by the user, every new task, session, or feature must start on a fresh branch created from an up-to-date `main`:
    1. Switch to `main`: `git checkout main`
    2. Pull upstream updates: `git pull origin main`
    3. Create and switch to topic branch: `git checkout -b <branch-type>/<short-description>`

---

## 2. Branching Strategy
*   `main` is always stable, deployable, and compiles without warnings.
*   All work is developed on short-lived branches created directly from an updated `main`:
    *   `feat/<feature-name>` (e.g., `feat/ble-multi-conn`, `feat/mqtt-relay`)
    *   `fix/<bug-name>` (e.g., `fix/heap-leak-discovery`, `fix/wifi-reconnect`)
    *   `chore/<task>` (e.g., `chore/devcontainer-tools`, `chore/ci-pipeline`)
    *   `refactor/<name>` (e.g., `refactor/queue-dispatcher`)
    *   `docs/<name>` (e.g., `docs/git-instructions`)

---

## 3. Conventional Commits
Use semantic, conventional commit messages:
*   `feat(ble): implement multi-device connection table`
*   `fix(relay): prevent queue overflow on WiFi drop`
*   `docs(agents): update devcontainer build guidelines`
*   `chore(deps): bump esp-idf platform in platformio.ini`

---

## 4. Pull Requests & CI
*   PRs must pass automated CI (`.github/workflows/ci.yml`) compiling `esp32dev`.
*   Document embedded testing notes: specify the number of BLE peripherals verified, RSSI levels, and heap stability before and after test.
*   Squash and merge into `main` via Pull Request.
