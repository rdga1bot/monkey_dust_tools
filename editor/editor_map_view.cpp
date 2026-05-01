#include "editor_map_view.h"
#include "rlgl.h"
#include "imgui.h"
#include <monkey_dust/flare/tile_map.h>
#include <monkey_dust/render/md_texture.h>
#include <cstdio>
#include <cstring>
#include <cmath>

// ── FBO management ────────────────────────────────────────────────────────────

void MapViewPanel::EnsureRT(int w, int h) {
    if (rt_ok_ && rt_w_ == w && rt_h_ == h) return;
    if (rt_ok_) UnloadRenderTexture(rt_);
    rt_   = LoadRenderTexture(w, h);
    rt_ok_= rt_.id > 0;
    rt_w_ = w;
    rt_h_ = h;
}

// ── Init / Shutdown ───────────────────────────────────────────────────────────

void MapViewPanel::Init() {
    if (init_) return;
    md::flare::TileMap2DRenderer::Get().Init();
    LoadMap(path_buf_);
    init_ = true;
}

void MapViewPanel::Shutdown() {
    if (!init_) return;
    if (rt_ok_) { UnloadRenderTexture(rt_); rt_ok_ = false; }
    md::flare::TileMap2DRenderer::Get().Shutdown();
    init_ = false;
}

// ── Map loading ───────────────────────────────────────────────────────────────

// ── Undo / Redo ───────────────────────────────────────────────────────────────

void MapViewPanel::ClearHistory() {
    undo_top_ = 0;
    redo_top_ = 0;
}

void MapViewPanel::PushUndo(const PaintOp& op) {
    if (undo_top_ == UNDO_MAX) {
        for (int i = 0; i < UNDO_MAX - 1; i++) undo_stack_[i] = undo_stack_[i + 1];
        undo_top_ = UNDO_MAX - 1;
    }
    undo_stack_[undo_top_++] = op;
    redo_top_ = 0;
}

void MapViewPanel::Undo() {
    if (undo_top_ == 0 || !loaded_) return;
    PaintOp op = undo_stack_[--undo_top_];
    map_.layers[op.layer].tiles[op.row * md::flare::MAX_MAP_WIDTH + op.col] = op.old_val;
    if (redo_top_ < UNDO_MAX) redo_stack_[redo_top_++] = op;
}

void MapViewPanel::Redo() {
    if (redo_top_ == 0 || !loaded_) return;
    PaintOp op = redo_stack_[--redo_top_];
    map_.layers[op.layer].tiles[op.row * md::flare::MAX_MAP_WIDTH + op.col] = op.new_val;
    if (undo_top_ < UNDO_MAX) undo_stack_[undo_top_++] = op;
}

bool MapViewPanel::NewMap(int width, int height, const char* tilesetdef) {
    // Use currently loaded map path as base for mod/tilesetdef resolution.
    // Fall back to the default goblin_camp path when no map is loaded yet.
    const char* base = (path_buf_[0])
        ? path_buf_
        : "third_party/flare-game/mods/empyrean_campaign/maps/goblin_camp.txt";

    md::flare::FlareMap tmp = {};
    md::flare::InitEmptyFlareMap(base, width, height, tilesetdef, tmp);

    map_    = tmp;
    loaded_ = true;

    sel_layer_ = 0;
    for (int i = 0; i < tmp.layer_count; i++) {
        if (tmp.layers[i].type == md::flare::LayerType::BACKGROUND) { sel_layer_ = i; break; }
    }
    snprintf(map_label_, sizeof(map_label_), "new_map");
    sel_tile_id_ = (tmp.meta.count > 0) ? tmp.meta.entries[0].tile_id : 1;
    erase_mode_  = false;

    path_buf_[0] = '\0';   // no file path yet — user must Save As
    save_buf_[0] = '\0';
    ClearHistory();

    md::flare::TileMap2DRenderer::Get().SetAtlases(map_);
    need_reset_ = true;
    return true;
}

bool MapViewPanel::SaveCurrent() {
    if (!loaded_ || !save_buf_[0]) return false;
    return md::flare::SaveFlareMap(save_buf_, map_);
}

bool MapViewPanel::SaveTo(const char* path) {
    if (!loaded_ || !path || !path[0]) return false;
    bool ok = md::flare::SaveFlareMap(path, map_);
    if (ok) snprintf(save_buf_, sizeof(save_buf_), "%s", path);
    return ok;
}

