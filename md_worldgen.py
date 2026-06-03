#!/usr/bin/env python3
"""
md_worldgen.py — Procedural world heightmap generator for Monkey Dust Engine.

Generates an original world_hmap.r32 (TerrainAtlas format, no Kenshi data).

Architecture:
  1. Macro layer  (64×64 zones):   large-scale geography (plateaus, basins, ranges)
  2. Meso  layer  (per-zone 65×65): regional hills/valleys within each zone
  3. Detail layer (per-zone 65×65): surface texture / dune/rock pattern
  Layers are blended per-biome; seams smoothed at zone borders.

Usage:
  python3 tools/md_worldgen.py [--seed N] [--preview]
"""

import argparse, struct, os, sys
import numpy as np
from scipy.ndimage import gaussian_filter
from PIL import Image

ATLAS_ZONES = 64
ATLAS_VERTS = 65
ATLAS_MAGIC = 0x414D4800
CHUNK_SIZE  = 500.0  # m per zone

# ── Biome profiles ────────────────────────────────────────────────────────────
#   amplitude : peak-to-valley height in metres
#   flatness  : 0=rough  1=perfectly flat
#   base_h    : sea-level offset (plateau height)
#   ridge     : fraction of ridge noise (sharp crests)
BIOME = {
    'desert':    dict(amplitude=50,  flatness=0.65, base_h=15, ridge=0.05),
    'canyon':    dict(amplitude=80,  flatness=0.35, base_h=35, ridge=0.40),
    'highlands': dict(amplitude=160, flatness=0.10, base_h=55, ridge=0.30),
    'highland':  dict(amplitude=130, flatness=0.15, base_h=45, ridge=0.25),
    'scrubland': dict(amplitude=35,  flatness=0.55, base_h=12, ridge=0.08),
    'swamp':     dict(amplitude=12,  flatness=0.80, base_h=3,  ridge=0.00),
    'volcanic':  dict(amplitude=100, flatness=0.20, base_h=45, ridge=0.45),
    'ashlands':  dict(amplitude=65,  flatness=0.38, base_h=30, ridge=0.20),
    'coast':     dict(amplitude=8,   flatness=0.88, base_h=1,  ridge=0.00),
    'unknown':   dict(amplitude=25,  flatness=0.60, base_h=8,  ridge=0.05),
}

# ── Parse terrain_config.txt ──────────────────────────────────────────────────

