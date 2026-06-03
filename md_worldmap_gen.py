#!/usr/bin/env python3
"""
md_worldmap_gen.py — Generate world_map.png using Kenshi biomemap zone shapes.

Pipeline:
  1. Read Kenshi biomemap.png (1024×1024): each unique colour = one zone
  2. For each colour cluster → compute centroid → map to zone grid (0-63)
  3. Match to our terrain_config.txt zones by nearest grid position
  4. Re-colour every pixel with our own biome palette (NO Kenshi colours remain)
  5. Upscale to 2048×2048, apply our heightmap hillshading, add labels

Result: organic Kenshi-quality zone boundaries, entirely our own colour scheme.
Legal: we use zone boundary SHAPES (positional/functional data), not Kenshi artwork.
       Every output pixel colour is derived from our own palette + our own heightmap.

Usage: python3 tools/md_worldmap_gen.py
"""

import os, struct, math, hashlib
import numpy as np
from scipy.ndimage import gaussian_filter, zoom as sci_zoom, label as nd_label
from scipy.spatial import cKDTree
from PIL import Image, ImageDraw

ATLAS_ZONES = 64
ATLAS_VERTS = 65
OUT_SIZE    = 2048

KENSHI_BIOME = "/run/media/rdga1/win/SteamLibrary/steamapps/common/Kenshi/" \
               "data/newland/land/biomemap.png"

# ── Per-biome colour palettes ─────────────────────────────────────────────────
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
    '_ocean':    [( 30, 52, 95)],
    '_default':  [(148,138,108),(135,125, 98),(158,148,118)],
}

def zone_color(biome, gx, gz):
    h   = int(hashlib.md5(f"{gx},{gz}".encode()).hexdigest(), 16)
    pal = BIOME_PALETTE.get(biome, BIOME_PALETTE['_default'])
    return np.array(pal[h % len(pal)], dtype=np.float32)


# ── Loaders ───────────────────────────────────────────────────────────────────

def load_atlas(r32_path):
    if not os.path.exists(r32_path):
        return None, None
    print(f"[mapgen] loading heightmap…")
    with open(r32_path, 'rb') as f:
        magic, _, _, verts = struct.unpack('<4I', f.read(16))
        if magic != 0x414D4800:
            return None, None
        G = verts - 1; full = ATLAS_ZONES * G
        hmap = np.zeros((full, full), dtype=np.float32)
        for zi in range(ATLAS_ZONES * ATLAS_ZONES):
            struct.unpack('<ff', f.read(8))
            zone = np.frombuffer(f.read(verts*verts*4), dtype=np.float32).reshape(verts, verts)
            zy_i, zx_i = zi // ATLAS_ZONES, zi % ATLAS_ZONES
            hmap[zy_i*G:zy_i*G+G, zx_i*G:zx_i*G+G] = zone[:G, :G]
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
                k, v = s.split('=', 1)
                cur[k.strip()] = v.strip()
    if cur and 'grid_x' in cur and 'grid_z' in cur:
        zones[(int(cur['grid_x']), int(cur['grid_z']))] = cur.copy()
    return zones


# ── Biomemap segmentation ─────────────────────────────────────────────────────

