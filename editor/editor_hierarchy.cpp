#ifdef MONKEY_DUST_EDITOR
#include "editor_hierarchy.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/ai_agent.h>
#include <monkey_dust/components/combat.h>
#include <monkey_dust/components/building.h>
#include <monkey_dust/components/inventory.h>
#include <monkey_dust/components/player_controller.h>
#include <monkey_dust/components/bt_component.h>
#include <monkey_dust/components/ai_script.h>
#include "building/build_system.h"
#ifdef MD_OPENGL43_ENABLED
#include <monkey_dust/world/transform_soa.h>
#endif
#include <entt/entt.hpp>
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
void EditorHierarchy::RefreshCache() {
    auto& reg = Registry::Get();
    entity_cache_count_ = 0;
    for (auto e : reg.storage<entt::entity>()) {
        if (entity_cache_count_ >= MAX_CACHE) break;
        entity_cache_[entity_cache_count_++] = e;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
const char* EditorHierarchy::EntityIcon(entt::entity e) const {
    auto& reg = Registry::Get();
    if (reg.all_of<PlayerController>(e))   return "[PLY]";
    if (reg.all_of<AIAgent>(e))            return "[NPC]";
    if (reg.all_of<Building>(e))           return "[BLD]";
    if (reg.all_of<Inventory>(e))          return "[INV]";
    if (reg.all_of<AIScript>(e))           return "[LUA]";
    return "[---]";
}

void EditorHierarchy::EntityLabel(entt::entity e, char* buf, int len) const {
    auto& reg = Registry::Get();
    uint32_t id = (uint32_t)entt::to_integral(e);
    if (reg.all_of<AIAgent>(e)) {
        const auto& ai = reg.get<AIAgent>(e);
        snprintf(buf, len, "%s Entity_%u [f%u LOD%u]",
                 EntityIcon(e), id, ai.faction_id, (uint32_t)ai.lod_level);
    } else if (reg.all_of<Building>(e)) {
        const auto& b = reg.get<Building>(e);
        const BuildingDef* d = BuildSystem::Get().GetDef(b.def_id);
        snprintf(buf, len, "%s Entity_%u [%s]",
                 EntityIcon(e), id, d ? d->name : "?");
    } else {
        snprintf(buf, len, "%s Entity_%u", EntityIcon(e), id);
    }
}

ImVec4 EditorHierarchy::EntityColor(entt::entity e) const {
    auto& reg = Registry::Get();
    if (reg.all_of<Combat>(e) && reg.get<Combat>(e).is_dead)
        return {1.0f, 0.3f, 0.3f, 1.0f}; // RED  = dead
    if (reg.all_of<AIAgent>(e)) {
        uint8_t lod = reg.get<AIAgent>(e).lod_level;
        if (lod == 2) return {0.5f, 0.5f, 0.5f, 1.0f}; // GRAY   = FROZEN
        if (lod == 1) return {1.0f, 0.9f, 0.3f, 1.0f}; // YELLOW = LOW
        return {0.3f, 1.0f, 0.5f, 1.0f};                // GREEN  = HIGH
    }
    if (reg.all_of<PlayerController>(e))
        return {0.4f, 0.8f, 1.0f, 1.0f}; // CYAN = player
    return {0.85f, 0.85f, 0.85f, 1.0f};  // WHITE = other
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorHierarchy::DrawContextMenu(entt::entity e) {
    auto& ec  = EditorCore::Get();
    auto& reg = Registry::Get();

    if (ImGui::MenuItem("Select"))    ec.Select(e);
    if (ImGui::MenuItem("Duplicate")) {
        if (reg.valid(e) && reg.all_of<WorldTransform>(e)) {
            auto dst = reg.create();
            auto& tr = reg.emplace<WorldTransform>(dst, reg.get<WorldTransform>(e));
            tr.x += 1.f;
#ifdef MD_OPENGL43_ENABLED
            uint32_t fid = reg.all_of<AIAgent>(e) ? reg.get<AIAgent>(e).faction_id : 0u;
            tr.slot = TransformSoA::Get().Alloc(dst, tr.x, tr.z, (uint8_t)fid);
#endif
            if (reg.all_of<AIAgent>(e))       reg.emplace<AIAgent>(dst,      reg.get<AIAgent>(e));
            if (reg.all_of<Health>(e))         reg.emplace<Health>(dst,       reg.get<Health>(e));
            if (reg.all_of<Combat>(e))         reg.emplace<Combat>(dst,       reg.get<Combat>(e));
            ec.Select(dst);
            cache_refresh_counter_ = 0; // force refresh
        }
    }
    if (ImGui::MenuItem("Delete")) {
        ec.Deselect(e);
#ifdef MD_OPENGL43_ENABLED
        if (reg.valid(e) && reg.all_of<WorldTransform>(e))
            TransformSoA::Get().Free(e);
#endif
        if (reg.valid(e)) reg.destroy(e);
        cache_refresh_counter_ = 0;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("BT Reset")) {
        if (reg.valid(e) && reg.all_of<BTComponent>(e))
            reg.get<BTComponent>(e).bt.reset();
    }
    if (ImGui::MenuItem("Kill NPC")) {
        if (reg.valid(e)) {
            if (reg.all_of<Combat>(e))  reg.get<Combat>(e).is_dead = true;
            if (reg.all_of<Health>(e))  reg.get<Health>(e).current = 0.f;
        }
    }
    if (ImGui::MenuItem("Freeze LOD")) {
        if (reg.valid(e) && reg.all_of<AIAgent>(e))
            reg.get<AIAgent>(e).lod_level = 2;
    }
    if (ImGui::MenuItem("Unfreeze LOD")) {
        if (reg.valid(e) && reg.all_of<AIAgent>(e))
            reg.get<AIAgent>(e).lod_level = 0;
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Add Component")) {
        if (ImGui::MenuItem("Health"))  { if (reg.valid(e) && !reg.all_of<Health>(e))  reg.emplace<Health>(e, Health{100.f, 100.f}); }
        if (ImGui::MenuItem("Combat"))  { if (reg.valid(e) && !reg.all_of<Combat>(e))  reg.emplace<Combat>(e); }
        if (ImGui::MenuItem("AIAgent")) { if (reg.valid(e) && !reg.all_of<AIAgent>(e)) reg.emplace<AIAgent>(e); }
        ImGui::EndMenu();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorHierarchy::Draw() {
    if (!EditorCore::Get().panels_visible[0]) return;

    ImGui::SetNextWindowSize(ImVec2(280, 500), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(0, 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Scene##Hierarchy", &EditorCore::Get().panels_visible[0])) {
        ImGui::End();
        return;
    }

    auto& ec  = EditorCore::Get();
    auto& reg = Registry::Get();

    // Header
    ImGui::Text("Entities (%d)", entity_cache_count_);
    ImGui::SameLine();
    if (ImGui::SmallButton("[+]")) {
        auto e = reg.create();
        auto& tr = reg.emplace<WorldTransform>(e);
        tr.x = ec.cam_target.x; tr.y = 0.f; tr.z = ec.cam_target.z; tr.rot_y = 0.f;
#ifdef MD_OPENGL43_ENABLED
        tr.slot = TransformSoA::Get().Alloc(e, tr.x, tr.z, 0);
#endif
        ec.Select(e);
        cache_refresh_counter_ = 0;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("[X]")) {
        for (int i = ec.selected_count - 1; i >= 0; --i) {
            entt::entity e = ec.selected[i];
            if (!reg.valid(e)) continue;
#ifdef MD_OPENGL43_ENABLED
            if (reg.all_of<WorldTransform>(e)) TransformSoA::Get().Free(e);
#endif
            reg.destroy(e);
        }
        ec.DeselectAll();
        cache_refresh_counter_ = 0;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("[R]")) cache_refresh_counter_ = 0;

    // Search filter
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##filter", entity_filter_, sizeof(entity_filter_));

    ImGui::Separator();

    // Refresh cache every 10 frames or on demand
    if (++cache_refresh_counter_ >= 10) {
        cache_refresh_counter_ = 0;
        RefreshCache();
    }

    // Pre-filter into a display list so ListClipper never skips items mid-loop.
    // (ListClipper requires every index in [DisplayStart, DisplayEnd) to render
    // something that advances the cursor — a 'continue' inside would crash it.)
    static entt::entity display[MAX_CACHE];
    static char         display_labels[MAX_CACHE][128];
    int display_count = 0;
    for (int i = 0; i < entity_cache_count_; ++i) {
        entt::entity e = entity_cache_[i];
        if (!reg.valid(e)) continue;
        EntityLabel(e, display_labels[display_count], 128);
        if (entity_filter_[0] != '\0' &&
            strstr(display_labels[display_count], entity_filter_) == nullptr)
            continue;
        display[display_count++] = e;
    }

    // Scrollable list with clipper
    ImGui::BeginChild("##HierarchyScroll", ImVec2(0, -26), false);

    ImGuiListClipper clipper;
    clipper.Begin(display_count);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            entt::entity e = display[i];
            bool selected = ec.IsSelected(e);
            ImVec4 col = EntityColor(e);
            ImGui::PushStyleColor(ImGuiCol_Text, col);

            if (ImGui::Selectable(display_labels[i], selected,
                    ImGuiSelectableFlags_AllowDoubleClick,
                    ImVec2(0, 0)))
            {
                bool multi = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
                ec.Select(e, multi);
                if (ImGui::IsMouseDoubleClicked(0)) ec.FocusOnSelected();
            }

            ImGui::PopStyleColor();

            if (ImGui::BeginPopupContextItem()) {
                DrawContextMenu(e);
                ImGui::EndPopup();
            }
        }
    }
    clipper.End();
    ImGui::EndChild();

    // Bottom status bar
    ImGui::Separator();
    if (entity_filter_[0] != '\0')
        ImGui::TextDisabled("sel: %d | shown: %d / %d", ec.selected_count, display_count, entity_cache_count_);
    else
        ImGui::TextDisabled("sel: %d | total: %d", ec.selected_count, entity_cache_count_);

    ImGui::End();
}
#endif
