#!/usr/bin/env python3
"""One-off helper for CLAUDE_CODE_RENDER_PROFILES_SPEC.md §1: capture
Forward/Deferred_Low/Deferred_High screenshots at worst zone(24,12) and
canyon zone(6,42), via the editor build (md.screenshot() is a no-op
under MONKEY_DUST_EDITOR=OFF). One process launch per tier, both zones
captured within that launch."""
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_cmd_driver import Driver  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
RENDER_SETTINGS_PATH = REPO_ROOT / "data" / "render_settings.json"
OUT_DIR = REPO_ROOT / "docs" / "assets" / "render_profiles_2026-09"
CHUNK_SIZE = 460.8
ZONES = {"worst": (24, 12), "canyon": (6, 42)}
TIERS = ["Forward", "Deferred_Low", "Deferred_High"]


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


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    original = RENDER_SETTINGS_PATH.read_text() if RENDER_SETTINGS_PATH.exists() else None
    try:
        for tier in TIERS:
            write_render_settings(tier)
            d = Driver(exe="build/game/monkey_dust")
            print(f"[shots] launching for tier={tier} ...")
            if not d.launch(wait_s=40):
                print(f"[shots] ERROR: failed to connect for {tier}", file=sys.stderr)
                continue
            for zname, (zx, zz) in ZONES.items():
                wx = (zx + 0.5) * CHUNK_SIZE
                wz = (zz + 0.5) * CHUNK_SIZE
                out = OUT_DIR / f"{tier}_{zname}.png"
                ok, r = d.screenshot(str(out), camera=(wx, 40.0, wz, 0.0, 10.0), settle_s=3.0)
                print(f"[shots] {tier}/{zname} -> {out} ok={ok}")
            d.shutdown()
            time.sleep(1)
    finally:
        if original is not None:
            RENDER_SETTINGS_PATH.write_text(original)
            print(f"[shots] restored {RENDER_SETTINGS_PATH}")


if __name__ == "__main__":
    main()
