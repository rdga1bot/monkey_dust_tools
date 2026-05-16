// md_flare_demo — Flare tile-map viewer + AI/ECS demo via SDL_GPU.
//
// Systems exercised end-to-end:
//   SDL3 window → GpuDevice (Vulkan/Metal/D3D12) → SPIR-V shaders
//   → TileMap2DRenderer (tile map) + FlareAnimSystem (NPC sprites)
//   → ECS entities: player + NPCs from Flare spawns
//   → SenseSystemUpdate (Visual cone + Audio falloff per NPC)
//   → BTSystem::Tick (SenseCheck → actDemoMove / actDemoWander)
//   → ProjectileSystem::Tick (no projectiles in base demo; hook in place)
//   → HotReload watching data/bt/demo_npc.bt.json (500ms poll)
//
// Controls:
//   WASD / arrow keys — pan (moves player entity too)
//   Q / E or scroll   — zoom out / in
//   R                 — reset camera
//   Escape            — quit
//
// Usage:
//   md_flare_demo [mods_root] [mod_name] [map_name]

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

// ── Demo constants ────────────────────────────────────────────────────────────

static constexpr int   DEMO_MAX_NPCS    = 32;
static constexpr float LOGIC_TICK_S     = 0.1f;   // 10 TPS
static constexpr float GUARD_CHASE_SPD  = 2.5f;   // m/s — full engage
static constexpr float GUARD_INVEST_SPD = 1.0f;   // m/s — suspicious approach
static constexpr float GUARD_CONV_SPD   = 1.8f;   // m/s — converge on squad alert
static constexpr float GUARD_PATROL_SPD = 1.5f;   // m/s — wander
static constexpr float GUARD_MELEE_RANGE= 1.5f;   // m
static const char*     BT_JSON_PATH     = "data/bt/guard_npc.bt.json";
static const char*     SENSE_JSON       = "data/ai/view_cone_sets.json";

// ── Demo state ────────────────────────────────────────────────────────────────

static entt::entity    s_player        = entt::null;
static entt::entity    s_npcs[DEMO_MAX_NPCS];
static int             s_npc_count     = 0;
static BTSystem        s_bt_sys;
static md::EngineContext s_ctx;
static volatile bool   s_reload_bt     = false;
static int             s_player_hp     = 100;    // decremented on melee hit
static int             s_melee_hits    = 0;      // total hits this session

// ── Repo root ─────────────────────────────────────────────────────────────────

static void ChdirToRepoRoot() {
#ifdef _WIN32
    // Windows: resolve from executable path (3 levels up from build/tools/<exe>)
    char exe[512] = {};
    DWORD n = GetModuleFileNameA(nullptr, exe, sizeof(exe) - 1);
    if (!n) return;
    for (int i = 0; i < 3; ++i) {
        char* p = strrchr(exe, '\\');
        if (!p) return; *p = '\0';
    }
    SetCurrentDirectoryA(exe);
    fprintf(stdout, "[demo] repo root: %s\n", exe);
#else
    char exe[512] = {};
    if (readlink("/proc/self/exe", exe, sizeof(exe) - 1) <= 0) return;
    for (int i = 0; i < 3; ++i) {
        char* p = strrchr(exe, '/');
        if (!p) return; *p = '\0';
    }
    if (exe[0] && chdir(exe) == 0)
        fprintf(stdout, "[demo] repo root: %s\n", exe);
#endif
}

// ── Guard leaf functions ───────────────────────────────────────────────────────

// Shared move helper — move entity toward (tx,tz) at speed_mps; returns dist.
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

// ENGAGE — fast chase toward last known player position.
static BTStatus actGuardChase(md::EngineContext&, entt::entity e) {
    auto& reg = Registry::Get();
    auto* wt  = reg.try_get<WorldTransform>(e);
    auto* sc  = reg.try_get<SenseComponent>(e);
    if (!wt || !sc) return BTStatus::Failure;
    float dist = MoveToward(*wt, sc->last_known_x, sc->last_known_z, GUARD_CHASE_SPD);
    return (dist < GUARD_MELEE_RANGE) ? BTStatus::Success : BTStatus::Running;
}

