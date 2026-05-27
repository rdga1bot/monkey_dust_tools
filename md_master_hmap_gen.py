#!/usr/bin/env python3
"""
md_master_hmap_gen.py — Kenshi TerrainAtlas → simplified macro master heightmap.

Reads world_hmap.r32 (TerrainAtlas binary: 64×64 zones, 65×65 verts each,
heights in metres), reconstructs the full world heightmap, downsamples to a
small resolution with heavy Gaussian blur to keep only macro geography, then
saves md_master_hmap.r32 in a format the engine can load.

Output format (engine's s_load_r32):
  uint32  width
  uint32  height
  float32 hmin
  float32 hmax
  float32 width*height  (heights in metres, row-major)

Usage:
  python3 tools/md_master_hmap_gen.py           # defaults below
  python3 tools/md_master_hmap_gen.py --dst-size 512 --blur-sigma 8

Dependencies: numpy, scipy  (pip install numpy scipy)
"""

import argparse
import struct
import sys
import numpy as np
from pathlib import Path

ATLAS_MAGIC  = 0x414D4800
ATLAS_ZONES  = 64
ATLAS_VERTS  = 65          # 65×65 heights per zone (last row/col shared with neighbour)


def load_atlas(path: str) -> np.ndarray:
    """Read TerrainAtlas binary → float32 array (ATLAS_ZONES*64) × (ATLAS_ZONES*64)."""
    with open(path, 'rb') as f:
        magic, zx, zy, verts = struct.unpack('<4I', f.read(16))
        if magic != ATLAS_MAGIC:
            sys.exit(f"ERROR: bad magic 0x{magic:08X} (expected 0x{ATLAS_MAGIC:08X})")
        if zx != ATLAS_ZONES or zy != ATLAS_ZONES or verts != ATLAS_VERTS:
            sys.exit(f"ERROR: unexpected atlas dimensions {zx}×{zy}, verts={verts}")

        # Reconstruct full heightmap: use first 64×64 from each zone (skip duplicate edge)
        # Result: (ATLAS_ZONES*64) × (ATLAS_ZONES*64) = 4096×4096
        G = ATLAS_VERTS - 1  # 64 unique vertices per zone in each axis
        full_size = ATLAS_ZONES * G  # 4096
        full = np.zeros((full_size, full_size), dtype=np.float32)

        for zi in range(ATLAS_ZONES * ATLAS_ZONES):
            _hmin, _hmax = struct.unpack('<ff', f.read(8))
            zone_h = np.frombuffer(f.read(ATLAS_VERTS * ATLAS_VERTS * 4),
                                   dtype=np.float32).reshape(ATLAS_VERTS, ATLAS_VERTS)
            zrow, zcol = zi // ATLAS_ZONES, zi % ATLAS_ZONES
            # Copy only the non-duplicate 64×64 interior (exclude last row/col)
            full[zrow*G:(zrow+1)*G, zcol*G:(zcol+1)*G] = zone_h[:G, :G]

    return full


def downsample(hmap: np.ndarray, dst_size: int) -> np.ndarray:
    from scipy.ndimage import zoom
    factor = dst_size / hmap.shape[0]
    return zoom(hmap, factor, order=1).astype(np.float32)  # bilinear


def blur(hmap: np.ndarray, sigma: float) -> np.ndarray:
    from scipy.ndimage import gaussian_filter
    return gaussian_filter(hmap, sigma=sigma).astype(np.float32)


def save_r32(path: str, hmap: np.ndarray):
    h, w = hmap.shape
    hmin, hmax = float(hmap.min()), float(hmap.max())
    with open(path, 'wb') as f:
        f.write(struct.pack('<II', w, h))
        f.write(struct.pack('<ff', hmin, hmax))
        f.write(hmap.astype(np.float32).tobytes())
    print(f"[saved] {path}  ({w}×{h}, hmin={hmin:.1f}m, hmax={hmax:.1f}m)")


def stats(label: str, hmap: np.ndarray):
    flat = hmap.flatten()
    p95 = np.percentile(flat, 95)
    p99 = np.percentile(flat, 99)
    below60 = (flat < 60.0).mean() * 100
    print(f"  {label:18s}: avg={flat.mean():.1f}m  max={flat.max():.1f}m  "
          f"p95={p95:.1f}m  p99={p99:.1f}m  <60m={below60:.1f}%")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input",      default="game/data/terrain/world_hmap.r32")
    ap.add_argument("--output",     default="game/data/terrain/md_master_hmap.r32")
    ap.add_argument("--dst-size",   type=int,   default=256,
                    help="Output resolution (256 for macro geography)")
    ap.add_argument("--blur-sigma", type=float, default=10.0,
                    help="Gaussian sigma in OUTPUT pixels; higher = smoother macro")
    args = ap.parse_args()

    if not Path(args.input).exists():
        sys.exit(f"ERROR: input not found: {args.input}")

    print(f"[load] {args.input} (TerrainAtlas 64×64 zones × 65×65 verts)...")
    full = load_atlas(args.input)
    stats("source (4096×4096)", full)

    print(f"[downsample] → {args.dst_size}×{args.dst_size}...")
    small = downsample(full, args.dst_size)
    stats("downsampled", small)

    print(f"[blur] sigma={args.blur_sigma}...")
    blurred = blur(small, args.blur_sigma)
    np.clip(blurred, 0.0, None, out=blurred)
    stats("blurred (master)", blurred)

    save_r32(args.output, blurred)
    print("Done.")


if __name__ == "__main__":
    main()
