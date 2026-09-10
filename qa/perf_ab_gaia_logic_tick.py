#!/usr/bin/env python3
"""
perf_ab_gaia_logic_tick.py -- GATE 4 measurement (flecs->gaia migration,
prompt_/PROMPT_GAIA_MIGRATION.md §6): "logic tick у мс під ON не гірший
за OFF більш ніж на 10%. Заміряй, не оцінюй."

Same zone/pose/settle convention as perf_ab_jobsystem_workers.py (real
precedent for A/B measurement in this repo) -- zone(24,12) "horizon",
teleport BOTH camera and player (AI proximity tiers gate off player
position, not camera). Unlike that script (which samples md.get_gpu_ms()
per-frame via the command channel), this parses the periodic
"[PERF] N FPS | NPCs=N | Logic=X.Yms(maxY.Y) ..." aggregate lines
(engine/include/monkey_dust/platform/frame_stats.h, same source
qa_perf_baseline.py already parses) directly out of game_stdout.log --
Logic= is exactly the logic-tick budget GATE 4 asks about, and it's
already aggregated (avg+max) over each 5s report window server-side, so
no separate per-frame sampling loop is needed.

Caller is responsible for having the correct MD_ECS_GAIA-flavored
`monkey_dust` binary built BEFORE invoking each side of the A/B (this
script does not build). Run once per side, diff the two RESULT_JSON
blocks (median Logic ms, %-diff, GATE 4 threshold=10%).

USAGE:
  python3 tools/qa/perf_ab_gaia_logic_tick.py --label flecs_off --exe build/game/monkey_dust
  python3 tools/qa/perf_ab_gaia_logic_tick.py --label gaia_on   --exe temp_/build_gaia_probe/game/monkey_dust
"""
import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_cmd_driver import Driver, median, stdev  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
STDOUT_PATH = REPO_ROOT / "tmp_" / "game_cmd" / "game_stdout.log"

CHUNK_SIZE = 460.8
DEFAULT_ZONE = (24, 12)
POSE_LABEL, PITCH, DIST = "horizon", 10.0, 22.0
SETTLE_S = 8.0
WINDOW_S = 35.0  # >= 6 full 5s [PERF] report intervals
SPAWN_BATCHES = 8   # md.spawn caps at 64/call (fixed array, no heap) -- 8x64=512
SPAWN_PER_BATCH = 64
SPAWN_SPREAD_M = 40.0  # world-space jitter between batches, avoid exact stacking

