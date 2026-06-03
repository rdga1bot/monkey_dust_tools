#!/usr/bin/env python3
"""
md_srtm_terrain.py — Build world_hmap.r32 from real NASA SRTM elevation data,
using a different real-world region for EACH biome from terrain_config.txt.

Each biome is sampled from a geographically appropriate location:
  desert    → Libyan Sahara       (flat, sandy, gently rolling dunes)
  canyon    → Wadi Rum, Jordan    (eroded sandstone cliffs, dramatic drops)
  volcanic  → Hoggar, Algeria     (volcanic massif, jagged peaks)
  swamp     → Mesopotamian marshes (flat delta, near sea-level)
  scrubland → Syrian steppe       (gently rolling open plains)
  highlands → Zagros Mts, Iran    (long high ridgelines, deep valleys)
  highland  → Zagros foothills    (same range, lower elevation band)
  ashlands  → Sinai plateau, Egypt (barren elevated plateau, harsh)
  coast     → Red Sea coast       (low coastal flat, gentle slopes)

Usage:
  python3 tools/md_srtm_terrain.py [--cfg game/data/terrain_config.txt]
                                    [--seed 42] [--preview] [--no-cache]
                                    [--out game/data/terrain/world_hmap.r32]
"""

import argparse, os, struct, math
import numpy as np
from scipy.ndimage import gaussian_filter, zoom as sci_zoom
from scipy.spatial import cKDTree
from PIL import Image

# ── Atlas constants ────────────────────────────────────────────────────────────
ATLAS_ZONES = 64
ATLAS_VERTS = 65
ATLAS_MAGIC = 0x414D4800

# ── Biome → SRTM source region (lat, lon, km_radius, description) ─────────────
BIOME_SRTM = {
    'desert':    (26.00, 15.00, 220, 'Libyan Sahara — flat sandy desert'),
    'canyon':    (29.60, 35.40, 120, 'Wadi Rum Jordan — eroded sandstone canyon'),
    'volcanic':  (23.30,  5.60, 200, 'Hoggar/Ahaggar Algeria — volcanic massif'),
    'swamp':     (31.20, 47.50, 180, 'Mesopotamian marshes Iraq — flat delta'),
    'scrubland': (36.50, 38.00, 200, 'Syrian steppe — gently rolling plains'),
    'highlands': (33.50, 47.00, 250, 'Zagros mountains Iran — high ridgelines'),
    'highland':  (32.00, 46.00, 200, 'Zagros foothills Iran — lower elevation'),
    'ashlands':  (29.00, 34.00, 160, 'Sinai plateau Egypt — barren elevated plateau'),
    'coast':     (28.50, 34.70, 150, 'Red Sea coast Jordan — low coastal terrain'),
}

# ── Biome → game height range [min_m, max_m] ──────────────────────────────────
BIOME_HEIGHT = {
    'desert':    (  0.0,  55.0),
    'canyon':    ( 40.0, 150.0),
    'volcanic':  ( 60.0, 200.0),
    'swamp':     (  0.0,  22.0),
    'scrubland': ( 15.0,  75.0),
    'highlands': ( 80.0, 200.0),
    'highland':  ( 50.0, 140.0),
    'ashlands':  ( 30.0,  90.0),
    'coast':     (  0.0,  38.0),
    '_default':  (  0.0, 120.0),
}

# ── Biome boundary blend radius (in zone cells) ────────────────────────────────
BLEND_RADIUS = 4.0   # zones; wider = softer biome transitions

def deg_per_km_lat():         return 1.0 / 111.0
def deg_per_km_lon(lat):      return 1.0 / (111.0 * math.cos(math.radians(lat)))


# ── SRTM download (with per-biome numpy cache) ─────────────────────────────────

def srtm_cache_path(biome, cache_dir):
    return os.path.join(cache_dir, f'srtm_{biome}.npy')


