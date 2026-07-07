#!/usr/bin/env python3
"""
md_worldmap_gen.py — Generate world_map.png using Kenshi biomemap zone shapes.

Pipeline:
  1. Read Kenshi biomemap.png (1024×1024): each unique colour = one zone
  2. For each colour cluster → compute centroid → map to zone grid (0-63)
  3. Match to our terrain_config.txt zones by nearest grid position
  4. Re-colour every pixel with our own biome palette (NO Kenshi colours remain)
  5. Apply hillshading from our heightmap, add danger markers + labels

Legal: we use zone boundary SHAPES (positional data), not Kenshi artwork.
       Every output pixel colour is derived from our own palette + our heightmap.

Usage: python3 tools/md_worldmap_gen.py
"""

import os, struct, math, hashlib
import numpy as np
from scipy.ndimage import gaussian_filter, zoom as sci_zoom
from scipy.spatial import cKDTree
from PIL import Image, ImageDraw

ATLAS_ZONES = 64
ATLAS_VERTS = 65
OUT_SIZE    = 2048

KENSHI_BIOME = "tmp_/kenshi/data/newland/land/biomemap.png"

# ── Per-biome colour palettes (vivid, clearly distinct) ──────────────────────
BIOME_PALETTE = {
    'desert':    [(235,210,140),(220,195,118),(248,225,155),(208,185,105),
                  (242,218,132),(226,202,124),(250,230,160)],
    'canyon':    [(195,100, 55),(178, 82, 40),(210,118, 68),(162, 72, 35),
                  (200,108, 58),(182, 90, 46),(215,128, 75)],
    'volcanic':  [( 85, 55, 90),( 68, 44, 80),(100, 62, 100),( 72, 48, 95),
                  ( 92, 60, 105),( 60, 50, 82),(108, 70, 108)],
    'swamp':     [( 55,118, 68),( 44,102, 56),( 66,132, 78),( 38, 90, 50),
                  ( 58,124, 72),( 50,110, 62),( 70,138, 82)],
    'scrubland': [(148,168, 98),(132,150, 84),(162,182,112),(124,140, 76),
                  (155,172,104),(140,158, 90),(168,188,118)],
    'highlands': [(108,100, 82),( 96, 90, 72),(120,112, 94),( 88, 82, 65),
                  (112,104, 86),(100, 94, 78),(128,120, 98)],
    'highland':  [(130,122, 98),(118,112, 88),(142,132,108),(108,102, 80),
                  (134,126,102),(122,116, 92),(148,138,112)],
    'ashlands':  [(162,138,100),(148,122, 88),(175,152,112),(135,112, 80),
                  (168,144,106),(154,130, 94),(180,158,118)],
    'coast':     [( 68,142,165),( 54,124,148),( 80,158,180),( 48,110,132),
                  ( 72,150,172),( 60,134,158),( 85,165,188)],
    '_ocean':    [( 28, 48, 88)],
    '_default':  [(158,148,115),(142,132,100),(168,158,125)],
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
    return (rgb_arr // tol * tol).astype(np.int32)


def segment_biomemap(biome_path, our_zones):
    print(f"[biome] loading {biome_path}…")
    img  = Image.open(biome_path).convert('RGB')
    H, W = img.size[1], img.size[0]
    arr  = np.array(img, dtype=np.int32)

    is_bg = (arr[:,:,1] > 200) & (arr[:,:,0] < 30) & (arr[:,:,2] < 30)
    q = quantize_color(arr)

    flat = q[~is_bg]
    unique_cols, _ = np.unique(flat.reshape(-1, 3), axis=0, return_inverse=True)
    print(f"[biome] {len(unique_cols)} unique zone colours found")

    px_per_zone = W / ATLAS_ZONES
    col_key = q[:,:,0]*65536 + q[:,:,1]*256 + q[:,:,2]

    col2idx = {}
    for i, c in enumerate(unique_cols):
        k = int(c[0])*65536 + int(c[1])*256 + int(c[2])
        col2idx[k] = i

    seg_col = np.full((H, W), -1, dtype=np.int32)
    for ky, row in enumerate(col_key):
        for kx, k in enumerate(row):
            if not is_bg[ky, kx] and k in col2idx:
                seg_col[ky, kx] = col2idx[k]

    our_pts  = np.array([(gz, gx) for (gx, gz) in our_zones], dtype=np.float32)
    our_keys = list(our_zones.keys())
    tree     = cKDTree(our_pts)

    col_to_zone = {}
    for i, c in enumerate(unique_cols):
        mask = (seg_col == i)
        if not mask.any():
            continue
        ys, xs = np.where(mask)
        gz_f   = ys.mean() / px_per_zone
        gx_f   = xs.mean() / px_per_zone
        _, ni  = tree.query([gz_f, gx_f])
        col_to_zone[i] = our_keys[ni]

    zone_key_list = list(our_zones.keys())
    zk2idx        = {k: i for i, k in enumerate(zone_key_list)}

    seg = np.full((H, W), -1, dtype=np.int32)
    for ci, zk in col_to_zone.items():
        seg[seg_col == ci] = zk2idx[zk]

    print(f"[biome] mapped {len(col_to_zone)} colour regions → "
          f"{len(set(col_to_zone.values()))} zones")
    return seg, zone_key_list


# ── Recolour + edges ──────────────────────────────────────────────────────────

def build_rgb_from_seg(seg, zone_key_list, our_zones, out_size):
    H_src, W_src = seg.shape
    S = out_size

    n_zones   = len(zone_key_list)
    pal_table = np.zeros((n_zones + 1, 3), dtype=np.float32)
    ocean_col = np.array(BIOME_PALETTE['_ocean'][0], dtype=np.float32)
    pal_table[0] = ocean_col
    for i, (gx, gz) in enumerate(zone_key_list):
        z     = our_zones.get((gx, gz), {})
        biome = z.get('biome', '_default')
        pal_table[i + 1] = zone_color(biome, gx, gz)

    # Fill any ocean pixels that fall inside a known zone's grid cell.
    # Kenshi biomemap only maps ~30 of our 59 zones; the rest appear as ocean.
    # Fix: nearest-zone fill from our grid positions so every zone has colour.
    zone_pts  = np.array([(gz / ATLAS_ZONES * H_src, gx / ATLAS_ZONES * W_src)
                           for (gx, gz) in zone_key_list], dtype=np.float32)
    zone_tree = cKDTree(zone_pts)
    ocean_mask = (seg == -1)
    if ocean_mask.any():
        oy, ox = np.where(ocean_mask)
        coords = np.stack([oy, ox], axis=1).astype(np.float32)
        dists, near_idx = zone_tree.query(coords, workers=-1)
        # Only fill cells that are close enough to a zone centre (within 1 zone width)
        cell_w = W_src / ATLAS_ZONES
        fill_mask = dists < cell_w * 0.8
        seg_fill = seg.copy()
        seg_fill[oy[fill_mask], ox[fill_mask]] = near_idx[fill_mask]
        filled = int(fill_mask.sum())
        print(f"[mapgen] filled {filled} ocean pixels with nearest-zone colour")
        seg = seg_fill

    seg_shifted = seg + 1
    rgb_src = pal_table[seg_shifted].astype(np.float32)

    # ── Soften zone-colour transitions ────────────────────────────────────────
    # Real Kenshi's in-game map is continuous relief-shaded terrain colour with
    # no political borders (verified against actual gameplay footage). A hard
    # per-pixel edge darken pass (previous approach) reads as a political/
    # administrative map instead. Blur the colour field so zones blend into
    # each other like natural terrain colour variation; hillshade (applied
    # after this function) supplies the actual relief definition.
    px_per_zone = W_src / ATLAS_ZONES
    blend_sigma = px_per_zone * 0.12
    rgb_src = np.stack(
        [gaussian_filter(rgb_src[:, :, c], sigma=blend_sigma) for c in range(3)],
        axis=2)

    # ── Subtle FBM texture variation within zones ────────────────────────────
    rng = np.random.default_rng(7)
    def micro_fbm(res, freq=0.03, oct=4):
        out = np.zeros((res, res), dtype=np.float32); amp = 1.; f = freq; norm = 0.
        for _ in range(oct):
            gs = max(4, int(res * f) + 2)
            g  = rng.random((gs + 2, gs + 2)).astype(np.float32)
            xs = np.linspace(0, gs - 1, res, endpoint=False)
            R, C = np.meshgrid(xs, xs, indexing='ij')
            RI = R.astype(int); RF = R - RI; RI1 = np.clip(RI + 1, 0, gs)
            CI = C.astype(int); CF = C - CI; CI1 = np.clip(CI + 1, 0, gs)
            v = (g[RI, CI]*(1-RF)*(1-CF) + g[RI, CI1]*(1-RF)*CF +
                 g[RI1, CI]*RF*(1-CF) + g[RI1, CI1]*RF*CF)
            out += amp * v; norm += amp; amp *= 0.5; f *= 2.
        return out / norm
    tex = micro_fbm(W_src) * 0.10 + 0.95   # ±5% variation
    rgb_src = np.clip(rgb_src * tex[:, :, None], 0, 255)

    # ── Upscale to output resolution ─────────────────────────────────────────
    print(f"[mapgen] upscaling {W_src}→{S}…")
    scale  = S / W_src
    rgb_up = np.stack([sci_zoom(rgb_src[:, :, c], scale, order=1) for c in range(3)], axis=2)
    return np.clip(rgb_up, 0, 255).astype(np.float32)


def apply_hillshade(rgb_f, hmap, full, S):
    scale   = S / full
    h_small = sci_zoom(hmap, scale, order=1).astype(np.float32)
    h_small = gaussian_filter(h_small, sigma=0.5)   # less blur = sharper hills

    # 460.8m/zone — real Kenshi zone size (engine/include/monkey_dust/world/chunk_def.h
    # CHUNK_SIZE); was stale 500.0 here, the same wrong assumption fixed engine-side.
    world_m_per_px = (ATLAS_ZONES * 460.8) / S
    k  = 0.006 / world_m_per_px                      # stronger slope contrast
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

    # Two lights: primary NW, secondary SE
    sh = np.clip(shade(315, 45)*0.80 + shade(135, 55)*0.20 + 0.15, 0.32, 1.40)
    return np.clip(rgb_f * sh[:, :, None], 0, 255).astype(np.float32)


def draw_labels(img, zones, S):
    draw = ImageDraw.Draw(img)
    ppz  = S / ATLAS_ZONES
    DCOL = {1:(255,240,140), 2:(255,225,110), 3:(255,195, 70),
            4:(255,155, 45),  5:(255,115, 35),  6:(255, 80, 55),
            7:(255, 55, 55),  8:(225, 38, 38),  9:(185, 20, 20)}
    for (gx, gz), z in zones.items():
        if gx >= ATLAS_ZONES or gz >= ATLAS_ZONES: continue
        cx_px = int((gx + 0.5) * ppz)
        cy_px = int((gz + 0.5) * ppz)
        name  = z.get('name', z.get('id', '?'))
        danger = int(z.get('danger', 1))
        r = 4 + danger // 2                           # larger dots
        c = DCOL.get(danger, (255, 80, 80))
        draw.ellipse([cx_px-r, cy_px-r, cx_px+r, cy_px+r], fill=c, outline=(0, 0, 0))
        short = ' '.join(name.split()[:2])[:14]
        # Shadow + bright label
        draw.text((cx_px+r+3, cy_px-5), short, fill=(0, 0, 0))
        draw.text((cx_px+r+2, cy_px-6), short, fill=(255, 250, 215))


# ── Fallback: domain-warped Voronoi (if biomemap not available) ───────────────

def fallback_voronoi(zones, S):
    rng = np.random.default_rng(42)
    px  = S / ATLAS_ZONES
    pts, cols = [], []
    for (gx, gz), z in zones.items():
        if gx >= ATLAS_ZONES or gz >= ATLAS_ZONES: continue
        pts.append((gz*px+px*0.5, gx*px+px*0.5))
        b   = z.get('biome', '_default')
        h   = int(hashlib.md5(f"{gx},{gz}".encode()).hexdigest(), 16)
        pal = BIOME_PALETTE.get(b, BIOME_PALETTE['_default'])
        cols.append(np.array(pal[h % len(pal)], dtype=np.float32))
    pts_np  = np.array(pts,  dtype=np.float32)
    cols_np = np.array(cols, dtype=np.float32)

    def fbm(res, freq=0.004, oct=7):
        out = np.zeros((res, res), dtype=np.float32); amp = 1.; f = freq; norm = 0.
        for _ in range(oct):
            gs = max(4, int(res*f)+2); g = rng.random((gs+2, gs+2)).astype(np.float32)
            xs = np.linspace(0, gs-1, res, endpoint=False)
            R, C = np.meshgrid(xs, xs, indexing='ij')
            RI = R.astype(int); RF = R-RI; RI1 = np.clip(RI+1, 0, gs)
            CI = C.astype(int); CF = C-CI; CI1 = np.clip(CI+1, 0, gs)
            v = (g[RI,CI]*(1-RF)*(1-CF)+g[RI,CI1]*(1-RF)*CF+
                 g[RI1,CI]*RF*(1-CF)+g[RI1,CI1]*RF*CF)
            out += amp*v; norm += amp; amp *= 0.52; f *= 2.
        return out/norm

    wp = px * 1.8
    wx = (fbm(S, 0.0035)-0.5)*2*wp + (fbm(S, 0.007)-0.5)*2*wp*0.4
    wy = (fbm(S, 0.0035)-0.5)*2*wp + (fbm(S, 0.007)-0.5)*2*wp*0.4
    yi, xi = np.mgrid[0:S, 0:S].astype(np.float32)
    coords  = np.stack([np.clip(yi+wy, 0, S-1).ravel(),
                        np.clip(xi+wx, 0, S-1).ravel()], axis=1)
    tree = cKDTree(pts_np)
    dists, idx = tree.query(coords, workers=-1)
    rgb_f = cols_np[idx.reshape(S, S)]

    ocean = np.array(BIOME_PALETTE['_ocean'][0], dtype=np.float32)
    of = np.clip((dists.reshape(S, S)/px-3.5)/4., 0, 1)
    of = gaussian_filter(of, sigma=6.)[:, :, None]
    return (rgb_f*(1-of) + ocean*of).astype(np.float32)


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
        print("[mapgen] using Kenshi biomemap zone shapes")
        seg, zone_key_list = segment_biomemap(KENSHI_BIOME, zones)
        rgb_f = build_rgb_from_seg(seg, zone_key_list, zones, S)
    else:
        print("[mapgen] Kenshi biomemap not found — using domain-warped Voronoi")
        rgb_f = fallback_voronoi(zones, S)

    print("[mapgen] hillshading…")
    rgb_f = apply_hillshade(rgb_f, hmap, full, S)

    img = Image.fromarray(np.clip(rgb_f, 0, 255).astype(np.uint8), 'RGB')
    draw_labels(img, zones, S)

    os.makedirs(os.path.dirname(out), exist_ok=True)
    img.save(out)
    print(f"[mapgen] saved {out} ({S}×{S})")


if __name__ == '__main__':
    main()
