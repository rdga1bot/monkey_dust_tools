#!/usr/bin/env python3
"""
qa_regression.py — Visual Regression for monkey_dust QA

BackstopJS-inspired: screenshot baseline → pixel diff → HTML with before/after scrubber.

Usage:
  python3 tools/qa/qa_regression.py --baseline 20260531_030631
  python3 tools/qa/qa_regression.py --compare  20260531_040000
  python3 tools/qa/qa_regression.py --compare  20260531_040000 --baseline 20260531_030631
  python3 tools/qa/qa_regression.py --list
"""

import argparse
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Optional

import numpy as np
from PIL import Image, ImageChops, ImageFilter

REPO_ROOT    = Path(__file__).resolve().parents[2]
CAPTURES_DIR = REPO_ROOT / "tools" / "qa" / "captures"
BASELINES_DIR = REPO_ROOT / "tools" / "qa" / "baselines"
REPORTS_DIR  = REPO_ROOT / "tools" / "qa" / "reports"

# Per-pixel diff threshold (0-255). Pixels below this are ignored (anti-aliasing noise).
DIFF_THRESHOLD = 8
# Max % of pixels that can differ before a frame is flagged as "failed".
FAIL_PCT = 2.0


# ── Helpers ───────────────────────────────────────────────────────────────────

def frames_for(capture_id: str) -> list[Path]:
    d = CAPTURES_DIR / capture_id / "frames"
    if not d.exists():
        # Try capture root directly (old layout: caps in capture root)
        d = CAPTURES_DIR / capture_id
    if not d.exists():
        return []
    return sorted(d.glob("frame_*.png")) or sorted(d.glob("cap_*.png"))


def latest_baseline() -> Optional[str]:
    if not BASELINES_DIR.exists():
        return None
    entries = sorted(BASELINES_DIR.iterdir(), key=lambda p: p.name)
    return entries[-1].name if entries else None


def diff_images(img_a: Image.Image, img_b: Image.Image) -> tuple[Image.Image, float]:
    """Returns (diff_image, changed_pixel_pct)."""
    a = img_a.convert("RGB")
    b = img_b.convert("RGB")
    if a.size != b.size:
        b = b.resize(a.size, Image.LANCZOS)

    na = np.array(a, dtype=np.int32)
    nb = np.array(b, dtype=np.int32)
    delta = np.abs(na - nb).max(axis=2)            # max channel diff per pixel
    mask  = (delta > DIFF_THRESHOLD).astype(np.uint8)
    pct   = mask.mean() * 100.0

    # Visualise: red = changed pixels, grey = unchanged
    grey  = np.mean(na, axis=2, keepdims=True).repeat(3, axis=2).astype(np.uint8)
    diff_arr = grey.copy()
    diff_arr[mask == 1] = [220, 40, 40]
    return Image.fromarray(diff_arr.astype(np.uint8)), pct


# ── Baseline ──────────────────────────────────────────────────────────────────

def cmd_baseline(capture_id: str) -> int:
    frames = frames_for(capture_id)
    if not frames:
        print(f"[regression] ERROR: no frames in capture {capture_id}")
        return 1

    dst = BASELINES_DIR / capture_id
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)

    for f in frames:
        shutil.copy2(f, dst / f.name)

    meta = {"capture_id": capture_id, "frame_count": len(frames)}
    (dst / "meta.json").write_text(json.dumps(meta, indent=2))
    print(f"[regression] Baseline saved: {capture_id} ({len(frames)} frames)")
    return 0


# ── Compare ───────────────────────────────────────────────────────────────────

