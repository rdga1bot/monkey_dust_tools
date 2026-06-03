#!/usr/bin/env python3
"""
md_worldmap_gen.py — Generate high-quality world_map.png (Kenshi areasmap style).

Visual approach:
  1. Domain-warped Voronoi  — organic, irregular zone boundaries
  2. Per-zone unique color  — 59 distinct shades from biome palettes
  3. FBM micro-texture      — stippled/painted look inside each zone
  4. Normal-map hillshading — terrain relief visible inside zones
  5. Zone markers + labels

Usage: python3 tools/md_worldmap_gen.py
"""

import os, struct, math, hashlib
import numpy as np
from scipy.ndimage import gaussian_filter, zoom as sci_zoom
from scipy.spatial  import cKDTree
from PIL import Image, ImageDraw

ATLAS_ZONES = 64
ATLAS_VERTS = 65
MAP_SIZE    = 2048   # output px

# ── Per-biome color palette (multiple shades per biome) ───────────────────────
# Each zone picks one shade deterministically (hash of its grid position)
BIOME_PALETTE = {
    'desert':    [(220,200,130),(205,182,112),(235,215,145),(195,172,100),
                  (242,222,158),(212,190,120),(228,208,136)],
    'canyon':    [(178, 96, 52),(165, 80, 42),(192,112, 65),(155, 72, 38),
                  (185,100, 55),(170, 88, 48),(200,120, 72)],
    'volcanic':  [( 72, 60, 72),( 58, 50, 68),( 88, 55, 85),( 65, 45, 80),
                  ( 80, 65, 90),( 50, 48, 68),( 95, 70, 95)],
    'swamp':     [( 52,105, 62),( 42, 92, 52),( 62,118, 72),( 38, 82, 48),
                  ( 55,110, 65),( 48, 98, 58),( 65,120, 70)],
    'scrubland': [(138,158, 90),(125,142, 78),(150,168,102),(118,135, 72),
                  (145,162, 96),(132,150, 85),(155,172,108)],
    'highlands': [( 98, 92, 78),( 88, 84, 70),(108,100, 86),( 82, 78, 65),
                  (102, 96, 82),( 92, 88, 74),(115,108, 90)],
    'highland':  [(118,112, 90),(108,104, 82),(125,118, 96),(100, 96, 75),
                  (120,115, 92),(112,108, 86),(130,124,100)],
    'ashlands':  [(148,128, 95),(135,115, 85),(158,138,105),(125,108, 78),
                  (152,132, 98),(140,120, 88),(162,142,108)],
    'coast':     [( 62,128,148),( 50,112,132),( 72,142,162),( 45,100,120),
                  ( 65,135,155),( 55,120,140),( 78,148,168)],
    '_default':  [(148,138,108),(135,125, 98),(158,148,118),(125,115, 88)],
}

def zone_color(biome, gx, gz):
    """Deterministically pick a palette shade for this zone."""
    h = int(hashlib.md5(f"{gx},{gz}".encode()).hexdigest(), 16)
    pal = BIOME_PALETTE.get(biome, BIOME_PALETTE['_default'])
    return np.array(pal[h % len(pal)], dtype=np.float32)


# ── Loaders ───────────────────────────────────────────────────────────────────

def load_atlas(r32_path):
    if not os.path.exists(r32_path):
        return None, None
    print(f"[mapgen] loading {r32_path}…")
    with open(r32_path, 'rb') as f:
        magic, zx, zy, verts = struct.unpack('<4I', f.read(16))
        if magic != 0x414D4800:
            print("[mapgen] bad magic"); return None, None
        G = verts - 1; full = ATLAS_ZONES * G
        hmap = np.zeros((full, full), dtype=np.float32)
        for zi in range(ATLAS_ZONES * ATLAS_ZONES):
            struct.unpack('<ff', f.read(8))
            zone = np.frombuffer(f.read(verts*verts*4), dtype=np.float32).reshape(verts,verts)
            zy_i, zx_i = zi//ATLAS_ZONES, zi%ATLAS_ZONES
            hmap[zy_i*G:zy_i*G+G, zx_i*G:zx_i*G+G] = zone[:G,:G]
    print(f"[mapgen] heightmap range {hmap.min():.1f}–{hmap.max():.1f}m")
    return hmap, full


def load_zones(config_path):
    zones, cur = {}, {}
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
                k, v = s.split('=', 1); cur[k.strip()] = v.strip()
    if cur and 'grid_x' in cur and 'grid_z' in cur:
        zones[(int(cur['grid_x']), int(cur['grid_z']))] = cur.copy()
    return zones


# ── FBM noise (tiling-safe, vectorised) ───────────────────────────────────────

