#ifdef MONKEY_DUST_EDITOR
#include "editor_inspector.h"
#include "editor_core.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/world/world_transform.h>
#include "components/health.h"
#include "components/combat.h"
#include "components/ai_agent.h"
#include "components/nav_agent.h"
#include "components/inventory.h"
#include "components/building.h"
#include "components/player_controller.h"
#include "components/ai_script.h"
#include "combat/hit_zones.h"
#include "building/build_system.h"
#ifdef MD_OPENGL43_ENABLED
#include <monkey_dust/world/transform_soa.h>
#endif
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
bool EditorInspector::CollapsingSection(const char* label, ImVec4 color) {
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(color.x*0.6f, color.y*0.6f, color.z*0.6f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(color.x*0.8f, color.y*0.8f, color.z*0.8f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  color);
    bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleColor(3);
    return open;
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawTransform(entt::entity e) {
    auto& reg = Registry::Get();
    if (!reg.all_of<WorldTransform>(e)) return;
    if (!CollapsingSection("Transform##insp", {0.2f, 0.8f, 0.3f, 1.f})) return;

    auto& tr = reg.get<WorldTransform>(e);
    ImGui::DragFloat("X##tr", &tr.x, 0.1f);
    ImGui::DragFloat("Y##tr", &tr.y, 0.1f);
    ImGui::DragFloat("Z##tr", &tr.z, 0.1f);
    ImGui::DragFloat("RotY##tr", &tr.rot_y, 1.f, -180.f, 180.f);

#ifdef MD_OPENGL43_ENABLED
    ImGui::TextDisabled("SoA slot: %u", tr.slot);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawHealth(entt::entity e) {
    auto& reg = Registry::Get();
    if (!reg.all_of<Health>(e)) return;
    if (!CollapsingSection("Health##insp", {0.9f, 0.2f, 0.2f, 1.f})) return;

    auto& hp = reg.get<Health>(e);
    float frac = (hp.max > 0.f) ? (hp.current / hp.max) : 0.f;
    char overlay[32];
    snprintf(overlay, sizeof(overlay), "%.0f / %.0f", hp.current, hp.max);
    ImGui::ProgressBar(frac, ImVec2(-1, 0), overlay);
    ImGui::SliderFloat("Max##hp", &hp.max, 1.f, 500.f);
    if (hp.current > hp.max) hp.current = hp.max;
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawCombat(entt::entity e) {
    auto& reg = Registry::Get();
    if (!reg.all_of<Combat>(e)) return;
    if (!CollapsingSection("Combat##insp", {0.9f, 0.5f, 0.1f, 1.f})) return;

    auto& c = reg.get<Combat>(e);

    static const char* dmg_types[] = {"Blunt", "Cut", "Pierce"};
    const char* dtype = dmg_types[(int)c.weapon.type];
    ImGui::Text("Weapon: %.0f dmg | %s | range %.1fm | %ums cd",
                c.weapon.damage, dtype, c.weapon.attack_range, c.weapon.attack_ms);
    ImGui::Text("Armor blunt:%.2f cut:%.2f pierce:%.2f",
                c.armor.blunt_resist, c.armor.cut_resist, c.armor.pierce_resist);
    ImGui::Separator();
    ImGui::Text("Dead: %s  |  Last atk: %.0f ms", c.is_dead ? "YES" : "no", c.last_attack_ms);

    if (ImGui::TreeNode("Hit Zones")) {
        static const char* zone_names[] = {"Head","Torso","LArrm","RArm","LLeg","RLeg"};
        for (int i = 0; i < (int)HitZone::COUNT; ++i) {
            float hp = c.zone_hp[i];
            float cap = ZONE_TABLE[i].cripple_hp * 2.f;
            float frac = (cap > 0.f) ? (hp / cap) : 0.f;
            ImVec4 col = (hp <= ZONE_TABLE[i].cripple_hp)
                         ? ImVec4(1.f,0.3f,0.3f,1.f)
                         : ImVec4(0.4f,0.9f,0.4f,1.f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "%.0f##z%d", hp, i);
            ImGui::ProgressBar(frac, ImVec2(120, 0), lbl);
            ImGui::SameLine();
            ImGui::Text("%s", zone_names[i]);
            ImGui::PopStyleColor();
        }
        ImGui::TreePop();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawAIAgent(entt::entity e) {
    auto& reg = Registry::Get();
    if (!reg.all_of<AIAgent>(e)) return;
    if (!CollapsingSection("AIAgent##insp", {0.2f, 0.5f, 1.0f, 1.f})) return;

    auto& ai = reg.get<AIAgent>(e);

    static const char* lod_names[] = {"HIGH", "LOW", "FROZEN"};
    int lod = (int)ai.lod_level;
    if (ImGui::Combo("LOD##ai", &lod, lod_names, 3))
        ai.lod_level = (uint8_t)lod;

    int faction = (int)ai.faction_id;
    if (ImGui::InputInt("Faction##ai", &faction))
        ai.faction_id = (uint32_t)(faction < 0 ? 0 : faction);

    ImGui::Text("BT node: %d  |  template: %u", (int)ai.bt_node, (unsigned)ai.bt_template_id);
    ImGui::Text("Personal relation: %d", (int)ai.personal_relation);
    ImGui::Text("Last tick: %.0f ms", ai.last_tick_ms);
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawNavAgent(entt::entity e) {
    auto& reg = Registry::Get();
    if (!reg.all_of<NavAgent>(e)) return;
    if (!CollapsingSection("NavAgent##insp", {0.2f, 0.8f, 0.7f, 1.f})) return;

    const auto& nav = reg.get<NavAgent>(e);
    ImGui::Text("Target: (%.1f, %.1f)", nav.target_x, nav.target_z);
    ImGui::Text("Path: %d/%d waypoints", nav.path_idx, nav.path_len);
    if (nav.path_len > 0 && nav.path_idx < nav.path_len) {
        int i = nav.path_idx;
        ImGui::Text("Next wp: (%.1f, %.1f, %.1f)",
                    nav.path[i*3+0], nav.path[i*3+1], nav.path[i*3+2]);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawInventory(entt::entity e) {
    auto& reg = Registry::Get();
    if (!reg.all_of<Inventory>(e)) return;
    if (!CollapsingSection("Inventory##insp", {0.9f, 0.8f, 0.1f, 1.f})) return;

    const auto& inv = reg.get<Inventory>(e);
    ImGui::Text("Slots: %d / %d", inv.slot_count, INV_MAX_SLOTS);
    if (ImGui::BeginTable("##inv_tbl", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Item ID");
        ImGui::TableSetupColumn("Amount");
        ImGui::TableHeadersRow();
        for (int i = 0; i < inv.slot_count; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%u", inv.item_ids[i]);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", inv.amounts[i]);
        }
        ImGui::EndTable();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawBuilding(entt::entity e) {
    auto& reg = Registry::Get();
    if (!reg.all_of<Building>(e)) return;
    if (!CollapsingSection("Building##insp", {0.4f, 0.6f, 0.8f, 1.f})) return;

    auto& b = reg.get<Building>(e);
    const BuildingDef* d = BuildSystem::Get().GetDef(b.def_id);
    ImGui::Text("Def: %u (%s)", b.def_id, d ? d->name : "?");
    ImGui::Text("Grid: (%d, %d)  Size: %dx%d", b.grid_x, b.grid_z, b.size_x, b.size_z);
    ImGui::Checkbox("Active##bld", &b.active);
    if (b.chain.valid && b.chain.time_s > 0.f) {
        float frac = b.progress_s / b.chain.time_s;
        char overlay[32];
        snprintf(overlay, sizeof(overlay), "%.1f / %.1fs", b.progress_s, b.chain.time_s);
        ImGui::ProgressBar(frac, ImVec2(-1, 0), overlay);
        static const char* prod_states[] = {"IDLE","RUNNING","FULL","NO_INPUT"};
        int si = (int)b.chain.state;
        if (si < 0 || si > 3) si = 0;
        ImGui::Text("State: %s", prod_states[si]);
    } else {
        ImGui::TextDisabled("No production chain");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawPlayerController(entt::entity e) {
    auto& reg = Registry::Get();
    if (!reg.all_of<PlayerController>(e)) return;
    if (!CollapsingSection("PlayerController##insp", {0.9f, 0.9f, 0.9f, 1.f})) return;

    auto& pc = reg.get<PlayerController>(e);
    ImGui::DragFloat("Move Speed##pc", &pc.move_speed, 0.1f, 0.5f, 20.f);
    ImGui::Text("Attack cooldown: %u ms", pc.attack_cooldown_ms);
    ImGui::Text("Last attack: %.0f ms", pc.last_attack_ms);
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawAIScript(entt::entity e) {
    auto& reg = Registry::Get();
    if (!reg.all_of<AIScript>(e)) return;
    if (!CollapsingSection("AIScript##insp", {0.7f, 0.2f, 0.9f, 1.f})) return;

    auto& sc = reg.get<AIScript>(e);
    ImGui::InputText("Func##aiscript", sc.script_func, sizeof(sc.script_func));
    ImGui::TextDisabled("Lua BT action function name");
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawAddComponent(entt::entity e) {
    auto& reg = Registry::Get();
    ImGui::Separator();
    if (ImGui::BeginMenu("Add Component##insp")) {
        if (ImGui::MenuItem("Health") && !reg.all_of<Health>(e))
            reg.emplace<Health>(e, Health{100.f, 100.f});
        if (ImGui::MenuItem("Combat") && !reg.all_of<Combat>(e))
            reg.emplace<Combat>(e, Combat::MakeBandit());
        if (ImGui::MenuItem("AIAgent") && !reg.all_of<AIAgent>(e))
            reg.emplace<AIAgent>(e);
        if (ImGui::MenuItem("NavAgent") && !reg.all_of<NavAgent>(e))
            reg.emplace<NavAgent>(e);
        if (ImGui::MenuItem("Inventory") && !reg.all_of<Inventory>(e)) {
            auto& inv = reg.emplace<Inventory>(e);
            inv.Clear();
        }
        if (ImGui::MenuItem("PlayerController") && !reg.all_of<PlayerController>(e))
            reg.emplace<PlayerController>(e, PlayerController{5.f, 0.f, 800u, 1u});
        if (ImGui::MenuItem("AIScript") && !reg.all_of<AIScript>(e)) {
            auto& sc = reg.emplace<AIScript>(e);
            sc.script_func[0] = '\0';
        }
        ImGui::EndMenu();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::Draw() {
    if (!EditorCore::Get().panels_visible[1]) return;

    ImGui::SetNextWindowSize(ImVec2(300, 550), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(1280 - 305, 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector##insp", &EditorCore::Get().panels_visible[1])) {
        ImGui::End();
        return;
    }

    auto& ec = EditorCore::Get();
    entt::entity e = ec.GetPrimary();

    if (e == entt::null || !Registry::Get().valid(e)) {
        ImGui::TextDisabled("No entity selected");
        ImGui::End();
        return;
    }

    uint32_t id = (uint32_t)entt::to_integral(e);
    ImGui::Text("Entity %u", id);
    ImGui::SameLine();
    if (ImGui::SmallButton("Focus")) ec.FocusOnSelected();
    ImGui::Separator();

    DrawTransform(e);
    DrawHealth(e);
    DrawCombat(e);
    DrawAIAgent(e);
    DrawNavAgent(e);
    DrawInventory(e);
    DrawBuilding(e);
    DrawPlayerController(e);
    DrawAIScript(e);
    DrawAddComponent(e);

    ImGui::End();
}
#endif
