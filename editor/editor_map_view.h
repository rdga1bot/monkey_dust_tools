#pragma once
#include "raylib.h"
#include "imgui.h"
#include <monkey_dust/flare/tile_map.h>
#include <monkey_dust/flare/tile_map_2d_renderer.h>

// M9.0/M9.1 — Map View Panel.
// M9.0: view + pan + zoom inside ImGui FBO viewport.
// M9.1: tile palette (left panel) + layer selector + RMB paint / MMB erase.
class MapViewPanel {
public:
    static MapViewPanel& Get() { static MapViewPanel inst; return inst; }

    void Init();
    void Shutdown();

    // Call inside the "Map" ImGui tab.
    void Draw(float dt);

    // Load a map by path (relative to CWD = repo root).
    bool LoadMap(const char* map_txt_path, const char* mods_root);

private:
    static constexpr float PALETTE_W = 164.0f;

    // FBO
    RenderTexture2D rt_   = {};
    int             rt_w_ = 0, rt_h_ = 0;
    bool            rt_ok_= false;
    void EnsureRT(int w, int h);

    // Map data
    md::flare::FlareMap map_    = {};
    bool                loaded_ = false;
    char map_label_[64]         = "goblin_camp";

    // Pan / zoom
    float origin_x_ = 0.0f, origin_y_ = 0.0f;
    float scale_    = 0.12f;
    bool  need_reset_= false;
    void  ResetView(int vp_w, int vp_h);

    // Input path buffer
    char path_buf_[256] = "third_party/flare-game/mods/empyrean_campaign/maps/goblin_camp.txt";
    char mods_buf_[256] = "third_party/flare-game/mods";

    // M9.1 — palette + paint
    uint16_t sel_tile_id_ = 1;
    int      sel_layer_   = 0;
    bool     erase_mode_  = false;

    void        DrawPalette();
    bool        PaintAt(float mx, float my);
    const char* LayerName(int layer_idx) const;

    bool  init_ = false;
};
