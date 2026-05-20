#ifdef MONKEY_DUST_EDITOR
#include "editor_terrain_panel.h"
#include "editor_core.h"
#include "imgui.h"
#include <monkey_dust/world/terrain_gen.h>
#include <SDL3/SDL.h>
#include <cstdio>

// Globals owned by main.cpp
extern bool  g_terrain_edit;
extern float g_brush_r;
extern float g_brush_str;

void EditorTerrainPanel::Draw(float dt) {
    if (!EditorCore::Get().panels_visible[14]) return;

    ImGui::SetNextWindowSize(ImVec2(300, 360), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(570, 60),   ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Terrain Sculpt##TerrainPanel",
                      &EditorCore::Get().panels_visible[14])) {
        ImGui::End();
        return;
    }

    // ── Activate / Deactivate ─────────────────────────────────────────────
    if (g_terrain_edit) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.f, 0.4f, 1.f));
        ImGui::TextUnformatted("● SCULPT ACTIVE");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Deactivate")) {
            g_terrain_edit = false;
            SDL_ShowCursor();
        }
    } else {
        if (!TerrainAtlas_Loaded()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.2f, 1.f));
            ImGui::TextUnformatted("! Height atlas not loaded");
            ImGui::PopStyleColor();
        } else {
            if (ImGui::Button("Activate Sculpt", ImVec2(-1, 0))) {
                g_terrain_edit = true;
                SDL_HideCursor();
            }
        }
    }

    ImGui::Separator();

    // ── Brush Settings ────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Brush", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Radius (m)##btr",  &g_brush_r,   5.f, 200.f, "%.0f m");
        ImGui::SliderFloat("Strength##bts",    &g_brush_str, 0.1f, 15.f,  "%.1f");

        if (ImGui::Button(" - ##rbr"))  g_brush_r   = fmaxf(5.f,   g_brush_r   - 5.f);
        ImGui::SameLine();
        if (ImGui::Button(" + ##rbr"))  g_brush_r   = fminf(200.f, g_brush_r   + 5.f);
        ImGui::SameLine(); ImGui::TextDisabled("radius");

        if (ImGui::Button(" - ##sbs"))  g_brush_str = fmaxf(0.1f,  g_brush_str - 0.5f);
        ImGui::SameLine();
        if (ImGui::Button(" + ##sbs"))  g_brush_str = fminf(15.f,  g_brush_str + 0.5f);
        ImGui::SameLine(); ImGui::TextDisabled("strength");
    }

    // ── Controls Reference ────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("LMB             Raise");
        ImGui::TextDisabled("RMB             Lower");
        ImGui::TextDisabled("Shift + LMB     Smooth");
        ImGui::TextDisabled("Scroll wheel    Radius +/-");
        ImGui::TextDisabled("S               Save height map");
    }

    ImGui::Separator();

    // ── Save ──────────────────────────────────────────────────────────────
    bool can_save = TerrainAtlas_Loaded();
    if (!can_save) ImGui::BeginDisabled();
    if (ImGui::Button("Save Height Map", ImVec2(-1, 0))) {
        if (TerrainAtlas_Save("game/data/terrain/world_hmap.r32")) {
            snprintf(save_status_, sizeof(save_status_), "Saved OK");
        } else {
            snprintf(save_status_, sizeof(save_status_), "Save FAILED");
        }
        save_status_timer_ = 3.f;
    }
    if (!can_save) ImGui::EndDisabled();

    if (save_status_timer_ > 0.f) {
        save_status_timer_ -= dt;
        bool ok = save_status_[5] == 'O';
        ImGui::PushStyleColor(ImGuiCol_Text,
            ok ? ImVec4(0.2f,1.f,0.4f,1.f) : ImVec4(1.f,0.3f,0.2f,1.f));
        ImGui::TextUnformatted(save_status_);
        ImGui::PopStyleColor();
    }

    ImGui::End();
}
#endif
