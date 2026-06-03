#!/usr/bin/env python3
"""
md_worldmap_gen.py — Generate a high-quality world_map.png for the in-game M overlay.

Renders the world heightmap as a proper terrain visualization:
  - Height-based color gradient (ocean → sand → scrub → rock → snow)
  - Normal-map hillshading for depth and relief
  - Smooth biome color tinting (no visible zone grid)
  - Zone labels and city markers
  - Vignette + atmospheric haze at distance

Usage: python3 tools/md_worldmap_gen.py
"""

import os, struct, math
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont
from scipy.ndimage import gaussian_filter

ATLAS_ZONES = 64
ATLAS_VERTS = 65
MAP_SIZE    = 2048   # output PNG resolution

# ── Color palette (height-based gradient) ────────────────────────────────────
# Inspired by Kenshi's colour palette: warm desert tones dominant
PALETTE = [
    # (height_fraction, R,G,B)  — 0.0=sea-level, 1.0=peak
    (0.000, ( 45,  75, 125)),   # deep ocean
    (0.020, ( 75, 110, 155)),   # shallow water
    (0.040, (195, 180, 130)),   # sandy coast / lowland
    (0.120, (200, 170, 115)),   # flat desert
    (0.200, (185, 155, 100)),   # dunes
    (0.300, (165, 140,  85)),   # low scrubland
    (0.400, (140, 125,  75)),   # medium scrub
    (0.500, (120, 110,  70)),   # highland scrub
    (0.620, ( 95,  95,  70)),   # rocky highland
    (0.740, ( 80,  78,  68)),   # rock / stone
    (0.860, ( 65,  62,  60)),   # dark rock face
    (1.000, (230, 230, 240)),   # snow peak
]

def height_to_color(h_norm):
    """Map normalised height [0..1] to RGB via palette."""
    for i in range(len(PALETTE) - 1):
        t0, c0 = PALETTE[i]
        t1, c1 = PALETTE[i + 1]
        if t0 <= h_norm <= t1:
            t = (h_norm - t0) / (t1 - t0)
            return tuple(int(c0[j] + t * (c1[j] - c0[j])) for j in range(3))
    return PALETTE[-1][1]


def load_atlas(r32_path):
    """Load world_hmap.r32 → full float32 heightmap (4096×4096)."""
    if not os.path.exists(r32_path):
        return None, None
    print(f"[mapgen] loading {r32_path}…")
    with open(r32_path, 'rb') as f:
        magic, zx, zy, verts = struct.unpack('<4I', f.read(16))
        if magic != 0x414D4800:
            print("[mapgen] bad magic"); return None, None
        G = verts - 1  # 64 unique verts per zone (shared edge excluded)
        full = ATLAS_ZONES * G  # 4096×4096
        hmap = np.zeros((full, full), dtype=np.float32)
        for zi in range(ATLAS_ZONES * ATLAS_ZONES):
            _hmin, _hmax = struct.unpack('<ff', f.read(8))
            zone = np.frombuffer(f.read(verts * verts * 4), dtype=np.float32)
            zone = zone.reshape(verts, verts)
            zy_i = zi // ATLAS_ZONES
            zx_i = zi % ATLAS_ZONES
            r0 = zy_i * G; c0 = zx_i * G
            hmap[r0:r0+G, c0:c0+G] = zone[:G, :G]
    print(f"[mapgen] heightmap {hmap.shape}, range {hmap.min():.1f}–{hmap.max():.1f}m")
    return hmap, full


def compute_normals(hmap, scale=1.0):
    """Compute surface normals from heightmap for hillshading."""
    dz = np.gradient(hmap, axis=0) * scale
    dx = np.gradient(hmap, axis=1) * scale
    # Normal = normalize(-dx, -dz, 1)
    mag = np.sqrt(dx*dx + dz*dz + 1.0)
    nx = -dx / mag
    nz = -dz / mag
    ny =  1.0 / mag
    return nx, ny, nz


def hillshade(nx, ny, nz, sun_az=315.0, sun_el=45.0):
    """Lambertian hillshading from a directional light."""
    az_r = math.radians(sun_az)
    el_r = math.radians(sun_el)
    lx = math.cos(el_r) * math.cos(az_r)
    lz = math.cos(el_r) * math.sin(az_r)
    ly = math.sin(el_r)
    shade = nx * lx + ny * ly + nz * lz
    return np.clip(shade, 0.0, 1.0).astype(np.float32)


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


