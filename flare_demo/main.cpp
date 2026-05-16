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
#include <monkey_dust/render/gpu_device.h>
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
#include <monkey_dust/ecs/engine_context.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/tools/hot_reload.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
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

// ── Demo state ────────────────────────────────────────────────────────────────

static entt::entity  s_player         = entt::null;
static entt::entity  s_npcs[DEMO_MAX_NPCS];
static int           s_npc_count      = 0;
static int           s_npc_hp[DEMO_MAX_NPCS]      = {};
static float         s_npc_atk_cd[DEMO_MAX_NPCS]  = {};  // per-NPC cooldown timer (s)
static int           s_kills          = 0;
static int           s_player_hp      = PLAYER_HP_MAX;
static bool          s_player_dead    = false;
static bool          s_player_moving  = false;
static BTSystem      s_bt_sys;
static md::EngineContext s_ctx;
static volatile bool s_reload_bt      = false;

// Simple xorshift32 RNG for random damage.
static uint32_t s_rng = 0xdeadbeef;
static uint32_t RandU() {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static int RandRange(int lo, int hi) {
    return lo + (int)(RandU() % (uint32_t)(hi - lo + 1));
}

// ── Repo root ─────────────────────────────────────────────────────────────────

static void ChdirToRepoRoot() {
#ifdef _WIN32
    char exe[512] = {};
    DWORD n = GetModuleFileNameA(nullptr, exe, sizeof(exe) - 1);
    if (!n) return;
    for (int i = 0; i < 3; ++i) { char* p = strrchr(exe, '\\'); if (!p) return; *p = '\0'; }
    SetCurrentDirectoryA(exe);
#else
    char exe[512] = {};
    if (readlink("/proc/self/exe", exe, sizeof(exe) - 1) <= 0) return;
    for (int i = 0; i < 3; ++i) { char* p = strrchr(exe, '/'); if (!p) return; *p = '\0'; }
    if (exe[0]) chdir(exe);
#endif
}

// ── Blackboard keys ───────────────────────────────────────────────────────────

static constexpr uint32_t kSX = md::fnv1a("spawn_x");
static constexpr uint32_t kSZ = md::fnv1a("spawn_z");
static constexpr uint32_t kWX = md::fnv1a("wx");
static constexpr uint32_t kWZ = md::fnv1a("wz");

// ── Move helper ───────────────────────────────────────────────────────────────

static float MoveToward(WorldTransform& wt, float tx, float tz, float speed_mps) {
    float dx = tx - wt.x, dz = tz - wt.z;
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist > 0.01f) {
        float step = speed_mps * LOGIC_TICK_S / dist;
        if (step > 1.f) step = 1.f;
        wt.x += dx * step;
        wt.z += dz * step;
        wt.rot_y = atan2f(dx, dz);
    }
    return dist;
}

// ── BT action: CHASE + melee attack ──────────────────────────────────────────

static BTStatus actGuardChase(md::EngineContext&, entt::entity e) {
    auto& reg = Registry::Get();
    auto* wt  = reg.try_get<WorldTransform>(e);
    auto* sc  = reg.try_get<SenseComponent>(e);
    if (!wt || !sc) return BTStatus::Failure;

    MoveToward(*wt, sc->last_known_x, sc->last_known_z, GUARD_CHASE_SPD);

    // Melee attack: deal damage to player if within range and cooldown expired.
    if (!s_player_dead && s_player != entt::null) {
        auto* pwt = reg.try_get<WorldTransform>(s_player);
        if (pwt) {
            float ddx = pwt->x - wt->x, ddz = pwt->z - wt->z;
            float dist = sqrtf(ddx*ddx + ddz*ddz);
            for (int i = 0; i < s_npc_count; ++i) {
                if (s_npcs[i] != e) continue;
                if (dist <= GUARD_MELEE_RANGE && s_npc_atk_cd[i] <= 0.f) {
                    int dmg = RandRange(NPC_DMG_LO, NPC_DMG_HI);
                    s_player_hp -= dmg;
                    if (s_player_hp < 0) s_player_hp = 0;
                    s_npc_atk_cd[i] = NPC_ATK_COOLDOWN;
                    fprintf(stderr, "[combat] NPC hits player -%d  (HP=%d)\n", dmg, s_player_hp);
                    if (s_player_hp == 0) {
                        s_player_dead = true;
                        fprintf(stderr, "[combat] PLAYER DEAD — press R to restart\n");
                    }
                }
                break;
            }
        }
    }
    return BTStatus::Running;
}

