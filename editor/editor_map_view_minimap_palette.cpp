#include "editor_map_view.h"
#include "imgui.h"
#include <monkey_dust/flare/tile_map.h>
#include <monkey_dust/render/md_texture.h>
#include <cmath>

// ── Minimap (M9.10) ──────────────────────────────────────────────────────────

bool MapViewPanel::DrawMinimap(ImVec2 img_pos, int vp_w, int vp_h) {
    if (!loaded_) return false;

    // Scale: fit map into at most 160×100 pixels
    const float MM_MAX_W = 160.0f, MM_MAX_H = 100.0f;
    float tile_px = fminf(MM_MAX_W / (float)map_.width, MM_MAX_H / (float)map_.height);
    if (tile_px < 0.5f) tile_px = 0.5f;
    float mm_w = map_.width  * tile_px;
    float mm_h = map_.height * tile_px;

    // Top-right corner of viewport
    float mm_x = img_pos.x + vp_w - mm_w - 4.0f;
    float mm_y = img_pos.y + 4.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Semi-transparent background + border
    dl->AddRectFilled({mm_x - 1, mm_y - 1}, {mm_x + mm_w + 1, mm_y + mm_h + 1},
                      IM_COL32(8, 10, 18, 210));
    dl->AddRect({mm_x - 1, mm_y - 1}, {mm_x + mm_w + 1, mm_y + mm_h + 1},
                IM_COL32(70, 75, 100, 220));

    using LT = md::flare::LayerType;

    // Draw tiles: sample topmost visible non-empty layer
    for (int r = 0; r < map_.height; r++) {
        for (int c = 0; c < map_.width; c++) {
            ImU32 col = IM_COL32(20, 22, 32, 255);
            for (int li = map_.layer_count - 1; li >= 0; li--) {
                if (!layer_visible_[li]) continue;
                int tid = map_.layers[li].tiles[r * md::flare::MAX_MAP_WIDTH + c];
                if (tid == 0) continue;
                switch (map_.layers[li].type) {
                    case LT::BACKGROUND: col = IM_COL32(35, 130, 45, 255); break;
                    case LT::FRINGE:     col = IM_COL32(18,  80, 22, 255); break;
                    case LT::OBJECT:     col = IM_COL32(90,  90, 90, 255); break;
                    case LT::COLLISION:  col = IM_COL32(160, 25, 25, 255); break;
                    default: break;
                }
                break;
            }
            float px = mm_x + c * tile_px;
            float py = mm_y + r * tile_px;
            float sz = (tile_px >= 2.0f) ? tile_px - 0.5f : tile_px;
            dl->AddRectFilled({px, py}, {px + sz, py + sz}, col);
        }
    }

    // Hero spawn marker
    dl->AddCircleFilled(
        {mm_x + map_.hero_x * tile_px, mm_y + map_.hero_y * tile_px},
        fmaxf(2.0f, tile_px), IM_COL32(50, 180, 255, 255));

    // Enemy spawn markers
    for (int i = 0; i < map_.spawn_count; i++) {
        dl->AddCircleFilled(
            {mm_x + map_.spawns[i].center_x * tile_px,
             mm_y + map_.spawns[i].center_y * tile_px},
            fmaxf(1.5f, tile_px * 0.7f), IM_COL32(255, 70, 50, 230));
    }

    // Viewport diamond: project 4 screen corners → tile space → minimap
    auto s2t = [&](float sx, float sy, float& tc, float& tr) {
        float lx = (sx - origin_x_) / scale_;
        float ly = (sy - origin_y_) / scale_;
        float cr = lx / 96.0f, cs = ly / 48.0f;
        tc = (cr + cs) * 0.5f;
        tr = (cs - cr) * 0.5f;
    };
    ImVec2 pts[4];
    float tc, tr;
    s2t(0.0f,   0.0f,   tc, tr); pts[0] = {mm_x + tc*tile_px, mm_y + tr*tile_px};
    s2t((float)vp_w, 0.0f,   tc, tr); pts[1] = {mm_x + tc*tile_px, mm_y + tr*tile_px};
    s2t((float)vp_w, (float)vp_h, tc, tr); pts[2] = {mm_x + tc*tile_px, mm_y + tr*tile_px};
    s2t(0.0f,   (float)vp_h, tc, tr); pts[3] = {mm_x + tc*tile_px, mm_y + tr*tile_px};
    dl->AddQuad(pts[0], pts[1], pts[2], pts[3], IM_COL32(255, 255, 255, 200), 1.0f);

    // Click minimap → pan viewport so clicked tile is centered
    ImVec2 mp = ImGui::GetIO().MousePos;
    bool over = (mp.x >= mm_x && mp.x <= mm_x + mm_w &&
                 mp.y >= mm_y && mp.y <= mm_y + mm_h);
    if (over && ImGui::IsWindowHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float clicked_c = (mp.x - mm_x) / tile_px;
        float clicked_r = (mp.y - mm_y) / tile_px;
        origin_x_ = vp_w * 0.5f - (clicked_c - clicked_r) * 96.0f * scale_;
        origin_y_ = vp_h * 0.5f - (clicked_c + clicked_r) * 48.0f * scale_;
    }
    return over;
}

