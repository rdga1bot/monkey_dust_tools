#!/usr/bin/env python3
"""
md_worldgen.py — Procedural world heightmap generator for Monkey Dust Engine.

Generates a completely original world_hmap.r32 in TerrainAtlas format.
Does NOT use any Kenshi/copyrighted data.

Zone biome/position metadata from terrain_config.txt is respected:
  - Each zone's biome drives local amplitude and terrain roughness
  - Zone grid positions seed biome interpolation across the full 64×64 atlas

Output: game/data/terrain/world_hmap.r32
Also writes: game/data/terrain/world_hmap_preview.png (visual check)

TerrainAtlas format:
  uint32 magic=0x414D4800, uint32 zx=64, uint32 zy=64, uint32 verts=65
  Per zone (4096 zones, row-major zy×zx):
    float32 hmin, float32 hmax, float32[65×65] heights in metres

Usage:
  python3 tools/md_worldgen.py [--seed N] [--preview]

Dependencies: numpy scipy Pillow (all available)
"""

import argparse
import struct
import os
import sys
import numpy as np
from scipy.ndimage import gaussian_filter
from PIL import Image

# ── Atlas constants (must match engine ATLAS_ZONES / ATLAS_VERTS) ─────────────
ATLAS_ZONES  = 64
ATLAS_VERTS  = 65       # 65×65 heights per zone (shared edge = seamless)
ATLAS_MAGIC  = 0x414D4800

CHUNK_SIZE   = 500.0    # metres per zone

# ── Biome height profiles ─────────────────────────────────────────────────────
# amplitude: max terrain height in metres
# flatness:  0=rough, 1=completely flat
# base_h:    sea-level offset (desert plateaus are elevated)
BIOME = {
    'desert':    dict(amplitude=45,  flatness=0.72, base_h=20.0, ridge=0.05),
    'canyon':    dict(amplitude=70,  flatness=0.40, base_h=30.0, ridge=0.30),
    'highlands': dict(amplitude=130, flatness=0.18, base_h=60.0, ridge=0.25),
    'highland':  dict(amplitude=110, flatness=0.22, base_h=50.0, ridge=0.20),
    'scrubland': dict(amplitude=28,  flatness=0.60, base_h=10.0, ridge=0.08),
    'swamp':     dict(amplitude=8,   flatness=0.90, base_h=2.0,  ridge=0.02),
    'volcanic':  dict(amplitude=85,  flatness=0.28, base_h=40.0, ridge=0.35),
    'ashlands':  dict(amplitude=55,  flatness=0.45, base_h=25.0, ridge=0.18),
    'coast':     dict(amplitude=6,   flatness=0.93, base_h=0.5,  ridge=0.01),
    'unknown':   dict(amplitude=20,  flatness=0.65, base_h=5.0,  ridge=0.05),
}

# ── Parse terrain_config.txt ──────────────────────────────────────────────────

