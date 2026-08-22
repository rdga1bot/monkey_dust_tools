// LibGodot editor stubs (task #537 panel-porting). Genuinely
// SDL_GPU-coupled pieces called from panels/Lua bindings that ARE
// ported, with no libgodot equivalent yet:
//
// 1. EditorToolbar::Draw()/SpawnEntity() -- the real implementation
//    (editor_toolbar.cpp) directly includes <SDL3/SDL_events.h> and
//    editor_map_view.h (3D viewport, SDL_GPU-coupled), blocked until
//    Крок 1e (shared terrain module, task #534) and a real libgodot 3D
//    viewport panel exist. No-op here, not fake functionality: there is
//    no toolbar UI or 3D scene to draw into or spawn entities into yet
//    in this target.
//
// 2. EditorReloadAllShaderPipelines() -- reloads SDL_GPU shader
//    pipelines (GpuPipeline HAL / CharPreviewSDLGPU). RenderingServer
//    manages its own shader compilation with no equivalent hot-reload
//    hook wired up yet. No-op here for the same reason.
//
// 3. EditorScreenshot_RequestPending() -- the real implementation
//    (editor_screenshot.cpp) reads back an SDL_GPU swapchain texture.
//    No-op here; md.editor.capture() is a no-op in this target until a
//    RenderingServer-side capture path exists.
//
// 4. editor_panels_dump_state() -- the real implementation
//    (editor_panels_entry.cpp) walks SDL_GPU/editor_map_view state
//    (3D viewport, blocked on Крок 1e same as #1). No-op here;
//    md.editor.dump() writes nothing extra beyond what's already ported.
//
// All EXCLUDED from the SDL3 monkey_dust_editor target (which uses the
// real implementations: editor_toolbar.cpp, editor_screenshot.cpp,
// editor_panels_entry.cpp, main.cpp) -- see tools/CMakeLists.txt.
#ifdef MONKEY_DUST_EDITOR
#include "editor_toolbar.h"

void EditorToolbar::Draw(float) {}
void EditorToolbar::SpawnEntity(const char*) {}
void EditorReloadAllShaderPipelines() {}
void EditorScreenshot_RequestPending(const char*) {}
extern "C" void editor_panels_dump_state(void*) {}
#endif
