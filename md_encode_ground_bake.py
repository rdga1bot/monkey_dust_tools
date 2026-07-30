#!/usr/bin/env python3
"""Encode tmp_/ground_bake_full.png (offline-baked flat-ground colour,
tools/md_bake_ground_layers.py) into game/data/textures/md_ground_baked.dds
-- BC3/DXT5 with full mip chain, same encoder/pattern as md_stitch_terrain.py.
"""
import os
from PIL import Image
import numpy as np
from md_bc3_encode import encode_bc3_dds_with_mips

IN_PATH  = "tmp_/ground_bake_full_v2.png"
OUT_PATH = "game/data/textures/md_ground_baked.dds"

print(f"Loading {IN_PATH}...")
im = Image.open(IN_PATH).convert("RGB")
arr = np.array(im)
print(f"Encoding {arr.shape[1]}x{arr.shape[0]} -> BC3 DDS with full mip chain...")
dds_bytes = encode_bc3_dds_with_mips(arr)

os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
with open(OUT_PATH, "wb") as f:
    f.write(dds_bytes)
print(f"Saved: {OUT_PATH} ({len(dds_bytes)/1024/1024:.1f} MiB)")