// ENGAGE — melee attack when close enough to player.
static BTStatus actGuardMelee(md::EngineContext&, entt::entity e) {
    auto& reg = Registry::Get();
    auto* wt  = reg.try_get<WorldTransform>(e);
    auto* sc  = reg.try_get<SenseComponent>(e);
    if (!wt || !sc) return BTStatus::Failure;
    float dx = sc->last_known_x - wt->x;
    float dz = sc->last_known_z - wt->z;
    if (dx * dx + dz * dz > GUARD_MELEE_RANGE * GUARD_MELEE_RANGE)
        return BTStatus::Failure;
    s_player_hp -= 5;
    ++s_melee_hits;
    if (s_player_hp < 0) s_player_hp = 0;
    fprintf(stderr, "[guard] MELEE HIT — player HP: %d\n", s_player_hp);
    return BTStatus::Running;
}

// INVESTIGATE — slow approach to last known position; Success when arrived.
static BTStatus actGuardInvestigate(md::EngineContext&, entt::entity e) {
    auto& reg = Registry::Get();
    auto* wt  = reg.try_get<WorldTransform>(e);
    auto* sc  = reg.try_get<SenseComponent>(e);
    if (!wt || !sc) return BTStatus::Failure;
    float dist = MoveToward(*wt, sc->last_known_x, sc->last_known_z, GUARD_INVEST_SPD);
    return (dist < 0.4f) ? BTStatus::Success : BTStatus::Running;
}

// CONVERGE — squad alert: move toward last known player position at medium speed.
static BTStatus actGuardConverge(md::EngineContext&, entt::entity e) {
    auto& reg = Registry::Get();
    auto* wt  = reg.try_get<WorldTransform>(e);
    auto* sc  = reg.try_get<SenseComponent>(e);
    if (!wt) return BTStatus::Failure;
    if (sc && sc->last_activated_ms[0] > 0u)
        MoveToward(*wt, sc->last_known_x, sc->last_known_z, GUARD_CONV_SPD);
    return BTStatus::Running;
}

// PATROL — random wander within ±8 tiles of current position.
static BTStatus actGuardPatrol(md::EngineContext& ctx, entt::entity e) {
    auto& reg = Registry::Get();
    auto* wt  = reg.try_get<WorldTransform>(e);
    auto* ab  = reg.try_get<AgentBlackboard>(e);
    if (!wt) return BTStatus::Failure;

    static const uint32_t kWX = md::fnv1a("wx");
    static const uint32_t kWZ = md::fnv1a("wz");

    float tx = ab ? bb_get_float(*ab, kWX, wt->x) : wt->x;
    float tz = ab ? bb_get_float(*ab, kWZ, wt->z) : wt->z;
    float dist = MoveToward(*wt, tx, tz, GUARD_PATROL_SPD);

    if (dist < 0.3f) {
        uint32_t r = ctx.frame_index * 2654435761u ^ static_cast<uint32_t>(entt::to_integral(e));
        tx = wt->x + (float)((int)((r >> 4) & 0x1F) - 16);
        tz = wt->z + (float)((int)((r >> 20) & 0x1F) - 16);
        if (ab) { bb_set_float(*ab, kWX, tx); bb_set_float(*ab, kWZ, tz); }
    }
    return BTStatus::Running;
}

// ── HotReload ─────────────────────────────────────────────────────────────────

static void OnBTFileChanged(const char*) { s_reload_bt = true; }

// ── BT setup ─────────────────────────────────────────────────────────────────

static void RegisterDemoActions() {
    auto& r = md::BTActionRegistry::Get();
    r.Clear();
    r.RegisterAction("actGuardChase",       actGuardChase);
    r.RegisterAction("actGuardMelee",       actGuardMelee);
    r.RegisterAction("actGuardInvestigate", actGuardInvestigate);
    r.RegisterAction("actGuardConverge",    actGuardConverge);
    r.RegisterAction("actGuardPatrol",      actGuardPatrol);
}

