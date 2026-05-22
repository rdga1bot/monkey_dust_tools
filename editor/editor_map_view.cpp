#include "editor_map_view.h"
#ifndef MD_SDL_GPU
#  include "glad.h"
#endif
#include "imgui.h"
#include <monkey_dust/flare/tile_map.h>
#include <monkey_dust/render/md_texture.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <initializer_list>

// ── RTT management ────────────────────────────────────────────────────────────

void MapViewPanel::EnsureRT(int w, int h) {
    if (rt_ok_ && rt_w_ == w && rt_h_ == h) return;
#ifndef MD_SDL_GPU
    if (rt_fbo_) {
        glDeleteFramebuffers(1, &rt_fbo_);
        glDeleteTextures(1, &rt_tex_);
        glDeleteRenderbuffers(1, &rt_depth_);
        rt_fbo_ = rt_tex_ = rt_depth_ = 0;
    }
    glGenFramebuffers(1, &rt_fbo_);
    glGenTextures(1, &rt_tex_);
    glGenRenderbuffers(1, &rt_depth_);

    glBindTexture(GL_TEXTURE_2D, rt_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindRenderbuffer(GL_RENDERBUFFER, rt_depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, rt_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt_tex_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rt_depth_);
    rt_ok_ = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#else
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (rt_color_) { SDL_ReleaseGPUTexture(dev, rt_color_); rt_color_ = nullptr; }
    if (rt_depth_) { SDL_ReleaseGPUTexture(dev, rt_depth_); rt_depth_ = nullptr; }

    SDL_GPUTextureCreateInfo ci = {};
    ci.type   = SDL_GPU_TEXTURETYPE_2D;
    ci.width  = (uint32_t)w;
    ci.height = (uint32_t)h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.usage  = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    rt_color_ = SDL_CreateGPUTexture(dev, &ci);

    ci.format = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    ci.usage  = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    rt_depth_ = SDL_CreateGPUTexture(dev, &ci);

    rt_ok_ = (rt_color_ != nullptr && rt_depth_ != nullptr);
#endif
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
#ifndef MD_SDL_GPU
    if (rt_fbo_) {
        glDeleteFramebuffers(1, &rt_fbo_);
        glDeleteTextures(1, &rt_tex_);
        glDeleteRenderbuffers(1, &rt_depth_);
        rt_fbo_ = rt_tex_ = rt_depth_ = 0;
    }
#else
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (rt_color_) { SDL_ReleaseGPUTexture(dev, rt_color_); rt_color_ = nullptr; }
    if (rt_depth_) { SDL_ReleaseGPUTexture(dev, rt_depth_); rt_depth_ = nullptr; }
#endif
    rt_ok_ = false;
    md::flare::TileMap2DRenderer::Get().Shutdown();
    init_ = false;
}

// ── SDL_GPU: render tile map to RTT (called from main.cpp before ImGui) ───────

#ifdef MD_SDL_GPU
void MapViewPanel::RenderFrame(SDL_GPUCommandBuffer* cmd) {
    if (!init_ || !loaded_ || !rt_ok_ || !rt_color_) return;
    SDL_GPUColorTargetInfo ct = {};
    ct.texture   = rt_color_;
    ct.load_op   = SDL_GPU_LOADOP_CLEAR;
    ct.store_op  = SDL_GPU_STOREOP_STORE;
    ct.clear_color = { 20/255.f, 20/255.f, 30/255.f, 1.f };
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    if (rp) SDL_EndGPURenderPass(rp);
    md::flare::TileMap2DRenderer::Get().RenderToTarget(
        map_, now_s_,
        origin_x_, origin_y_, scale_,
        rt_w_, rt_h_, LayerMask(),
        cmd, rt_color_);
}
#endif

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
    if (op.type == OpType::FLOOD) {
        auto& s = snap_pool_[op.count];
        for (int i = 0; i < map_.width * map_.height; i++)
            map_.layers[op.layer].tiles[i] = s.before[i];
    } else {
        for (int i = 0; i < op.count; i++)
            map_.layers[op.layer].tiles[op.cells[i].row * md::flare::MAX_MAP_WIDTH + op.cells[i].col] = op.cells[i].old_val;
    }
    if (redo_top_ < UNDO_MAX) redo_stack_[redo_top_++] = op;
}

