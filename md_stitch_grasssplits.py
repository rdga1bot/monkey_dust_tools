#!/usr/bin/env python3
"""Stitch Kenshi grasssplits/fullmap.X.Y.raw density tiles into one combined
density map for ClutterGen_Build's real-data grass/clutter placement.

Format (RE'd this session, cross-verified against the real, confirmed-linked
Ogre PagedGeometry library — kenshi_x64.exe contains Forests::GrassLoader/
PagedGeometry RTTI + literal "..\\..\\source\\GrassLoader.cpp" paths, so this
is GrassLoader::setDensityMap()'s actual input, not a guess):
  fullmap.<zx>.<zy>.raw — 257x257 uint16 grid, one file per WorldRegistry
  zone (matches CHUNK_SIZE=460.8m step — same zone numbering terrain_config.txt
  uses). Locally available files cover only a 16x16 sub-rect of the full
  64x64 world (zx 10..25, zy 16..31) — NOT the whole map. Values are a
  smooth density FIELD (PagedGeometry's GrassLayer::setDensityMap() treats
  each pixel as a 0..1 probability that a candidate grass slot spawns
  there), not a discrete type-ID list — confirmed by comparing a
  near-empty zone (10,16: ~99.7% zero, few small values at patch edges)
  against a lush zone (22,25: continuous, nearly-full-range values almost
  everywhere) — same model, different fill level, not two formats.

Output: game/data/textures/md_grasssplits_density.raw — 8-byte header
  (int32 origin_zx, int32 origin_zy) + uint8 grid, W=nx*256+1, H=ny*256+1,
  normalized from uint16 via /65535*255. Consumed by clutter_gen.cpp's
  s_load_kenshi_grass_density()/s_kenshi_grass_density_at().
"""
import glob
import os
import re
import struct

import numpy as np

SRC_DIR = "tmp_/kenshi/data/land/grasssplits"
OUT_PATH = "game/data/textures/md_grasssplits_density.raw"
ZONE_PX = 256  # per-zone step (257 samples, last row/col shared with neighbour)


def main():
    files = glob.glob(os.path.join(SRC_DIR, "fullmap.*.raw"))
    if not files:
        print(f"[md_stitch_grasssplits] no files found in {SRC_DIR} — nothing to do")
        return

    coords = []
    for f in files:
        m = re.search(r"fullmap\.(\d+)\.(\d+)\.raw$", f)
        if m:
            coords.append((int(m.group(1)), int(m.group(2)), f))
    zx_min = min(c[0] for c in coords)
    zx_max = max(c[0] for c in coords)
    zy_min = min(c[1] for c in coords)
    zy_max = max(c[1] for c in coords)
    nx = zx_max - zx_min + 1
    ny = zy_max - zy_min + 1
    print(f"[md_stitch_grasssplits] {len(coords)} files, zone rect "
          f"x:[{zx_min}..{zx_max}] y:[{zy_min}..{zy_max}] ({nx}x{ny} zones)")

    W = nx * ZONE_PX + 1
    H = ny * ZONE_PX + 1
    combined = np.zeros((H, W), dtype=np.uint16)

    for zx, zy, f in coords:
        d = np.fromfile(f, dtype=np.uint16)
        if d.size != 257 * 257:
            print(f"  [skip] {f}: unexpected size {d.size}")
            continue
        d = d.reshape(257, 257)
        ox = (zx - zx_min) * ZONE_PX
        oy = (zy - zy_min) * ZONE_PX
        combined[oy:oy + 257, ox:ox + 257] = d

    # Normalize uint16 -> uint8 (0..255), simple linear /65535*255 — no
    # per-file rescaling (would break the cross-zone density comparison
    # PagedGeometry relies on: an empty zone must stay near-zero, not get
    # stretched to fill 0..255 on its own tiny local range).
    density_u8 = (combined.astype(np.float32) / 65535.0 * 255.0).astype(np.uint8)

    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "wb") as fh:
        fh.write(struct.pack("<ii", zx_min, zy_min))
        fh.write(density_u8.tobytes())

    nz = int(np.count_nonzero(density_u8))
    print(f"[md_stitch_grasssplits] wrote {OUT_PATH}: {W}x{H} u8, "
          f"origin=({zx_min},{zy_min}), {nz}/{W*H} nonzero ({100.0*nz/(W*H):.1f}%)")


if __name__ == "__main__":
    main()
