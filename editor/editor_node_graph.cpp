#ifdef MONKEY_DUST_EDITOR
#include "editor_node_graph.h"
#include "editor_core.h"
#include <cstring>
#include <cstdio>

// ── Per-kind metadata ─────────────────────────────────────────────────────────

struct KindMeta {
    const char*         label;
    md::PcgDataType     in_types[4];
    int                 in_cnt;
    md::PcgDataType     out_type;
};

using DT = md::PcgDataType;
static const KindMeta kMeta[EditorNodeGraphPanel::NK_COUNT] = {
    // NoiseFBM
    { "Noise FBM",    {}, 0, DT::HeightMap },
    // DomainWarp
    { "Domain Warp",  {DT::HeightMap}, 1, DT::HeightMap },
    // Ridge
    { "Ridge",        {DT::HeightMap}, 1, DT::HeightMap },
    // Biome
    { "Biome",        {}, 0, DT::BiomeMap },
    // TerrainOutput
    { "Terrain Out",  {DT::HeightMap, DT::BiomeMap}, 2, DT::None },
    // Add
    { "Add",          {DT::HeightMap, DT::HeightMap}, 2, DT::HeightMap },
    // Multiply
    { "Multiply",     {DT::HeightMap, DT::Mask}, 2, DT::HeightMap },
    // Clamp
    { "Clamp",        {DT::HeightMap}, 1, DT::HeightMap },
    // Slope
    { "Slope",        {DT::HeightMap}, 1, DT::Mask },
    // Scatter
    { "Scatter",      {DT::Mask}, 1, DT::PointCloud },
};

static const char* kInLabel[EditorNodeGraphPanel::NK_COUNT][4] = {
    {},                           // NoiseFBM
    {"In"},                       // DomainWarp
    {"In"},                       // Ridge
    {},                           // Biome
    {"Height", "Biome"},          // TerrainOutput
    {"A", "B"},                   // Add
    {"Height", "Mask"},           // Multiply
    {"In"},                       // Clamp
    {"Height"},                   // Slope
    {"Density"},                  // Scatter
};

// ── Init / Shutdown ───────────────────────────────────────────────────────────

void EditorNodeGraphPanel::Init() {
    if (ctx_) return;
    ed::Config cfg;
    cfg.SettingsFile = "data/terrain_node_graph.json";
    ctx_ = ed::CreateEditor(&cfg);
    BuildDefaultGraph();
}

void EditorNodeGraphPanel::Shutdown() {
    if (!ctx_) return;
    ed::DestroyEditor(ctx_);
    ctx_ = nullptr;
}

// ── MakePin ───────────────────────────────────────────────────────────────────

int EditorNodeGraphPanel::MakePin(int nid, ed::PinKind kind,
                                  md::PcgDataType dt, const char* lbl) {
    if (pin_cnt_ >= MAX_P) return -1;
    Pin& p = pins_[pin_cnt_];
    p.id        = next_id_++;
    p.kind      = kind;
    p.data_type = dt;
    p.node_id   = nid;
    strncpy(p.label, lbl, sizeof(p.label) - 1);
    return pin_cnt_++;
}

// ── GetPcgNode — pick next concrete node instance for a given kind ────────────

md::PcgNode* EditorNodeGraphPanel::GetPcgNode(NK kind) {
    switch (kind) {
    case NK::NoiseFBM:      return &pcg_noise_  [cnt_noise_++];
    case NK::DomainWarp:    return &pcg_warp_   [cnt_warp_++];
    case NK::Ridge:         return &pcg_ridge_  [cnt_ridge_++];
    case NK::Biome:         return &pcg_biome_  [cnt_biome_++];
    case NK::TerrainOutput: return &pcg_output_ [cnt_out_++];
    case NK::Add:           return &pcg_add_    [cnt_add_++];
    case NK::Multiply:      return &pcg_mul_    [cnt_mul_++];
    case NK::Clamp:         return &pcg_clamp_  [cnt_clamp_++];
    case NK::Slope:         return &pcg_slope_  [cnt_slope_++];
    case NK::Scatter:       return &pcg_scatter_[cnt_scatter_++];
    }
    return nullptr;
}