def generate_map(hmap, full, zones, out_size):
    """Render the full terrain map image."""
    S = out_size

    # ── 1. Downsample heightmap to output resolution ─────────────────────────
    from scipy.ndimage import zoom
    scale = S / full
    print(f"[mapgen] downsampling {full}→{S}…")
    h_small = zoom(hmap, scale, order=1).astype(np.float32)

    # Light Gaussian blur to reduce pixel aliasing from zone seams
    h_small = gaussian_filter(h_small, sigma=0.8)

    # ── 2. Normalise heights ─────────────────────────────────────────────────
    h_min  = float(np.percentile(h_small[h_small > 0.5], 2))
    h_max  = float(np.percentile(h_small, 99.5))
    h_norm = np.clip((h_small - h_min) / max(h_max - h_min, 1.0), 0.0, 1.0)

    # ── 3. Base color from height palette ────────────────────────────────────
    print("[mapgen] applying color palette…")
    rgb = np.zeros((S, S, 3), dtype=np.uint8)
    # Vectorised palette lookup (build LUT of 1024 entries)
    lut_n   = 1024
    lut_rgb = np.zeros((lut_n, 3), dtype=np.float32)
    for i in range(lut_n):
        c = height_to_color(i / (lut_n - 1))
        lut_rgb[i] = c
    idx = (h_norm * (lut_n - 1)).astype(np.int32).clip(0, lut_n - 1)
    rgb[:,:,0] = lut_rgb[idx, 0].astype(np.uint8)
    rgb[:,:,1] = lut_rgb[idx, 1].astype(np.uint8)
    rgb[:,:,2] = lut_rgb[idx, 2].astype(np.uint8)

    # ── 4. Hillshading ────────────────────────────────────────────────────────
    print("[mapgen] computing hillshading…")
    # Use pixel spacing relative to actual world size for correct gradient
    world_m_per_px = (ATLAS_ZONES * 500.0) / S
    nx, ny, nz = compute_normals(h_small, scale=0.003 / world_m_per_px)

    # Two-light setup: main sun (NW, 45°) + soft fill (overhead)
    shade1 = hillshade(nx, ny, nz, sun_az=315, sun_el=45)
    shade2 = hillshade(nx, ny, nz, sun_az=135, sun_el=60)  # fill from SE
    shade  = shade1 * 0.7 + shade2 * 0.3 + 0.15           # ambient floor

    # Darken valleys, brighten peaks
    shade = np.clip(shade, 0.4, 1.35)

    for c in range(3):
        rgb[:,:,c] = np.clip(rgb[:,:,c].astype(np.float32) * shade, 0, 255).astype(np.uint8)

    # ── 5. Atmospheric haze at ocean edges ───────────────────────────────────
    gz, gx = np.mgrid[0:S, 0:S]
    dist = np.sqrt(((gx/S - 29/64)**2 + (gz/S - 26/64)**2) / (0.5**2))
    ocean_frac = gaussian_filter(np.clip((dist - 1.05) / 0.3, 0, 1).astype(np.float32), sigma=S//80)
    fog_col = np.array([45, 75, 125], dtype=np.float32)
    for c in range(3):
        rgb[:,:,c] = np.clip(
            rgb[:,:,c].astype(np.float32) * (1 - ocean_frac) + fog_col[c] * ocean_frac,
            0, 255).astype(np.uint8)

    # ── 6. Subtle vignette ───────────────────────────────────────────────────
    cy, cx = S//2, S//2
    Y, X   = np.mgrid[0:S, 0:S]
    vignette = 1.0 - 0.25 * ((X-cx)**2 + (Y-cy)**2) / (0.7 * S)**2
    vignette = np.clip(vignette, 0.75, 1.0).astype(np.float32)
    for c in range(3):
        rgb[:,:,c] = np.clip(rgb[:,:,c] * vignette, 0, 255).astype(np.uint8)

    img  = Image.fromarray(rgb, 'RGB')
    draw = ImageDraw.Draw(img)

    # ── 7. Zone labels and markers ───────────────────────────────────────────
    print("[mapgen] drawing zone labels…")
    px_per_zone = S / ATLAS_ZONES

    for (gx_z, gz_z), z in zones.items():
        if gx_z >= ATLAS_ZONES or gz_z >= ATLAS_ZONES: continue
        # Centre of zone in image pixels
        cx_px = int((gx_z + 0.5) * px_per_zone)
        cy_px = int((gz_z + 0.5) * px_per_zone)

        name   = z.get('name', z.get('id', '?'))
        biome  = z.get('biome', 'unknown')
        danger = int(z.get('danger', 1))

        # Marker dot — size by danger level
        r   = 3 + danger // 2
        col = (255, 220, 100) if danger <= 2 else \
              (255, 155,  60) if danger <= 4 else (255, 80, 80)
        draw.ellipse([cx_px-r, cy_px-r, cx_px+r, cy_px+r],
                     fill=col, outline=(0,0,0))

        # Abbreviated name (max 2 words, 12 chars)
        words = name.split()
        short = ' '.join(words[:2])[:14]
        draw.text((cx_px + r + 2, cy_px - 5), short,
                  fill=(255, 245, 200))

    return img


def main():
    root    = os.path.join(os.path.dirname(__file__), '..')
    r32     = os.path.join(root, 'game', 'data', 'terrain', 'world_hmap.r32')
    cfg     = os.path.join(root, 'game', 'data', 'terrain_config.txt')
    out     = os.path.join(root, 'game', 'data', 'textures', 'world_map.png')

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
    img.save(out, quality=95)
    print(f"[mapgen] saved {out}")

if __name__ == '__main__':
    main()
