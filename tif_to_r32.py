#!/usr/bin/env python3
"""Convert Kenshi fullmap.tif to monkey_dust atlas .r32 format.

TIF:   16385×16385 uint16, 256 px/zone, ~300m height range.
Atlas: 64×64 zones, 65×65 verts/zone (ATLAS_ZBLOCK=4225), float32 metres.
Step:  256/64 = 4 px per vert — exact, no interpolation needed.

Usage:
  python3 tools/tif_to_r32.py                            # defaults
  python3 tools/tif_to_r32.py --tif <path> --out <path>  # custom paths
"""

import struct, sys, argparse, time
import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None

ATLAS_MAGIC = 0x414D4800
ATLAS_ZONES = 64
ATLAS_VERTS = 65          # TERRAIN_GRID+1
TIF_ZONE_PX = 256         # pixels per zone in fullmap.tif
STEP        = TIF_ZONE_PX // (ATLAS_VERTS - 1)   # = 4
HEIGHT_MAX_M = 300.0      # Kenshi world max confirmed height

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tif", default="/run/media/rdga1/win/SteamLibrary/steamapps/common/Kenshi/data/newland/land/fullmap.tif")
    ap.add_argument("--out", default="game/data/terrain/world_hmap.r32")
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

    # ── Write binary atlas ────────────────────────────────────────────────────
    print(f"  Writing {args.out} ...")
    with open(args.out, "wb") as f:
        # 16-byte header
        f.write(struct.pack("<IIII", ATLAS_MAGIC, ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS))
        for zy in range(ATLAS_ZONES):
            for zx in range(ATLAS_ZONES):
                h = all_h[zy, zx]          # shape (65, 65) float32
                hmin = float(h.min())
                hmax = float(h.max())
                f.write(struct.pack("<ff", hmin, hmax))
                f.write(h.tobytes())       # 65*65*4 = 16900 bytes

    elapsed = time.time() - t0
    size_mb = (16 + ATLAS_ZONES**2 * (8 + ATLAS_VERTS**2 * 4)) / 1048576
    print(f"Done in {elapsed:.1f}s — {args.out} ({size_mb:.1f} MB)")

if __name__ == "__main__":
    main()
