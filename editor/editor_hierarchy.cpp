#ifdef MONKEY_DUST_EDITOR
#include "editor_hierarchy.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/ai_agent.h>
#include <monkey_dust/components/combat.h>
#include <monkey_dust/components/building.h>
#include <monkey_dust/components/inventory.h>
#include <monkey_dust/components/player_controller.h>
#include <monkey_dust/components/bt_component.h>
#include <monkey_dust/components/ai_script.h>
#include <monkey_dust/components/nav_agent.h>
#include <monkey_dust/components/renderable.h>
#include <monkey_dust/components/stat_sheet.h>
#include <monkey_dust/components/faction.h>
#include <monkey_dust/building/build_system.h>
#include <monkey_dust/world/transform_soa.h>
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
void EditorHierarchy::RefreshCache() {
    auto& reg = MdRegistry::Get();
    entity_cache_count_ = 0;
    reg.Each([&](MdEntity e) -> bool {
        if (entity_cache_count_ >= MAX_CACHE) return false;
        entity_cache_[entity_cache_count_++] = e;
        return true;
    });
}

// ─────────────────────────────────────────────────────────────────────────────
const char* EditorHierarchy::EntityIcon(MdEntity e) const {
    auto& reg = MdRegistry::Get();
    if ((reg.Handle(e).has<PlayerController>()))   return "[PLY]";
    if ((reg.Handle(e).has<AIAgent>()))            return "[NPC]";
    if ((reg.Handle(e).has<Building>()))           return "[BLD]";
    if ((reg.Handle(e).has<Inventory>()))          return "[INV]";
    if ((reg.Handle(e).has<AIScript>()))           return "[LUA]";
    return "[---]";
}

void EditorHierarchy::EntityLabel(MdEntity e, char* buf, int len) const {
    auto& reg = MdRegistry::Get();
    uint32_t id = e.ToIntegral();
    if ((reg.Handle(e).has<AIAgent>())) {
        const auto& ai = reg.Handle(e).get_mut<AIAgent>();
        snprintf(buf, len, "%s Entity_%u [f%u LOD%u]",
                 EntityIcon(e), id, ai.faction_id, (uint32_t)ai.lod_level);
    } else if ((reg.Handle(e).has<Building>())) {
        const auto& b = reg.Handle(e).get_mut<Building>();
        const BuildingDef* d = BuildSystem::Get().GetDef(b.def_id);
        snprintf(buf, len, "%s Entity_%u [%s]",
                 EntityIcon(e), id, d ? d->name : "?");
    } else {
        snprintf(buf, len, "%s Entity_%u", EntityIcon(e), id);
    }
}

