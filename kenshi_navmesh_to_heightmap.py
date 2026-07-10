#!/usr/bin/env python3
"""Merge Kenshi's real navmesh geometry (tmp_/kenshi_navmesh_obj/zone_X_Z.obj,
produced by kenshi_navmesh_extract) into world_hmap.r32.

Why: world_hmap.r32 currently comes entirely from fullmap.tif, a flat 16385x16385
uint16 heightmap subsampled to 129x129/zone (tools/tif_to_r32.py). The Havok AI
navmesh Kenshi actually shipped with carries real per-zone walkable-surface
geometry at a different (finer, irregular) resolution -- a second, independent
ground truth we can cross-reference/refine against.

Axis convention (verified numerically against world_hmap.r32, not assumed):
  navmesh vertex X -> grid column, navmesh vertex Z -> grid row, no flips,
  zone(gx,gz) obj <-> world_hmap.r32 zone index (zx=gx, zy=gz) directly.
  (See conversation notes: 6-point spot check across 2 well-matching zones,
  max residual ~2.7m -- consistent with navmesh/TIF using different underlying
  triangulations, not an axis error.)

Per-zone acceptance: the navmesh only covers WALKABLE surface -- water, cliffs,
and out-of-bounds areas are absent, and a handful of zones only contain a
degenerate flat placeholder quad (not real terrain; confirmed via 6-zone
spot-check: zone(0,0)/(10,10)/(5,40) all extract to an exact flat Y regardless
of the existing heightmap's real (non-flat) range there). Reject a zone's
navmesh data outright if its height range doesn't overlap the existing
heightmap zone's range (within ZONE_TOLERANCE_M) -- keep the existing
fullmap.tif-derived heights for that whole zone instead of introducing a
false, disconnected reading.

Per-vertex within an accepted zone: only grid points actually covered by a
navmesh triangle are replaced; everything else (holes -- water, unwalkable
slopes) keeps the existing fullmap.tif-derived height.

Output is written to a NEW file (does not overwrite world_hmap.r32) --
promotion to production is a separate, explicit step after visual review.

Usage:
  python3 tools/kenshi_navmesh_to_heightmap.py
  python3 tools/kenshi_navmesh_to_heightmap.py --hmap <path> --obj-dir <path> --out <path>
"""
import struct, argparse, glob, os, re, time
import numpy as np

ATLAS_MAGIC = 0x414D4800
ATLAS_ZONES = 64
ATLAS_VERTS = 129
CHUNK_SIZE_M = 460.8
ZONE_TOLERANCE_M = 10.0   # overlap slack when deciding whether to trust a zone's navmesh


def load_hmap(path):
    with open(path, "rb") as f:
        magic, nzx, nzy, verts = struct.unpack("<IIII", f.read(16))
        assert magic == ATLAS_MAGIC and verts == ATLAS_VERTS, "unexpected world_hmap.r32 header"
        grids = np.empty((nzy, nzx, verts, verts), dtype=np.float32)
        for zy in range(nzy):
            for zx in range(nzx):
                f.read(8)  # stored hmin/hmax -- recomputed on write
                grids[zy, zx] = np.frombuffer(f.read(verts * verts * 4), dtype=np.float32).reshape(verts, verts)
    return nzx, nzy, verts, grids


def write_hmap(path, nzx, nzy, verts, grids):
    with open(path, "wb") as f:
        f.write(struct.pack("<IIII", ATLAS_MAGIC, nzx, nzy, verts))
        for zy in range(nzy):
            for zx in range(nzx):
                g = grids[zy, zx]
                f.write(struct.pack("<ff", float(g.min()), float(g.max())))
                f.write(g.tobytes())


def parse_obj(path):
    verts = []
    faces = []
    with open(path) as f:
        for line in f:
            if line.startswith("v "):
                _, x, y, z = line.split()
                verts.append((float(x), float(y), float(z)))
            elif line.startswith("f "):
                faces.append([int(t) - 1 for t in line.split()[1:]])
    return np.array(verts, dtype=np.float64), faces