def download_srtm_grid(biome, center_lat, center_lon, region_km,
                       n=ATLAS_ZONES, cache_dir=None):
    if cache_dir:
        cp = srtm_cache_path(biome, cache_dir)
        if os.path.exists(cp):
            grid = np.load(cp)
            print(f"[srtm] {biome:12s} — loaded from cache ({grid.min():.0f}–{grid.max():.0f}m)")
            return grid

    import srtm as _srtm
    print(f"[srtm] {biome:12s} — downloading {region_km}km area around "
          f"({center_lat:.1f}°N, {center_lon:.1f}°E) …")
    data   = _srtm.get_data()
    hlat   = (region_km / 2.0) * deg_per_km_lat()
    hlon   = (region_km / 2.0) * deg_per_km_lon(center_lat)
    ls, lo = center_lat - hlat, center_lon - hlon
    dlat, dlon = 2*hlat/n, 2*hlon/n

    grid = np.zeros((n, n), dtype=np.float32)
    for r in range(n):
        lat = ls + r * dlat
        for c in range(n):
            h = data.get_elevation(lat, lo + c * dlon)
            grid[r, c] = float(h) if h is not None else 0.0
        if r % 16 == 0:
            print(f"[srtm]   {biome}: row {r}/{n}")

    # Fill SRTM voids (no-data pixels)
    voids = (grid == 0)
    if voids.sum() > 0:
        from scipy.ndimage import distance_transform_edt
        ind = distance_transform_edt(voids, return_distances=False, return_indices=True)
        grid[voids] = grid[tuple(ind[:, voids])]

    print(f"[srtm] {biome:12s} — done: {grid.min():.0f}–{grid.max():.0f}m")
    if cache_dir:
        os.makedirs(cache_dir, exist_ok=True)
        np.save(srtm_cache_path(biome, cache_dir), grid)
    return grid


# ── terrain_config.txt parser ─────────────────────────────────────────────────

def load_zone_biomes(cfg_path):
    """Returns {(grid_x, grid_z): biome_str}."""
    zones, cur = {}, {}
    with open(cfg_path) as f:
        for raw in f:
            s = raw.strip()
            if s.startswith('zone='):
                if cur.get('gx') is not None and cur.get('biome'):
                    zones[(cur['gx'], cur.get('gz', 0))] = cur['biome']
                cur = {}
            elif '=' in s and not s.startswith('#'):
                k, v = s.split('=', 1)
                k, v = k.strip(), v.strip()
                if k == 'grid_x':  cur['gx']    = int(v)
                elif k == 'grid_z': cur['gz']   = int(v)
                elif k == 'biome':  cur['biome'] = v
    if cur.get('gx') is not None and cur.get('biome'):
        zones[(cur['gx'], cur.get('gz', 0))] = cur['biome']
    return zones


# ── Voronoi biome fill ────────────────────────────────────────────────────────

def voronoi_biome_grid(zone_biomes, grid_size=ATLAS_ZONES):
    """Return (grid_size×grid_size) array of biome strings via Voronoi fill."""
    pts  = np.array([(gz, gx) for (gx, gz) in zone_biomes if 0 <= gx < grid_size and 0 <= gz < grid_size])
    biom = np.array([zone_biomes[(gx, gz)] for (gx, gz) in zone_biomes if 0 <= gx < grid_size and 0 <= gz < grid_size])
    tree = cKDTree(pts)
    yi, xi = np.mgrid[0:grid_size, 0:grid_size]
    coords = np.stack([yi.ravel(), xi.ravel()], axis=1)
    _, idx = tree.query(coords)
    return biom[idx].reshape(grid_size, grid_size)


# ── Normalise SRTM to game height range ───────────────────────────────────────

def normalize_srtm(grid, lo, hi, p_lo=2, p_hi=98):
    """Stretch percentile range [p_lo..p_hi] of grid to [lo..hi]."""
    vlo = float(np.percentile(grid, p_lo))
    vhi = float(np.percentile(grid, p_hi))
    if vhi <= vlo: vhi = vlo + 1.0
    n = np.clip((grid - vlo) / (vhi - vlo), 0.0, 1.0)
    return n * (hi - lo) + lo