void MapViewPanel::Redo() {
    if (redo_top_ == 0 || !loaded_) return;
    PaintOp op = redo_stack_[--redo_top_];
    if (op.type == OpType::FLOOD) {
        auto& s = snap_pool_[op.count];
        for (int i = 0; i < map_.width * map_.height; i++)
            map_.layers[op.layer].tiles[i] = s.after[i];
    } else {
        for (int i = 0; i < op.count; i++)
            map_.layers[op.layer].tiles[op.cells[i].row * md::flare::MAX_MAP_WIDTH + op.cells[i].col] = op.cells[i].new_val;
    }
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

// ── Layer visibility mask ─────────────────────────────────────────────────────

uint8_t MapViewPanel::LayerMask() const {
    uint8_t mask = 0;
    for (int i = 0; i < map_.layer_count && i < md::flare::MAX_MAP_LAYERS; i++)
        if (layer_visible_[i]) mask |= (1u << i);
    return mask;
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
    int center_col = (int)roundf((cr + cs) * 0.5f);
    int center_row = (int)roundf((cs - cr) * 0.5f);
    if (sel_layer_ < 0 || sel_layer_ >= map_.layer_count) return false;

    uint16_t new_val = erase_mode_ ? 0 : sel_tile_id_;
    if (new_val != 0 && !map_.meta.Find(new_val)) return false;

    int half = brush_size_ / 2;
    PaintOp op;
    op.layer = sel_layer_;
    op.count = 0;

    for (int dr = -half; dr <= half; dr++) {
        for (int dc = -half; dc <= half; dc++) {
            int row = center_row + dr;
            int col = center_col + dc;
            if (col < 0 || col >= map_.width || row < 0 || row >= map_.height) continue;
            uint16_t& cell = map_.layers[sel_layer_].tiles[
                row * md::flare::MAX_MAP_WIDTH + col];
            if (cell == new_val) continue;
            auto& c = op.cells[op.count++];
            c.row     = (int16_t)row;
            c.col     = (int16_t)col;
            c.old_val = cell;
            c.new_val = new_val;
            cell      = new_val;
        }
    }
    if (op.count == 0) return false;
    PushUndo(op);
    return true;
}

// ── Flood fill (Shift+LMB) ───────────────────────────────────────────────────

bool MapViewPanel::FloodFillAt(float mx, float my) {
    if (!loaded_) return false;
    if (sel_layer_ < 0 || sel_layer_ >= map_.layer_count) return false;

    float sx = (mx - origin_x_) / scale_;
    float sy = (my - origin_y_) / scale_;
    int sc = (int)roundf((sx / 96.0f + sy / 48.0f) * 0.5f);
    int sr = (int)roundf((sy / 48.0f - sx / 96.0f) * 0.5f);
    if (sc < 0 || sc >= map_.width || sr < 0 || sr >= map_.height) return false;

    uint16_t new_val = erase_mode_ ? 0 : sel_tile_id_;
    if (new_val != 0 && !map_.meta.Find(new_val)) return false;

    uint16_t* tiles   = map_.layers[sel_layer_].tiles;
    int        stride = md::flare::MAX_MAP_WIDTH;
    uint16_t   target = tiles[sr * stride + sc];
    if (target == new_val) return false;

    // Snapshot before state
    int si = snap_next_ % SNAP_MAX;
    snap_next_++;
    auto& snap = snap_pool_[si];
    for (int i = 0; i < map_.width * map_.height; i++)
        snap.before[i] = tiles[i];  // copy only live area via row*stride+col below

    // BFS flood fill using static queue/visited (singleton — no concurrent use)
    static bool    visited[md::flare::MAX_MAP_WIDTH * md::flare::MAX_MAP_HEIGHT];
    static struct  { int16_t c, r; }
                   queue[md::flare::MAX_MAP_WIDTH * md::flare::MAX_MAP_HEIGHT];

    // Clear only the live area
    for (int r = 0; r < map_.height; r++)
        for (int c = 0; c < map_.width; c++)
            visited[r * stride + c] = false;

    int qhead = 0, qtail = 0;
    visited[sr * stride + sc] = true;
    queue[qtail++] = {(int16_t)sc, (int16_t)sr};

    const int dr[] = {-1, 1, 0, 0};
    const int dc[] = {0, 0, -1, 1};

    while (qhead < qtail) {
        auto [c, r] = queue[qhead++];
        tiles[r * stride + c] = new_val;
        for (int d = 0; d < 4; d++) {
            int nc = c + dc[d], nr = r + dr[d];
            if (nc < 0 || nc >= map_.width || nr < 0 || nr >= map_.height) continue;
            if (visited[nr * stride + nc]) continue;
            if (tiles[nr * stride + nc] != target) continue;
            visited[nr * stride + nc] = true;
            queue[qtail++] = {(int16_t)nc, (int16_t)nr};
        }
    }

    // Snapshot after state
    for (int i = 0; i < map_.width * map_.height; i++)
        snap.after[i] = tiles[i];

    PaintOp op;
    op.type  = OpType::FLOOD;
    op.layer = sel_layer_;
    op.count = si;
    PushUndo(op);
    return true;
}

// ── Spawn helpers ─────────────────────────────────────────────────────────────

// Map iso coords → viewport-relative screen position.
static void SpawnToScreen(float cx, float cy, float ox, float oy, float sc,
                          float& sx, float& sy) {
    sx = (cx - cy) * 96.0f * sc + ox;
    sy = (cx + cy) * 48.0f * sc + oy;
}

void MapViewPanel::DrawSpawnMarkers(ImVec2 img_pos) {
    if (!loaded_) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Hero spawn marker (always drawn)
    {
        float sx, sy;
        SpawnToScreen(map_.hero_x, map_.hero_y, origin_x_, origin_y_, scale_, sx, sy);
        ImVec2 p = {img_pos.x + sx, img_pos.y + sy};
        bool props = (palette_tab_ == 2);
        dl->AddCircleFilled(p, 9.0f, IM_COL32(50, 180, 255, 200));
        dl->AddCircle(p, 9.5f, IM_COL32(255,255,255, props ? 255 : 160), 0, props ? 2.0f : 1.0f);
        dl->AddText({p.x - 3.5f, p.y - 6.5f}, IM_COL32(255, 255, 255, 255), "H");
        if (props && ImGui::IsWindowHovered()) {
            ImVec2 mp = ImGui::GetIO().MousePos;
            float dx = mp.x - p.x, dy = mp.y - p.y;
            if (dx*dx + dy*dy < 100.0f)
                ImGui::SetTooltip("Hero spawn (%.0f, %.0f)", map_.hero_x, map_.hero_y);
        }
    }

    if (map_.spawn_count == 0) return;

    for (int i = 0; i < map_.spawn_count; i++) {
        const auto& s = map_.spawns[i];
        float sx, sy;
        SpawnToScreen(s.center_x, s.center_y, origin_x_, origin_y_, scale_, sx, sy);
        ImVec2 p = {img_pos.x + sx, img_pos.y + sy};

        bool  selected = (sel_spawn_ == i);
        ImU32 fill = selected ? IM_COL32(255, 220, 40, 230) : IM_COL32(220, 60, 40, 210);
        dl->AddCircleFilled(p, 8.0f, fill);
        dl->AddCircle(p, 8.5f, IM_COL32(255, 255, 255, 200), 0, 1.5f);

        char letter[2] = { s.category[0] ? s.category[0] : '?', '\0' };
        dl->AddText({p.x - 3.5f, p.y - 6.5f}, IM_COL32(255, 255, 255, 255), letter);

        if (palette_tab_ == 1 && ImGui::IsWindowHovered()) {
            ImVec2 mp = ImGui::GetIO().MousePos;
            float  dx = mp.x - p.x, dy = mp.y - p.y;
            if (dx*dx + dy*dy < 100.0f)
                ImGui::SetTooltip("%s  lv%d  ×%d  r%d\n(%.0f, %.0f)",
                    s.category, s.level, s.number_min, s.wander_radius,
                    s.center_x, s.center_y);
        }
    }
}

void MapViewPanel::SpawnInteract(float mx, float my) {
    if (!loaded_) return;

    // Hit-test existing spawns
    auto hit_spawn = [&]() -> int {
        for (int i = 0; i < map_.spawn_count; i++) {
            const auto& s = map_.spawns[i];
            float sx, sy;
            SpawnToScreen(s.center_x, s.center_y, origin_x_, origin_y_, scale_, sx, sy);
            float dx = mx - sx, dy = my - sy;
            if (dx*dx + dy*dy < 100.0f) return i;
        }
        return -1;
    };

    // Click: select existing or add new
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int hit = hit_spawn();
        if (hit >= 0) {
            sel_spawn_      = hit;
            spawn_dragging_ = true;
        } else if (map_.spawn_count < md::flare::MAX_SPAWNS_PER_MAP) {
            float lx = (mx - origin_x_) / scale_;
            float ly = (my - origin_y_) / scale_;
            float cr = lx / 96.0f, cs = ly / 48.0f;
            int   c  = (int)roundf((cr + cs) * 0.5f);
            int   r  = (int)roundf((cs - cr) * 0.5f);
            if (c >= 0 && c < map_.width && r >= 0 && r < map_.height) {
                md::flare::FlareSpawn s = {};
                snprintf(s.category, sizeof(s.category), "goblin");
                s.center_x = (float)c;
                s.center_y = (float)r;
                s.number_min = 1;
                s.level = 1;
                s.wander_radius = 3;
                map_.spawns[map_.spawn_count++] = s;
                sel_spawn_      = map_.spawn_count - 1;
                spawn_dragging_ = true;
            }
        }
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) spawn_dragging_ = false;

    // Drag: move selected spawn
    if (spawn_dragging_ && sel_spawn_ >= 0 &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        float lx = (mx - origin_x_) / scale_;
        float ly = (my - origin_y_) / scale_;
        float cr = lx / 96.0f, cs = ly / 48.0f;
        auto& s = map_.spawns[sel_spawn_];
        s.center_x = roundf((cr + cs) * 0.5f);
        s.center_y = roundf((cs - cr) * 0.5f);
        s.center_x = (s.center_x < 0) ? 0 : (s.center_x >= map_.width  ? map_.width  - 1 : s.center_x);
        s.center_y = (s.center_y < 0) ? 0 : (s.center_y >= map_.height ? map_.height - 1 : s.center_y);
    }

    // Delete key: remove selected spawn
    if (sel_spawn_ >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        for (int i = sel_spawn_; i < map_.spawn_count - 1; i++)
            map_.spawns[i] = map_.spawns[i + 1];
        map_.spawn_count--;
        if (sel_spawn_ >= map_.spawn_count) sel_spawn_ = map_.spawn_count - 1;
    }
}

