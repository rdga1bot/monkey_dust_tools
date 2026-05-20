#!/usr/bin/env python3
"""
md_re_structure.py — convert re_extract_report.json into category prompt-files.

Each output file in tmp_/re/<category>.md is a self-contained prompt/spec
ready to paste into a Claude session for implementation decisions.

Usage:
  python3 tools/md_re_structure.py [--report PATH] [--out-dir PATH]
"""

import json, os, argparse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_REPORT  = os.path.join(REPO, "tmp_", "re_extract_report.json")
DEFAULT_OUT_DIR = os.path.join(REPO, "tmp_", "re")

# ── helpers ────────────────────────────────────────────────────────────────────

def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"  → {os.path.relpath(path, REPO)}")


# ── Alien-specific filter ──────────────────────────────────────────────────────

ALIEN_KW = {
    "flamethrower","vent","backstage","frontage","alien","scare","scream",
    "incubation","facehugger","xenomorph","steadycam","debugmenu","debugstring",
    "assert","fakesense","listeningconvo","convo","gamepad","crawlspace",
    "glass","deepcrouch","invent","invents",
}

def is_alien(name):
    low = name.lower()
    return any(k in low for k in ALIEN_KW)

# ── Priority triage for BT nodes ──────────────────────────────────────────────

HIGH_BT = {
    "ActionAbortMeleeAttack","ActionMoveToAttackTarget","ActionHitTargetAndRun",
    "ActionGetOutOfTheWay","ActionMoveInDirection","ActionTakeStep",
    "ActionThreatAware","ActionThreatEscalation","ActionSuspend",
    "ConditionCanShootNow","ConditionHasMeleeBlockAvailable",
    "ConditionHasMeleeCounterAttackAvailable","ConditionIsCurrentCoverValid",
    "ConditionTargetIsUsingMeleeAttack","ConditionTargetIsInWeaponRange",
    "ConditionTargetIsTargetingMe","ConditionLastTimeTargetShotAtMe",
    "ConditionWithdrawState","ConditionHasGroupAwarenessState",
    "ConditionCheckHealthState","ConditionHasAggroLevel",
    "DecoratorLoop",
}
MED_BT = {
    "ActionForceSearch","ActionResetSearchJobs","ActionNotifySquad",
    "ActionSetWithdrawState","ActionSuspiciousItemMoveTo",
    "ActionSuspiciousItemDoneStage","ActionMoveWithGamepad",
    "ActionSetGaugeAmount","ActionSetFrameFlag",
    "ConditionIsCoverTooClose","ConditionIsInCombatArea",
    "ConditionTargetIsInCombatArea","ConditionObjectiveIsInCombatArea",
    "ConditionObjectiveIsWithinDistance","ConditionTargetIsWithinAggroRadius",
    "ConditionTargetIsWithinDistanceThreshold",
    "ConditionTargetIsWithinRoutingDistance",
    "ConditionNeedsToGetOutOfTheWay","ConditionShouldFollow",
    "ConditionTargetNearestStandPointIsWithinDistance",
    "ConditionMostRecentSenseActivationHasBeenAbove",
    "ConditionHasSenseActivationBeenAbove",
    "DecoratorSetSenseSet","DecoratorSuspiciousItemInProgress",
    "SelectorLinear","SequenceLinear",
}


def bt_priority(name):
    if name in HIGH_BT: return "HIGH"
    if name in MED_BT:  return "MEDIUM"
    return "LOW"


# ── File generators ────────────────────────────────────────────────────────────

