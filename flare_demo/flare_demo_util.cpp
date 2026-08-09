#include "flare_demo_internal.h"

// State storage (declared extern in the shared header)
MdEntity  s_player         = MdEntity::Null();
MdEntity  s_npcs[DEMO_MAX_NPCS];
int           s_npc_count      = 0;
int           s_npc_hp[DEMO_MAX_NPCS]      = {};
float         s_npc_atk_cd[DEMO_MAX_NPCS]  = {};  // per-NPC cooldown timer (s)
int           s_kills          = 0;
int           s_player_hp      = PLAYER_HP_MAX;
bool          s_player_dead    = false;
bool          s_player_moving  = false;
float         s_player_tgt_x   = 0.f;   // click-to-move destination
float         s_player_tgt_z   = 0.f;
bool          s_player_has_tgt = false;
int           s_player_atk_tgt = -1;    // index into s_npcs[] (-1 = none)
float         s_player_atk_cd  = 0.f;   // player attack cooldown (s)
BTSystem      s_bt_sys;
md::EngineContext s_ctx;
volatile bool s_reload_bt      = false;
uint32_t s_rng = 0xdeadbeef;
pid_t s_rec_pid         = -1;  // active recording child PID
pid_t s_rec_pid_pending = -1;  // PID awaiting blocking reap at shutdown
bool  s_recording       = false;
bool s_view_3d  = false;
float s_cam_az  = 0.f;     // orbit azimuth  (radians)
float s_cam_el  = 0.9f;    // orbit elevation (radians, 0=horizon 1.57=top)
float s_cam_dist = 40.f;   // distance from map center
GpuPipeline    s_w3d_pipeline;
GpuStaticBuffer s_w3d_vbuf;
GpuDepthTexture s_w3d_depth;
md::CasPass    s_cas;
int            s_w3d_tri_count = 0;
Vec3           s_w3d_target    = {0.f,0.f,0.f};  // look-at (map center, world space)
SDL_Window*    s_w3d_window    = nullptr;
float s_w3d_raw_verts[md::flare::GEO_MAX_VERTS * 3];
int   s_w3d_raw_tris [md::flare::GEO_MAX_TRIS  * 3];
float s_w3d_flat     [md::flare::GEO_MAX_TRIS  * 9];  // expanded flat VB

int CollisionAt(const md::flare::FlareMap& map, int col, int row) {
    if (col < 0 || col >= map.width || row < 0 || row >= map.height) return 3;
    for (int li = 0; li < map.layer_count; ++li) {
        if (map.layers[li].type == md::flare::LayerType::COLLISION)
            return (int)map.layers[li].tiles[row * md::flare::MAX_MAP_WIDTH + col];
    }
    return 0; // no collision layer — assume passable
}

// Returns true if the fractional tile position (x, z) is walkable.
// Rounds to nearest integer tile before lookup.
bool IsPassable(const md::flare::FlareMap& map, float x, float z) {
    return CollisionAt(map, (int)floorf(x + 0.5f), (int)floorf(z + 0.5f)) == 0;
}

// ── Simple xorshift32 RNG for random damage.
uint32_t RandU() {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
int RandRange(int lo, int hi) {
    return lo + (int)(RandU() % (uint32_t)(hi - lo + 1));
}

// ── Camera-button recording ─────────────────────────────────────────────────── (BTN_SZ/BTN_MARGIN moved to flare_demo_internal.h -- main.cpp needs them too)


// Draw camera icon into BTN_SZ×BTN_SZ RGBA8 buffer.
// Pixel layout designed at 72×72.
