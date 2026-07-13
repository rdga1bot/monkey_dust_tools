#!/usr/bin/env python3
"""
editor_screenshot_compare.py — RMSE comparison for editor scenario screenshots
(Etap 4, AUTONOMY_SYSTEM_PROMPT_v2.md).

Distinct from qa_regression.py's diff_images() (changed-pixel-percentage
metric, used for the game's own baselines/ tree) — the plan asks specifically
for RMSE. Baselines are stored PER-BACKEND (editor_baselines/hw/,
editor_baselines/lavapipe/) — RMSE between different backends is never
compared, only same-backend-vs-itself, since GPU driver/rasterizer
differences are expected and not a regression signal.

Usage:
  python3 tools/qa/editor_screenshot_compare.py --save NAME SCREENSHOT.png [--backend hw]
  python3 tools/qa/editor_screenshot_compare.py --compare NAME SCREENSHOT.png [--backend hw]
"""
import argparse
import sys
import shutil
from pathlib import Path

from PIL import Image
import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
BASELINE_ROOT = REPO_ROOT / "tools" / "qa" / "editor_baselines"


def rmse(img_a: Image.Image, img_b: Image.Image) -> float:
    a = img_a.convert("RGB")
    b = img_b.convert("RGB")
    if a.size != b.size:
        b = b.resize(a.size, Image.LANCZOS)
    na = np.array(a, dtype=np.float64)
    nb = np.array(b, dtype=np.float64)
    return float(np.sqrt(np.mean((na - nb) ** 2)))


def cmd_save(name: str, screenshot: Path, backend: str) -> int:
    if not screenshot.exists():
        print(f"[editor_compare] ERROR: screenshot not found: {screenshot}")
        return 1
    dst_dir = BASELINE_ROOT / backend
    dst_dir.mkdir(parents=True, exist_ok=True)
    dst = dst_dir / f"{name}.png"
    shutil.copy2(screenshot, dst)
    print(f"[editor_compare] baseline saved: {dst}")
    return 0


def cmd_compare(name: str, screenshot: Path, backend: str, threshold_pct: float) -> int:
    baseline_path = BASELINE_ROOT / backend / f"{name}.png"
    if not baseline_path.exists():
        print(f"[editor_compare] ERROR: no baseline '{name}' for backend '{backend}' "
              f"(expected {baseline_path}) — run --save first")
        return 1
    if not screenshot.exists():
        print(f"[editor_compare] ERROR: screenshot not found: {screenshot}")
        return 1

    baseline_img = Image.open(baseline_path)
    current_img  = Image.open(screenshot)
    r = rmse(baseline_img, current_img)
    # RMSE is in 0-255 pixel-value space; report as a percentage of the max
    # possible per-channel error (255) so the plan's "< 0.5%" threshold has
    # a well-defined, resolution-independent meaning.
    r_pct = (r / 255.0) * 100.0
    print(f"[editor_compare] RMSE={r:.4f} ({r_pct:.4f}%) vs baseline '{name}' (backend={backend})")
    if r_pct > threshold_pct:
        print(f"[editor_compare] FAIL: RMSE {r_pct:.4f}% exceeds threshold {threshold_pct:.4f}%")
        return 1
    print(f"[editor_compare] PASS: RMSE {r_pct:.4f}% within threshold {threshold_pct:.4f}%")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Editor scenario screenshot RMSE comparison")
    ap.add_argument("--save",    metavar="NAME")
    ap.add_argument("--compare", metavar="NAME")
    ap.add_argument("screenshot", type=Path, nargs="?")
    ap.add_argument("--backend", default="hw", help="baseline sub-directory (default: hw)")
    ap.add_argument("--threshold-pct", type=float, default=0.5)
    args = ap.parse_args()

    if args.save:
        if not args.screenshot:
            print("--save requires a screenshot path"); return 1
        return cmd_save(args.save, args.screenshot, args.backend)
    if args.compare:
        if not args.screenshot:
            print("--compare requires a screenshot path"); return 1
        return cmd_compare(args.compare, args.screenshot, args.backend, args.threshold_pct)
    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
