# monkey_dust/tools — editor + Flare converters

## Binaries
1. `monkey_dust_editor` — Wicked-style scene/asset editor (rlImGui).
   Збірка: `MONKEY_DUST_EDITOR=ON` (компілюється у головний `monkey_dust` binary
   як панелі) АБО окремий target.
2. `md_flare_convert` — CLI: Flare .txt → native JSON.

## Dependencies
- `monkey_dust_editor` лінкує: engine + game/src/ headers (для components/,
  потрібно для ItemEditor/FactionEditor які знають про Flare-семантику).
- `md_flare_convert` лінкує: ТІЛЬКИ engine (без game).

## Editor knows about Flare semantics
ItemEditor / FactionEditor / EditorFlareBrowser — bundled. Це свідомий компроміс:
tools = ALL gameplay-aware editing utilities.

> ItemEditor.h де-факто **game-specific**, бо знає поля `flare_items.json`.
> При майбутньому split-у на 3 repo tools repo потребуватиме game як
> git submodule. Прийнято як обмеження для одного-розробника проекту.

## Convert-once pipeline (важливо)
md_flare_convert конвертує Flare upstream `.txt` → нативні JSON ОДИН РАЗ під
час asset-prep. Game у runtime НЕ парсить .txt. FlareIniConverter живе ТІЛЬКИ
у `tools/flare_convert/`, НЕ у `game/`.

## Forbidden (доповнення до CLAUDE_CONSTITUTION.md §2.2)
- НЕ запускати editor без MD_OPENGL43=ON (rlImGui + 3D viewport).
- НЕ зберігати editor state у game .mdsave файлах.
- `md_flare_convert` НЕ має лінкуватись з game/ (це чистий CLI).