// ── SpawnNode ─────────────────────────────────────────────────────────────────

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
        MakePin(n.id, ed::PinKind::Input, m.in_types[i], kInLabel[(int)kind][i]);

    n.first_out = pin_cnt_;
    n.out_cnt   = (m.out_type != DT::None) ? 1 : 0;
    if (n.out_cnt)
        MakePin(n.id, ed::PinKind::Output, m.out_type, "Out");

    // Register in PcgGraph
    md::PcgNode* pn = GetPcgNode(kind);
    if (pn) {
        pn->alive  = true;
        n.pcg_slot = pcg_graph_.AddNode(pn);
        SyncParamsToEngine(idx);
    }

    return idx;
}

// ── SyncParamsToEngine ────────────────────────────────────────────────────────
// Called whenever a slider changes value; copies UI params → concrete PcgNode.

void EditorNodeGraphPanel::SyncParamsToEngine(int idx) {
    Node& n = nodes_[idx];
    if (n.pcg_slot < 0) return;
    md::PcgNode* base = pcg_graph_.nodes[n.pcg_slot];
    if (!base) return;

    switch (n.kind) {
    case NK::NoiseFBM: {
        auto* p = static_cast<md::PcgNoiseNode*>(base);
        p->base_scale  = n.base_scale;  p->amplitude  = n.amplitude;
        p->octaves     = n.octaves;     p->persistence = n.persistence;
        p->lacunarity  = n.lacunarity;  p->seed        = n.seed;
        break; }
    case NK::DomainWarp: {
        auto* p = static_cast<md::PcgDomainWarpNode*>(base);
        p->warp_strength = n.warp_strength; p->warp_seed = n.seed;
        break; }
    case NK::Ridge: {
        auto* p = static_cast<md::PcgRidgeNode*>(base);
        p->ridge_weight    = n.ridge_weight;
        p->redistrib_power = n.redist_power;
        break; }
    case NK::Biome: {
        auto* p = static_cast<md::PcgBiomeNode*>(base);
        p->biome_scale = n.biome_scale; p->num_biomes = n.num_factions;
        p->seed        = n.seed;
        break; }
    case NK::Add: {
        auto* p = static_cast<md::PcgAddNode*>(base);
        p->weight_a = n.weight_a; p->weight_b = n.weight_b;
        break; }
    case NK::Clamp: {
        auto* p = static_cast<md::PcgClampNode*>(base);
        p->lo = n.clamp_lo; p->hi = n.clamp_hi;
        break; }
    case NK::Scatter: {
        auto* p = static_cast<md::PcgScatterNode*>(base);
        p->density   = n.density;  p->radius = n.scatter_radius;
        p->seed      = n.seed;     p->prefab_id = n.prefab_id;
        break; }
    default: break;
    }
    pcg_graph_.MarkDirty(n.pcg_slot);
}

// ── BuildDefaultGraph ─────────────────────────────────────────────────────────

void EditorNodeGraphPanel::BuildDefaultGraph() {
    int fbm   = SpawnNode(NK::NoiseFBM);
    int warp  = SpawnNode(NK::DomainWarp);
    int ridge = SpawnNode(NK::Ridge);
    int biome = SpawnNode(NK::Biome);
    int out   = SpawnNode(NK::TerrainOutput);
    if (fbm < 0 || warp < 0 || ridge < 0 || biome < 0 || out < 0) return;

    auto link = [&](int fn, int fp_local, int tn, int tp_local) {
        if (link_cnt_ >= MAX_L) return;
        int fp = pins_[nodes_[fn].first_out + fp_local].id;
        int tp = pins_[nodes_[tn].first_in  + tp_local].id;
        links_[link_cnt_++] = { next_id_++, fp, tp, fn, tn, fp_local, tp_local, true };
        pcg_graph_.AddLink(nodes_[fn].pcg_slot, fp_local, nodes_[tn].pcg_slot, tp_local);
    };
    link(fbm, 0, warp,  0);
    link(warp,0, ridge, 0);
    link(ridge,0, out,  0);
    link(biome,0, out,  1);
}

// ── EvalGraph (legacy — TerrainGenParams for non-PcgGraph callers) ────────────

