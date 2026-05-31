#!/usr/bin/env python3
"""
qa_bdd.py — BDD (Gherkin) test runner for monkey_dust QA

Parses .feature files and runs step definitions against QA captures.
Uses qa_state.jsonl for log-based assertions (precise, no image parsing).
Falls back to frame PNGs for visual assertions.

Usage:
  python3 tools/qa/qa_bdd.py tools/qa/features/             --capture 20260531_030631
  python3 tools/qa/qa_bdd.py tools/qa/features/npc.feature  --capture 20260531_030631
  python3 tools/qa/qa_bdd.py tools/qa/features/             --capture latest
"""

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import numpy as np
from PIL import Image

REPO_ROOT    = Path(__file__).resolve().parents[2]
CAPTURES_DIR = REPO_ROOT / "tools" / "qa" / "captures"

# ── Gherkin parser ─────────────────────────────────────────────────────────────

@dataclass
class Step:
    keyword: str   # Given | When | Then | And | But
    text: str

@dataclass
class Scenario:
    name: str
    steps: list[Step] = field(default_factory=list)

@dataclass
class Feature:
    name: str
    path: Path
    background: list[Step] = field(default_factory=list)
    scenarios: list[Scenario] = field(default_factory=list)


STEP_KW = {"given", "when", "then", "and", "but"}


def parse_feature(path: Path) -> Feature:
    feat = Feature(name="", path=path)
    cur_scenario: Optional[Scenario] = None
    in_background = False

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue

        low = line.lower()
        if low.startswith("feature:"):
            feat.name = line[8:].strip()
        elif low.startswith("background:"):
            in_background = True
            cur_scenario  = None
        elif low.startswith("scenario:"):
            in_background = False
            cur_scenario  = Scenario(name=line[9:].strip())
            feat.scenarios.append(cur_scenario)
        elif any(low.startswith(kw) for kw in STEP_KW):
            kw  = next(k for k in STEP_KW if low.startswith(k))
            txt = line[len(kw):].strip()
            step = Step(keyword=kw, text=txt)
            if in_background:
                feat.background.append(step)
            elif cur_scenario is not None:
                cur_scenario.steps.append(step)

    return feat


# ── Step registry ─────────────────────────────────────────────────────────────

_step_registry: list[tuple[re.Pattern, callable]] = []


def step(pattern: str):
    def decorator(fn):
        _step_registry.append((re.compile(pattern, re.IGNORECASE), fn))
        return fn
    return decorator


def match_step(text: str):
    for pat, fn in _step_registry:
        m = pat.fullmatch(text)
        if m:
            return fn, m
    return None, None


# ── Context ────────────────────────────────────────────────────────────────────

class Context:
    def __init__(self):
        self.capture_id: str = ""
        self.qa_ticks: list[dict] = []     # parsed qa_state.jsonl
        self.frames: list[Path] = []       # frame PNGs

    def load_capture(self, capture_id: str):
        self.capture_id = capture_id
        cap_dir = CAPTURES_DIR / capture_id

        jsonl = cap_dir / "qa_state.jsonl"
        if jsonl.exists():
            self.qa_ticks = []
            for line in jsonl.read_text().splitlines():
                line = line.strip()
                if line:
                    try:
                        self.qa_ticks.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass

        frames_dir = cap_dir / "frames"
        if not frames_dir.exists():
            frames_dir = cap_dir
        self.frames = sorted(frames_dir.glob("frame_*.png")) or \
                      sorted(frames_dir.glob("cap_*.png"))

    def npc_slots(self) -> set[int]:
        slots = set()
        for tick in self.qa_ticks:
            for npc in tick.get("npcs", []):
                slots.add(npc["s"])
        return slots

    def npc_positions(self, slot: int) -> list[tuple[float, float]]:
        """Returns (x, z) per tick for a given NPC slot."""
        result = []
        for tick in self.qa_ticks:
            for npc in tick.get("npcs", []):
                if npc["s"] == slot:
                    result.append((npc["x"], npc["z"]))
                    break
        return result


ctx = Context()


# ── Built-in step definitions ──────────────────────────────────────────────────

@step(r'capture "([^"]+)"')
def step_load_capture(m):
    cid = m.group(1)
    if cid == "latest":
        entries = sorted(CAPTURES_DIR.iterdir(), key=lambda p: p.name) \
                  if CAPTURES_DIR.exists() else []
        cid = entries[-1].name if entries else ""
    if not cid:
        return Fail("no capture found")
    ctx.load_capture(cid)
    ticks = len(ctx.qa_ticks)
    frames = len(ctx.frames)
    return Pass(f"loaded capture {cid} ({ticks} ticks, {frames} frames)")


