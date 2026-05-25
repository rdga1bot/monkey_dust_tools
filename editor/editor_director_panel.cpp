#ifdef MONKEY_DUST_EDITOR
#include "editor_director_panel.h"
#include "editor_core.h"
#include "imgui.h"
#include <monkey_dust/ai/director_system.h>

static const char* s_stage_name(DirectorStage s) {
    switch (s) {
    case DirectorStage::Unaware:    return "Unaware";
    case DirectorStage::Suspicious: return "Suspicious";
    case DirectorStage::Hunting:    return "Hunting";
    case DirectorStage::Intense:    return "Intense";
    default: return "?";
    }
}

static ImVec4 s_stage_color(DirectorStage s) {
    switch (s) {
    case DirectorStage::Unaware:    return ImVec4(0.4f, 0.8f, 0.4f, 1.f);
    case DirectorStage::Suspicious: return ImVec4(1.0f, 0.8f, 0.1f, 1.f);
    case DirectorStage::Hunting:    return ImVec4(1.0f, 0.4f, 0.0f, 1.f);
    case DirectorStage::Intense:    return ImVec4(1.0f, 0.1f, 0.1f, 1.f);
    default: return ImVec4(1,1,1,1);
    }
}

void EditorDirectorPanel::Draw() {
    if (!EditorCore::Get().panels_visible[10]) return;

    ImGui::SetNextWindowSize(ImVec2(300, 360), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(670, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Director Tuning##dir", &EditorCore::Get().panels_visible[10])) {
        ImGui::End(); return;
    }
    DrawContent();
    ImGui::End();
}

void EditorDirectorPanel::DrawContent() {

    auto& ds = DirectorSystem::Get();

    // ── Menace gauge ──────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Menace Gauge", ImGuiTreeNodeFlags_DefaultOpen)) {
        float menace = ds.GetMenace();
        DirectorStage stage = ds.GetStage();

        // Coloured progress bar
        ImVec4 col = s_stage_color(stage);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(menace, ImVec2(-1, 20));
        ImGui::PopStyleColor();

        ImGui::TextColored(col, "Stage: %s  (%.3f)", s_stage_name(stage), menace);
        ImGui::Spacing();

        // Optional manual override for debugging
        bool overriding = (override_menace_ >= 0.0f);
        if (ImGui::Checkbox("Override menace##dir", &overriding)) {
            override_menace_ = overriding ? menace : -1.0f;
        }
        if (overriding) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            ImGui::SliderFloat("##ov", &override_menace_, 0.0f, 1.0f);
        }
    }

    // ── Stage thresholds (read-only reference) ────────────────────────
    if (ImGui::CollapsingHeader("Stage Thresholds")) {
        ImGui::Text("Unaware    < 0.25");
        ImGui::Text("Suspicious  0.25 – 0.50");
        ImGui::Text("Hunting     0.50 – 0.75");
        ImGui::Text("Intense    > 0.75");
    }

    // ── Current profile ───────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Active Profile", ImGuiTreeNodeFlags_DefaultOpen)) {
        const DirectorProfile* p = ds.GetCurrentProfile();
        if (!p) {
            ImGui::TextDisabled("No profiles loaded.");
        } else {
            ImGui::Text("Name: %s", p->name);
            ImGui::Separator();

            float fill = p->gauge_fill_rate;
            ImGui::Text("Fill rate:       %.3f /s", fill);
            ImGui::Text("Hunt timeout:    %.1f – %.1f s",
                        p->hunt_timeout_min_s, p->hunt_timeout_max_s);
            ImGui::Text("Ambush wait:     %.1f s", p->ambush_wait_s);
            ImGui::Text("Trap trigger:    %.1f s", p->trap_trigger_s);
            ImGui::Text("Sweep radius:    %.1f m", p->sweep_radius_m);
            ImGui::Text("Max menaces:     %d",     p->max_menaces);
        }
    }

}
#endif