ImVec4 EditorHierarchy::EntityColor(MdEntity e) const {
    auto& reg = MdRegistry::Get();
    if ((reg.Handle(e).has<Combat>()) && reg.Handle(e).get_mut<Combat>().is_dead)
        return {1.0f, 0.3f, 0.3f, 1.0f}; // RED  = dead
    if ((reg.Handle(e).has<AIAgent>())) {
        uint8_t lod = reg.Handle(e).get_mut<AIAgent>().lod_level;
        if (lod == 2) return {0.5f, 0.5f, 0.5f, 1.0f}; // GRAY   = FROZEN
        if (lod == 1) return {1.0f, 0.9f, 0.3f, 1.0f}; // YELLOW = LOW
        return {0.3f, 1.0f, 0.5f, 1.0f};                // GREEN  = HIGH
    }
    if ((reg.Handle(e).has<PlayerController>()))
        return {0.4f, 0.8f, 1.0f, 1.0f}; // CYAN = player
    return {0.85f, 0.85f, 0.85f, 1.0f};  // WHITE = other
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorHierarchy::DrawContextMenu(MdEntity e) {
    auto& ec  = EditorCore::Get();
    auto& reg = MdRegistry::Get();

    if (ImGui::MenuItem("Select"))    ec.Select(e);
    if (ImGui::MenuItem("Duplicate")) {
        if (reg.Valid(e) && (reg.Handle(e).has<WorldTransform>())) {
            auto dst = reg.Create();
            reg.Handle(dst).emplace<WorldTransform>(reg.Handle(e).get_mut<WorldTransform>());
            auto& tr = reg.Handle(dst).get_mut<WorldTransform>();
            tr.x += 1.f; tr.slot = 0xFFFFFFFFu;
            uint32_t fid = (reg.Handle(e).has<AIAgent>()) ? reg.Handle(e).get_mut<AIAgent>().faction_id : 0u;
            tr.slot = TransformSoA::Get().Alloc(dst, tr.x, tr.z, (uint8_t)fid);
            // Copy all components present on source.
            if ((reg.Handle(e).has<AIAgent>()))       reg.Handle(dst).emplace<AIAgent>(reg.Handle(e).get_mut<AIAgent>());
            if ((reg.Handle(e).has<Health>()))         reg.Handle(dst).emplace<Health>(reg.Handle(e).get_mut<Health>());
            if ((reg.Handle(e).has<Combat>()))         reg.Handle(dst).emplace<Combat>(reg.Handle(e).get_mut<Combat>());
            if ((reg.Handle(e).has<Building>()))       reg.Handle(dst).emplace<Building>(reg.Handle(e).get_mut<Building>());
            if ((reg.Handle(e).has<Inventory>()))      reg.Handle(dst).emplace<Inventory>(reg.Handle(e).get_mut<Inventory>());
            if ((reg.Handle(e).has<BTComponent>()))    reg.Handle(dst).emplace<BTComponent>(reg.Handle(e).get_mut<BTComponent>());
            if ((reg.Handle(e).has<AIScript>()))       reg.Handle(dst).emplace<AIScript>(reg.Handle(e).get_mut<AIScript>());
            if ((reg.Handle(e).has<NavAgent>()))       reg.Handle(dst).emplace<NavAgent>(reg.Handle(e).get_mut<NavAgent>());
            if ((reg.Handle(e).has<Renderable>()))     reg.Handle(dst).emplace<Renderable>(reg.Handle(e).get_mut<Renderable>());
            if ((reg.Handle(e).has<StatSheet>()))      reg.Handle(dst).emplace<StatSheet>(reg.Handle(e).get_mut<StatSheet>());
            if ((reg.Handle(e).has<Faction>()))          reg.Handle(dst).emplace<Faction>(reg.Handle(e).get_mut<Faction>());
            ec.Select(dst);
            cache_refresh_counter_ = 0;
        }
    }
    if (ImGui::MenuItem("Delete")) {
        ec.Deselect(e);
        if (reg.Valid(e) && (reg.Handle(e).has<WorldTransform>()))
            TransformSoA::Get().Free(e);
        if (reg.Valid(e)) reg.Destroy(e);
        cache_refresh_counter_ = 0;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("BT Reset")) {
        if (reg.Valid(e) && (reg.Handle(e).has<BTComponent>()))
            reg.Handle(e).get_mut<BTComponent>().bt.reset();
    }
    if (ImGui::MenuItem("Kill NPC")) {
        if (reg.Valid(e)) {
            if ((reg.Handle(e).has<Combat>()))  reg.Handle(e).get_mut<Combat>().is_dead = true;
            if ((reg.Handle(e).has<Health>())) { auto& h = reg.Handle(e).get_mut<Health>(); for (int _i=0;_i<LIMB_COUNT;++_i) h.hp[_i]=0.f; }
        }
    }
    if (ImGui::MenuItem("Freeze LOD")) {
        if (reg.Valid(e) && (reg.Handle(e).has<AIAgent>()))
            reg.Handle(e).get_mut<AIAgent>().lod_level = 2;
    }
    if (ImGui::MenuItem("Unfreeze LOD")) {
        if (reg.Valid(e) && (reg.Handle(e).has<AIAgent>()))
            reg.Handle(e).get_mut<AIAgent>().lod_level = 0;
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Add Component")) {
        if (ImGui::MenuItem("Health"))  { if (reg.Valid(e) && !(reg.Handle(e).has<Health>()))  reg.Handle(e).emplace<Health>(Health{100.f, 100.f}); }
        if (ImGui::MenuItem("Combat"))  { if (reg.Valid(e) && !(reg.Handle(e).has<Combat>()))  reg.Handle(e).emplace<Combat>(); }
        if (ImGui::MenuItem("AIAgent")) { if (reg.Valid(e) && !(reg.Handle(e).has<AIAgent>())) reg.Handle(e).emplace<AIAgent>(); }
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
    DrawContent();
    ImGui::End();
}

void EditorHierarchy::DrawContent() {

    auto& ec  = EditorCore::Get();
    auto& reg = MdRegistry::Get();

    // Header
    ImGui::Text("Entities (%d)", entity_cache_count_);
    ImGui::SameLine();
    if (ImGui::SmallButton("[+]")) {
        auto e = reg.Create();
        reg.Handle(e).emplace<WorldTransform>();
        auto& tr = reg.Handle(e).get_mut<WorldTransform>();
        tr.x = ec.cam_target.x; tr.y = 0.f; tr.z = ec.cam_target.z; tr.rot_y = 0.f;
        tr.slot = TransformSoA::Get().Alloc(e, tr.x, tr.z, 0);
        ec.Select(e);
        cache_refresh_counter_ = 0;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("[X]")) {
        for (int i = ec.selected_count - 1; i >= 0; --i) {
            MdEntity e = ec.selected[i];
            if (!reg.Valid(e)) continue;
            if ((reg.Handle(e).has<WorldTransform>())) TransformSoA::Get().Free(e);
            reg.Destroy(e);
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
    static MdEntity display[MAX_CACHE];
    static char         display_labels[MAX_CACHE][128];
    int display_count = 0;
    for (int i = 0; i < entity_cache_count_; ++i) {
        MdEntity e = entity_cache_[i];
        if (!reg.Valid(e)) continue;
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
            MdEntity e = display[i];
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

}
#endif
