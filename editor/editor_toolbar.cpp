#ifdef MONKEY_DUST_EDITOR
#include "editor_toolbar.h"
#include <monkey_dust/ecs/registry.h>
#include "game_state.h"
#include <monkey_dust/world/world_transform.h>
#include "components/health.h"
#include "components/ai_agent.h"
#include "components/renderable.h"
#include "components/combat.h"
#include "components/building.h"
#include "world/faction_system.h"
#include "building/build_system.h"
#include "dialog/dialog_system.h"
#include "quest/quest_system.h"
#include <monkey_dust/nav/nav_system.h>
#include <monkey_dust/save/save_system.h>
#include <monkey_dust/tools/debug_system.h>
#include "scene_serializer.h"
#include "icon_definitions.h"
#include "raylib.h"
#include "raymath.h"
#ifdef MD_OPENGL43_ENABLED
#include <monkey_dust/world/transform_soa.h>
#endif
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
void EditorToolbar::Draw(float dt) {
    DrawMenuBar();
    DrawButtonBar();

    // Hotkeys — only when ImGui doesn't want keyboard
    if (!ImGui::GetIO().WantCaptureKeyboard) {
        auto& ec = EditorCore::Get();
        if (IsKeyPressed(KEY_W)) ec.gizmo_op    = EditorGizmoOp::TRANSLATE;
        if (IsKeyPressed(KEY_E)) ec.gizmo_op    = EditorGizmoOp::ROTATE;
        if (IsKeyPressed(KEY_R)) ec.gizmo_op    = EditorGizmoOp::SCALE;
        if (IsKeyPressed(KEY_G)) ec.gizmo_space = (ec.gizmo_space == EditorGizmoSpace::WORLD)
                                                   ? EditorGizmoSpace::LOCAL
                                                   : EditorGizmoSpace::WORLD;
        if (IsKeyPressed(KEY_F)) ec.FocusOnSelected();
    }
    (void)dt;
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorToolbar::DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;

    // ── File ─────────────────────────────────────────────────
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene")) {
            EditorCore::Get().DeselectAll();
            Registry::Get().clear();
#ifdef MD_OPENGL43_ENABLED
            TransformSoA::Get().Init();
#endif
            TraceLog(LOG_INFO, "[Editor] New scene");
        }
        if (ImGui::MenuItem("Import Scene (.json)...")) {
            SceneSerializer::Import(scene_path_);
        }
        if (ImGui::MenuItem("Export Scene (.json)...")) {
            SceneSerializer::Export(scene_path_);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_SAVE " Save Game (F5)")) {
            SaveSystem::Get().SaveAsync(SaveSystem::DefaultPath());
            TraceLog(LOG_INFO, "[Editor] Async save → %s", SaveSystem::DefaultPath());
        }
        if (ImGui::MenuItem(ICON_LOAD " Load Game (F9)")) {
            SaveSystem::Get().Load(SaveSystem::DefaultPath());
            TraceLog(LOG_INFO, "[Editor] Load ← %s", SaveSystem::DefaultPath());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit Editor")) EditorCore::Get().editor_open = false;
        ImGui::EndMenu();
    }

    // ── Edit ─────────────────────────────────────────────────
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem(ICON_UNDO " Undo", "Ctrl+Z"))
            EditorCore::Get().Undo();
        if (ImGui::MenuItem(ICON_REDO " Redo", "Ctrl+Y"))
            EditorCore::Get().Redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            auto& ec  = EditorCore::Get();
            auto& reg = Registry::Get();
            entt::entity src = ec.GetPrimary();
            if (reg.valid(src) && reg.all_of<WorldTransform>(src)) {
                entt::entity dst = reg.create();
                auto& tr = reg.emplace<WorldTransform>(dst, reg.get<WorldTransform>(src));
                tr.x += 1.f;
#ifdef MD_OPENGL43_ENABLED
                tr.slot = TransformSoA::Get().Alloc(dst, tr.x, tr.z, 0);
#endif
                if (reg.all_of<Health>(src))    reg.emplace<Health>(dst,    reg.get<Health>(src));
                if (reg.all_of<AIAgent>(src))   reg.emplace<AIAgent>(dst,   reg.get<AIAgent>(src));
                if (reg.all_of<Renderable>(src))reg.emplace<Renderable>(dst,reg.get<Renderable>(src));
                ec.Select(dst);
            }
        }
        if (ImGui::MenuItem("Delete", "Del")) {
            auto& ec  = EditorCore::Get();
            auto& reg = Registry::Get();
            for (int i = ec.selected_count - 1; i >= 0; --i) {
                entt::entity e = ec.selected[i];
                if (!reg.valid(e)) continue;
#ifdef MD_OPENGL43_ENABLED
                if (reg.all_of<WorldTransform>(e))
                    TransformSoA::Get().Free(e);
#endif
                reg.destroy(e);
            }
            ec.DeselectAll();
        }
        if (ImGui::MenuItem("Select All", "Ctrl+A")) {
            auto& ec  = EditorCore::Get();
            auto& reg = Registry::Get();
            ec.DeselectAll();
            for (auto e : reg.storage<entt::entity>())
                if (ec.selected_count < EditorCore::MAX_SELECTED)
                    ec.selected[ec.selected_count++] = e;
        }
        ImGui::EndMenu();
    }

    // ── View ─────────────────────────────────────────────────
    if (ImGui::BeginMenu("View")) {
        auto& pv = EditorCore::Get().panels_visible;
        ImGui::MenuItem("Hierarchy",   nullptr, &pv[0]);
        ImGui::MenuItem("Inspector",   nullptr, &pv[1]);
        ImGui::MenuItem("Assets",      nullptr, &pv[2]);
        ImGui::MenuItem("Console",     nullptr, &pv[3]);
        ImGui::MenuItem("Graphics",    nullptr, &pv[4]);
        ImGui::MenuItem("Camera",      nullptr, &pv[5]);
        ImGui::MenuItem("Animation",   nullptr, &pv[6]);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {
            for (int i = 0; i < 8; ++i) pv[i] = (i < 6);
        }
        ImGui::EndMenu();
    }

    // ── Scene ─────────────────────────────────────────────────
    if (ImGui::BeginMenu("Scene")) {
        if (ImGui::MenuItem("Reload JSON Data")) {
            FactionSystem::Get().LoadFromFile("data/factions/factions.json");
            BuildSystem::Get().LoadFromFile("data/buildings/buildings.json");
            DialogSystem::LoadFromFile("data/dialogs/dialogs.json");
            QuestSystem::Get().LoadFromFile("data/quests/quests.json");
            TraceLog(LOG_INFO, "[Editor] JSON data reloaded");
        }
        if (ImGui::MenuItem("Rebuild NavMesh")) {
            Vector3 t = EditorCore::Get().cam_target;
            NavSystem::Get().EnqueueRebuild(t.x, t.z, nullptr, 0, nullptr, 0);
            TraceLog(LOG_INFO, "[Editor] NavMesh rebuild enqueued at (%.0f,%.0f)", t.x, t.z);
        }
        if (ImGui::MenuItem("Bake Lights (stub)")) {
            TraceLog(LOG_INFO, "[Editor] Bake not available: Phase 33 CSM is runtime-only");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Spawn NPC here (Bandit)"))  SpawnEntity("NPC Bandit");
        if (ImGui::MenuItem("Spawn NPC here (Trader)"))  SpawnEntity("NPC Trader");
        ImGui::EndMenu();
    }

    // ── Debug ─────────────────────────────────────────────────
    if (ImGui::BeginMenu("Debug")) {
        auto& ds = DebugSystem::Get();
        ImGui::MenuItem("Debug Overlay (F1)", nullptr, &ds.overlay_on);
        ImGui::MenuItem("SpatialGrid (F2)",   nullptr, &ds.grid_on);
        ImGui::MenuItem("NavMesh Wireframe",  nullptr, &ds.navmesh_on);
        ImGui::MenuItem("Screenshot Mode",    nullptr, &ds.clean_mode);
        ImGui::Separator();
        ImGui::MenuItem("Physics Paused", nullptr, &EditorCore::Get().physics_paused);
        ImGui::EndMenu();
    }

    // ── Help ──────────────────────────────────────────────────
    if (ImGui::BeginMenu("Help")) {
        ImGui::Text("monkey_dust Editor — Phase 34");
        ImGui::Text("F3 = toggle editor | W/E/R = gizmo");
        ImGui::Text("F = focus | G = local/world | Ctrl+Z/Y = undo/redo");
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorToolbar::DrawButtonBar() {
    float menu_h = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(0, menu_h));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, 30));
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("##EditorToolbar", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    auto& ec = EditorCore::Get();

    // [+] new entity popup
    if (ImGui::Button(ICON_NEW_SCENE " New Entity")) ImGui::OpenPopup("NewEntityPopup");
    if (ImGui::BeginPopup("NewEntityPopup")) {
        if (ImGui::MenuItem("Transform (empty)")) SpawnEntity("Transform");
        if (ImGui::MenuItem("NPC Bandit"))        SpawnEntity("NPC Bandit");
        if (ImGui::MenuItem("NPC Trader"))        SpawnEntity("NPC Trader");
        if (ImGui::MenuItem("NPC Holy"))          SpawnEntity("NPC Holy");
        if (ImGui::MenuItem("Building (stub)"))   SpawnEntity("Building");
        if (ImGui::MenuItem("Light (Phase 36)"))  TraceLog(LOG_INFO, "[Editor] Light entity: Phase 36");
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    // Gizmo mode buttons
    bool t_active = (ec.gizmo_op == EditorGizmoOp::TRANSLATE);
    bool r_active = (ec.gizmo_op == EditorGizmoOp::ROTATE);
    bool s_active = (ec.gizmo_op == EditorGizmoOp::SCALE);

    if (t_active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_TRANSLATE)) ec.gizmo_op = EditorGizmoOp::TRANSLATE;
    if (t_active) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (r_active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_ROTATE)) ec.gizmo_op = EditorGizmoOp::ROTATE;
    if (r_active) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (s_active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_SCALE)) ec.gizmo_op = EditorGizmoOp::SCALE;
    if (s_active) ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    // Space toggle
    bool is_global = (ec.gizmo_space == EditorGizmoSpace::WORLD);
    if (is_global) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_GLOBAL)) {
        ec.gizmo_space = is_global ? EditorGizmoSpace::LOCAL : EditorGizmoSpace::WORLD;
    }
    if (is_global) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s space", is_global ? "World" : "Local");

    ImGui::SameLine();

    // Physics pause
    if (ec.physics_paused) ImGui::PushStyleColor(ImGuiCol_Button, {0.7f, 0.3f, 0.1f, 1.f});
    if (ImGui::Button(ICON_PHYSICS)) ec.physics_paused = !ec.physics_paused;
    if (ec.physics_paused) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Physics: %s", ec.physics_paused ? "PAUSED" : "RUNNING");

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    // FPS
    char fps_buf[32];
    snprintf(fps_buf, sizeof(fps_buf), "%.0f FPS", (float)GetFPS());
    ImGui::TextDisabled("%s", fps_buf);

    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorToolbar::SpawnEntity(const char* type) {
    auto& reg = Registry::Get();
    auto& ec  = EditorCore::Get();
    Vector3 pos = ec.cam_target;

    if (strcmp(type, "Transform") == 0) {
        auto e = reg.create();
        auto& tr = reg.emplace<WorldTransform>(e);
        tr.x = pos.x; tr.y = 0.f; tr.z = pos.z; tr.rot_y = 0.f;
#ifdef MD_OPENGL43_ENABLED
        tr.slot = TransformSoA::Get().Alloc(e, pos.x, pos.z, 0);
#endif
        ec.Select(e);
        TraceLog(LOG_INFO, "[Editor] Spawned Transform entity at (%.1f,%.1f)", pos.x, pos.z);
        return;
    }

    uint32_t faction = 0;
    if      (strcmp(type, "NPC Bandit") == 0) faction = 1;
    else if (strcmp(type, "NPC Trader") == 0) faction = 2;
    else if (strcmp(type, "NPC Holy")   == 0) faction = 3;
    else if (strcmp(type, "Building")   == 0) {
        TraceLog(LOG_INFO, "[Editor] Building spawn: use BuildSystem::Get().Place() directly");
        return;
    }

    auto e = reg.create();
    auto& tr = reg.emplace<WorldTransform>(e);
    tr.x = pos.x; tr.y = 0.f; tr.z = pos.z; tr.rot_y = 0.f;
#ifdef MD_OPENGL43_ENABLED
    tr.slot = TransformSoA::Get().Alloc(e, pos.x, pos.z, faction);
#endif
    auto& ai = reg.emplace<AIAgent>(e);
    ai.faction_id = faction;
    ai.lod_level  = 0;
    auto& hp = reg.emplace<Health>(e);
    hp.current = hp.max = 100.f;
    reg.emplace<Combat>(e);
    reg.emplace<Renderable>(e);

    ec.Select(e);
    TraceLog(LOG_INFO, "[Editor] Spawned %s at (%.1f,%.1f)", type, pos.x, pos.z);
}
#endif
