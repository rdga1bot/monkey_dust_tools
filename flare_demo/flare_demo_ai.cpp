#include "flare_demo_internal.h"

void ChdirToRepoRoot() {
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

float MoveToward(WorldTransform& wt, float tx, float tz, float speed_mps) {
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

BTStatus actGuardChase(md::EngineContext&, MdEntity e) {
    auto& reg = MdRegistry::Get();
    auto* wt  = reg.Handle(e).try_get_mut<WorldTransform>();
    auto* sc  = reg.Handle(e).try_get_mut<SenseComponent>();
    if (!wt || !sc) return BTStatus::Failure;

    MoveToward(*wt, sc->last_known_x, sc->last_known_z, GUARD_CHASE_SPD);

    // Melee attack: deal damage to player if within range and cooldown expired.
    if (!s_player_dead && s_player != MdEntity::Null()) {
        auto* pwt = reg.Handle(s_player).try_get_mut<WorldTransform>();
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

BTStatus actGuardInvestigate(md::EngineContext&, MdEntity e) {
    auto& reg = MdRegistry::Get();
    auto* wt  = reg.Handle(e).try_get_mut<WorldTransform>();
    auto* sc  = reg.Handle(e).try_get_mut<SenseComponent>();
    if (!wt || !sc) return BTStatus::Failure;
    MoveToward(*wt, sc->last_known_x, sc->last_known_z, GUARD_INVEST_SPD);
    return BTStatus::Running;
}

// ── BT action: PATROL ─────────────────────────────────────────────────────────

BTStatus actGuardPatrol(md::EngineContext& ctx, MdEntity e) {
    auto& reg = MdRegistry::Get();
    auto* wt  = reg.Handle(e).try_get_mut<WorldTransform>();
    auto* ab  = reg.Handle(e).try_get_mut<AgentBlackboard>();
    if (!wt || !ab) return BTStatus::Failure;

    float sx = bb_get_float(*ab, kSX, wt->x);
    float sz = bb_get_float(*ab, kSZ, wt->z);
    float tx = bb_get_float(*ab, kWX, sx);
    float tz = bb_get_float(*ab, kWZ, sz);

    float dist = MoveToward(*wt, tx, tz, GUARD_PATROL_SPD);

    if (dist < 0.3f) {
        uint32_t r = ctx.frame_index * 2654435761u ^ e.ToIntegral();
        float angle  = (float)((r & 0xFFu)) / 255.f * 6.28318f;
        float radius = (float)(((r >> 8) & 0xFFu)) / 255.f * WANDER_RADIUS;
        bb_set_float(*ab, kWX, sx + cosf(angle) * radius);
        bb_set_float(*ab, kWZ, sz + sinf(angle) * radius);
    }
    return BTStatus::Running;
}

// ── HotReload ─────────────────────────────────────────────────────────────────

void OnBTFileChanged(const char*) { s_reload_bt = true; }

// ── BT setup ──────────────────────────────────────────────────────────────────

void RegisterDemoActions() {
    auto& r = md::BTActionRegistry::Get();
    r.Clear();
    r.RegisterAction("actGuardChase",       actGuardChase);
    r.RegisterAction("actGuardInvestigate", actGuardInvestigate);
    r.RegisterAction("actGuardPatrol",      actGuardPatrol);
}

void LoadNpcBT(BehaviorTree& bt) {
    RegisterDemoActions();
    if (!BTJsonLoader::LoadFromFile(bt, BT_JSON_PATH))
        fprintf(stderr, "[demo] BT load failed: %s\n", BT_JSON_PATH);
}

void RespawnNpcBT(MdEntity e) {
    auto& reg = MdRegistry::Get();
    auto* old = reg.Handle(e).try_get_mut<BehaviorTreeComponent>();
    if (old && old->owning && old->tree) { delete old->tree; old->tree = nullptr; }
    auto* tree = new BehaviorTree();
    LoadNpcBT(*tree);
    reg.Handle(e).set<BehaviorTreeComponent>(BehaviorTreeComponent{});
    auto& btc = reg.Handle(e).get_mut<BehaviorTreeComponent>();
    btc.tree = tree; btc.owning = true; btc.enabled = true;
}

// ── Spawn ─────────────────────────────────────────────────────────────────────

void SpawnDemoEntities(const md::flare::FlareRuntime& rt) {
    auto& reg       = MdRegistry::Get();
    const auto& map = rt.GetMap();

    // Player — spawn at map's hero_pos, but for this demo use a position
    // that's in the middle of the goblin camp (near NPC spawn groups).
    // goblin_camp: hero_pos=5,2 is a dead-end corner; spawn at (18,26)
    // which is passable and surrounded by the first two goblin groups.
    s_player = reg.Create();
    reg.Handle(s_player).emplace<AgentState>();
    reg.Handle(s_player).get_mut<AgentState>().lcflags.set(lcf::IS_PLAYER);
    reg.Handle(s_player).emplace<WorldTransform>();
    {
        auto& pwt = reg.Handle(s_player).get_mut<WorldTransform>();
        pwt.x = 18.f;
        pwt.z = 26.f;
        pwt.y = 0.f; pwt.rot_y = 0.f;
    }
    s_player_hp      = PLAYER_HP_MAX;
    s_player_dead    = false;
    s_player_moving  = false;
    s_player_has_tgt = false;
    s_player_atk_tgt = -1;
    s_player_atk_cd  = 0.f;
    s_kills = 0;

    // NPCs from Flare [enemy] spawns.
    s_npc_count = 0;
    for (int i = 0; i < map.spawn_count && s_npc_count < DEMO_MAX_NPCS; ++i) {
        const auto& sp = map.spawns[i];
        int n = (sp.number_min < 1 ? 1 : sp.number_min);
        for (int j = 0; j < n && s_npc_count < DEMO_MAX_NPCS; ++j) {
            MdEntity e = reg.Create();
            int idx = s_npc_count++;
            s_npcs[idx]        = e;
            s_npc_hp[idx]      = NPC_HP_INIT;
            s_npc_atk_cd[idx]  = 0.f;

            reg.Handle(e).emplace<AgentState>();
            reg.Handle(e).emplace<AgentBlackboard>();
            reg.Handle(e).emplace<SquadMemberComponent>();
            reg.Handle(e).get_mut<SquadMemberComponent>().squad_id = 0;

            float spx = sp.center_x + (float)j * 0.8f;
            float spz = sp.center_y + (float)j * 0.8f;

            reg.Handle(e).emplace<WorldTransform>();
            auto& wt = reg.Handle(e).get_mut<WorldTransform>();
            wt.x = spx; wt.z = spz; wt.y = 0.f; wt.rot_y = 0.f;

            // B3.4: re-fetch AgentBlackboard here — the reference from its
            // Emplace() above was invalidated by the Emplace<SquadMemberComponent>/
            // Emplace<WorldTransform> calls since (same pattern as
            // world_init.cpp/world_serializer.cpp).
            auto& ab = reg.Handle(e).get_mut<AgentBlackboard>();
            bb_set_float(ab, kSX, spx);
            bb_set_float(ab, kSZ, spz);

            reg.Handle(e).emplace<SenseComponent>();
            auto& sc = reg.Handle(e).get_mut<SenseComponent>();
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
    const auto& pwt_final = reg.Handle(s_player).get_mut<WorldTransform>();
    fprintf(stderr, "[demo] Player at (%.0f,%.0f) | %d NPCs from %d spawn entries\n",
            pwt_final.x, pwt_final.z, s_npc_count, map.spawn_count);
}

void DestroyDemoEntities() {
    auto& reg = MdRegistry::Get();
    for (int i = 0; i < s_npc_count; ++i)
        if (reg.Valid(s_npcs[i])) reg.Destroy(s_npcs[i]);
    if (s_player != MdEntity::Null() && reg.Valid(s_player)) reg.Destroy(s_player);
    s_npc_count = 0;
    s_player    = MdEntity::Null();
}

// ── Logic tick (10 TPS) ───────────────────────────────────────────────────────

void LogicTick(float now_ms) {
    ++s_ctx.logic_tick;
    ++s_ctx.frame_index;
    s_ctx.delta_time = LOGIC_TICK_S;
    s_ctx.now_s      = now_ms * 0.001f;

    SquadSignalBus::Get().ClearAll();
    SenseSystemUpdate(now_ms);
    s_bt_sys.Tick(s_ctx, MdRegistry::Get().Raw(), static_cast<uint32_t>(now_ms));
    md::ProjectileSystem::Get().Tick(LOGIC_TICK_S);
}

// ── main ──────────────────────────────────────────────────────────────────────

