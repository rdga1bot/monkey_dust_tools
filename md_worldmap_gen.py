#!/usr/bin/env python3
"""
md_worldmap_gen.py — Generate world_map.png for the in-game M overlay.

Each biome is rendered in its own colour (Kenshi-style area map):
  desert    → warm sandy tan
  canyon    → orange-red eroded rock
  volcanic  → dark ash / grey-black
  swamp     → deep green
  scrubland → olive-green
  highlands → grey-brown stone
  highland  → lighter grey-brown
  ashlands  → pale bleached grey
  coast     → teal blue

Colours are Voronoi-assigned from terrain_config.txt zone positions,
Gaussian-blended at biome boundaries, then hillshaded for relief.

Usage: python3 tools/md_worldmap_gen.py
"""

import os, struct, math
import numpy as np
from scipy.ndimage import gaussian_filter, zoom as sci_zoom
from scipy.spatial import cKDTree
from PIL import Image, ImageDraw

ATLAS_ZONES = 64
ATLAS_VERTS = 65
MAP_SIZE    = 2048

# ── Biome base colours (RGB) ──────────────────────────────────────────────────
BIOME_COLOR = {
    'desert':    (205, 175, 100),
    'canyon':    (175,  95,  50),
    'volcanic':  ( 65,  60,  58),
    'swamp':     ( 55, 105,  65),
    'scrubland': (135, 155,  88),
    'highlands': ( 98,  92,  78),
    'highland':  (115, 108,  88),
    'ashlands':  (138, 120,  92),
    'coast':     ( 60, 120, 140),
    '_ocean':    ( 30,  55, 100),
    '_default':  (145, 135, 105),
}

# Height tint — multipliers applied on top of biome color based on elevation
# Keeps variation visible without overriding biome identity
HEIGHT_TINT = [
    # (fraction, mult_r, mult_g, mult_b)  — fraction of biome's own max height
    (0.00, 0.60, 0.60, 0.65),  # valley: slight blue-shade
    (0.15, 0.88, 0.85, 0.80),  # lowland
    (0.40, 1.00, 1.00, 1.00),  # mid (no change)
    (0.70, 1.10, 1.08, 1.02),  # upper slope: slight warm brighten
    (1.00, 1.25, 1.22, 1.20),  # peak: brighten toward white
]


def lerp_height_tint(frac):
    for i in range(len(HEIGHT_TINT) - 1):
        t0, *c0 = HEIGHT_TINT[i]
        t1, *c1 = HEIGHT_TINT[i + 1]
        if t0 <= frac <= t1:
            t = (frac - t0) / (t1 - t0)
            return tuple(c0[j] + t * (c1[j] - c0[j]) for j in range(3))
    return HEIGHT_TINT[-1][1:]


def compute_normals(hmap, scale=1.0):
    dz = np.gradient(hmap, axis=0) * scale
    dx = np.gradient(hmap, axis=1) * scale
    mag = np.sqrt(dx*dx + dz*dz + 1.0)
    return -dx/mag, 1.0/mag, -dz/mag


def hillshade(nx, ny, nz, az=315.0, el=45.0):
    az_r, el_r = math.radians(az), math.radians(el)
    lx = math.cos(el_r) * math.cos(az_r)
    lz = math.cos(el_r) * math.sin(az_r)
    ly = math.sin(el_r)
    return np.clip(nx*lx + ny*ly + nz*lz, 0.0, 1.0).astype(np.float32)


# ── Loaders ───────────────────────────────────────────────────────────────────

def load_atlas(r32_path):
    if not os.path.exists(r32_path):
        return None, None
    print(f"[mapgen] loading {r32_path}…")
    with open(r32_path, 'rb') as f:
        magic, zx, zy, verts = struct.unpack('<4I', f.read(16))
        if magic != 0x414D4800:
            print("[mapgen] bad magic"); return None, None
        G    = verts - 1
        full = ATLAS_ZONES * G
        hmap = np.zeros((full, full), dtype=np.float32)
        for zi in range(ATLAS_ZONES * ATLAS_ZONES):
            _hmin, _hmax = struct.unpack('<ff', f.read(8))
            zone = np.frombuffer(f.read(verts * verts * 4), dtype=np.float32).reshape(verts, verts)
            zy_i, zx_i = zi // ATLAS_ZONES, zi % ATLAS_ZONES
            r0, c0 = zy_i * G, zx_i * G
            hmap[r0:r0+G, c0:c0+G] = zone[:G, :G]
    print(f"[mapgen] heightmap {hmap.shape}, range {hmap.min():.1f}–{hmap.max():.1f}m")
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
                k, v = s.split('=', 1)
                cur[k.strip()] = v.strip()
    if cur and 'grid_x' in cur and 'grid_z' in cur:
        zones[(int(cur['grid_x']), int(cur['grid_z']))] = cur.copy()
    return zones


