#!/usr/bin/env python3
"""
md_worldmap_gen.py — Generate world_map.png from terrain_config.txt + heightmap.

Produces a clean top-down map with:
  - Height-shaded terrain (dark=low, light=high)
  - Biome color tinting per zone (Voronoi-style sharp boundaries)
  - Zone name labels
  - Road/river placeholder lines between zones

Usage: python3 tools/md_worldmap_gen.py
"""
import os, struct
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy.ndimage import gaussian_filter

ATLAS_ZONES = 64
W, H = 2048, 2048
PX = W / ATLAS_ZONES          # pixels per zone

BIOME_COL = {
    'desert':    (210, 180, 110),
    'canyon':    (165, 105,  65),
    'highlands': ( 95, 120,  75),
    'highland':  (105, 128,  80),
    'scrubland': (155, 155,  95),
    'swamp':     ( 70, 115,  80),
    'volcanic':  (110,  65,  55),
    'ashlands':  (135, 125, 115),
    'coast':     (160, 190, 205),
    'unknown':   (165, 160, 145),
}
OCEAN = (45, 75, 125)
TEXT_COL = (255, 245, 200)

def load_zones(path):
    zones = {}
    cur = {}
    with open(path) as f:
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

def voronoi_color_map(zones, w):
    """Build WxW color map: each pixel gets color of nearest zone (Voronoi)."""
    if not zones:
        return np.full((w, w, 3), OCEAN, dtype=np.uint8)

    gxs = np.array([p[0] for p in zones], dtype=np.float32)
    gzs = np.array([p[1] for p in zones], dtype=np.float32)
    # Pixel positions (image x=atlas cx, image y=atlas cz)
    py, px = np.mgrid[0:ATLAS_ZONES, 0:ATLAS_ZONES]
    py = py.astype(np.float32); px = px.astype(np.float32)

    # Nearest-zone assignment
    best_d  = np.full((ATLAS_ZONES, ATLAS_ZONES), np.inf, dtype=np.float32)
    best_i  = np.zeros((ATLAS_ZONES, ATLAS_ZONES), dtype=np.int32)
    zlist   = list(zones.values())
    for i, (gx, gz) in enumerate(zones):
        d = (px - gx)**2 + (py - gz)**2
        mask = d < best_d
        best_d[mask] = d[mask]
        best_i[mask] = i

    cols = np.array([BIOME_COL.get(z.get('biome','unknown'), BIOME_COL['unknown'])
                     for z in zlist], dtype=np.float32)  # (N,3)
    cmap = cols[best_i]  # (64,64,3)

    # Coastal ocean (fade edges)
    gy, gx_m = np.mgrid[0:ATLAS_ZONES, 0:ATLAS_ZONES]
    dist = np.sqrt(((gx_m - 29.0)/23.0)**2 + ((gy - 26.0)/20.0)**2)
    ocean_frac = gaussian_filter(np.clip((dist - 1.0) / 0.4, 0, 1).astype(np.float32), sigma=2)
    oc = np.array(OCEAN, dtype=np.float32)
    for c in range(3):
        cmap[:,:,c] = cmap[:,:,c] * (1 - ocean_frac) + oc[c] * ocean_frac

    # Upsample with nearest + slight blur for soft zone borders
    from scipy.ndimage import zoom
    scale = w / ATLAS_ZONES
    rgb_big = np.zeros((w, w, 3), dtype=np.float32)
    for c in range(3):
        rgb_big[:,:,c] = zoom(cmap[:,:,c], scale, order=0)  # nearest = sharp borders

    # Subtle soft blur only inside zones (blurs ±4px at zone edges)
    rgb_blur = np.zeros_like(rgb_big)
    for c in range(3):
        rgb_blur[:,:,c] = gaussian_filter(rgb_big[:,:,c], sigma=1.5)
    # Blend: 80% sharp + 20% blurred
    rgb_big = rgb_big * 0.8 + rgb_blur * 0.2

    return np.clip(rgb_big, 0, 255).astype(np.uint8)

