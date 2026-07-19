#!/usr/bin/env python3
"""
md_hmap_io.py — shared read/write helpers for monkey_dust's terrain
heightmap files (2026-07-19, replaces the old per-zone float32 .r32
TerrainAtlas binary — see CLAUDE.md "Terrain shading" history /
docs/NEXT_KENSHI_REAL_PLACEMENT_AND_SEAMS.md for why).

Kenshi's own source heightmap (fullmap.tif) is uint16 already — storing our
own derived heightmaps as float32 doubled the size (260MB) for zero real
precision gain. A single global HEIGHT_MAX_M scale round-trips losslessly
to/from that same uint16 precision.

world_hmap format, chosen per real measured trade-off (not guessed) after
comparing PNG16 (DEFLATE-compressed) against raw uint16: raw uint16 (.r16),
no compression. PNG16 was ~2.7x smaller (50.6MB vs 136.3MB) but ~5x SLOWER
to load in the real engine (1.79s vs 0.35s stb_image decode cost vs plain
fread) — this project's target hardware (Intel HD 520) has a documented
history of user-reported slow startup (CLAUDE.md task #158c, 7-8s blank
screen), so load time won over disk size here. Still less than half the
size of the original float32 .r32 (260MB).

The base file is never edited in-place by the engine — the editor's terrain
brush writes a separate small "*_edits.r32" overlay file instead
(engine/src/world/terrain_gen.cpp's TerrainAtlas_Save/Load).

2026-07-19: the macro-geography "md_master_hmap" layer (a blurred 256x256
PNG16, used only as a procedural-generation guide) was removed along with
the rest of the procedural terrain fallback — real Kenshi zone data covers
every in-bounds chunk, so there was never a real gap for it to fill. See
CLAUDE_STATE.md / docs/NEXT_KENSHI_REAL_PLACEMENT_AND_SEAMS.md.

world_hmap layout: one big (ATLAS_ZONES*ATLAS_VERTS)^2 = 8256x8256 grid,
tiled by zone — index [zy*ATLAS_VERTS + row, zx*ATLAS_VERTS + col] holds
zone (zx,zy)'s vertex (row,col). This mirrors the old .r32 atlas's
per-zone block layout exactly (including the redundant shared-border
vertex each neighbouring zone duplicates its own copy of) so engine-side
consumption code barely changed. See md_terrain_erode.py for an alternate
DE-DUPLICATED (ATLAS_ZONES*128+1)^2 continuous-grid representation used
for erosion math — a different, unrelated representation choice, not a
second file format.
"""
import numpy as np

ATLAS_ZONES  = 64
ATLAS_VERTS  = 129          # TERRAIN_GRID+1
PNG_SIZE     = ATLAS_ZONES * ATLAS_VERTS   # 8256, also the r16 grid size
# Must match engine/src/world/terrain_gen.cpp's TERRAIN_HEIGHT_SCALE_M
# exactly — the global uint16[0..65535] <-> metres[0..980] scale. See
# tools/tif_to_r32.py's own HEIGHT_MAX_M doc comment for where 980.0 comes
# from (Ogre Terrain::setTerrainScale, community save-file measurement).
HEIGHT_MAX_M = 980.0
# "MR16" LE — a distinct 8-byte header (magic + zone count) so a
# mismatched/corrupt file fails fast instead of silently misreading.
R16_MAGIC = 0x3631524D


def height_to_u16(h_m: np.ndarray) -> np.ndarray:
    """metres -> uint16, clamped to the representable [0, HEIGHT_MAX_M] range."""
    scaled = np.round(h_m * (65535.0 / HEIGHT_MAX_M))
    return np.clip(scaled, 0, 65535).astype(np.uint16)


def u16_to_height(u16: np.ndarray) -> np.ndarray:
    """uint16 -> metres (float32)."""
    return u16.astype(np.float32) * (HEIGHT_MAX_M / 65535.0)


def load_atlas_tiled(path: str) -> np.ndarray:
    """Read world_hmap.r16 -> float32 (ATLAS_ZONES, ATLAS_ZONES,
    ATLAS_VERTS, ATLAS_VERTS) array indexed [zy, zx, row, col], heights in
    metres."""
    with open(path, 'rb') as f:
        header = np.frombuffer(f.read(8), dtype=np.uint32)
        if header[0] != R16_MAGIC or header[1] != ATLAS_ZONES:
            raise ValueError(f"{path}: bad r16 header {header}, expected magic={R16_MAGIC:#x} zones={ATLAS_ZONES}")
        arr16 = np.frombuffer(f.read(), dtype=np.uint16).reshape(PNG_SIZE, PNG_SIZE)
    h_m = u16_to_height(arr16)
    return h_m.reshape(ATLAS_ZONES, ATLAS_VERTS, ATLAS_ZONES, ATLAS_VERTS).transpose(0, 2, 1, 3)


def save_atlas_tiled(path: str, tiled: np.ndarray):
    """Write a (ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS, ATLAS_VERTS) float32
    array indexed [zy, zx, row, col] (heights in metres) as world_hmap.r16
    (raw uint16, no compression — see module doc comment for why)."""
    assert tiled.shape == (ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS, ATLAS_VERTS), tiled.shape
    arr16 = height_to_u16(tiled).transpose(0, 2, 1, 3).reshape(PNG_SIZE, PNG_SIZE)
    with open(path, 'wb') as f:
        f.write(np.array([R16_MAGIC, ATLAS_ZONES], dtype=np.uint32).tobytes())
        f.write(np.ascontiguousarray(arr16).tobytes())
