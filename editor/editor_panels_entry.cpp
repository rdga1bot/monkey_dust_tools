// editor_panels_entry.cpp — extern "C" entry points for libeditor_panels.so.
// Called by EditorModule after dlopen/dlclose cycle.
//
// ImGui context is owned by the host .exe; editor_panels_init() must call
// ImGui::SetCurrentContext(ctx) so all ImGui calls use the host's context.
//
// GPU resources (RTTs, pipelines) are created/destroyed per Init/Shutdown cycle.
// Panel layout state persists via JSON (layout_path) across reloads.
#include "imgui.h"
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/platform/md_log.h>
#include "editor_core.h"
#include "editor_ui.h"
#include "item_editor.h"
#include "faction_editor.h"
#include "editor_world_panel.h"
#include "editor_world_3d_sdlgpu.h"
#include "editor_char_preview_sdlgpu.h"
#include "character_editor.h"
#include "npc_archetype_editor.h"
#include "editor_map_view.h"
#include "editor_node_graph.h"
#include "editor_layout.h"
#include "settings_editor.h"
#include "bug_capture.h"
#include "editor_reflect_bridge.h"
#include "editor_reflect_inspector.h"
#include "lua_editor_scenario_api.h"
#include "lua_editor_automation_api.h"
#include "editor_command_palette.h"
#include "editor_std_commands.h"
#include <monkey_dust/editor/cmd_registry.h>
#include <monkey_dust/scripting/lua_system.h>
#include <cstdio>
#include <cstring>

static constexpr const char* CFG_PATH = "data/editor_config.json";

// EDITOR_AUTOMATION_PLAN_v1.md Phase 0: this module's owner id for
// EditorCmdRegistry — every command registered from editor_panels_init()
// must be tagged with this id so editor_panels_shutdown() can purge
// exactly (and only) this module's commands before dlclose.
static constexpr uint32_t kPanelsModuleId = 1;

// ecs_world_t*, opaque across the dlopen boundary (see editor_module.h's
// Config::ecs_world doc comment). Stored so BuildUI can pass it into the
// Inspector tab every frame; re-set on every editor_panels_init() call.
static ecs_world_t* s_ecs_world = nullptr;

// Persistent panel layout (all tabs).  Loaded on init, saved on shutdown.
static EditorLayout::Layout s_lay;

