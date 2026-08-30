#!/usr/bin/env python3
"""
perf_gate.py -- PERF_MASTER_PROMPT.md FAZA 4 item 3: regression gate over
a small set of reference world points. Measures GPU ms at each point via
the same release + MD_PERF_TEST_HOOKS live-driving mechanism as
perf_phase4_world_scan.py, compares against a stored JSON baseline, and
exits non-zero if any point's median regressed by more than --threshold
percent.

Reference points: the 5 worst + 2 control points from the original
Faza 1 scan (task #413/#414, CLAUDE_STATE.md) -- the same convention
PERF_MASTER_PROMPT.md's Faza 2 uses ("5 найгірших точок за p95 і 2
медіанні як контроль"). If tools/qa/baselines/perf_gate_points.json
doesn't exist, this falls back to the worst/best points found in the
most recent tools/qa/reports/phase4_world_scan.csv (2026-08-29 run) --
documented fallback, not a silent guess.

USAGE:
  python3 tools/qa/perf_gate.py --update-baseline   # record current numbers as the baseline
  python3 tools/qa/perf_gate.py                     # check current numbers against baseline
  python3 tools/qa/perf_gate.py --threshold 15       # override default 10% regression gate
"""
import argparse
import csv
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_cmd_driver import Driver, median  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
RELEASE_EXE = REPO_ROOT / "build_release" / "game" / "monkey_dust"
BASELINE_PATH = REPO_ROOT / "tools" / "qa" / "baselines" / "perf_gate_baseline.json"
POINTS_PATH = REPO_ROOT / "tools" / "qa" / "baselines" / "perf_gate_points.json"
PHASE4_CSV = REPO_ROOT / "tools" / "qa" / "reports" / "phase4_world_scan.csv"

CHUNK_SIZE = 460.8
SETTLE_S = 6.0
SAMPLES_PER_POINT = 20
DEFAULT_THRESHOLD_PCT = 10.0


def load_reference_points():
    if POINTS_PATH.exists():
        return json.loads(POINTS_PATH.read_text())
    if not PHASE4_CSV.exists():
        print(f"[perf_gate] ERROR: no {POINTS_PATH} and no {PHASE4_CSV} to fall back to", file=sys.stderr)
        sys.exit(1)
    rows = list(csv.DictReader(open(PHASE4_CSV)))
    rows.sort(key=lambda r: float(r["gpu_ms_median"]))
    picked = rows[-5:] + rows[len(rows) // 2 - 1: len(rows) // 2 + 1]
    pts = [{"zone_x": int(r["zone_x"]), "zone_z": int(r["zone_z"]), "pose": r["pose"],
            "pitch": 40.0 if r["pose"] == "gameplay" else 10.0} for r in picked]
    print(f"[perf_gate] {POINTS_PATH} not found -- falling back to worst-5+control-2 "
          f"from {PHASE4_CSV.name} ({len(pts)} points)")
    return pts


def measure(points, exe):
    d = Driver(exe=exe)
    if not d.launch(wait_s=40):
        print("[perf_gate] ERROR: driver failed to connect", file=sys.stderr)
        sys.exit(1)
    d.send("md.set_gpu_sync_timing(true)")
    results = {}
    try:
        for p in points:
            wx = (p["zone_x"] + 0.5) * CHUNK_SIZE
            wz = (p["zone_z"] + 0.5) * CHUNK_SIZE
            key = f"{p['zone_x']},{p['zone_z']},{p['pose']}"
            d.send(f"md.teleport_camera({wx:.1f}, {wz:.1f})")
            d.send(f"md.teleport_player({wx:.1f}, {wz:.1f})")
            d.send(f"md.set_camera_orbit(0.0, {p['pitch']}, 22.0)")
            time.sleep(SETTLE_S)
            samples = []
            for _ in range(SAMPLES_PER_POINT):
                val, _err = d.get_number("md.get_gpu_ms()")
                if val is not None and val > 0:
                    samples.append(val)
                time.sleep(0.03)
            results[key] = median(samples) if samples else 0.0
            print(f"[perf_gate] {key}: median={results[key]:.2f}ms (n={len(samples)})")
    finally:
        d.shutdown()
    return results


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD_PCT)
    ap.add_argument("--exe", default=str(RELEASE_EXE))
    args = ap.parse_args()

    points = load_reference_points()
    current = measure(points, args.exe)

    if args.update_baseline:
        BASELINE_PATH.parent.mkdir(parents=True, exist_ok=True)
        BASELINE_PATH.write_text(json.dumps({"points": points, "metrics": current}, indent=2))
        print(f"[perf_gate] baseline updated: {BASELINE_PATH}")
        return 0

    if not BASELINE_PATH.exists():
        print(f"[perf_gate] WARN: no baseline at {BASELINE_PATH} -- run --update-baseline first")
        return 0

    baseline = json.loads(BASELINE_PATH.read_text())["metrics"]
    failed = []
    for key, cur_ms in current.items():
        base_ms = baseline.get(key)
        if base_ms is None or base_ms <= 0:
            print(f"[perf_gate] {key}: no baseline value, skipping")
            continue
        pct = (cur_ms - base_ms) / base_ms * 100.0
        status = "FAIL" if pct > args.threshold else "ok"
        print(f"[perf_gate] {key}: baseline={base_ms:.2f}ms current={cur_ms:.2f}ms delta={pct:+.1f}% [{status}]")
        if status == "FAIL":
            failed.append(key)

    if failed:
        print(f"[perf_gate] REGRESSION: {len(failed)} point(s) exceeded +{args.threshold}%: {failed}")
        return 1
    print("[perf_gate] PASS -- no point regressed beyond threshold")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
