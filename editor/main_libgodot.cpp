// LibGodot migration -- editor entry point (task #537, Крок 3a).
// Mirrors game/src/main_libgodot.cpp's own pattern: own target
// (monkey_dust_libgodot_editor, USE_LIBGODOT-gated, tools/CMakeLists.txt),
// own single source file, does NOT touch tools/editor/main.cpp (SDL3
// path, target monkey_dust_editor) at all -- no dual-render in one
// process, same precedent as the game side.
//
// Scope of THIS step (3a + input follow-up): prove the CMake target
// links, the RenderingServer-backed ImGui adapter (imgui_impl_
// renderingserver.h, Фаза E.1, already live-verified by the game HUD)
// draws real editor panel content on screen, AND real mouse + keyboard
// input (position + left/right/middle buttons via input.h's renderer-
// agnostic input_mouse_x/y/down(); the curated KEY_* set via
// input_key_down(), see FeedKeyboardToImGui() below) reaches
// ImGui::GetIO() so panels are genuinely clickable/navigable, not just
// drawn. Deliberately NOT in scope yet (tracked as remaining Крок 3
// work in CLAUDE_STATE.md):
//   - full UTF8 text typing (io.AddInputCharacterUTF8 + a much larger
//     key set than input.h's curated game-hotkey subset -- needed for
//     renaming assets, search boxes)
//   - mouse wheel (input.h's own comment: Godot represents it as
//     discrete WHEEL_UP/DOWN button events, needs a SceneTree _input()
//     hookup, unspiked)
//   - the 68 editor_*.h/.cpp panels themselves (this file draws ONE
//     generic smoke-test panel; the rest follow incrementally, excluding
//     any panel that turns out to reach render/scene_render.h
//     transitively, mirroring game/CMakeLists.txt's own
//     LIBGODOT_GAME_ECS_SOURCES filter comment: "exact remaining gaps
//     surface as real compile/link errors, not guesswork -- fix forward
//     from there")
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

// Keyboard follow-up (task #537): input.h's KEY_* set is a deliberately
// small, curated subset (game hotkeys, not full text entry -- no full
// alphabet/punctuation, see input.h's own scancode_table() comment).
// Feeds what's there into ImGui::GetIO() via AddKeyEvent -- enough for
// navigation (arrows/Tab/Escape/Enter/Space/Backspace) and shortcuts
// (Ctrl/Shift + F-keys/digits/letters), not yet full UTF8 text typing
// (needs io.AddInputCharacterUTF8 + a much larger key set, separate
// follow-up). Local to this TU (not a shared header) -- small, and the
// project's own convention favors this over a premature abstraction.
static void FeedKeyboardToImGui(ImGuiIO& io) {
    struct KeyMap { int md_key; ImGuiKey imgui_key; };
    static const KeyMap kKeys[] = {
        {KEY_A, ImGuiKey_A}, {KEY_B, ImGuiKey_B}, {KEY_D, ImGuiKey_D},
        {KEY_E, ImGuiKey_E}, {KEY_F, ImGuiKey_F}, {KEY_M, ImGuiKey_M},
        {KEY_R, ImGuiKey_R}, {KEY_S, ImGuiKey_S}, {KEY_T, ImGuiKey_T},
        {KEY_W, ImGuiKey_W}, {KEY_X, ImGuiKey_X}, {KEY_Y, ImGuiKey_Y},
        {KEY_Z, ImGuiKey_Z},
        {KEY_ONE, ImGuiKey_1}, {KEY_TWO, ImGuiKey_2},
        {KEY_THREE, ImGuiKey_3}, {KEY_FOUR, ImGuiKey_4},
        {KEY_ESCAPE, ImGuiKey_Escape}, {KEY_TAB, ImGuiKey_Tab},
        {KEY_F1, ImGuiKey_F1}, {KEY_F2, ImGuiKey_F2}, {KEY_F3, ImGuiKey_F3},
        {KEY_F4, ImGuiKey_F4}, {KEY_F5, ImGuiKey_F5}, {KEY_F6, ImGuiKey_F6},
        {KEY_F7, ImGuiKey_F7}, {KEY_F8, ImGuiKey_F8}, {KEY_F9, ImGuiKey_F9},
        {KEY_F10, ImGuiKey_F10},
        {KEY_LEFT_CONTROL, ImGuiKey_LeftCtrl}, {KEY_RIGHT_CONTROL, ImGuiKey_RightCtrl},
        {KEY_LEFT_SHIFT, ImGuiKey_LeftShift}, {KEY_RIGHT_SHIFT, ImGuiKey_RightShift},
        {KEY_LEFT, ImGuiKey_LeftArrow}, {KEY_RIGHT, ImGuiKey_RightArrow},
        {KEY_UP, ImGuiKey_UpArrow}, {KEY_DOWN, ImGuiKey_DownArrow},
        {KEY_ENTER, ImGuiKey_Enter}, {KEY_SPACE, ImGuiKey_Space},
        {KEY_BACKSPACE, ImGuiKey_Backspace},
    };
    for (const auto& k : kKeys) {
        io.AddKeyEvent(k.imgui_key, input_key_down(k.md_key));
    }
    io.AddKeyEvent(ImGuiMod_Ctrl, input_key_down(KEY_LEFT_CONTROL) || input_key_down(KEY_RIGHT_CONTROL));
    io.AddKeyEvent(ImGuiMod_Shift, input_key_down(KEY_LEFT_SHIFT) || input_key_down(KEY_RIGHT_SHIFT));
}

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
        input_begin_frame();
        window_begin_frame();

        io.DisplaySize = ImVec2((float)window_get_width(), (float)window_get_height());
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(input_mouse_x(), input_mouse_y());
        io.AddMouseButtonEvent(0, input_mouse_down(MOUSE_BUTTON_LEFT));
        io.AddMouseButtonEvent(1, input_mouse_down(MOUSE_BUTTON_RIGHT));
        io.AddMouseButtonEvent(2, input_mouse_down(MOUSE_BUTTON_MIDDLE));
        FeedKeyboardToImGui(io);
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