def gen_bt_nodes(nodes_missing, out_dir):
    rpg = [(n, bt_priority(n)) for n in nodes_missing if not is_alien(n)]
    high  = [(n,p) for n,p in rpg if p == "HIGH"]
    med   = [(n,p) for n,p in rpg if p == "MEDIUM"]
    low   = [(n,p) for n,p in rpg if p == "LOW"]

    lines = [
        "# BT_NODES_TODO.md — Missing BT nodes from AI:Isolation (RPG-relevant)",
        "",
        "Source: `tmp_/AI/AI.exe.c` — LegendPlugin.Nodes.* strings",
        "Filter: Alien/vent/scream-specific nodes removed (28 excluded)",
        "",
        "## How to use this file",
        "Paste the HIGH section as context for a Claude session:",
        "```",
        'PROJECT: monkey_dust | PHASE: BT-ext | TASK: add N BT nodes from list below',
        "FILES: engine/include/monkey_dust/ai/behavior_tree.h",
        "       engine/src/ai/bt_vm_ext.inc",
        "       engine/src/ai/behavior_tree.cpp",
        "CONSTRAINT: C++17, no malloc in hot-path, BTNode 24B, stackless VM",
        "OUTPUT: REPLACE blocks only.",
        "```",
        "",
        f"## HIGH priority ({len(high)} nodes) — implement next",
        "Direct combat/navigation impact; low alien-specificity.",
        "",
    ]
    for n,_ in high:
        lines.append(f"- `{n}`")
    lines += [
        "",
        f"## MEDIUM priority ({len(med)} nodes)",
        "Useful for search/investigation/cover behaviour richness.",
        "",
    ]
    for n,_ in med:
        lines.append(f"- `{n}`")
    lines += [
        "",
        f"## LOW priority ({len(low)} nodes)",
        "Situational; implement when a specific BT tree needs them.",
        "",
    ]
    for n,_ in low:
        lines.append(f"- `{n}`")
    lines += [
        "",
        "## Implementation pattern (per node)",
        "```cpp",
        "// behavior_tree.h — BTNodeType enum",
        "ActionFoo,   // data = <encoding>",
        "",
        "// behavior_tree.h — builder decl",
        "uint16_t addActionFoo(/* params */);",
        "",
        "// behavior_tree.cpp — builder impl",
        "uint16_t BehaviorTree::addActionFoo() {",
        "    uint16_t i = m_nodeCount++;",
        "    initNode(m_nodes[i], BTNodeType::ActionFoo);",
        "    return i;",
        "}",
        "",
        "// bt_vm_ext.inc — VM case",
        "case BTNodeType::ActionFoo: {",
        "    AgentState* as = Registry::Get().try_get<AgentState>(e);",
        "    if (as) as->frame_flags |= (1ull << ff::SHOULD_FOO);",
        "    result = BTStatus::Running; goto exit_loop;",
        "}",
        "```",
    ]
    write(os.path.join(out_dir, "BT_NODES_TODO.md"), "\n".join(lines))


def gen_squad_fsm(states, out_dir):
    # Categorise
    attack  = [s for s in states if "ATTACK" in s]
    patrol  = [s for s in states if "PATROL" in s]
    support = [s for s in states if any(k in s for k in ("FORMATION","SUPPORT","FOLLOW","RETREAT","REGROUP","HORN"))]
    other   = [s for s in states if s not in attack+patrol+support]

    lines = [
        "# SQUAD_FSM.md — VBfA Squad Activity States → monkey_dust SquadSystem",
        "",
        "Source: `tmp_/VBfA/viking.exe.c` — AT_SQUAD_ACTIVITY_* string literals",
        "",
        "## How to use",
        "```",
        "PROJECT: monkey_dust | PHASE: squad-ext | TASK: enrich SquadSystem with activity states",
        "FILES: engine/include/monkey_dust/ai/squad_controller.h",
        "       engine/src/ai/squad_controller.cpp",
        "CONSTRAINT: C++17; MAX_SQUADS=8; no FSM — use BT+flags",
        "OUTPUT: REPLACE blocks only.",
        "```",
        "",
        "## Current state",
        "SquadSystem tracks: player distance, formation centre, morale.",
        "Missing: explicit activity enum (all squads share one codepath).",
        "",
        f"## Attack activities ({len(attack)})",
    ]
    for s in attack: lines.append(f"- `{s}`")
    lines += [f"\n## Patrol activities ({len(patrol)})"]
    for s in patrol: lines.append(f"- `{s}`")
    lines += [f"\n## Formation / support ({len(support)})"]
    for s in support: lines.append(f"- `{s}`")
    lines += [f"\n## Other ({len(other)})"]
    for s in other: lines.append(f"- `{s}`")
    lines += [
        "",
        "## Suggested implementation",
        "Add `SquadActivityState` enum to `squad_controller.h`; transition via",
        "`SquadSystem::Update()` based on player distance + threat level.",
        "Use `SquadSignalBus` to broadcast state changes to member BTs.",
    ]
    write(os.path.join(out_dir, "SQUAD_FSM.md"), "\n".join(lines))


