#!/usr/bin/env python3
"""
md_re_extract.py — mine Ghidra decompile outputs for patterns valuable to monkey_dust.

Reads:
  tmp_/VBfA/viking.exe.c          (Viking: Battle for Asgard — AI / squad FSM patterns)
  tmp_/AI/AI.exe.c                (Alien: Isolation — BT node / AI architecture patterns)
  tmp_/kenshi/kenshi_x64.exe.c    (Kenshi — world sim / faction / economy / nav patterns)

Outputs:
  tmp_/re_extract_report.json

Usage:
  python3 tools/md_re_extract.py [--vbfa PATH] [--ai PATH] [--kenshi PATH] [--out PATH]
"""

import re
import json
import argparse
import os
from collections import defaultdict

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_VBFA   = os.path.join(REPO_ROOT, "tmp_", "VBfA",   "viking.exe.c")
DEFAULT_AI     = os.path.join(REPO_ROOT, "tmp_", "AI",     "AI.exe.c")
DEFAULT_KENSHI = os.path.join(REPO_ROOT, "tmp_", "kenshi", "kenshi_x64.exe.c")
DEFAULT_OUT    = os.path.join(REPO_ROOT, "tmp_", "re_extract_report.json")

# ── Pattern tables ─────────────────────────────────────────────────────────────

# BT node strings (from AI.exe.c "LegendPlugin.Nodes.*" literals)
BT_NODE_RE = re.compile(r'"LegendPlugin\.Nodes\.([A-Za-z_][A-Za-z0-9_]*)"')

# Numeric constants: MAX_*, N_*, k* prefixes or SCREAMING_SNAKE
CONST_RE = re.compile(
    r'(?:MAX|MIN|N|NUM|COUNT|LIMIT|CAPACITY|THRESHOLD|RANGE|RADIUS|SPEED|RATE|'
    r'SCALE|FACTOR|BUDGET|TICK|TIME|DIST|WEIGHT|CHANCE|PROB|LOD|HEALTH|DAMAGE|'
    r'ARMOR|AGGRO|MORALE|STAMINA|DETECTION|AWARENESS|ALERT|SQUAD|CROWD|AGENT|'
    r'THREAD|WORKER|BATCH|CHUNK|CELL|TILE|SLOT|POOL|RING|FRAME|UPDATE|INTERVAL)'
    r'[_A-Z0-9]+'
    r'\s*=\s*(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)',
    re.ASCII
)

# AI activity / state machine states (VBfA squad FSM)
STATE_RE = re.compile(r'"(AT_SQUAD_[A-Z_]+|PATROL_[A-Z_]+|SQUAD_[A-Z_]+)"')

# Task-graph / threading hints: references to thread counts, worker pools, batch sizes
TASK_GRAPH_RE = re.compile(
    r'(?:num_threads|n_threads|worker_count|thread_count|batch_size|dispatch_count'
    r'|NUM_WORKERS|N_WORKERS|MAX_WORKERS)\s*[=<>!]+\s*(\d+)',
    re.IGNORECASE
)

# Render / shader config hints
SHADER_RE = re.compile(
    r'"(?:MAX_LIGHTS|MAX_SHADOWS|SHADOW_CASCADE|SSAO_SAMPLES|BLOOM_PASSES|'
    r'LOD_DIST|DISSOLVE_RATE|EDGE_WIDTH|PARTICLE_MAX|INSTANCED_MAX|'
    r'MAX_DRAW_CALLS|MAX_VERTS|DRAW_BUDGET)_?[A-Z0-9]*"'
)

# LOD distance thresholds
LOD_RE = re.compile(
    r'(?:lod|LOD|lodDist|lod_dist|lodRange|lod_range|detailDist)\s*[=<>\[\(]+\s*(\d+(?:\.\d+)?)',
    re.IGNORECASE
)