bool MapViewPanel::LoadMap(const char* map_txt_path) {
    md::flare::FlareMap tmp = {};
    if (!md::flare::LoadFlareMap(map_txt_path, tmp)) {
        fprintf(stderr, "[MapView] LoadMap failed: %s\n", map_txt_path);
        return false;
    }
    snprintf(path_buf_, sizeof(path_buf_), "%s", map_txt_path);
    map_    = tmp;
    loaded_ = true;
    // Default to the first Background layer so painting doesn't accidentally
    // overwrite Fringe (trees/rocks) or Object tiles.
    sel_layer_ = 0;
    for (int i = 0; i < tmp.layer_count; i++) {
        if (tmp.layers[i].type == md::flare::LayerType::BACKGROUND) {
            sel_layer_ = i;
            break;
        }
    }

    const char* slash = strrchr(map_txt_path, '/');
    const char* name  = slash ? slash + 1 : map_txt_path;
    snprintf(map_label_, sizeof(map_label_), "%s", name);
    char* dot = strrchr(map_label_, '.');
    if (dot) *dot = '\0';

    md::flare::TileMap2DRenderer::Get().SetAtlases(map_);
    if (map_.meta.count > 0)
        sel_tile_id_ = map_.meta.entries[0].tile_id;
    // Default save path = load path (user can edit before saving).
    snprintf(save_buf_, sizeof(save_buf_), "%s", map_txt_path);
    ClearHistory();
    need_reset_ = true;
    return true;
}

// ── View reset ────────────────────────────────────────────────────────────────

void MapViewPanel::ResetView(int vp_w, int vp_h) {
    if (!loaded_) return;
    float map_scr_w = (float)(map_.width  + map_.height) * 96.0f;
    float map_scr_h = (float)(map_.width  + map_.height) * 48.0f;
    scale_ = fminf((float)vp_w / map_scr_w, (float)vp_h / map_scr_h) * 0.82f;
    float cx = (float)(map_.width  - map_.height) * 48.0f;
    float cy = (float)(map_.width  + map_.height) * 24.0f;
    origin_x_ = (float)vp_w * 0.5f - cx * scale_;
    origin_y_ = (float)vp_h * 0.5f - cy * scale_;
}

// ── Layer name helper ─────────────────────────────────────────────────────────

const char* MapViewPanel::LayerName(int idx) const {
    if (idx < 0 || idx >= map_.layer_count) return "?";
    switch (map_.layers[idx].type) {
        case md::flare::LayerType::BACKGROUND: return "Background";
        case md::flare::LayerType::FRINGE:     return "Fringe";
        case md::flare::LayerType::OBJECT:     return "Object";
        case md::flare::LayerType::COLLISION:  return "Collision";
        default:                               return "Unknown";
    }
}

// ── Paint at mouse position ───────────────────────────────────────────────────

bool MapViewPanel::PaintAt(float mx, float my) {
    if (!loaded_) return false;
    float sx = (mx - origin_x_) / scale_;
    float sy = (my - origin_y_) / scale_;
    float cr = sx / 96.0f;
    float cs = sy / 48.0f;
    int col = (int)roundf((cr + cs) * 0.5f);
    int row = (int)roundf((cs - cr) * 0.5f);
    if (col < 0 || col >= map_.width || row < 0 || row >= map_.height) return false;
    if (sel_layer_ < 0 || sel_layer_ >= map_.layer_count) return false;

    uint16_t new_val = erase_mode_ ? 0 : sel_tile_id_;
    // Guard: don't paint with an ID that isn't in the registry (would render invisible).
    if (new_val != 0 && !map_.meta.Find(new_val)) return false;
    uint16_t& cell = map_.layers[sel_layer_].tiles[
        row * md::flare::MAX_MAP_WIDTH + col];
    if (cell == new_val) return false;
    PushUndo({sel_layer_, row, col, cell, new_val});
    cell = new_val;
    return true;
}

// ── Palette panel ─────────────────────────────────────────────────────────────

