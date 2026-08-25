---
id: kb-tools-command-inventory
type: reference
status: active
date: 2026-08-02
updated: 2026-08-02
repo: tools
tags: [editor, commands, registry, undo, traceability]
summary: "EDITOR_AUTOMATION_PLAN_v1 Phase 1.4 traceability: every EditorCmdRegistry command, args, undo status, reachable UI surfaces"
---

# EditorCmdRegistry Command Inventory

EDITOR_AUTOMATION_PLAN_v1.md Phase 1.4 traceability artifact. Every command
registered via `RegisterStdEditorCommands()` (`editor/editor_std_commands.h`
+ `.cpp`, plus `DeleteSelectedCmd` in `editor/editor_command_palette.cpp`),
its arg schema, undo status, and every UI surface that dispatches it.
Regenerate the table by hand after any registry change — this is a doc, not
a generated artifact; keep it in sync manually.

## Registered commands

| Command | Args | Undo | Origin (body) | Reachable from |
|---|---|---|---|---|
| `Delete Selected` | — | NO_UNDO | `editor_command_palette.cpp` | Palette, Toolbar (Edit>Delete), Console (`/exec Delete Selected`) |
| `Duplicate Selected` | — | NO_UNDO | `editor_std_commands.cpp` | Toolbar (Edit>Duplicate) |
| `Select All` | — | NO_UNDO | `editor_std_commands.cpp` | Palette, Toolbar (Edit>Select All) |
| `New Scene` | — | NO_UNDO | `editor_std_commands.cpp` | Toolbar (File>New Scene) |
| `Import Scene` | `path: Str` | NO_UNDO | `editor_std_commands.cpp` | Toolbar (File>Import Scene) |
| `Export Scene` | `path: Str` | NO_UNDO | `editor_std_commands.cpp` | Toolbar (File>Export Scene) |
| `Save Game` | — | NO_UNDO | `editor_std_commands.cpp` | Palette, Toolbar (File>Save Game F5), Console (`save`) |
| `Load Game` | — | NO_UNDO | `editor_std_commands.cpp` | Palette, Toolbar (File>Load Game F9), Console (`load`) |
| `Reload Shaders` | — | NO_UNDO | `editor_std_commands.cpp` | Console (`reload-shaders`) |
| `Rebuild NavMesh` | — | NO_UNDO | `editor_std_commands.cpp` | Palette, Toolbar (Scene>Rebuild NavMesh) |
| `Spawn Entity` | `faction: I64, x: F64, z: F64` | NO_UNDO | `editor_std_commands.cpp` | Console (`spawn <faction> <x> <z>`) |
| `Kill Entity` | `entity_id: Entity` | NO_UNDO | `editor_std_commands.cpp` | Console (`kill <id>`) |
| `Set Faction Relation` | `a: I64, b: I64, v: I64` | NO_UNDO | `editor_std_commands.cpp` | Console (`faction <a> <b> <v>`) |
| `FPS Query` | — | NO_UNDO | `editor_std_commands.cpp` | Console (`fps`) |
| `Add Component` | `entity_id: Entity, component_name: Str` | NO_UNDO | `editor_std_commands.cpp` | Inspector ("+ Add component" combo) |