// ── BT action: INVESTIGATE ────────────────────────────────────────────────────

static BTStatus actGuardInvestigate(md::EngineContext&, entt::entity e) {
    auto& reg = Registry::Get();
    auto* wt  = reg.try_get<WorldTransform>(e);
    auto* sc  = reg.try_get<SenseComponent>(e);
    if (!wt || !sc) return BTStatus::Failure;
    MoveToward(*wt, sc->last_known_x, sc->last_known_z, GUARD_INVEST_SPD);
    return BTStatus::Running;
}

// ── BT action: PATROL ─────────────────────────────────────────────────────────

static BTStatus actGuardPatrol(md::EngineContext& ctx, entt::entity e) {
    auto& reg = Registry::Get();
    auto* wt  = reg.try_get<WorldTransform>(e);
    auto* ab  = reg.try_get<AgentBlackboard>(e);
    if (!wt || !ab) return BTStatus::Failure;

    float sx = bb_get_float(*ab, kSX, wt->x);
    float sz = bb_get_float(*ab, kSZ, wt->z);
    float tx = bb_get_float(*ab, kWX, sx);
    float tz = bb_get_float(*ab, kWZ, sz);

    float dist = MoveToward(*wt, tx, tz, GUARD_PATROL_SPD);

    if (dist < 0.3f) {
        uint32_t r = ctx.frame_index * 2654435761u ^ static_cast<uint32_t>(entt::to_integral(e));
        float angle  = (float)((r & 0xFFu)) / 255.f * 6.28318f;
        float radius = (float)(((r >> 8) & 0xFFu)) / 255.f * WANDER_RADIUS;
        bb_set_float(*ab, kWX, sx + cosf(angle) * radius);
        bb_set_float(*ab, kWZ, sz + sinf(angle) * radius);
    }
    return BTStatus::Running;
}

// ── HotReload ─────────────────────────────────────────────────────────────────

static void OnBTFileChanged(const char*) { s_reload_bt = true; }

// ── BT setup ──────────────────────────────────────────────────────────────────

static void RegisterDemoActions() {
    auto& r = md::BTActionRegistry::Get();
    r.Clear();
    r.RegisterAction("actGuardChase",       actGuardChase);
    r.RegisterAction("actGuardInvestigate", actGuardInvestigate);
    r.RegisterAction("actGuardPatrol",      actGuardPatrol);
}

static void LoadNpcBT(BehaviorTree& bt) {
    RegisterDemoActions();
    if (!BTJsonLoader::LoadFromFile(bt, BT_JSON_PATH))
        fprintf(stderr, "[demo] BT load failed: %s\n", BT_JSON_PATH);
}

static void RespawnNpcBT(entt::entity e) {
    auto& reg = Registry::Get();
    auto* old = reg.try_get<BehaviorTreeComponent>(e);
    if (old && old->owning && old->tree) { delete old->tree; old->tree = nullptr; }
    auto* tree = new BehaviorTree();
    LoadNpcBT(*tree);
    auto& btc = reg.emplace_or_replace<BehaviorTreeComponent>(e);
    btc.tree = tree; btc.owning = true; btc.enabled = true;
}

// ── Spawn ─────────────────────────────────────────────────────────────────────