def gen_kenshi_combat(injuries, out_dir):
    # Split into meaningful groups
    armour  = sorted([x for x in injuries if "armour" in x.lower() or "armor" in x.lower()])
    attack  = sorted([x for x in injuries if "attack" in x.lower()])
    combat  = sorted([x for x in injuries if "combat" in x.lower()])
    bleed   = sorted([x for x in injuries if "bleed" in x.lower() or "wound" in x.lower()])
    body    = sorted([x for x in injuries if x not in armour+attack+combat+bleed])

    lines = [
        "# KENSHI_COMBAT.md — Kenshi combat constants → monkey_dust hit_zones / damage_calc",
        "",
        "Source: `tmp_/kenshi/kenshi_x64.exe.c` — combat/armour/attack string keys",
        "",
        "## How to use",
        "```",
        "PROJECT: monkey_dust | PHASE: combat-tune | TASK: enrich hit_zones.h and damage_calc",
        "FILES: engine/include/monkey_dust/combat/hit_zones.h",
        "       engine/include/monkey_dust/combat/damage_calc.h",
        "CONSTRAINT: fixed arrays, no malloc",
        "OUTPUT: REPLACE blocks only.",
        "```",
        "",
        "## Key values referenced in kenshi_x64",
        f"### Armour system ({len(armour)} keys)",
    ]
    for x in armour: lines.append(f"- `{x}`")
    lines += [f"\n### Attack modifiers ({len(attack)})"]
    for x in attack: lines.append(f"- `{x}`")
    lines += [f"\n### Combat stats ({len(combat)})"]
    for x in combat: lines.append(f"- `{x}`")
    lines += [f"\n### Bleeding / wound system ({len(bleed)})"]
    for x in bleed: lines.append(f"- `{x}`")
    lines += [f"\n### Other ({len(body)})"]
    for x in body: lines.append(f"- `{x}`")
    lines += [
        "",
        "## Priority items for monkey_dust",
        "- `max num attack slots` → MdTokenRegistry default limit (already at 2; verify vs Kenshi)",
        "- `armour grade` / `armour penetration` → add to WeaponComponent + damage_calc multipliers",
        "- `bleed rate` / `bleed mult` → implement DoT bleed component",
        "- `combat move speed mult` → apply in NavAgent desired_vel scaling",
        "- `attack distance min vs static` → add min-range melee condition to BT",
    ]
    write(os.path.join(out_dir, "KENSHI_COMBAT.md"), "\n".join(lines))