# ── Biome colour map (Voronoi → Gaussian blend) ───────────────────────────────

def build_biome_rgb(zones, out_size, blend_sigma=60.0):
    """
    Build out_size×out_size RGB array with Voronoi biome colours, smoothly
    blended across biome boundaries.  Regions beyond the zone cluster fade to
    ocean colour.
    """
    S = out_size
    px_per_zone = S / ATLAS_ZONES

    # Collect zone seed points in pixel coords + their biome
    pts, biomes = [], []
    for (gx, gz), z in zones.items():
        b = z.get('biome', '_default')
        pts.append(  (gz * px_per_zone + px_per_zone * 0.5,   # row (y)
                      gx * px_per_zone + px_per_zone * 0.5))  # col (x)
        biomes.append(b)
    pts    = np.array(pts,   dtype=np.float32)
    biomes = np.array(biomes)

    unique_biomes = sorted(set(biomes))

    # For each pixel, Voronoi → nearest zone biome
    yi, xi = np.mgrid[0:S, 0:S]
    coords  = np.stack([yi.ravel().astype(np.float32),
                        xi.ravel().astype(np.float32)], axis=1)
    tree    = cKDTree(pts)
    dists, idx = tree.query(coords)
    dists   = dists.reshape(S, S)
    biome_px = biomes[idx].reshape(S, S)

    # Gaussian-blend multiple biome colour layers
    rgb_f = np.zeros((S, S, 3), dtype=np.float32)
    w_sum = np.zeros((S, S),    dtype=np.float32)

    for b in unique_biomes:
        col  = np.array(BIOME_COLOR.get(b, BIOME_COLOR['_default']), dtype=np.float32)
        mask = (biome_px == b).astype(np.float32)
        w    = gaussian_filter(mask, sigma=blend_sigma)
        for c in range(3):
            rgb_f[:,:,c] += col[c] * w
        w_sum += w

    rgb_f /= np.maximum(w_sum[:,:,None], 1e-6)

    # Distance-based ocean fade: pixels far from any zone → ocean colour
    # Compute distance to nearest zone centre (in zone-cell units)
    ocean_col = np.array(BIOME_COLOR['_ocean'], dtype=np.float32)
    dist_zones = dists / px_per_zone          # distance in zone-cell units
    ocean_frac = np.clip((dist_zones - 5.0) / 6.0, 0.0, 1.0).astype(np.float32)
    ocean_frac = gaussian_filter(ocean_frac, sigma=blend_sigma * 0.8)
    for c in range(3):
        rgb_f[:,:,c] = rgb_f[:,:,c] * (1 - ocean_frac) + ocean_col[c] * ocean_frac

    return rgb_f


# ── Main render ───────────────────────────────────────────────────────────────