def rasterize_zone(verts, faces, n):
    """Return (height_grid[n,n], covered_mask[n,n]) in local grid-index space."""
    height = np.zeros((n, n), dtype=np.float64)
    covered = np.zeros((n, n), dtype=bool)
    if len(verts) == 0:
        return height, covered

    scale = (n - 1) / CHUNK_SIZE_M
    gx_all = verts[:, 0] * scale
    gz_all = verts[:, 2] * scale
    gy_all = verts[:, 1]

    for face in faces:
        for i in range(1, len(face) - 1):
            ia, ib, ic = face[0], face[i], face[i + 1]
            ax, az, ay = gx_all[ia], gz_all[ia], gy_all[ia]
            bx, bz, by = gx_all[ib], gz_all[ib], gy_all[ib]
            cx, cz, cy = gx_all[ic], gz_all[ic], gy_all[ic]

            lo_c = max(int(np.floor(min(ax, bx, cx))), 0)
            hi_c = min(int(np.ceil(max(ax, bx, cx))), n - 1)
            lo_r = max(int(np.floor(min(az, bz, cz))), 0)
            hi_r = min(int(np.ceil(max(az, bz, cz))), n - 1)
            if lo_c > hi_c or lo_r > hi_r:
                continue

            cols = np.arange(lo_c, hi_c + 1)
            rows = np.arange(lo_r, hi_r + 1)
            px, pz = np.meshgrid(cols.astype(np.float64), rows.astype(np.float64))

            v0x, v0z = bx - ax, bz - az
            v1x, v1z = cx - ax, cz - az
            v2x, v2z = px - ax, pz - az
            d00 = v0x * v0x + v0z * v0z
            d01 = v0x * v1x + v0z * v1z
            d11 = v1x * v1x + v1z * v1z
            d20 = v2x * v0x + v2z * v0z
            d21 = v2x * v1x + v2z * v1z
            denom = d00 * d11 - d01 * d01
            if abs(denom) < 1e-9:
                continue
            v = (d11 * d20 - d01 * d21) / denom
            w = (d00 * d21 - d01 * d20) / denom
            u = 1.0 - v - w
            inside = (u >= -1e-6) & (v >= -1e-6) & (w >= -1e-6)
            if not inside.any():
                continue
            h = u * ay + v * by + w * cy
            rr = rows[:, None] * np.ones((1, len(cols)), dtype=int)
            cc = np.ones((len(rows), 1), dtype=int) * cols[None, :]
            height[rr[inside], cc[inside]] = h[inside]
            covered[rr[inside], cc[inside]] = True

    return height, covered


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hmap", default="game/data/terrain/world_hmap.r32")
    ap.add_argument("--obj-dir", default="tmp_/kenshi_navmesh_obj")
    ap.add_argument("--out", default="game/data/terrain/world_hmap_navmesh_merged.r32")
    args = ap.parse_args()

    t0 = time.time()
    print(f"[navmesh_to_hmap] loading {args.hmap} ...")
    nzx, nzy, verts, grids = load_hmap(args.hmap)
    print(f"  {nzx}x{nzy} zones, {verts}x{verts} verts/zone")

    obj_files = sorted(glob.glob(os.path.join(args.obj_dir, "zone_*_*.obj")))
    print(f"  found {len(obj_files)} navmesh OBJ files")

    n_replaced = 0
    n_rejected = 0
    n_no_obj = nzx * nzy - len(obj_files)
    covered_fraction_sum = 0.0
    delta_abs_sum = 0.0
    delta_abs_count = 0
    rejected_list = []

    pat = re.compile(r"zone_(\d+)_(\d+)\.obj$")
    for path in obj_files:
        m = pat.search(path)
        if not m:
            continue
        gx, gz = int(m.group(1)), int(m.group(2))
        if gx >= nzx or gz >= nzy:
            continue

        mesh_verts, faces = parse_obj(path)
        if len(mesh_verts) == 0:
            n_no_obj += 1
            continue

        mesh_min, mesh_max = float(mesh_verts[:, 1].min()), float(mesh_verts[:, 1].max())
        existing = grids[gz, gx]
        hmap_min, hmap_max = float(existing.min()), float(existing.max())
        mesh_span = mesh_max - mesh_min
        hmap_span = hmap_max - hmap_min

        # monkey_dust: 1022/3995 zones extract to a perfectly flat mesh, and
        # 1016 of those to an exact Y=10.0 constant -- a Havok-side placeholder
        # for zones with no real walkable navmesh (water/unused), not actual
        # terrain. Of those, 84 sit on top of a heightmap zone that DOES have
        # real relief (span>5m) -- an unambiguous placeholder mismatch, reject
        # regardless of the overlap check below (a flat placeholder can
        # spuriously overlap a wide real range by chance).
        if mesh_span < 1.0 and hmap_span > 5.0:
            n_rejected += 1
            rejected_list.append((gx, gz, mesh_min, mesh_max, hmap_min, hmap_max))
            continue

        overlap = min(mesh_max, hmap_max) - max(mesh_min, hmap_min)
        if overlap < -ZONE_TOLERANCE_M:
            n_rejected += 1
            rejected_list.append((gx, gz, mesh_min, mesh_max, hmap_min, hmap_max))
            continue

        height, covered = rasterize_zone(mesh_verts, faces, verts)
        if not covered.any():
            n_rejected += 1
            continue

        old_vals = existing[covered]
        new_vals = height[covered]
        delta_abs_sum += float(np.abs(new_vals - old_vals).sum())
        delta_abs_count += int(covered.sum())
        covered_fraction_sum += float(covered.mean())

        merged = np.where(covered, height, existing)
        grids[gz, gx] = merged.astype(np.float32)
        n_replaced += 1

    print(f"[navmesh_to_hmap] writing {args.out} ...")
    write_hmap(args.out, nzx, nzy, verts, grids)

    elapsed = time.time() - t0
    print(f"\n=== Summary ({elapsed:.1f}s) ===")
    print(f"  zones replaced (navmesh accepted): {n_replaced}")
    print(f"  zones rejected (range mismatch / no coverage): {n_rejected}")
    print(f"  zones with no navmesh OBJ at all: {n_no_obj}")
    if n_replaced:
        print(f"  avg covered fraction per replaced zone: {covered_fraction_sum/n_replaced*100:.1f}%")
    if delta_abs_count:
        print(f"  avg |old-new| height delta at covered points: {delta_abs_sum/delta_abs_count:.3f}m")
    if rejected_list:
        print(f"\n  first 10 rejected zones (gx,gz,mesh_min,mesh_max,hmap_min,hmap_max):")
        for row in rejected_list[:10]:
            print("   ", row)


if __name__ == "__main__":
    main()