def fbm2d(rng, res, octaves=6, freq=0.004, persist=0.52):
    out = np.zeros((res, res), dtype=np.float32)
    amp = 1.0; f = freq; norm = 0.0
    for _ in range(octaves):
        gs = max(4, int(res * f) + 2)
        g  = rng.random((gs+2, gs+2)).astype(np.float32)
        xs = np.linspace(0, gs-1, res, endpoint=False)
        R, C   = np.meshgrid(xs, xs, indexing='ij')
        RI     = R.astype(int); RF = R-RI; RI1 = np.clip(RI+1,0,gs)
        CI     = C.astype(int); CF = C-CI; CI1 = np.clip(CI+1,0,gs)
        v = (g[RI,CI]*(1-RF)*(1-CF) + g[RI,CI1]*(1-RF)*CF +
             g[RI1,CI]*RF*(1-CF)    + g[RI1,CI1]*RF*CF)
        out += amp*v; norm += amp; amp *= persist; f *= 2.0
    return out / norm   # [0,1]


# ── Domain-warped Voronoi colour map ──────────────────────────────────────────

def build_map_rgb(zones, h_small, out_size, seed=42):
    """
    Build the RGB colour map at out_size×out_size:
      - Domain-warped Voronoi (organic zone boundaries)
      - Per-zone unique colour from biome palette
      - FBM micro-texture overlay (stippled/painted look)
      - Height-driven brightness variation (valleys dark, peaks bright)
    """
    S   = out_size
    rng = np.random.default_rng(seed)
    px  = S / ATLAS_ZONES   # pixels per zone cell

    # ── 1. Zone seed positions (pixel coords) + colours ──────────────────────
    pts_yx, colors, zone_keys = [], [], []
    for (gx, gz), z in zones.items():
        if gx >= ATLAS_ZONES or gz >= ATLAS_ZONES: continue
        biome = z.get('biome', '_default')
        pts_yx.append((gz * px + px*0.5, gx * px + px*0.5))
        colors.append(zone_color(biome, gx, gz))
        zone_keys.append((gx, gz))
    pts_yx = np.array(pts_yx, dtype=np.float32)
    colors = np.array(colors, dtype=np.float32)      # (N, 3)

    # Distance from nearest zone (for ocean fade)
    tree = cKDTree(pts_yx)

    # ── 2. Domain warp fields ─────────────────────────────────────────────────
    # Two independent FBM fields; warp_px = 1.8 zone widths ≈ organic shapes
    warp_px = px * 1.8
    wx = (fbm2d(rng, S, octaves=7, freq=0.0035, persist=0.55) - 0.5) * 2 * warp_px
    wy = (fbm2d(rng, S, octaves=7, freq=0.0035, persist=0.55) - 0.5) * 2 * warp_px
    # Second warp pass (domain warping of domain warping = extra organic)
    wx2 = (fbm2d(rng, S, octaves=5, freq=0.007,  persist=0.50) - 0.5) * 2 * warp_px * 0.4
    wy2 = (fbm2d(rng, S, octaves=5, freq=0.007,  persist=0.50) - 0.5) * 2 * warp_px * 0.4
    wx  = wx + wx2; wy = wy + wy2

    # ── 3. Warped Voronoi lookup ──────────────────────────────────────────────
    yi, xi = np.mgrid[0:S, 0:S].astype(np.float32)
    qy = np.clip(yi + wy, 0, S-1).ravel()
    qx = np.clip(xi + wx, 0, S-1).ravel()
    coords = np.stack([qy, qx], axis=1)

    dists_raw, idx = tree.query(coords)
    dists = dists_raw.reshape(S, S)
    idx_2d = idx.reshape(S, S)

    # Base zone colour
    rgb_f = colors[idx_2d]                            # (S,S,3)

    # ── 4. FBM micro-texture overlay ─────────────────────────────────────────
    # High-frequency noise gives stippled "hand-painted" appearance
    tex_hf = fbm2d(rng, S, octaves=8, freq=0.018, persist=0.58)  # fine grain
    tex_mf = fbm2d(rng, S, octaves=5, freq=0.006, persist=0.55)  # medium blobs
    # Combine: more medium-scale variation, less fine grain
    texture = tex_mf * 0.65 + tex_hf * 0.35
    # Map to [-1, 1] and scale to ±18% brightness swing
    tex_mult = 1.0 + (texture - 0.5) * 2.0 * 0.18
    for c in range(3):
        rgb_f[:,:,c] = np.clip(rgb_f[:,:,c] * tex_mult, 0, 255)

    # ── 5. Height-based brightness (valleys darker, ridges brighter) ─────────
    h_norm = np.clip((h_small - h_small.min()) /
                     max(h_small.max() - h_small.min(), 1.0), 0, 1)
    # [-15%, +20%] range, subtle so biome colour dominates
    h_mult = 0.85 + h_norm * 0.35
    for c in range(3):
        rgb_f[:,:,c] = np.clip(rgb_f[:,:,c] * h_mult, 0, 255)

    # ── 6. Ocean fade (distance from nearest zone) ────────────────────────────
    ocean_col = np.array([38, 65, 105], dtype=np.float32)
    # "coast" zones are at most ~5 zone widths from others; fade starts at 6
    ocean_frac = np.clip((dists / px - 5.5) / 5.0, 0, 1).astype(np.float32)
    ocean_frac = gaussian_filter(ocean_frac, sigma=px * 1.0)
    for c in range(3):
        rgb_f[:,:,c] = rgb_f[:,:,c] * (1 - ocean_frac) + ocean_col[c] * ocean_frac

    # ── 7. Thin biome-boundary darkening (zone edge contrast) ─────────────────
    # Detect Voronoi cell edges from the warped index map
    edge_h = np.abs(np.diff(idx_2d, axis=0, append=idx_2d[-1:])) > 0
    edge_v = np.abs(np.diff(idx_2d, axis=1, append=idx_2d[:,-1:])) > 0
    edge   = (edge_h | edge_v).astype(np.float32)
    edge   = gaussian_filter(edge, sigma=1.2)           # 2-3px soft dark line
    edge_dark = 1.0 - edge * 0.45
    for c in range(3):
        rgb_f[:,:,c] = np.clip(rgb_f[:,:,c] * edge_dark, 0, 255)

    return rgb_f


