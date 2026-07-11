#!/usr/bin/env python3
"""Stitch Kenshi new_overlay.X.Y.png tiles (8x8 grid, 1024x1024 each) into
a single 4096x4096 RGBA world mask texture for terrain splat blending.

Channel roles (verified against terrainfp4.hlsl's computeBiome()/main_fs and
confirmed visually — R=organic blob mask, G=sparse noise, B=cracked/patchy
mask, A=clean connected thin-line network):
  R = grass presence mask   (merged with G via max() in the real shader)
  G = secondary/rare grass variant mask
  B = dirt presence mask
  A = road presence mask

This is a MASK, not colour — sampled in the fragment shader to select which
splat layer (grass/dirt/road) shows through over the procedural base/slope/
cliff layers, not multiplied in as tint (that's colour.X.Y.png's role, see
md_stitch_terrain.py / md_terrain.png).

Output: game/data/textures/md_overlay_mask.png
       game/data/textures/md_overlay_mask.raw (gitignored — regenerate here)
Tile layout: X=col 0..7 (west→east), Y=row 0..7 (north→south) — same 8x8
grid and orientation as colour.X.Y.png (md_stitch_terrain.py).

The .raw sidecar (8-byte <II width,height header + raw RGBA8 bytes) exists
because ClutterGen_LoadSources()'s stb_image PNG decode of this 4096x4096
image was measured taking ~760ms at every game startup (single-threaded
DEFLATE inflate) -- a plain fread of the raw bytes takes a few ms instead.
engine/src/world/clutter_gen.cpp's s_load_grass_density() prefers the .raw
file and falls back to decoding the .png if it's missing, so this script
is the ONLY place that needs to stay in sync -- run it again after any
change to the source Kenshi overlay tiles.
"""
import os
import struct
from PIL import Image

KENSHI_DIR = "tmp_/kenshi/data"
TILES_DIR  = os.path.join(KENSHI_DIR, "newland/land/overlaymaps")
OUT_PATH   = "game/data/textures/md_overlay_mask.png"
RAW_PATH   = "game/data/textures/md_overlay_mask.raw"
GRID       = 8        # 8x8 tiles
TILE_OUT   = 512      # downsampled size per tile (matches md_terrain.png's ratio)
IMG_SIZE   = GRID * TILE_OUT  # 4096x4096

print(f"Stitching {GRID}x{GRID} Kenshi overlay-mask tiles -> {IMG_SIZE}x{IMG_SIZE}...")

result = Image.new("RGBA", (IMG_SIZE, IMG_SIZE))

for ty in range(GRID):
    for tx in range(GRID):
        path = os.path.join(TILES_DIR, f"new_overlay.{tx}.{ty}.png")
        if not os.path.exists(path):
            print(f"  WARNING: missing {path}")
            continue
        tile = Image.open(path).convert("RGBA")
        # NEAREST, not LANCZOS: this is a mask (road channel is thin
        # connected lines) — smooth resampling filters would blur/lose the
        # road mask; a mask can tolerate blockiness far better than losing
        # thin features entirely.
        tile = tile.resize((TILE_OUT, TILE_OUT), Image.NEAREST)
        px = tx * TILE_OUT
        py = ty * TILE_OUT
        result.paste(tile, (px, py))
        print(f"  [{tx},{ty}] ok", end="\r", flush=True)

os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
result.save(OUT_PATH, "PNG", optimize=False)
print(f"Saved: {OUT_PATH}  ({IMG_SIZE}x{IMG_SIZE} RGBA)")

with open(RAW_PATH, "wb") as f:
    f.write(struct.pack("<II", IMG_SIZE, IMG_SIZE))
    f.write(result.tobytes())
print(f"Saved: {RAW_PATH}  (fast-load sidecar, gitignored)")
