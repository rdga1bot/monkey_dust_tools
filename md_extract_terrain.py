#!/usr/bin/env python3
"""Extract per-zone R32 heightmap files from Kenshi fullmap.tif.

Kenshi world: 64×64 zones, each 500m × 500m.
fullmap.tif:  16385×16385 uint16 (zone_zx covers pixels [zx*256 .. zx*256+256]).
Output R32:   game/data/terrain/zone_ZX_ZY.r32
              Header: uint32 w=65, uint32 h=65, float hmin, float hmax
              Data:   65×65 float32 heights in metres (row-major, row=Z col=X)

Height scale: 300m max (fullmap max ≈ 65535 → 300m).
Vertex (col,row) samples fullmap at pixel (zx*256 + col*4, zy*256 + row*4).
"""
import sys, os, struct
import PIL.Image; PIL.Image.MAX_IMAGE_PIXELS = None
from PIL import Image
import numpy as np

KENSHI_DIR   = "/run/media/rdga1/win/SteamLibrary/steamapps/common/Kenshi"
FULLMAP_PATH = os.path.join(KENSHI_DIR, "data/newland/land/fullmap.tif")
ZONES_DIR    = os.path.join(KENSHI_DIR, "data/newland/leveldata/Newwworld")
OUT_DIR      = "game/data/terrain"
HEIGHT_MAX   = 300.0    # metres for value=65535
GRID         = 64       # TERRAIN_GRID in our engine (65×65 vertices)
PIXEL_STEP   = 4        # 256 pixels / 64 quads = 4 pixels per vertex

os.makedirs(OUT_DIR, exist_ok=True)

print("Loading fullmap.tif (16385×16385)...")
img = Image.open(FULLMAP_PATH)
arr = np.array(img, dtype=np.uint16)
print(f"  shape={arr.shape}  min={arr.min()}  max={arr.max()}")

# Extract ALL 64×64 zones from fullmap.tif — not just zones with .zone files.
# Zones with no playable content will have real height data (ocean/void areas stay at h≈0
# naturally from the fullmap), avoiding zero-height "cliffs" at zone borders.
zones = [(zx, zy) for zy in range(GRID) for zx in range(GRID)]
print(f"Extracting all {len(zones)} zones (full 64×64 grid from fullmap.tif)")

total = len(zones)
for i, (zx, zy) in enumerate(zones):
    out_path = os.path.join(OUT_DIR, f"zone_{zx}_{zy}.r32")

    # Vectorized pixel sample: col_idx in 0..64, row_idx in 0..64
    col_idx = np.arange(GRID + 1, dtype=np.int32)
    row_idx = np.arange(GRID + 1, dtype=np.int32)
    px_arr = np.clip(zx * 256 + col_idx * PIXEL_STEP, 0, 16384)
    py_arr = np.clip(zy * 256 + row_idx * PIXEL_STEP, 0, 16384)
    heights = (arr[np.ix_(py_arr, px_arr)].astype(np.float32) / 65535.0 * HEIGHT_MAX)

    hmin = float(heights.min())
    hmax = float(heights.max())

    with open(out_path, "wb") as f:
        f.write(struct.pack("<II", GRID+1, GRID+1))   # w=65, h=65
        f.write(struct.pack("<ff", hmin, hmax))
        f.write(heights.tobytes())

    print(f"  [{i+1}/{total}] zone_{zx}_{zy}.r32  h={hmin:.1f}..{hmax:.1f}m", end="\r", flush=True)

print(f"\nDone. {total} R32 files in {OUT_DIR}/")


def pack_atlas(out_path="game/data/terrain/world_hmap.r32"):
    """Pack all zone_X_Y.r32 into a single atlas file.

    Atlas format:
      uint32 magic=0x414D4800  uint32 zones_x=64  uint32 zones_y=64  uint32 zone_verts=65
      Then 64*64 zone blocks in (zy*64+zx) order:
        float hmin  float hmax  float[65*65] heights (row-major, row=Z col=X)
    Total: 16 + 4096*(8+65*65*4) = ~66 MB
    """
    MAGIC     = 0x414D4800
    ZONES     = 64
    VERTS     = GRID + 1       # 65
    ZBLOCK    = VERTS * VERTS  # 4225 floats
    ZERO_ZONE = struct.pack("<ff", 0.0, 0.0) + bytes(ZBLOCK * 4)

    print(f"Packing atlas → {out_path}")
    with open(out_path, "wb") as out:
        out.write(struct.pack("<IIII", MAGIC, ZONES, ZONES, VERTS))
        missing = 0
        for zy in range(ZONES):
            for zx in range(ZONES):
                src = os.path.join(OUT_DIR, f"zone_{zx}_{zy}.r32")
                if os.path.exists(src):
                    with open(src, "rb") as f:
                        w, h = struct.unpack("<II", f.read(8))
                        hmin, hmax = struct.unpack("<ff", f.read(8))
                        data = f.read(ZBLOCK * 4)
                    out.write(struct.pack("<ff", hmin, hmax))
                    out.write(data)
                else:
                    out.write(ZERO_ZONE)
                    missing += 1
            print(f"  row {zy+1}/64", end="\r", flush=True)
    sz = os.path.getsize(out_path)
    print(f"\nAtlas: {sz//1024//1024} MB  ({missing} missing zones → zeros)  → {out_path}")


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "pack_atlas":
    pack_atlas(sys.argv[2] if len(sys.argv) > 2 else "game/data/terrain/world_hmap.r32")