TerrainGenParams EditorNodeGraphPanel::EvalGraph() const {
    TerrainGenParams p = TerrainGenParams::ProcWorld();
    for (int i = 0; i < node_cnt_; ++i) {
        const Node& n = nodes_[i];
        if (!n.alive) continue;
        switch (n.kind) {
        case NK::NoiseFBM:
            p.base_scale  = n.base_scale;  p.amplitude = n.amplitude;
            p.octaves     = n.octaves;     p.seed      = n.seed;
            p.force_noise = true;          break;
        case NK::DomainWarp:
            p.domain_warp_strength = n.warp_strength; break;
        case NK::Ridge:
            p.ridge_weight = n.ridge_weight; p.redistribution_power = n.redist_power; break;
        case NK::Biome:
            p.biome_scale = n.biome_scale; p.num_factions = n.num_factions; break;
        default: break;
        }
    }
    return p;
}

// ── DrawPinDot — coloured circle for pin type ─────────────────────────────────

void EditorNodeGraphPanel::DrawPinDot(md::PcgDataType dt, bool is_output) {
    md::PinColor c = md::PcgPinColor(dt);
    ImVec4 col4(c.r, c.g, c.b, c.a);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = 5.f;
    if (is_output) p.x += ImGui::GetContentRegionAvail().x - r * 2;
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(p.x + r, p.y + r), r, ImGui::ColorConvertFloat4ToU32(col4));
    ImGui::Dummy(ImVec2(r * 2 + 4, r * 2));
}

// ── DrawNode ──────────────────────────────────────────────────────────────────

void EditorNodeGraphPanel::DrawNode(Node& n) {
    ed::BeginNode(ed::NodeId(n.id));

    // Header: type name with accent colour
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.6f, 1.f));
    ImGui::TextUnformatted(n.label);
    ImGui::PopStyleColor();

    // Dirty indicator
    if (n.pcg_slot >= 0 && pcg_graph_.nodes[n.pcg_slot] &&
        pcg_graph_.nodes[n.pcg_slot]->dirty)
        ImGui::SameLine(); ImGui::TextDisabled("*");

    char uid[32];

    // Per-kind inline controls — each DragFloat/Int calls SyncParamsToEngine on change
    bool changed = false;
    ImGui::PushItemWidth(120.f);
    switch (n.kind) {
    case NK::NoiseFBM:
        snprintf(uid,sizeof(uid),"##sc%d",n.id);
        changed |= ImGui::DragFloat(uid,&n.base_scale,0.0001f,0.0001f,0.05f,"scale %.4f");
        snprintf(uid,sizeof(uid),"##amp%d",n.id);
        changed |= ImGui::DragFloat(uid,&n.amplitude,1.f,1.f,2000.f,"amp %.0fm");
        snprintf(uid,sizeof(uid),"##oct%d",n.id);
        changed |= ImGui::DragInt(uid,&n.octaves,0.1f,1,10,"oct %d");
        snprintf(uid,sizeof(uid),"##seed%d",n.id);
        changed |= ImGui::DragInt(uid,&n.seed,1.f,0,9999,"seed %d");
        break;
    case NK::DomainWarp:
        snprintf(uid,sizeof(uid),"##dw%d",n.id);
        changed |= ImGui::DragFloat(uid,&n.warp_strength,0.5f,0.f,200.f,"warp %.1f");
        break;
    case NK::Ridge:
        snprintf(uid,sizeof(uid),"##rw%d",n.id);
        changed |= ImGui::DragFloat(uid,&n.ridge_weight,0.01f,0.f,1.f,"ridge %.3f");
        snprintf(uid,sizeof(uid),"##rp%d",n.id);
        changed |= ImGui::DragFloat(uid,&n.redist_power,0.1f,0.1f,10.f,"redist %.1f");
        break;
    case NK::Biome:
        snprintf(uid,sizeof(uid),"##bs%d",n.id);
        changed |= ImGui::DragFloat(uid,&n.biome_scale,0.00001f,0.00001f,0.01f,"scale %.5f");
        snprintf(uid,sizeof(uid),"##nb%d",n.id);
        changed |= ImGui::DragInt(uid,&n.num_factions,0.1f,1,16,"biomes %d");
        break;
    case NK::Add:
        snprintf(uid,sizeof(uid),"##wa%d",n.id);
        changed |= ImGui::DragFloat(uid,&n.weight_a,0.01f,0.f,4.f,"A %.2f");
        snprintf(uid,sizeof(uid),"##wb%d",n.id);
        changed |= ImGui::DragFloat(uid,&n.weight_b,0.01f,0.f,4.f,"B %.2f");
        break;
    case NK::Clamp:
        snprintf(uid,sizeof(uid),"##clo%d",n.id);
        changed |= ImGui::DragFloat(uid,&n.clamp_lo,0.5f,-200.f,200.f,"lo %.1f");
        snprintf(uid,sizeof(uid),"##chi%d",n.id);
        changed |= ImGui::DragFloat(uid,&n.clamp_hi,0.5f,-200.f,2000.f,"hi %.1f");
        break;
    case NK::Scatter:
        snprintf(uid,sizeof(uid),"##den%d",n.id);
        changed |= ImGui::DragFloat(uid,&n.density,0.001f,0.f,0.5f,"density %.3f");
        snprintf(uid,sizeof(uid),"##srad%d",n.id);
        changed |= ImGui::DragFloat(uid,&n.scatter_radius,0.1f,0.5f,50.f,"radius %.1f");
        break;
    case NK::TerrainOutput:
        ImGui::TextDisabled("(sink — drives rebuild)");
        break;
    default: break;
    }
    ImGui::PopItemWidth();

    if (changed) SyncParamsToEngine((int)(&n - nodes_));

    // Input pins (left side, coloured dot)
    for (int i = 0; i < n.in_cnt; ++i) {
        const Pin& p = pins_[n.first_in + i];
        ed::BeginPin(ed::PinId(p.id), ed::PinKind::Input);
            md::PinColor c = md::PcgPinColor(p.data_type);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.r,c.g,c.b,c.a));
            ImGui::Text("▶ %s", p.label);
            ImGui::PopStyleColor();
        ed::EndPin();
    }

    // Output pin (right side)
    for (int i = 0; i < n.out_cnt; ++i) {
        const Pin& p = pins_[n.first_out + i];
        ed::BeginPin(ed::PinId(p.id), ed::PinKind::Output);
            md::PinColor c = md::PcgPinColor(p.data_type);
            float w = ImGui::CalcTextSize("Out ◀").x + 16.f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                ImGui::GetContentRegionAvail().x - w);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.r,c.g,c.b,c.a));
            ImGui::Text("Out ◀");
            ImGui::PopStyleColor();
        ed::EndPin();
    }

    ed::EndNode();
}