# ── Hillshading ───────────────────────────────────────────────────────────────

def compute_normals(hmap, scale=1.0):
    dz = np.gradient(hmap, axis=0) * scale
    dx = np.gradient(hmap, axis=1) * scale
    mag = np.sqrt(dx*dx + dz*dz + 1.0)
    return -dx/mag, 1.0/mag, -dz/mag

def hillshade(nx, ny, nz, az, el):
    ar, er = math.radians(az), math.radians(el)
    lx = math.cos(er)*math.cos(ar); lz = math.cos(er)*math.sin(ar); ly = math.sin(er)
    return np.clip(nx*lx + ny*ly + nz*lz, 0, 1).astype(np.float32)


# ── Full render ───────────────────────────────────────────────────────────────

def generate_map(hmap, full, zones, out_size):
    S = out_size

    print(f"[mapgen] downsampling {full}→{S}…")
    h_small = sci_zoom(hmap, S/full, order=1).astype(np.float32)
    h_small = gaussian_filter(h_small, sigma=0.6)

    print("[mapgen] building domain-warped Voronoi + texture…")
    rgb_f = build_map_rgb(zones, h_small, S)

    print("[mapgen] hillshading…")
    world_m_per_px = (ATLAS_ZONES * 500.0) / S
    nx, ny, nz = compute_normals(h_small, scale=0.004 / world_m_per_px)
    sh1 = hillshade(nx, ny, nz, 315, 42)
    sh2 = hillshade(nx, ny, nz, 135, 65)
    shade = np.clip(sh1*0.68 + sh2*0.22 + 0.22, 0.38, 1.30)

    for c in range(3):
        rgb_f[:,:,c] = np.clip(rgb_f[:,:,c] * shade, 0, 255)

    # Subtle vignette
    cy, cx = S//2, S//2
    Y, X   = np.mgrid[0:S, 0:S]
    vig    = np.clip(1.0 - 0.18 * ((X-cx)**2+(Y-cy)**2) / (0.72*S)**2, 0.82, 1.0)
    for c in range(3):
        rgb_f[:,:,c] = np.clip(rgb_f[:,:,c] * vig, 0, 255)

    img  = Image.fromarray(rgb_f.astype(np.uint8), 'RGB')
    draw = ImageDraw.Draw(img)

    # Zone markers
    print("[mapgen] drawing markers…")
    px = S / ATLAS_ZONES
    DCOL = {1:(255,235,130),2:(255,220,100),3:(255,190,65),4:(255,155,45),
            5:(255,115,35),6:(255,80,55),7:(255,55,55),8:(220,40,40),9:(180,20,20)}
    for (gx_z, gz_z), z in zones.items():
        if gx_z >= ATLAS_ZONES or gz_z >= ATLAS_ZONES: continue
        cx_px = int((gx_z + 0.5) * px)
        cy_px = int((gz_z + 0.5) * px)
        danger = int(z.get('danger', 1))
        r = 3 + danger // 2
        col = DCOL.get(danger, (255,80,80))
        draw.ellipse([cx_px-r, cy_px-r, cx_px+r, cy_px+r], fill=col, outline=(0,0,0))
        name  = z.get('name', z.get('id', '?'))
        short = ' '.join(name.split()[:2])[:14]
        draw.text((cx_px+r+3, cy_px-5), short, fill=(0,0,0))
        draw.text((cx_px+r+2, cy_px-6), short, fill=(255,248,210))

    return img


def main():
    root = os.path.join(os.path.dirname(__file__), '..')
    r32  = os.path.join(root, 'game/data/terrain/world_hmap.r32')
    cfg  = os.path.join(root, 'game/data/terrain_config.txt')
    out  = os.path.join(root, 'game/data/textures/world_map.png')

    hmap, full = load_atlas(r32)
    if hmap is None:
        print("[mapgen] ERROR: could not load world_hmap.r32"); return

    zones = load_zones(cfg)
    print(f"[mapgen] {len(zones)} zones")

    if os.path.exists(out):
        bak = out.replace('.png', '_orig.png')
        if not os.path.exists(bak):
            import shutil; shutil.copy2(out, bak)

    img = generate_map(hmap, full, zones, MAP_SIZE)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    img.save(out)
    print(f"[mapgen] saved {out}")


if __name__ == '__main__':
    main()
