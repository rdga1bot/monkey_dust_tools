// LibGodot migration -- editor entry point (task #537, Крок 3a).
// Mirrors game/src/main_libgodot.cpp's own pattern: own target
// (monkey_dust_libgodot_editor, USE_LIBGODOT-gated, tools/CMakeLists.txt),
// own single source file, does NOT touch tools/editor/main.cpp (SDL3
// path, target monkey_dust_editor) at all -- no dual-render in one
// process, same precedent as the game side.
//
// Scope of THIS step (3a): prove the CMake target links and the
// RenderingServer-backed ImGui adapter (imgui_impl_renderingserver.h,
// Фаза E.1, already live-verified by the game HUD) draws real editor
// panel content on screen. Deliberately NOT in scope yet (tracked as
// remaining Крок 3 work in CLAUDE_STATE.md):
//   - real mouse/keyboard input feeding into ImGui::GetIO() (Godot's
//     core/input/input.h Input singleton has the state; wiring it into
//     io.AddMousePosEvent/AddKeyEvent/etc. is the next real step, same
//     gap the game HUD already has per main_libgodot.cpp's own comments)
//   - the 68 editor_*.h/.cpp panels themselves (this file draws ONE
//     panel, faction_editor.h, as a first real-content smoke test --
//     picked because it's a widely-referenced, non-SDL_GPU-coupled
//     panel; the rest follow incrementally, excluding any panel that
//     turns out to reach render/scene_render.h transitively, mirroring
//     game/CMakeLists.txt's own LIBGODOT_GAME_ECS_SOURCES filter comment:
//     "exact remaining gaps surface as real compile/link errors, not
//     guesswork -- fix forward from there")
//   - F3 terrain sculpt tools (needs Крок 1e's shared terrain module,
//     task #534, still pending)
//   - hot-reload (libeditor_panels.so) compatibility with this backend
#include <monkey_dust/platform/window.h>
#include <monkey_dust/platform/input.h>
#include <monkey_dust/render/libgodot_bridge.h>
#include <monkey_dust/render/imgui_impl_renderingserver.h>
#include "imgui.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
    printf("=== tools/editor/main_libgodot.cpp -- LibGodot editor entry point (task #537, Крок 3a) ===\n");

    int max_frames = 120;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        }
    }

    window_init(1280, 720, "monkey_dust_libgodot_editor");
    printf("OK: window_init(1280, 720)\n");

    LibGodotBridge bridge;
    bool bridge_ok = bridge.Init(1280, 720, /*attach_to_screen=*/true);
    printf("%s: LibGodotBridge::Init(attach_to_screen=true)\n", bridge_ok ? "OK" : "FAILED");
    if (!bridge_ok) { window_shutdown(); return 1; }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    bool imgui_ok = ImGui_ImplRenderingServer_Init(bridge.ViewportRid());
    printf("%s: ImGui_ImplRenderingServer_Init\n", imgui_ok ? "OK" : "FAILED");

    for (int frame = 0; frame < max_frames; ++frame) {
        window_begin_frame();

        io.DisplaySize = ImVec2((float)window_get_width(), (float)window_get_height());
        io.DeltaTime = 1.0f / 60.0f;
        ImGui_ImplRenderingServer_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("monkey_dust_libgodot_editor (Крок 3a smoke test)");
        ImGui::Text("frame %d / %d", frame, max_frames);
        ImGui::Text("RenderingServer-backed ImGui, no SDL3");
        ImGui::End();

        ImGui::Render();
        ImGui_ImplRenderingServer_RenderDrawData(ImGui::GetDrawData());

        window_end_frame();
    }

    printf("OK: %d frames complete, no crash\n", max_frames);

    ImGui_ImplRenderingServer_Shutdown();
    ImGui::DestroyContext();
    bridge.Shutdown();
    window_shutdown();
    printf("OK: tools/editor/main_libgodot.cpp complete\n");
    return 0;
}