// ── HandleCreateDelete ────────────────────────────────────────────────────────

void EditorNodeGraphPanel::HandleCreateDelete() {
    if (ed::BeginCreate()) {
        ed::PinId from_pid, to_pid;
        if (ed::QueryNewLink(&from_pid, &to_pid) && from_pid != to_pid) {
            // Find pins
            int from_p = -1, to_p = -1;
            for (int i = 0; i < pin_cnt_; ++i) {
                if (pins_[i].id == (int)(uintptr_t)from_pid.Get()) from_p = i;
                if (pins_[i].id == (int)(uintptr_t)to_pid.Get())   to_p   = i;
            }
            // Type validation
            bool ok = false;
            if (from_p >= 0 && to_p >= 0)
                ok = md::PcgPinCompatible(pins_[from_p].data_type, pins_[to_p].data_type);

            ImVec4 col = ok ? ImVec4(0.4f,0.8f,0.4f,1.f) : ImVec4(0.9f,0.2f,0.2f,1.f);
            if (ok && ed::AcceptNewItem(col, 2.f)) {
                if (link_cnt_ < MAX_L && from_p >= 0 && to_p >= 0) {
                    // Resolve node indices
                    int fn = -1, tn = -1, fpl = 0, tpl = 0;
                    for (int i = 0; i < node_cnt_; ++i) {
                        if (!nodes_[i].alive) continue;
                        for (int k = 0; k < nodes_[i].out_cnt; ++k)
                            if (nodes_[i].first_out + k == from_p)
                                { fn = i; fpl = k; }
                        for (int k = 0; k < nodes_[i].in_cnt; ++k)
                            if (nodes_[i].first_in + k == to_p)
                                { tn = i; tpl = k; }
                    }
                    links_[link_cnt_++] = { next_id_++,
                        pins_[from_p].id, pins_[to_p].id,
                        fn, tn, fpl, tpl, true };
                    if (fn >= 0 && tn >= 0 && nodes_[fn].pcg_slot >= 0 && nodes_[tn].pcg_slot >= 0)
                        pcg_graph_.AddLink(nodes_[fn].pcg_slot, fpl,
                                           nodes_[tn].pcg_slot, tpl);
                }
            } else if (!ok) {
                ed::RejectNewItem(col, 2.f);
            }
        }
    }
    ed::EndCreate();

    if (ed::BeginDelete()) {
        ed::LinkId lid;
        while (ed::QueryDeletedLink(&lid)) {
            int link_id = (int)(uintptr_t)lid.Get();
            if (ed::AcceptDeletedItem()) {
                for (int i = 0; i < link_cnt_; ++i) {
                    if (links_[i].id == link_id && links_[i].alive) {
                        links_[i].alive = false;
                        pcg_graph_.RemoveLink(i);
                    }
                }
            }
        }
        ed::NodeId nid;
        while (ed::QueryDeletedNode(&nid)) {
            int node_id = (int)(uintptr_t)nid.Get();
            if (ed::AcceptDeletedItem()) {
                for (int i = 0; i < node_cnt_; ++i) {
                    if (nodes_[i].id == node_id && nodes_[i].alive) {
                        nodes_[i].alive = false;
                        if (nodes_[i].pcg_slot >= 0)
                            pcg_graph_.RemoveNode(nodes_[i].pcg_slot);
                    }
                }
            }
        }
    }
    ed::EndDelete();
}

