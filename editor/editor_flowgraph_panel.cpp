#ifdef MONKEY_DUST_EDITOR
#include "editor_flowgraph_panel.h"
#include "editor_core.h"
#include "imgui.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/scripting/flow_graph.h>
#include <monkey_dust/ai/fnv.h>

static const char* s_node_type_name(uint8_t t) {
    switch (t) {
    case 0: return "Event";
    case 1: return "Condition";
    case 2: return "Action";
    case 3: return "Delay";
    case 4: return "Variable";
    default: return "?";
    }
}

void EditorFlowGraphPanel::Draw() {
    if (!EditorCore::Get().panels_visible[9]) return;

    ImGui::SetNextWindowSize(ImVec2(380, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(340, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("FlowGraph##fg", &EditorCore::Get().panels_visible[9])) {
        ImGui::End(); return;
    }

    auto& reg = Registry::Get();

    // Prefer selected entity; fall back to first FlowGraph in registry.
    entt::entity target = entt::null;
    entt::entity sel = EditorCore::Get().GetPrimary();
    if (sel != entt::null && reg.valid(sel) && reg.all_of<FlowGraph>(sel))
        target = sel;
    if (target == entt::null) {
        auto view = reg.view<FlowGraph>();
        if (!view.empty()) target = *view.begin();
    }

    if (target == entt::null) {
        ImGui::TextDisabled("No FlowGraph entity in scene.");
        ImGui::End(); return;
    }

    auto& fg = reg.get<FlowGraph>(target);
    ImGui::Text("Entity: %u  |  Nodes: %d  Conns: %d  Vars: %d",
                (uint32_t)target, fg.node_count, fg.conn_count, fg.var_count);

    // ── Nodes ─────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Nodes", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("nodes##fg", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
            ImVec2(0, 120)))
        {
            ImGui::TableSetupColumn("ID (hex)");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Out-conns");
            ImGui::TableHeadersRow();
            for (int i = 0; i < fg.node_count; ++i) {
                const FlowNode& n = fg.nodes[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%08X", n.id);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(s_node_type_name(n.type));
                ImGui::TableNextColumn(); ImGui::Text("%u", n.conn_count);
            }
            ImGui::EndTable();
        }
    }

    // ── Variables (editable) ──────────────────────────────────────────
    if (ImGui::CollapsingHeader("Variables", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int i = 0; i < fg.var_count; ++i) {
            FlowVar& v = fg.vars[i];
            char label[32];
            snprintf(label, sizeof(label), "0x%08X##fgv%d", v.key, i);
            ImGui::SetNextItemWidth(120);
            ImGui::DragFloat(label, &v.value, 0.01f);
        }
        if (fg.var_count == 0) ImGui::TextDisabled("(none)");
    }

    // ── Pending triggers ──────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Pending Triggers")) {
        int h = fg.pending_head, t = fg.pending_tail;
        int cnt = (t - h + FlowGraph::MAX_PENDING) % FlowGraph::MAX_PENDING;
        if (cnt == 0) { ImGui::TextDisabled("(ring buffer empty)"); }
        for (int i = 0; i < cnt; ++i) {
            const FlowPendingTrigger& pt =
                fg.pending[(h + i) % FlowGraph::MAX_PENDING];
            ImGui::Text("[%d] node=%08X  fire_at=%.3fs", i, pt.node_id,
                        (float)pt.fire_at_s);
        }
    }

    // ── Fire trigger ──────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Fire External Trigger")) {
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("Node name##fg", fire_node_name_,
                         sizeof(fire_node_name_));
        ImGui::SameLine();
        if (ImGui::Button("Fire##fg")) {
            if (fire_node_name_[0] != '\0') {
                uint32_t id = md::fnv1a(fire_node_name_);
                fg.FireTrigger(id, 0.0);
            }
        }
    }

    ImGui::End();
}
#endif
