#!/usr/bin/env python3
"""Convert Kenshi fullmap.tif to monkey_dust's world_hmap.r16 atlas.

TIF:   16385×16385 uint16, 256 px/zone, ~980m height range.
Atlas: 64×64 zones, 129×129 verts/zone, raw uint16 (no compression — was
       float32 .r32 until 2026-07-19, then briefly 16-bit PNG; see
       tools/md_hmap_io.py's doc comment: PNG was ~2.7x smaller but ~5x
       slower to load in the real engine, and this project's target
       hardware has a documented history of slow-startup complaints, so
       raw uint16 won — still under half the size of the original .r32).
Step:  256/128 = 2 px per vert — exact, no interpolation needed. Matches
       Kenshi's own real in-engine resolution (RE-confirmed: raw 258x258
       tile fetch downsampled internally to 129x129/zone,
       re_docs/kenshi/terrain.md) — was 65 verts/zone, coarser than Kenshi.

Usage:
  python3 tools/tif_to_r32.py                            # defaults
  python3 tools/tif_to_r32.py --tif <path> --out <path>  # custom paths
"""

import sys, argparse, time
import numpy as np
from PIL import Image
sys.path.insert(0, "tools")
from md_hmap_io import ATLAS_ZONES, ATLAS_VERTS, HEIGHT_MAX_M, save_atlas_tiled

Image.MAX_IMAGE_PIXELS = None

TIF_ZONE_PX = 256         # pixels per zone in fullmap.tif
STEP        = TIF_ZONE_PX // (ATLAS_VERTS - 1)   # = 2
# HEIGHT_MAX_M (from md_hmap_io, shared with the engine's TERRAIN_HEIGHT_
# SCALE_M): Ogre Terrain::setTerrainScale() vertical_scale=9800.0 world
# UNITS, /10 for Kenshi's engine unit (1 unit=0.1m decimetre) — confirmed
# against the real map size (29.491km, community save-file measurement;
# 294912/10=29491.2m matches to 5 sig figs). A prior version of this fix
# used 9800.0 directly (treating units as metres) — 10x too tall.

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tif", default="tmp_/kenshi/data/newland/land/fullmap.tif")
    ap.add_argument("--out", default="game/data/terrain/world_hmap.r16")
    args = ap.parse_args()

    t0 = time.time()
    print(f"[tif_to_r32] Loading {args.tif} ...")
    img = Image.open(args.tif)
    arr = np.array(img, dtype=np.float32)  # uint16 → float32
    print(f"  size: {img.size}  dtype: uint16  min: {arr.min():.0f}  max: {arr.max():.0f}")

    expected = ATLAS_ZONES * TIF_ZONE_PX + 1   # 16385
    assert arr.shape == (expected, expected), \
        f"Unexpected TIF size {arr.shape}, expected ({expected}, {expected})"

    # Scale uint16 [0..65535] → metres [0..HEIGHT_MAX_M]
    arr_m = arr * (HEIGHT_MAX_M / 65535.0)
    print(f"  height range: {arr_m.min():.1f}m .. {arr_m.max():.1f}m")

    # ── Vectorised extraction ────────────────────────────────────────────────
    # row_idx[zy, vr] = zy * TIF_ZONE_PX + vr * STEP
    # col_idx[zx, vc] = zx * TIF_ZONE_PX + vc * STEP
    zy_v = np.arange(ATLAS_ZONES, dtype=np.int32)
    vr_v = np.arange(ATLAS_VERTS, dtype=np.int32)
    row_idx = (zy_v[:, None] * TIF_ZONE_PX + vr_v[None, :] * STEP)   # (64, 65)

    zx_v = np.arange(ATLAS_ZONES, dtype=np.int32)
    vc_v = np.arange(ATLAS_VERTS, dtype=np.int32)
    col_idx = (zx_v[:, None] * TIF_ZONE_PX + vc_v[None, :] * STEP)   # (64, 65)

    # all_h[zy, vr, zx, vc] = arr_m[row_idx[zy,vr], col_idx[zx,vc]]
    print("  Sampling heights (vectorised)...")
    all_h = arr_m[row_idx[:, :, None, None],    # (64, 65,  1,  1)
                  col_idx[None, None, :, :]]     # ( 1,  1, 64, 65)
    # shape → (64, 65, 64, 65) = [zy, vr, zx, vc]

    # Reorder to [zy, zx, vr, vc] = (64, 64, 65, 65)
    all_h = all_h.transpose(0, 2, 1, 3).astype(np.float32)

    # ── Write tiled raw uint16 atlas ─────────────────────────────────────────
    print(f"  Writing {args.out} ...")
    save_atlas_tiled(args.out, all_h)

    elapsed = time.time() - t0
    import os
    size_mb = os.path.getsize(args.out) / 1048576
    print(f"Done in {elapsed:.1f}s — {args.out} ({size_mb:.1f} MB)")

if __name__ == "__main__":
    main()
