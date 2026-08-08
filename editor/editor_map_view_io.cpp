#include "editor_map_view.h"
#include <monkey_dust/flare/tile_map.h>
#include <cstdio>
#include <cstring>
#include <cmath>

// ── Map loading ───────────────────────────────────────────────────────────────

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
