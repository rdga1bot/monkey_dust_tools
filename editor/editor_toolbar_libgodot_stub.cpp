// LibGodot editor stubs (task #537 panel-porting). Two genuinely
// SDL_GPU-coupled pieces that editor_core.cpp/editor_std_commands.cpp/
// editor_command_palette.cpp call, with no libgodot equivalent yet:
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
// Both are EXCLUDED from the SDL3 monkey_dust_editor target (which uses
// the real implementations, tools/editor/editor_toolbar.cpp + main.cpp)
// -- see tools/CMakeLists.txt.
#ifdef MONKEY_DUST_EDITOR
#include "editor_toolbar.h"

void EditorToolbar::Draw(float) {}
void EditorToolbar::SpawnEntity(const char*) {}
void EditorReloadAllShaderPipelines() {}
#endif