static void SpawnDemoEntities(const md::flare::FlareRuntime& rt) {
    auto& reg       = Registry::Get();
    const auto& map = rt.GetMap();

    // Player — start at map center so goblins are nearby.
    s_player = reg.create();
    auto& pas = reg.emplace<AgentState>(s_player);
    pas.lcflags.set(lcf::IS_PLAYER);
    auto& pwt = reg.emplace<WorldTransform>(s_player);
    pwt.x = (float)map.width  * 0.5f;
    pwt.z = (float)map.height * 0.5f;
    pwt.y = 0.f; pwt.rot_y = 0.f;
    s_player_hp   = PLAYER_HP_MAX;
    s_player_dead = false;
    s_player_moving = false;
    s_kills = 0;

    // NPCs from Flare [enemy] spawns.
    s_npc_count = 0;
    for (int i = 0; i < map.spawn_count && s_npc_count < DEMO_MAX_NPCS; ++i) {
        const auto& sp = map.spawns[i];
        int n = (sp.number_min < 1 ? 1 : sp.number_min);
        for (int j = 0; j < n && s_npc_count < DEMO_MAX_NPCS; ++j) {
            entt::entity e = reg.create();
            int idx = s_npc_count++;
            s_npcs[idx]        = e;
            s_npc_hp[idx]      = NPC_HP_INIT;
            s_npc_atk_cd[idx]  = 0.f;

            reg.emplace<AgentState>(e);
            auto& ab = reg.emplace<AgentBlackboard>(e);
            reg.emplace<SquadMemberComponent>(e).squad_id = 0;

            float spx = sp.center_x + (float)j * 0.8f;
            float spz = sp.center_y + (float)j * 0.8f;

            auto& wt = reg.emplace<WorldTransform>(e);
            wt.x = spx; wt.z = spz; wt.y = 0.f; wt.rot_y = 0.f;

            bb_set_float(ab, kSX, spx);
            bb_set_float(ab, kSZ, spz);

            auto& sc = reg.emplace<SenseComponent>(e);
            sc.cone_set_idx = 0;
            sc.threshold_lo = 0.3f;
            sc.threshold_hi = 0.7f;
            for (int s = 0; s < MAX_SENSES; ++s) {
                sc.activation[s]        = 0.f;
                sc.last_activated_ms[s] = 0u;
            }
            sc.last_known_x = 0.f;
            sc.last_known_z = 0.f;

            RespawnNpcBT(e);
        }
    }
    fprintf(stderr, "[demo] Player at (%.0f,%.0f) | %d NPCs from %d spawn entries\n",
            pwt.x, pwt.z, s_npc_count, map.spawn_count);
}

static void DestroyDemoEntities() {
    auto& reg = Registry::Get();
    for (int i = 0; i < s_npc_count; ++i)
        if (reg.valid(s_npcs[i])) reg.destroy(s_npcs[i]);
    if (s_player != entt::null && reg.valid(s_player)) reg.destroy(s_player);
    s_npc_count = 0;
    s_player    = entt::null;
}

// ── Logic tick (10 TPS) ───────────────────────────────────────────────────────

