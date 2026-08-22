// SDL3-specific half of EditorCore's camera update (task #537,
// panel-porting follow-up). Split out of editor_core.cpp so that file
// -- and the panels that only need EditorCore's declarations -- can
// compile without <SDL3/SDL.h> at all, which the LibGodot editor
// target requires (it links no SDL3).
//
// UpdateEditorCamera() gathers input via raw SDL3 calls that input.h's
// abstraction doesn't cover (unlimited relative mouse delta, direct
// keyboard scancode array) and forwards plain floats/bools to
// EditorCore::UpdateEditorCameraFromInput() (editor_core.cpp), which
// has zero platform dependency and contains 100% of the actual
// movement/orbit math, unchanged from before this split.
#ifdef MONKEY_DUST_EDITOR
#include "editor_core.h"
#include <monkey_dust/platform/input.h>
#include <monkey_dust/platform/window.h>  // _wnd::ptr() for RelativeMouseMode
#include <SDL3/SDL.h>
#include "imgui.h"

void EditorCore::UpdateEditorCamera(float dt, bool viewport_hovered) {
    (void)viewport_hovered;

    ImGuiIO& io = ImGui::GetIO();

    // Relative mouse mode for unlimited 360° rotation on RMB (no window-edge clamping).
    bool rmb_down = io.MouseDown[ImGuiMouseButton_Right];
    {
        static bool s_rel = false;
        if (rmb_down != s_rel) {
            SDL_SetWindowRelativeMouseMode(_wnd::ptr(), rmb_down);
            s_rel = rmb_down;
        }
    }
    // SDL raw delta — unlimited, not clamped to window edges.
    float rdx = 0.f, rdy = 0.f;
    SDL_GetRelativeMouseState(&rdx, &rdy);
    if (!rmb_down) { rdx = 0.f; rdy = 0.f; }

    const bool* kb = (const bool*)SDL_GetKeyboardState(nullptr);
    bool shift = kb[SDL_SCANCODE_LSHIFT] || kb[SDL_SCANCODE_RSHIFT];
    bool key_w = kb[SDL_SCANCODE_W]||kb[SDL_SCANCODE_UP];
    bool key_s = kb[SDL_SCANCODE_S]||kb[SDL_SCANCODE_DOWN];
    bool key_a = kb[SDL_SCANCODE_A];
    bool key_d = kb[SDL_SCANCODE_D];
    bool key_q = kb[SDL_SCANCODE_Q]||kb[SDL_SCANCODE_PAGEDOWN];
    bool key_e = kb[SDL_SCANCODE_E]||kb[SDL_SCANCODE_PAGEUP];
    // Use input_get_scroll_y() — io.MouseWheel is 0 here because
    // UpdateEditorCamera() runs BEFORE imgui_new_frame() in the game loop.
    float wheel = input_get_scroll_y();
    if (wheel == 0.f) wheel = io.MouseWheel; // fallback for standalone editor

    UpdateEditorCameraFromInput(dt, rmb_down, rdx, rdy, key_w, key_a, key_s, key_d,
                                key_q, key_e, shift, wheel);
}
#endif // MONKEY_DUST_EDITOR
