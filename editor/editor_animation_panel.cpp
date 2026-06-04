#ifdef MONKEY_DUST_EDITOR
#include "editor_animation_panel.h"
#include "editor_core.h"
#include "imgui.h"


void EditorAnimationPanel::Draw() {
    if (!EditorCore::Get().panels_visible[6]) return;

    ImGui::SetNextWindowSize(ImVec2(300, 340), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(570, 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Animation##AnimPanel", &EditorCore::Get().panels_visible[6])) {
        ImGui::End();
        return;
    }
    DrawContent();
    ImGui::End();
}

void EditorAnimationPanel::DrawContent() {
    ImGui::TextDisabled("GPU skinning: AnimationSoA + OzzAnimator (SDL_GPU path).");
    ImGui::TextDisabled("Toggle anim debug overlay: F12 in-game.");
}
#endif // MONKEY_DUST_EDITOR
