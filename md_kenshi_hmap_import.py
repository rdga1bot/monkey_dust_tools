#!/usr/bin/env python3
"""
md_kenshi_hmap_import.py — Convert Kenshi fullmap.tif → monkey_dust world_hmap.r32

Kenshi fullmap.tif:
  Size   : 16385×16385 px (uint16)
  Layout : 64×64 zones, each zone = 256×256 px + 1 shared edge pixel
  Values : 0–65535 raw uint16 (Kenshi internal height units)

Our world_hmap.r32 (TerrainAtlas):
  Layout : 64×64 zones, each zone = 65×65 float32 vertices
  Values : metres above sea level (0–200m range typical)

Conversion:
  1. For each zone (cz, cx): extract 257×257 px tile from fullmap
  2. Downsample 257→65 (take every 4th pixel = exact subdivision)
  3. Normalise Kenshi uint16 → metres using percentile mapping
  4. Write TerrainAtlas format

Height calibration:
  Kenshi terrain is defined in some internal unit where sea level ≈ 0.
  We map p2–p98 of the non-zero terrain to 0–200m game range,
  clamping extreme outliers and preserving all relative relief.

Usage:
  python3 tools/md_kenshi_hmap_import.py [--out game/data/terrain/world_hmap.r32]
                                          [--max-height 200]
"""

import argparse, os, struct
import numpy as np
from PIL import Image
from scipy.ndimage import zoom as sci_zoom, gaussian_filter

Image.MAX_IMAGE_PIXELS = None   # large tif

KENSHI_FULLMAP = ("/run/media/rdga1/win/SteamLibrary/steamapps/common/Kenshi/"
                  "data/newland/land/fullmap.tif")
ATLAS_MAGIC    = 0x414D4800
ATLAS_ZONES    = 64
ATLAS_VERTS    = 65   # 65×65 vertices per zone (64 unique + 1 shared edge)
SRC_VERTS_ZONE = 256  # Kenshi: 256×256 px per zone (+ 1 shared edge = 257)


def load_and_normalise(path, max_height_m=200.0):
    """
    Load fullmap.tif, normalise to metres.
    Returns float32 array of shape (16385, 16385).
    """
    print(f"[import] loading {path}  (512 MB, may take ~30s)…")
    img = Image.open(path)
    raw = np.array(img, dtype=np.float32)   # uint16 → float32

    # Non-zero terrain statistics (zeros = sea / below-map areas)
    nonzero = raw[raw > 0]
    lo  = float(np.percentile(nonzero, 2))
    hi  = float(np.percentile(nonzero, 98))
    print(f"[import] height raw range: 0 – {raw.max():.0f}")
    print(f"[import] non-zero p2={lo:.0f}  p98={hi:.0f}")

    # Normalise: lo→0m, hi→max_height_m
    normed = np.clip((raw - lo) / (hi - lo), 0.0, 1.0) * max_height_m
    # Pixels that were 0 in raw stay at 0m (sea level)
    normed[raw == 0] = 0.0

    print(f"[import] normalised range: {normed.min():.1f} – {normed.max():.1f} m")
    return normed


def extract_zone(hmap_full, cz, cx):
    """
    Extract the 65×65 vertex tile for zone (cz, cx).
    Kenshi: zone (cz,cx) starts at pixel (cz*256, cx*256).
    We sample every 4th pixel: 0,4,8,…,256 → 65 values.
    """
    r0 = cz * SRC_VERTS_ZONE
    c0 = cx * SRC_VERTS_ZONE
    tile = hmap_full[r0:r0+257, c0:c0+257]   # 257×257
    # Subsample: take rows/cols 0,4,8,…,256 → 65 samples
    sampled = tile[::4, ::4]                  # 65×65
    if sampled.shape != (ATLAS_VERTS, ATLAS_VERTS):
        # Edge tile: pad if needed
        pad_r = ATLAS_VERTS - sampled.shape[0]
        pad_c = ATLAS_VERTS - sampled.shape[1]
        sampled = np.pad(sampled, ((0,pad_r),(0,pad_c)), mode='edge')
    return sampled.astype(np.float32)