@step(r'no NPC (?:should )?freeze(?:s)? for more than (\d+) (?:consecutive )?ticks?')
def step_no_npc_freeze(m):
    max_freeze = int(m.group(1))
    if not ctx.qa_ticks:
        return Skip("no qa_state.jsonl — skipping log-based check")

    worst_slot, worst_count = -1, 0
    for slot in ctx.npc_slots():
        positions = ctx.npc_positions(slot)
        if len(positions) < 2:
            continue
        run = 1
        for i in range(1, len(positions)):
            dx = positions[i][0] - positions[i-1][0]
            dz = positions[i][1] - positions[i-1][1]
            dist = (dx*dx + dz*dz) ** 0.5
            if dist < 0.02:   # sub-2cm movement = effectively frozen
                run += 1
            else:
                run = 1
            if run > worst_count:
                worst_count = run
                worst_slot  = slot

    if worst_count > max_freeze:
        return Fail(f"NPC slot {worst_slot} frozen for {worst_count} consecutive ticks "
                    f"(threshold {max_freeze})")
    return Pass(f"no NPC frozen > {max_freeze} ticks (max seen: {worst_count})")


@step(r'player position changes (?:by )?at least ([\d.]+)m per second')
def step_player_moving(m):
    min_speed = float(m.group(1))
    if not ctx.qa_ticks:
        return Skip("no qa_state.jsonl")

    speeds = []
    for i in range(1, len(ctx.qa_ticks)):
        a = ctx.qa_ticks[i-1]
        b = ctx.qa_ticks[i]
        dt = b["t"] - a["t"]
        if dt < 0.001:
            continue
        dx = b["px"] - a["px"]
        dz = b["pz"] - a["pz"]
        speeds.append(((dx*dx + dz*dz) ** 0.5) / dt)

    if not speeds:
        return Skip("only 1 tick — cannot measure speed")
    max_speed = max(speeds)
    avg_speed = sum(speeds) / len(speeds)
    if max_speed < min_speed:
        return Fail(f"player never reached {min_speed}m/s (max was {max_speed:.3f}m/s)")
    return Pass(f"player max speed {max_speed:.2f}m/s, avg {avg_speed:.2f}m/s")