def load_heights(r32):
    if not os.path.exists(r32): return None
    with open(r32,'rb') as f:
        if struct.unpack('<I', f.read(4))[0] != 0x414D4800: return None
        f.seek(0); f.read(16)
        means = np.zeros((ATLAS_ZONES, ATLAS_ZONES), dtype=np.float32)
        for zi in range(ATLAS_ZONES**2):
            f.read(8)
            h = np.frombuffer(f.read(65*65*4), dtype=np.float32)
            means[zi//ATLAS_ZONES, zi%ATLAS_ZONES] = h.mean()
    return means

def generate_map(zones, r32=None):
    print("[mapgen] Voronoi color map…")
    rgb = voronoi_color_map(zones, W)

    if r32:
        print("[mapgen] height shading…")
        means = load_heights(r32)
        if means is not None:
            hmax = float(np.percentile(means[means > 1], 95))
            norm = np.clip(means / max(hmax, 1.0), 0, 1)
            from scipy.ndimage import zoom
            shade = zoom(norm, W / ATLAS_ZONES, order=1)
            # Hillshade: high=brighten, low=darken (±30% effect)
            mult = 0.70 + shade * 0.60
            for c in range(3):
                rgb[:,:,c] = np.clip(rgb[:,:,c] * mult, 0, 255).astype(np.uint8)

    # Zone border lines (1px white outline between different biomes)
    img  = Image.fromarray(rgb, 'RGB')
    draw = ImageDraw.Draw(img)

    # Draw zone boundary grid lines (faint)
    for i in range(1, ATLAS_ZONES):
        x = int(i * PX)
        draw.line([(x, 0), (x, H-1)], fill=(0,0,0,30), width=1)
    for i in range(1, ATLAS_ZONES):
        y = int(i * PX)
        draw.line([(0, y), (W-1, y)], fill=(0,0,0,30), width=1)

    # Zone markers and names
    print("[mapgen] drawing labels…")
    for (gx, gz), z in zones.items():
        if gx >= ATLAS_ZONES or gz >= ATLAS_ZONES: continue
        cx = int(gx * PX + PX * 0.5)
        cy = int(gz * PX + PX * 0.5)

        name = z.get('name', z.get('id', '?'))
        biome = z.get('biome', 'unknown')

        # Dot (biome-colored outline)
        r = 5
        outline = (255, 255, 255)
        fill_col = BIOME_COL.get(biome, BIOME_COL['unknown'])
        draw.ellipse([cx-r, cy-r, cx+r, cy+r], fill=fill_col, outline=outline, width=1)

        # Name label (abbreviated if long)
        words = name.split()
        if len(name) > 16 and len(words) > 1:
            name = '\n'.join(w[:5] for w in words[:2])
        draw.text((cx + r + 3, cy - 7), name, fill=TEXT_COL)

    return img

def main():
    root     = os.path.join(os.path.dirname(__file__), '..')
    cfg      = os.path.join(root, 'game', 'data', 'terrain_config.txt')
    r32      = os.path.join(root, 'game', 'data', 'terrain', 'world_hmap.r32')
    out      = os.path.join(root, 'game', 'data', 'textures', 'world_map.png')

    print(f"[mapgen] loading {len(open(cfg).readlines())} config lines…")
    zones = load_zones(cfg)
    print(f"[mapgen] {len(zones)} zones")

    if os.path.exists(out):
        bak = out.replace('.png', '_orig.png')
        if not os.path.exists(bak):
            import shutil; shutil.copy2(out, bak)
            print(f"[mapgen] backed up → {bak}")

    img = generate_map(zones, r32)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    img.save(out)
    print(f"[mapgen] saved {out} ({img.width}×{img.height})")

if __name__ == '__main__':
    main()
