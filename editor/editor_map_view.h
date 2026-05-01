#pragma once
#include "raylib.h"
#include <monkey_dust/flare/tile_map.h>
#include <monkey_dust/flare/tile_map_2d_renderer.h>

// M9.0–M9.6 — Map View Panel.
// Load/Save operations are handled by the caller (main.cpp menu bar).
// This panel owns only the viewport + palette + paint tool.
class MapViewPanel {
public:
    static MapViewPanel& Get() { static MapViewPanel inst; return inst; }

    void Init();
    void Shutdown();

    // Call inside the "Map" ImGui tab.
    void Draw(float dt);

    // Load/Save/New — called from menu bar in main.cpp.
    bool        LoadMap(const char* path);
    bool        NewMap(int width, int height, const char* tilesetdef);
    bool        SaveCurrent();           // save to last loaded/saved path
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
    static constexpr int   SNAP_MAX    = 8;    // flood-fill snapshots

    enum class OpType : uint8_t { BRUSH, FLOOD };

    // Brush op: count cells changed by one PaintAt() call.
    // Flood op: count = index into snap_pool_.
    struct PaintOp {
        OpType   type  = OpType::BRUSH;
        int      layer = 0;
        int      count = 0;
        struct Cell { int16_t row, col; uint16_t old_val, new_val; } cells[BRUSH_CELLS];
    };

    // Snapshot of one layer before/after a flood fill.
    // 8 snapshots × 64 KB = 512 KB total; stored in BSS (singleton).
    struct FloodSnap {
        uint16_t before[md::flare::MAX_MAP_WIDTH * md::flare::MAX_MAP_HEIGHT];
        uint16_t after[md::flare::MAX_MAP_WIDTH * md::flare::MAX_MAP_HEIGHT];
    };

    // FBO
    RenderTexture2D rt_   = {};
    int             rt_w_ = 0, rt_h_ = 0;
    bool            rt_ok_= false;
    void EnsureRT(int w, int h);

    // Map data
    md::flare::FlareMap map_    = {};
    bool                loaded_ = false;
    char map_label_[64]         = {};

    // Paths (internal — menu bar owns the UI for these)
    char path_buf_[256] = "third_party/flare-game/mods/empyrean_campaign/maps/goblin_camp.txt";
    char save_buf_[256] = "";

    // Pan / zoom
    float origin_x_ = 0.0f, origin_y_ = 0.0f;
    float scale_    = 0.12f;
    bool  need_reset_= false;
    void  ResetView(int vp_w, int vp_h);

    // M9.1 — palette + paint
    uint16_t sel_tile_id_ = 1;
    int      sel_layer_   = 0;
    bool     erase_mode_  = false;
    int      brush_size_  = 1;   // 1, 3, or 5

    // M9.7 — layer visibility
    bool     layer_visible_[md::flare::MAX_MAP_LAYERS] = {
        true, true, true, true, true, true
    };
    uint8_t  LayerMask() const;

    // Undo/Redo
    PaintOp   undo_stack_[UNDO_MAX] = {};
    int       undo_top_ = 0;
    PaintOp   redo_stack_[UNDO_MAX] = {};
    int       redo_top_ = 0;
    FloodSnap snap_pool_[SNAP_MAX]  = {};
    int       snap_next_ = 0;   // circular slot allocator

    void    PushUndo(const PaintOp& op);
    void    ClearHistory();

    void        DrawPalette();
    bool        PaintAt(float mx, float my);
    bool        FloodFillAt(float mx, float my);
    const char* LayerName(int layer_idx) const;

    bool  init_ = false;
};
