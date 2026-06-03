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
#include "settings_editor.h"
#include "editor_world_panel.h"
#include "editor_world_3d_sdlgpu.h"
#include "editor_hmap_2d.h"
#include "editor_char_preview_sdlgpu.h"
#include "character_editor.h"
#include "npc_archetype_editor.h"
#include "editor_map_view.h"
#include "editor_node_graph.h"
#include "editor_terrain_panel.h"
#include "editor_layout.h"
#include "bug_capture.h"
#include <cstdio>
#include <cstring>

static constexpr const char* CFG_PATH = "data/editor_config.json";

extern "C" {

// ── Init — called after dlopen ────────────────────────────────────────────────
// ctx:         ImGuiContext* from host (must share for ImGui calls to work)
// gpu:         SDL_GPUDevice* (for RTT init)
// window:      SDL_Window*
// overlay_top: Y offset when running under RenderDoc overlay
// layout_path: JSON file for panel positions (e.g. "data/editor_layout.json")
void editor_panels_init(void* ctx, void* /*gpu*/, void* /*window*/,
                        float overlay_top, const char* layout_path) {
    // Share host's ImGui context — CRITICAL, must be first.
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx));

    SettingsEditor::Load(CFG_PATH);
    ItemEditor::Load("data/items/items.json");
    FactionEditor::Load("data/factions/factions.json");
    NpcArchetypeEditor::Load("game/data/defs/npc_archetypes.json");
    WorldPanel::Init();

    WorldEditor3D_SDLGPU::Init(
        "game/data/textures/md_terrain.png", 29, 25);

    CharacterEditor::LoadJSON("game/data/chars/player.chardef");
    CharacterEditor::LoadMorphNames("game/data/chars/morph_names.txt");
    MapViewPanel::Get().Init();
    EditorCore::Get().Init();

    // Restore panel positions from previous session
    if (layout_path && layout_path[0]) {
        using P = EditorLayout::Panel;
        P pi, pf, pn, pc, ps;
        if (EditorLayout::Load(layout_path, pi, pf, pn, pc, ps)) {
            ItemEditor::g_detached         = pi.detached;
            ItemEditor::g_win_pos          = pi.pos;
            ItemEditor::g_win_size         = pi.size;
            FactionEditor::g_detached      = pf.detached;
            FactionEditor::g_win_pos       = pf.pos;
            FactionEditor::g_win_size      = pf.size;
            NpcArchetypeEditor::g_detached = pn.detached;
            NpcArchetypeEditor::g_win_pos  = pn.pos;
            NpcArchetypeEditor::g_win_size = pn.size;
            CharacterEditor::g_detached    = pc.detached;
            CharacterEditor::g_win_pos     = pc.pos;
            CharacterEditor::g_win_size    = pc.size;
            SettingsEditor::g_detached     = ps.detached;
            SettingsEditor::g_win_pos      = ps.pos;
            SettingsEditor::g_win_size     = ps.size;
        }
    }

    (void)overlay_top;
    MD_LOG(MD_LOG_INFO, "[EditorPanels] init complete");
}

// ── Shutdown — called before dlclose ─────────────────────────────────────────
void editor_panels_shutdown(const char* layout_path) {
    if (layout_path && layout_path[0]) {
        EditorLayout::Save(layout_path,
            {ItemEditor::g_detached,         ItemEditor::g_win_pos,         ItemEditor::g_win_size},
            {FactionEditor::g_detached,      FactionEditor::g_win_pos,      FactionEditor::g_win_size},
            {NpcArchetypeEditor::g_detached, NpcArchetypeEditor::g_win_pos, NpcArchetypeEditor::g_win_size},
            {CharacterEditor::g_detached,    CharacterEditor::g_win_pos,    CharacterEditor::g_win_size},
            {SettingsEditor::g_detached,     SettingsEditor::g_win_pos,     SettingsEditor::g_win_size});
    }
    EditorCore::Get().Shutdown();
    MD_LOG(MD_LOG_INFO, "[EditorPanels] shutdown complete");
}

// ── BuildUI — called inside ImGui frame ──────────────────────────────────────
// Returns active-viewport bitmask:
//   bit 0: 3D World, bit 1: CharPreview, bit 2: Heightmap, bit 3: Map View
uint32_t editor_panels_build_ui(float dt, float toolbar_h,
                                 char* status_msg, float* status_timer) {
    if (status_timer && *status_timer > 0.f) *status_timer -= dt;

    uint32_t flags = 0;

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
    ImGui::SetCursorPos({0, 0});
    ImGui::Separator();
    ImGui::SetCursorPosX(4);

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Items")) {
            ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
            if (ItemEditor::Draw("data/items/items.json") && status_msg)
                snprintf(status_msg, 64, "Items saved!");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Factions")) {
            ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
            if (FactionEditor::Draw("data/factions/factions.json") && status_msg)
                snprintf(status_msg, 64, "Factions saved!");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Map")) {
            flags |= (1u << 3);
            ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
            ImGuiIO& mio = ImGui::GetIO();
            if (mio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
                MapViewPanel::Get().Undo();
            if (mio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
                MapViewPanel::Get().Redo();
            MapViewPanel::Get().Draw(dt);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("World")) {
            ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
            WorldPanel::Draw(dt);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Terrain")) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float graph_w = avail.x * 0.70f;
            ImGui::BeginChild("##terrain_ng", {graph_w, avail.y}, false,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            EditorNodeGraphPanel::Get().DrawContent();
            ImGui::EndChild();
            ImGui::SameLine(0, 4);
            ImGui::BeginChild("##terrain_sculpt", {0, avail.y}, false);
            EditorTerrainPanel::Get().DrawContent(dt);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Heightmap")) {
            flags |= (1u << 2);
            HmapEditor2D::DrawPanel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("3D World")) {
            flags |= (1u << 0);
            ImVec2 avail = ImGui::GetContentRegionAvail();
            WorldEditor3D_SDLGPU::DrawImGui(avail.x, avail.y - 2, dt);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("NPCs")) {
            ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
            NpcArchetypeEditor::Draw();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Characters")) {
            flags |= (1u << 1);
            ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
            CharacterEditor::Draw(false);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            ImGui::SetCursorPos({12, ImGui::GetCursorPosY() + 6});
            SettingsEditor::Draw(CFG_PATH, status_msg, status_timer);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    return flags;
}

// ── Render — RTT renders before ImGui present ─────────────────────────────────
// active_flags: bitmask from PREVIOUS frame's BuildUI call (1-frame lag OK)
void editor_panels_render(void* cmd_ptr, float dt, uint32_t active_flags) {
    auto* cmd = static_cast<SDL_GPUCommandBuffer*>(cmd_ptr);
    if ((active_flags >> 2) & 1) HmapEditor2D::UploadTexture(cmd);
    WorldEditor3D_SDLGPU::RenderFrame(cmd, dt, (active_flags >> 0) & 1);
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

// ── UploadTerrainHeightmap — bridge from terrain panel ────────────────────────
void editor_panels_upload_hmap(const float* hmap, int W, int H,
                                float world_size_m, int chunk_x, int chunk_z) {
    WorldEditor3D_SDLGPU::UploadTerrainHeightmap(hmap, W, H, world_size_m,
                                                  chunk_x, chunk_z);
}

// ── ReloadShaders — /reload-shaders console command ───────────────────────────
void editor_panels_reload_shaders() {
    CharPreviewSDLGPU::ReloadPipelines();
    MD_LOG(MD_LOG_INFO, "[EditorPanels] shader pipelines reloaded");
}

} // extern "C"