extern "C" {

// ── Init — called after dlopen ────────────────────────────────────────────────
// ctx:         ImGuiContext* from host (must share for ImGui calls to work)
// ecs_world:   ecs_world_t* from host (Registry::Get().c_ptr()) — untyped
//              flecs C API only past this point; see editor_reflect_bridge.h
// gpu:         SDL_GPUDevice* (for RTT init)
// window:      SDL_Window*
// overlay_top: Y offset when running under RenderDoc overlay
// layout_path: JSON file for panel positions (e.g. "data/editor_layout.json")
void editor_panels_init(void* ctx, void* ecs_world, void* /*gpu*/, void* /*window*/,
                        float overlay_top, const char* layout_path, void* lua_system) {
    // Share host's ImGui context — CRITICAL, must be first.
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx));

    // Re-resolve every reflected component id fresh on every load/reload —
    // ids from a previous dlopen cycle are not valid in this one.
    s_ecs_world = static_cast<ecs_world_t*>(ecs_world);
    EcsReflectBridge::Get().Init(s_ecs_world);

    // Autonomy system (Etap 4): register md.editor_* into the HOST's
    // LuaSystem instance (see EditorModule::Config::lua_system doc comment —
    // LuaSystem::Get() is a header-inline singleton, would otherwise
    // duplicate independently inside this .so). Re-registering on every
    // reload is safe/idempotent — RegisterNamespaceFunction just overwrites
    // the same md.* table fields.
    if (lua_system) {
        RegisterLuaEditorScenarioAPI(*static_cast<LuaSystem*>(lua_system));
        RegisterLuaEditorAutomationAPI(*static_cast<LuaSystem*>(lua_system));
    }

    SettingsEditor::Load(CFG_PATH);
    ItemEditor::Load("data/items/items.json");
    FactionEditor::Load("data/factions/factions.json");
    NpcArchetypeEditor::Load("game/data/defs/npc_archetypes.json");
    WorldPanel::Init();

    WorldEditor3D_SDLGPU::Init("game/data/textures/md_terrain.dds", 29, 25);
    CharacterEditor::LoadJSON("game/data/chars/player.chardef");
    CharacterEditor::LoadMorphNames("game/data/chars/morph_names.txt");
    MapViewPanel::Get().Init();
    EditorCore::Get().Init();

    // Restore panel layout from previous session
    if (layout_path && layout_path[0]) {
        s_lay = EditorLayout::Layout{};        // reset to defaults first
        EditorLayout::Load(layout_path, s_lay);
        ItemEditor::g_detached         = s_lay.items.detached;
        ItemEditor::g_win_pos          = s_lay.items.pos;
        ItemEditor::g_win_size         = s_lay.items.size;
        FactionEditor::g_detached      = s_lay.factions.detached;
        FactionEditor::g_win_pos       = s_lay.factions.pos;
        FactionEditor::g_win_size      = s_lay.factions.size;
        NpcArchetypeEditor::g_detached = s_lay.npcs.detached;
        NpcArchetypeEditor::g_win_pos  = s_lay.npcs.pos;
        NpcArchetypeEditor::g_win_size = s_lay.npcs.size;
        CharacterEditor::g_detached    = s_lay.chars.detached;
        CharacterEditor::g_win_pos     = s_lay.chars.pos;
        CharacterEditor::g_win_size    = s_lay.chars.size;
        SettingsEditor::g_detached     = s_lay.settings.detached;
        SettingsEditor::g_win_pos      = s_lay.settings.pos;
        SettingsEditor::g_win_size     = s_lay.settings.size;
        // map/world/terrain/world3d used directly from s_lay in BuildUI
    }
    (void)overlay_top;

    // EDITOR_AUTOMATION_PLAN_v1.md Phase 0/1.1-1.2/1.3: register every std
    // command every init (including hot-reload re-init) — Register() is
    // idempotent (updates in place on repeat name hash), so this is safe to
    // call unconditionally on every load. Shared with game/src/main.cpp
    // (see RegisterStdEditorCommands's doc comment, editor_std_commands.h,
    // for why this can't just live inline here).
    RegisterStdEditorCommands(kPanelsModuleId);

    MD_LOG(MD_LOG_INFO, "[EditorPanels] init complete");
}

// ── Shutdown — called before dlclose ─────────────────────────────────────────
void editor_panels_shutdown(const char* layout_path) {
    if (layout_path && layout_path[0]) {
        // Sync header-namespace states back into s_lay before saving
        s_lay.items    = {ItemEditor::g_detached,         ItemEditor::g_win_pos,         ItemEditor::g_win_size};
        s_lay.factions = {FactionEditor::g_detached,      FactionEditor::g_win_pos,      FactionEditor::g_win_size};
        s_lay.npcs     = {NpcArchetypeEditor::g_detached, NpcArchetypeEditor::g_win_pos, NpcArchetypeEditor::g_win_size};
        s_lay.chars    = {CharacterEditor::g_detached,    CharacterEditor::g_win_pos,    CharacterEditor::g_win_size};
        s_lay.settings = {SettingsEditor::g_detached,    SettingsEditor::g_win_pos,     SettingsEditor::g_win_size};
        // map/world/terrain/world3d are updated live in BuildUI → already in s_lay
        EditorLayout::Save(layout_path, s_lay);
    }
    // Must join before dlclose() (below, in EditorModule::Shutdown) unloads
    // this .so out from under the thread's own code — see
    // WorldEditor3D_SDLGPU::Shutdown()'s doc comment for the race this fixes.
    WorldEditor3D_SDLGPU::Shutdown();
    EditorCore::Get().Shutdown();

    // EDITOR_AUTOMATION_PLAN_v1.md Phase 0 falsification probe: purge this
    // module's commands from the HOST-owned registry before dlclose, else
    // DeleteSelectedCmd's function pointer (a symbol inside THIS .so) goes
    // dangling the moment the .so is unmapped — the exact hazard the
    // registry's own header comment (cmd_registry.h) warns about.
    EditorCmdRegistry::Get().UnregisterModule(kPanelsModuleId);

    MD_LOG(MD_LOG_INFO, "[EditorPanels] shutdown complete");
}

