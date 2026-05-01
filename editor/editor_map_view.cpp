#include "editor_map_view.h"
#include "rlgl.h"
#include <monkey_dust/flare/tile_map.h>
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
    // Load default map immediately so the tab isn't blank on first open.
    LoadMap(path_buf_, mods_buf_);
    init_ = true;
}

void MapViewPanel::Shutdown() {
    if (!init_) return;
    if (rt_ok_) { UnloadRenderTexture(rt_); rt_ok_ = false; }
    md::flare::TileMap2DRenderer::Get().Shutdown();
    init_ = false;
}

// ── Map loading ───────────────────────────────────────────────────────────────

bool MapViewPanel::LoadMap(const char* map_txt_path, const char* /*mods_root*/) {
    md::flare::FlareMap tmp = {};
    if (!md::flare::LoadFlareMap(map_txt_path, tmp)) {
        fprintf(stderr, "[MapView] failed to load: %s\n", map_txt_path);
        return false;
    }
    map_    = tmp;
    loaded_ = true;

    // Extract label from path for status display.
    const char* slash = strrchr(map_txt_path, '/');
    const char* name  = slash ? slash + 1 : map_txt_path;
    snprintf(map_label_, sizeof(map_label_), "%s", name);
    // Strip ".txt"
    char* dot = strrchr(map_label_, '.');
    if (dot) *dot = '\0';

    md::flare::TileMap2DRenderer::Get().SetAtlases(map_);
    return true;
}

// ── View reset ────────────────────────────────────────────────────────────────

void MapViewPanel::ResetView(int vp_w, int vp_h) {
    if (!loaded_) return;
    float map_scr_w = (float)(map_.width  + map_.height) * 96.0f;
    float map_scr_h = (float)(map_.width  + map_.height) * 48.0f;
    scale_ = fminf((float)vp_w / map_scr_w, (float)vp_h / map_scr_h) * 0.82f;
    // Center: for square map, tile(0,0) anchor is at screen (mh*96, 0).
    float cx = (float)(map_.width  - map_.height) * 48.0f;
    float cy = (float)(map_.width  + map_.height) * 24.0f;
    origin_x_ = (float)vp_w * 0.5f - cx * scale_;
    origin_y_ = (float)vp_h * 0.5f - cy * scale_;
}

// ── Draw ──────────────────────────────────────────────────────────────────────

void MapViewPanel::Draw(float dt) {
    (void)dt;

    // ── Toolbar ───────────────────────────────────────────────────────────────
    ImGui::SetNextItemWidth(360);
    ImGui::InputText("##mappath", path_buf_, sizeof(path_buf_));
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        if (LoadMap(path_buf_, mods_buf_)) {
            // view will be reset below after vp size is known
            rt_w_ = rt_h_ = 0;  // force RT resize to trigger ResetView
        }
    }
    ImGui::SameLine();
    if (loaded_) {
        ImGui::TextColored({0.5f, 1.0f, 0.6f, 1.0f}, "%s  (%dx%d)",
                           map_label_, map_.width, map_.height);
    } else {
        ImGui::TextDisabled("no map loaded");
    }
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
    if (ImGui::Button("Reset##view")) { rt_w_ = rt_h_ = 0; }
    ImGui::Separator();

    // ── Viewport size ─────────────────────────────────────────────────────────
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int vp_w = (int)avail.x;
    int vp_h = (int)avail.y - 20;  // leave room for status bar
    if (vp_w < 64) vp_w = 64;
    if (vp_h < 64) vp_h = 64;

    bool first_load = (rt_w_ != vp_w || rt_h_ != vp_h);
    EnsureRT(vp_w, vp_h);
    if (first_load) ResetView(vp_w, vp_h);

    // ── Render map to FBO ─────────────────────────────────────────────────────
    if (loaded_ && rt_ok_) {
        BeginTextureMode(rt_);
        ClearBackground({20, 20, 30, 255});
        md::flare::TileMap2DRenderer::Get().Render(
            map_, (float)GetTime(),
            origin_x_, origin_y_, scale_,
            vp_w, vp_h);
        // Restore GL state so Raylib/rlImGui work after our renderer.
        rlActiveTextureSlot(0);
        rlEnableTexture(0);
        EndTextureMode();
    }

    // ── ImGui image ───────────────────────────────────────────────────────────
    ImVec2 img_pos = ImGui::GetCursorScreenPos();
    ImGui::Image(
        (ImTextureID)(intptr_t)(rt_ok_ ? rt_.texture.id : 0),
        ImVec2((float)vp_w, (float)vp_h),
        ImVec2(0, 1), ImVec2(1, 0)   // flip Y: OpenGL FBO is bottom-up
    );

    // ── Input inside viewport ─────────────────────────────────────────────────
    bool hovered = ImGui::IsItemHovered();

    // Mouse position relative to viewport
    ImVec2 mouse_abs = ImGui::GetIO().MousePos;
    float  mx = mouse_abs.x - img_pos.x;
    float  my = mouse_abs.y - img_pos.y;

    if (hovered) {
        // Pan: left mouse drag
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            origin_x_ += delta.x;
            origin_y_ += delta.y;
        }

        // Zoom: scroll wheel, anchored to mouse position
        float scroll = ImGui::GetIO().MouseWheel;
        if (scroll != 0.0f) {
            float old_scale = scale_;
            if (scroll > 0) scale_ = fminf(scale_ * 1.15f, 8.0f);
            else            scale_ = fmaxf(scale_ * 0.87f, 0.01f);
            // Zoom toward mouse cursor
            origin_x_ = mx - (mx - origin_x_) * (scale_ / old_scale);
            origin_y_ = my - (my - origin_y_) * (scale_ / old_scale);
        }
    }

    // ── Status bar ────────────────────────────────────────────────────────────
    // Convert mouse → tile coords using inverse Flare formula.
    int tile_col = -1, tile_row = -1;
    if (hovered && loaded_) {
        // Invert: screen_x = (col-row)*96*s + ox, screen_y = (col+row)*48*s + oy
        float sx = (mx - origin_x_) / scale_;   // screen_x in atlas pixels
        float sy = (my - origin_y_) / scale_;   // screen_y in atlas pixels
        // col - row = sx / 96, col + row = sy / 48
        float cr = sx / 96.0f;
        float cs = sy / 48.0f;
        tile_col = (int)roundf((cr + cs) * 0.5f);
        tile_row = (int)roundf((cs - cr) * 0.5f);
    }

    if (hovered && tile_col >= 0 && tile_row >= 0 &&
        tile_col < map_.width && tile_row < map_.height) {
        ImGui::Text("Tile (%d, %d)   Scale: %.2f", tile_col, tile_row, scale_);
    } else {
        ImGui::Text("WASD or drag=pan   Scroll=zoom   Reset button=fit");
    }
}
