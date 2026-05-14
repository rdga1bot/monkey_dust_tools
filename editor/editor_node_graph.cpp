#ifdef MONKEY_DUST_EDITOR
#include "editor_node_graph.h"
#include "editor_core.h"
#include <cstring>
#include <cstdio>

// ── Per-node-kind metadata ────────────────────────────────────────────────────

struct KindMeta {
    const char* label;
    const char* inputs[4];
    int         in_cnt;
    const char* outputs[4];
    int         out_cnt;
};

static const KindMeta kMeta[(int)EditorNodeGraphPanel::NK_COUNT] = {
    { "Tex Sample", {"UV"},          1, {"R","G","B","A"}, 4 }, // TexSample
    { "Multiply",   {"A","B"},        2, {"Out"},           1 }, // Multiply
    { "Add",        {"A","B"},        2, {"Out"},           1 }, // Add
    { "Lerp",       {"A","B","T"},    3, {"Out"},           1 }, // Lerp
    { "Const Float",{},               0, {"Val"},           1 }, // ConstFloat
    { "Const Color",{},               0, {"RGBA"},          1 }, // ConstColor
    { "Mat Output", {"Base","Metal","Rough","Normal"}, 4, {}, 0 }, // MatOutput
};

// ── Init / Shutdown ───────────────────────────────────────────────────────────

void EditorNodeGraphPanel::Init() {
    if (ctx_) return;
    ed::Config cfg;
    cfg.SettingsFile = "data/node_graph.json";
    ctx_ = ed::CreateEditor(&cfg);
    BuildDefaultGraph();
}

void EditorNodeGraphPanel::Shutdown() {
    if (!ctx_) return;
    ed::DestroyEditor(ctx_);
    ctx_ = nullptr;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

int EditorNodeGraphPanel::MakePin(int nid, ed::PinKind kind, const char* lbl) {
    if (pin_cnt_ >= MAX_P) return -1;
    Pin& p = pins_[pin_cnt_];
    p.id      = next_id_++;
    p.kind    = kind;
    p.node_id = nid;
    strncpy(p.label, lbl, sizeof(p.label) - 1);
    return pin_cnt_++;
}

int EditorNodeGraphPanel::SpawnNode(NK kind) {
    if (node_cnt_ >= MAX_N) return -1;
    int idx   = node_cnt_++;
    Node& n   = nodes_[idx];
    n.id      = next_id_++;
    n.kind    = kind;
    n.alive   = true;
    const KindMeta& m = kMeta[(int)kind];
    strncpy(n.label, m.label, sizeof(n.label) - 1);

    n.first_in = pin_cnt_;
    n.in_cnt   = m.in_cnt;
    for (int i = 0; i < m.in_cnt; ++i)
        MakePin(n.id, ed::PinKind::Input, m.inputs[i]);

    n.first_out = pin_cnt_;
    n.out_cnt   = m.out_cnt;
    for (int i = 0; i < m.out_cnt; ++i)
        MakePin(n.id, ed::PinKind::Output, m.outputs[i]);

    return idx;
}

void EditorNodeGraphPanel::BuildDefaultGraph() {
    int ts = SpawnNode(NK::TexSample);
    int cf = SpawnNode(NK::ConstFloat);
    int ml = SpawnNode(NK::Multiply);
    int mo = SpawnNode(NK::MatOutput);
    if (ts < 0 || cf < 0 || ml < 0 || mo < 0) return;

    // TexSample.R → Multiply.A
    if (link_cnt_ < MAX_L)
        links_[link_cnt_++] = { next_id_++,
            pins_[nodes_[ts].first_out].id,
            pins_[nodes_[ml].first_in].id, true };
    // ConstFloat.Val → Multiply.B
    if (link_cnt_ < MAX_L)
        links_[link_cnt_++] = { next_id_++,
            pins_[nodes_[cf].first_out].id,
            pins_[nodes_[ml].first_in + 1].id, true };
    // Multiply.Out → MatOutput.Base
    if (link_cnt_ < MAX_L)
        links_[link_cnt_++] = { next_id_++,
            pins_[nodes_[ml].first_out].id,
            pins_[nodes_[mo].first_in].id, true };
}

// ── DrawNode ──────────────────────────────────────────────────────────────────

void EditorNodeGraphPanel::DrawNode(Node& n) {
    ed::BeginNode(ed::NodeId(n.id));

    // Node title
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.85f, 0.4f, 1.f));
    ImGui::TextUnformatted(n.label);
    ImGui::PopStyleColor();

    // Per-kind inline controls
    if (n.kind == NK::ConstFloat) {
        ImGui::SetNextItemWidth(80.f);
        char uid[24]; snprintf(uid, sizeof(uid), "##cf%d", n.id);
        ImGui::DragFloat(uid, &n.float_val, 0.01f, 0.f, 1.f, "%.3f");
    } else if (n.kind == NK::ConstColor) {
        ImGui::SetNextItemWidth(100.f);
        char uid[24]; snprintf(uid, sizeof(uid), "##cc%d", n.id);
        ImGui::ColorEdit4(uid, &n.color_val.x,
            ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha);
    } else if (n.kind == NK::TexSample) {
        ImGui::SetNextItemWidth(130.f);
        char uid[24]; snprintf(uid, sizeof(uid), "##tp%d", n.id);
        ImGui::InputText(uid, n.tex_path, sizeof(n.tex_path));
    }

    // Input pins (left side)
    for (int i = 0; i < n.in_cnt; ++i) {
        const Pin& p = pins_[n.first_in + i];
        ed::BeginPin(ed::PinId(p.id), ed::PinKind::Input);
            ImGui::Text("▶ %s", p.label);
        ed::EndPin();
    }

    // Output pins (right side, aligned)
    for (int i = 0; i < n.out_cnt; ++i) {
        const Pin& p = pins_[n.first_out + i];
        ed::BeginPin(ed::PinId(p.id), ed::PinKind::Output);
            float w = ImGui::CalcTextSize(p.label).x + 16.f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                ImGui::GetContentRegionAvail().x - w);
            ImGui::Text("%s ◀", p.label);
        ed::EndPin();
    }

    ed::EndNode();
}