def generate_map(hmap, full, zones, out_size):
    S = out_size

    # 1. Downsample heightmap
    scale = S / full
    print(f"[mapgen] downsampling {full}→{S}…")
    h_small = sci_zoom(hmap, scale, order=1).astype(np.float32)
    h_small = gaussian_filter(h_small, sigma=0.8)

    # 2. Build biome colour layer
    print("[mapgen] building biome colour map…")
    biome_rgb = build_biome_rgb(zones, S, blend_sigma=max(12, S // 80))

    # 3. Height-based tint (applied per-pixel using normalised local height)
    print("[mapgen] applying height tint…")
    h_min = float(np.percentile(h_small[h_small > 0.5], 2))
    h_max = float(np.percentile(h_small, 99.5))
    h_norm = np.clip((h_small - h_min) / max(h_max - h_min, 1.0), 0.0, 1.0)

    # Build tint LUT
    lut_n  = 512
    lut_tm = np.zeros((lut_n, 3), dtype=np.float32)
    for i in range(lut_n):
        lut_tm[i] = lerp_height_tint(i / (lut_n - 1))
    tidx = (h_norm * (lut_n - 1)).astype(np.int32).clip(0, lut_n - 1)
    tint = lut_tm[tidx]                       # (S,S,3) multipliers

    rgb_f = np.zeros((S, S, 3), dtype=np.float32)
    for c in range(3):
        rgb_f[:,:,c] = np.clip(biome_rgb[:,:,c] * tint[:,:,c], 0, 255)

    # 4. Hillshading
    print("[mapgen] hillshading…")
    world_m_per_px = (ATLAS_ZONES * 500.0) / S
    nx, ny, nz = compute_normals(h_small, scale=0.003 / world_m_per_px)
    shade1 = hillshade(nx, ny, nz, az=315, el=45)
    shade2 = hillshade(nx, ny, nz, az=135, el=60)
    shade  = np.clip(shade1 * 0.72 + shade2 * 0.28 + 0.18, 0.35, 1.35)

    for c in range(3):
        rgb_f[:,:,c] = np.clip(rgb_f[:,:,c] * shade, 0, 255)

    # 5. Subtle vignette
    cy, cx = S//2, S//2
    Y, X   = np.mgrid[0:S, 0:S]
    vig = np.clip(1.0 - 0.20 * ((X-cx)**2 + (Y-cy)**2) / (0.7*S)**2, 0.80, 1.0)
    for c in range(3):
        rgb_f[:,:,c] = np.clip(rgb_f[:,:,c] * vig, 0, 255)

    img  = Image.fromarray(rgb_f.astype(np.uint8), 'RGB')
    draw = ImageDraw.Draw(img)

    # 6. Zone markers and labels
    print("[mapgen] drawing zone labels…")
    px_per_zone = S / ATLAS_ZONES

    DANGER_COLOR = {
        1: (255, 230, 120),
        2: (255, 220, 100),
        3: (255, 185,  65),
        4: (255, 145,  45),
        5: (255, 110,  35),
        6: (255,  80,  55),
        7: (255,  55,  55),
        8: (220,  40,  40),
        9: (180,  20,  20),
    }

    for (gx_z, gz_z), z in zones.items():
        if gx_z >= ATLAS_ZONES or gz_z >= ATLAS_ZONES:
            continue
        cx_px = int((gx_z + 0.5) * px_per_zone)
        cy_px = int((gz_z + 0.5) * px_per_zone)

        name   = z.get('name', z.get('id', '?'))
        danger = int(z.get('danger', 1))
        biome  = z.get('biome', '_default')

        # Dot — size and colour by danger
        r   = 3 + danger // 2
        col = DANGER_COLOR.get(danger, (255, 80, 80))
        draw.ellipse([cx_px-r, cy_px-r, cx_px+r, cy_px+r],
                     fill=col, outline=(0, 0, 0))

        # Short label (2 words max)
        short = ' '.join(name.split()[:2])[:14]
        # Shadow
        draw.text((cx_px + r + 3, cy_px - 5), short, fill=(0, 0, 0, 180))
        draw.text((cx_px + r + 2, cy_px - 6), short, fill=(255, 248, 210))

    return img


def main():
    root  = os.path.join(os.path.dirname(__file__), '..')
    r32   = os.path.join(root, 'game', 'data', 'terrain', 'world_hmap.r32')
    cfg   = os.path.join(root, 'game', 'data', 'terrain_config.txt')
    out   = os.path.join(root, 'game', 'data', 'textures', 'world_map.png')

    hmap, full = load_atlas(r32)
    if hmap is None:
        print("[mapgen] ERROR: could not load world_hmap.r32"); return

    zones = load_zones(cfg)
    print(f"[mapgen] {len(zones)} zones loaded")

    if os.path.exists(out):
        bak = out.replace('.png', '_orig.png')
        if not os.path.exists(bak):
            import shutil; shutil.copy2(out, bak)

    img = generate_map(hmap, full, zones, MAP_SIZE)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    img.save(out)
    print(f"[mapgen] saved {out} ({img.width}×{img.height})")


if __name__ == '__main__':
    main()
