# tools/editor/ — CLAUDE.md

## dlopen hot-reload — критичні інваріанти
`build/hot/libeditor_panels.so` — ОКРЕМА ninja-ціль
(`monkey_dust_editor_panels`), яку `monkey_dust_editor` `dlopen()`-ить при
старті. Rebuilding `monkey_dust_editor` саму по собі лишає `.so` застарілим
→ `dlopen` undefined-symbol → Lua API мовчки зникає. Завжди білдити ОБИДВА:
```
ninja -C build monkey_dust_editor monkey_dust_editor_panels
```
Жодних кешованих ID/вказівників, що переживають F5-reload (той самий
інваріант, що в `EcsReflectBridge`). Фонові треди (`s_loader_thread`
тощо) — `.join()`, НІКОЛИ `.detach()`: `GpuDevice::Shutdown()` може
знищити Vulkan-device поки detached-тред ще вивантажує текстуру
(SIGSEGV, підтверджено coredump'ом 2026-07-26).

## `DrawContent()` invariant
Кожна панель: `Draw()` (Begin/End + visibility guard) і `DrawContent()`
(тільки вміст). F3-таби (`##f3editor`) викликають ТІЛЬКИ `DrawContent()`
— тому lazy init (`seq_.count`, `imnodes_ctx_`) МУСИТЬ бути в
`DrawContent()`, не в `Draw()`.

## ImGui — тільки #ifdef MONKEY_DUST_EDITOR
Ніколи не в реліз-білді гри. Callbacks на кшталт `FlameGetter`
(imgui-flame-graph) передають `nullptr` для деяких параметрів — завжди
null-check перед dereference.

## Editor 3D World viewport
`EDITOR_TNKN=64` (повний 64×64 світ) — ІНША система за `TNKN=9` у грі.
Synthesis VBO (256×256) — завжди фон, LOD chunks поверх через depth test.
`handle_input()` guard МУСИТЬ включати `any_move_key` — інакше WASD
зависає при виході миші за межі viewport.

Глибокий довідник: `docs/CLAUDE_ARCH.md`.
