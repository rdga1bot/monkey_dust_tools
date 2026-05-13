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

    ImGui::TextDisabled("Animation requires Phase 31 (MD_OPENGL43_ENABLED).");
    ImGui::TextDisabled("Rebuild with -DMD_OPENGL43=ON to enable GPU skinning.");
    ImGui::End();
    return;
}
#endif // MONKEY_DUST_EDITOR