# Awareness / detection config values
AWARENESS_RE = re.compile(
    r'(?:awareness|AWARENESS|detection|DETECTION|alert|ALERT|aggro|AGGRO|'
    r'suspicion|SUSPICION|threat|THREAT)\s*[=><!]+\s*(\d+(?:\.\d+)?)',
    re.IGNORECASE
)

# Memory pool / allocator hints
ALLOC_RE = re.compile(
    r'(?:pool_size|arena_size|heap_size|block_size|alloc_size|POOL_SIZE|ARENA_SIZE)'
    r'\s*=\s*(\d+)',
    re.IGNORECASE
)

# Battle / combat event strings (VBfA specific)
BATTLE_SPEECH_RE = re.compile(r'"(BATTLE_SPEECH_[A-Z_]+)"')

# Weapon type strings from AI.exe.c
WEAPON_TYPE_RE = re.compile(r'"(WEAPON_TYPE_[A-Z_]+)"')

# Kenshi uses lowercase multi-word string keys (e.g. "faction relations", "lod range")

# Kenshi: faction / squad relationship properties
KENSHI_FACTION_RE = re.compile(
    r'"(faction[^"]{0,40}|squad[^"]{0,40}|alliance[^"]{0,40}|pfaction[^"]{0,40})"',
    re.IGNORECASE)

# Kenshi: world-sim / economy properties
KENSHI_ECONOMY_RE = re.compile(
    r'"(trade[^"]{0,40}|bounty[^"]{0,40}|raidFrequency[^"]{0,40}|raidSize[^"]{0,40}'
    r'|shop[^"]{0,40}|vendor[^"]{0,40})"',
    re.IGNORECASE)

# Kenshi: terrain / nav / LOD configuration keys
KENSHI_ZONE_RE = re.compile(
    r'"(terrain[^"]{0,40}|navmesh[^"]{0,40}|lod[^"]{0,40}|pathfind[^"]{0,40}'
    r'|gridHash[^"]{0,40}|nbGridCells[^"]{0,40})"',
    re.IGNORECASE)

# Kenshi: threading / physics config keys
KENSHI_STATUS_RE = re.compile(
    r'"(workerThread[^"]{0,40}|simThread[^"]{0,40}|backgroundThread[^"]{0,40}'
    r'|internalThread[^"]{0,40}|solverBatch[^"]{0,40})"',
    re.IGNORECASE)

# Kenshi: combat/injury system keys
KENSHI_INJURY_RE = re.compile(
    r'"(combat[^"]{0,40}|armour[^"]{0,40}|attack[^"]{0,40}|bleed[^"]{0,40}'
    r'|wound[^"]{0,40}|starting health[^"]{0,40}|max num attack[^"]{0,40})"',
    re.IGNORECASE)

# Combat formula constants
COMBAT_FORMULA_RE = re.compile(
    r'(?:damage|DAMAGE|blunt|BLUNT|cut|CUT|pierce|PIERCE|armor|ARMOR|'
    r'stun|STUN|bleed|BLEED)\s*\*?\s*=?\s*(\d+\.\d+)',
    re.IGNORECASE
)

# ── Scanner ────────────────────────────────────────────────────────────────────

