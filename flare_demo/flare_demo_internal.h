#pragma once
// Shared includes/constants/state for flare_demo's split translation units
// (flare_demo_util.cpp, flare_demo_recording.cpp, flare_demo_world3d.cpp,
// flare_demo_ai.cpp) plus main.cpp itself.

// md_flare_demo — Flare-style isometric RPG demo via SDL_GPU.
//
// Like Flare Empyrean Campaign:
//   • Camera follows player (WASD / arrows move player in isometric 4 dirs)
//   • Goblin NPCs patrol, detect player, chase, and melee-attack
//   • Left-click attacks goblin within melee range; goblins die and stop
//   • HP bar in title, kill count, FPS
//   • Q/E or scroll — zoom; R — reset; Escape — quit
//
// Usage:  md_flare_demo [mods_root] [mod_name] [map_name]

#include <monkey_dust/flare/flare_runtime.h>
#include <monkey_dust/flare/tile_map_2d_renderer.h>
#include <monkey_dust/flare/tile_collision.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal.h>
#include <monkey_dust/render/render_pass_graph.h>
#include <monkey_dust/render/cas_pass.h>
#include <monkey_dust/render/moc_culler.h>
#include <monkey_dust/render/npc_gpu_culler.h>
#include <monkey_dust/render/evsm_shadow.h>
#include <monkey_dust/ecs/component_reflect.h>
#include <monkey_dust/ecs/component_warmup.h>
#include <monkey_dust/spatial/world_bvh.h>
#include <monkey_dust/platform/math_types.h>
#include <monkey_dust/ai/sense_system.h>
#include <monkey_dust/ai/bt_system.h>
#include <monkey_dust/ai/bt_action_registry.h>
#include <monkey_dust/ai/bt_json_loader.h>
#include <monkey_dust/ai/fnv.h>
#include <monkey_dust/ai/sense_registry.h>
#include <monkey_dust/ai/squad_signal.h>
#include <monkey_dust/combat/projectile_system.h>
#include <monkey_dust/components/agent_state.h>
#include <monkey_dust/components/bt_components.h>
#include <monkey_dust/components/sense_component.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/ecs/engine_context.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/render/terrain_renderer.h>
#include <monkey_dust/tools/hot_reload.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/wait.h>
#endif

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr int   DEMO_MAX_NPCS     = 32;
static constexpr float LOGIC_TICK_S      = 0.1f;

// NPC speeds (tiles/s)
static constexpr float GUARD_CHASE_SPD   = 2.5f;
static constexpr float GUARD_INVEST_SPD  = 1.0f;
static constexpr float GUARD_PATROL_SPD  = 1.2f;
static constexpr float GUARD_MELEE_RANGE = 1.4f;

// Player
static constexpr float PLAYER_SPD        = 4.5f;   // tiles/s
static constexpr float PLAYER_ATK_RANGE  = 1.5f;   // melee range
static constexpr int   PLAYER_HP_MAX     = 100;
static constexpr int   PLAYER_DMG_LO     = 15;
static constexpr int   PLAYER_DMG_HI     = 35;

// NPC combat
static constexpr int   NPC_HP_INIT       = 30;
static constexpr int   NPC_DMG_LO        = 5;
static constexpr int   NPC_DMG_HI        = 12;
static constexpr float NPC_ATK_COOLDOWN  = 1.2f;   // s

// Camera
static constexpr float CAMERA_SCALE_INIT = 1.0f;   // 1 pixel = 1 atlas pixel

// Wander
static constexpr float WANDER_RADIUS     = 3.f;

// Paths
static const char* BT_JSON_PATH  = "data/bt/guard_npc.bt.json";
static const char* SENSE_JSON    = "data/ai/view_cone_sets.json";

// ── Demo state (extern; real definitions in flare_demo_util.cpp) ──────────────
extern MdEntity  s_player;
extern MdEntity  s_npcs[DEMO_MAX_NPCS];
extern int           s_npc_count;
extern int           s_npc_hp[DEMO_MAX_NPCS];
extern float         s_npc_atk_cd[DEMO_MAX_NPCS];  // per-NPC cooldown timer (s)
extern int           s_kills;
extern int           s_player_hp;
extern bool          s_player_dead;
extern bool          s_player_moving;
extern float         s_player_tgt_x;   // click-to-move destination
extern float         s_player_tgt_z;
extern bool          s_player_has_tgt;
extern int           s_player_atk_tgt;    // index into s_npcs[] (-1 = none)
extern float         s_player_atk_cd;   // player attack cooldown (s)
extern BTSystem      s_bt_sys;
extern md::EngineContext s_ctx;
extern volatile bool s_reload_bt;
extern uint32_t s_rng;
extern pid_t s_rec_pid;  // active recording child PID
extern pid_t s_rec_pid_pending;  // PID awaiting blocking reap at shutdown
extern bool  s_recording;
extern bool s_view_3d;
extern float s_cam_az;     // orbit azimuth  (radians)
extern float s_cam_el;    // orbit elevation (radians, 0=horizon 1.57=top)
extern float s_cam_dist;   // distance from map center
extern GpuPipeline    s_w3d_pipeline;
extern GpuStaticBuffer s_w3d_vbuf;
extern GpuDepthTexture s_w3d_depth;
extern md::CasPass    s_cas;
extern int            s_w3d_tri_count;
extern Vec3           s_w3d_target;  // look-at (map center, world space)
extern SDL_Window*    s_w3d_window;
extern float s_w3d_raw_verts[md::flare::GEO_MAX_VERTS * 3];
extern int   s_w3d_raw_tris [md::flare::GEO_MAX_TRIS  * 3];
extern float s_w3d_flat     [md::flare::GEO_MAX_TRIS  * 9];  // expanded flat VB

static constexpr int BTN_SZ     = 72;
static constexpr int BTN_MARGIN = 14;

// ── Cross-TU function declarations ─────────────────────────────────────────
int CollisionAt(const md::flare::FlareMap& map, int col, int row);
bool IsPassable(const md::flare::FlareMap& map, float x, float z);
uint32_t RandU();
int RandRange(int lo, int hi);
void GenCamIcon(uint8_t* p, bool rec);
bool MakeCamTex(SDL_GPUDevice* dev, const uint8_t* pixels, GpuColorTexture& out);
void StartRecording();
void StopRecording();
void WaitRecordingChild();
void World3DInit(SDL_Window* window, const md::flare::FlareMap& map, float tile_world_size = 1.f);
void World3DRender(int vp_w, int vp_h, float dt);
void World3DShutdown();
void ChdirToRepoRoot();
float MoveToward(WorldTransform& wt, float tx, float tz, float speed_mps);
BTStatus actGuardChase(md::EngineContext&, MdEntity e);
BTStatus actGuardInvestigate(md::EngineContext&, MdEntity e);
BTStatus actGuardPatrol(md::EngineContext& ctx, MdEntity e);
void OnBTFileChanged(const char*);
void RegisterDemoActions();
void LoadNpcBT(BehaviorTree& bt);
void RespawnNpcBT(MdEntity e);
void SpawnDemoEntities(const md::flare::FlareRuntime& rt);
void DestroyDemoEntities();
void LogicTick(float now_ms);