All 15 are `NO_UNDO` — see each command's doc comment
(`editor_std_commands.h`) for why: real undo needs enough captured state to
reverse the action (full entity+component data for Delete/Duplicate/Spawn,
prior HP for Kill, prior relation value for Set Faction Relation, etc.) into
the 64-byte `undo_data` blob, which is per-command-body work deferred to a
follow-up now that the registry's undo-mediation *mechanism* itself is
proven (Phase 1.1-1.2's `UndoCapableCommandRoundTripsThroughFakeStack` test).

## Console-only passthroughs (decision, not migrated)

Recorded inline in `editor_console.cpp` next to `ExecCommand`, repeated here
for the inventory's sake:

| Console verb | Why it stays console-only |
|---|---|
| `help` | Pure text dump, nothing to script or undo. |
| `set` / `get` / `list` | CVarRegistry is already an adequate mini-registry (generic name+value across all cvars) — folding it into EditorCmdRegistry would just duplicate it. |
| `subsystem` | Same reasoning, SubsystemRegistry. |
| `reload` | Vestigial: prints a hint pointing at `reload-shaders`, not a distinct action. Found during this phase's inventory pass; not fixed here — a finding, not a fix, inside a migration commit. |
| `navmesh` | Read-only status query; better home is Phase 2.3's `md.editor.status()` extension than a "command" that doesn't command anything. |

## Registration lifetime

`RegisterStdEditorCommands(owner_module_id)` is called from **two**
independent places with **two distinct** `owner_module_id`s, because there
are two independent binaries that bring up an `EditorCore` instance:

- `editor_panels_entry.cpp::editor_panels_init()` — `kPanelsModuleId = 1`.
  Used by `monkey_dust_editor`, which dlopens `libeditor_panels.so`.
  Unregistered in `editor_panels_shutdown()` before dlclose.
- `game/src/main.cpp` (`InitGame`) — `kGameEditorModuleId = 2`. Used by the
  game's own statically-linked F3 editor (`monkey_dust
  -DMONKEY_DUST_EDITOR=ON`), which never dlopens that .so at all.
  Unregistered in `ShutdownGame`.

Without the second call site, every migrated palette/toolbar/console
dispatch in the game's F3 editor would silently hit an empty
`EditorCmdRegistry` — found and fixed during this phase (see
`editor_std_commands.h`'s `RegisterStdEditorCommands` doc comment).

## Phase 1's own acceptance criteria — honest status

- ✅ `grep` for action lambdas in `editor_toolbar.cpp` / `editor_command_palette.cpp`
  finds only registry dispatches for every command in the table above
  (UI-layout/gizmo-mode/panel-visibility toggles intentionally excluded —
  see "Deliberately not migrated" below).
- ⚠️ **PARTIAL**: "Every command in the inventory executes from the console
  with args." The generic, schema-driven `exec <name> [args...]` parser
  described in plan section 1.3.3 was **not** built this phase — `exec` is
  still the Phase 0/1.1-1.2 passthrough (dispatches by name with an empty
  `CmdArgs`, no string-to-CmdArgs conversion). The 7 commands with a
  pre-existing console verb (`spawn`/`kill`/`save`/`load`/`faction`/`fps`/
  `reload-shaders`) still parse their own args via the original `sscanf`
  calls and build `CmdArgs` by hand before dispatching — they work from the
  console, just not through the generic parser. The other 8 commands
  (`Duplicate Selected`, `Select All`, `New Scene`, `Import Scene`,
  `Export Scene`, `Rebuild NavMesh`, `Add Component`, and `Delete Selected`
  itself) have **no console verb at all** and are unreachable from the
  console today except via `exec <name>` for the ones that take zero args.
  Building the real generic parser is Phase 2's `md.editor.exec(name,
  args_table)` territory (Lua-side arg tables sidestep string parsing
  entirely) — recorded here as an open finding, not silently closed.
- ✅ Ctrl+Z/Ctrl+Y still route through `EditorCore::Undo()`/`Redo()`
  (`CommandStack`) unchanged; no command in this table pushes onto it
  (all NO_UNDO), so undo/redo behavior for these actions is unchanged from
  before this phase (still a no-op for all of them, since `history.Push()`
  had zero call sites before Phase 1.3 and still has zero after — real
  undo-capable commands are a follow-up, not part of this phase's scope).
- ✅ Hot-reload cycle: `tests/test_editor_cmd_registry.cpp`'s
  `SurvivesHotReloadCycle` test covers the full register →
  unregister-by-module → re-register sequence at the mechanism level;
  confirmed live via `monkey_dust_editor --exec` smoke run (exit=0, no
  crash, no coredump) with the full 15-command roster registered.

## Known non-unification: two parallel spawn mechanisms

`Spawn Entity` (registered, above) takes `(faction, x, z)` and builds a
bare bandit-preset entity — it mirrors the console's pre-existing `spawn`
verb exactly. `EditorToolbar::SpawnEntity(const char* type)` is a
*different*, older mechanism (archetype strings: `"Transform"`, `"NPC
Bandit"`, `"NPC Trader"`, `"NPC Holy"`) used by the toolbar's "New Entity"
popup and the palette's four `Spawn: ...` entries — already unified
between those two call sites (both call the same function), just not
through the registry, and not the same code path as `Spawn Entity`.
Left alone this phase: unifying them would mean redesigning archetype
definitions to fit `CmdArgs`' fixed tagged-value shape, which is a real
design task, not a mechanical migration — flagged here as a finding for
whoever picks up archetype-driven authoring (Phase 3 territory) to decide,
not silently merged or silently left undocumented.

## Deliberately not migrated (scoping decision, not an oversight)

Palette/toolbar entries that are pure UI/view state (gizmo mode, gizmo
space, camera focus, panel visibility toggles, debug overlay toggles,
physics-pause toggle) were **not** turned into registry commands this
phase. They don't mutate ECS/engine state an agent would meaningfully
script, and the plan's own invariant ("Commands must not call ImGui... UI
state is UI-layer, not a command") argues against forcing them through the
same mechanism as real actions. If a future phase's authoring/regression
work needs to script camera framing or gizmo mode, revisit this list then —
don't pre-emptively wrap everything just because the mechanism exists.