def cmd_compare(compare_id: str, baseline_id: Optional[str]) -> int:
    if not baseline_id:
        baseline_id = latest_baseline()
    if not baseline_id:
        print("[regression] ERROR: no baseline found. Run --baseline first.")
        return 1

    baseline_dir = BASELINES_DIR / baseline_id
    if not baseline_dir.exists():
        print(f"[regression] ERROR: baseline {baseline_id} not found")
        return 1

    compare_frames = frames_for(compare_id)
    baseline_frames = sorted(baseline_dir.glob("frame_*.png")) or \
                      sorted(baseline_dir.glob("cap_*.png"))

    if not compare_frames:
        print(f"[regression] ERROR: no frames in compare capture {compare_id}")
        return 1
    if not baseline_frames:
        print(f"[regression] ERROR: no frames in baseline {baseline_id}")
        return 1

    n = min(len(compare_frames), len(baseline_frames))
    print(f"[regression] Comparing {compare_id} vs {baseline_id} — {n} frames")

    report_dir = REPORTS_DIR / compare_id / "regression"
    report_dir.mkdir(parents=True, exist_ok=True)
    diffs_dir  = report_dir / "diffs"
    diffs_dir.mkdir(exist_ok=True)

    results = []
    failed  = 0
    for i in range(n):
        img_a = Image.open(baseline_frames[i])
        img_b = Image.open(compare_frames[i])
        diff_img, pct = diff_images(img_a, img_b)

        diff_name = f"diff_{i:04d}.png"
        diff_img.save(diffs_dir / diff_name)

        status = "FAIL" if pct > FAIL_PCT else "pass"
        if status == "FAIL":
            failed += 1
        results.append({
            "frame": i,
            "baseline": baseline_frames[i].name,
            "compare":  compare_frames[i].name,
            "diff":     diff_name,
            "pct":      round(pct, 3),
            "status":   status,
        })
        if status == "FAIL":
            print(f"  FAIL frame {i:04d}: {pct:.1f}% pixels changed")

    # Write JSON summary
    summary = {
        "baseline_id":  baseline_id,
        "compare_id":   compare_id,
        "frame_count":  n,
        "failed_frames": failed,
        "pass_rate":    round((n - failed) / n * 100, 1),
        "frames":       results,
    }
    (report_dir / "summary.json").write_text(json.dumps(summary, indent=2))

    # Generate HTML report
    html_path = report_dir / "report.html"
    _write_html(html_path, summary, baseline_frames, compare_frames,
                baseline_id, compare_id)

    status_str = "PASS" if failed == 0 else f"FAIL ({failed}/{n} frames)"
    print(f"[regression] Result: {status_str}")
    print(f"[regression] Report: {html_path}")
    return 1 if failed > 0 else 0


# ── HTML report ───────────────────────────────────────────────────────────────