def load_zones(config_path):
    zones = {}
    cur = {}
    with open(config_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or line.startswith('['):
                continue
            if line.startswith('zone='):
                if cur and 'grid_x' in cur and 'grid_z' in cur:
                    gx, gz = int(cur['grid_x']), int(cur['grid_z'])
                    zones[(gx, gz)] = cur.copy()
                cur = {'id': line[5:].strip()}
            elif '=' in line and cur:
                k, v = line.split('=', 1)
                cur[k.strip()] = v.strip()
    if cur and 'grid_x' in cur and 'grid_z' in cur:
        gx, gz = int(cur['grid_x']), int(cur['grid_z'])
        zones[(gx, gz)] = cur.copy()
    return zones

# ── Noise building blocks (pure numpy, no external noise lib) ─────────────────

def make_value_noise(rng, res, freq):
    """Tiled value noise at given frequency, bilinearly interpolated."""
    grid_sz = max(2, int(res * freq) + 2)
    grid = rng.random((grid_sz, grid_sz)).astype(np.float32)
    # Bilinear interpolation
    xs = np.linspace(0, grid_sz - 1, res, endpoint=False)
    zs = np.linspace(0, grid_sz - 1, res, endpoint=False)
    xi = xs.astype(int); fx = xs - xi
    zi = zs.astype(int); fz = zs - zi
    xi1 = (xi + 1) % grid_sz
    zi1 = (zi + 1) % grid_sz
    ZI, XI = np.meshgrid(zi, xi, indexing='ij')
    ZF, XF = np.meshgrid(fz, fx, indexing='ij')
    ZI1, XI1 = np.meshgrid(zi1, xi1, indexing='ij')
    v00 = grid[ZI,  XI ]; v01 = grid[ZI,  XI1]
    v10 = grid[ZI1, XI ]; v11 = grid[ZI1, XI1]
    return (v00*(1-XF)*(1-ZF) + v01*XF*(1-ZF) +
            v10*(1-XF)*ZF     + v11*XF*ZF)

def fbm(rng, res, octaves=6, freq0=0.004, persistence=0.5, lacunarity=2.0):
    """Fractional Brownian Motion — multi-octave layered noise."""
    out  = np.zeros((res, res), dtype=np.float32)
    amp  = 1.0
    freq = freq0
    norm = 0.0
    for _ in range(octaves):
        out  += amp * make_value_noise(rng, res, freq)
        norm += amp
        amp  *= persistence
        freq *= lacunarity
    return out / norm  # normalised [0, 1]

def domain_warp(rng, h, strength=0.10, seed_offset=100):
    """Domain warping: offset sample position by another noise field."""
    res  = h.shape[0]
    rng2 = np.random.default_rng(rng.integers(0, 2**32) + seed_offset)
    wx   = fbm(rng2, res, octaves=4, freq0=0.006) - 0.5
    rng3 = np.random.default_rng(rng.integers(0, 2**32) + seed_offset + 1)
    wz   = fbm(rng3, res, octaves=4, freq0=0.006) - 0.5
    # Warp: shift each pixel by (wx*strength*res, wz*strength*res)
    rows, cols = np.mgrid[0:res, 0:res]
    src_r = np.clip(rows + wz * strength * res, 0, res - 1)
    src_c = np.clip(cols + wx * strength * res, 0, res - 1)
    from scipy.ndimage import map_coordinates
    return map_coordinates(h, [src_r, src_c], order=1, mode='nearest').astype(np.float32)

def ridge_noise(h):
    """Convert FBM to ridge noise (sharp mountain ridges)."""
    return 1.0 - np.abs(h * 2.0 - 1.0)

# ── Build biome map (64×64, one value per atlas zone) ─────────────────────────

def build_biome_map(zones):
    """IDW interpolation of zone biomes → (64,64) per-zone biome parameters."""
    amp_map  = np.full((ATLAS_ZONES, ATLAS_ZONES), BIOME['unknown']['amplitude'], dtype=np.float32)
    flat_map = np.full((ATLAS_ZONES, ATLAS_ZONES), BIOME['unknown']['flatness'],  dtype=np.float32)
    base_map = np.full((ATLAS_ZONES, ATLAS_ZONES), BIOME['unknown']['base_h'],    dtype=np.float32)
    ridg_map = np.full((ATLAS_ZONES, ATLAS_ZONES), BIOME['unknown']['ridge'],     dtype=np.float32)

    # Collect anchor points
    anchors = []
    for (gx, gz), z in zones.items():
        if gx < 0 or gx >= ATLAS_ZONES or gz < 0 or gz >= ATLAS_ZONES:
            continue
        b = BIOME.get(z.get('biome', 'unknown'), BIOME['unknown'])
        anchors.append((gx, gz, b))

    if not anchors:
        return amp_map, flat_map, base_map, ridg_map

    gxs = np.array([a[0] for a in anchors], dtype=np.float32)
    gzs = np.array([a[1] for a in anchors], dtype=np.float32)
    amps  = np.array([a[2]['amplitude'] for a in anchors], dtype=np.float32)
    flats = np.array([a[2]['flatness']  for a in anchors], dtype=np.float32)
    bases = np.array([a[2]['base_h']    for a in anchors], dtype=np.float32)
    ridgs = np.array([a[2]['ridge']     for a in anchors], dtype=np.float32)

    for cz in range(ATLAS_ZONES):
        for cx in range(ATLAS_ZONES):
            dx  = gxs - cx
            dz  = gzs - cz
            d2  = dx*dx + dz*dz + 0.25  # avoid zero
            w   = 1.0 / d2
            ws  = w.sum()
            amp_map [cz, cx] = (w * amps ).sum() / ws
            flat_map[cz, cx] = (w * flats).sum() / ws
            base_map[cz, cx] = (w * bases).sum() / ws
            ridg_map[cz, cx] = (w * ridgs).sum() / ws

    return amp_map, flat_map, base_map, ridg_map

# ── Generate full world heightmap ─────────────────────────────────────────────

def generate_world(seed=42):
    print(f"[worldgen] seed={seed}")
    rng = np.random.default_rng(seed)

    # Full resolution: ATLAS_ZONES × (ATLAS_VERTS-1) + 1 = 64×64 + 1 = 4097, but we use
    # 64×64 internally and sample per-zone.  Generate at atlas-zone resolution first.
    FULL = ATLAS_ZONES * 64  # 4096 pixels (internal; sample per zone)

    print("[worldgen] generating macro FBM…")
    h_macro = fbm(rng, FULL, octaves=7, freq0=0.0015, persistence=0.55, lacunarity=2.1)
    h_macro = domain_warp(rng, h_macro, strength=0.08)

    print("[worldgen] generating detail FBM…")
    h_detail = fbm(rng, FULL, octaves=5, freq0=0.008, persistence=0.50, lacunarity=2.0)

    print("[worldgen] generating ridge FBM…")
    h_ridge_raw = fbm(rng, FULL, octaves=4, freq0=0.005, persistence=0.60, lacunarity=1.8)
    h_ridge = ridge_noise(h_ridge_raw)

    print("[worldgen] loading zone config…")
    cfg_path = os.path.join(os.path.dirname(__file__), '..', 'game', 'data', 'terrain_config.txt')
    zones = load_zones(cfg_path) if os.path.exists(cfg_path) else {}
    print(f"[worldgen] {len(zones)} zones loaded")

    print("[worldgen] building biome map…")
    amp_map, flat_map, base_map, ridg_map = build_biome_map(zones)

    # Smooth biome maps to avoid hard transitions
    amp_map  = gaussian_filter(amp_map,  sigma=3.0)
    flat_map = gaussian_filter(flat_map, sigma=3.0)
    base_map = gaussian_filter(base_map, sigma=3.0)
    ridg_map = gaussian_filter(ridg_map, sigma=3.0)

    # Continental mask: full strength inside zone cluster, fade to 0 beyond.
    # Zone config: gx=8..50, gz=8..44 → centre (29,26), half-extents (21,18).
    # Inner radius 1.0 = full height; fade from 1.0 to 1.6 → ocean.
    cy, cx = 26.0, 29.0
    gy, gx = np.mgrid[0:ATLAS_ZONES, 0:ATLAS_ZONES]
    dist = np.sqrt(((gx - cx) / 23.0)**2 + ((gy - cy) / 20.0)**2)
    cont_mask = np.clip(1.0 - (dist - 1.0) / 0.6, 0, 1).astype(np.float32)
    cont_mask = gaussian_filter(cont_mask, sigma=3.0)

    print("[worldgen] building atlas…")
    zones_h = np.zeros((ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS, ATLAS_VERTS), dtype=np.float32)

    # Sample full-res maps at per-zone resolution
    # For each zone (cz, cx), sample ATLAS_VERTS×ATLAS_VERTS grid within FULL
    from scipy.ndimage import map_coordinates

    for cz in range(ATLAS_ZONES):
        for cx2 in range(ATLAS_ZONES):
            # Pixel coords in FULL map for this zone's 65 vertices
            r0 = cz * 64;  r1 = r0 + 65
            c0 = cx2 * 64; c1 = c0 + 65

            # Sample each full-res field at this zone
            rows = np.linspace(r0, min(r0+63, FULL-1), ATLAS_VERTS)
            cols = np.linspace(c0, min(c0+63, FULL-1), ATLAS_VERTS)
            RR, CC = np.meshgrid(rows, cols, indexing='ij')

            m  = map_coordinates(h_macro,  [RR, CC], order=1, mode='nearest')
            d  = map_coordinates(h_detail, [RR, CC], order=1, mode='nearest')
            rd = map_coordinates(h_ridge,  [RR, CC], order=1, mode='nearest')

            # Biome parameters for this zone
            A  = float(amp_map [cz, cx2])
            F  = float(flat_map[cz, cx2])
            B  = float(base_map[cz, cx2])
            R  = float(ridg_map[cz, cx2])
            CM = float(cont_mask[cz, cx2])

            # Blend: macro sets large shape, detail adds texture, ridge adds mountains
            h = (m * (1.0 - F) + 0.5 * F)   # flatten effect
            h = h * (1.0 - R) + rd * R        # ridge blend
            h = h + d * 0.15 * (1.0 - F)      # detail texture

            # Apply amplitude and base
            h = h * A + B

            # Apply continental mask (edges → 0)
            h = h * CM

            zones_h[cz, cx2] = h.astype(np.float32)

        if cz % 8 == 0:
            print(f"[worldgen]   row {cz}/{ATLAS_ZONES}…")

    # Smooth seams between zones (3-vertex overlap blur at zone edges)
    print("[worldgen] smoothing zone seams…")
    for cz in range(ATLAS_ZONES - 1):
        for cx2 in range(ATLAS_ZONES - 1):
            # Blend right edge of (cz,cx2) with left edge of (cz,cx2+1)
            e0 = zones_h[cz, cx2,  :, -1]
            e1 = zones_h[cz, cx2+1,:,  0]
            blended = (e0 + e1) * 0.5
            zones_h[cz, cx2,   :, -1] = blended
            zones_h[cz, cx2+1, :,  0] = blended
            # Blend bottom edge of (cz,cx2) with top edge of (cz+1,cx2)
            e0 = zones_h[cz,   cx2, -1, :]
            e1 = zones_h[cz+1, cx2,  0, :]
            blended = (e0 + e1) * 0.5
            zones_h[cz,   cx2, -1, :] = blended
            zones_h[cz+1, cx2,  0, :] = blended

    return zones_h

# ── Write TerrainAtlas binary ─────────────────────────────────────────────────

def write_atlas(zones_h, out_path):
    print(f"[worldgen] writing {out_path}…")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(struct.pack('<4I', ATLAS_MAGIC, ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS))
        for cz in range(ATLAS_ZONES):
            for cx in range(ATLAS_ZONES):
                z = zones_h[cz, cx]
                hmin = float(z.min())
                hmax = float(z.max())
                f.write(struct.pack('<ff', hmin, hmax))
                f.write(z.tobytes())
    sz = os.path.getsize(out_path)
    print(f"[worldgen] wrote {sz/1024/1024:.1f} MB")

# ── Write preview PNG ─────────────────────────────────────────────────────────

def write_preview(zones_h, out_path, zones=None):
    print(f"[worldgen] writing preview {out_path}…")
    # Build flat full-res overview from zone 0,0 cells
    V = 8  # pixels per zone in preview
    SZ = ATLAS_ZONES * V
    img = np.zeros((SZ, SZ), dtype=np.float32)
    for cz in range(ATLAS_ZONES):
        for cx in range(ATLAS_ZONES):
            avg = float(zones_h[cz, cx].mean())
            img[cz*V:(cz+1)*V, cx*V:(cx+1)*V] = avg

    # Normalise to [0,255]
    lo, hi = img.min(), img.max()
    if hi > lo:
        img = (img - lo) / (hi - lo)
    img8 = (img * 255).astype(np.uint8)

    # Colorize: low=sand, mid=scrub, high=rock/snow
    rgb = np.zeros((SZ, SZ, 3), dtype=np.uint8)
    # ocean (< 5%)
    m = img < 0.05
    rgb[m] = [30, 60, 120]
    # sand (5-20%)
    m = (img >= 0.05) & (img < 0.20)
    v = ((img[m] - 0.05) / 0.15 * 255).astype(np.uint8)
    rgb[m] = [220, 180, 120]
    # scrub (20-40%)
    m = (img >= 0.20) & (img < 0.40)
    rgb[m] = [150, 130, 80]
    # highland (40-65%)
    m = (img >= 0.40) & (img < 0.65)
    rgb[m] = [100, 90, 70]
    # rock (65-85%)
    m = (img >= 0.65) & (img < 0.85)
    rgb[m] = [80, 75, 70]
    # snow (> 85%)
    m = img >= 0.85
    rgb[m] = [240, 240, 255]

    # Draw zone markers
    if zones:
        for (gx, gz), z in zones.items():
            if gx < ATLAS_ZONES and gz < ATLAS_ZONES:
                py = gz*V + V//2; px = gx*V + V//2
                rgb[max(0,py-1):py+2, max(0,px-1):px+2] = [255, 0, 0]

    Image.fromarray(rgb, 'RGB').save(out_path)
    print(f"[worldgen] preview saved: {out_path}")

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description='MD procedural world heightmap generator')
    ap.add_argument('--seed',    type=int, default=42,    help='RNG seed')
    ap.add_argument('--preview', action='store_true',      help='Write PNG preview')
    ap.add_argument('--out',     default='game/data/terrain/world_hmap.r32')
    args = ap.parse_args()

    root = os.path.join(os.path.dirname(__file__), '..')
    out_path = os.path.join(root, args.out)

    # Back up existing file
    if os.path.exists(out_path):
        bak = out_path + '.bak'
        if not os.path.exists(bak):
            import shutil; shutil.copy2(out_path, bak)
            print(f"[worldgen] backed up → {bak}")

    zones_h = generate_world(seed=args.seed)
    write_atlas(zones_h, out_path)

    if args.preview:
        cfg_path = os.path.join(root, 'game', 'data', 'terrain_config.txt')
        zones = load_zones(cfg_path) if os.path.exists(cfg_path) else {}
        prev_path = out_path.replace('.r32', '_preview.png')
        write_preview(zones_h, prev_path, zones)

    print("[worldgen] done.")

if __name__ == '__main__':
    main()