// ── HandleContextMenu ─────────────────────────────────────────────────────────

void EditorNodeGraphPanel::HandleContextMenu() {
    ed::Suspend();
    if (ed::ShowBackgroundContextMenu()) ImGui::OpenPopup("##ngAdd");
    if (ImGui::BeginPopup("##ngAdd")) {
        ImGui::TextDisabled("Add Node");
        ImGui::Separator();
        const char* labels[NK_COUNT] = {
            "Noise FBM","Domain Warp","Ridge","Biome","Terrain Output",
            "Add","Multiply","Clamp","Slope","Scatter"
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
    ImGui::SetNextWindowSize(ImVec2(860, 540), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(300, 80),   ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Terrain Nodes##ng", &EditorCore::Get().panels_visible[12])) {
        ImGui::End(); return;
    }
    DrawContent();
    ImGui::End();
}

void EditorNodeGraphPanel::DrawContent() {
    if (!ctx_) Init();

    // Status bar (above canvas)
    int alive_nodes = 0, dirty_count = 0;
    for (int i = 0; i < node_cnt_; ++i) {
        if (!nodes_[i].alive) continue;
        ++alive_nodes;
        if (nodes_[i].pcg_slot >= 0 && pcg_graph_.nodes[nodes_[i].pcg_slot] &&
            pcg_graph_.nodes[nodes_[i].pcg_slot]->dirty)
            ++dirty_count;
    }
    ImGui::TextDisabled("Nodes:%d  Links:%d", alive_nodes, link_cnt_);
    ImGui::SameLine();
    if (pcg_graph_.needs_rebuild)
        ImGui::TextColored(ImVec4(1.f,0.7f,0.1f,1.f), "[DIRTY]");
    else
        ImGui::TextColored(ImVec4(0.3f,0.9f,0.3f,1.f), "[OK v%u]", pcg_graph_.exec_version);
    ImGui::SameLine();
    if (md::TerrainTileGen::Get().IsBuilding())
        ImGui::TextColored(ImVec4(0.4f,0.7f,1.f,1.f), "⏳ building...");

    // Canvas
    ed::SetCurrentEditor(ctx_);
    ed::Begin("##NodeCanvas", ImGui::GetContentRegionAvail());

    for (int i = 0; i < node_cnt_; ++i)
        if (nodes_[i].alive) DrawNode(nodes_[i]);

    for (int i = 0; i < link_cnt_; ++i) {
        if (!links_[i].alive) continue;
        md::PinColor c = {0.4f,0.9f,0.5f,1.f};
        int fp_idx = -1;
        for (int p = 0; p < pin_cnt_; ++p)
            if (pins_[p].id == links_[i].from_pin) { fp_idx = p; break; }
        if (fp_idx >= 0) c = md::PcgPinColor(pins_[fp_idx].data_type);

        ed::Link(ed::LinkId(links_[i].id),
                 ed::PinId(links_[i].from_pin),
                 ed::PinId(links_[i].to_pin),
                 ImVec4(c.r,c.g,c.b,c.a), 2.f);
    }

    HandleCreateDelete();
    HandleContextMenu();

    ed::End();
    ed::SetCurrentEditor(nullptr);
}

#endif // MONKEY_DUST_EDITOR
