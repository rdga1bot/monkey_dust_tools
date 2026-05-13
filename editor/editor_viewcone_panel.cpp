#ifdef MONKEY_DUST_EDITOR
#include "editor_viewcone_panel.h"
#include "editor_core.h"
#include "imgui.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/components/sense_component.h>
#include <monkey_dust/ai/sense_registry.h>

static const char* s_type_names[] = { "Close", "Focused", "Normal", "Peripheral" };

void EditorViewConePanel::Draw() {
    if (!EditorCore::Get().panels_visible[8]) return;

    ImGui::SetNextWindowSize(ImVec2(320, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("ViewCone Inspector##vc", &EditorCore::Get().panels_visible[8])) {
        ImGui::End(); return;
    }

    auto& reg = Registry::Get();
    entt::entity e = EditorCore::Get().GetPrimary();

    if (e == entt::null || !reg.valid(e) || !reg.all_of<SenseComponent>(e)) {
        ImGui::TextDisabled("Select an entity with SenseComponent.");
        ImGui::End(); return;
    }

    auto& sc = reg.get<SenseComponent>(e);

    // ── Activation bars ────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Activation", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Visual");
        ImGui::SameLine(70);
        float vis = sc.activation[0];
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
            vis > sc.threshold_hi ? ImVec4(1,0,0,1) :
            vis > sc.threshold_lo ? ImVec4(1,0.7f,0,1) : ImVec4(0,0.7f,0.2f,1));
        ImGui::ProgressBar(vis, ImVec2(-1, 14));
        ImGui::PopStyleColor();

        ImGui::Text("Audio");
        ImGui::SameLine(70);
        float aud = sc.activation[1];
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
            aud > sc.threshold_hi ? ImVec4(1,0,0,1) :
            aud > sc.threshold_lo ? ImVec4(1,0.7f,0,1) : ImVec4(0,0.4f,1,1));
        ImGui::ProgressBar(aud, ImVec2(-1, 14));
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Text("Threshold lo: %.2f   hi: %.2f", sc.threshold_lo, sc.threshold_hi);
    }

    // ── Last known position ────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Last Known Position")) {
        if (sc.last_known_x != 0.0f || sc.last_known_z != 0.0f)
            ImGui::Text("(%.1f, %.1f)", sc.last_known_x, sc.last_known_z);
        else
            ImGui::TextDisabled("No detection yet.");
    }

    // ── ViewConeSet parameters ─────────────────────────────────────────
    const ViewConeSet* vcs = SenseRegistry::Get().At(sc.cone_set_idx);
    if (!vcs) {
        ImGui::TextDisabled("cone_set_idx=%u — not loaded", sc.cone_set_idx);
        ImGui::End(); return;
    }

    if (ImGui::CollapsingHeader("ViewConeSet", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Name: %s  (idx=%u)", vcs->name, sc.cone_set_idx);
        ImGui::Text("Cones: %u", vcs->cone_count);

        ImGui::Separator();
        if (ImGui::BeginTable("cones##vc", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("HAngle");
            ImGui::TableSetupColumn("Length");
            ImGui::TableSetupColumn("Dist lo/hi");
            ImGui::TableHeadersRow();
            for (int i = 0; i < vcs->cone_count && i < MAX_VIEW_CONE_TYPES; ++i) {
                const ViewCone& c = vcs->cones[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(s_type_names[i]);
                ImGui::TableNextColumn(); ImGui::Text("%.0f°", c.h_angle_deg);
                ImGui::TableNextColumn(); ImGui::Text("%.1fm", c.length_m);
                ImGui::TableNextColumn(); ImGui::Text("%.1f/%.1f", c.dist_lo, c.dist_hi);
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}
#endif