def load_zones(config_path):
    zones = {}
    cur = {}
    with open(config_path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith('#') or s.startswith('['):
                continue
            if s.startswith('zone='):
                if cur and 'grid_x' in cur and 'grid_z' in cur:
                    zones[(int(cur['grid_x']), int(cur['grid_z']))] = cur.copy()
                cur = {'id': s[5:].strip()}
            elif '=' in s and cur:
                k, v = s.split('=', 1)
                cur[k.strip()] = v.strip()
    if cur and 'grid_x' in cur and 'grid_z' in cur:
        zones[(int(cur['grid_x']), int(cur['grid_z']))] = cur.copy()
    return zones

# ── Per-zone noise (65×65) ────────────────────────────────────────────────────

def zone_fbm(rng, octaves=6, freq0=0.12, persist=0.50, lacun=2.0):
    """Generate 65×65 FBM noise for a single zone. freq0 in [0..1] units."""
    N   = ATLAS_VERTS
    out = np.zeros((N, N), dtype=np.float32)
    amp = 1.0; f = freq0; norm = 0.0
    for _ in range(octaves):
        gs  = max(3, int(N * f) + 2)
        g   = rng.random((gs, gs)).astype(np.float32)
        xs  = np.linspace(0, gs-1, N, endpoint=False)
        xi  = xs.astype(int); fx = xs - xi; xi1 = (xi+1) % gs
        R, C   = np.meshgrid(xs, xs, indexing='ij')
        RI     = R.astype(int); RF = R - RI; RI1 = (RI+1) % gs
        CI     = C.astype(int); CF = C - CI; CI1 = (CI+1) % gs
        v = (g[RI ,CI ]*(1-RF)*(1-CF) + g[RI ,CI1]*(1-RF)*CF +
             g[RI1,CI ]*   RF *(1-CF) + g[RI1,CI1]*   RF *CF)
        out += amp * v;  norm += amp;  amp *= persist;  f *= lacun
    return out / norm   # [0..1]

def ridge(h):
    return 1.0 - np.abs(h * 2.0 - 1.0)

# ── Build biome maps (zone-resolution 64×64) ──────────────────────────────────

def build_biome_maps(zones):
    """Build per-zone biome parameter maps.

    Priority order (highest → lowest):
      1. Exact zone position: use terrain_config amplitude × SCALE + biome flatness/ridge
      2. IDW from nearby zones (sigma=1.2 — sharp boundaries)
    """
    AMP_SCALE = 3.0   # scale terrain_config amplitude to visual metres
    keys = ['amplitude', 'flatness', 'base_h', 'ridge']
    maps = {k: np.zeros((ATLAS_ZONES, ATLAS_ZONES), dtype=np.float32) for k in keys}

    gxs  = np.array([p[0] for p in zones], dtype=np.float32)
    gzs  = np.array([p[1] for p in zones], dtype=np.float32)

    # Build source values: use terrain_config amplitude (scaled) + biome flatness/ridge
    amp_src = np.array([
        float(z.get('amplitude', BIOME.get(z.get('biome','unknown'),BIOME['unknown'])['amplitude']/AMP_SCALE))
        * AMP_SCALE
        for z in zones.values()], dtype=np.float32)
    flat_src = np.array([BIOME.get(z.get('biome','unknown'), BIOME['unknown'])['flatness']
                         for z in zones.values()], dtype=np.float32)
    base_src = np.array([BIOME.get(z.get('biome','unknown'), BIOME['unknown'])['base_h']
                         for z in zones.values()], dtype=np.float32)
    ridg_src = np.array([BIOME.get(z.get('biome','unknown'), BIOME['unknown'])['ridge']
                         for z in zones.values()], dtype=np.float32)
    src = {'amplitude': amp_src, 'flatness': flat_src, 'base_h': base_src, 'ridge': ridg_src}

    # IDW interpolation
    for cz in range(ATLAS_ZONES):
        for cx in range(ATLAS_ZONES):
            d2 = (gxs-cx)**2 + (gzs-cz)**2 + 0.01
            w  = 1.0 / d2; ws = w.sum()
            for k in keys:
                maps[k][cz, cx] = (w * src[k]).sum() / ws

    # Sharper blur — keep zone identities distinct
    for k in keys:
        maps[k] = gaussian_filter(maps[k], sigma=1.2)

    return maps

# ── Macro FBM at zone resolution (64×64) ─────────────────────────────────────

def macro_fbm(rng, octaves=6, freq0=0.08, persist=0.55, lacun=2.0):
    """World-scale FBM at zone resolution. Each value drives one zone's base."""
    N   = ATLAS_ZONES
    out = np.zeros((N, N), dtype=np.float32)
    amp = 1.0; f = freq0; norm = 0.0
    for _ in range(octaves):
        gs  = max(3, int(N * f) + 2)
        g   = rng.random((gs, gs)).astype(np.float32)
        xs  = np.linspace(0, gs-1, N, endpoint=False)
        xi  = xs.astype(int); xi1 = (xi+1) % gs; fx = xs - xi
        R, C  = np.meshgrid(xs, xs, indexing='ij')
        RI    = R.astype(int); RF = R - RI; RI1 = (RI+1) % gs
        CI    = C.astype(int); CF = C - CI; CI1 = (CI+1) % gs
        v = (g[RI ,CI ]*(1-RF)*(1-CF) + g[RI ,CI1]*(1-RF)*CF +
             g[RI1,CI ]*   RF *(1-CF) + g[RI1,CI1]*   RF *CF)
        out += amp * v;  norm += amp;  amp *= persist;  f *= lacun
    return out / norm

# ── Continental mask (zone-resolution) ────────────────────────────────────────

def continental_mask(cx=29.0, cz=26.0, rx=23.0, rz=20.0):
    """Elliptical mask: 1 inside zone cluster, fade to 0 beyond."""
    gz, gx = np.mgrid[0:ATLAS_ZONES, 0:ATLAS_ZONES]
    dist   = np.sqrt(((gx-cx)/rx)**2 + ((gz-cz)/rz)**2)
    mask   = np.clip(1.0 - (dist - 1.0) / 0.5, 0, 1).astype(np.float32)
    return gaussian_filter(mask, sigma=2.5)

# ── Main generation ───────────────────────────────────────────────────────────

def generate_world(seed=42):
    print(f"[worldgen] seed={seed}")
    rng_master = np.random.default_rng(seed)

    cfg = os.path.join(os.path.dirname(__file__), '..', 'game', 'data', 'terrain_config.txt')
    zones = load_zones(cfg) if os.path.exists(cfg) else {}
    print(f"[worldgen] {len(zones)} zones from terrain_config.txt")

    print("[worldgen] building biome maps…")
    bmaps = build_biome_maps(zones) if zones else {
        k: np.full((ATLAS_ZONES,ATLAS_ZONES), BIOME['unknown'][k], dtype=np.float32)
        for k in ['amplitude','flatness','base_h','ridge']}

    print("[worldgen] macro FBM (zone-resolution)…")
    macro  = macro_fbm(np.random.default_rng(seed),    octaves=7, freq0=0.06)
    macro2 = macro_fbm(np.random.default_rng(seed+1),  octaves=5, freq0=0.15)
    macro_ridge = ridge(macro_fbm(np.random.default_rng(seed+2), octaves=4, freq0=0.10))
    cont   = continental_mask()

    zones_h = np.zeros((ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS, ATLAS_VERTS), dtype=np.float32)

    print("[worldgen] generating per-zone heights…")
    for cz in range(ATLAS_ZONES):
        for cx in range(ATLAS_ZONES):
            A  = float(bmaps['amplitude'][cz, cx])
            F  = float(bmaps['flatness' ][cz, cx])
            B  = float(bmaps['base_h'   ][cz, cx])
            R  = float(bmaps['ridge'    ][cz, cx])
            CM = float(cont[cz, cx])

            if CM < 0.01:
                zones_h[cz, cx] = 0.0
                continue

            # Macro value for this zone (scalar → drives zone base elevation)
            m_val  = float(macro [cz, cx])   # 0..1 world topology
            m2_val = float(macro2[cz, cx])   # secondary large-scale
            mr_val = float(macro_ridge[cz, cx])  # ridgeline

            # Per-zone meso noise (65×65) — medium hills/valleys (~18m wavelength)
            # freq0=0.40 → gs≈28 nodes → 65/28≈2.3 verts/feature → good hills
            rng_meso = np.random.default_rng(int(rng_master.integers(0, 2**32)))
            h_meso   = zone_fbm(rng_meso, octaves=6, freq0=0.40, persist=0.55)

            # Per-zone detail noise — surface texture, dunes, rocks
            rng_det  = np.random.default_rng(int(rng_master.integers(0, 2**32)))
            h_det    = zone_fbm(rng_det,  octaves=4, freq0=0.80, persist=0.48)

            # Ridge pattern for sharp crests (highlands/canyon)
            h_ridge  = ridge(h_meso)

            # ── Combine layers ──────────────────────────────────────────────
            # Base: macro topology scalar → sets zone's overall elevation band
            base = m_val * 0.55 + m2_val * 0.30 + mr_val * 0.15 * R

            # Local relief: main source of visible hills within zone
            local_raw = h_meso * (1.0 - R) + h_ridge * R
            # Flatness compresses local range toward 0.5 (flat biomes → gentle)
            local = local_raw * (1.0 - F) + 0.5 * F

            # Detail: micro-roughness (dunes for desert, rocks for canyon)
            detail = h_det * 0.18 * (1.0 - F * 0.7)

            # Combine — give local more weight so hills are prominent
            h = base * 0.25 + local * 0.65 + detail

            # Scale to biome amplitude and offset
            h = h * A + B

            # Continental mask (ocean at edges)
            h = h * CM

            zones_h[cz, cx] = h.astype(np.float32)

        if cz % 8 == 0:
            print(f"[worldgen]   row {cz}/{ATLAS_ZONES}  "
                  f"(max so far: {zones_h[:cz+1].max():.0f}m)")

    # Smooth zone boundary seams
    print("[worldgen] smoothing seams…")
    for cz in range(ATLAS_ZONES - 1):
        for cx in range(ATLAS_ZONES - 1):
            b = (zones_h[cz, cx, :, -1] + zones_h[cz, cx+1, :,  0]) * 0.5
            zones_h[cz, cx,   :, -1] = b;  zones_h[cz, cx+1, :,  0] = b
            b = (zones_h[cz, cx, -1, :] + zones_h[cz+1, cx, 0, :]) * 0.5
            zones_h[cz, cx,  -1, :] = b;  zones_h[cz+1, cx, 0, :] = b

    print(f"[worldgen] height range: {zones_h.min():.1f} – {zones_h.max():.1f} m")
    return zones_h

# ── Write TerrainAtlas ────────────────────────────────────────────────────────

def write_atlas(zones_h, out_path):
    print(f"[worldgen] writing {out_path}…")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(struct.pack('<4I', ATLAS_MAGIC, ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS))
        for cz in range(ATLAS_ZONES):
            for cx in range(ATLAS_ZONES):
                z = zones_h[cz, cx]
                f.write(struct.pack('<ff', float(z.min()), float(z.max())))
                f.write(z.tobytes())
    print(f"[worldgen] wrote {os.path.getsize(out_path)/1024/1024:.1f} MB")

# ── Write preview PNG ─────────────────────────────────────────────────────────

BIOME_COLORS = {
    'desert':    (220,190,120), 'canyon':    (160,100, 70),
    'highlands': ( 90,110, 80), 'highland':  (100,115, 85),
    'scrubland': (150,155, 90), 'swamp':     ( 70,110, 80),
    'volcanic':  (100, 60, 50), 'ashlands':  (130,120,115),
    'coast':     (180,200,210), 'unknown':   (160,155,140),
}

def write_preview(zones_h, zones, out_path):
    V  = 8; SZ = ATLAS_ZONES * V
    img = np.zeros((SZ, SZ), dtype=np.float32)
    for cz in range(ATLAS_ZONES):
        for cx in range(ATLAS_ZONES):
            img[cz*V:(cz+1)*V, cx*V:(cx+1)*V] = zones_h[cz, cx].mean()
    lo, hi = img.min(), img.max()
    img = (img - lo) / max(hi - lo, 1.0)

    rgb = np.zeros((SZ, SZ, 3), dtype=np.uint8)
    rgb[img < 0.03] = (30, 60, 120)
    for t, col in [(0.03,(200,180,130)),(0.15,(155,140,100)),
                   (0.35,(110,105, 80)),(0.55,( 90, 85, 75)),
                   (0.75,( 75, 72, 68)),(0.90,(220,225,235))]:
        rgb[img >= t] = col

    for (gx,gz), z in zones.items():
        if gx < ATLAS_ZONES and gz < ATLAS_ZONES:
            r,c = gz*V+V//2, gx*V+V//2
            rgb[max(0,r-1):r+2, max(0,c-1):c+2] = (255,50,50)

    Image.fromarray(rgb,'RGB').save(out_path)
    print(f"[worldgen] preview → {out_path}")

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seed',    type=int, default=42)
    ap.add_argument('--preview', action='store_true')
    ap.add_argument('--out', default='game/data/terrain/world_hmap.r32')
    args = ap.parse_args()

    root     = os.path.join(os.path.dirname(__file__), '..')
    out_path = os.path.join(root, args.out)

    if os.path.exists(out_path):
        bak = out_path + '.bak'
        if not os.path.exists(bak):
            import shutil; shutil.copy2(out_path, bak)
            print(f"[worldgen] backed up old file → {bak}")

    zh = generate_world(seed=args.seed)
    write_atlas(zh, out_path)

    if args.preview:
        cfg  = os.path.join(root, 'game', 'data', 'terrain_config.txt')
        z    = load_zones(cfg) if os.path.exists(cfg) else {}
        prev = out_path.replace('.r32', '_preview.png')
        write_preview(zh, z, prev)

    print("[worldgen] done.")

if __name__ == '__main__':
    main()
