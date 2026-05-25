#ifdef MONKEY_DUST_EDITOR
#include "editor_inspector.h"
#include "editor_core.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/combat.h>
#include <monkey_dust/components/ai_agent.h>
#include <monkey_dust/components/nav_agent.h>
#include <monkey_dust/components/inventory.h>
#include <monkey_dust/components/building.h>
#include <monkey_dust/components/player_controller.h>
#include <monkey_dust/components/ai_script.h>
#include <monkey_dust/components/renderable.h>
#include <monkey_dust/combat/hit_zones.h>
#include <monkey_dust/building/build_system.h>
#include "world/character_def.h"
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

}

// ─────────────────────────────────────────────────────────────────────────────
void EditorInspector::DrawHealth(entt::entity e) {
    auto& reg = Registry::Get();
    if (!reg.all_of<Health>(e)) return;
    if (!CollapsingSection("Health##insp", {0.9f, 0.2f, 0.2f, 1.f})) return;

    auto& hp = reg.get<Health>(e);
    float frac = hp.HpFraction();
    float cur  = hp.TotalHp(), mx = hp.TotalMax();
    char overlay[32];
    snprintf(overlay, sizeof(overlay), "%.0f / %.0f", cur, mx);
    ImGui::ProgressBar(frac, ImVec2(-1, 0), overlay);
    for (int i = 0; i < LIMB_COUNT; ++i) {
        char lbl[16]; snprintf(lbl, sizeof(lbl), "Limb %d##hp", i);
        ImGui::SliderFloat(lbl, &hp.hp[i], 0.f, hp.max[i]);
    }
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
            reg.emplace<PlayerController>(e, PlayerController{true, 5.f, 0.f, 800u, 1u});
        if (ImGui::MenuItem("AIScript") && !reg.all_of<AIScript>(e)) {
            auto& sc = reg.emplace<AIScript>(e);
            sc.script_func[0] = '\0';
        }
        ImGui::EndMenu();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GH-5: Character editor — morph sliders for per-entity CharacterDef.
// CharacterDef is not an ECS component; we store one per-entity using the entity
// integral id as a map key (static fixed array up to MAX_CHAR_SLOTS).
static constexpr int MAX_CHAR_SLOTS = 256;
static CharacterDef s_char_defs[MAX_CHAR_SLOTS];
static uint32_t     s_char_slot_ids[MAX_CHAR_SLOTS] = {};
static int          s_char_slot_count = 0;

static CharacterDef* GetOrCreateCharDef(entt::entity e) {
    uint32_t eid = (uint32_t)entt::to_integral(e);
    for (int i = 0; i < s_char_slot_count; ++i)
        if (s_char_slot_ids[i] == eid) return &s_char_defs[i];
    if (s_char_slot_count >= MAX_CHAR_SLOTS) return nullptr;
    int idx = s_char_slot_count++;
    s_char_slot_ids[idx] = eid;
    s_char_defs[idx] = CharacterDef{};
    return &s_char_defs[idx];
}

void EditorInspector::DrawCharacterDef(entt::entity e) {
    if (!Registry::Get().all_of<Renderable>(e)) return;
    if (!CollapsingSection("Character##insp", {0.3f, 0.7f, 0.9f, 1.f})) return;

    CharacterDef* def = GetOrCreateCharDef(e);
    if (!def) { ImGui::TextDisabled("slot limit reached"); return; }

    ImGui::Text("Body");
    ImGui::DragFloat("Height##cd", &def->height,  0.01f, 0.80f, 1.20f);
    ImGui::DragFloat("Bulk##cd",   &def->bulk,    0.01f, 0.70f, 1.40f);

    ImGui::Separator();
    ImGui::Text("Skin");
    ImGui::ColorEdit3("Skin##cd",  &def->skin_r);
    ImGui::ColorEdit3("Hair##cd",  &def->hair_r);
    ImGui::DragFloat("Tint##cd",   &def->color_strength, 0.01f, 0.f, 1.f);
    ImGui::DragFloat("Saturation##cd",  &def->skintone_sat, 0.01f, 0.f, 2.f);
    ImGui::DragFloat("Brightness##cd",  &def->skintone_bri, 0.005f, -0.3f, 0.3f);

    ImGui::Separator();
    int mc = def->morph_count < CHARDEF_MAX_MORPHS ? def->morph_count : CHARDEF_MAX_MORPHS;
    ImGui::Text("Morphs (%d / %d)", mc, CHARDEF_MAX_MORPHS);
    if (ImGui::SmallButton("+##morph") && mc < CHARDEF_MAX_MORPHS)
        def->morph_count = (uint8_t)(mc + 1);
    ImGui::SameLine();
    if (ImGui::SmallButton("-##morph") && mc > 0)
        def->morph_count = (uint8_t)(mc - 1);

    for (int i = 0; i < mc; ++i) {
        char lbl[24]; snprintf(lbl, sizeof(lbl), "Morph %d##cm%d", i, i);
        ImGui::SliderFloat(lbl, &def->morph_weights[i], 0.f, 1.f);
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
    DrawContent();
    ImGui::End();
}

void EditorInspector::DrawContent() {

    auto& ec = EditorCore::Get();
    entt::entity e = ec.GetPrimary();

    if (e == entt::null || !Registry::Get().valid(e)) {
        ImGui::TextDisabled("No entity selected");
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
    DrawCharacterDef(e);
    DrawAddComponent(e);

}
#endif
