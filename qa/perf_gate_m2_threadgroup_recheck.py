#!/usr/bin/env python3
"""
GATE M2 recheck (2026-09-02, after Крок 3): re-measure gt_cur_freq_mhz
under load at worst zone(24,12) horizon pose with ThreadGroup(1,0)
running concurrently, for direct comparison against:
  - P5 baseline (docs/GRANITE_P5_BASELINE.md): 76.6% floor-time, avg 308.0MHz
  - M2 Крок 4's own earlier ThreadGroup(1,0) run
    (docs/GRANITE_M2_PROGRESS.md): 90.6% floor-time, avg 302.2MHz

Крок 3 (this session) touched ONLY C++ type declarations (opaque-handle
aliases) with zero proven runtime behavior change (verified via ASan+
ctest identical across 5 groups) -- so this re-run's purpose is to
confirm the GATE condition still holds, not to discover anything new.

Same CHUNK_SIZE/ZONE/pose convention as
tools/qa/perf_ab_jobsystem_workers.py.
"""
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_cmd_driver import Driver  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
RELEASE_EXE = REPO_ROOT / "build_release" / "game" / "monkey_dust"
FREQ_PATH = Path("/sys/class/drm/card1/gt_cur_freq_mhz")
OUT_DIR = REPO_ROOT / "docs" / "audit" / "raw"

CHUNK_SIZE = 460.8
ZONE = (24, 12)
PITCH, DIST = 10.0, 22.0
SETTLE_S = 8.0
DURATION_S = 215.0
FREQ_SAMPLE_INTERVAL_S = 0.2

PROBE_BIN = Path("/tmp/md_gate_tg_build/m2_threadgroup_probe")


def main() -> int:
    if not RELEASE_EXE.exists():
        print(f"ERROR: {RELEASE_EXE} missing", file=sys.stderr)
        return 1
    if not PROBE_BIN.exists():
        print(f"ERROR: {PROBE_BIN} missing", file=sys.stderr)
        return 1

    zx, zz = ZONE
    wx = (zx + 0.5) * CHUNK_SIZE
    wz = (zz + 0.5) * CHUNK_SIZE

    d = Driver(exe=str(RELEASE_EXE))
    print(f"[gate-recheck] launching {RELEASE_EXE} ...")
    if not d.launch(wait_s=40):
        print("[gate-recheck] ERROR: driver failed to connect", file=sys.stderr)
        return 1
    print("[gate-recheck] connected")

    d.send(f"md.teleport_camera({wx:.1f}, {wz:.1f})")
    d.send(f"md.teleport_player({wx:.1f}, {wz:.1f})")
    d.send(f"md.set_camera_orbit(0.0, {PITCH}, {DIST})")
    print(f"[gate-recheck] settling {SETTLE_S}s at zone{ZONE} horizon (wx={wx:.1f}, wz={wz:.1f}) ...")
    time.sleep(SETTLE_S)

    print(f"[gate-recheck] starting ThreadGroup(1,0) probe for {DURATION_S:.0f}s ...")
    probe = subprocess.Popen([str(PROBE_BIN), str(int(DURATION_S))])

    freq_log_path = OUT_DIR / "m2_gate_recheck_freq_worstzone_plus_threadgroup.log"
    freq_samples = []
    t_start = time.monotonic()
    with open(freq_log_path, "w") as flog:
        while time.monotonic() - t_start < DURATION_S:
            try:
                mhz = int(FREQ_PATH.read_text().strip())
            except Exception:
                mhz = -1
            ts = time.time()
            flog.write(f"{ts:.6f} {mhz}\n")
            flog.flush()
            if mhz >= 0:
                freq_samples.append(mhz)
            time.sleep(FREQ_SAMPLE_INTERVAL_S)

    probe.wait(timeout=30)
    d.shutdown()

    if not freq_samples:
        print("[gate-recheck] ERROR: no freq samples collected", file=sys.stderr)
        return 1

    at_floor = sum(1 for x in freq_samples if x <= 300)
    pct_floor = 100.0 * at_floor / len(freq_samples)
    avg = sum(freq_samples) / len(freq_samples)
    print(f"\n[gate-recheck] N={len(freq_samples)} freq min/avg/max="
          f"{min(freq_samples)}/{avg:.1f}/{max(freq_samples)} MHz")
    print(f"[gate-recheck] %@300MHz-floor = {pct_floor:.1f}%")
    print(f"[gate-recheck] P5 baseline (no ThreadGroup):        76.6% floor, avg 308.0MHz")
    print(f"[gate-recheck] Крок 4 ThreadGroup(1,0) (this session): 90.6% floor, avg 302.2MHz")
    print(f"[gate-recheck] log: {freq_log_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