void MapViewPanel::DrawSpawnPanel() {
    if (!loaded_) { ImGui::TextDisabled("Load a map"); return; }
    ImGui::TextDisabled("%d spawns", map_.spawn_count);
    ImGui::SameLine();
    if (ImGui::SmallButton("+##sp")) {
        if (map_.spawn_count < md::flare::MAX_SPAWNS_PER_MAP) {
            md::flare::FlareSpawn s = {};
            snprintf(s.category, sizeof(s.category), "goblin");
            s.center_x = map_.width  * 0.5f;
            s.center_y = map_.height * 0.5f;
            s.number_min = 1; s.level = 1; s.wander_radius = 3;
            map_.spawns[map_.spawn_count++] = s;
            sel_spawn_ = map_.spawn_count - 1;
        }
    }
    ImGui::SameLine();
    if (sel_spawn_ >= 0 && sel_spawn_ < map_.spawn_count && ImGui::SmallButton("Del")) {
        for (int i = sel_spawn_; i < map_.spawn_count - 1; i++)
            map_.spawns[i] = map_.spawns[i + 1];
        map_.spawn_count--;
        if (sel_spawn_ >= map_.spawn_count) sel_spawn_ = map_.spawn_count - 1;
    }
    ImGui::Separator();

    // Spawn list (scrollable)
    ImGui::BeginChild("##splist", ImVec2(-1, 120), false);
    for (int i = 0; i < map_.spawn_count; i++) {
        const auto& s = map_.spawns[i];
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "%s (%d,%d)##s%d",
                 s.category, (int)s.center_x, (int)s.center_y, i);
        if (ImGui::Selectable(lbl, sel_spawn_ == i)) sel_spawn_ = i;
    }
    ImGui::EndChild();

    // Inspector for selected spawn
    if (sel_spawn_ >= 0 && sel_spawn_ < map_.spawn_count) {
        ImGui::Separator();
        auto& s = map_.spawns[sel_spawn_];
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##cat", s.category, sizeof(s.category));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Category");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##lv", &s.level, 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Level");
        if (s.level < 0) s.level = 0;
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##cnt", &s.number_min, 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Count");
        if (s.number_min < 1) s.number_min = 1;
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##rad", &s.wander_radius, 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Wander radius");
        if (s.wander_radius < 0) s.wander_radius = 0;
        ImGui::TextDisabled("(%.0f, %.0f)", s.center_x, s.center_y);
    }
}