# ── Build 64×64 macro elevation grid from biome SRTM data ────────────────────

def build_macro_grid(biome_grid, srtm_grids):
    """
    Compose the 64×64 macro elevation grid:
      1. Assign each cell its biome's normalised SRTM height.
      2. Compute distance-from-nearest-other-biome for each cell.
      3. Blend across biome boundaries (Gaussian-weighted transition).
    """
    G = ATLAS_ZONES
    unique_biomes = list(dict.fromkeys(biome_grid.ravel()))   # preserve order

    # Per-biome normalised height grids
    height_layers = {}
    for b in unique_biomes:
        sg = srtm_grids.get(b)
        if sg is None:
            print(f"[build] WARNING: no SRTM data for biome '{b}' — using flat 50m")
            sg = np.full((G, G), 50.0, dtype=np.float32)
        lo, hi = BIOME_HEIGHT.get(b, BIOME_HEIGHT['_default'])
        height_layers[b] = normalize_srtm(sg, lo, hi)

    # Biome index grid
    biome_list = sorted(set(biome_grid.ravel()))
    b2i = {b: i for i, b in enumerate(biome_list)}
    idx_grid = np.vectorize(b2i.get)(biome_grid).astype(np.int32)

    # Blended macro: for each biome, weight = exp(-dist²/(2σ²))
    sigma  = BLEND_RADIUS
    macro  = np.zeros((G, G), dtype=np.float32)
    w_sum  = np.zeros((G, G), dtype=np.float32)

    for b in biome_list:
        bi  = b2i[b]
        mask = (idx_grid == bi).astype(np.float32)
        # Gaussian blur of the mask gives the blending weight
        w   = gaussian_filter(mask, sigma=sigma)
        macro  += height_layers[b] * w
        w_sum  += w

    macro /= np.maximum(w_sum, 1e-6)

    # Final light smoothing to remove any pixel-level discontinuities
    macro = gaussian_filter(macro, sigma=1.5)
    macro = np.maximum(macro, 0.0)
    return macro


# ── Global FBM detail noise ────────────────────────────────────────────────────

def global_fbm(rng, res, octaves=7, freq0=0.006, persist=0.52):
    out = np.zeros((res, res), dtype=np.float32)
    amp = 1.0; f = freq0; norm = 0.0
    for _ in range(octaves):
        gs  = max(3, int(res * f) + 2)
        g   = rng.random((gs, gs)).astype(np.float32)
        xs  = np.linspace(0, gs-1, res, endpoint=False)
        R, C  = np.meshgrid(xs, xs, indexing='ij')
        RI  = R.astype(int); RF = R-RI; RI1 = (RI+1) % gs
        CI  = C.astype(int); CF = C-CI; CI1 = (CI+1) % gs
        v = (g[RI,CI]*(1-RF)*(1-CF) + g[RI,CI1]*(1-RF)*CF +
             g[RI1,CI]*RF*(1-CF)    + g[RI1,CI1]*RF*CF)
        out += amp * v; norm += amp; amp *= persist; f *= 2.0
    return out / norm


# ── Build full zone atlas from 64×64 macro ────────────────────────────────────

