#!/usr/bin/env python3
"""
qa_validation_check.py — Vulkan validation-layer log scan for monkey_dust QA

MD_GPU_VALIDATION (engine/src/render/gpu_device.cpp) turns on Vulkan
validation layers whenever CMAKE_BUILD_TYPE=Debug — real per-pipeline
compile overhead (documented: ~4.5s stall on terrain_forward alone), so
this is NOT part of the default fast qa_run.sh path. Run
`qa_run.sh --validation` (separate temp_/build_qa_validation dir, Debug
build) to actually exercise this, then this script scans the resulting
<capture>/game_stdout.log for VUID-*/ERROR-level messages.

Usage:
  python3 tools/qa/qa_validation_check.py                       # latest capture
  python3 tools/qa/qa_validation_check.py --capture 20260722_235959
"""

import argparse
import re
import sys
from pathlib import Path
from typing import Optional

REPO_ROOT    = Path(__file__).resolve().parents[2]
CAPTURES_DIR = REPO_ROOT / "tools" / "qa" / "captures"

# Vulkan validation layers print "Validation Error/Warning: ... [ VUID-... ]"
# via SDL's own log callback. WARNING-level (deprecation notices, perf
# advisories) is common/expected noise — only ERROR-level (real misuse:
# invalid state transitions, resource-lifetime hazards, sync hazards) fails
# the check, matching QA_GUIDE.md's existing "log only, don't fail on
# WARNING unless explicitly critical" convention (parse_validation_log.py
# equivalent from the original design doc).
WARNING_RE = re.compile(r"Validation Warning|\bWARNING\b.*Vulkan", re.IGNORECASE)
# Bare VUID- without an explicit Error/Warning keyword is ambiguous — treat
# as an error signal only when NOT already claimed by WARNING_RE above (a
# VUID always appears in both warning and error validation lines, so this
# must be checked second, on lines WARNING_RE didn't already match, or a
# "Validation Warning: ... VUID-..." line would wrongly count as an error).
ERROR_RE = re.compile(r"VUID-[\w-]+|Validation Error|\bERROR\b.*Vulkan", re.IGNORECASE)


def latest_capture() -> Optional[str]:
    if not CAPTURES_DIR.exists():
        return None
    entries = sorted(p.name for p in CAPTURES_DIR.iterdir() if p.is_dir())
    return entries[-1] if entries else None


def scan(log_path: Path) -> tuple[list[str], list[str]]:
    errors, warnings = [], []
    if not log_path.exists():
        return errors, warnings
    for line in log_path.read_text(errors="replace").splitlines():
        if WARNING_RE.search(line):
            warnings.append(line.strip())
        elif ERROR_RE.search(line):
            errors.append(line.strip())
    return errors, warnings


def main() -> int:
    ap = argparse.ArgumentParser(description="monkey_dust Vulkan validation-layer log scan")
    ap.add_argument("--capture", help="capture ID (default: latest)")
    # --log: scan an arbitrary log file directly (e.g. a `--exec` scenario's
    # captured stdout/stderr) instead of resolving a qa_run.sh capture
    # directory — added for the Granite-migration verification-methodology
    # phase (plan at /home/rdga1/.claude/plans/serene-pondering-teapot.md)
    # so `--exec` runs can reuse this exact scan/regex without faking a
    # captures/<id>/game_stdout.log directory structure just to invoke it.
    ap.add_argument("--log", help="scan this log file directly instead of a capture directory")
    args = ap.parse_args()

    if args.log:
        log_path = Path(args.log)
        errors, warnings = scan(log_path)
    else:
        capture_id = args.capture or latest_capture()
        if not capture_id:
            print("[qa_validation] No captures found under tools/qa/captures/", file=sys.stderr)
            return 1
        log_path = CAPTURES_DIR / capture_id / "game_stdout.log"
        errors, warnings = scan(log_path)

    if warnings:
        print(f"[qa_validation] {len(warnings)} validation WARNING(s) (logged, not failing):")
        for w in warnings[:20]:
            print(f"  {w}")
        if len(warnings) > 20:
            print(f"  ... and {len(warnings) - 20} more")

    if errors:
        print(f"[qa_validation] FAIL: {len(errors)} validation ERROR(s):")
        for e in errors[:20]:
            print(f"  {e}")
        if len(errors) > 20:
            print(f"  ... and {len(errors) - 20} more")
        return 1

    if not log_path.exists():
        print(f"[qa_validation] WARN: {log_path} not found — was this capture run with "
              f"`qa_run.sh --validation` (MD_GPU_VALIDATION needs a Debug build)?")
        return 0

    print("[qa_validation] PASS (0 validation errors)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
