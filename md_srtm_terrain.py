#!/usr/bin/env python3
"""
md_srtm_terrain.py — Build world_hmap.r32 from real NASA SRTM elevation data.

Downloads public-domain SRTM3 tiles (90m resolution) for a chosen region,
blends with high-frequency procedural noise for sub-90m detail, outputs
a TerrainAtlas compatible with monkey_dust engine.

Preset regions (--region flag):
  hoggar    : Hoggar/Ahaggar mountains, Algeria (volcanic peaks + flat Sahara)
  zagros    : Zagros mountains, Iran/Iraq (long ridges + plateau)
  atlas     : Atlas mountains, Morocco (dramatic ranges)
  sinai     : Sinai peninsula, Egypt (triangular peninsula, desert + peaks)
  karakum   : Karakum desert, Turkmenistan (flat with gentle dunes)
  oman      : Hajar mountains, Oman (dramatic coast + interior)

Usage:
  python3 tools/md_srtm_terrain.py [--region hoggar] [--seed 42] [--preview]
"""

import argparse, os, struct, math
import numpy as np
from scipy.ndimage import gaussian_filter, zoom
from PIL import Image

# ── Atlas format ──────────────────────────────────────────────────────────────
ATLAS_ZONES  = 64
ATLAS_VERTS  = 65
ATLAS_MAGIC  = 0x414D4800
CHUNK_SIZE   = 500.0  # m per zone
WORLD_KM     = ATLAS_ZONES * CHUNK_SIZE / 1000.0  # 32 km

# ── Presets: (center_lat, center_lon, region_km, description) ────────────────
REGIONS = {
    'hoggar':  (23.30,  5.60, 200, 'Hoggar/Ahaggar — volcanic peaks + flat Sahara'),
    'zagros':  (33.50, 47.00, 250, 'Zagros mountains — long ridgelines, Iran'),
    'atlas':   (31.50, -5.00, 180, 'Atlas mountains — dramatic N.Africa ridges'),
    'sinai':   (29.00, 34.00, 160, 'Sinai peninsula — desert plateau + peaks'),
    'karakum': (39.50, 59.00, 300, 'Karakum desert — flat expanses'),
    'oman':    (23.60, 57.80, 180, 'Hajar mountains — coast to interior'),
}

def deg_per_km_lat(): return 1.0 / 111.0
def deg_per_km_lon(lat): return 1.0 / (111.0 * math.cos(math.radians(lat)))

def download_srtm_grid(center_lat, center_lon, region_km, n=ATLAS_ZONES):
    """Download SRTM3 elevations for an n×n grid covering region_km × region_km."""
    import srtm
    print(f"[srtm] downloading elevation data around ({center_lat:.2f}°N, {center_lon:.2f}°E)…")
    print(f"[srtm] region: {region_km}km × {region_km}km → sampling {n}×{n} = {n*n} points")
    print(f"[srtm] NOTE: first run downloads ~10-50 MB of SRTM tiles, cached afterwards.")

    data = srtm.get_data()

    half_lat = (region_km / 2.0) * deg_per_km_lat()
    half_lon = (region_km / 2.0) * deg_per_km_lon(center_lat)

    lat_start = center_lat - half_lat
    lon_start = center_lon - half_lon
    lat_step  = (2.0 * half_lat) / n
    lon_step  = (2.0 * half_lon) / n

    grid = np.zeros((n, n), dtype=np.float32)
    for row in range(n):
        lat = lat_start + row * lat_step
        for col in range(n):
            lon = lon_start + col * lon_step
            h = data.get_elevation(lat, lon)
            grid[row, col] = float(h) if h is not None else 0.0
        if row % 8 == 0:
            print(f"[srtm]   row {row}/{n}…")

    # Fix any voids (SRTM has occasional missing pixels → set to nearest valid)
    mask = grid == 0
    if mask.sum() > 0:
        from scipy.ndimage import distance_transform_edt
        idx   = distance_transform_edt(mask, return_distances=False, return_indices=True)
        grid[mask] = grid[tuple(idx[:,mask])]

    print(f"[srtm] done. elevation: {grid.min():.0f}m – {grid.max():.0f}m "
          f"(mean={grid.mean():.0f}m)")
    return grid


def normalize_to_world(grid, target_min=0.0, target_max=180.0):
    """Normalize elevation grid to game height range, preserving relative relief."""
    lo = float(np.percentile(grid, 2))
    hi = float(np.percentile(grid, 98))
    if hi <= lo: hi = lo + 1.0
    norm = (grid - lo) / (hi - lo)
    norm = np.clip(norm, 0.0, 1.0)
    return norm * (target_max - target_min) + target_min