static void LogicTick(float now_ms) {
    ++s_ctx.logic_tick;
    ++s_ctx.frame_index;
    s_ctx.delta_time = LOGIC_TICK_S;
    s_ctx.now_s      = now_ms * 0.001f;

    SquadSignalBus::Get().ClearAll();
    SenseSystemUpdate(now_ms);
    s_bt_sys.Tick(s_ctx, Registry::Get(), static_cast<uint32_t>(now_ms));
    md::ProjectileSystem::Get().Tick(LOGIC_TICK_S);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    ChdirToRepoRoot();

    const char* mods_root = (argc > 1) ? argv[1] : "third_party/flare-game/mods";
    const char* mod_name  = (argc > 2) ? argv[2] : "empyrean_campaign";
    const char* map_name  = (argc > 3) ? argv[3] : "maps/goblin_camp.txt";

    // SDL3
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[demo] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    const int WIN_W = 1280, WIN_H = 720;
    SDL_Window* window = SDL_CreateWindow("md_flare_demo", WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    if (!window) { fprintf(stderr, "[demo] Window failed\n"); SDL_Quit(); return 1; }

    if (!md::GpuDevice::Get().Init(window)) {
        fprintf(stderr, "[demo] GpuDevice failed\n");
        SDL_DestroyWindow(window); SDL_Quit(); return 1;
    }
    fprintf(stderr, "[demo] GPU: %s\n", md::GpuDevice::Get().DriverName());

    if (!SenseRegistry::Get().Load(SENSE_JSON))
        fprintf(stderr, "[demo] No sense cones — NPCs use fallback\n");

    // Load Flare mod + map.
    auto& rt = md::flare::FlareRuntime::Get();
    if (!rt.LoadMod(mod_name, mods_root, map_name)) {
        fprintf(stderr, "[demo] LoadMod failed: %s / %s / %s\n", mods_root, mod_name, map_name);
        md::GpuDevice::Get().Shutdown(); SDL_DestroyWindow(window); SDL_Quit(); return 1;
    }
    const auto& map = rt.GetMap();
    fprintf(stderr, "[demo] Map: %s  (%dx%d)\n", map.title, map.width, map.height);

    // Tile renderer.
    auto& tmr2d = md::flare::TileMap2DRenderer::Get();
    tmr2d.Init();
    tmr2d.SetAtlases(map);

    // Load goblin sprite sheet (fantasycore).
    tmr2d.SetNpcSpriteSheet(
        "third_party/flare-game/mods/fantasycore/images/sprites/goblin.png");

    // ECS + BT.
    BTSystem::ConnectRegistry(Registry::Get());
    SpawnDemoEntities(rt);

    HotReload::Get().Watch(BT_JSON_PATH, OnBTFileChanged);
    HotReload::Get().Start(500);

    // ── Camera state ──────────────────────────────────────────────────────────
    int   vp_w = WIN_W, vp_h = WIN_H;
    float scale    = CAMERA_SCALE_INIT;
    float origin_x = 0.f, origin_y = 0.f;

    // Compute origin so camera centers on player's tile position.
    auto ComputeOrigin = [&]() {
        if (s_player == entt::null) return;
        auto* pwt = Registry::Get().try_get<WorldTransform>(s_player);
        if (!pwt) return;
        origin_x = (float)vp_w * 0.5f - (pwt->x - pwt->z) * 96.f * scale;
        origin_y = (float)vp_h * 0.5f - (pwt->x + pwt->z) * 48.f * scale;
    };

    auto ResetCamera = [&]() {
        SDL_GetWindowSize(window, &vp_w, &vp_h);
        scale = CAMERA_SCALE_INIT;
        ComputeOrigin();
    };
    ResetCamera();

    // ── Game loop ─────────────────────────────────────────────────────────────
    float    logic_accum = 0.f;
    uint64_t frame_count = 0;
    uint64_t prev_ms     = SDL_GetTicks();
    bool     quit        = false;
    bool     prev_lmb    = false;

    while (!quit) {
        uint64_t now_ms = SDL_GetTicks();
        float dt = (float)(now_ms - prev_ms) * 0.001f;
        if (dt > 0.1f) dt = 0.1f;
        prev_ms = now_ms;
        ++frame_count;

        float scroll_y   = 0.f;
        bool  do_zoom_in = false, do_zoom_out = false, do_reset = false;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) { quit = true; break; }
            if (ev.type == SDL_EVENT_MOUSE_WHEEL) scroll_y = ev.wheel.y;
            if (ev.type == SDL_EVENT_WINDOW_RESIZED)
                SDL_GetWindowSize(window, &vp_w, &vp_h);
            if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                switch (ev.key.scancode) {
                    case SDL_SCANCODE_ESCAPE: quit = true; break;
                    case SDL_SCANCODE_Q:      do_zoom_out = true; break;
                    case SDL_SCANCODE_E:      do_zoom_in  = true; break;
                    case SDL_SCANCODE_R:
                        // Restart if dead, else reset camera.
                        if (s_player_dead) {
                            DestroyDemoEntities();
                            SpawnDemoEntities(rt);
                            ResetCamera();
                        } else {
                            do_reset = true;
                        }
                        break;
                    default: break;
                }
            }
        }
        if (quit) break;

        // ── Player movement (WASD / arrows) ───────────────────────────────────
        // Isometric 4-direction: W=screen-up(NW tile), S=screen-down(SE),
        //                        A=screen-left(SW), D=screen-right(NE).
        if (!s_player_dead && s_player != entt::null) {
            const bool* kb = SDL_GetKeyboardState(nullptr);
            float dx = 0.f, dz = 0.f;
            if (kb[SDL_SCANCODE_W] || kb[SDL_SCANCODE_UP])    { dx -= 1.f; dz -= 1.f; }
            if (kb[SDL_SCANCODE_S] || kb[SDL_SCANCODE_DOWN])  { dx += 1.f; dz += 1.f; }
            if (kb[SDL_SCANCODE_A] || kb[SDL_SCANCODE_LEFT])  { dx -= 1.f; dz += 1.f; }
            if (kb[SDL_SCANCODE_D] || kb[SDL_SCANCODE_RIGHT]) { dx += 1.f; dz -= 1.f; }

            s_player_moving = (dx != 0.f || dz != 0.f);
            if (s_player_moving) {
                auto* pwt = Registry::Get().try_get<WorldTransform>(s_player);
                if (pwt) {
                    float len = sqrtf(dx*dx + dz*dz);
                    dx /= len; dz /= len;
                    float spd = PLAYER_SPD * dt;
                    pwt->x += dx * spd;
                    pwt->z += dz * spd;
                    pwt->rot_y = atan2f(dx, dz);
                    // Clamp to map bounds.
                    pwt->x = fmaxf(0.f, fminf(pwt->x, (float)(map.width  - 1)));
                    pwt->z = fmaxf(0.f, fminf(pwt->z, (float)(map.height - 1)));
                }
            }
        }

        // ── Left-click: attack nearest goblin within melee range ───────────────
        {
            float mx_f = 0.f, my_f = 0.f;
            bool  lmb = (SDL_GetMouseState(&mx_f, &my_f) & SDL_BUTTON_LMASK) != 0;
            bool  lmb_click = lmb && !prev_lmb;
            prev_lmb = lmb;

            if (lmb_click && !s_player_dead && s_player != entt::null) {
                auto* pwt = Registry::Get().try_get<WorldTransform>(s_player);
                if (pwt) {
                    // Mouse → tile coords (inverse isometric formula).
                    float xmz = (mx_f - origin_x) / (96.f * scale);
                    float xpz = (my_f - origin_y) / (48.f * scale);
                    float mtx = (xmz + xpz) * 0.5f;
                    float mtz = (xpz - xmz) * 0.5f;

                    float best = PLAYER_ATK_RANGE + 1.f;  // click tolerance
                    int   hit  = -1;
                    auto& reg  = Registry::Get();
                    for (int i = 0; i < s_npc_count; ++i) {
                        if (!reg.valid(s_npcs[i])) continue;
                        if (s_npc_hp[i] <= 0) continue;
                        auto* nwt = reg.try_get<WorldTransform>(s_npcs[i]);
                        if (!nwt) continue;
                        // Must be within player melee range.
                        float px = nwt->x - pwt->x, pz = nwt->z - pwt->z;
                        if (sqrtf(px*px + pz*pz) > PLAYER_ATK_RANGE) continue;
                        // Prefer nearest to mouse click.
                        float cx = nwt->x - mtx, cz = nwt->z - mtz;
                        float d  = sqrtf(cx*cx + cz*cz);
                        if (d < best) { best = d; hit = i; }
                    }

                    if (hit >= 0) {
                        int dmg = RandRange(PLAYER_DMG_LO, PLAYER_DMG_HI);
                        s_npc_hp[hit] -= dmg;
                        fprintf(stderr, "[combat] Player hits NPC[%d] -%d  (hp=%d)\n",
                                hit, dmg, s_npc_hp[hit]);
                        if (s_npc_hp[hit] <= 0) {
                            // Mark dead in ECS.
                            auto* nas = Registry::Get().try_get<AgentState>(s_npcs[hit]);
                            if (nas) nas->lcflags.set(lcf::IS_DEAD);
                            ++s_kills;
                            fprintf(stderr, "[combat] NPC[%d] killed!  kills=%d\n", hit, s_kills);
                        }
                    }
                }
            }
        }

        // ── Zoom ──────────────────────────────────────────────────────────────
        {
            float old_scale = scale;
            if (do_zoom_out)    scale = fmaxf(scale * 0.92f, 0.1f);
            if (do_zoom_in)     scale = fminf(scale * 1.08f, 4.0f);
            if (scroll_y != 0.f) {
                float f = powf(1.05f, scroll_y);
                scale = fmaxf(fminf(scale * f, 4.0f), 0.1f);
            }
            if (scale != old_scale) {
                // Zoom toward screen center while keeping player centered.
                (void)old_scale;
                ComputeOrigin();
            }
        }
        if (do_reset) ResetCamera();

        // Camera always follows player.
        ComputeOrigin();

        // ── NPC attack cooldowns ───────────────────────────────────────────────
        for (int i = 0; i < s_npc_count; ++i)
            if (s_npc_atk_cd[i] > 0.f) s_npc_atk_cd[i] -= dt;

        // ── Logic tick (10 TPS) ───────────────────────────────────────────────
        logic_accum += dt;
        while (logic_accum >= LOGIC_TICK_S) {
            logic_accum -= LOGIC_TICK_S;
            LogicTick((float)now_ms);
        }

        // ── Hot-reload BT ─────────────────────────────────────────────────────
        if (s_reload_bt) {
            s_reload_bt = false;
            for (int i = 0; i < s_npc_count; ++i)
                RespawnNpcBT(s_npcs[i]);
        }

        // ── Title bar (≈1/s) ──────────────────────────────────────────────────
        if (frame_count % 60 == 0) {
            int alive = 0;
            for (int i = 0; i < s_npc_count; ++i)
                if (s_npc_hp[i] > 0) ++alive;
            float fps = (dt > 0.f) ? 1.f / dt : 0.f;
            s_ctx.fps = fps;
            char title[256];
            if (s_player_dead) {
                snprintf(title, sizeof(title),
                         "md_flare_demo | DEAD — press R to restart | Kills:%d | Map:%s",
                         s_kills, map.title);
            } else {
                snprintf(title, sizeof(title),
                         "md_flare_demo | FPS:%.0f | HP:%d/%d | NPCs:%d | Kills:%d | Map:%s",
                         fps, s_player_hp, PLAYER_HP_MAX, alive, s_kills, map.title);
            }
            SDL_SetWindowTitle(window, title);
        }

        rt.Tick(dt);

        // ── Collect sprites: player first, then living NPCs ───────────────────
        {
            static constexpr int kMax = 64;
            static float   sp_x[kMax], sp_z[kMax], sp_rot[kMax];
            static uint8_t sp_mov[kMax];
            int sp_n = 0;
            auto& reg = Registry::Get();

            // Player sprite.
            if (!s_player_dead && s_player != entt::null) {
                auto* pwt = reg.try_get<WorldTransform>(s_player);
                if (pwt && sp_n < kMax) {
                    sp_x[sp_n]   = pwt->x;
                    sp_z[sp_n]   = pwt->z;
                    sp_rot[sp_n] = pwt->rot_y;
                    sp_mov[sp_n] = s_player_moving ? 1 : 0;
                    ++sp_n;
                }
            }

            // NPC sprites — skip dead ones.
            for (int i = 0; i < s_npc_count && sp_n < kMax; ++i) {
                if (s_npc_hp[i] <= 0) continue;
                if (!reg.valid(s_npcs[i])) continue;
                auto* wt = reg.try_get<WorldTransform>(s_npcs[i]);
                if (!wt) continue;
                sp_x[sp_n]   = wt->x;
                sp_z[sp_n]   = wt->z;
                sp_rot[sp_n] = wt->rot_y;
                sp_mov[sp_n] = 1;
                ++sp_n;
            }

            tmr2d.SetNpcSprites(sp_x, sp_z, sp_rot, sp_mov, sp_n,
                                (float)now_ms * 0.001f);
        }

        tmr2d.Render(map, (float)now_ms * 0.001f,
                     origin_x, origin_y, scale, vp_w, vp_h);
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    HotReload::Get().Stop();
    HotReload::Get().Unwatch(BT_JSON_PATH);
    DestroyDemoEntities();
    tmr2d.Shutdown();
    md::GpuDevice::Get().Shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