def scan_file(path: str, label: str) -> dict:
    """Scan a single decompile file and return a structured findings dict."""
    findings = {
        "source": path,
        "label": label,
        "bt_nodes": [],
        "numeric_constants": defaultdict(list),
        "squad_states": [],
        "task_graph_hints": [],
        "lod_thresholds": [],
        "awareness_values": [],
        "alloc_hints": [],
        "battle_speech": [],
        "weapon_types": [],
        "combat_formula_consts": [],
        # Kenshi-specific
        "kenshi_factions": [],
        "kenshi_economy": [],
        "kenshi_zones": [],
        "kenshi_status_flags": [],
        "kenshi_injuries": [],
    }

    if not os.path.exists(path):
        findings["error"] = f"File not found: {path}"
        return findings

    file_size_mb = os.path.getsize(path) / (1024 * 1024)
    print(f"  Scanning {label} ({file_size_mb:.0f} MB)...")

    bt_set       = set()
    state_set    = set()
    speech_set   = set()
    weapon_set   = set()
    const_dict   = {}
    lod_set      = set()
    aware_set    = set()
    alloc_set    = set()
    formula_set  = set()
    task_set     = set()
    ken_fac_set  = set()
    ken_eco_set  = set()
    ken_zone_set = set()
    ken_stat_set = set()
    ken_inj_set  = set()

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            # BT nodes
            for m in BT_NODE_RE.finditer(line):
                bt_set.add(m.group(1))

            # Named constants
            for m in CONST_RE.finditer(line):
                name = m.group(0).split("=")[0].strip()
                val  = m.group(1)
                const_dict[name] = val

            # Squad / activity states
            for m in STATE_RE.finditer(line):
                state_set.add(m.group(1))

            # Task graph / thread hints
            for m in TASK_GRAPH_RE.finditer(line):
                task_set.add((m.group(0).split("=")[0].split("<")[0].split(">")[0].strip(), m.group(1)))

            # LOD
            for m in LOD_RE.finditer(line):
                lod_set.add(m.group(1))

            # Awareness
            for m in AWARENESS_RE.finditer(line):
                aware_set.add(m.group(1))

            # Alloc
            for m in ALLOC_RE.finditer(line):
                alloc_set.add(m.group(1))

            # Battle speech
            for m in BATTLE_SPEECH_RE.finditer(line):
                speech_set.add(m.group(1))

            # Weapon types
            for m in WEAPON_TYPE_RE.finditer(line):
                weapon_set.add(m.group(1))

            # Combat formula
            for m in COMBAT_FORMULA_RE.finditer(line):
                formula_set.add(m.group(1))

            # Kenshi-specific
            for m in KENSHI_FACTION_RE.finditer(line):
                ken_fac_set.add(m.group(1))
            for m in KENSHI_ECONOMY_RE.finditer(line):
                ken_eco_set.add(m.group(1))
            for m in KENSHI_ZONE_RE.finditer(line):
                ken_zone_set.add(m.group(1))
            for m in KENSHI_STATUS_RE.finditer(line):
                ken_stat_set.add(m.group(1))
            for m in KENSHI_INJURY_RE.finditer(line):
                ken_inj_set.add(m.group(1))

    findings["bt_nodes"]             = sorted(bt_set)
    findings["numeric_constants"]    = dict(sorted(const_dict.items()))
    findings["squad_states"]         = sorted(state_set)
    findings["task_graph_hints"]     = sorted([f"{k}={v}" for k, v in task_set])
    findings["lod_thresholds"]       = sorted(lod_set, key=float)
    findings["awareness_values"]     = sorted(aware_set, key=float)
    findings["alloc_hints"]          = sorted(alloc_set, key=int)
    findings["battle_speech"]        = sorted(speech_set)
    findings["weapon_types"]         = sorted(weapon_set)
    findings["combat_formula_consts"]= sorted(formula_set, key=float)
    findings["kenshi_factions"]      = sorted(ken_fac_set)
    findings["kenshi_economy"]       = sorted(ken_eco_set)
    findings["kenshi_zones"]         = sorted(ken_zone_set)
    findings["kenshi_status_flags"]  = sorted(ken_stat_set)
    findings["kenshi_injuries"]      = sorted(ken_inj_set)

    counts = {k: len(v) for k, v in findings.items()
              if isinstance(v, (list, dict)) and k not in ("source", "label")}
    print(f"    → " + ", ".join(f"{k}: {n}" for k, n in counts.items() if n > 0))

    return findings


# ── Cross-reference: what's already in monkey_dust vs what's new ──────────────