def build_zones_from_macro(macro_64, seed=42):
    """
    Build 64×64×65×65 atlas from:
      - macro_64 : 64×64 zone-resolution real terrain (SRTM derived)
      - per-zone procedural detail noise (sub-SRTM-resolution detail)

    Result: realistic macro shape + visible local variation at ground level.
    """
    rng = np.random.default_rng(seed)

    # Upsample macro to (ATLAS_ZONES*(ATLAS_VERTS-1)+1) for seamless zone extraction
    # Each zone shares its edge with the next → need ATLAS_ZONES*64+1 = 4097 pixels
    target = ATLAS_ZONES * (ATLAS_VERTS - 1) + 1  # 4097
    print(f"[build] upsampling macro 64→{target}…")
    macro_full = zoom(macro_64, target / ATLAS_ZONES, order=3)
    if macro_full.shape[0] < target:  # ensure exact size
        macro_full = np.pad(macro_full, ((0, target-macro_full.shape[0]),
                                          (0, target-macro_full.shape[1])), mode='edge')
    macro_full = gaussian_filter(macro_full, sigma=2.0)

    # Detail FBM noise (zone-scale, ~20–80m features)
    def zone_fbm(rng_local, octaves=6, freq0=0.45, persist=0.52):
        N   = ATLAS_VERTS
        out = np.zeros((N, N), dtype=np.float32)
        amp = 1.0; f = freq0; norm = 0.0
        for _ in range(octaves):
            gs  = max(3, int(N * f) + 2)
            g   = rng_local.random((gs, gs)).astype(np.float32)
            xs  = np.linspace(0, gs-1, N, endpoint=False)
            R, C   = np.meshgrid(xs, xs, indexing='ij')
            RI     = R.astype(int); RF = R-RI; RI1 = (RI+1)%gs
            CI     = C.astype(int); CF = C-CI; CI1 = (CI+1)%gs
            v = (g[RI,CI]*(1-RF)*(1-CF)+g[RI,CI1]*(1-RF)*CF+
                 g[RI1,CI]*RF*(1-CF)+g[RI1,CI1]*RF*CF)
            out += amp*v; norm += amp; amp *= persist; f *= 2.0
        return out / norm  # [0,1]

    # Global detail noise at full resolution — eliminates zone seam artifacts
    print("[build] generating global detail noise (no seams)…")
    N_full = target
    def global_fbm(rng_g, res, octaves=7, freq0=0.005, persist=0.52):
        out = np.zeros((res, res), dtype=np.float32)
        amp = 1.0; f = freq0; norm = 0.0
        for _ in range(octaves):
            gs  = max(3, int(res * f) + 2)
            g   = rng_g.random((gs, gs)).astype(np.float32)
            xs  = np.linspace(0, gs-1, res, endpoint=False)
            R, C  = np.meshgrid(xs, xs, indexing='ij')
            RI    = R.astype(int); RF = R-RI; RI1 = (RI+1)%gs
            CI    = C.astype(int); CF = C-CI; CI1 = (CI+1)%gs
            v = (g[RI,CI]*(1-RF)*(1-CF)+g[RI,CI1]*(1-RF)*CF+
                 g[RI1,CI]*RF*(1-CF)+g[RI1,CI1]*RF*CF)
            out += amp*v; norm += amp; amp *= persist; f *= 2.0
        return out / norm  # [0,1]

    detail_global = global_fbm(rng, N_full, octaves=7, freq0=0.008)

    # Local slope map: higher slope → more detail contrast
    slope = np.abs(np.gradient(macro_full, axis=0)) + np.abs(np.gradient(macro_full, axis=1))
    slope_norm = np.clip(slope / (slope.max() + 1e-6), 0, 1)
    # Detail amplitude: flat=5m, steep=20m
    detail_amp_map = 5.0 + slope_norm * 15.0

    detail_scaled = (detail_global - 0.5) * 2.0 * detail_amp_map  # [-amp, +amp]

    combined = macro_full + detail_scaled
    combined = np.maximum(combined, 0.0)

    print("[build] extracting per-zone arrays…")
    zones_h = np.zeros((ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS, ATLAS_VERTS), dtype=np.float32)
    for cz in range(ATLAS_ZONES):
        for cx in range(ATLAS_ZONES):
            r0 = cz * (ATLAS_VERTS - 1)
            c0 = cx * (ATLAS_VERTS - 1)
            zones_h[cz, cx] = combined[r0:r0+ATLAS_VERTS, c0:c0+ATLAS_VERTS].astype(np.float32)
        if cz % 16 == 0:
            print(f"[build]   {cz}/{ATLAS_ZONES}…")

    hmax = float(zones_h.max())
    print(f"[build] height range: {zones_h.min():.1f} – {hmax:.1f} m")
    return zones_h