void MapViewPanel::DrawPalette() {
    if (!loaded_) {
        ImGui::TextDisabled("Load a map");
        ImGui::TextDisabled("to see tiles");
        return;
    }

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
        if (!atlas.id || atlas.w <= 0 || atlas.h <= 0) continue;

        float scale = fminf(THUMB_W / (float)m.w, THUMB_H / (float)m.h);
        float tw    = (float)m.w * scale;
        float th    = (float)m.h * scale;

        // UV: stbi flip active → v_gl = 1 - y_file/H
        float u0 = (float)m.src_x / (float)atlas.w;
        float v0 = 1.0f - (float)m.src_y / (float)atlas.h;
        float u1 = (float)(m.src_x + m.w) / (float)atlas.w;
        float v1 = 1.0f - (float)(m.src_y + m.h) / (float)atlas.h;

        bool sel = !erase_mode_ && (m.tile_id == sel_tile_id_);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.2f, 1.0f));

        ImGui::PushID(i);
        if (ImGui::ImageButton("##t",
                               (ImTextureID)(intptr_t)atlas.id,
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

// ── Draw ──────────────────────────────────────────────────────────────────────

void MapViewPanel::Draw(float dt) {
    (void)dt;

    // ── Toolbar: map label + Reset + Layer + Erase ────────────────────────────
    if (loaded_)
        ImGui::TextColored({0.5f, 1.0f, 0.6f, 1.0f}, "%s  (%dx%d)",
                           map_label_, map_.width, map_.height);
    else
        ImGui::TextDisabled("no map — use File > Open Map");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 60);
    if (ImGui::Button("Reset##view")) need_reset_ = true;

    if (loaded_) {
        ImGui::SetNextItemWidth(110);
        if (ImGui::BeginCombo("Layer##sel", LayerName(sel_layer_))) {
            for (int i = 0; i < map_.layer_count; i++) {
                bool is_sel = (i == sel_layer_);
                if (ImGui::Selectable(LayerName(i), is_sel)) sel_layer_ = i;
                if (is_sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (erase_mode_) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.1f, 1.0f));
            if (ImGui::Button("[Erase]")) erase_mode_ = false;
            ImGui::PopStyleColor();
        } else {
            if (ImGui::Button(" Erase ")) erase_mode_ = true;
        }
        ImGui::SameLine();
        if (erase_mode_)
            ImGui::TextColored({1.0f, 0.5f, 0.3f, 1.0f}, "erase mode  LMB=erase");
        else
            ImGui::Text("tile %d   LMB=paint   RMB/MMB=pan", sel_tile_id_);
    }
    ImGui::Separator();

    // ── Content split: palette (left) + map viewport (right) ─────────────────
    ImVec2 avail   = ImGui::GetContentRegionAvail();
    int content_h  = (int)avail.y - 22;
    if (content_h < 64) content_h = 64;

    int vp_w = (int)(avail.x - PALETTE_W - 8);
    int vp_h = content_h;
    if (vp_w < 64) vp_w = 64;

    // Left: palette
    ImGui::BeginChild("##pal_panel", ImVec2(PALETTE_W, (float)content_h), true);
    DrawPalette();
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: map viewport
    ImGui::BeginChild("##map_vp", ImVec2((float)vp_w, (float)vp_h), false);
    ImVec2 img_pos = ImGui::GetCursorScreenPos();

    EnsureRT(vp_w, vp_h);
    if (need_reset_) { ResetView(vp_w, vp_h); need_reset_ = false; }

    if (loaded_ && rt_ok_) {
        BeginTextureMode(rt_);
        ClearBackground({20, 20, 30, 255});
        md::flare::TileMap2DRenderer::Get().Render(
            map_, (float)GetTime(),
            origin_x_, origin_y_, scale_,
            vp_w, vp_h);
        rlActiveTextureSlot(0);
        rlEnableTexture(0);
        EndTextureMode();
    }

    ImGui::Image(
        (ImTextureID)(intptr_t)(rt_ok_ ? rt_.texture.id : 0),
        ImVec2((float)vp_w, (float)vp_h),
        ImVec2(0, 1), ImVec2(1, 0));

    bool hovered = ImGui::IsItemHovered();
    ImVec2 mouse_abs = ImGui::GetIO().MousePos;
    float  mx = mouse_abs.x - img_pos.x;
    float  my = mouse_abs.y - img_pos.y;

    if (hovered) {
        // Pan: right drag (or middle drag)
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
            ImVec2 d = ImGui::GetIO().MouseDelta;
            origin_x_ += d.x;
            origin_y_ += d.y;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
            ImVec2 d = ImGui::GetIO().MouseDelta;
            origin_x_ += d.x;
            origin_y_ += d.y;
        }
        // Zoom: scroll, anchored to cursor
        float scroll = ImGui::GetIO().MouseWheel;
        if (scroll != 0.0f) {
            float os = scale_;
            scale_ = (scroll > 0) ? fminf(scale_ * 1.15f, 8.0f)
                                   : fmaxf(scale_ * 0.87f, 0.01f);
            origin_x_ = mx - (mx - origin_x_) * (scale_ / os);
            origin_y_ = my - (my - origin_y_) * (scale_ / os);
        }
        // Paint: left click/drag
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            PaintAt(mx, my);
        }
    }

    ImGui::EndChild();

    // ── Status bar ────────────────────────────────────────────────────────────
    int tile_col = -1, tile_row = -1;
    if (loaded_) {
        float sx = (mx - origin_x_) / scale_;
        float sy = (my - origin_y_) / scale_;
        float cr = sx / 96.0f;
        float cs = sy / 48.0f;
        tile_col = (int)roundf((cr + cs) * 0.5f);
        tile_row = (int)roundf((cs - cr) * 0.5f);
    }
    if (hovered && tile_col >= 0 && tile_row >= 0 &&
        tile_col < map_.width && tile_row < map_.height)
        ImGui::Text("Tile (%d, %d)   Scale %.2f   Layer: %s",
                    tile_col, tile_row, scale_, LayerName(sel_layer_));
    else
        ImGui::TextDisabled("LMB=paint   RMB/MMB=pan   Scroll=zoom");
}
