#ifdef MONKEY_DUST_EDITOR
#include "editor_flowgraph_panel.h"
#include "editor_core.h"
#include "imgui.h"
#include "imnodes.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/scripting/flow_graph.h>
#include <monkey_dust/ai/fnv.h>

// Per-type node colors (ABGR for ImNodes IM_COL32)
static const ImU32 kNodeColors[5] = {
    IM_COL32( 60, 120, 220, 200),   // 0=Event     blue
    IM_COL32(220, 150,  40, 200),   // 1=Condition  orange
    IM_COL32(200,  50,  50, 200),   // 2=Action     red
    IM_COL32(140,  60, 220, 200),   // 3=Delay      purple
    IM_COL32( 50, 160,  80, 200),   // 4=Variable   green
};

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

void EditorFlowGraphPanel::Shutdown() {
    if (imnodes_ctx_) { ImNodes::DestroyContext(imnodes_ctx_); imnodes_ctx_ = nullptr; }
}

void EditorFlowGraphPanel::Draw() {
    if (!EditorCore::Get().panels_visible[9]) return;

    if (!imnodes_ctx_) imnodes_ctx_ = ImNodes::CreateContext();
    ImNodes::SetCurrentContext(imnodes_ctx_);

    ImGui::SetNextWindowSize(ImVec2(620, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(200, 120), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("FlowGraph##fg", &EditorCore::Get().panels_visible[9])) {
        ImGui::End(); return;
    }

    auto& reg = Registry::Get();

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
                (uint32_t)entt::to_integral(target),
                fg.node_count, fg.conn_count, fg.var_count);

    // ── imnodes visual graph ──────────────────────────────────────────
    ImGui::BeginChild("##fg_canvas", ImVec2(0, 240), true, ImGuiWindowFlags_NoScrollbar);
    ImNodes::BeginNodeEditor();

    for (int i = 0; i < fg.node_count; ++i) {
        const FlowNode& n   = fg.nodes[i];
        int             nid = (i + 1) * 100;
        ImU32           col = kNodeColors[n.type < 5 ? n.type : 0];

        ImNodes::PushColorStyle(ImNodesCol_NodeBackground,         col);
        ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundHovered,  col | 0x30000000u);
        ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundSelected, col | 0x60000000u);
        ImNodes::BeginNode(nid);

        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(s_node_type_name(n.type));
        ImGui::SameLine();
        ImGui::TextDisabled("%08X", n.id);
        ImNodes::EndNodeTitleBar();

        ImNodes::BeginInputAttribute(nid + 1);
        ImGui::TextUnformatted("in");
        ImNodes::EndInputAttribute();

        ImNodes::BeginOutputAttribute(nid + 2);
        ImGui::TextUnformatted("out");
        ImNodes::EndOutputAttribute();

        ImNodes::EndNode();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
    }

    // Draw links from FlowConn table (from_node/to_node are FNV hashes → find index)
    for (int c = 0; c < fg.conn_count; ++c) {
        const FlowConn& conn = fg.conns[c];
        int from_idx = -1, to_idx = -1;
        for (int j = 0; j < fg.node_count; ++j) {
            if (fg.nodes[j].id == conn.from_node) from_idx = j;
            if (fg.nodes[j].id == conn.to_node)   to_idx   = j;
        }
        if (from_idx < 0 || to_idx < 0) continue;
        ImNodes::Link(c + 1,
                      (from_idx + 1) * 100 + 2,
                      (to_idx   + 1) * 100 + 1);
    }

    ImNodes::EndNodeEditor();
    ImGui::EndChild();

    // ── Variables (editable) ──────────────────────────────────────────
    if (ImGui::CollapsingHeader("Variables", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int i = 0; i < fg.var_count; ++i) {
            FlowVar& v = fg.vars[i];
            char label[32];
            snprintf(label, sizeof(label), "0x%08X##fgv%d", v.key, i);
            ImGui::SetNextItemWidth(120);
            ImGui::DragFloat(label, &v.val.f, 0.01f);
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

    // ── Fire external trigger ─────────────────────────────────────────
    if (ImGui::CollapsingHeader("Fire External Trigger")) {
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("Node name##fg", fire_node_name_, sizeof(fire_node_name_));
        ImGui::SameLine();
        if (ImGui::Button("Fire##fg") && fire_node_name_[0] != '\0') {
            fg.FireTrigger(md::fnv1a(fire_node_name_), 0.0);
        }
    }

    ImGui::End();
}
#endif
