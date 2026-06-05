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
#include "editor_hmap_2d.h"
#include "editor_char_preview_sdlgpu.h"
#include "character_editor.h"
#include "npc_archetype_editor.h"
#include "editor_map_view.h"
#include "editor_node_graph.h"
#include "editor_layout.h"
#include "bug_capture.h"
#include <cstdio>
#include <cstring>

static constexpr const char* CFG_PATH = "data/editor_config.json";

// Persistent panel layout (all tabs).  Loaded on init, saved on shutdown.
static EditorLayout::Layout s_lay;

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

    ItemEditor::Load("data/items/items.json");
    FactionEditor::Load("data/factions/factions.json");
    NpcArchetypeEditor::Load("game/data/defs/npc_archetypes.json");
    WorldPanel::Init();

    WorldEditor3D_SDLGPU::Init("game/data/textures/md_terrain.png", 29, 25);
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
        // Heightmap: DrawPanel() manages its own floating window — sync its internal statics
        HmapEditor2D::s_detached = s_lay.heightmap.detached;
        HmapEditor2D::s_win_pos  = s_lay.heightmap.pos;
        HmapEditor2D::s_win_size = s_lay.heightmap.size;
        // map/world/terrain/world3d used directly from s_lay in BuildUI
    }
    (void)overlay_top;
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
        // Heightmap: read back from DrawPanel()'s own statics
        s_lay.heightmap = {HmapEditor2D::s_detached, HmapEditor2D::s_win_pos, HmapEditor2D::s_win_size};
        // map/world/terrain/world3d are updated live in BuildUI → already in s_lay
        EditorLayout::Save(layout_path, s_lay);
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
    ImGui::Separator();

    static constexpr ImGuiWindowFlags FLOAT_FLAGS =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Items")) {
            if (!ItemEditor::g_detached) {
                float bw = ImGui::CalcTextSize("Detach##items").x + ImGui::GetStyle().FramePadding.x * 2 + 2;
                ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - bw);
                if (ImGui::SmallButton("Detach##items")) {
                    ItemEditor::g_detached = true;
                    ItemEditor::g_win_pos  = {10.f, 110.f};
                    ItemEditor::g_win_size = {ImGui::GetIO().DisplaySize.x * 0.50f, 520.f};
                }
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
                    float bw = ImGui::CalcTextSize("Dock##items").x + ImGui::GetStyle().FramePadding.x * 2 + 2;
                    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - bw);
                    if (ImGui::SmallButton("Dock##items")) ItemEditor::g_detached = false;
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
        if (ImGui::BeginTabItem("Factions")) {
            if (!FactionEditor::g_detached) {
                float bw = ImGui::CalcTextSize("Detach##fac").x + ImGui::GetStyle().FramePadding.x * 2 + 2;
                ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - bw);
                if (ImGui::SmallButton("Detach##fac")) {
                    FactionEditor::g_detached = true;
                    FactionEditor::g_win_pos  = {10.f, 110.f};
                    FactionEditor::g_win_size = {ImGui::GetIO().DisplaySize.x * 0.50f, 500.f};
                }
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
                    float bw = ImGui::CalcTextSize("Dock##fac").x + ImGui::GetStyle().FramePadding.x * 2 + 2;
                    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - bw);
                    if (ImGui::SmallButton("Dock##fac")) FactionEditor::g_detached = false;
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
        if (ImGui::BeginTabItem("Map")) {
            flags |= (1u << 3);
            auto draw_map = [&]() {
                ImGuiIO& mio = ImGui::GetIO();
                if (mio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) MapViewPanel::Get().Undo();
                if (mio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) MapViewPanel::Get().Redo();
                MapViewPanel::Get().Draw(dt);
            };
            if (!s_lay.map.detached) {
                float bw = ImGui::CalcTextSize("Detach##map").x + ImGui::GetStyle().FramePadding.x * 2 + 2;
                ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - bw);
                if (ImGui::SmallButton("Detach##map")) {
                    s_lay.map.detached = true;
                    s_lay.map.pos  = {10.f, 110.f};
                    s_lay.map.size = {fio.DisplaySize.x * 0.70f, 600.f};
                }
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
                    float bw = ImGui::CalcTextSize("Dock##map").x + ImGui::GetStyle().FramePadding.x * 2 + 2;
                    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - bw);
                    if (ImGui::SmallButton("Dock##map")) s_lay.map.detached = false;
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
        if (ImGui::BeginTabItem("World")) {
            if (!s_lay.world.detached) {
                float bw = ImGui::CalcTextSize("Detach##world").x + ImGui::GetStyle().FramePadding.x * 2 + 2;
                ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - bw);
                if (ImGui::SmallButton("Detach##world")) {
                    s_lay.world.detached = true;
                    s_lay.world.pos  = {10.f, 110.f};
                    s_lay.world.size = {fio.DisplaySize.x * 0.60f, 600.f};
                }
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
                    float bw = ImGui::CalcTextSize("Dock##world").x + ImGui::GetStyle().FramePadding.x * 2 + 2;
                    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - bw);
                    if (ImGui::SmallButton("Dock##world")) s_lay.world.detached = false;
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
        if (ImGui::BeginTabItem("Heightmap")) {
            flags |= (1u << 2);
            HmapEditor2D::DrawPanel();  // handles own Detach/Dock + floating window internally
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("3D World")) {
            flags |= (1u << 0);
            ImVec2 avail = ImGui::GetContentRegionAvail();
            WorldEditor3D_SDLGPU::DrawImGui(avail.x, avail.y - 2, dt);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("NPCs")) {
            if (!NpcArchetypeEditor::g_detached) {
                float bw = ImGui::CalcTextSize("Detach##npcs").x + ImGui::GetStyle().FramePadding.x * 2 + 2;
                ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - bw);
                if (ImGui::SmallButton("Detach##npcs")) {
                    NpcArchetypeEditor::g_detached = true;
                    NpcArchetypeEditor::g_win_pos  = {10.f, 110.f};
                    NpcArchetypeEditor::g_win_size = {ImGui::GetIO().DisplaySize.x * 0.50f, 540.f};
                }
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
                    float bw = ImGui::CalcTextSize("Dock##npcs").x + ImGui::GetStyle().FramePadding.x * 2 + 2;
                    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - bw);
                    if (ImGui::SmallButton("Dock##npcs")) NpcArchetypeEditor::g_detached = false;
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
        if (ImGui::BeginTabItem("Characters")) {
            flags |= (1u << 1);
            if (!CharacterEditor::g_detached) {
                float bw = ImGui::CalcTextSize("Detach##chars").x + ImGui::GetStyle().FramePadding.x * 2 + 2;
                ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - bw);
                if (ImGui::SmallButton("Detach##chars")) {
                    CharacterEditor::g_detached = true;
                    CharacterEditor::g_win_pos  = {10.f, 110.f};
                    CharacterEditor::g_win_size = {fio.DisplaySize.x * 0.70f, 640.f};
                }
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
                    float bw = ImGui::CalcTextSize("Dock##chars").x + ImGui::GetStyle().FramePadding.x * 2 + 2;
                    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - bw);
                    if (ImGui::SmallButton("Dock##chars")) CharacterEditor::g_detached = false;
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
        ImGui::EndTabBar();
    }
    ImGui::End();

    return flags;
}

// ── Render — RTT renders before ImGui present ─────────────────────────────────
// active_flags: bitmask from PREVIOUS frame's BuildUI call (1-frame lag OK)
void editor_panels_render(void* cmd_ptr, float dt, uint32_t active_flags) {
    auto* cmd = static_cast<SDL_GPUCommandBuffer*>(cmd_ptr);
    if ((active_flags >> 0) & 1) WorldEditor3D_SDLGPU::RenderFrame(cmd, dt, (active_flags >> 0) & 1);
    if ((active_flags >> 2) & 1) HmapEditor2D::UploadTexture(cmd);
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