def _write_html(path: Path, summary: dict, baseline_frames: list[Path],
                compare_frames: list[Path], baseline_id: str, compare_id: str):

    rows = ""
    for r in summary["frames"]:
        i   = r["frame"]
        pct = r["pct"]
        cls = "fail" if r["status"] == "FAIL" else "pass"
        # Relative paths from report dir
        b_rel = Path("../../../captures") / baseline_id / "frames" / r["baseline"]
        c_rel = Path("../../../captures") / compare_id  / "frames" / r["compare"]
        d_rel = Path("diffs") / r["diff"]
        rows += f"""
      <tr class="{cls}">
        <td>{i:04d}</td>
        <td>{pct:.2f}%</td>
        <td class="status-cell">{r["status"]}</td>
        <td>
          <div class="compare-wrap">
            <div class="compare-slider" id="s{i}">
              <img class="img-before" src="{b_rel}" alt="baseline">
              <img class="img-after"  src="{c_rel}" alt="compare">
              <div class="divider" id="d{i}"></div>
            </div>
            <img class="diff-thumb" src="{d_rel}" alt="diff" title="diff: {pct:.2f}%">
          </div>
        </td>
      </tr>"""

    total = summary["frame_count"]
    failed = summary["failed_frames"]
    pass_rate = summary["pass_rate"]
    overall = "PASS" if failed == 0 else "FAIL"

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Regression: {compare_id} vs {baseline_id}</title>
<style>
  body {{ font-family: monospace; background:#111; color:#ddd; margin:0; padding:16px; }}
  h1   {{ color:#e8c96e; margin-bottom:4px; }}
  .meta {{ color:#888; margin-bottom:16px; font-size:13px; }}
  .badge {{ display:inline-block; padding:3px 10px; border-radius:4px; font-weight:bold; }}
  .PASS {{ background:#1a4a1a; color:#4ec94e; }}
  .FAIL {{ background:#4a1a1a; color:#e04040; }}
  table {{ border-collapse:collapse; width:100%; }}
  th, td {{ padding:6px 10px; text-align:left; border-bottom:1px solid #2a2a2a; }}
  th {{ background:#1a1a1a; color:#aaa; font-size:12px; }}
  tr.fail td {{ background:#2a1212; }}
  tr.pass td {{ background:#111; }}
  .status-cell {{ font-weight:bold; }}
  tr.fail .status-cell {{ color:#e04040; }}
  tr.pass .status-cell {{ color:#4ec94e; }}

  /* Side-by-side scrubber */
  .compare-wrap {{ display:flex; gap:8px; align-items:flex-start; }}
  .compare-slider {{
    position:relative; width:300px; height:168px; overflow:hidden;
    cursor:ew-resize; border:1px solid #333; flex-shrink:0;
    user-select:none;
  }}
  .compare-slider img {{ position:absolute; top:0; left:0; width:300px; height:168px; object-fit:cover; }}
  .img-before {{ clip-path:inset(0 50% 0 0); }}
  .divider {{
    position:absolute; top:0; left:50%; width:2px; height:100%;
    background:#e8c96e; pointer-events:none;
  }}
  .diff-thumb {{ width:150px; height:84px; object-fit:contain; border:1px solid #333; }}
</style>
</head>
<body>
<h1>Visual Regression</h1>
<div class="meta">
  Baseline: <strong>{baseline_id}</strong> &rarr;
  Compare: <strong>{compare_id}</strong> &nbsp;|&nbsp;
  {total} frames &nbsp;|&nbsp;
  Pass rate: {pass_rate}%
  &nbsp;<span class="badge {overall}">{overall}</span>
</div>
<table>
  <thead><tr><th>#</th><th>Δ px%</th><th>Status</th><th>Before / After (drag) + Diff</th></tr></thead>
  <tbody>{rows}
  </tbody>
</table>
<script>
// Drag-to-reveal scrubber for each compare-slider
document.querySelectorAll('.compare-slider').forEach(function(el) {{
  var before = el.querySelector('.img-before');
  var div    = el.querySelector('.divider');
  var dragging = false;
  function setPos(x) {{
    var r = el.getBoundingClientRect();
    var pct = Math.max(0, Math.min(100, (x - r.left) / r.width * 100));
    before.style.clipPath = 'inset(0 ' + (100 - pct) + '% 0 0)';
    div.style.left = pct + '%';
  }}
  el.addEventListener('mousedown', function(e) {{ dragging = true; setPos(e.clientX); }});
  document.addEventListener('mousemove', function(e) {{ if (dragging) setPos(e.clientX); }});
  document.addEventListener('mouseup',   function()  {{ dragging = false; }});
  el.addEventListener('touchstart', function(e) {{ setPos(e.touches[0].clientX); }}, {{passive:true}});
  el.addEventListener('touchmove',  function(e) {{ setPos(e.touches[0].clientX); }}, {{passive:true}});
}});
</script>
</body>
</html>"""
    path.write_text(html)


# ── List ──────────────────────────────────────────────────────────────────────

def cmd_list():
    print("── Baselines ──────────────────────────────────────────────")
    if BASELINES_DIR.exists():
        for b in sorted(BASELINES_DIR.iterdir()):
            meta_f = b / "meta.json"
            meta = json.loads(meta_f.read_text()) if meta_f.exists() else {}
            fc = meta.get("frame_count", "?")
            print(f"  {b.name}  ({fc} frames)")
    else:
        print("  (none)")

    print("── Captures ───────────────────────────────────────────────")
    if CAPTURES_DIR.exists():
        for c in sorted(CAPTURES_DIR.iterdir()):
            fc = len(frames_for(c.name))
            print(f"  {c.name}  ({fc} frames)")
    else:
        print("  (none)")


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="monkey_dust visual regression")
    ap.add_argument("--baseline", metavar="CAPTURE_ID",
                    help="mark a capture as baseline reference")
    ap.add_argument("--compare",  metavar="CAPTURE_ID",
                    help="compare capture against baseline")
    ap.add_argument("--baseline-id", metavar="BASELINE_ID",
                    help="explicit baseline to compare against (default: latest)")
    ap.add_argument("--list", action="store_true",
                    help="list baselines and captures")
    args = ap.parse_args()

    if args.list:
        cmd_list()
        return 0

    if args.baseline:
        return cmd_baseline(args.baseline)

    if args.compare:
        return cmd_compare(args.compare, args.baseline_id)

    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