def build_atlas_from_macro(macro_64, biome_grid, seed=42):
    rng    = np.random.default_rng(seed)
    target = ATLAS_ZONES * (ATLAS_VERTS - 1) + 1          # 4097

    print(f"[build] upsampling macro 64×64 → {target}×{target}…")
    macro_full = sci_zoom(macro_64, target / ATLAS_ZONES, order=3)
    if macro_full.shape[0] < target:
        macro_full = np.pad(macro_full,
                            ((0, target - macro_full.shape[0]),
                             (0, target - macro_full.shape[1])), mode='edge')
    macro_full = gaussian_filter(macro_full, sigma=2.0)

    print("[build] generating global detail noise…")
    detail = global_fbm(rng, target)
    slope  = (np.abs(np.gradient(macro_full, axis=0)) +
              np.abs(np.gradient(macro_full, axis=1)))
    slope_norm  = np.clip(slope / (slope.max() + 1e-6), 0, 1)
    detail_amp  = 4.0 + slope_norm * 18.0                 # 4m flat, 22m steep
    combined = macro_full + (detail - 0.5) * 2.0 * detail_amp
    combined = np.maximum(combined, 0.0)

    print("[build] extracting per-zone tiles…")
    zones_h = np.zeros((ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS, ATLAS_VERTS),
                       dtype=np.float32)
    for cz in range(ATLAS_ZONES):
        for cx in range(ATLAS_ZONES):
            r0 = cz * (ATLAS_VERTS - 1)
            c0 = cx * (ATLAS_VERTS - 1)
            zones_h[cz, cx] = combined[r0:r0+ATLAS_VERTS, c0:c0+ATLAS_VERTS]
        if cz % 16 == 0:
            print(f"[build]   row {cz}/{ATLAS_ZONES}")

    # Verify seams are zero (shared-edge invariant)
    max_seam = 0.0
    for cz in range(ATLAS_ZONES - 1):
        for cx in range(ATLAS_ZONES - 1):
            max_seam = max(max_seam,
                           float(np.abs(zones_h[cz,cx,-1,:] - zones_h[cz+1,cx,0,:]).max()),
                           float(np.abs(zones_h[cz,cx,:,-1] - zones_h[cz,cx+1,:,0]).max()))
    print(f"[build] max seam error: {max_seam:.4f}m  (target: 0.0)")
    print(f"[build] height range: {zones_h.min():.1f} – {zones_h.max():.1f} m")
    return zones_h


# ── TerrainAtlas writer ───────────────────────────────────────────────────────

def write_atlas(zones_h, out_path):
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    print(f"[write] {out_path} …")
    with open(out_path, 'wb') as f:
        f.write(struct.pack('<4I', ATLAS_MAGIC, ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS))
        for cz in range(ATLAS_ZONES):
            for cx in range(ATLAS_ZONES):
                z = zones_h[cz, cx]
                f.write(struct.pack('<ff', float(z.min()), float(z.max())))
                f.write(z.tobytes())
    size_mb = os.path.getsize(out_path) / 1024 / 1024
    print(f"[write] {size_mb:.1f} MB written")


# ── Hillshaded preview PNG ────────────────────────────────────────────────────

BIOME_COLOR = {
    'desert':    (210, 185, 100),
    'canyon':    (175, 110,  65),
    'volcanic':  ( 80,  70,  70),
    'swamp':     ( 60, 110,  75),
    'scrubland': (140, 165, 100),
    'highlands': (105, 100,  85),
    'highland':  (120, 115,  90),
    'ashlands':  (120, 100,  80),
    'coast':     ( 80, 140, 155),
    '_default':  (150, 150, 140),
}