def gen_kenshi_world(zones, out_dir):
    nav     = sorted([x for x in zones if "nav" in x.lower() or "navmesh" in x.lower()])
    terrain = sorted([x for x in zones if "terrain" in x.lower()])
    lod     = sorted([x for x in zones if "lod" in x.lower()])
    grid    = sorted([x for x in zones if "grid" in x.lower() or "cell" in x.lower()])
    path    = sorted([x for x in zones if "path" in x.lower()])
    other   = sorted([x for x in zones if x not in nav+terrain+lod+grid+path])

    lines = [
        "# KENSHI_WORLD.md — Kenshi nav/terrain/LOD config keys → monkey_dust",
        "",
        "Source: `tmp_/kenshi/kenshi_x64.exe.c`",
        "",
        "## How to use",
        "```",
        "PROJECT: monkey_dust | PHASE: world-tune | TASK: tune nav/terrain/LOD params",
        "FILES: engine/include/monkey_dust/nav/nav_system.h",
        "       engine/include/monkey_dust/world/chunk_manager.h",
        "       game/src/logic_tick.cpp  (BulkComputeLOD thresholds)",
        "OUTPUT: REPLACE blocks only.",
        "```",
        "",
        f"### NavMesh keys ({len(nav)})",
    ]
    for x in nav: lines.append(f"- `{x}`")
    lines += [f"\n### Terrain config ({len(terrain)})"]
    for x in terrain: lines.append(f"- `{x}`")
    lines += [f"\n### LOD ({len(lod)})"]
    for x in lod: lines.append(f"- `{x}`")
    lines += [f"\n### Spatial grid ({len(grid)})"]
    for x in grid: lines.append(f"- `{x}`")
    lines += [f"\n### Pathfinding ({len(path)})"]
    for x in path: lines.append(f"- `{x}`")
    lines += [f"\n### Other ({len(other)})"]
    for x in other: lines.append(f"- `{x}`")
    lines += [
        "",
        "## Priority tuning notes",
        "- `lod range` → confirm 50m/150m in TransformSoA::BulkComputeLOD matches Kenshi scale",
        "- `terrain hi-res distance=400` → Kenshi uses 400m hi-res; our world is 500m zones",
        "- `terrain patch size` / `terrain threshold` → tune ChunkManager chunk size",
        "- `pathfind footprint radius` → maps to CrowdSystem agent radius (0.35f currently)",
        "- `gridHashCellSize` → maps to SpatialGrid::CELL_SIZE",
        "- `nbGridCellsX/Y` → maps to SpatialGrid grid dimensions",
    ]
    write(os.path.join(out_dir, "KENSHI_WORLD.md"), "\n".join(lines))


def gen_kenshi_economy(economy, out_dir):
    bounty = sorted([x for x in economy if "bounty" in x.lower()])
    trade  = sorted([x for x in economy if "trade" in x.lower()])
    vendor = sorted([x for x in economy if "vendor" in x.lower()])
    raid   = sorted([x for x in economy if "raid" in x.lower()])
    other  = sorted([x for x in economy if x not in bounty+trade+vendor+raid])

    lines = [
        "# KENSHI_ECONOMY.md — Kenshi trade/faction economy → monkey_dust WorldSimulation",
        "",
        "Source: `tmp_/kenshi/kenshi_x64.exe.c`",
        "",
        "## How to use",
        "```",
        "PROJECT: monkey_dust | PHASE: economy | TASK: implement economy tick in WorldSimulation",
        "FILES: engine/include/monkey_dust/world/world_simulation.h",
        "       engine/src/world/world_simulation.cpp",
        "CONSTRAINT: 1Hz Tick; no malloc; fixed arrays",
        "OUTPUT: REPLACE blocks only.",
        "```",
        "",
        "## Kenshi economy parameters",
        f"### Bounty system ({len(bounty)})",
    ]
    for x in bounty: lines.append(f"- `{x}`")
    lines += [f"\n### Trade system ({len(trade)})"]
    for x in trade: lines.append(f"- `{x}`")
    lines += [f"\n### Vendor system ({len(vendor)})"]
    for x in vendor: lines.append(f"- `{x}`")
    lines += [f"\n### Raid frequency ({len(raid)})"]
    for x in raid: lines.append(f"- `{x}`")
    lines += [f"\n### Other ({len(other)})"]
    for x in other: lines.append(f"- `{x}`")
    lines += [
        "",
        "## Priority for monkey_dust",
        "- `bounty amount` / `bounty factions` → add to FactionSystem + QuestSystem",
        "- `raidFrequencyMult` / `raidSizeMult` → DirectorSystem::Tick spawn scaling",
        "- `trade price mult` / `trade profit margins` → Inventory price system",
        "- `vendors refresh time` → WorldSimulation 1Hz shop restock tick",
    ]
    write(os.path.join(out_dir, "KENSHI_ECONOMY.md"), "\n".join(lines))


