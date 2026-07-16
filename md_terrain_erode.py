#!/usr/bin/env python3
"""
md_terrain_erode.py — mass-conserving talus-angle smoothing over
world_hmap.r32's TerrainAtlas, ported from plate-tectonics' plate::erode()
(github.com/Mindwerks/plate-tectonics, src/plate.cpp:346-438) — a 4-neighbor
height-redistribution rule that only touches cell pairs whose height
difference exceeds a talus threshold, moving a fraction of the excess from
the higher cell to the lower one. Mass-conserving (sum of all heights in
the world is unchanged by construction) and applied iteratively.

Motivating theory (docs/OSS_TERRAIN_METHODS.md, "pillar" artifact
investigation, task terrain-patches): some Kenshi zones have up to
~500m of height variation within a single 460.8m chunk — a near-
vertical real cliff in the source elevation data — theorized to be why
the terrain LOD/normal pipeline produces degenerate, rapidly-
alternating-normal "pillar"/ribbon rendering artifacts there.

**TESTED 2026-07-17, DID NOT FIX THE ARTIFACT.** Ran this at both
--talus 12 (max cell-to-cell diff 332m -> 25.5m, a 92% reduction) and
--talus 5 (-> 23.0m, 93% reduction) against the reported repro camera
pose (local (-122.3, 428.6, 2826.5), yaw 81, pitch 42.4) — the ribbon/
gap artifact's shape, position, and extent were pixel-for-pixel
IDENTICAL before and after both runs. Also confirmed via a world-
position diagnostic shader that wherever a terrain fragment DOES render,
its world position is smooth and continuous with no discontinuity —
the gaps are missing/degenerate triangles, not wrong-but-rendered ones.
Conclusion: height MAGNITUDE is not the root cause of this specific
artifact (contradicts the earlier pillar-bug theory, which may only
have applied to that investigation's own different repro location) —
most likely a genuine index-buffer/vertex-construction defect,
independent of height data. Do not re-attempt this exact fix without
new evidence; the heightmap was restored to its pre-erosion state
(tmp_/world_hmap_backup.r32 was the backup used for the revert).

This script itself is not broken — it's a working, verified,
mass-conserving talus-slope smoother, kept for any future case that
genuinely needs it (e.g. procedurally-generated terrain with real
too-steep synthetic spikes). It smooths only cell pairs whose height
difference exceeds --talus (metres, over the atlas's native 3.6m grid
spacing) — real mountains and ordinary steep slopes below the
threshold are left untouched.

Pipeline position: run AFTER tools/tif_to_r32.py (which produces
world_hmap.r32 from the raw Kenshi fullmap.tif) and BEFORE
tools/md_master_hmap_gen.py / tools/md_worldmap_gen.py (per CLAUDE.md's
"Terrain sync rule" — both must be re-run after this script touches
world_hmap.r32 too, since they derive from it).

Usage:
  python3 tools/md_terrain_erode.py                          # in-place, defaults
  python3 tools/md_terrain_erode.py --talus 12 --iterations 10 --rate 0.5
  python3 tools/md_terrain_erode.py --dry-run                # report only, no write
"""

import argparse
import struct
import sys
import time
import numpy as np
from pathlib import Path

ATLAS_MAGIC  = 0x414D4800
ATLAS_ZONES  = 64
ATLAS_VERTS  = 129   # 129x129 heights per zone; last row/col duplicates the next zone's first
G            = ATLAS_VERTS - 1  # 128 unique verts per zone per axis
FULL_SIZE    = ATLAS_ZONES * G + 1   # 8193 — includes the true unique edge row/col from the
                                     # last zone row/col (tif_to_r32.py samples vr=128 of zone 63
                                     # at the tif's own real final row, not a duplicate — truncating
                                     # to ATLAS_ZONES*G would silently drop that one real edge strip)


def load_atlas(path: str) -> np.ndarray:
    """TerrainAtlas binary -> float32 (FULL_SIZE, FULL_SIZE), one row-major
    world height field. Zones 0..62 contribute their non-duplicate 128x128
    interior; zone 63 (last row/col) contributes its full 129x129 block,
    since its own last row/col is real unique source data, not a duplicate
    of a following zone that doesn't exist."""
    full = np.zeros((FULL_SIZE, FULL_SIZE), dtype=np.float32)
    with open(path, 'rb') as f:
        magic, zx, zy, verts = struct.unpack('<4I', f.read(16))
        if magic != ATLAS_MAGIC:
            sys.exit(f"ERROR: bad magic 0x{magic:08X} (expected 0x{ATLAS_MAGIC:08X})")
        if zx != ATLAS_ZONES or zy != ATLAS_ZONES or verts != ATLAS_VERTS:
            sys.exit(f"ERROR: unexpected atlas dimensions {zx}x{zy}, verts={verts}")
        for zi in range(ATLAS_ZONES * ATLAS_ZONES):
            f.read(8)  # per-zone hmin/hmax — recomputed on save, skip here
            zone_h = np.frombuffer(f.read(ATLAS_VERTS * ATLAS_VERTS * 4),
                                    dtype=np.float32).reshape(ATLAS_VERTS, ATLAS_VERTS)
            zrow, zcol = zi // ATLAS_ZONES, zi % ATLAS_ZONES
            rn = ATLAS_VERTS if zrow == ATLAS_ZONES - 1 else G
            cn = ATLAS_VERTS if zcol == ATLAS_ZONES - 1 else G
            full[zrow*G:zrow*G+rn, zcol*G:zcol*G+cn] = zone_h[:rn, :cn]
    return full