// ── WaitLoaderReady — Phase 4 regression-suite fix ──────────────────────────
// EDITOR_AUTOMATION_PLAN_v1.md Phase 4: md.editor.trigger_hot_reload()
// (lua_editor_automation_api.cpp) previously returned as soon as
// editor_panels_init() spawned WorldEditor3D_SDLGPU's background GPU-
// resource loader thread, WITHOUT waiting for it to finish — a script
// that reloads and then quits shortly after (exactly what the Phase 4
// regress_hot_reload.lua scenario does) races that thread against process
// shutdown, reproduced as a SIGSEGV/SIGABRT in `coredumpctl` during this
// phase's own testing (~1-in-a-few runs when reload is called repeatedly).
// This reuses WorldEditor3D_SDLGPU::Shutdown()'s existing join-until-done
// logic (safe to call multiple times — joinable() is false after the
// first join, so a later real editor_panels_shutdown() call is a no-op)
// WITHOUT the rest of Shutdown()'s teardown, so EditorModule can make
// Reload() synchronous from the caller's point of view without actually
// tearing anything down.
void editor_panels_wait_loader_ready() {
    WorldEditor3D_SDLGPU::Shutdown();
}

// ── BuildUI — called inside ImGui frame ──────────────────────────────────────
// Returns active-viewport bitmask:
//   bit 0: 3D World, bit 1: CharPreview, bit 2: (unused), bit 3: Map View
uint32_t editor_panels_build_ui(float dt, float toolbar_h,
                                 char* status_msg, float* status_timer) {
    if (status_timer && *status_timer > 0.f) *status_timer -= dt;

    uint32_t flags = 0;
    // Autonomy system: md.editor_open_panel(name) forces a tab select this
    // frame. Consumed ONCE here (not per-tab) — EditorPanels_ConsumeForcedTab
    // clears the pending name on read, so a single local copy must be
    // compared against every candidate tab below.
    const char* forced_tab = EditorPanels_ConsumeForcedTab();

    EditorCore::Get().Update(dt);

    ImGuiIO& fio = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, toolbar_h});
    ImGui::SetNextWindowSize({fio.DisplaySize.x, fio.DisplaySize.y - toolbar_h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::Begin("##editor", nullptr,
        ImGuiWindowFlags_NoTitleBar    | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove        | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    ImGui::Separator();

    static constexpr ImGuiWindowFlags FLOAT_FLAGS =
        ImGuiWindowFlags_NoSavedSettings;

    static int s_active_tab = 0;
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Items")) { s_active_tab = 0;
            if (!ItemEditor::g_detached) {
                ImGui::Separator();
                ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                if (ItemEditor::DrawContent("data/items/items.json") && status_msg)
                    snprintf(status_msg, 64, "Items saved!");
            } else {
                ImVec2& pos = ItemEditor::g_win_pos;
                ImVec2& sz  = ItemEditor::g_win_size;
                const float min_y = toolbar_h + ImGui::GetFrameHeight() * 2 + 4.f;
                if (pos.y < min_y) pos.y = min_y;
                ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(sz,  ImGuiCond_Appearing);
                if (ImGui::Begin("Items##float", &ItemEditor::g_detached, FLOAT_FLAGS)) {
                    ImGui::Separator();
                    if (ItemEditor::DrawContent("data/items/items.json") && status_msg)
                        snprintf(status_msg, 64, "Items saved!");
                }
                pos = ImGui::GetWindowPos();
                if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
                sz = ImGui::GetWindowSize();
                ImGui::End();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Factions")) { s_active_tab = 1;
            if (!FactionEditor::g_detached) {
                ImGui::Separator();
                ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                if (FactionEditor::DrawContent("data/factions/factions.json") && status_msg)
                    snprintf(status_msg, 64, "Factions saved!");
            } else {
                ImVec2& pos = FactionEditor::g_win_pos;
                ImVec2& sz  = FactionEditor::g_win_size;
                const float min_y = toolbar_h + ImGui::GetFrameHeight() * 2 + 4.f;
                if (pos.y < min_y) pos.y = min_y;
                ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(sz,  ImGuiCond_Appearing);
                if (ImGui::Begin("Factions##float", &FactionEditor::g_detached, FLOAT_FLAGS)) {
                    ImGui::Separator();
                    if (FactionEditor::DrawContent("data/factions/factions.json") && status_msg)
                        snprintf(status_msg, 64, "Factions saved!");
                }
                pos = ImGui::GetWindowPos();
                if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
                sz = ImGui::GetWindowSize();
                ImGui::End();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Map")) { s_active_tab = 2;
            flags |= (1u << 3);
            auto draw_map = [&]() {
                ImGuiIO& mio = ImGui::GetIO();
                if (mio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) MapViewPanel::Get().Undo();
                if (mio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) MapViewPanel::Get().Redo();
                MapViewPanel::Get().Draw(dt);
            };
            if (!s_lay.map.detached) {
                ImGui::Separator();
                ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                draw_map();
            } else {
                ImVec2& pos = s_lay.map.pos; ImVec2& sz = s_lay.map.size;
                const float min_y = toolbar_h + ImGui::GetFrameHeight() * 2 + 4.f;
                if (pos.y < min_y) pos.y = min_y;
                ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(sz,  ImGuiCond_Appearing);
                if (ImGui::Begin("Map##float", &s_lay.map.detached, FLOAT_FLAGS)) {
                    ImGui::Separator();
                    draw_map();
                }
                pos = ImGui::GetWindowPos();
                if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
                sz = ImGui::GetWindowSize();
                ImGui::End();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("World")) { s_active_tab = 3;
            if (!s_lay.world.detached) {
                ImGui::Separator();
                ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                WorldPanel::Draw(dt);
            } else {
                ImVec2& pos = s_lay.world.pos; ImVec2& sz = s_lay.world.size;
                const float min_y = toolbar_h + ImGui::GetFrameHeight() * 2 + 4.f;
                if (pos.y < min_y) pos.y = min_y;
                ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(sz,  ImGuiCond_Appearing);
                if (ImGui::Begin("World##float", &s_lay.world.detached, FLOAT_FLAGS)) {
                    ImGui::Separator();
                    WorldPanel::Draw(dt);
                }
                pos = ImGui::GetWindowPos();
                if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
                sz = ImGui::GetWindowSize();
                ImGui::End();
            }
            ImGui::EndTabItem();
        }
        ImGuiTabItemFlags world3d_flags = (forced_tab && strcmp(forced_tab, "3D World") == 0)
            ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem("3D World", nullptr, world3d_flags)) { s_active_tab = 4;
            flags |= (1u << 0);
            ImVec2 avail = ImGui::GetContentRegionAvail();
            WorldEditor3D_SDLGPU::DrawImGui(avail.x, avail.y - 2, dt);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("NPCs")) { s_active_tab = 5;
            if (!NpcArchetypeEditor::g_detached) {
                ImGui::Separator();
                ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                NpcArchetypeEditor::DrawContent();
            } else {
                ImVec2& pos = NpcArchetypeEditor::g_win_pos;
                ImVec2& sz  = NpcArchetypeEditor::g_win_size;
                const float min_y = toolbar_h + ImGui::GetFrameHeight() * 2 + 4.f;
                if (pos.y < min_y) pos.y = min_y;
                ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(sz,  ImGuiCond_Appearing);
                if (ImGui::Begin("NPC Archetypes##float", &NpcArchetypeEditor::g_detached, FLOAT_FLAGS)) {
                    ImGui::Separator();
                    NpcArchetypeEditor::DrawContent();
                }
                pos = ImGui::GetWindowPos();
                if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
                sz = ImGui::GetWindowSize();
                ImGui::End();
            }
            ImGui::EndTabItem();
        }
        ImGuiTabItemFlags characters_flags = (forced_tab && strcmp(forced_tab, "Characters") == 0)
            ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem("Characters", nullptr, characters_flags)) { s_active_tab = 6;
            flags |= (1u << 1);
            if (!CharacterEditor::g_detached) {
                ImGui::Separator();
                ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                CharacterEditor::Draw(false);
            } else {
                ImVec2& pos = CharacterEditor::g_win_pos;
                ImVec2& sz  = CharacterEditor::g_win_size;
                const float min_y = toolbar_h + ImGui::GetFrameHeight() * 2 + 4.f;
                if (pos.y < min_y) pos.y = min_y;
                ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(sz,  ImGuiCond_Appearing);
                if (ImGui::Begin("Characters##float", &CharacterEditor::g_detached, FLOAT_FLAGS)) {
                    ImGui::Separator();
                    CharacterEditor::Draw(false);
                }
                pos = ImGui::GetWindowPos();
                if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
                sz = ImGui::GetWindowSize();
                ImGui::End();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Inspector")) { s_active_tab = 7;
            ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
            EditorReflectInspector::DrawContent(s_ecs_world);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) { s_active_tab = 8;
            if (!SettingsEditor::g_detached) {
                ImGui::Separator();
                ImGui::SetCursorPos({12, ImGui::GetCursorPosY() + 4});
                SettingsEditor::DrawContent(CFG_PATH, status_msg, status_timer);
            } else {
                ImVec2& pos = SettingsEditor::g_win_pos;
                ImVec2& sz  = SettingsEditor::g_win_size;
                const float min_y = toolbar_h + ImGui::GetFrameHeight() * 2 + 4.f;
                if (pos.y < min_y) pos.y = min_y;
                ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(sz,  ImGuiCond_Appearing);
                if (ImGui::Begin("Settings##float", &SettingsEditor::g_detached, FLOAT_FLAGS)) {
                    ImGui::Separator();
                    SettingsEditor::DrawContent(CFG_PATH, status_msg, status_timer);
                }
                pos = ImGui::GetWindowPos();
                if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
                sz = ImGui::GetWindowSize();
                ImGui::End();
            }
            ImGui::EndTabItem();
        }
        // Trailing "Detach" rendered directly in the tab bar's own row
        // (ImGuiTabItemFlags_Trailing sorts it to the end regardless of call
        // order) instead of a per-panel button inside the tab body. Only
        // shown for the currently active tab while it's docked. 3D World /
        // Inspector have no detach concept — skipped (nullptr).
        bool* active_det = nullptr;
        switch (s_active_tab) {
            case 0: active_det = &ItemEditor::g_detached;         break;
            case 1: active_det = &FactionEditor::g_detached;      break;
            case 2: active_det = &s_lay.map.detached;             break;
            case 3: active_det = &s_lay.world.detached;           break;
            case 5: active_det = &NpcArchetypeEditor::g_detached; break;
            case 6: active_det = &CharacterEditor::g_detached;    break;
            case 8: active_det = &SettingsEditor::g_detached;     break;
            default: break;
        }
        if (active_det && !*active_det) {
            if (ImGui::TabItemButton("Detach", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
                *active_det = true;
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    return flags;
}

// ── Render — RTT renders before ImGui present ─────────────────────────────────
// active_flags: bitmask from PREVIOUS frame's BuildUI call (1-frame lag OK)
void editor_panels_render(void* cmd_ptr, float dt, uint32_t active_flags) {
    auto* cmd = static_cast<md::GpuCommandBufferHandle>(cmd_ptr);
    if ((active_flags >> 0) & 1) WorldEditor3D_SDLGPU::RenderFrame(cmd, dt, (active_flags >> 0) & 1);
    if ((active_flags >> 1) & 1) CharPreviewSDLGPU::RenderFrame(cmd);
    if ((active_flags >> 3) & 1) MapViewPanel::Get().RenderFrame(cmd);
}

// ── DumpState — F9 bug capture ────────────────────────────────────────────────
void editor_panels_dump_state(void* file_ptr) {
    FILE* f = static_cast<FILE*>(file_ptr);
    if (!f) return;
    fprintf(f, "[EditorPanels]\n");
    fprintf(f, "  chars_detached=%d\n\n", CharacterEditor::g_detached ? 1 : 0);
#ifdef MD_SDL_GPU
    CharPreviewSDLGPU::DumpState(f);
#endif
}

// ── ReloadShaders — /reload-shaders console command ───────────────────────────
void editor_panels_reload_shaders() {
    CharPreviewSDLGPU::ReloadPipelines();
    MD_LOG(MD_LOG_INFO, "[EditorPanels] shader pipelines reloaded");
}

} // extern "C"
