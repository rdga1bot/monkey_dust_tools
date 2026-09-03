#!/usr/bin/env python3
"""
perf_ab_jobsystem_workers.py -- single-point A/B measurement for the
DEBT-JOBS worker_threads=2 change (game/data/tasks.cfg), requested before
any Granite work so a later Granite-attributable change isn't misattributed
to this JobSystem config change.

Point: zone(24,12) "horizon" pose -- the documented worst point in
tools/qa/baselines/perf_gate_baseline.json ("24,12,horizon": 13.518 gpu_ms).
Same CHUNK_SIZE/pose convention as tools/qa/perf_phase4_world_scan.py.

Caller is responsible for rebuilding build_release/game/monkey_dust with
the desired game/data/tasks.cfg worker_threads value BEFORE each side of
the A/B (this script does not edit source or rebuild -- see the shell
wrapper this is invoked from). Each invocation is one side of the A/B;
run it twice, once per worker_threads value, and diff the two reports.

USAGE:
  python3 tools/qa/perf_ab_jobsystem_workers.py --label workers2
  python3 tools/qa/perf_ab_jobsystem_workers.py --label workers0auto
"""
import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_cmd_driver import Driver, median, stdev  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
RELEASE_EXE = REPO_ROOT / "build_release" / "game" / "monkey_dust"
STDOUT_PATH = REPO_ROOT / "tmp_" / "game_cmd" / "game_stdout.log"

CHUNK_SIZE = 460.8
ZONE = (24, 12)
POSE_LABEL, PITCH, DIST = "horizon", 10.0, 22.0
SETTLE_S = 8.0
SAMPLES = 40
SAMPLE_INTERVAL_S = 0.03


def percentile(xs, p):
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return 0.0
    idx = max(0, min(n - 1, int(round(p * (n - 1)))))
    return s[idx]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", required=True, help="tag for this A/B side, e.g. workers2")
    ap.add_argument("--exe", default=str(RELEASE_EXE))
    args = ap.parse_args()

    zx, zz = ZONE
    wx = (zx + 0.5) * CHUNK_SIZE
    wz = (zz + 0.5) * CHUNK_SIZE

    d = Driver(exe=args.exe)
    print(f"[ab] launching {args.exe} (label={args.label}) ...")
    if not d.launch(wait_s=40):
        print("[ab] ERROR: driver failed to connect", file=sys.stderr)
        return 1
    print("[ab] connected")

    ok, r = d.send("md.set_gpu_sync_timing(true)")
    if not ok:
        print(f"[ab] ERROR: set_gpu_sync_timing failed: {r}", file=sys.stderr)
        d.shutdown()
        return 1

    d.send(f"md.teleport_camera({wx:.1f}, {wz:.1f})")
    d.send(f"md.teleport_player({wx:.1f}, {wz:.1f})")
    d.send(f"md.set_camera_orbit(0.0, {PITCH}, {DIST})")
    print(f"[ab] settling {SETTLE_S}s at zone{ZONE} {POSE_LABEL} (wx={wx:.1f}, wz={wz:.1f}) ...")
    time.sleep(SETTLE_S)

    # mark stdout log offset -- only count [FRAMESPIKE]/[PERF] lines that
    # appear from here on, not anything from teleport/settle.
    pre_offset = STDOUT_PATH.stat().st_size if STDOUT_PATH.exists() else 0

    samples = []
    t_start = time.monotonic()
    for _ in range(SAMPLES):
        val, err = d.get_number("md.get_gpu_ms()")
        if val is not None and val > 0:
            samples.append(val)
        time.sleep(SAMPLE_INTERVAL_S)
    t_elapsed = time.monotonic() - t_start

    d.send("md.log('AB_WINDOW_END')")
    time.sleep(0.2)

    framespike_lines = []
    perf_lines = []
    if STDOUT_PATH.exists():
        with open(STDOUT_PATH, "r", errors="replace") as f:
            f.seek(pre_offset)
            window_text = f.read()
        framespike_lines = [ln for ln in window_text.splitlines() if "[FRAMESPIKE]" in ln]
        perf_lines = [ln for ln in window_text.splitlines() if "[PERF]" in ln]

    d.shutdown()

    if not samples:
        print("[ab] ERROR: no valid gpu_ms samples collected", file=sys.stderr)
        return 1

    med = median(samples)
    p95 = percentile(samples, 0.95)
    sd = stdev(samples, sum(samples) / len(samples))
    lo, hi = min(samples), max(samples)

    print(f"\n[ab] === {args.label} @ zone{ZONE} {POSE_LABEL} ===")
    print(f"[ab] n={len(samples)} window={t_elapsed:.1f}s")
    print(f"[ab] gpu_ms  median={med:.3f}  p95={p95:.3f}  min={lo:.3f}  max={hi:.3f}  stdev={sd:.3f}")
    print(f"[ab] frame_ms  ~= gpu_ms (same [PERF] RenderTotal source) -- see [PERF] lines below")
    print(f"[ab] FRAMESPIKE count in sampling window: {len(framespike_lines)}")
    for ln in framespike_lines[:10]:
        print(f"[ab]   {ln.strip()}")
    if len(framespike_lines) > 10:
        print(f"[ab]   ... ({len(framespike_lines) - 10} more)")
    if perf_lines:
        print(f"[ab] last [PERF] line in window: {perf_lines[-1].strip()}")
    else:
        print("[ab] no [PERF] aggregate line fired during the sampling window "
              "(window shorter than the 5s report interval -- not an error)")

    out = {
        "label": args.label,
        "zone": ZONE,
        "pose": POSE_LABEL,
        "n_samples": len(samples),
        "gpu_ms_median": med,
        "gpu_ms_p95": p95,
        "gpu_ms_min": lo,
        "gpu_ms_max": hi,
        "gpu_ms_stdev": sd,
        "framespike_count": len(framespike_lines),
    }
    print(f"\n[ab] RESULT_JSON {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
