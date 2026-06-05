# monkey_dust/tools — editor + Flare converters

> **Головний документ проекту:** `CLAUDE_CONSTITUTION.md` у приватному репо `monkey_dust/`.
> Всі правила §2.2 (заборони, стандарти коду, архітектурні інваріанти) діють тут повністю.

## Binaries
1. `monkey_dust_editor` — Wicked-style scene/asset editor (ImGui + SDL_GPU).
   Збірка: `MONKEY_DUST_EDITOR=ON` (компілюється у головний `monkey_dust` binary
   як панелі) АБО окремий target.
2. `md_flare_convert` — CLI: Flare .txt → native JSON.

## Dependencies (після DECOUPLE-2, 2026-06-06)
- `monkey_dust_editor` лінкує: **тільки engine** — нуль посилань на `game/`
  - game-coupled панелі (`editor_inspector`, `editor_terrain_panel`) компілюються
    у `game/` binary (MONKEY_DUST_EDITOR=ON), а не у standalone editor
  - `editor_world_3d_sdlgpu`, `settings_editor` — у `tools/editor/` (нуль game/ deps)
- `md_flare_convert` лінкує: ТІЛЬКИ stdlib (без engine, без game).

## Editor knows about Flare semantics
ItemEditor / FactionEditor / EditorFlareBrowser — bundled. Це свідомий компроміс:
tools = ALL gameplay-aware editing utilities, але тільки через engine/ API.

## Convert-once pipeline (важливо)
md_flare_convert конвертує Flare upstream `.txt` → нативні JSON ОДИН РАЗ під
час asset-prep. Game у runtime НЕ парсить .txt. FlareIniConverter живе ТІЛЬКИ
у `tools/flare_convert/`, НЕ у `game/`.

## Forbidden (доповнення до CLAUDE_CONSTITUTION.md §2.2)
- НЕ використовувати MD_OPENGL43_ENABLED — backend SDL_GPU (USE_SDL3=ON); OpenGL = legacy.
- НЕ зберігати editor state у game .mdsave файлах.
- `md_flare_convert` НЕ має лінкуватись з game/ або engine/ (чистий CLI).
- `tools/editor/` НЕ має `#include` з `game/` — split-readiness інваріант (DECOUPLE-2).
