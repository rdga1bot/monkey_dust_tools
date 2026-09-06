#!/usr/bin/env python3
"""
render_profiles_tier_measure.py -- CLAUDE_CODE_RENDER_PROFILES_SPEC.md §1:
measure gpu_ms/frame_ms (p50/p95/p99) for a forced RenderTier at a given
zone, with gt_cur_freq_mhz logged in parallel, uncapped present mode.

Combines two existing precedents rather than inventing a new protocol:
  - pose/settle protocol from tools/qa/p5_reproducibility_recheck.py
    (teleport_camera + teleport_player + set_camera_orbit, 8s settle)
  - parallel gt_cur_freq_mhz sampling from
    tools/qa/perf_gate_m2_threadgroup_recheck.py (card1, 0.2s interval)

Uses md.get_gpu_ms()/md.get_frame_ms() (serialized-fence GPU timing,
test-only -- see l_md_set_gpu_sync_timing's doc comment in
lua_scenario_api_misc.cpp) polled 40x over 5s windows (200s total),
NOT the [PERF] log's RenderTotal (that measures CPU-side FrameStats
timing, not the same thing).

Forces the render tier via data/render_settings.json BEFORE launch
(RenderTierSystem::Detect's JSON-override path, priority 1 -- no code
change). Also forces passes.ssao/passes.deferred_ambient to true for
the duration of the run, since RenderQualityConfig::ApplyTierPreset
does NOT touch pass enable/disable -- that's a separate, independent
override in the same JSON, and leaving it at the repo's current
Forward-tuned ssao=false would silently measure a tier that never
actually runs SSAO regardless of render_tier. (deferred_ambient is
tier-gated directly in GBufferSystem::enabled_ regardless of this key
-- see docs/AI_DEV_PROTOCOL.md V7 -- forcing it true here just keeps
this script's intent explicit now that the key is real.)
Restores the original file content on exit (including on error).

Requires a MONKEY_DUST_EDITOR=OFF, MD_PERF_TEST_HOOKS=ON build (this
repo's usual build/ has EDITOR=ON; use build_release/ configured with
-DMONKEY_DUST_EDITOR=OFF -DMD_PERF_TEST_HOOKS=ON).

Usage:
    python3 tools/qa/render_profiles_tier_measure.py \\
        --tier Deferred_Low --zone worst
    python3 tools/qa/render_profiles_tier_measure.py \\
        --tier Deferred_High --zone canyon --exe build_release/game/monkey_dust
"""
import argparse
import json
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_cmd_driver import Driver  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
RENDER_SETTINGS_PATH = REPO_ROOT / "data" / "render_settings.json"
FREQ_PATH = Path("/sys/class/drm/card1/gt_cur_freq_mhz")
OUT_DIR = REPO_ROOT / "docs" / "audit" / "raw"

CHUNK_SIZE = 460.8
ZONES = {
    "worst":  (24, 12),   # worst zone(24,12) horizon -- established P5 pose
    "canyon": (6, 42),    # canyon biome, far from worst zone -- kenshi_bowl_2
                            # turned out to be a biome_table.txt shading-param
                            # row, not a teleportable zone (not in the 11
                            # WorldRegistry::kBiomeNames grid categories, not
                            # in game/data/biome_map.txt at all) -- see
                            # docs/RENDER_PROFILES_SPEC.md §1 for the record.
}
PITCH, DIST = 10.0, 22.0
SETTLE_S = 8.0
N_SAMPLES = 40
SAMPLE_INTERVAL_S = 5.0
FREQ_SAMPLE_INTERVAL_S = 0.2


def percentile(vals, p):
    if not vals:
        return float("nan")
    s = sorted(vals)
    k = (len(s) - 1) * p
    f, c = int(k), min(int(k) + 1, len(s) - 1)
    if f == c:
        return s[f]
    return s[f] + (s[c] - s[f]) * (k - f)


def write_render_settings(tier):
    cfg = {
        "render_tier": tier,
        "passes": {
            "tiles_2d": True, "npc_sprites": True, "world_3d": True,
            "overlay": True,
            "ssao": True, "deferred_ambient": True, "motion_blur": False,
            "cas_sharpening": True, "evsm_shadow": True,
        },
    }
    RENDER_SETTINGS_PATH.write_text(json.dumps(cfg, indent=2))