// ── Props panel (M9.9) ───────────────────────────────────────────────────────

void MapViewPanel::DrawPropsPanel() {
    if (!loaded_) { ImGui::TextDisabled("Load a map"); return; }

    ImGui::TextDisabled("Map Properties");
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##title", map_.title, sizeof(map_.title));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Title");

    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##music", map_.music_path, sizeof(map_.music_path));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Music path");

    ImGui::Separator();
    ImGui::TextDisabled("Hero spawn:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputFloat("##hx", &map_.hero_x, 1.0f, 0.0f, "X: %.0f");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputFloat("##hy", &map_.hero_y, 1.0f, 0.0f, "Y: %.0f");
    map_.hero_x = (map_.hero_x < 0) ? 0 : (map_.hero_x >= map_.width  ? (float)(map_.width -1) : map_.hero_x);
    map_.hero_y = (map_.hero_y < 0) ? 0 : (map_.hero_y >= map_.height ? (float)(map_.height-1) : map_.hero_y);

    ImGui::Separator();
    ImGui::TextDisabled("Size: %d × %d", map_.width, map_.height);
    ImGui::TextDisabled("Tilesetdef:");
    ImGui::TextWrapped("%s", map_.tileset_def);
    ImGui::Spacing();
    ImGui::TextColored({0.5f,0.8f,0.5f,1.f}, "LMB = set hero spawn");
}

void MapViewPanel::PropsInteract(float mx, float my) {
    if (!loaded_) return;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        float lx = (mx - origin_x_) / scale_;
        float ly = (my - origin_y_) / scale_;
        float cr = lx / 96.0f, cs = ly / 48.0f;
        float c = (cr + cs) * 0.5f, r = (cs - cr) * 0.5f;
        c = (c < 0) ? 0 : (c >= map_.width  ? (float)(map_.width -1) : c);
        r = (r < 0) ? 0 : (r >= map_.height ? (float)(map_.height-1) : r);
        map_.hero_x = roundf(c);
        map_.hero_y = roundf(r);
    }
}

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