ALREADY_IMPLEMENTED = {
    # BT nodes confirmed in engine/include/monkey_dust/ai/behavior_tree.h
    "ActionWeaponEquip", "ActionScript", "ActionRangedShoot", "ActionMeleeAttack",
    "ActionMoveToTarget", "ActionDead", "ActionIdle", "ActionSuccess", "ActionFail",
    "ActionPerformRole", "ActionBreakout", "ActionForceRetreat", "ActionHoldPosition",
    "ActionCallForHelp", "ActionTauntTarget", "ActionSurrenderSelf", "ActionPursueTarget",
    "ActionCircleTarget", "ActionBackOff", "ActionCrouchMove", "ActionVault",
    "ActionChangeCover", "ActionMoveToCover", "ActionIdleInCover", "ActionRangedAim",
    "ActionRequestCover", "ActionStartSearch", "ActionPerformAmbush", "ActionMakeAggressive",
    "ActionIdleTime", "ActionIdleTimeFacingTarget", "ActionSuspiciousItemReaction",
    "ConditionHasToken", "ActionReleaseToken",  # just added
    "ConditionHasAWeapon", "ConditionCurrentWeaponIsEquipped",
    "ConditionCurrentWeaponNeedsReloading", "ConditionHasMeleeAttackAvailable",
    "ConditionHasObjective", "ConditionIsDead", "ConditionIsInCover",
    "ConditionShouldUseCover", "ConditionIsCharacterClass", "ConditionHasScript",
    "ConditionAllowedToSearch", "ConditionAllowedToAttackTarget",
    "ConditionAllowedToPursueTarget", "ConditionEventCountAbove",
    "ConditionSpatialMemoryCountAbove", "ConditionMotivationTicksAbove",
    "ConditionHasVisualHistory", "ConditionHasVentCloseToAlien",
    "ConditionHasFlankedVentCloseToPlayer", "ConditionTargetIsOnlyAccessibleCrouching",
    "ConditionAngleNPCToTargetsAimLessThan", "ConditionTargetIsWithinDistance",
    "ConditionHasLostTarget", "ConditionIsCoverExposed", "ConditionIsInTargetsWeaponRange",
    "ConditionHasSearchedMostRecentSensedPosition", "ConditionHasDoneSuspectResponseMoveTo",
    "ConditionHasDoneSuspectResponseWithinTime", "ConditionHasKilltrap",
    "ConditionHasMeleeAttackAvailableOrIsAttacking", "ConditionIsBranchActive",
    "ConditionIsRequestingCover", "ConditionHasValidCoverToChangeTo",
    "ConditionLastTimeSearchedWithinTime", "ConditionTargetRoutingDistance",
    "ConditionAngleToTarget", "ConditionShouldSuspend",
    "ConditionNpcDevelopmentStageAbove", "ConditionNpcHasAbility",
    "ConditionIsHostileToPlayer", "ConditionIsEnemyOfTarget",
    "ConditionHaveDoneSuspectMoveTo", "ConditionHaveSearchedPos",
    "ConditionAnotherAlienIsAttacking", "ConditionIsPartOfNPCGroup",
    "ConditionIsBackstage", "ConditionCanBreakout", "ConditionIsInVent",
    "ConditionHaveTarget", "ConditionHaveNextTarget",
    "ConditionTargetsWeaponHasAmmo", "ConditionTargetsWeaponHasProperty",
    "ConditionHasDoneSuspectMoveTo", "ConditionHasSearchedPos",
}


def diff_bt_nodes(ai_nodes: list) -> dict:
    """Return nodes from AI.exe.c that are NOT yet implemented in monkey_dust."""
    missing = [n for n in ai_nodes if n not in ALREADY_IMPLEMENTED]
    present = [n for n in ai_nodes if n in ALREADY_IMPLEMENTED]
    return {"missing_in_monkey_dust": missing, "already_implemented": present}


# ── Recommendations builder ────────────────────────────────────────────────────

