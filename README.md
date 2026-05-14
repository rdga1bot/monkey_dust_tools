# monkey_dust — Tools

Editor, asset converter, and Flare demo viewer for the monkey_dust engine.
Depends on [monkey\_dust\_engine](https://github.com/rdga1bot/monkey_dust_engine) as a submodule.

> **Full documentation →** [rdga1bot.github.io/monkey\_dust\_engine/monkey\_dust\_docs.html](https://rdga1bot.github.io/monkey_dust_engine/monkey_dust_docs.html)

---

## Targets

### `monkey_dust_editor` — Wicked-style Level Editor

ImGui-based editor compiled into the engine binary via `MONKEY_DUST_EDITOR=ON`.
Runs in a separate window with full SDL\_GPU rendering behind it.

**14 panels:**

| # | Panel | Description |
|---|-------|-------------|
| 0 | Hierarchy | Entity tree; multi-select; drag to reparent |
| 1 | Inspector | Component editing with undo/redo (256 steps) |
| 2 | Assets | Asset browser with texture preview |
| 3 | Console | Log output + live Lua REPL (ImGuiColorTextEdit) |
| 4 | Graphics | Renderer state: SSAO / SMAA / shadow cascade toggles |
| 5 | Camera | Orbit ↔ Flythrough mode; FOV; near/far clip |
| 6 | Animation | AnimationSoA clip table; play / stop |
| 7 | *(reserved)* | — |
| 8 | ViewCone Inspector | SenseComponent activation bars + ViewConeSet table |
| 9 | FlowGraph | imnodes visual graph; node/var/pending display; trigger fire |
| 10 | Director | Menace gauge; stage color; profile params; manual override |
| 11 | GPU Profiler | imgui-flame-graph; per-pass CPU timings and budget bars |
| 12 | Node Graph | Generic imnodes workspace |
| 13 | Sequencer | ImSequencer timeline |

**Toolbar:**
- New Entity popup (Transform / NPC Bandit / NPC Trader / NPC Holy / Building)
- Gizmo mode buttons (Translate W · Rotate E · Scale R)
- World ↔ Local space toggle (G)
- Physics pause toggle
- FPS counter

**Command Palette** (`Ctrl+P`):
Fuzzy-scored command search (+1 prefix · +4 acronym · +2 substring) across all editor actions.

**Menu bar:** File · Edit (Undo/Redo, Duplicate, Delete, Select All) · View · Scene · Debug · Help

**Camera:**
- *Orbit*: RMB drag rotates around `cam_target`; scroll = zoom
- *Flythrough*: hold RMB + WASD flies in look direction; Q/E = up/down

---

### `md_flare_convert` — FLARE INI → JSON Converter

Converts FLARE engine `.ini` tileset / map files to the monkey\_dust JSON format.

```bash
./md_flare_convert <input.ini> <output.json>
```

---

### `md_flare_demo` — Standalone Flare Tile Viewer

Renders a FLARE map file using the engine's `TileMapRenderer` without the game layer.
Useful for validating atlas packing, animation timing, and TINST stride correctness.

```
Controls: WASD = pan · Scroll = zoom · F3 = stats overlay
```

---

## Build

```bash
cmake -S .. -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_SDL3=ON
ninja -C build monkey_dust_editor    # editor
ninja -C build md_flare_convert      # converter
ninja -C build md_flare_demo         # demo viewer
```

Editor with MONKEY\_DUST\_EDITOR panels compiled into the game binary:
```bash
cmake -S .. -B build -G Ninja -DUSE_SDL3=ON -DMONKEY_DUST_EDITOR=ON
ninja -C build monkey_dust           # game + editor overlay
```

---

## Architecture

```
tools/
  editor/              ← ImGui wicked-style editor (MONKEY_DUST_EDITOR)
    editor_core.*      ← EditorCore singleton; camera; undo history
    editor_toolbar.*   ← Menu bar + button bar + hotkeys
    editor_hierarchy.* ← Entity tree panel
    editor_inspector.* ← Component inspector
    editor_console.*   ← Log panel + Lua REPL
    editor_map_view.*  ← M9 map editor (FBO viewport + tile palette)
    editor_*_panel.*   ← Specialist panels (ViewCone, FlowGraph, Director, GPU Profiler …)
    scene_serializer.h ← Import/Export scene JSON
    editor_game_context.h ← Callback bridge to game-side systems (dialog/quest reload)
  flare_convert/       ← FLARE INI converter
  flare_demo/          ← Standalone tile viewer
  flare_2d_render.py   ← Python helper: renders a map frame to PNG (offline preview)
```

**Dependency rule:** `tools/` depends only on `engine/` (via `<monkey_dust/...>` headers).
Zero includes from `game/`. Game-specific callbacks are injected at runtime via `EditorGameContext`.

---

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).
