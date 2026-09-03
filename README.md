---
id: kb-tools-readme
type: reference
status: active
date: 2026-05-14
updated: 2026-08-23
repo: tools
tags: [tools, readme, editor, public-repo, shader-hot-reload]
summary: "Public tools/ README: editor panels, shader hot-reload, QA scripts, build targets, repo architecture"
---

# monkey_dust — Tools

Editor, asset converter, terrain pipeline, and Flare demo viewer for the monkey_dust engine.
Depends on [monkey\_dust\_engine](https://github.com/rdga1bot/monkey_dust_engine) as a submodule.

All tool names use the `md_` prefix (no proprietary asset branding in public repos).

> **Render backend: SDL3/SDL_GPU, permanently.** An embedded LibGodot RenderingServer replacement for the editor's `monkey_dust_editor`/`monkey_dust_editor_panels` targets was explored but **rejected 2026-08-23** after a measured ~50% GPU perf regression on target hardware. See the parent [`monkey_dust`](https://github.com/rdga1bot/monkey_dust) repo's `docs/SDL3_TO_LIBGODOT_MIGRATION_AUDIT.md` for status. All LibGodot code was fully removed 2026-09-03.

> **Full documentation →** [rdga1bot.github.io/monkey\_dust\_engine/monkey\_dust\_docs.html](https://rdga1bot.github.io/monkey_dust_engine/monkey_dust_docs.html)

---

## Targets

### `monkey_dust_editor` — Wicked-style Level Editor

Standalone SDL\_GPU/Vulkan editor (RD-3: migrated from OpenGL).
All ImGui libraries (core + extensions) live in `tools/third_party/`: imgui, imnodes, imgui-node-editor, ImGuiColorTextEdit, imguizmo, imgui-flame-graph, imgui-command-palette.
Zero dependencies on `game/` sources — compiles without the game repo present (`MONKEY_DUST_STANDALONE_EDITOR` is always defined for this target).

**11 standalone panels:**

| # | Panel | Description |
|---|-------|-------------|
| 0 | Hierarchy | Entity tree; multi-select; drag to reparent |
| 1 | Assets | Asset browser with texture preview |
| 2 | Console | Log output + live Lua REPL (ImGuiColorTextEdit) |
| 3 | Graphics | Renderer state: SSAO / SMAA / shadow cascade toggles |
| 4 | Camera | Orbit ↔ Flythrough mode; FOV; near/far clip |
| 5 | Animation | GPU skinning: AnimationSoA + OzzAnimator; clip table; play / stop |
| 6 | ViewCone Inspector | SenseComponent activation bars + ViewConeSet table |
| 7 | FlowGraph | imnodes visual graph; node/var/pending display; trigger fire |
| 8 | Director | Menace gauge; stage color; profile params; manual override |
| 9 | GPU Profiler | imgui-flame-graph; per-pass CPU timings and budget bars |
| 10 | Sequencer | ImSequencer timeline |

> **Game-coupled panels** (Inspector · Terrain) are compiled into the game binary via
> `-DMONKEY_DUST_EDITOR=ON` and live in `game/src/editor/` — they require `game/` headers and are
> never part of the standalone `tools/` build. **3D World and Settings panels are NOT game-coupled**
> — they live in `tools/editor/` (`editor_world_3d_sdlgpu.h/.cpp`, `settings_editor.h`), depend only
> on `engine/` + SDL3, and ARE part of the standalone `monkey_dust_editor` build.
>
> **3D World is view-only** (2026-07-19) — the whole-world (64×64 zone) viewport is for navigation/
> preview only, no in-viewport terrain brush. Real terrain sculpting is the in-game F3 "Terrain
> Sculpt" panel (`game/src/editor/editor_terrain_panel.cpp`), which writes directly to the real
> Kenshi `TerrainAtlas`. The old 2D "Heightmap" brush tab and the 3D World's own macro-geography
> brush both painted a separate, now-removed synthetic `md_master_hmap` layer that only ever guided
> procedural generation (also removed) — neither ever touched real terrain, so removing them lost no
> real editing capability.

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

### Shader hot-reload — zero-restart shader iteration

```bash
# Terminal 2: auto-recompile on .vert/.frag/.comp save
bash tools/shader_watch.sh

# In editor console after compilation:
/reload-shaders   # reloads all GPU pipelines without restart
```

`tools/shader_watch.sh` uses `inotifywait` (inotify-tools) to watch `shaders/`; falls back to 5-second polling if not available.
`/reload-shaders` calls `GpuPipeline::Reload()` on all char-preview pipelines after running `compile_shaders.sh`.

**RenderDoc integration:**
```bash
SDL_GPU_DEBUGMODE=1 renderdoc build/tools/monkey_dust_editor   # editor
SDL_GPU_DEBUGMODE=1 renderdoc build/game/monkey_dust            # game
# F12 = capture frame → inspect uniform buffers, depth texture, per-draw state
```

**GPU debug overlay** (Ctrl+D in Characters tab): real-time `hair_color`, `eye_pos`, pipeline status, bone21 diagonal. Red highlight when `hair_color` = white (HairFU binding error).

---

### `tools/qa/char_preview_qa.sh` — Character Preview Visual Regression

```bash
bash tools/qa/char_preview_qa.sh --save    hair_front   # baseline from latest screenshot
bash tools/qa/char_preview_qa.sh --compare hair_front   # pixel diff (ImageMagick RMSE)
bash tools/qa/char_preview_qa.sh --list                 # list saved baselines
```

Baselines stored in `tools/qa/baselines/char_preview/`. Threshold: RMSE < 0.02.

> **Note:** `tools/qa/captures/` (PNG frames + JSONL logs from `qa_run.sh`) is gitignored — runtime artifacts only, not tracked in the repo.

---

### `md_extract_terrain.py` — REMOVED (2026-07-10)

Used to build `world_hmap.r32` from `fullmap.tif`, but with a stale
`HEIGHT_MAX = 300.0` constant that conflicted with the real, current value
(`980.0`, real Kenshi scale — see
`re/re_docs/kenshi/impl_status/CLAUDE_KEN_TERRAIN_SCALE_FIX.md`). Two
competing, simultaneously-documented ways to build the same artifact was
a confirmed real risk (`docs/ENGINE_AUDIT.md` §1/§2) — deleted rather than
left deprecated-in-place, per the resolved decision in
`TERRAIN_FIX_PROMPT.md` Stage 1. **Use `tools/tif_to_r32.py`** (documented
in the root `CLAUDE.md` under "Kenshi Terrain Pipeline" — the sole
authoritative path).

---

### `md_convert.py` — Mesh Pipeline

OGRE XML skeleton/mesh → cgltf GLB with embedded animations. Used for character assets.

```bash
python3 tools/md_convert.py   # → game/data/props/md_human.glb
```

---

### `md_mod_import.py` — World Data Import

Imports JSON mod data into `game/data/md_world.json`. Uses `md_id` as the canonical identifier key.

---

### `md_flare_convert` — FLARE INI → JSON Converter

Converts FLARE engine `.ini` tileset / map files to the monkey\_dust JSON format.

```bash
./md_flare_convert <input.ini> <output.json>
```

---

### `md_flare_demo` — Standalone Flare Tile + 3D World Viewer

Renders a FLARE map file using the engine's `TileMapRenderer` without the game layer.
Press **S** to toggle between 2D isometric tile view and 3D world view.

**2D mode:** validates atlas packing, animation timing, NPC sprite overlay, `uint64_t` depth sort, `fadeOverlapTile` transparency.

**3D mode:** exercises the full SDL_GPU pipeline — GPU NPC frustum culling (`npc_cull.comp`), EVSM soft shadows (3-cascade CSM with texel-snap), OIT transparent quads (2-MRT compute composite), deferred lighting, SSAO, SMAA.

```
Controls: WASD = pan/fly · Scroll = zoom · S = toggle 2D/3D · F3 = stats overlay
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
  editor/              ← ImGui wicked-style editor (standalone, no game/ deps)
    editor_core.*      ← EditorCore singleton; camera; undo history
    editor_toolbar.*   ← Menu bar + button bar + hotkeys
    editor_hierarchy.* ← Entity tree panel
    editor_console.*   ← Log panel + Lua REPL
    editor_map_view.*  ← M9 map editor (FBO viewport + tile palette)
    editor_world_panel.h ← World tab: Zone/Faction/Town + map preview
    editor_*_panel.*   ← Specialist panels (ViewCone, FlowGraph, Director, GPU Profiler …)
    scene_serializer.h ← Import/Export scene JSON
  md_terrain/          ← md2terrain.py terrain zone helpers
  md_mesh_conv/        ← md_chars.py · ogre2glb.py — OGRE XML → GLB
  (md_extract_terrain.py removed 2026-07-10 — see tif_to_r32.py instead)
  md_convert.py        ← OGRE mesh → GLB pipeline
  md_mod_import.py     ← JSON mod data → md_world.json (key: md_id)
  md_biome_import.py   ← Biome map JSON import
  md_heightmap_import.py ← Raw heightmap → atlas format
  md_stitch_terrain.py ← Post-process zone edge stitching
  flare_convert/       ← FLARE INI converter
  flare_demo/          ← Standalone tile viewer
  flare_2d_render.py   ← Python helper: renders a map frame to PNG (offline preview)
```

**Dependency rule:** `tools/` depends only on `engine/` (via `<monkey_dust/...>` headers) plus its own `tools/third_party/` (imgui + extensions).
Zero `#include` from `game/`. Game-coupled panels (Inspector, Terrain) live in `game/src/editor/` and
are compiled into the game binary only — they are never part of the `tools/` build. 3D World and
Settings panels live in `tools/editor/` and are part of the standalone `tools/` build.

---

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).