def build_recommendations(vbfa: dict, ai: dict, kenshi: dict) -> list:
    recs = []

    # New BT nodes from AI.exe.c
    diff = diff_bt_nodes(ai.get("bt_nodes", []))
    if diff["missing_in_monkey_dust"]:
        recs.append({
            "priority": "high",
            "category": "bt_nodes",
            "title": "Unimplemented BT node types from AI.exe",
            "items": diff["missing_in_monkey_dust"],
            "note": "Add to BTNodeType enum + bt_vm_ext.inc + behavior_tree.cpp"
        })

    # Squad FSM states from VBfA
    if vbfa.get("squad_states"):
        recs.append({
            "priority": "medium",
            "category": "squad_fsm",
            "title": "Squad activity states from VBfA",
            "items": vbfa["squad_states"],
            "note": "Enrich SquadSystem with these activity types"
        })

    # Battle speech event types
    if vbfa.get("battle_speech"):
        recs.append({
            "priority": "low",
            "category": "audio",
            "title": "Battle speech event types from VBfA",
            "items": vbfa["battle_speech"],
            "note": "Add to NpcSoundEvent enum for combat voice lines"
        })

    # Weapon type strings from AI.exe
    if ai.get("weapon_types"):
        recs.append({
            "priority": "medium",
            "category": "combat",
            "title": "Weapon type strings from AI.exe",
            "items": ai["weapon_types"],
            "note": "Cross-check against WeaponComponent::weapon_type constants"
        })

    # Task-graph threading hints
    all_task = vbfa.get("task_graph_hints", []) + ai.get("task_graph_hints", [])
    if all_task:
        recs.append({
            "priority": "medium",
            "category": "performance",
            "title": "Thread/batch sizing hints from decompile",
            "items": list(dict.fromkeys(all_task)),  # deduplicate preserving order
            "note": "Compare against LOGIC_TICK_S worker counts in logic_tick.cpp"
        })

    # LOD thresholds
    all_lod = list(dict.fromkeys(
        vbfa.get("lod_thresholds", []) + ai.get("lod_thresholds", [])
    ))
    if all_lod:
        recs.append({
            "priority": "medium",
            "category": "rendering",
            "title": "LOD distance thresholds from decompile",
            "items": all_lod,
            "note": "Compare against TransformSoA::BulkComputeLOD thresholds (50m/150m)"
        })

    # Combat formula constants
    all_formula = list(dict.fromkeys(
        vbfa.get("combat_formula_consts", []) + ai.get("combat_formula_consts", [])
    ))
    if all_formula:
        recs.append({
            "priority": "low",
            "category": "combat_balance",
            "title": "Combat multiplier constants from decompile",
            "items": all_formula,
            "note": "Tune damage_calc.cpp zone multipliers against these values"
        })

    # Large named constants (pool/budget sizes)
    combined_consts = {**vbfa.get("numeric_constants", {}),
                       **ai.get("numeric_constants", {}),
                       **kenshi.get("numeric_constants", {})}
    interesting = {k: v for k, v in combined_consts.items()
                   if any(kw in k for kw in ("MAX", "BUDGET", "POOL", "CAPACITY", "LIMIT", "BATCH"))}
    if interesting:
        recs.append({
            "priority": "medium",
            "category": "constants",
            "title": "MAX/BUDGET/POOL constants from decompile",
            "items": [f"{k} = {v}" for k, v in sorted(interesting.items())],
            "note": "Cross-check engine limits (MAX_AGENTS=128, MAX_BODIES=512, etc.)"
        })

    # Kenshi faction / economy data
    if kenshi.get("kenshi_factions"):
        recs.append({
            "priority": "medium",
            "category": "kenshi_factions",
            "title": "Faction identifiers from kenshi_x64.exe",
            "items": kenshi["kenshi_factions"],
            "note": "Cross-check against game/data/terrain_config.txt faction names"
        })
    if kenshi.get("kenshi_economy"):
        recs.append({
            "priority": "low",
            "category": "kenshi_economy",
            "title": "Economy/trade event strings from kenshi_x64.exe",
            "items": kenshi["kenshi_economy"],
            "note": "Potential WorldSimulation economy tick events to implement"
        })
    if kenshi.get("kenshi_zones"):
        recs.append({
            "priority": "medium",
            "category": "kenshi_world",
            "title": "Zone/biome identifiers from kenshi_x64.exe",
            "items": kenshi["kenshi_zones"],
            "note": "Cross-check against WorldRegistry 34 zones in terrain_config.txt"
        })
    if kenshi.get("kenshi_injuries"):
        recs.append({
            "priority": "high",
            "category": "kenshi_combat",
            "title": "Body part / injury constants from kenshi_x64.exe",
            "items": kenshi["kenshi_injuries"],
            "note": "Enrich hit_zones.h zone table with Kenshi body-part naming"
        })
    if kenshi.get("kenshi_status_flags"):
        recs.append({
            "priority": "medium",
            "category": "kenshi_status",
            "title": "Character status flags from kenshi_x64.exe",
            "items": kenshi["kenshi_status_flags"][:40],  # cap for readability
            "note": "Potential lcflags expansions in agent_state.h"
        })

    return recs