def freq_logger(stop_event, samples, log_path):
    with open(log_path, "w") as flog:
        while not stop_event.is_set():
            try:
                mhz = int(FREQ_PATH.read_text().strip())
            except Exception:
                mhz = -1
            ts = time.time()
            flog.write(f"{ts:.6f} {mhz}\n")
            flog.flush()
            if mhz >= 0:
                samples.append(mhz)
            time.sleep(FREQ_SAMPLE_INTERVAL_S)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tier", required=True,
                     choices=["Forward", "Deferred_Low", "Deferred_Med", "Deferred_High"])
    ap.add_argument("--zone", required=True, choices=list(ZONES.keys()))
    ap.add_argument("--exe", default=str(REPO_ROOT / "build_release" / "game" / "monkey_dust"))
    args = ap.parse_args()

    exe_path = Path(args.exe)
    if not exe_path.exists():
        print(f"ERROR: {exe_path} missing", file=sys.stderr)
        return 1

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    original_settings = RENDER_SETTINGS_PATH.read_text() if RENDER_SETTINGS_PATH.exists() else None

    zx, zz = ZONES[args.zone]
    wx = (zx + 0.5) * CHUNK_SIZE
    wz = (zz + 0.5) * CHUNK_SIZE

    d = None
    try:
        write_render_settings(args.tier)
        print(f"[measure] render_settings.json -> render_tier={args.tier}, "
              f"ssao=true, deferred_ambient=true")

        d = Driver(exe=str(exe_path))
        print(f"[measure] launching {exe_path} ...")
        if not d.launch(wait_s=40):
            print("[measure] ERROR: driver failed to connect", file=sys.stderr)
            return 1
        print("[measure] connected")

        # Verify the tier override actually took effect (independent-variable
        # check BEFORE trusting any downstream measurement -- see
        # docs/AI_DEV_PROTOCOL.md B.1, the SetWindowSize no-op precedent this
        # is deliberately guarding against). RenderTierSystem::Detect() logs
        # "RenderTier: JSON override -> <tier>" on the priority-1 path -- grep
        # the driver's own stdout capture for it rather than trusting the
        # JSON write alone.
        from game_cmd_driver import STDOUT_PATH  # noqa: E402
        stdout_text = STDOUT_PATH.read_text(errors="replace") if STDOUT_PATH.exists() else ""
        if f"RenderTier: JSON override" in stdout_text:
            for line in stdout_text.splitlines():
                if "RenderTier: JSON override" in line:
                    print(f"[measure] tier confirmed from log: {line.strip()}")
                    if args.tier not in line:
                        print(f"[measure] ERROR: requested tier {args.tier} not in log line "
                              f"-- override did not take effect as expected", file=sys.stderr)
                        return 1
                    break
        else:
            print("[measure] ERROR: 'RenderTier: JSON override' not found in stdout -- "
                  "tier override may not have applied, refusing to trust measurement",
                  file=sys.stderr)
            return 1

        d.send(f"md.teleport_camera({wx:.1f}, {wz:.1f})")
        d.send(f"md.teleport_player({wx:.1f}, {wz:.1f})")
        d.send(f"md.set_camera_orbit(0.0, {PITCH}, {DIST})")
        d.send("md.set_vsync(-1)")  # uncapped present (IMMEDIATE mode)
        print(f"[measure] settling {SETTLE_S}s at zone{(zx, zz)} horizon "
              f"(wx={wx:.1f}, wz={wz:.1f}) ...")
        time.sleep(SETTLE_S)

        d.send("md.set_gpu_sync_timing(true)")

        freq_samples = []
        stop_event = threading.Event()
        freq_log_path = OUT_DIR / f"render_profiles_freq_{args.tier}_{args.zone}.log"
        ft = threading.Thread(target=freq_logger, args=(stop_event, freq_samples, freq_log_path))
        ft.start()

        gpu_ms_vals, frame_ms_vals = [], []
        print(f"[measure] sampling {N_SAMPLES}x{SAMPLE_INTERVAL_S:.0f}s "
              f"({N_SAMPLES * SAMPLE_INTERVAL_S:.0f}s total) ...")
        for i in range(N_SAMPLES):
            time.sleep(SAMPLE_INTERVAL_S)
            gpu_ms, gerr = d.get_number("md.get_gpu_ms()")
            frame_ms, ferr = d.get_number("md.get_frame_ms()")
            if gpu_ms is not None:
                gpu_ms_vals.append(gpu_ms)
            if frame_ms is not None:
                frame_ms_vals.append(frame_ms)
            if (i + 1) % 10 == 0:
                print(f"[measure]   {i+1}/{N_SAMPLES} samples "
                      f"(last gpu_ms={gpu_ms}, frame_ms={frame_ms})")

        stop_event.set()
        ft.join(timeout=5)

        d.send("md.set_gpu_sync_timing(false)")
        d.shutdown()
        d = None

        if not gpu_ms_vals:
            print("[measure] ERROR: no gpu_ms samples collected -- "
                  "md.get_gpu_ms() may not exist in this build", file=sys.stderr)
            return 1

        n = len(gpu_ms_vals)
        print(f"\n[measure] === {args.tier} @ zone{(zx, zz)} ({args.zone}) ===")
        print(f"[measure] N={n} samples")
        print(f"[measure] gpu_ms   p50/p95/p99 = "
              f"{percentile(gpu_ms_vals, 0.50):.2f} / "
              f"{percentile(gpu_ms_vals, 0.95):.2f} / "
              f"{percentile(gpu_ms_vals, 0.99):.2f}")
        if frame_ms_vals:
            print(f"[measure] frame_ms p50/p95/p99 = "
                  f"{percentile(frame_ms_vals, 0.50):.2f} / "
                  f"{percentile(frame_ms_vals, 0.95):.2f} / "
                  f"{percentile(frame_ms_vals, 0.99):.2f}")
        if freq_samples:
            at_floor = sum(1 for x in freq_samples if x <= 300)
            pct_floor = 100.0 * at_floor / len(freq_samples)
            avg = sum(freq_samples) / len(freq_samples)
            print(f"[measure] gt_cur_freq_mhz N={len(freq_samples)} "
                  f"min/avg/max={min(freq_samples)}/{avg:.1f}/{max(freq_samples)} MHz, "
                  f"%@300MHz-floor={pct_floor:.1f}%")
        print(f"[measure] freq log: {freq_log_path}")
        return 0
    finally:
        if d is not None:
            try:
                d.shutdown()
            except Exception:
                pass
        if original_settings is not None:
            RENDER_SETTINGS_PATH.write_text(original_settings)
            print(f"[measure] restored {RENDER_SETTINGS_PATH} to original content")


if __name__ == "__main__":
    raise SystemExit(main())
