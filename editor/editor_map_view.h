#pragma once
#ifndef MD_SDL_GPU
#  ifndef GLAD_H_INCLUDED
#    include "glad.h"
#  endif
#else
#  include <monkey_dust/render/gpu_device.h>
#  include <monkey_dust/render/gpu_hal.h>
#  include <SDL3/SDL_gpu.h>
#endif
#include "imgui.h"
#include <monkey_dust/flare/tile_map.h>
#include <monkey_dust/flare/tile_map_2d_renderer.h>

// M9.0–M9.6 — Map View Panel.
// Load/Save operations are handled by the caller (main.cpp menu bar).
// This panel owns only the viewport + palette + paint tool.
// SDL_GPU path: RTT rendered via RenderFrame(cmd) before ImGui; displayed via Image().
class MapViewPanel {
public:
    static MapViewPanel& Get() { static MapViewPanel inst; return inst; }

    void Init();
    void Shutdown();

    // Call inside the "Map" ImGui tab.
    void Draw(float dt);

#ifdef MD_SDL_GPU
    // Call once per frame before ImGui render (from main.cpp), same cmd buffer.
    void RenderFrame(SDL_GPUCommandBuffer* cmd);
#endif

    // Load/Save/New — called from menu bar in main.cpp.
    bool        LoadMap(const char* path);
    bool        NewMap(int width, int height, const char* tilesetdef);
    bool        SaveCurrent();
    bool        SaveTo(const char* path);

    bool        IsLoaded()    const { return loaded_; }
    const char* GetLoadPath() const { return path_buf_; }
    const char* GetSavePath() const { return save_buf_; }

    void Undo();
    void Redo();
    bool CanUndo() const { return undo_top_ > 0; }
    bool CanRedo() const { return redo_top_ > 0; }

private:
    static constexpr float PALETTE_W   = 164.0f;
    static constexpr int   UNDO_MAX    = 256;
    static constexpr int   BRUSH_CELLS = 25;   // max 5×5
    static constexpr int   SNAP_MAX    = 8;

    enum class OpType : uint8_t { BRUSH, FLOOD };

    struct PaintOp {
        OpType   type  = OpType::BRUSH;
        int      layer = 0;
        int      count = 0;
        struct Cell { int16_t row, col; uint16_t old_val, new_val; } cells[BRUSH_CELLS];
    };

    struct FloodSnap {
        uint16_t before[md::flare::MAX_MAP_WIDTH * md::flare::MAX_MAP_HEIGHT];
        uint16_t after[md::flare::MAX_MAP_WIDTH * md::flare::MAX_MAP_HEIGHT];
    };

    // RTT
#ifndef MD_SDL_GPU
    GLuint rt_fbo_   = 0;
    GLuint rt_tex_   = 0;
    GLuint rt_depth_ = 0;
#else
    SDL_GPUTexture* rt_color_ = nullptr;
    SDL_GPUTexture* rt_depth_ = nullptr;
#endif
    int    rt_w_ = 0, rt_h_ = 0;
    bool   rt_ok_= false;
    void   EnsureRT(int w, int h);

    float  now_s_ = 0.0f;

    // Map data
    md::flare::FlareMap map_    = {};
    bool                loaded_ = false;
    char map_label_[64]         = {};

    char path_buf_[256] = "third_party/flare-game/mods/empyrean_campaign/maps/goblin_camp.txt";
    char save_buf_[256] = "";

    float origin_x_ = 0.0f, origin_y_ = 0.0f;
    float scale_    = 0.12f;
    bool  need_reset_= false;
    void  ResetView(int vp_w, int vp_h);

    uint16_t sel_tile_id_ = 1;
    int      sel_layer_   = 0;
    bool     erase_mode_  = false;
    int      brush_size_  = 1;

    bool     layer_visible_[md::flare::MAX_MAP_LAYERS] = {
        true, true, true, true, true, true
    };
    uint8_t  LayerMask() const;

    PaintOp   undo_stack_[UNDO_MAX] = {};
    int       undo_top_ = 0;
    PaintOp   redo_stack_[UNDO_MAX] = {};
    int       redo_top_ = 0;
    FloodSnap snap_pool_[SNAP_MAX]  = {};
    int       snap_next_ = 0;

    void    PushUndo(const PaintOp& op);
    void    ClearHistory();

    int  palette_tab_    = 0;
    int  sel_spawn_      = -1;
    bool spawn_dragging_ = false;

    void DrawPalette();
    void DrawSpawnPanel();
    void DrawPropsPanel();
    void DrawSpawnMarkers(ImVec2 img_pos);
    bool DrawMinimap(ImVec2 img_pos, int vp_w, int vp_h);
    bool PaintAt(float mx, float my);
    bool FloodFillAt(float mx, float my);
    void SpawnInteract(float mx, float my);
    void PropsInteract(float mx, float my);
    const char* LayerName(int layer_idx) const;

    bool  init_ = false;
};