def gen_perf_hints(vbfa, ai, kenshi, out_dir):
    lines = [
        "# PERF_HINTS.md — Threading / LOD / physics config from all 3 decompiles",
        "",
        "## How to use",
        "```",
        "PROJECT: monkey_dust | PHASE: perf-tune | TASK: tune thread counts and LOD thresholds",
        "FILES: game/src/logic_tick.cpp  (loco worker count, LOD thresholds)",
        "       engine/include/monkey_dust/nav/crowd_system.h  (MAX_AGENTS)",
        "       engine/include/monkey_dust/physics/jolt_world.h",
        "OUTPUT: inline comments + constant adjustments only.",
        "```",
        "",
        "## Current monkey_dust settings (for comparison)",
        "- Loco workers: 4 (VBfA-AI7, `N_LOCO_WORKERS=4`)",
        "- CrowdSystem::MAX_AGENTS = 128",
        "- JoltWorld max_bodies = 512 (CharacterVirtual)",
        "- LOD thresholds: 50m (tier0→1), 150m (tier1→2, no Jolt)",
        "- SpatialGrid::CELL_SIZE → check vs Kenshi gridHashCellSize",
        "",
        "## Kenshi threading config",
    ]
    for x in kenshi.get("kenshi_status_flags", []):
        lines.append(f"- `{x}`")

    lines += [
        "",
        "## Task-graph hints (all sources)",
    ]
    all_task = (vbfa.get("task_graph_hints", []) +
                ai.get("task_graph_hints",  []) +
                kenshi.get("task_graph_hints", []))
    seen = set()
    for t in all_task:
        if t not in seen:
            lines.append(f"- `{t}`")
            seen.add(t)

    lines += [
        "",
        "## LOD thresholds found",
    ]
    all_lod = sorted(set(
        vbfa.get("lod_thresholds", []) +
        ai.get("lod_thresholds",   []) +
        kenshi.get("lod_thresholds", [])
    ), key=float)
    for l in all_lod:
        lines.append(f"- `{l}` m")

    lines += [
        "",
        "## Notes",
        "- Kenshi: `internalThreadCount`, `workerThreadPriority`, `solverBatchSize`",
        "  — Kenshi uses Havok AI (hkaiNavMesh); our equivalent is Detour (no batch config).",
        "- `simThreadPriority` → SDL_CreateThread priority; SDL3 has no thread priority API.",
        "- VBfA: 16 loco workers in AI_CHARACTER_LOCO (we use 4; safe on 2-core i5-6200U).",
        "- LOD 50/150m appears consistent with AI:Isolation typical values.",
    ]
    write(os.path.join(out_dir, "PERF_HINTS.md"), "\n".join(lines))


def gen_vbfa_speech(speech, out_dir):
    lines = [
        "# VBFA_BATTLE_SPEECH.md — VBfA battle voice event types → NpcSoundEvent",
        "",
        "Source: `tmp_/VBfA/viking.exe.c`",
        "",
        "## How to use",
        "```",
        "PROJECT: monkey_dust | PHASE: audio | TASK: add battle speech events to NpcSoundEvent",
        "FILES: engine/include/monkey_dust/ai/npc_sound.h",
        "       game/src/ai/ai_goal.cpp or frame_flag_dispatch.cpp",
        "CONSTRAINT: NpcSoundBus MAX_EVENTS limit",
        "OUTPUT: REPLACE blocks only.",
        "```",
        "",
        "## VBfA battle speech event types",
        "Triggered by combat state (winning/losing/about_to_kill/about_to_die).",
        "Two faction variants: HUMAN and DRAKAN (enemy faction).",
        "",
    ]
    for s in speech:
        lines.append(f"- `{s}`")
    lines += [
        "",
        "## Mapping to monkey_dust",
        "- `BATTLE_SPEECH_*_WINNING_`  → trigger when faction morale > 0.7",
        "- `BATTLE_SPEECH_*_LOSING_`   → trigger when faction morale < 0.3",
        "- `BATTLE_SPEECH_*_ABOUT_TO_KILL_` → trigger when target hp < 20%",
        "- `BATTLE_SPEECH_*_ABOUT_TO_DIE_`  → trigger when self hp < 15%",
        "- `BATTLE_SPEECH_*_COMBAT_GENERIC_` → random trigger every ~30s in combat",
    ]
    write(os.path.join(out_dir, "VBFA_BATTLE_SPEECH.md"), "\n".join(lines))


