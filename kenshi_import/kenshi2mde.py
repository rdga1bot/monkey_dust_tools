#!/usr/bin/env python3
"""kenshi2mde — Phase 5 CLI converter (TERRAIN_MASTER_PROMPT.md).

Python, not C++ (per the plan's own "C++ (or Python, if justified)"
allowance) — every existing Kenshi-import script in this project
(tif_to_r32.py, md_worldmap_gen.py, md_stitch_terrain.py,
md_bake_ground_layers.py, private/md_mod_import.py, ...) is already
Python/numpy/PIL, so a Python CLI stays consistent with the established
toolchain instead of introducing a second language for the same job.

This is NOT a from-scratch reimplementation of Kenshi format parsing --
that parsing already exists, is already used in production (see
CLAUDE.md "Kenshi asset pipeline"), and per Phase 4's findings
(terrain_research/02_KENSHI_FORMAT.md Sec.4) is understood for every
format except blendinfo.dat's exact byte layout. kenshi2mde is the
unified --inspect/--export-png/--convert/--validate CLI surface the
master plan calls for, orchestrating the existing per-format scripts
rather than duplicating their logic.

Modes:
  --inspect      Print source-file + existing runtime-artifact metadata,
                 no conversion performed.
  --export-png   Regenerate game/data/textures/world_map.png (visual
                 verification) by invoking tools/md_worldmap_gen.py.
  --convert      Full heightmap pipeline: tools/tif_to_r32.py then
                 tools/md_worldmap_gen.py. Ground-layer bake
                 (tools/md_bake_ground_layers.py) is NOT run by default
                 -- it's slow (~10 min) and its blend-weight input
                 (blendinfo.dat) has an unresolved exact byte format
                 (Phase 4 finding), so its output is a disclosed
                 heuristic approximation, not run silently as part of
                 --convert. Pass --with-ground-bake to include it.
  --validate     Load the existing world_hmap.r16 and verify every
                 zone-to-zone shared edge matches exactly (the
                 "seam=0.0m" construction tif_to_r32.py's own doc
                 comment claims, checked here as an automated,
                 reproducible test rather than a claim taken on faith).

Requires the repo's Kenshi source mirror at tmp_/kenshi/data/newland/land/
(gitignored, per CLAUDE.md "tmp_/ = ПОСТІЙНИЙ: RE дані Kenshi") -- run
from the repo root, or pass --repo-root.
"""
import argparse
import os
import subprocess
import sys

import numpy as np


def _repo_root(explicit):
    if explicit:
        return os.path.abspath(explicit)
    # tools/kenshi_import/kenshi2mde.py -> tools/kenshi_import -> tools -> repo root
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _tools_dir(root):
    return os.path.join(root, "tools")


def cmd_inspect(root):
    land = os.path.join(root, "tmp_", "kenshi", "data", "newland", "land")
    print(f"[inspect] Kenshi source mirror: {land}")
    if not os.path.isdir(land):
        print("[inspect] NOT FOUND -- see CLAUDE.md 'Kenshi asset pipeline' for setup.")
        return 1
    for name in sorted(os.listdir(land)):
        p = os.path.join(land, name)
        if os.path.isfile(p):
            print(f"  {name:20s} {os.path.getsize(p):>12d} B")

    sys.path.insert(0, _tools_dir(root))
    from md_hmap_io import ATLAS_ZONES, ATLAS_VERTS, HEIGHT_MAX_M, R16_MAGIC  # noqa: E402

    r16 = os.path.join(root, "game", "data", "terrain", "world_hmap.r16")
    print(f"\n[inspect] runtime heightmap: {r16}")
    if os.path.exists(r16):
        with open(r16, "rb") as f:
            header = np.frombuffer(f.read(8), dtype=np.uint32)
        ok = header[0] == R16_MAGIC and header[1] == ATLAS_ZONES
        print(f"  magic={header[0]:#x} zones={header[1]} "
              f"(expected magic={R16_MAGIC:#x} zones={ATLAS_ZONES}) -> {'OK' if ok else 'MISMATCH'}")
        print(f"  size={os.path.getsize(r16)} B, verts/zone={ATLAS_VERTS}, "
              f"height_max_m={HEIGHT_MAX_M}")
    else:
        print("  not yet generated -- run --convert first.")
    return 0


def cmd_export_png(root):
    script = os.path.join(_tools_dir(root), "md_worldmap_gen.py")
    print(f"[export-png] running {script}")
    return subprocess.call([sys.executable, script], cwd=root)


