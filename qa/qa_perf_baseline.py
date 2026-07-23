#!/usr/bin/env python3
"""
qa_perf_baseline.py — Perf regression tracking for monkey_dust QA

Parses FrameStats' periodic "[PERF] N FPS | NPCs=N | Name=avgms(maxms) ..."
lines (engine/include/monkey_dust/platform/frame_stats.h, printed every 5s
to stderr, saved by qa_run.sh to <capture>/game_stdout.log) and compares
against a stored baseline. Same intent as visual regression (qa_regression.py)
but for CPU frame-time budgets instead of pixels.

Usage:
  python3 tools/qa/qa_perf_baseline.py --check                    # latest capture vs baseline
  python3 tools/qa/qa_perf_baseline.py --check --capture 20260722_235959
  python3 tools/qa/qa_perf_baseline.py --update-baseline           # save latest capture as new baseline
  python3 tools/qa/qa_perf_baseline.py --update-baseline --capture 20260722_235959
  python3 tools/qa/qa_perf_baseline.py --check --threshold 15      # override default 10% regression gate
"""

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Optional

REPO_ROOT    = Path(__file__).resolve().parents[2]
CAPTURES_DIR = REPO_ROOT / "tools" / "qa" / "captures"
BASELINE_PATH = REPO_ROOT / "tools" / "qa" / "baselines" / "perf_baseline.json"

DEFAULT_THRESHOLD_PCT = 10.0

# "[PERF] 58 FPS | NPCs=12 | Logic=0.1ms(max0.9) RenderTotal=7.3ms(max33.1) ..."
PERF_LINE_RE = re.compile(r"^\[PERF\]\s+(\d+)\s+FPS\s+\|\s+NPCs=(\d+)\s+\|\s+(.*)$")
PASS_RE      = re.compile(r"(\w+)=([\d.]+)ms\(max([\d.]+)\)")


def latest_capture() -> Optional[str]:
    if not CAPTURES_DIR.exists():
        return None
    entries = sorted(p.name for p in CAPTURES_DIR.iterdir() if p.is_dir())
    return entries[-1] if entries else None


def parse_perf_lines(log_path: Path) -> list[dict]:
    """Every [PERF] line in the log → {fps, npcs, passes: {name: avg_ms}}."""
    if not log_path.exists():
        return []
    samples = []
    for line in log_path.read_text(errors="replace").splitlines():
        m = PERF_LINE_RE.match(line.strip())
        if not m:
            continue
        fps, npcs, rest = m.groups()
        passes = {name: float(avg) for name, avg, _max in PASS_RE.findall(rest)}
        samples.append({"fps": float(fps), "npcs": int(npcs), "passes": passes})
    return samples


def aggregate(samples: list[dict]) -> Optional[dict]:
    """Average across all report intervals — skips the FIRST sample (startup
    warmup: shader/pipeline compile + asset load still bleeding into the
    first 5s window, per gpu_device.cpp's own documented ~4.5s pipeline-
    compile-stall precedent). Returns None if too few samples to be
    meaningful (need at least 2 post-warmup intervals)."""
    usable = samples[1:] if len(samples) > 1 else samples
    if not usable:
        return None
    metrics = {"fps": sum(s["fps"] for s in usable) / len(usable)}
    pass_names = set()
    for s in usable:
        pass_names.update(s["passes"].keys())
    for name in pass_names:
        vals = [s["passes"][name] for s in usable if name in s["passes"]]
        if vals:
            metrics[name] = sum(vals) / len(vals)
    return metrics


def load_baseline() -> Optional[dict]:
    if not BASELINE_PATH.exists():
        return None
    return json.loads(BASELINE_PATH.read_text())


def save_baseline(capture_id: str, metrics: dict) -> None:
    BASELINE_PATH.parent.mkdir(parents=True, exist_ok=True)
    BASELINE_PATH.write_text(json.dumps(
        {"captured_from": capture_id, "metrics": metrics}, indent=2))


def check(capture_id: str, threshold_pct: float) -> int:
    log_path = CAPTURES_DIR / capture_id / "game_stdout.log"
    samples = parse_perf_lines(log_path)
    if not samples:
        print(f"[qa_perf] WARN: no [PERF] lines found in {log_path} "
              f"(run may be too short — FrameStats reports every 5s — or "
              f"game_stdout.log missing on an older capture predating this "
              f"script)")
        return 0  # not a regression signal — just nothing to check
    current = aggregate(samples)
    if current is None:
        print(f"[qa_perf] WARN: not enough post-warmup [PERF] samples to aggregate")
        return 0

    baseline = load_baseline()
    if baseline is None:
        print(f"[qa_perf] No baseline yet ({BASELINE_PATH}) — nothing to compare against. "
              f"Run with --update-baseline once a capture looks healthy.")
        return 0

    base_metrics = baseline["metrics"]
    fail = False
    print(f"[qa_perf] Comparing {capture_id} against baseline "
          f"({baseline.get('captured_from', '?')}), threshold={threshold_pct}%")
    for name, base_val in base_metrics.items():
        cur_val = current.get(name)
        if cur_val is None:
            print(f"  ? {name}: missing in current capture (skipped)")
            continue
        if name == "fps":
            # FPS: lower is worse.
            delta_pct = (base_val - cur_val) / base_val * 100.0 if base_val else 0.0
            regressed = delta_pct > threshold_pct
        else:
            # ms passes: higher is worse.
            delta_pct = (cur_val - base_val) / base_val * 100.0 if base_val else 0.0
            regressed = delta_pct > threshold_pct
        marker = "✗ REGRESSION" if regressed else "✓"
        print(f"  {marker} {name}: baseline={base_val:.2f} current={cur_val:.2f} "
              f"delta={delta_pct:+.1f}%")
        if regressed:
            fail = True

    if fail:
        print(f"[qa_perf] FAIL: one or more metrics regressed beyond {threshold_pct}%")
        return 1
    print("[qa_perf] PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="monkey_dust perf regression check")
    ap.add_argument("--check", action="store_true", help="compare capture against baseline")
    ap.add_argument("--update-baseline", action="store_true", help="save capture as new baseline")
    ap.add_argument("--capture", help="capture ID (default: latest)")
    ap.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD_PCT,
                     help=f"regression threshold in %% (default {DEFAULT_THRESHOLD_PCT})")
    args = ap.parse_args()

    capture_id = args.capture or latest_capture()
    if not capture_id:
        print("[qa_perf] No captures found under tools/qa/captures/", file=sys.stderr)
        return 1

    if args.update_baseline:
        log_path = CAPTURES_DIR / capture_id / "game_stdout.log"
        samples = parse_perf_lines(log_path)
        metrics = aggregate(samples)
        if metrics is None:
            print(f"[qa_perf] No usable [PERF] samples in {log_path} — cannot update baseline",
                  file=sys.stderr)
            return 1
        save_baseline(capture_id, metrics)
        print(f"[qa_perf] Baseline updated from {capture_id}: {metrics}")
        return 0

    # Default action (also covers --check): compare.
    return check(capture_id, args.threshold)


if __name__ == "__main__":
    sys.exit(main())