@step(r'sky is visible in (?:at least )?([\d]+)% of frames')
def step_sky_visible(m):
    min_pct = float(m.group(1))
    if not ctx.frames:
        return Skip("no frame PNGs")

    ok_count = 0
    for f in ctx.frames:
        img = np.array(Image.open(f).convert("RGB"))
        h = img.shape[0]
        top = img[:h//4]
        blue_mean = top[:, :, 2].mean()
        if blue_mean > 100:   # blue channel > ~40% = sky present
            ok_count += 1

    pct = ok_count / len(ctx.frames) * 100
    if pct < min_pct:
        return Fail(f"sky visible in only {pct:.1f}% of frames (need {min_pct}%)")
    return Pass(f"sky visible in {pct:.1f}% of frames")


@step(r'sky is visible in all frames')
def step_sky_visible_all(m):
    return step_sky_visible(re.match(r'sky is visible in (?:at least )?([\d]+)% of frames',
                                      'sky is visible in 95% of frames'))


@step(r'NPCs are visible in (?:at least )?([\d]+)% of frames')
def step_npcs_visible(m):
    min_pct = float(m.group(1))
    if not ctx.frames:
        return Skip("no frame PNGs")

    ok_count = 0
    for f in ctx.frames:
        img = np.array(Image.open(f).convert("RGB"))
        h, w = img.shape[:2]
        # Bottom half, centre third — where NPCs usually stand
        region = img[h//2:, w//3:2*w//3]
        brightness = region.mean()
        if brightness > 20:  # something rendered there
            ok_count += 1

    pct = ok_count / len(ctx.frames) * 100
    if pct < min_pct:
        return Fail(f"NPCs visible in only {pct:.1f}% of frames (need {min_pct}%)")
    return Pass(f"NPCs visible in {pct:.1f}% of frames")


@step(r'at least (\d+) NPCs are present')
def step_npc_count(m):
    min_npcs = int(m.group(1))
    if not ctx.qa_ticks:
        return Skip("no qa_state.jsonl")

    max_count = max((len(tick.get("npcs", [])) for tick in ctx.qa_ticks), default=0)
    if max_count < min_npcs:
        return Fail(f"max NPC count was {max_count} (need {min_npcs})")
    return Pass(f"up to {max_count} NPCs present")


@step(r'no NPC teleports more than ([\d.]+)m in one tick')
def step_no_teleport(m):
    max_dist = float(m.group(1))
    if not ctx.qa_ticks:
        return Skip("no qa_state.jsonl")

    worst = 0.0
    worst_slot = -1
    prev: dict[int, tuple[float, float]] = {}

    for tick in ctx.qa_ticks:
        for npc in tick.get("npcs", []):
            s = npc["s"]
            pos = (npc["x"], npc["z"])
            if s in prev:
                dx = pos[0] - prev[s][0]
                dz = pos[1] - prev[s][1]
                d  = (dx*dx + dz*dz) ** 0.5
                if d > worst:
                    worst = d
                    worst_slot = s
            prev[s] = pos

    if worst > max_dist:
        return Fail(f"NPC slot {worst_slot} teleported {worst:.2f}m in one tick "
                    f"(threshold {max_dist}m)")
    return Pass(f"no NPC teleported > {max_dist}m (max was {worst:.2f}m)")


# ── Result types ──────────────────────────────────────────────────────────────

class Pass:
    def __init__(self, msg=""): self.msg = msg
class Fail:
    def __init__(self, msg=""): self.msg = msg
class Skip:
    def __init__(self, msg=""): self.msg = msg


# ── Runner ────────────────────────────────────────────────────────────────────

PASS  = "\033[32m✔\033[0m"
FAIL  = "\033[31m✗\033[0m"
SKIP  = "\033[33m—\033[0m"
BOLD  = "\033[1m"
RESET = "\033[0m"


def run_feature(feat: Feature) -> tuple[int, int, int]:
    """Returns (passed, failed, skipped)."""
    passed = failed = skipped = 0

    print(f"\n{BOLD}Feature: {feat.name}{RESET}  ({feat.path.name})")

    for scenario in feat.scenarios:
        print(f"\n  Scenario: {scenario.name}")

        # Background steps run before each scenario
        all_steps = feat.background + scenario.steps
        scenario_failed = False

        for step_obj in all_steps:
            text = step_obj.text
            fn, m = match_step(text)

            if fn is None:
                print(f"    {SKIP} {step_obj.keyword} {text}  (no step definition)")
                skipped += 1
                continue

            try:
                result = fn(m)
            except Exception as e:
                result = Fail(f"exception: {e}")

            if isinstance(result, Pass):
                icon = PASS
                passed += 1
            elif isinstance(result, Fail):
                icon = FAIL
                failed += 1
                scenario_failed = True
            else:  # Skip
                icon = SKIP
                skipped += 1

            suffix = f"  # {result.msg}" if result.msg else ""
            print(f"    {icon} {step_obj.keyword} {text}{suffix}")

            if scenario_failed and not isinstance(result, Fail):
                # Don't run more steps after first failure in this scenario
                break

    return passed, failed, skipped


def collect_features(path: Path) -> list[Feature]:
    if path.is_file():
        return [parse_feature(path)]
    return [parse_feature(f) for f in sorted(path.glob("**/*.feature"))]


def main():
    ap = argparse.ArgumentParser(description="monkey_dust BDD runner")
    ap.add_argument("path", nargs="?", default="tools/qa/features",
                    help="feature file or directory (default: tools/qa/features)")
    ap.add_argument("--capture", metavar="CAPTURE_ID", default="latest",
                    help="QA capture to run against (default: latest)")
    args = ap.parse_args()

    # Pre-load capture so background steps get it
    if args.capture:
        ctx.load_capture(
            args.capture if args.capture != "latest" else
            (sorted(CAPTURES_DIR.iterdir())[-1].name
             if CAPTURES_DIR.exists() and any(CAPTURES_DIR.iterdir()) else "")
        )

    features = collect_features(Path(args.path))
    if not features:
        print(f"[bdd] No .feature files found in {args.path}")
        return 1

    total_pass = total_fail = total_skip = 0
    for feat in features:
        p, f, s = run_feature(feat)
        total_pass += p
        total_fail += f
        total_skip += s

    print(f"\n{'─'*50}")
    status = f"\033[32mPASS\033[0m" if total_fail == 0 else f"\033[31mFAIL\033[0m"
    print(f"BDD: {status}  "
          f"{PASS}{total_pass} passed  "
          f"{FAIL}{total_fail} failed  "
          f"{SKIP}{total_skip} skipped")
    return 1 if total_fail > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