def cmd_convert(root, with_ground_bake):
    tif2r32 = os.path.join(_tools_dir(root), "tif_to_r32.py")
    print(f"[convert] step 1/2: {tif2r32}")
    rc = subprocess.call([sys.executable, tif2r32], cwd=root)
    if rc != 0:
        print(f"[convert] tif_to_r32.py failed (exit {rc})")
        return rc

    worldmap = os.path.join(_tools_dir(root), "md_worldmap_gen.py")
    print(f"[convert] step 2/2: {worldmap}")
    rc = subprocess.call([sys.executable, worldmap], cwd=root)
    if rc != 0:
        print(f"[convert] md_worldmap_gen.py failed (exit {rc})")
        return rc

    if with_ground_bake:
        bake = os.path.join(_tools_dir(root), "md_bake_ground_layers.py")
        print(f"[convert] optional ground-layer bake: {bake}")
        print("[convert] NOTE: ground-texture-layer selection is a heuristic "
              "approximation (steepness+overlay-mask argmax) -- blendinfo.dat's "
              "real per-pixel weights are NOT used, its exact byte format is "
              "still unresolved (Phase 4, terrain_research/02_KENSHI_FORMAT.md Sec.4.2).")
        rc = subprocess.call([sys.executable, bake], cwd=root)
        if rc != 0:
            print(f"[convert] md_bake_ground_layers.py failed (exit {rc})")
            return rc

    print("[convert] done.")
    return 0


def check_seams(tiled):
    """tiled: (ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS, ATLAS_VERTS) float32,
    indexed [zy, zx, row, col], heights in metres.

    Returns (max_abs_mismatch_m, num_checked_edges). A watertight atlas
    (each zone's edge sampling the exact same source pixel as its
    neighbour, per tif_to_r32.py's "seam=0.0m" construction) should give
    exactly 0.0 -- this makes that claim an automated, reproducible check
    instead of an assertion taken on faith.
    """
    zones = tiled.shape[0]
    max_mismatch = 0.0
    checked = 0
    # Horizontal neighbours: zone (zy, zx)'s right edge (col=-1) vs
    # zone (zy, zx+1)'s left edge (col=0), for every row.
    for zy in range(zones):
        for zx in range(zones - 1):
            right_edge = tiled[zy, zx, :, -1]
            left_edge = tiled[zy, zx + 1, :, 0]
            mismatch = float(np.max(np.abs(right_edge - left_edge)))
            max_mismatch = max(max_mismatch, mismatch)
            checked += 1
    # Vertical neighbours: zone (zy, zx)'s bottom edge (row=-1) vs
    # zone (zy+1, zx)'s top edge (row=0), for every column.
    for zy in range(zones - 1):
        for zx in range(zones):
            bottom_edge = tiled[zy, zx, -1, :]
            top_edge = tiled[zy + 1, zx, 0, :]
            mismatch = float(np.max(np.abs(bottom_edge - top_edge)))
            max_mismatch = max(max_mismatch, mismatch)
            checked += 1
    return max_mismatch, checked


def cmd_validate(root, tolerance_m):
    sys.path.insert(0, _tools_dir(root))
    from md_hmap_io import load_atlas_tiled  # noqa: E402

    r16 = os.path.join(root, "game", "data", "terrain", "world_hmap.r16")
    if not os.path.exists(r16):
        print(f"[validate] {r16} not found -- run --convert first.")
        return 1

    print(f"[validate] loading {r16} ...")
    tiled = load_atlas_tiled(r16)
    max_mismatch, checked = check_seams(tiled)
    status = "PASS" if max_mismatch <= tolerance_m else "FAIL"
    print(f"[validate] checked {checked} zone-to-zone edges, "
          f"max mismatch = {max_mismatch:.6f} m (tolerance {tolerance_m} m) -> {status}")
    return 0 if status == "PASS" else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--inspect", action="store_true")
    mode.add_argument("--export-png", action="store_true")
    mode.add_argument("--convert", action="store_true")
    mode.add_argument("--validate", action="store_true")
    ap.add_argument("--with-ground-bake", action="store_true",
                     help="--convert also runs the (slow, heuristic) ground-layer bake")
    ap.add_argument("--tolerance-m", type=float, default=0.0,
                     help="--validate: max allowed seam mismatch in metres (default 0.0, exact)")
    ap.add_argument("--repo-root", default=None,
                     help="repo root (default: inferred from this script's location)")
    args = ap.parse_args()

    root = _repo_root(args.repo_root)

    if args.inspect:
        sys.exit(cmd_inspect(root))
    elif args.export_png:
        sys.exit(cmd_export_png(root))
    elif args.convert:
        sys.exit(cmd_convert(root, args.with_ground_bake))
    elif args.validate:
        sys.exit(cmd_validate(root, args.tolerance_m))


if __name__ == "__main__":
    main()
