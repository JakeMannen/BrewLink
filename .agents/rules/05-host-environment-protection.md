# Rule: Host Environment Protection & Zero-Installation Policy

This rule strictly prohibits modifying the user's host operating system environment or installing software on the host computer.

---

## 1. Zero Host Installation Policy (STRICT & ABSOLUTE)
*   **NEVER install software directly on the host computer**.
*   This prohibition applies to:
    *   **Frameworks & Runtimes**: ESP-IDF, Arduino, .NET, Node.js, Python versions/environments, Ruby, Java, etc.
    *   **Package Managers & Global Packages**: `pip install` (global or user), `npm install -g`, `choco`, `winget`, `scoop`, `apt`, `brew`, `cargo install`, etc.
    *   **Build Tools & Compilers**: GCC, Clang, CMake, Ninja, Make, PlatformIO Core on host.
    *   **IDE Extensions & Addons**: VS Code extensions, PlatformIO IDE plugins, devcontainer plugins.
    *   **System Binaries & Drivers**: `usbipd`, CP210x / CH340 / FTDI drivers, Docker, Git, terminal utilities.

---

## 2. Devcontainer Isolation Boundary
*   All firmware compilation, toolchains, libraries, and build scripts must execute inside the designated **VS Code Devcontainer** via:
    ```bash
    devcontainer exec pio run -e esp32dev -t build
    ```
*   The host environment must remain clean, unmodified, and unpolluted by project-specific tooling.

---

## 3. Mandatory User Escalation Procedure
If a required tool, framework, CLI utility, driver, or extension is missing and execution cannot proceed:
1.  **STOP execution immediately**: Do NOT attempt any automatic installation, download script, package manager invocation, or silent workaround on the host machine.
2.  **Prompt the user clearly** explaining:
    *   The exact name and version of the missing tool/framework/addon.
    *   Why it is required for the workflow.
    *   Step-by-step instructions for how the user can install or configure it manually on their host system or in `.devcontainer/`.
3.  **Await user confirmation**: Wait until the user confirms the tool has been installed or gives explicit alternate instructions before attempting to proceed.
