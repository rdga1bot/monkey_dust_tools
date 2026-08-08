#include "editor_map_view.h"
#ifndef MD_SDL_GPU
#  include "glad.h"
#endif
#include "imgui.h"
#include <monkey_dust/flare/tile_map.h>
#include <cstdio>
#include <cmath>
#include <initializer_list>

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
        // Layer row: eye toggle + radio button per layer
        for (int i = 0; i < map_.layer_count; i++) {
            ImGui::PushID(i);
            // Eye toggle
            bool vis = layer_visible_[i];
            if (!vis) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
            if (ImGui::SmallButton(vis ? "O" : "-")) layer_visible_[i] = !layer_visible_[i];
            if (!vis) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", vis ? "Hide layer" : "Show layer");
            ImGui::SameLine();
            // Radio button — selects active paint layer
            bool active = (sel_layer_ == i);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.2f, 1.0f));
            if (ImGui::SmallButton(LayerName(i))) sel_layer_ = i;
            if (active) ImGui::PopStyleColor();
            ImGui::PopID();
            ImGui::SameLine();
        }
        ImGui::NewLine();

        // Brush size buttons
        ImGui::SameLine(0, 0);
        ImGui::TextDisabled("Brush:");
        ImGui::SameLine();
        for (int bs : {1, 3, 5}) {
            char lbl[4];
            snprintf(lbl, sizeof(lbl), "%d", bs);
            bool active = (brush_size_ == bs);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 1.0f));
            ImGui::PushID(bs);
            if (ImGui::Button(lbl, {24, 0})) brush_size_ = bs;
            ImGui::PopID();
            if (active) ImGui::PopStyleColor();
            ImGui::SameLine();
        }

        if (erase_mode_) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.1f, 1.0f));
            if (ImGui::Button("[Erase]")) erase_mode_ = false;
            ImGui::PopStyleColor();
        } else {
            if (ImGui::Button(" Erase ")) erase_mode_ = true;
        }
        ImGui::SameLine();
        if (erase_mode_)
            ImGui::TextColored({1.0f, 0.5f, 0.3f, 1.0f}, "erase mode  LMB=erase  Shift+LMB=fill");
        else
            ImGui::Text("tile %d   LMB=paint  Shift+LMB=fill  RMB/MMB=pan", sel_tile_id_);
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

    now_s_ += dt;
#ifndef MD_SDL_GPU
    if (loaded_ && rt_ok_) {
        int vp_save[4];
        glGetIntegerv(GL_VIEWPORT, vp_save);
        glBindFramebuffer(GL_FRAMEBUFFER, rt_fbo_);
        glViewport(0, 0, rt_w_, rt_h_);
        glClearColor(20/255.f, 20/255.f, 30/255.f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        md::flare::TileMap2DRenderer::Get().Render(
            map_, now_s_,
            origin_x_, origin_y_, scale_,
            vp_w, vp_h, LayerMask());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(vp_save[0], vp_save[1], vp_save[2], vp_save[3]);
    }
    ImGui::Image(
        (ImTextureID)(intptr_t)(rt_ok_ ? rt_tex_ : 0u),
        ImVec2((float)vp_w, (float)vp_h),
        ImVec2(0, 1), ImVec2(1, 0));
#else
    // SDL_GPU: tile map was already rendered to rt_color_ in RenderFrame().
    // Display the RTT; tile animation time is accumulated here.
    if (rt_ok_ && rt_color_)
        ImGui::GetWindowDrawList()->AddImage(
            (ImTextureID)rt_color_, img_pos, {img_pos.x + vp_w, img_pos.y + vp_h});
    else
        ImGui::GetWindowDrawList()->AddRectFilled(
            img_pos, {img_pos.x + vp_w, img_pos.y + vp_h}, IM_COL32(20,20,30,255));
    ImGui::Dummy(ImVec2((float)vp_w, (float)vp_h));
#endif

    bool hovered = ImGui::IsItemHovered();
    ImVec2 mouse_abs = ImGui::GetIO().MousePos;
    float  mx = mouse_abs.x - img_pos.x;
    float  my = mouse_abs.y - img_pos.y;

    // Overlay markers (drawn before interaction so minimap is on top)
    DrawSpawnMarkers(img_pos);
    bool mm_captured = DrawMinimap(img_pos, vp_w, vp_h);

    if (hovered && !mm_captured) {
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
        // Zoom: scroll anchored to cursor, 5 % per wheel unit
        float scroll = ImGui::GetIO().MouseWheel;
        if (scroll != 0.0f) {
            float os = scale_;
            float factor = powf(1.05f, scroll);
            scale_ = fmaxf(fminf(scale_ * factor, 8.0f), 0.01f);
            origin_x_ = mx - (mx - origin_x_) * (scale_ / os);
            origin_y_ = my - (my - origin_y_) * (scale_ / os);
        }
        if (palette_tab_ == 1) {
            SpawnInteract(mx, my);
        } else if (palette_tab_ == 2) {
            PropsInteract(mx, my);
        } else {
            // Paint / Flood fill: LMB (plain = brush, Shift = flood fill)
            bool shift = ImGui::GetIO().KeyShift;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && shift) {
                FloodFillAt(mx, my);
            } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !shift) {
                PaintAt(mx, my);
            }
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
        ImGui::TextDisabled("LMB=paint  Shift+LMB=fill  RMB/MMB=pan  Scroll=zoom");
}
