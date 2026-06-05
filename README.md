# monkey_dust — Tools

Editor, asset converter, terrain pipeline, and Flare demo viewer for the monkey_dust engine.
Depends on [monkey\_dust\_engine](https://github.com/rdga1bot/monkey_dust_engine) as a submodule.

All tool names use the `md_` prefix (no proprietary asset branding in public repos).

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

> **Game-coupled panels** (Inspector · Terrain · 3D World · Settings) are compiled into the game binary via `-DMONKEY_DUST_EDITOR=ON` and live in `game/src/editor/` — they require `game/` headers and are never part of the standalone `tools/` build.

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

### `ozz_bake` — GLB → ozz Animation Converter

Offline converter: reads a GLB file with embedded skeleton and animations, writes `.ozz` skeleton + per-clip animation files consumed by `OzzAnimPlayer`.

```bash
ninja -C build ozz_bake
./build/tools/ozz_bake   # input: game/data/props/md_human.glb
                          # output: game/data/anim/md_human.ozz
```

---

### `md_extract_terrain.py` — Terrain Atlas Builder

Extracts all 4096 terrain zones from `fullmap.tif` (16385×16385 uint16) and packs them into a single `world_hmap.r32` atlas (67 MB).

```bash
# Extract individual zone files:
python3 tools/md_extract_terrain.py extract

# Pack into atlas:
python3 tools/md_extract_terrain.py pack_atlas
# → game/data/terrain/world_hmap.r32
```

Atlas format: `magic=0x414D4800` · 4096 zones (64×64 grid) · each zone = `float hmin + float hmax + float[65×65]` heights.

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
  ozz_bake/            ← GLB → ozz animation converter
  md_terrain/          ← md2terrain.py terrain zone helpers
  md_mesh_conv/        ← md_chars.py · ogre2glb.py — OGRE XML → GLB
  md_extract_terrain.py ← fullmap.tif → 4096 zone r32 → world_hmap.r32 atlas
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
Zero `#include` from `game/`. Game-coupled panels (Inspector, Terrain, 3D World, Settings) live in `game/src/editor/` and are compiled into the game binary only — they are never part of the `tools/` build.

---

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).