static void LoadNpcBT(BehaviorTree& bt) {
    RegisterDemoActions();
    if (!BTJsonLoader::LoadFromFile(bt, BT_JSON_PATH))
        fprintf(stderr, "[demo] BT load failed: %s\n", BT_JSON_PATH);
}

// Destroy old BT, create a new one from JSON, attach to entity.
static void RespawnNpcBT(entt::entity e) {
    auto& reg = Registry::Get();
    auto* old = reg.try_get<BehaviorTreeComponent>(e);
    if (old && old->owning && old->tree) { delete old->tree; old->tree = nullptr; }

    auto* tree = new BehaviorTree();
    LoadNpcBT(*tree);
    auto& btc  = reg.emplace_or_replace<BehaviorTreeComponent>(e);
    btc.tree   = tree;
    btc.owning = true;
    btc.enabled = true;
}

// ── Entity spawning ───────────────────────────────────────────────────────────

static void SpawnDemoEntities(const md::flare::FlareRuntime& rt) {
    auto& reg      = Registry::Get();
    const auto& map = rt.GetMap();

    // ── Player entity (tracks camera center) ─────────────────────────────────
    s_player = reg.create();
    auto& pas = reg.emplace<AgentState>(s_player);
    pas.lcflags.set(lcf::IS_PLAYER);
    auto& pwt = reg.emplace<WorldTransform>(s_player);
    pwt.x = map.hero_x; pwt.z = map.hero_y; pwt.y = 0.f; pwt.rot_y = 0.f;

    // ── NPC entities from Flare [enemy] spawns ────────────────────────────────
    s_npc_count = 0;
    for (int i = 0; i < map.spawn_count && s_npc_count < DEMO_MAX_NPCS; ++i) {
        const auto& sp = map.spawns[i];
        int n = (sp.number_min < 1 ? 1 : sp.number_min);
        for (int j = 0; j < n && s_npc_count < DEMO_MAX_NPCS; ++j) {
            entt::entity e = reg.create();
            s_npcs[s_npc_count++] = e;

            reg.emplace<AgentState>(e);
            reg.emplace<AgentBlackboard>(e);
            reg.emplace<SquadMemberComponent>(e).squad_id = 0;  // all guards share squad 0

            auto& wt = reg.emplace<WorldTransform>(e);
            wt.x = sp.center_x + (float)j * 0.8f;
            wt.z = sp.center_y + (float)j * 0.8f;
            wt.y = 0.f; wt.rot_y = 0.f;

            auto& sc = reg.emplace<SenseComponent>(e);
            sc.cone_set_idx = 0;   // first ViewConeSet (Normal vision)
            sc.threshold_lo = 0.3f;
            sc.threshold_hi = 0.7f;
            for (int s = 0; s < MAX_SENSES; ++s) {
                sc.activation[s]      = 0.f;
                sc.last_activated_ms[s] = 0u;
            }
            sc.last_known_x = sp.center_x;
            sc.last_known_z = sp.center_y;

            RespawnNpcBT(e);
        }
    }
    fprintf(stderr, "[demo] Spawned player + %d NPCs (from %d spawn entries)\n",
            s_npc_count, map.spawn_count);
}

// ── Cleanup ───────────────────────────────────────────────────────────────────

static void DestroyDemoEntities() {
    auto& reg = Registry::Get();
    for (int i = 0; i < s_npc_count; ++i) {
        if (reg.valid(s_npcs[i])) reg.destroy(s_npcs[i]);
    }
    if (s_player != entt::null && reg.valid(s_player)) reg.destroy(s_player);
    s_npc_count = 0;
    s_player = entt::null;
}

// ── Logic tick (10 TPS) ───────────────────────────────────────────────────────

