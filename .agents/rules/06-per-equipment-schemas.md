# Rule: Per-Equipment Schemas (No Shared Schemas)

BrewLink does **not** use shared/unified data schemas. Every supported equipment model has its **own** schema.

*   **One schema per equipment model**: A boiler model (e.g. Grainfather G30) has its own schema; a fermenter model added later gets its own schema, a cooler model gets its own, etc. Two models of the same category (e.g. two different boilers) do **not** share a schema either.
*   **Naming/location**: `include/<model>_schema.h` (e.g. `g30_schema.h`), with all types prefixed by the model (`g30_state_t`, `g30_command_t`, `g30_recipe_t`, `G30_CMD_ACTION_*`). Never use `unified_*` or other generic type names.
*   **Adding new equipment is incomplete without**: (1) its `<model>_schema.h` (state, commands, recipe/profile if applicable), (2) a driver registered in the equipment manager using only that schema, (3) a telemetry JSON mapping documented in the model's skill/runbook, (4) native unit tests, (5) a new per-model subsection in Section 6 of `.agents/skills/brewlink-external-client/SKILL.md` (state, telemetry, control actions, recipe/profile, plus a row in its supported-models table) so consumers can use the equipment, and (6) updates to `AGENTS.md`/`GEMINI.md` listing the new schema and skill.
*   **Allowed shared code**: only non-schema routing infrastructure, such as `equipment_types.h` (category enum for registry/routing) and the driver interface. Do not put model telemetry, command, or recipe fields into shared headers.
*   **Never reuse another model's schema** by copy-pasting or casting; define the new model's fields explicitly.
*   `placeholder_schema.h` contains temporary stubs for fermenter/cooler not tied to any real model; they must be replaced by per-model schemas when real equipment is added.
