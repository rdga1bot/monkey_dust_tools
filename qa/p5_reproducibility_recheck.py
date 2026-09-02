#!/usr/bin/env python3
"""
P5 §3 reproducibility re-check (task #635 follow-up, 2026-09-02): re-measure
"worst zone(24,12) horizon" with the now-fixed windowed TerrainPendingMax/
VtResidentMax counters (docs/GRANITE_M2_PROGRESS.md, "Задача #635 — перший
варіант не працював"), to see whether terrain-streaming/VT-cache state
explains the RenderTotal discrepancy between:
  - P5 baseline (docs/GRANITE_P5_BASELINE.md §3): median 15.30ms, 43 windows
  - GATE M2 recheck (docs/GRANITE_M2_PROGRESS.md §GATE M2): 3-10ms, deemed
    "not comparable" at the time because no streaming-state counter existed

Same pose/protocol as both prior runs: zone(24,12) horizon,
wx=(24+0.5)*460.8, wz=(12+0.5)*460.8, pitch=10.0, dist=22.0, no freq-lock,
whatever resolution SDL actually opens at (P5 got 1366x730, not requested
1920x1080 -- this script does not fight that, just records it).
"""
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_cmd_driver import Driver, STDOUT_PATH  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
RELEASE_EXE = REPO_ROOT / "build_release" / "game" / "monkey_dust"

CHUNK_SIZE = 460.8
ZONE = (24, 12)
PITCH, DIST = 10.0, 22.0
SETTLE_S = 8.0
DURATION_S = 215.0  # matches P5's 43x5s-window run

PERF_RE = re.compile(
    r"\[PERF\] (\d+) FPS \| NPCs=(\d+) \| "
    r"TerrainPendingMax=(\d+)\((\d+)/(\d+)fr>0\) VtResidentMax=(\d+) \| "
    r".*?RenderTotal=([\d.]+)ms\(max([\d.]+)\)"
)


def percentile(vals, p):
    if not vals:
        return float("nan")
    s = sorted(vals)
    k = (len(s) - 1) * p
    f, c = int(k), min(int(k) + 1, len(s) - 1)
    if f == c:
        return s[f]
    return s[f] + (s[c] - s[f]) * (k - f)


def main() -> int:
    if not RELEASE_EXE.exists():
        print(f"ERROR: {RELEASE_EXE} missing", file=sys.stderr)
        return 1

    zx, zz = ZONE
    wx = (zx + 0.5) * CHUNK_SIZE
    wz = (zz + 0.5) * CHUNK_SIZE

    d = Driver(exe=str(RELEASE_EXE))
    print(f"[p5-recheck] launching {RELEASE_EXE} ...")
    if not d.launch(wait_s=40):
        print("[p5-recheck] ERROR: driver failed to connect", file=sys.stderr)
        return 1
    print("[p5-recheck] connected")

    # Mark the log offset AFTER connect (startup splash/init spam precedes
    # it) so we only parse [PERF] lines from this run's actual measurement
    # window, not any residual content from a previous launch reusing the
    # same STDOUT_PATH.
    start_offset = STDOUT_PATH.stat().st_size if STDOUT_PATH.exists() else 0

    d.send(f"md.teleport_camera({wx:.1f}, {wz:.1f})")
    d.send(f"md.teleport_player({wx:.1f}, {wz:.1f})")
    d.send(f"md.set_camera_orbit(0.0, {PITCH}, {DIST})")
    print(f"[p5-recheck] settling {SETTLE_S}s at zone{ZONE} horizon "
          f"(wx={wx:.1f}, wz={wz:.1f}) ...")
    time.sleep(SETTLE_S)

    print(f"[p5-recheck] measuring for {DURATION_S:.0f}s ...")
    time.sleep(DURATION_S)
    d.shutdown()
    print("[p5-recheck] done, parsing log ...")

    with open(STDOUT_PATH, "r", errors="replace") as f:
        f.seek(start_offset)
        lines = f.readlines()

    render_avgs, render_maxes, fps_vals = [], [], []
    tpm_vals, tpm_frac_vals = [], []
    vtr_vals = []
    for line in lines:
        m = PERF_RE.search(line)
        if not m:
            continue
        fps, npcs, tpm, tpm_nz, tpm_n, vtr, rt_avg, rt_max = m.groups()
        fps_vals.append(int(fps))
        tpm_vals.append(int(tpm))
        tpm_frac_vals.append(int(tpm_nz) / max(int(tpm_n), 1))
        vtr_vals.append(int(vtr))
        render_avgs.append(float(rt_avg))
        render_maxes.append(float(rt_max))

    if not render_avgs:
        print("[p5-recheck] ERROR: no [PERF] lines matched -- check regex "
              "against actual log format", file=sys.stderr)
        return 1

    n = len(render_avgs)
    print(f"\n[p5-recheck] N={n} report windows")
    print(f"[p5-recheck] RenderTotal avg p50/p95/p99 (ms) = "
          f"{percentile(render_avgs,0.50):.2f} / "
          f"{percentile(render_avgs,0.95):.2f} / "
          f"{percentile(render_avgs,0.99):.2f}")
    print(f"[p5-recheck] RenderTotal max p50/p95/p99 (ms) = "
          f"{percentile(render_maxes,0.50):.2f} / "
          f"{percentile(render_maxes,0.95):.2f} / "
          f"{percentile(render_maxes,0.99):.2f}")
    print(f"[p5-recheck] FPS p50 = {percentile(fps_vals,0.50):.0f}")
    print(f"[p5-recheck] TerrainPendingMax: windows with any pending>0 = "
          f"{sum(1 for x in tpm_vals if x > 0)}/{n}, max seen = {max(tpm_vals)}")
    print(f"[p5-recheck] VtResidentMax: max seen across all windows = "
          f"{max(vtr_vals)}")
    print(f"\n[p5-recheck] P5 baseline (docs/GRANITE_P5_BASELINE.md §3): "
          f"43 windows, RenderTotal avg p50/p95/p99 = 15.30/16.95/17.16ms")
    print(f"[p5-recheck] GATE M2 recheck (uninstrumented, deemed "
          f"non-comparable): RenderTotal 3-10ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
