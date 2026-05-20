#!/usr/bin/env python3
"""Stitch Kenshi colour.X.Y.png tiles (8x8 grid, 2048x2048 each) into
a single 4096x4096 overview texture for terrain rendering.

Output: game/data/textures/md_terrain.png
Tile layout: X=col 0..7 (west→east), Y=row 0..7 (north→south)
"""
import sys
import os
from PIL import Image

KENSHI_DIR = "/run/media/rdga1/win/SteamLibrary/steamapps/common/Kenshi"
TILES_DIR  = os.path.join(KENSHI_DIR, "data/newland/land/overlaymaps")
OUT_PATH   = "game/data/textures/md_terrain.png"
GRID       = 8        # 8×8 tiles
TILE_OUT   = 512      # downsampled size per tile
IMG_SIZE   = GRID * TILE_OUT  # 4096×4096

print(f"Stitching {GRID}×{GRID} Kenshi terrain tiles → {IMG_SIZE}×{IMG_SIZE}...")

result = Image.new("RGB", (IMG_SIZE, IMG_SIZE))

for ty in range(GRID):
    for tx in range(GRID):
        path = os.path.join(TILES_DIR, f"colour.{tx}.{ty}.png")
        if not os.path.exists(path):
            print(f"  WARNING: missing {path}")
            continue
        tile = Image.open(path).convert("RGB")
        tile = tile.resize((TILE_OUT, TILE_OUT), Image.LANCZOS)
        px = tx * TILE_OUT
        py = ty * TILE_OUT
        result.paste(tile, (px, py))
        print(f"  [{tx},{ty}] ok", end="\r", flush=True)

os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
result.save(OUT_PATH, "PNG", optimize=False)
print(f"\nSaved: {OUT_PATH}  ({IMG_SIZE}×{IMG_SIZE} RGB)")