def write_atlas(zones_h, out_path):
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    print(f"[write] {out_path}…")
    with open(out_path, 'wb') as f:
        f.write(struct.pack('<4I', ATLAS_MAGIC, ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS))
        for cz in range(ATLAS_ZONES):
            for cx in range(ATLAS_ZONES):
                z = zones_h[cz, cx]
                f.write(struct.pack('<ff', float(z.min()), float(z.max())))
                f.write(z.tobytes())
    mb = os.path.getsize(out_path) / 1024 / 1024
    print(f"[write] {mb:.1f} MB written")

    # Verify seams
    max_seam = 0.0
    for cz in range(ATLAS_ZONES - 1):
        for cx in range(ATLAS_ZONES - 1):
            max_seam = max(max_seam,
                float(np.abs(zones_h[cz,cx,-1,:] - zones_h[cz+1,cx,0,:]).max()),
                float(np.abs(zones_h[cz,cx,:,-1] - zones_h[cz,cx+1,:,0]).max()))
    print(f"[verify] max seam error: {max_seam:.4f}m")


def write_preview(zones_h, out_path):
    from PIL import Image as _Im
    V = 8; SZ = ATLAS_ZONES * V
    avg = np.zeros((SZ, SZ), dtype=np.float32)
    for cz in range(ATLAS_ZONES):
        for cx in range(ATLAS_ZONES):
            avg[cz*V:(cz+1)*V, cx*V:(cx+1)*V] = zones_h[cz,cx].mean()
    lo, hi = avg.min(), avg.max()
    norm   = (avg - lo) / max(hi - lo, 1.0)
    dz     = np.gradient(avg, axis=0); dx = np.gradient(avg, axis=1)
    shade  = np.clip(1.0/np.sqrt(dx*dx+dz*dz+1.0)*0.8+0.3, 0.2, 1.3)
    rgb    = np.zeros((SZ,SZ,3), dtype=np.uint8)
    for (t0,c0),(t1,c1) in zip(
        [(0.0,(30,50,90)),(0.02,(195,175,120)),(0.15,(160,135,80)),
         (0.4,(105,100,70)),(0.65,(80,75,65))],
        [(0.02,(195,175,120)),(0.15,(160,135,80)),(0.4,(105,100,70)),
         (0.65,(80,75,65)),(1.0,(230,230,240))]):
        m = (norm>=t0)&(norm<t1)
        t = np.where(m,(norm-t0)/max(t1-t0,1e-6),0.)
        for c in range(3):
            rgb[:,:,c] = np.where(m,np.clip(c0[c]+t*(c1[c]-c0[c]),0,255),
                                  rgb[:,:,c]).astype(np.uint8)
    for c in range(3):
        rgb[:,:,c] = np.clip(rgb[:,:,c]*shade,0,255).astype(np.uint8)
    _Im.fromarray(rgb,'RGB').save(out_path)
    print(f"[preview] {out_path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src',        default=KENSHI_FULLMAP)
    ap.add_argument('--out',        default='game/data/terrain/world_hmap.r32')
    ap.add_argument('--max-height', type=float, default=200.0,
                    help='Target max terrain height in metres')
    ap.add_argument('--preview',    action='store_true')
    args = ap.parse_args()

    root     = os.path.join(os.path.dirname(__file__), '..')
    out_path = os.path.join(root, args.out)

    if not os.path.exists(args.src):
        print(f"[import] ERROR: {args.src} not found")
        print("[import] Mount Windows drive first:")
        print("         sudo mount /dev/sdX1 /run/media/rdga1/win")
        return

    # Backup existing
    if os.path.exists(out_path):
        bak = out_path + '.bak'
        if not os.path.exists(bak):
            import shutil; shutil.copy2(out_path, bak)
            print(f"[import] backed up → {bak}")

    # 1. Load + normalise
    hmap_full = load_and_normalise(args.src, args.max_height)

    # 2. Extract per-zone tiles
    print("[import] extracting 64×64 zone tiles…")
    zones_h = np.zeros((ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS, ATLAS_VERTS),
                       dtype=np.float32)
    for cz in range(ATLAS_ZONES):
        for cx in range(ATLAS_ZONES):
            zones_h[cz, cx] = extract_zone(hmap_full, cz, cx)
        if cz % 16 == 0:
            print(f"[import]   row {cz}/{ATLAS_ZONES}…")

    del hmap_full   # free ~537 MB

    hmin = zones_h.min(); hmax = zones_h.max()
    print(f"[import] atlas height range: {hmin:.1f} – {hmax:.1f} m")

    # 3. Write
    write_atlas(zones_h, out_path)

    if args.preview:
        prev = out_path.replace('.r32', '_preview.png')
        write_preview(zones_h, prev)

    print("\n[done] Regenerate world map:")
    print("  python3 tools/md_worldmap_gen.py")
    print("  ninja -C build monkey_dust")


if __name__ == '__main__':
    main()
