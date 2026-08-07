#include "editor_map_view.h"
#include <monkey_dust/flare/tile_map.h>
#include <cmath>

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

    // Snapshot before state. tiles[] is a fixed MAX_MAP_WIDTH*MAX_MAP_HEIGHT
    // row-major buffer (index = row*MAX_MAP_WIDTH+col, see stride above) --
    // NOT tightly packed to map_.width/height. snap.before/after share that
    // exact same fixed size/layout, so copy the whole buffer 1:1 instead of
    // the first map_.width*map_.height elements (which, for any map narrower
    // than MAX_MAP_WIDTH, previously read/wrote the wrong cells entirely on
    // Undo/Redo -- confirmed bug, see Undo()/Redo() below).
    int si = snap_next_ % SNAP_MAX;
    snap_next_++;
    auto& snap = snap_pool_[si];
    for (int i = 0; i < md::flare::MAX_MAP_WIDTH * md::flare::MAX_MAP_HEIGHT; i++)
        snap.before[i] = tiles[i];

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

    // Snapshot after state (full fixed buffer, see before-state comment above)
    for (int i = 0; i < md::flare::MAX_MAP_WIDTH * md::flare::MAX_MAP_HEIGHT; i++)
        snap.after[i] = tiles[i];

    PaintOp op;
    op.type  = OpType::FLOOD;
    op.layer = sel_layer_;
    op.count = si;
    PushUndo(op);
    return true;
}
