#ifdef MONKEY_DUST_EDITOR
#include "editor_terrain_panel.h"
#include "editor_node_graph.h"
#include "editor_3d_bridge.h"
#include "editor_core.h"
#include "imgui.h"
#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/nodegraph/terrain_tile_gen.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/world/world_transform.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include "../../game/src/render/scene_render.h"

// ── Scatter ECS tag (P-NG-6) ─────────────────────────────────────────────────
struct PcgScatterTag {
    uint16_t prefab_id;
    float    scale;
    float    rot_y;
};

static entt::entity s_scatter_ents[md::PCG_MAX_SCATTER];
static int          s_scatter_count = 0;

void EditorTerrainPanel::Draw(float dt) {
    if (!EditorCore::Get().panels_visible[14]) return;
    ImGui::SetNextWindowSize(ImVec2(300, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(570, 60),   ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Terrain Sculpt##TerrainPanel",
                      &EditorCore::Get().panels_visible[14])) {
        ImGui::End();
        return;
    }
    DrawContent(dt);
    ImGui::End();
}

void EditorTerrainPanel::DrawContent(float dt) {
    auto& sr = SceneRender::Get();

    // ── PCG Generate ──────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("PCG Generate", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& tg = md::TerrainTileGen::Get();
        auto& ng = EditorNodeGraphPanel::Get();
        bool building = tg.IsBuilding();

        // Progress bar / status
        if (building) {
            ImGui::ProgressBar(-1.f * (float)ImGui::GetTime(), ImVec2(-1, 0), "Building...");
        } else {
            const md::TileData& ready = tg.GetReady();
            if (ready.applied) {
                ImGui::TextColored(ImVec4(0.3f,0.9f,0.3f,1.f),
                    "OK v%u (%dx%d)", ready.graph_version, md::TILE_RES, md::TILE_RES);
            } else {
                ImGui::TextDisabled("Not built yet");
            }
        }

        // Preview resolution selector
        static const char* kResLabels[3] = { "64", "128", "256" };
        ImGui::TextDisabled("Preview res:");
        ImGui::SameLine();
        for (int i = 0; i < 3; ++i) {
            if (i > 0) ImGui::SameLine();
            bool sel = (preview_res_idx_ == i);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f,0.6f,0.9f,1.f));
            char lbl[16]; snprintf(lbl, sizeof(lbl), "%s##pres%d", kResLabels[i], i);
            if (ImGui::SmallButton(lbl)) preview_res_idx_ = i;
            if (sel) ImGui::PopStyleColor();
        }

        // Rebuild button
        if (building) ImGui::BeginDisabled();
        if (ImGui::Button("Rebuild##pcgbuild", ImVec2(-1, 0))) {
            tg.RequestRebuild(ng.GetPcgGraph(), 0, 0, 500.f);
            rebuild_debounce_s_ = 0.f;
        }
        if (building) ImGui::EndDisabled();

        // Auto-rebuild checkbox + debounce
        ImGui::Checkbox("Auto-rebuild (400ms)##pcgar", &auto_rebuild_);
        if (auto_rebuild_ && ng.GetPcgGraph().needs_rebuild && !building) {
            rebuild_debounce_s_ += dt;
            if (rebuild_debounce_s_ >= 0.4f) {
                tg.RequestRebuild(ng.GetPcgGraph(), 0, 0, 500.f);
                rebuild_debounce_s_ = 0.f;
            }
        } else if (!ng.GetPcgGraph().needs_rebuild) {
            rebuild_debounce_s_ = 0.f;
        }

        // Poll for completed tile — upload to 3D viewport + sync scatter ECS
        if (tg.PollApply()) {
            const md::TileData& tile = tg.GetReady();
            EditorW3D_UploadTerrainHeightmap(
                tile.heightmap, md::TILE_RES, md::TILE_RES,
                tile.world_size, tile.chunk_x, tile.chunk_z);

            // P-NG-6.1-6.2: purge old scatter entities, create new ones
            entt::registry& reg = Registry::Get();
            for (int i = 0; i < s_scatter_count; ++i)
                if (reg.valid(s_scatter_ents[i]))
                    reg.destroy(s_scatter_ents[i]);
            s_scatter_count = 0;

            int n = tile.scatter_count;
            if (n > md::PCG_MAX_SCATTER) n = md::PCG_MAX_SCATTER;
            for (int i = 0; i < n; ++i) {
                const md::ScatterPoint& sp = tile.scatter[i];
                entt::entity e = reg.create();
                reg.emplace<WorldTransform>(e, WorldTransform{sp.x, 0.f, sp.z, sp.rot_y, 0xFFFFFFFFu});
                reg.emplace<PcgScatterTag>(e, PcgScatterTag{sp.prefab_id, sp.scale, sp.rot_y});
                s_scatter_ents[s_scatter_count++] = e;
            }
        }

        // Apply to World button (writes PCG heightmap into master + refreshes chunks)
        const md::TileData& ready = tg.GetReady();
        if (!ready.applied) ImGui::BeginDisabled();
        if (ImGui::Button("Apply to World##pcgapply", ImVec2(-1, 0))) {
            EditorW3D_UploadTerrainHeightmap(
                ready.heightmap, md::TILE_RES, md::TILE_RES,
                ready.world_size, ready.chunk_x, ready.chunk_z);
        }
        if (!ready.applied) ImGui::EndDisabled();
    }

    ImGui::Separator();

    // ── Activate / Deactivate sculpt ──────────────────────────────────────────
    if (sr.terrain_edit) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.f, 0.4f, 1.f));
        ImGui::TextUnformatted("● SCULPT ACTIVE");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Deactivate")) {
            sr.terrain_edit = false;
            SDL_ShowCursor();
        }
    } else {
        if (!TerrainAtlas_Loaded()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.2f, 1.f));
            ImGui::TextUnformatted("! Height atlas not loaded");
            ImGui::PopStyleColor();
        } else {
            if (ImGui::Button("Activate Sculpt", ImVec2(-1, 0))) {
                sr.terrain_edit = true;
                SDL_HideCursor();
            }
        }
    }

    ImGui::Separator();

    // ── Brush Settings ────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Brush", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Radius (m)##btr",  &sr.brush_r,   5.f, 200.f, "%.0f m");
        ImGui::SliderFloat("Strength##bts",    &sr.brush_str, 0.1f, 15.f,  "%.1f");

        if (ImGui::Button(" - ##rbr"))  sr.brush_r   = fmaxf(5.f,   sr.brush_r   - 5.f);
        ImGui::SameLine();
        if (ImGui::Button(" + ##rbr"))  sr.brush_r   = fminf(200.f, sr.brush_r   + 5.f);
        ImGui::SameLine(); ImGui::TextDisabled("radius");

        if (ImGui::Button(" - ##sbs"))  sr.brush_str = fmaxf(0.1f,  sr.brush_str - 0.5f);
        ImGui::SameLine();
        if (ImGui::Button(" + ##sbs"))  sr.brush_str = fminf(15.f,  sr.brush_str + 0.5f);
        ImGui::SameLine(); ImGui::TextDisabled("strength");
    }

    // ── Controls Reference ────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("LMB             Raise");
        ImGui::TextDisabled("RMB             Lower");
        ImGui::TextDisabled("Shift + LMB     Smooth");
        ImGui::TextDisabled("Scroll wheel    Radius +/-");
        ImGui::TextDisabled("S               Save height map");
    }

    ImGui::Separator();

    // ── Save ──────────────────────────────────────────────────────────────────
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
}
#endif
