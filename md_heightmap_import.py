#!/usr/bin/env python3
"""
kenshi_heightmap_import.py — extract per-zone R32 heightmaps from Kenshi fullmap.tif.

fullmap.tif layout:
  16385×16385 uint16, no compression
  64 zones × 256 pixels + 1 overlap pixel = 16385
  Zone (zx, zz) occupies pixels [zz*256 : zz*256+257, zx*256 : zx*256+257]
  Height in metres = pixel_value / 128.0

Output R32 layout (matches engine TerrainGen / TERRAIN_GRID=64):
  65×65 float32 binary (row-major, Y=0 is top/north)
  → game/data/terrain/zone_{zx}_{zz}.r32

Usage:
    python3 tools/md_heightmap_import.py            # all zones from terrain_config.txt
    python3 tools/md_heightmap_import.py 28 16      # single zone by grid coords
    python3 tools/md_heightmap_import.py --all      # all 64×64 zones

Run from repo root.
"""

import sys, os, struct, re, argparse
import numpy as np

FULLMAP = "/run/media/rdga1/win/SteamLibrary/steamapps/common/Kenshi/data/newland/land/fullmap.tif"
OUT_DIR  = "game/data/terrain"
GRID     = 64        # TERRAIN_GRID
SZ       = GRID + 1  # 65 vertices per side in R32
ZONE_PX  = 256       # pixels per zone in fullmap (+ 1 shared border)
SCALE    = 128.0     # uint16 / SCALE = metres

def load_fullmap():
    try:
        import tifffile
        print(f"[import] reading {FULLMAP} …", flush=True)
        arr = tifffile.imread(FULLMAP)
        assert arr.shape == (16385, 16385), f"unexpected shape {arr.shape}"
        print(f"[import] loaded {arr.shape} uint16  range {arr.min()}–{arr.max()}"
              f"  ({arr.min()/SCALE:.1f}–{arr.max()/SCALE:.1f} m)")
        return arr
    except ImportError:
        sys.exit("tifffile not installed — run: pip3 install tifffile --break-system-packages")

def extract_zone(arr, zx, zz):
    """Return 65×65 float32 heightmap in metres for zone (zx, zz)."""
    py0, px0 = zz * ZONE_PX, zx * ZONE_PX
    patch = arr[py0 : py0 + ZONE_PX + 1, px0 : px0 + ZONE_PX + 1]  # 257×257
    # Downsample 257→65: stride-4 (indices 0,4,8,…,256 = 65 values per axis)
    downsampled = patch[::4, ::4].astype(np.float32) / SCALE
    assert downsampled.shape == (SZ, SZ), f"bad shape {downsampled.shape}"
    return downsampled

def write_r32(hmap, zx, zz):
    """Write R32 with engine header: uint32 w, uint32 h, float hmin, float hmax, float[w*h]."""
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, f"zone_{zx}_{zz}.r32")
    h32 = hmap.astype(np.float32)
    with open(path, "wb") as f:
        f.write(struct.pack("<II", SZ, SZ))                    # width, height
        f.write(struct.pack("<ff", float(h32.min()), float(h32.max())))  # hmin, hmax
        h32.tofile(f)
    return path

def zones_from_config():
    cfg = open("game/data/terrain_config.txt").read()
    hits = re.findall(r'zone=(\w+).*?grid_x=(\d+).*?grid_z=(\d+)', cfg, re.DOTALL)
    return [(int(gx), int(gz), name) for name, gx, gz in hits]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all",  action="store_true", help="extract all 64×64 zones")
    ap.add_argument("zx",     nargs="?", type=int)
    ap.add_argument("zz",     nargs="?", type=int)
    args = ap.parse_args()

    arr = load_fullmap()

    if args.all:
        targets = [(zx, zz, f"zone_{zx}_{zz}") for zz in range(64) for zx in range(64)]
    elif args.zx is not None and args.zz is not None:
        targets = [(args.zx, args.zz, f"zone_{args.zx}_{args.zz}")]
    else:
        targets = zones_from_config()

    ok = 0
    for zx, zz, name in targets:
        if not (0 <= zx < 64 and 0 <= zz < 64):
            print(f"  SKIP ({zx},{zz}) out of range")
            continue
        hmap = extract_zone(arr, zx, zz)
        path = write_r32(hmap, zx, zz)
        print(f"  [{ok+1:3}/{len(targets)}] ({zx:2},{zz:2}) {name:25}  "
              f"h={hmap.min():.1f}–{hmap.max():.1f}m  → {path}")
        ok += 1

    print(f"\n[import] done — {ok}/{len(targets)} zones written to {OUT_DIR}/")

if __name__ == "__main__":
    main()