PERF_LINE_RE = re.compile(r"^\[PERF\]\s+(\d+)\s+FPS\s+\|\s+NPCs=(\d+)\s+\|\s+(.*)$")
PASS_RE = re.compile(r"(\w+)=([\d.]+)ms\(max([\d.]+)\)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", required=True, help="tag for this A/B side, e.g. flecs_off / gaia_on")
    ap.add_argument("--exe", required=True)
    ap.add_argument("--window", type=float, default=WINDOW_S)
    ap.add_argument("--zone", default=None, help="grid_x,grid_z override (default 24,12)")
    ap.add_argument("--spawn-npcs", type=int, default=SPAWN_BATCHES * SPAWN_PER_BATCH,
                     help="target NPC population near player before measuring (0 = don't spawn, use ambient population only)")
    args = ap.parse_args()

    ZONE = tuple(int(v) for v in args.zone.split(",")) if args.zone else DEFAULT_ZONE
    zx, zz = ZONE
    wx = (zx + 0.5) * CHUNK_SIZE
    wz = (zz + 0.5) * CHUNK_SIZE

    d = Driver(exe=args.exe)
    print(f"[ab] launching {args.exe} (label={args.label}) ...")
    if not d.launch(wait_s=40):
        print("[ab] ERROR: driver failed to connect", file=sys.stderr)
        return 1
    print("[ab] connected")

    d.send(f"md.teleport_camera({wx:.1f}, {wz:.1f})")
    d.send(f"md.teleport_player({wx:.1f}, {wz:.1f})")
    d.send(f"md.set_camera_orbit(0.0, {PITCH}, {DIST})")
    d.send("md.set_editor_open(false)")
    print(f"[ab] settling {SETTLE_S}s at zone{ZONE} {POSE_LABEL} (wx={wx:.1f}, wz={wz:.1f}) ...")
    time.sleep(SETTLE_S)

    if args.spawn_npcs > 0:
        # md.spawn(x, z, ...) writes x/z DIRECTLY into WorldTransform with
        # NO coordinate conversion, unlike md.teleport_player(wx, wz) which
        # treats its args as ABSOLUTE Kenshi world metres and converts to
        # SceneRender's local tnoff-relative space internally
        # (lua_scenario_api_core.cpp's l_md_teleport_player vs l_md_spawn --
        # found 2026-09-08 root-causing why a spawned population showed
        # NPCs=384 then decayed to 0 within ~10s: passing the same absolute
        # wx/wz to md.spawn put NPCs in the wrong local space, far outside
        # TickOffscreenAndChunks's 800m capture-and-destroy radius from the
        # REAL (correctly-converted) player position). Read back the
        # player's own local x/z post-teleport via md.get_player_pos() and
        # spawn relative to THAT instead of the absolute wx/wz.
        px, err = d.get_number("md.get_player_pos().x")
        pz, err2 = d.get_number("md.get_player_pos().z")
        if px is None or pz is None:
            print(f"[ab] ERROR: could not read back player local pos: {err or err2}", file=sys.stderr)
            d.shutdown()
            return 1
        n_batches = (args.spawn_npcs + SPAWN_PER_BATCH - 1) // SPAWN_PER_BATCH
        print(f"[ab] spawning ~{args.spawn_npcs} NPCs near player local ({px:.1f},{pz:.1f}) ({n_batches} batches) ...")
        for i in range(n_batches):
            jx = px + ((i % 4) - 1.5) * SPAWN_SPREAD_M
            jz = pz + ((i // 4) - 1.5) * SPAWN_SPREAD_M
            ok, r = d.send(f"md.spawn('npc', {jx:.1f}, {jz:.1f}, {SPAWN_PER_BATCH})")
            if not ok:
                print(f"[ab] WARNING: spawn batch {i} failed: {r}", file=sys.stderr)
        # Liveness check (not just spawn-count) -- feedback_verify_population_
        # liveness_before_measuring: confirm the population is actually
        # present and ticking at MEASUREMENT time, not just right after
        # spawn, by polling the real [PERF] NPCs= field until it stabilizes
        # instead of trusting a fixed sleep.
        print("[ab] verifying spawned population is live (polling [PERF] NPCs=) ...")
        last_npc = -1
        stable_polls = 0
        deadline = time.time() + 30.0
        while time.time() < deadline and stable_polls < 2:
            pre = STDOUT_PATH.stat().st_size if STDOUT_PATH.exists() else 0
            time.sleep(5.5)
            text = ""
            if STDOUT_PATH.exists():
                with open(STDOUT_PATH, "r", errors="replace") as f:
                    f.seek(pre)
                    text = f.read()
            m = re.findall(r"\[PERF\]\s+\d+\s+FPS\s+\|\s+NPCs=(\d+)", text)
            cur = int(m[-1]) if m else -1
            print(f"[ab]   NPCs={cur}")
            if cur == last_npc and cur > 0:
                stable_polls += 1
            else:
                stable_polls = 0
            last_npc = cur
        if last_npc <= 0:
            print(f"[ab] ERROR: spawned population never showed up alive (last NPCs={last_npc})", file=sys.stderr)
            d.shutdown()
            return 1
        print(f"[ab] population live and stable at NPCs={last_npc}")

    pre_offset = STDOUT_PATH.stat().st_size if STDOUT_PATH.exists() else 0

    print(f"[ab] sampling window {args.window:.1f}s ...")
    d.send("md.log('AB_WINDOW_START')")
    time.sleep(args.window)
    d.send("md.log('AB_WINDOW_END')")
    time.sleep(0.3)

    window_text = ""
    if STDOUT_PATH.exists():
        with open(STDOUT_PATH, "r", errors="replace") as f:
            f.seek(pre_offset)
            window_text = f.read()

    d.shutdown()

    samples = []
    npc_counts = []
    for line in window_text.splitlines():
        m = PERF_LINE_RE.match(line.strip())
        if not m:
            continue
        fps, npcs, rest = m.groups()
        npc_counts.append(int(npcs))
        for name, avg, _mx in PASS_RE.findall(rest):
            if name == "Logic":
                samples.append(float(avg))

    if not samples:
        print("[ab] ERROR: no Logic= samples found in [PERF] lines during window", file=sys.stderr)
        print(f"[ab] raw window text ({len(window_text)} chars):\n{window_text[-2000:]}", file=sys.stderr)
        return 1

    med = median(samples)
    sd = stdev(samples, sum(samples) / len(samples))
    lo, hi = min(samples), max(samples)
    npc_med = median(npc_counts) if npc_counts else 0

    print(f"\n[ab] === {args.label} @ zone{ZONE} {POSE_LABEL} ===")
    print(f"[ab] n={len(samples)} [PERF] samples, npc_median={npc_med}")
    print(f"[ab] Logic ms  median={med:.4f}  min={lo:.4f}  max={hi:.4f}  stdev={sd:.4f}")

    out = {
        "label": args.label,
        "zone": ZONE,
        "pose": POSE_LABEL,
        "n_samples": len(samples),
        "npc_median": npc_med,
        "logic_ms_median": med,
        "logic_ms_min": lo,
        "logic_ms_max": hi,
        "logic_ms_stdev": sd,
    }
    print(f"\n[ab] RESULT_JSON {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