// ── Palette panel ─────────────────────────────────────────────────────────────

void MapViewPanel::DrawPalette() {
    if (!loaded_) {
        ImGui::TextDisabled("Load a map");
        ImGui::TextDisabled("to see tiles");
        return;
    }

    // Palette tab bar: Tiles | Spawns | Props
    auto tab_btn = [&](const char* label, int idx, ImVec4 color) {
        if (palette_tab_ == idx) ImGui::PushStyleColor(ImGuiCol_Button, color);
        if (ImGui::SmallButton(label)) palette_tab_ = idx;
        if (palette_tab_ == idx) ImGui::PopStyleColor();
        ImGui::SameLine();
    };
    tab_btn("Tiles",  0, ImVec4(0.20f, 0.50f, 0.80f, 1.f));
    tab_btn("Spawns", 1, ImVec4(0.70f, 0.40f, 0.10f, 1.f));
    tab_btn("Props",  2, ImVec4(0.25f, 0.55f, 0.45f, 1.f));
    ImGui::NewLine();
    ImGui::Separator();

    if (palette_tab_ == 1) { DrawSpawnPanel(); return; }
    if (palette_tab_ == 2) { DrawPropsPanel(); return; }

    auto& r2d = md::flare::TileMap2DRenderer::Get();
    const auto& meta = map_.meta;
    ImGui::TextDisabled("%d tiles", meta.count);
    ImGui::Separator();

    // Fit each tile into a 48×56 cell preserving aspect ratio.
    // Ground tiles (64×32): tw=48, th=24.  Billboards (64×128): tw=28, th=56.
    const float THUMB_W = 48.0f, THUMB_H = 56.0f;

    for (int i = 0; i < meta.count; i++) {
        const auto& m = meta.entries[i];
        MdTexture atlas = r2d.GetAtlas(m.atlas_idx);
#ifndef MD_SDL_GPU
        if (!atlas.id || atlas.w <= 0 || atlas.h <= 0) continue;
        ImTextureID atlas_imgui = (ImTextureID)(intptr_t)atlas.id;
#else
        if (!atlas.sdl_tex || atlas.w <= 0 || atlas.h <= 0) continue;
        ImTextureID atlas_imgui = (ImTextureID)atlas.sdl_tex;
#endif

        float scale = fminf(THUMB_W / (float)m.w, THUMB_H / (float)m.h);
        float tw    = (float)m.w * scale;
        float th    = (float)m.h * scale;

        // UV: stbi flip active (both GL and SDL_GPU path) → v = 1 - y_file/H
        float u0 = (float)m.src_x / (float)atlas.w;
        float v0 = 1.0f - (float)m.src_y / (float)atlas.h;
        float u1 = (float)(m.src_x + m.w) / (float)atlas.w;
        float v1 = 1.0f - (float)(m.src_y + m.h) / (float)atlas.h;

        bool sel = !erase_mode_ && (m.tile_id == sel_tile_id_);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.2f, 1.0f));

        ImGui::PushID(i);
        if (ImGui::ImageButton("##t",
                               atlas_imgui,
                               ImVec2(tw, th),
                               ImVec2(u0, v0), ImVec2(u1, v1))) {
            sel_tile_id_ = m.tile_id;
            erase_mode_  = false;
        }
        ImGui::PopID();

        if (sel) ImGui::PopStyleColor();

        if (ImGui::IsItemHovered()) {
            // Classify: billboard (tall sprite) → Fringe/Object; flat → Background
            const char* layer_hint = (m.offset_y > m.h / 2)
                                   ? "→ Fringe / Object layer"
                                   : "→ Background layer";
            ImGui::SetTooltip("Tile %d\n%dx%d  atlas[%d]\n%s",
                              m.tile_id, m.w, m.h, m.atlas_idx, layer_hint);
        }

        ImGui::SameLine();
        if (sel) ImGui::TextColored({0.3f, 0.9f, 0.4f, 1.0f}, "T%d", m.tile_id);
        else     ImGui::TextDisabled("T%d", m.tile_id);
    }
}