def save_atlas(path: str, full: np.ndarray):
    """Write full (FULL_SIZE, FULL_SIZE) height field back into the
    TerrainAtlas format — each zone's last row/col mirrors the next zone's
    first row/col (edge-shared verts, true world edge zones use their own
    real last row/col from `full`, restored 1:1 by load_atlas above)."""
    with open(path, 'wb') as f:
        f.write(struct.pack('<IIII', ATLAS_MAGIC, ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS))
        for zi in range(ATLAS_ZONES * ATLAS_ZONES):
            zrow, zcol = zi // ATLAS_ZONES, zi % ATLAS_ZONES
            r0, c0 = zrow*G, zcol*G
            zone_h = full[r0:r0+ATLAS_VERTS, c0:c0+ATLAS_VERTS]
            hmin, hmax = float(zone_h.min()), float(zone_h.max())
            f.write(struct.pack('<ff', hmin, hmax))
            f.write(np.ascontiguousarray(zone_h).tobytes())


def erode_pass(h: np.ndarray, talus: float, rate: float) -> np.ndarray:
    """One iteration of mass-conserving talus-angle smoothing. Horizontal
    and vertical neighbour pairs are each processed exactly once (not per-
    direction) so flux subtracted from one side is added to the other with
    no double-counting -- total sum of h is unchanged by this function."""
    delta = np.zeros_like(h)

    diff_h = h[:, :-1] - h[:, 1:]
    excess_h = np.sign(diff_h) * np.clip(np.abs(diff_h) - talus, 0.0, None) * rate
    delta[:, :-1] -= excess_h
    delta[:, 1:]  += excess_h

    diff_v = h[:-1, :] - h[1:, :]
    excess_v = np.sign(diff_v) * np.clip(np.abs(diff_v) - talus, 0.0, None) * rate
    delta[:-1, :] -= excess_v
    delta[1:, :]  += excess_v

    return h + delta


def slope_stats(label: str, h: np.ndarray):
    dh = np.abs(h[:, :-1] - h[:, 1:])
    dv = np.abs(h[:-1, :] - h[1:, :])
    d = np.concatenate([dh.ravel(), dv.ravel()])
    print(f"  {label:22s}: max_cell_diff={d.max():.1f}m  p99.9={np.percentile(d, 99.9):.1f}m  "
          f"p99={np.percentile(d, 99):.1f}m  mass={h.sum():.3e}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input",      default="game/data/terrain/world_hmap.r32")
    ap.add_argument("--output",     default=None, help="default: overwrite --input")
    ap.add_argument("--talus",      type=float, default=12.0,
                    help="max allowed height diff (m) between adjacent 3.6m cells before erosion")
    ap.add_argument("--rate",       type=float, default=0.5, help="fraction of excess moved per pass")
    ap.add_argument("--iterations", type=int,   default=10)
    ap.add_argument("--dry-run",    action="store_true", help="report slope stats only, do not write")
    args = ap.parse_args()

    if not Path(args.input).exists():
        sys.exit(f"ERROR: input not found: {args.input}")
    out_path = args.output or args.input

    t0 = time.time()
    print(f"[load] {args.input} (TerrainAtlas {ATLAS_ZONES}x{ATLAS_ZONES} zones, "
          f"{FULL_SIZE}x{FULL_SIZE} world cells)...")
    h = load_atlas(args.input)
    slope_stats("before", h)

    print(f"[erode] talus={args.talus}m  rate={args.rate}  iterations={args.iterations}...")
    for i in range(args.iterations):
        h = erode_pass(h, args.talus, args.rate)
        if (i + 1) % max(1, args.iterations // 5) == 0 or i == args.iterations - 1:
            slope_stats(f"after pass {i+1}/{args.iterations}", h)

    if args.dry_run:
        print(f"[dry-run] no file written. Elapsed {time.time()-t0:.1f}s")
        return

    print(f"[save] {out_path} ...")
    save_atlas(out_path, h)
    print(f"Done in {time.time()-t0:.1f}s — re-run md_master_hmap_gen.py + "
          f"md_worldmap_gen.py (CLAUDE.md's Terrain sync rule).")


if __name__ == "__main__":
    main()