# ── Entry point ────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--vbfa",   default=DEFAULT_VBFA,   help="Path to viking.exe.c")
    parser.add_argument("--ai",    default=DEFAULT_AI,     help="Path to AI.exe.c")
    parser.add_argument("--kenshi",default=DEFAULT_KENSHI, help="Path to kenshi_x64.exe.c")
    parser.add_argument("--out",   default=DEFAULT_OUT,    help="Output JSON path")
    args = parser.parse_args()

    print("md_re_extract — monkey_dust RE pattern extractor")
    print(f"  VBfA:   {args.vbfa}")
    print(f"  AI:     {args.ai}")
    print(f"  Kenshi: {args.kenshi}")

    vbfa_findings   = scan_file(args.vbfa,   "VBfA/viking.exe.c")
    ai_findings     = scan_file(args.ai,     "AI/AI.exe.c")
    kenshi_findings = scan_file(args.kenshi, "kenshi/kenshi_x64.exe.c")

    # BT node diff (AI.exe only — that's where BT nodes live)
    bt_diff = diff_bt_nodes(ai_findings.get("bt_nodes", []))

    recs = build_recommendations(vbfa_findings, ai_findings, kenshi_findings)

    report = {
        "generated_by": "tools/md_re_extract.py",
        "note": "RE data — private repo only. EULA: Sega/Creative Assembly/Lo-Fi Games.",
        "summary": {
            "vbfa_bt_nodes": len(vbfa_findings.get("bt_nodes", [])),
            "ai_bt_nodes_total": len(ai_findings.get("bt_nodes", [])),
            "ai_bt_nodes_missing_in_monkey_dust": len(bt_diff["missing_in_monkey_dust"]),
            "vbfa_squad_states": len(vbfa_findings.get("squad_states", [])),
            "kenshi_factions": len(kenshi_findings.get("kenshi_factions", [])),
            "kenshi_zones": len(kenshi_findings.get("kenshi_zones", [])),
            "kenshi_injuries": len(kenshi_findings.get("kenshi_injuries", [])),
            "recommendations": len(recs),
        },
        "bt_node_diff": bt_diff,
        "recommendations": recs,
        "raw": {
            "vbfa":   vbfa_findings,
            "ai":     ai_findings,
            "kenshi": kenshi_findings,
        },
    }

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)

    print(f"\nReport written → {args.out}")
    s = report["summary"]
    print(f"  {s['ai_bt_nodes_missing_in_monkey_dust']} unimplemented BT nodes (AI.exe)")
    print(f"  {s['vbfa_squad_states']} squad FSM states (VBfA)")
    print(f"  {s['kenshi_factions']} faction IDs, {s['kenshi_zones']} zone IDs, "
          f"{s['kenshi_injuries']} injury constants (Kenshi)")
    print(f"  {len(recs)} recommendation groups total")


if __name__ == "__main__":
    main()