def gen_index(out_dir, files):
    lines = [
        "# RE Analysis — Category Prompt Files",
        "",
        "Generated by `tools/md_re_structure.py` from `tmp_/re_extract_report.json`.",
        "**RE data — private repo only. EULA: Sega / Creative Assembly / Lo-Fi Games.**",
        "",
        "| File | Category | Priority | Items |",
        "|------|----------|----------|-------|",
    ]
    for name, category, priority, count, note in files:
        lines.append(f"| [{name}]({name}) | {category} | {priority} | {count} — {note} |")
    lines += [
        "",
        "## Usage",
        "1. Open the relevant `.md` file",
        "2. Copy the **How to use** block as your Claude prompt header",
        "3. Append the specific items you want to implement",
        "4. Let Claude generate the REPLACE blocks",
    ]
    write(os.path.join(out_dir, "INDEX.md"), "\n".join(lines))


# ── Entry point ────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--report",  default=DEFAULT_REPORT)
    parser.add_argument("--out-dir", default=DEFAULT_OUT_DIR)
    args = parser.parse_args()

    with open(args.report, encoding="utf-8") as f:
        r = json.load(f)

    raw    = r["raw"]
    vbfa   = raw["vbfa"]
    ai     = raw["ai"]
    kenshi = raw["kenshi"]
    nodes_missing = r["bt_node_diff"]["missing_in_monkey_dust"]

    print("md_re_structure — generating category prompt files")

    gen_bt_nodes(nodes_missing,              args.out_dir)
    gen_squad_fsm(vbfa["squad_states"],      args.out_dir)
    gen_kenshi_combat(kenshi["kenshi_injuries"], args.out_dir)
    gen_kenshi_world(kenshi["kenshi_zones"], args.out_dir)
    gen_kenshi_economy(kenshi["kenshi_economy"], args.out_dir)
    gen_perf_hints(vbfa, ai, kenshi,         args.out_dir)
    gen_vbfa_speech(vbfa["battle_speech"],   args.out_dir)

    rpg_bt = [n for n in nodes_missing if not is_alien(n)]
    index_rows = [
        ("BT_NODES_TODO.md",      "AI BT nodes",          "HIGH",   len(rpg_bt),                         "unimplemented RPG-relevant node types"),
        ("SQUAD_FSM.md",          "VBfA squad states",    "MEDIUM", len(vbfa['squad_states']),            "activity states for SquadSystem"),
        ("KENSHI_COMBAT.md",      "Kenshi combat",        "HIGH",   len(kenshi['kenshi_injuries']),       "armour/attack/bleed keys"),
        ("KENSHI_WORLD.md",       "Kenshi nav/terrain",   "MEDIUM", len(kenshi['kenshi_zones']),          "NavMesh/LOD/terrain config keys"),
        ("KENSHI_ECONOMY.md",     "Kenshi economy",       "LOW",    len(kenshi['kenshi_economy']),        "trade/bounty/vendor params"),
        ("PERF_HINTS.md",         "Performance",          "MEDIUM", len(kenshi.get('kenshi_status_flags',[])), "threading + LOD tuning hints"),
        ("VBFA_BATTLE_SPEECH.md", "VBfA audio",           "LOW",    len(vbfa['battle_speech']),           "battle voice event types"),
    ]
    gen_index(args.out_dir, index_rows)

    print(f"\nDone → {os.path.relpath(args.out_dir, REPO)}/")
    print(f"  {len(index_rows)+1} files written (including INDEX.md)")


if __name__ == "__main__":
    main()