// ── Create / Delete interactions ──────────────────────────────────────────────

void EditorNodeGraphPanel::HandleCreateDelete() {
    if (ed::BeginCreate()) {
        ed::PinId from_pid, to_pid;
        if (ed::QueryNewLink(&from_pid, &to_pid) &&
            from_pid != to_pid &&
            ed::AcceptNewItem(ImVec4(0.4f, 0.8f, 0.4f, 1.f), 2.f))
        {
            if (link_cnt_ < MAX_L)
                links_[link_cnt_++] = { next_id_++,
                    (int)(uintptr_t)from_pid.Get(),
                    (int)(uintptr_t)to_pid.Get(), true };
        }
    }
    ed::EndCreate();

    if (ed::BeginDelete()) {
        ed::LinkId lid;
        while (ed::QueryDeletedLink(&lid)) {
            int link_id = (int)(uintptr_t)lid.Get();
            if (ed::AcceptDeletedItem()) {
                for (int i = 0; i < link_cnt_; ++i)
                    if (links_[i].id == link_id) links_[i].alive = false;
            }
        }
        ed::NodeId nid;
        while (ed::QueryDeletedNode(&nid)) {
            int node_id = (int)(uintptr_t)nid.Get();
            if (ed::AcceptDeletedItem()) {
                for (int i = 0; i < node_cnt_; ++i)
                    if (nodes_[i].id == node_id) nodes_[i].alive = false;
            }
        }
    }
    ed::EndDelete();
}

// ── Right-click "Add Node" popup ──────────────────────────────────────────────

void EditorNodeGraphPanel::HandleContextMenu() {
    ed::Suspend();
    if (ed::ShowBackgroundContextMenu()) ImGui::OpenPopup("##ngAdd");
    if (ImGui::BeginPopup("##ngAdd")) {
        ImGui::TextDisabled("Add Node");
        ImGui::Separator();
        const char* labels[NK_COUNT] = {
            "Tex Sample","Multiply","Add","Lerp",
            "Const Float","Const Color","Mat Output"
        };
        for (int i = 0; i < NK_COUNT; ++i)
            if (ImGui::MenuItem(labels[i])) SpawnNode((NK)i);
        ImGui::EndPopup();
    }
    ed::Resume();
}

// ── Draw ──────────────────────────────────────────────────────────────────────

void EditorNodeGraphPanel::Draw() {
    if (!EditorCore::Get().panels_visible[12]) return;
    if (!ctx_) Init();

    ImGui::SetNextWindowSize(ImVec2(720, 500), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(300, 80),  ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Node Graph##ng", &EditorCore::Get().panels_visible[12])) {
        ImGui::End(); return;
    }

    ed::SetCurrentEditor(ctx_);
    ed::Begin("##NodeCanvas", ImGui::GetContentRegionAvail());

    for (int i = 0; i < node_cnt_; ++i)
        if (nodes_[i].alive) DrawNode(nodes_[i]);

    for (int i = 0; i < link_cnt_; ++i)
        if (links_[i].alive)
            ed::Link(ed::LinkId(links_[i].id),
                     ed::PinId(links_[i].from_pin),
                     ed::PinId(links_[i].to_pin),
                     ImVec4(0.8f, 0.7f, 0.2f, 1.f), 2.f);

    HandleCreateDelete();
    HandleContextMenu();

    ed::End();
    ed::SetCurrentEditor(nullptr);

    ImGui::End();
}

#endif // MONKEY_DUST_EDITOR
