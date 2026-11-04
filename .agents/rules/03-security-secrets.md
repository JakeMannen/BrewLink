# Rule: Secrets & Security Policy

This rule governs credential handling and secrets isolation in the repository.

---

## 1. Secrets Isolation Policy
*   **Rule**: NEVER commit credentials, WiFi SSIDs, passwords, API tokens, or broker private keys to git.
*   Credentials belong exclusively in `include/secrets.h`.
*   `include/secrets.h` is strictly excluded in `.gitignore`.

---

## 2. Template Maintenance
*   Always keep `include/secrets.example.h` updated with placeholder values whenever new configuration keys or credentials are added to the codebase.
*   CI workflows must use `include/secrets.example.h` to populate a dummy `include/secrets.h` if missing during compilation.