static void LogicTick(float now_ms) {
    ++s_ctx.logic_tick;
    ++s_ctx.frame_index;
    s_ctx.delta_time = LOGIC_TICK_S;
    s_ctx.now_s      = now_ms * 0.001f;

    SquadSignalBus::Get().ClearAll();   // reset signals before BT writes new ones
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

    // ── SDL3 init ─────────────────────────────────────────────────────────────
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[demo] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const int WIN_W = 1280, WIN_H = 720;
    SDL_Window* window = SDL_CreateWindow(
        "md_flare_demo — SDL_GPU Tile Map + AI",
        WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "[demo] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // ── GPU device ────────────────────────────────────────────────────────────
    if (!md::GpuDevice::Get().Init(window)) {
        fprintf(stderr, "[demo] GpuDevice::Init failed — no Vulkan/Metal/D3D12?\n");
        SDL_DestroyWindow(window); SDL_Quit(); return 1;
    }
    fprintf(stderr, "[demo] SDL_GPU driver: %s\n", md::GpuDevice::Get().DriverName());

    // ── Sense registry (view cones) ───────────────────────────────────────────
    if (!SenseRegistry::Get().Load(SENSE_JSON))
        fprintf(stderr, "[demo] Warning: '%s' not found — NPCs use fallback (no cone)\n",
                SENSE_JSON);

    // ── Load Flare mod ────────────────────────────────────────────────────────
    fprintf(stderr, "[demo] LoadMod '%s' from '%s' ...\n", mod_name, mods_root);
    auto& rt = md::flare::FlareRuntime::Get();
    if (!rt.LoadMod(mod_name, mods_root, map_name, 1.0f)) {
        fprintf(stderr, "[demo] LoadMod FAILED\n");
        md::GpuDevice::Get().Shutdown();
        SDL_DestroyWindow(window); SDL_Quit(); return 1;
    }
    const auto& map = rt.GetMap();
    fprintf(stderr, "[demo] Map: %dx%d '%s'  enemies:%d  items:%d  powers:%d\n",
            map.width, map.height, map.title,
            rt.GetEnemies().count, rt.GetItems().count, rt.GetPowers().count);

    // ── 2D tile renderer ──────────────────────────────────────────────────────
    auto& tmr2d = md::flare::TileMap2DRenderer::Get();
    tmr2d.Init();
    if (map.tileset_atlas_count > 0) {
        tmr2d.SetAtlases(map);
        fprintf(stderr, "[demo] %d atlas(es) loaded\n", map.tileset_atlas_count);
    } else {
        fprintf(stderr, "[demo] Warning: no atlases — tiles invisible\n");
    }

    // ── ECS + BT setup ───────────────────────────────────────────────────────
    BTSystem::ConnectRegistry(Registry::Get());
    SpawnDemoEntities(rt);

    // ── HotReload ─────────────────────────────────────────────────────────────
    HotReload::Get().Watch(BT_JSON_PATH, OnBTFileChanged);
    HotReload::Get().Start(500);

    // ── 2D camera state ───────────────────────────────────────────────────────
    const float MAP_SCR_W = (float)(map.width + map.height) * 96.f;
    const float MAP_SCR_H = (float)(map.width + map.height) * 48.f;

    int   vp_w = WIN_W, vp_h = WIN_H;
    float scale = 1.f, origin_x = 0.f, origin_y = 0.f;

    auto ResetOrigin = [&]() {
        SDL_GetWindowSize(window, &vp_w, &vp_h);
        float cx_tile = (float)(map.width - map.height) * 48.f;
        float cy_tile = (float)(map.width + map.height) * 24.f;
        scale    = fminf((float)vp_w / MAP_SCR_W, (float)vp_h / MAP_SCR_H) * 0.82f;
        origin_x = (float)vp_w * 0.5f - cx_tile * scale;
        origin_y = (float)vp_h * 0.5f - cy_tile * scale;
    };
    ResetOrigin();

    const float PAN_SPEED = 400.f;   // screen pixels / second
    float       logic_accum = 0.f;
    uint32_t    frame_count = 0;
    uint64_t    prev_ms = SDL_GetTicks();
    bool        quit = false;

    // ── Game loop ─────────────────────────────────────────────────────────────
    while (!quit) {
        uint64_t now_ms = SDL_GetTicks();
        float dt = (float)(now_ms - prev_ms) * 0.001f;
        if (dt > 0.1f) dt = 0.1f;
        prev_ms = now_ms;
        ++frame_count;

        float scroll_y = 0.f;
        bool  do_zoom_out = false, do_zoom_in = false, do_reset = false;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) { quit = true; break; }
            if (ev.type == SDL_EVENT_MOUSE_WHEEL) scroll_y = ev.wheel.y;
            if (ev.type == SDL_EVENT_WINDOW_RESIZED)
                SDL_GetWindowSize(window, &vp_w, &vp_h);
            if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                switch (ev.key.scancode) {
                case SDL_SCANCODE_ESCAPE: quit = true;        break;
                case SDL_SCANCODE_Q:     do_zoom_out = true;  break;
                case SDL_SCANCODE_E:     do_zoom_in  = true;  break;
                case SDL_SCANCODE_R:     do_reset    = true;  break;
                default: break;
                }
            }
        }
        if (quit) break;

        // Pan camera
        const bool* kb   = SDL_GetKeyboardState(nullptr);
        float step = PAN_SPEED * dt;
        if (kb[SDL_SCANCODE_A] || kb[SDL_SCANCODE_LEFT])  origin_x += step;
        if (kb[SDL_SCANCODE_D] || kb[SDL_SCANCODE_RIGHT]) origin_x -= step;
        if (kb[SDL_SCANCODE_W] || kb[SDL_SCANCODE_UP])    origin_y += step;
        if (kb[SDL_SCANCODE_S] || kb[SDL_SCANCODE_DOWN])  origin_y -= step;

        // Zoom toward screen center
        float old_scale = scale;
        if (do_zoom_out) scale = fmaxf(scale * 0.92f, 0.02f);
        if (do_zoom_in)  scale = fminf(scale * 1.08f, 4.0f);
        if (scroll_y != 0.f) {
            float factor = powf(1.05f, scroll_y);
            scale = fmaxf(fminf(scale * factor, 4.0f), 0.02f);
        }
        if (scale != old_scale) {
            float cx = (float)vp_w * 0.5f, cy = (float)vp_h * 0.5f;
            origin_x = cx - (cx - origin_x) * (scale / old_scale);
            origin_y = cy - (cy - origin_y) * (scale / old_scale);
        }
        if (do_reset) ResetOrigin();

        // Track player entity position to camera center (in tile coords)
        // iso tile mapping: screen_x = origin_x + (col - row) * 48 * scale
        //                   screen_y = origin_y + (col + row) * 24 * scale
        // center → col = ((sx - origin_x)/(48*scale) + (sy - origin_y)/(24*scale)) / 2
        if (s_player != entt::null) {
            if (auto* pwt = Registry::Get().try_get<WorldTransform>(s_player)) {
                float sx = (float)vp_w * 0.5f - origin_x;
                float sy = (float)vp_h * 0.5f - origin_y;
                float s48 = 48.f * scale, s24 = 24.f * scale;
                pwt->x = (sx / s48 + sy / s24) * 0.5f;
                pwt->z = (sy / s24 - sx / s48) * 0.5f;
            }
        }

        // Logic tick at 10 TPS
        logic_accum += dt;
        while (logic_accum >= LOGIC_TICK_S) {
            logic_accum -= LOGIC_TICK_S;
            LogicTick(static_cast<float>(now_ms));
        }

        // Hot-reload BT JSON if changed
        if (s_reload_bt) {
            s_reload_bt = false;
            fprintf(stderr, "[demo] Reloading BT: %s\n", BT_JSON_PATH);
            for (int i = 0; i < s_npc_count; ++i)
                RespawnNpcBT(s_npcs[i]);
        }

        // HUD via window title (every 60 frames ≈ 1 s)
        if (frame_count % 60 == 0) {
            int sensed = 0;
            Registry::Get().view<SenseComponent>().each([&](const SenseComponent& sc) {
                if (sc.activation[0] >= sc.threshold_lo) ++sensed;
            });
            s_ctx.fps = (dt > 0.f) ? 1.f / dt : 0.f;
            char title[220];
            snprintf(title, sizeof(title),
                     "md_flare_demo | FPS:%.0f | Guards:%d | Detected:%d | PlayerHP:%d | Hits:%d | Map:%s",
                     s_ctx.fps, s_npc_count, sensed, s_player_hp, s_melee_hits, map.title);
            SDL_SetWindowTitle(window, title);
        }

        rt.Tick(dt);

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