def write_preview(zones_h, biome_grid, out_path):
    V  = 8
    SZ = ATLAS_ZONES * V
    avg = np.zeros((SZ, SZ), dtype=np.float32)
    for cz in range(ATLAS_ZONES):
        for cx in range(ATLAS_ZONES):
            avg[cz*V:(cz+1)*V, cx*V:(cx+1)*V] = zones_h[cz, cx].mean()

    # Biome color base
    rgb = np.zeros((SZ, SZ, 3), dtype=np.float32)
    for cz in range(ATLAS_ZONES):
        for cx in range(ATLAS_ZONES):
            b   = biome_grid[cz, cx]
            col = BIOME_COLOR.get(b, BIOME_COLOR['_default'])
            rgb[cz*V:(cz+1)*V, cx*V:(cx+1)*V] = col

    # Hillshade
    dz = np.gradient(avg, axis=0)
    dx = np.gradient(avg, axis=1)
    shade = gaussian_filter(
        np.clip(1.0 / np.sqrt(dx*dx + dz*dz + 1.0) * 0.8 + 0.35, 0.3, 1.4),
        sigma=1.5)
    for c in range(3):
        rgb[:,:,c] = np.clip(rgb[:,:,c] * shade, 0, 255)

    Image.fromarray(rgb.astype(np.uint8), 'RGB').save(out_path)
    print(f"[preview] {out_path}")


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cfg',      default='game/data/terrain_config.txt')
    ap.add_argument('--out',      default='game/data/terrain/world_hmap.r32')
    ap.add_argument('--seed',     type=int, default=42)
    ap.add_argument('--preview',  action='store_true')
    ap.add_argument('--no-cache', action='store_true',
                    help='Re-download SRTM tiles even if cache exists')
    args = ap.parse_args()

    root      = os.path.join(os.path.dirname(__file__), '..')
    cfg_path  = os.path.join(root, args.cfg)
    out_path  = os.path.join(root, args.out)
    cache_dir = os.path.join(root, 'tmp_md', 'srtm_cache') if not args.no_cache else None

    # ── 1. Parse zone biome map ───────────────────────────────────────────────
    print(f"[config] loading {cfg_path} …")
    zone_biomes = load_zone_biomes(cfg_path)
    active_biomes = sorted(set(zone_biomes.values()))
    print(f"[config] {len(zone_biomes)} zones, {len(active_biomes)} biomes: "
          f"{', '.join(active_biomes)}")

    # ── 2. Voronoi fill → full 64×64 biome grid ───────────────────────────────
    print("[voronoi] filling 64×64 biome grid …")
    biome_grid = voronoi_biome_grid(zone_biomes)
    counts = {b: int((biome_grid == b).sum()) for b in active_biomes}
    print("[voronoi] biome cell counts:")
    for b, n in sorted(counts.items(), key=lambda x: -x[1]):
        print(f"  {b:12s} {n:4d} cells ({n*100/ATLAS_ZONES**2:.1f}%)")

    # ── 3. Download SRTM for each active biome ─────────────────────────────────
    srtm_grids = {}
    for b in active_biomes:
        if b not in BIOME_SRTM:
            print(f"[srtm] WARNING: no SRTM preset for biome '{b}', will use flat 50m")
            srtm_grids[b] = None
            continue
        lat, lon, km, desc = BIOME_SRTM[b]
        print(f"\n[srtm] === {b} ({desc}) ===")
        srtm_grids[b] = download_srtm_grid(b, lat, lon, km, n=ATLAS_ZONES,
                                            cache_dir=cache_dir)

    # ── 4. Backup existing output ─────────────────────────────────────────────
    if os.path.exists(out_path):
        import shutil
        bak = out_path + '.bak'
        if not os.path.exists(bak):
            shutil.copy2(out_path, bak)
            print(f"\n[backup] {bak}")

    # ── 5. Build blended macro grid ───────────────────────────────────────────
    print("\n[build] compositing biome terrain …")
    macro = build_macro_grid(biome_grid, srtm_grids)
    print(f"[build] macro range: {macro.min():.1f} – {macro.max():.1f} m")

    # ── 6. Build full atlas with detail noise ─────────────────────────────────
    zones_h = build_atlas_from_macro(macro, biome_grid, seed=args.seed)

    # ── 7. Write TerrainAtlas ─────────────────────────────────────────────────
    write_atlas(zones_h, out_path)

    # ── 8. Optional preview ───────────────────────────────────────────────────
    if args.preview:
        prev = out_path.replace('.r32', '_preview.png')
        write_preview(zones_h, biome_grid, prev)

    print("\n[done] Next steps:")
    print("  python3 tools/md_master_hmap_gen.py   # optional: regen master hmap")
    print("  python3 tools/md_worldmap_gen.py      # regen world map PNG")
    print(f"  ninja -C build monkey_dust            # rebuild + launch to verify terrain")


if __name__ == '__main__':
    main()
