#!/usr/bin/env python3
"""Stitch Kenshi colour.X.Y.png tiles (8x8 grid, 2048x2048 each) into
a single 16384x16384 overview texture for terrain rendering — pasted at
native resolution, no downsampling.

Output: game/data/textures/md_terrain.dds — BC3/DXT5-compressed DDS with a
full mip chain (16384 down to 1x1), via tools/md_bc3_encode.py's from-scratch
numpy encoder (no BC3 tool available in this environment — checked
nvcompress/compressonatorcli/texconv, all absent; Pillow can't write DXT).
2026-07-25: switched from the previous uncompressed-RGBA DDS (1GiB) to BC3
(4:1, ~256MiB) on explicit user request after weighing size against the
already-fast load — GPU-native BC3 decode at sample time costs nothing
extra vs uncompressed, same rationale as the existing ground-texture DDS
arrays (engine's GpuTexture::InitFromDDSArray) already using BC1/BC3.
Quality verified on a real Kenshi tile: mean abs error 2.7/255, visually
indistinguishable at native zoom — see md_bc3_encode.py's own doc comment.
Mip chain matters here specifically because BC-compressed textures can't
be GPU-auto-mipmapped (unlike the old uncompressed path's gen_mipmap=true) —
skipping mips would be a real regression (shimmering/aliasing at distance),
so every level down to 1x1 is pre-encoded.
Tile layout: X=col 0..7 (west→east), Y=row 0..7 (north→south)
"""
import os
from PIL import Image
from md_bc3_encode import encode_bc3_dds_with_mips

KENSHI_DIR = "/run/media/rdga1/win/SteamLibrary/steamapps/common/Kenshi"
TILES_DIR  = os.path.join(KENSHI_DIR, "data/newland/land/overlaymaps")
# Fallback: Steam mount is not always live (external drive) — tmp_/kenshi/ is
# a permanent local copy of the same real Kenshi data (see project docs on
# tmp_ vs temp_ conventions), used by the RE pipeline and confirmed to have
# all 64 colour.X.Y.png tiles at the same native 2048x2048 resolution.
if not os.path.isdir(TILES_DIR):
    TILES_DIR = "tmp_/kenshi/data/newland/land/overlaymaps"
OUT_PATH   = "game/data/textures/md_terrain.dds"
GRID       = 8        # 8×8 tiles
# 2026-07-24: was 512 (LANCZOS-downsampled from the real 2048x2048 source
# tiles, 16x fewer texels) — user-reported "unnatural"-looking terrain
# traced to this: real Kenshi's per-tile source data has fine geological
# detail (cracks, erosion mottling) discarded before it ever reached the
# GPU. User explicitly rejected any further downsampling ("не стискаємо
# взагалі") — tiles are now pasted at their native 2048x2048 resolution,
# no resize call at all. VRAM/RAM confirmed sufficient by user.
TILE_OUT   = 2048     # native resolution, no downsample
IMG_SIZE   = GRID * TILE_OUT  # 16384×16384

print(f"Stitching {GRID}×{GRID} Kenshi terrain tiles → {IMG_SIZE}×{IMG_SIZE} (native, no resize)...")

result = Image.new("RGB", (IMG_SIZE, IMG_SIZE))

for ty in range(GRID):
    for tx in range(GRID):
        path = os.path.join(TILES_DIR, f"colour.{tx}.{ty}.png")
        if not os.path.exists(path):
            print(f"  WARNING: missing {path}")
            continue
        tile = Image.open(path).convert("RGB")
        px = tx * TILE_OUT
        py = ty * TILE_OUT
        result.paste(tile, (px, py))
        print(f"  [{tx},{ty}] ok", end="\r", flush=True)

print(f"\nEncoding {IMG_SIZE}×{IMG_SIZE} → BC3 DDS with full mip chain (this takes a while)...")
import numpy as np
arr = np.array(result)  # (IMG_SIZE, IMG_SIZE, 3) uint8
dds_bytes = encode_bc3_dds_with_mips(arr)

os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
with open(OUT_PATH, "wb") as f:
    f.write(dds_bytes)
print(f"Saved: {OUT_PATH}  ({IMG_SIZE}×{IMG_SIZE} BC3/DXT5, {len(dds_bytes)} bytes, full mip chain)")