def quantize_color(rgb_arr, tol=6):
    """
    Reduce colours by rounding to nearest multiple of tol.
    Handles slight anti-aliasing at zone edges.
    """
    return (rgb_arr // tol * tol).astype(np.int32)


def segment_biomemap(biome_path, our_zones):
    """
    Read Kenshi biomemap, segment into zone regions, match each region
    to our nearest zone by centroid grid position.

    Returns:
        seg   — (H, W) int32 array: pixel → zone index (-1 = unassigned)
        zones_ordered — list of (gx, gz) matching seg indices
    """
    print(f"[biome] loading {biome_path}…")
    img  = Image.open(biome_path).convert('RGB')
    H, W = img.size[1], img.size[0]   # 1024, 1024
    arr  = np.array(img, dtype=np.int32)   # (H, W, 3)

    # Background = bright green (0,255,0) or close variants
    is_bg = (arr[:,:,1] > 200) & (arr[:,:,0] < 30) & (arr[:,:,2] < 30)

    # Quantise to merge near-identical colours (edge blending)
    q = quantize_color(arr)

    # Find unique non-background colours
    flat = q[~is_bg]                         # (N, 3)
    unique_cols, inv = np.unique(
        flat.reshape(-1, 3), axis=0, return_inverse=True)
    print(f"[biome] {len(unique_cols)} unique zone colours found")

    # For each unique colour → centroid in pixel space → grid coords
    # biomemap 1024×1024 covers Kenshi 64×64 zone grid
    px_per_zone = W / ATLAS_ZONES           # 16 px/zone

    # Build pixel → colour index map
    col_key = q[:,:,0]*65536 + q[:,:,1]*256 + q[:,:,2]
    bg_key  = (is_bg).astype(np.int32) * -1

    col2idx = {}
    for i, c in enumerate(unique_cols):
        k = int(c[0])*65536 + int(c[1])*256 + int(c[2])
        col2idx[k] = i

    seg_col = np.full((H, W), -1, dtype=np.int32)
    for ky, row in enumerate(col_key):
        for kx, k in enumerate(row):
            if not is_bg[ky, kx] and k in col2idx:
                seg_col[ky, kx] = col2idx[k]

    # Compute centroids for each colour cluster
    our_pts  = np.array([(gz, gx) for (gx, gz) in our_zones], dtype=np.float32)
    our_keys = list(our_zones.keys())
    tree     = cKDTree(our_pts)

    col_to_zone = {}   # colour_idx → (gx, gz)
    for i, c in enumerate(unique_cols):
        mask = (seg_col == i)
        if not mask.any():
            continue
        ys, xs = np.where(mask)
        cy_px  = ys.mean()
        cx_px  = xs.mean()
        # Convert pixel centroid → zone grid position
        gz_f   = cy_px / px_per_zone
        gx_f   = cx_px / px_per_zone
        # Find nearest our-zone
        _, ni  = tree.query([gz_f, gx_f])
        col_to_zone[i] = our_keys[ni]

    # Build final seg map: pixel → our zone (gx, gz) index
    zone_key_list = list(our_zones.keys())
    zk2idx        = {k: i for i, k in enumerate(zone_key_list)}

    seg = np.full((H, W), -1, dtype=np.int32)
    for ci, zk in col_to_zone.items():
        seg[seg_col == ci] = zk2idx[zk]

    print(f"[biome] mapped {len(col_to_zone)} colour regions → "
          f"{len(set(col_to_zone.values()))} zones")
    return seg, zone_key_list


# ── Recolour + hillshade ──────────────────────────────────────────────────────

def build_rgb_from_seg(seg, zone_key_list, our_zones, out_size):
    """
    seg: (H,W) zone index (-1 = unassigned)
    Recolours every pixel with our palette.  Upscales to out_size.
    """
    H_src, W_src = seg.shape
    S = out_size

    # ── Zone colour lookup table ──────────────────────────────────────────────
    n_zones   = len(zone_key_list)
    pal_table = np.zeros((n_zones + 1, 3), dtype=np.float32)
    ocean_col = np.array(BIOME_PALETTE['_ocean'][0], dtype=np.float32)
    pal_table[0] = ocean_col   # index -1 → ocean
    for i, (gx, gz) in enumerate(zone_key_list):
        z    = our_zones.get((gx, gz), {})
        biome = z.get('biome', '_default')
        pal_table[i + 1] = zone_color(biome, gx, gz)

    # Shift seg so -1 → index 0 (ocean)
    seg_shifted = seg + 1   # -1→0, zone_i→i+1

    # ── Vectorised recolour at source resolution ──────────────────────────────
    rgb_src = pal_table[seg_shifted]   # (H_src, W_src, 3)

    # ── Thin dark edge between adjacent different zones ───────────────────────
    # Dilate zone boundaries: where neighbouring pixels differ → darken
    from scipy.ndimage import uniform_filter
    seg_f   = seg_shifted.astype(np.float32)
    blur    = uniform_filter(seg_f, size=2)
    is_edge = np.abs(blur - seg_f) > 0.1
    edge_w  = gaussian_filter(is_edge.astype(np.float32), sigma=0.8)
    edge_w  = np.clip(edge_w * 1.2, 0, 0.6)[:, :, None]
    rgb_src = rgb_src * (1.0 - edge_w)

    # ── Upscale to output resolution ─────────────────────────────────────────
    print(f"[mapgen] upscaling {W_src}→{S}…")
    scale   = S / W_src
    rgb_up  = np.stack([
        sci_zoom(rgb_src[:,:,c], scale, order=1)
        for c in range(3)], axis=2)

    return np.clip(rgb_up, 0, 255).astype(np.float32)


def apply_hillshade(rgb_f, hmap, full, S):
    scale   = S / full
    h_small = sci_zoom(hmap, scale, order=1).astype(np.float32)
    h_small = gaussian_filter(h_small, sigma=0.6)

    world_m_per_px = (ATLAS_ZONES * 500.0) / S
    k  = 0.003 / world_m_per_px
    dz = np.gradient(h_small, axis=0) * k
    dx = np.gradient(h_small, axis=1) * k
    mag = np.sqrt(dx*dx + dz*dz + 1.0)
    nx, ny, nz = -dx/mag, 1.0/mag, -dz/mag

    def shade(az, el):
        ar, er = math.radians(az), math.radians(el)
        lx = math.cos(er)*math.cos(ar)
        lz = math.cos(er)*math.sin(ar)
        ly = math.sin(er)
        return np.clip(nx*lx + ny*ly + nz*lz, 0.0, 1.0)

    sh = np.clip(shade(315,45)*0.72 + shade(135,60)*0.28 + 0.22, 0.38, 1.35)
    return np.clip(rgb_f * sh[:,:,None], 0, 255).astype(np.float32)


def apply_vignette(rgb_f, S):
    cy, cx = S//2, S//2
    Y, X   = np.mgrid[0:S, 0:S]
    vig    = np.clip(1.0 - 0.16*((X-cx)**2+(Y-cy)**2)/(0.72*S)**2, 0.84, 1.0)
    return np.clip(rgb_f * vig[:,:,None], 0, 255)


def draw_labels(img, zones, S):
    draw = ImageDraw.Draw(img)
    ppz  = S / ATLAS_ZONES
    DCOL = {1:(255,235,130),2:(255,220,100),3:(255,185,60),
            4:(255,145,40), 5:(255,105,30),6:(255,75,50),
            7:(255,50,50),  8:(220,35,35), 9:(180,18,18)}
    for (gx, gz), z in zones.items():
        if gx >= ATLAS_ZONES or gz >= ATLAS_ZONES: continue
        cx_px = int((gx + 0.5) * ppz)
        cy_px = int((gz + 0.5) * ppz)
        name  = z.get('name', z.get('id', '?'))
        danger = int(z.get('danger', 1))
        r = 3 + danger // 2
        c = DCOL.get(danger, (255,80,80))
        draw.ellipse([cx_px-r, cy_px-r, cx_px+r, cy_px+r], fill=c, outline=(0,0,0))
        short = ' '.join(name.split()[:2])[:14]
        draw.text((cx_px+r+3, cy_px-5), short, fill=(0,0,0))
        draw.text((cx_px+r+2, cy_px-6), short, fill=(255,248,210))


# ── Fallback: domain-warped Voronoi (if biomemap not available) ───────────────

def fallback_voronoi(zones, hmap, full, S):
    """Domain-warped Voronoi (no Kenshi file needed)."""
    from scipy.spatial import cKDTree as _KDTree
    import hashlib as _hl

    px = S / ATLAS_ZONES
    pts, cols = [], []
    for (gx, gz), z in zones.items():
        if gx >= ATLAS_ZONES or gz >= ATLAS_ZONES: continue
        pts.append((gz*px+px*0.5, gx*px+px*0.5))
        b   = z.get('biome', '_default')
        h   = int(_hl.md5(f"{gx},{gz}".encode()).hexdigest(), 16)
        pal = BIOME_PALETTE.get(b, BIOME_PALETTE['_default'])
        cols.append(np.array(pal[h % len(pal)], dtype=np.float32))
    pts_np = np.array(pts, dtype=np.float32)
    cols_np = np.array(cols, dtype=np.float32)

    rng = np.random.default_rng(42)
    def fbm(res, freq=0.004, oct=7):
        out = np.zeros((res,res),dtype=np.float32); amp=1.; f=freq; norm=0.
        for _ in range(oct):
            gs=max(4,int(res*f)+2); g=rng.random((gs+2,gs+2)).astype(np.float32)
            xs=np.linspace(0,gs-1,res,endpoint=False)
            R,C=np.meshgrid(xs,xs,indexing='ij')
            RI=R.astype(int); RF=R-RI; RI1=np.clip(RI+1,0,gs)
            CI=C.astype(int); CF=C-CI; CI1=np.clip(CI+1,0,gs)
            v=(g[RI,CI]*(1-RF)*(1-CF)+g[RI,CI1]*(1-RF)*CF+
               g[RI1,CI]*RF*(1-CF)+g[RI1,CI1]*RF*CF)
            out+=amp*v; norm+=amp; amp*=0.52; f*=2.
        return out/norm
    wp = px*1.8
    wx=(fbm(S,0.0035)-0.5)*2*wp+(fbm(S,0.007)-0.5)*2*wp*0.4
    wy=(fbm(S,0.0035)-0.5)*2*wp+(fbm(S,0.007)-0.5)*2*wp*0.4

    yi,xi=np.mgrid[0:S,0:S].astype(np.float32)
    coords=np.stack([np.clip(yi+wy,0,S-1).ravel(),
                     np.clip(xi+wx,0,S-1).ravel()],axis=1)
    tree=_KDTree(pts_np)
    dists,idx=tree.query(coords,workers=-1)
    rgb_f=cols_np[idx.reshape(S,S)]

    ocean=np.array(BIOME_PALETTE['_ocean'][0],dtype=np.float32)
    of=np.clip((dists.reshape(S,S)/px-3.5)/4.,0,1)
    of=gaussian_filter(of,sigma=6.)[:,:,None]
    rgb_f=rgb_f*(1-of)+ocean*of
    return rgb_f.astype(np.float32)


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    root = os.path.join(os.path.dirname(__file__), '..')
    r32  = os.path.join(root, 'game', 'data', 'terrain', 'world_hmap.r32')
    cfg  = os.path.join(root, 'game', 'data', 'terrain_config.txt')
    out  = os.path.join(root, 'game', 'data', 'textures', 'world_map.png')

    hmap, full = load_atlas(r32)
    if hmap is None:
        print("[mapgen] ERROR: heightmap not found"); return

    zones = load_zones(cfg)
    print(f"[mapgen] {len(zones)} zones")

    S = OUT_SIZE

    if os.path.exists(KENSHI_BIOME):
        # ── Path A: Kenshi biomemap available ────────────────────────────────
        print("[mapgen] using Kenshi biomemap zone shapes")
        seg, zone_key_list = segment_biomemap(KENSHI_BIOME, zones)
        rgb_f = build_rgb_from_seg(seg, zone_key_list, zones, S)
    else:
        # ── Path B: fallback domain-warped Voronoi ────────────────────────────
        print("[mapgen] Kenshi biomemap not found — using domain-warped Voronoi")
        rgb_f = fallback_voronoi(zones, hmap, full, S)

    print("[mapgen] hillshading…")
    rgb_f = apply_hillshade(rgb_f, hmap, full, S)
    rgb_f = apply_vignette(rgb_f, S)

    img = Image.fromarray(rgb_f.astype(np.uint8), 'RGB')
    draw_labels(img, zones, S)

    if os.path.exists(out):
        bak = out.replace('.png', '_orig.png')
        if not os.path.exists(bak):
            import shutil; shutil.copy2(out, bak)

    os.makedirs(os.path.dirname(out), exist_ok=True)
    img.save(out)
    print(f"[mapgen] saved {out} ({S}×{S})")


if __name__ == '__main__':
    main()