def write_atlas(zones_h, out_path):
    print(f"[write] writing {out_path}…")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(struct.pack('<4I', ATLAS_MAGIC, ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS))
        for cz in range(ATLAS_ZONES):
            for cx in range(ATLAS_ZONES):
                z = zones_h[cz, cx]
                f.write(struct.pack('<ff', float(z.min()), float(z.max())))
                f.write(z.tobytes())
    print(f"[write] {os.path.getsize(out_path)/1024/1024:.1f} MB written")


def write_preview(zones_h, out_path):
    """Write a quick hillshaded preview PNG."""
    V  = 8; SZ = ATLAS_ZONES * V
    avg = np.zeros((SZ, SZ), dtype=np.float32)
    for cz in range(ATLAS_ZONES):
        for cx in range(ATLAS_ZONES):
            avg[cz*V:(cz+1)*V, cx*V:(cx+1)*V] = zones_h[cz, cx].mean()

    lo, hi = avg.min(), avg.max()
    norm = (avg - lo) / max(hi - lo, 1.0)
    # Hillshade
    dz = np.gradient(avg, axis=0)
    dx = np.gradient(avg, axis=1)
    mag = np.sqrt(dx*dx + dz*dz + 1.0)
    shade = (1.0 / mag) * 0.7 + 0.3

    # Color palette
    rgb = np.zeros((SZ, SZ, 3), dtype=np.uint8)
    for (t0, c0), (t1, c1) in zip(
        [(0.0,(45,75,125)),(0.04,(195,180,130)),(0.15,(165,140,85)),
         (0.40,(110,105,70)),(0.65,(80,78,68))],
        [(0.04,(195,180,130)),(0.15,(165,140,85)),(0.40,(110,105,70)),
         (0.65,(80,78,68)),(1.0,(230,230,240))]):
        m = (norm >= t0) & (norm < t1)
        t = np.where(m, (norm - t0) / max(t1 - t0, 1e-6), 0.0)
        for c in range(3):
            rgb[:,:,c] = np.where(m,
                np.clip(c0[c] + t*(c1[c]-c0[c]), 0, 255), rgb[:,:,c]).astype(np.uint8)

    for c in range(3):
        rgb[:,:,c] = np.clip(rgb[:,:,c] * shade, 0, 255).astype(np.uint8)
    Image.fromarray(rgb, 'RGB').save(out_path)
    print(f"[preview] {out_path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--region', default='hoggar',
                    choices=list(REGIONS.keys()),
                    help='Terrain preset region')
    ap.add_argument('--seed',    type=int, default=42)
    ap.add_argument('--preview', action='store_true')
    ap.add_argument('--out', default='game/data/terrain/world_hmap.r32')
    args = ap.parse_args()

    root     = os.path.join(os.path.dirname(__file__), '..')
    out_path = os.path.join(root, args.out)

    lat, lon, km, desc = REGIONS[args.region]
    print(f"[srtm] Region: {args.region} — {desc}")
    print(f"[srtm] Center: {lat}°N {lon}°E, sampling {ATLAS_ZONES}km = {km}km area")

    # Backup
    if os.path.exists(out_path):
        bak = out_path + '.bak'
        if not os.path.exists(bak):
            import shutil; shutil.copy2(out_path, bak)
            print(f"[srtm] backed up → {bak}")

    # 1. Download SRTM macro grid (64×64 zone-level samples)
    macro = download_srtm_grid(lat, lon, km, n=ATLAS_ZONES)

    # 2. Normalise to game height range (keep relative relief)
    macro_norm = normalize_to_world(macro, target_min=0.0, target_max=190.0)

    # 3. Build full atlas with per-zone detail noise
    zones_h = build_zones_from_macro(macro_norm, seed=args.seed)

    # 4. Write
    write_atlas(zones_h, out_path)

    if args.preview:
        prev = out_path.replace('.r32', '_preview.png')
        write_preview(zones_h, prev)

    # 5. Remind to regenerate master hmap and world map
    print("\n[srtm] Next steps:")
    print("  python3 tools/md_master_hmap_gen.py")
    print("  python3 tools/md_worldmap_gen.py")
    print("[srtm] done.")


if __name__ == '__main__':
    main()
