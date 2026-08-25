#!/usr/bin/env python3
"""chunklod_mesh_spike.py — Phase 2 spike, docs/TERRAIN_CHUNKLOD_PORT_PLAN.md.

Extends chunklod_spike.py's verified error-bound port with the actual
mesh-generation half of Ulrich's algorithm: generate_block()/
generate_quadrant() (Lindstrom et al. SIGGRAPH '96 triangle-strip walk)
and generate_edge_data() (crack-avoidance skirts, MAXIMUM_ALLOWED_
NEIGHBOR_DIFFERENCE=2). Ported directly from
tmp_/chunklod_reference/heightfield_chunker.cpp lines 1037-1377,
re-read from disk immediately before this port (not from memory).

Unlike the real bake tool (a later phase), this script skips the 16-bit
quantized binary .chu output -- it keeps vertex positions as plain
world-space floats and dumps real, inspectable OBJ meshes plus a
matplotlib 3D render, because the Phase 2 gate is "visually sane
geometry", not "correct binary format" (that's Phase 2's C++ port,
gated on this Python version being visually verified first).

Usage:
  python3 tools/research/chunklod_mesh_spike.py --zone 23 34 --max-error 2.0
"""
import sys, math, argparse, os
import numpy as np

sys.path.insert(0, "tools")
sys.path.insert(0, "tools/research")
from md_hmap_io import ATLAS_ZONES, ATLAS_VERTS, load_atlas_tiled  # noqa: E402
from chunklod_spike import Heightfield, update, propagate_activation_level  # noqa: E402

MAXIMUM_ALLOWED_NEIGHBOR_DIFFERENCE = 2  # heightfield_chunker.cpp:1319 -- must match the runtime's can_split() bound


def lowest_one(x: int) -> int:
    """Direct port of heightfield_chunker.cpp lowest_one() -- lines 279-291."""
    if x == 0:
        return 32
    i = 0
    while (x & 1) == 0:
        x >>= 1
        i += 1
    return i


def node_index(hf: Heightfield, x: int, z: int) -> int:
    """Direct port of heightfield::node_index() -- lines 426-450."""
    if x < 0 or x >= hf.m_size or z < 0 or z >= hf.m_size:
        return -1
    l1 = lowest_one(x | z)
    depth = hf.m_log_size - l1 - 1
    base = 0x55555555 & ((1 << (depth * 2)) - 1)
    shift = l1 + 1
    col = x >> shift
    row = z >> shift
    return base + (row << depth) + col


def minimum_edge_lod(hf: Heightfield, coord: int) -> int:
    """Direct port of heightfield::minimum_edge_lod() -- lines 453-469."""
    l1 = lowest_one(coord)
    depth = hf.m_log_size - l1 - 1
    return int(np.clip(hf.root_level - depth, 0, hf.root_level))


def height_query(hf: Heightfield, level, x, z, ax, az, rx, rz, lx, lz):
    """Direct port of height_query() -- lines 788-846."""
    if (x == ax and z == az) or (x == rx and z == rz) or (x == lx and z == lz):
        return hf.height(x, z)

    dx = lx - rx
    dz = lz - rz
    if abs(dx) <= 1 and abs(dz) <= 1:
        return hf.height(ax, az)  # error condition in the original; shouldn't hit in practice

    bx = rx + (dx >> 1)
    bz = rz + (dz >> 1)

    edge_length_squared = (dx * dx + dz * dz) / 2.0
    sr = ((x - ax) * (rx - ax) + (z - az) * (rz - az)) / edge_length_squared
    sl = ((x - ax) * (lx - ax) + (z - az) * (lz - az)) / edge_length_squared

    base_vert_level = hf.get_level(bx, bz)
    if base_vert_level >= level:
        if sr >= sl:
            return height_query(hf, level, x, z, bx, bz, ax, az, rx, rz)
        else:
            return height_query(hf, level, x, z, bx, bz, lx, lz, ax, az)

    ay = hf.height(ax, az)
    dr = hf.height(rx, rz) - ay
    dl = hf.height(lx, lz) - ay
    return math.floor(ay + sl * dl + sr * dr + 0.5)


def get_height_at_LOD(hf: Heightfield, level, x, z):
    """Direct port of get_height_at_LOD() -- lines 849-860."""
    size = hf.m_size
    if z > x:
        return height_query(hf, level, x, z, 0, size - 1, size - 1, size - 1, 0, 0)
    else:
        return height_query(hf, level, x, z, size - 1, 0, 0, 0, size - 1, size - 1)


class Mesh:
    """Direct port of the `mesh` namespace -- lines 1381-1695.

    Keeps real world-space-scale (x,z in heightfield units, y in metres)
    positions rather than the original's 16-bit quantized compressed
    format -- quantization is a binary-.chu-format concern (a later
    phase), not a geometry-correctness concern (this phase's gate).
    """

    def __init__(self):
        self.vertices = []       # list of (x, y, z, special)
        self.vertex_indices = []  # triangle-strip indices
        self._index_table = {}   # (x,z) -> index, mirrors lookup_index()/get_vertex_index()

    def get_vertex_index(self, hf: Heightfield, x, z):
        key = (x, z)
        if key in self._index_table:
            return self._index_table[key]
        idx = len(self.vertices)
        self.vertices.append((x, hf.height(x, z), z, False))
        self._index_table[key] = idx
        return idx

    def special_vertex_index(self, x, y, z):
        idx = len(self.vertices)
        self.vertices.append((x, y, z, True))
        return idx

    def emit_vertex(self, hf: Heightfield, x, z):
        idx = self.get_vertex_index(hf, x, z)
        self.vertex_indices.append(idx)

    def emit_special_vertex(self, x, y, z):
        idx = self.special_vertex_index(x, y, z)
        self.vertex_indices.append(idx)

    def emit_previous_vertex(self):
        assert len(self.vertex_indices) > 0
        self.vertex_indices.append(self.vertex_indices[-1])

    def get_index_count(self):
        return len(self.vertex_indices)


class GenState:
    """Direct port of `struct gen_state` -- lines 1117-1136."""

    def __init__(self, activation_level):
        self.my_buffer = [[-1, -1], [-1, -1]]
        self.activation_level = activation_level
        self.ptr = 0
        self.previous_level = 0

    def in_my_buffer(self, x, z):
        return (x, z) == tuple(self.my_buffer[0]) or (x, z) == tuple(self.my_buffer[1])

    def set_my_buffer(self, x, z):
        self.my_buffer[self.ptr] = [x, z]


def generate_quadrant(hf: Heightfield, mesh: Mesh, s: GenState, lx, lz, tx, tz, rx, rz, recursion_level):
    """Direct port of generate_quadrant() -- lines 1199-1228."""
    if recursion_level <= 0:
        return

    if hf.get_level(tx, tz) >= s.activation_level:
        bx = (lx + rx) >> 1
        bz = (lz + rz) >> 1

        generate_quadrant(hf, mesh, s, lx, lz, bx, bz, tx, tz, recursion_level - 1)

        if not s.in_my_buffer(tx, tz):
            if (recursion_level + s.previous_level) & 1:
                s.ptr ^= 1
            else:
                x, z = s.my_buffer[1 - s.ptr]
                mesh.emit_vertex(hf, x, z)
            mesh.emit_vertex(hf, tx, tz)
            s.set_my_buffer(tx, tz)
            s.previous_level = recursion_level

        generate_quadrant(hf, mesh, s, tx, tz, bx, bz, rx, rz, recursion_level - 1)


def generate_block(hf: Heightfield, mesh: Mesh, activation_level, log_size, cx, cz):
    """Direct port of generate_block() -- lines 1139-1196."""
    hs = 1 << (log_size - 1)
    q = [
        (cx + hs, cz + hs),  # se
        (cx + hs, cz - hs),  # ne
        (cx - hs, cz - hs),  # nw
        (cx - hs, cz + hs),  # sw
    ]

    s = GenState(activation_level)
    mesh.emit_vertex(hf, *q[0])
    s.set_my_buffer(*q[0])

    for i in range(4):
        if (s.previous_level & 1) == 0:
            s.ptr ^= 1
        else:
            x, z = s.my_buffer[1 - s.ptr]
            mesh.emit_vertex(hf, x, z)

        mesh.emit_vertex(hf, *q[i])
        s.set_my_buffer(*q[i])
        s.previous_level = 2 * log_size + 1

        generate_quadrant(hf, mesh, s, q[i][0], q[i][1], cx, cz, q[(i + 1) & 3][0], q[(i + 1) & 3][1], 2 * log_size)

    if not s.in_my_buffer(*q[0]):
        mesh.emit_vertex(hf, *q[0])


def generate_edge_data(hf: Heightfield, mesh: Mesh, direction, x0, z0, x1, z1, level):
    """Direct port of generate_edge_data() -- lines 1231-1377."""
    if x0 < x1:
        assert z0 == z1
        dx, dz, steps = 1, 0, x1 - x0 + 1
    elif x0 > x1:
        assert z0 == z1
        dx, dz, steps = -1, 0, x0 - x1 + 1
    elif z0 < z1:
        assert x0 == x1
        dx, dz, steps = 0, 1, z1 - z0 + 1
    elif z0 > z1:
        assert x0 == x1
        dx, dz, steps = 0, -1, z0 - z1 + 1
    else:
        assert False

    vert_minimums = []
    current_min = hf.height(x0, z0)
    x, z = x0, z0
    for i in range(steps):
        current_min = min(current_min, hf.height(x, z))

        if hf.get_level(x, z) >= level:
            major_coord = x0 if dz == 0 else z0
            min_edge_lod = minimum_edge_lod(hf, major_coord)

            level_diff = min(min_edge_lod + 1, hf.root_level) - level
            level_diff = min(level_diff, MAXIMUM_ALLOWED_NEIGHBOR_DIFFERENCE)

            for lod in range(level, level + level_diff + 1):
                lod_height = get_height_at_LOD(hf, lod, x, z)
                current_min = min(current_min, lod_height)

            if current_min > -32768:
                current_min -= 1
            vert_minimums.append(current_min)
            current_min = hf.height(x, z)

        x += dx
        z += dz

    mesh.emit_previous_vertex()
    if (mesh.get_index_count() & 1) == 0:
        mesh.emit_previous_vertex()

    vert_index = 0
    x, z = x0, z0
    for i in range(steps):
        if hf.get_level(x, z) >= level:
            min_height = vert_minimums[vert_index]
            if len(vert_minimums) > vert_index + 1:
                min_height = min(min_height, vert_minimums[vert_index + 1])

            mesh.emit_vertex(hf, x, z)
            if i == 0:
                mesh.emit_previous_vertex()
            mesh.emit_special_vertex(x, min_height, z)

            vert_index += 1
        x += dx
        z += dz


def generate_node_mesh(hf: Heightfield, x0, z0, log_size, level) -> Mesh:
    """Direct port of the mesh-generation portion of generate_node_data()
    -- lines 1037-1113 -- for a SINGLE node (no recursion to children;
    the caller picks which node(s) to generate, unlike the original which
    always bakes the whole tree)."""
    size = 1 << log_size
    half_size = size >> 1
    cx = x0 + half_size
    cz = z0 + half_size

    # Make sure corner verts are activated on this level (heightfield_chunker.cpp:1078-1082).
    hf.activate(x0 + size, z0, level)
    hf.activate(x0, z0, level)
    hf.activate(x0, z0 + size, level)
    hf.activate(x0 + size, z0 + size, level)

    mesh = Mesh()
    generate_block(hf, mesh, level, log_size, cx, cz)

    generate_edge_data(hf, mesh, 0, cx + half_size, cz + half_size, cx + half_size, cz - half_size, level)  # east
    generate_edge_data(hf, mesh, 1, cx + half_size, cz - half_size, cx - half_size, cz - half_size, level)  # north
    generate_edge_data(hf, mesh, 2, cx - half_size, cz - half_size, cx - half_size, cz + half_size, level)  # west
    generate_edge_data(hf, mesh, 3, cx - half_size, cz + half_size, cx + half_size, cz + half_size, level)  # south

    return mesh


def strip_to_triangles(mesh: Mesh):
    """Convert the triangle-strip index list into real triangles, dropping
    degenerates (matches the original's own real-triangle-count logic,
    heightfield_chunker.cpp:1589-1601)."""
    tris = []
    idx = mesh.vertex_indices
    for i in range(len(idx) - 2):
        a, b, c = idx[i], idx[i + 1], idx[i + 2]
        if a == b or b == c or a == c:
            continue  # degenerate
        if i % 2 == 0:
            tris.append((a, b, c))
        else:
            tris.append((a, c, b))  # alternate winding for strip parity
    return tris


def write_obj(path, mesh: Mesh, tris, sample_spacing=1.8, y_offset=0.0):
    with open(path, "w") as f:
        f.write("# chunklod_mesh_spike.py -- Phase 2 gate dump\n")
        for (x, y, z, special) in mesh.vertices:
            f.write(f"v {x * sample_spacing:.3f} {y + y_offset:.3f} {z * sample_spacing:.3f}\n")
        for (a, b, c) in tris:
            f.write(f"f {a + 1} {b + 1} {c + 1}\n")  # OBJ is 1-indexed


def render_png(path, mesh: Mesh, tris, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection

    # IMPORTANT axis mapping: matplotlib's (X,Y,Z) = (world_x, world_z,
    # world_height) -- NOT (world_x, world_height, world_z). This makes
    # matplotlib's own "top-down" view (elev=90, which looks straight
    # down its Z axis) a genuine top-down view of the real XZ footprint.
    # Getting this backwards (as an earlier version of this script did)
    # makes elev=90 look down the world-Z axis instead -- a north/south
    # side view mislabeled "top-down", which nearly caused a real bug
    # (degenerate strip winding) to be missed behind a rendering mistake.
    verts = np.array([(x, z, y) for (x, y, z, sp) in mesh.vertices])
    fig = plt.figure(figsize=(8, 8))
    ax = fig.add_subplot(111, projection="3d")

    faces = [[verts[a], verts[b], verts[c]] for (a, b, c) in tris]
    heights = np.array([np.mean([verts[a][2], verts[b][2], verts[c][2]]) for (a, b, c) in tris])
    if len(heights) > 0:
        norm = (heights - heights.min()) / max(1e-6, (heights.max() - heights.min()))
        colors = plt.cm.terrain(norm)
    else:
        colors = "gray"

    x_range = max(verts[:, 0].max() - verts[:, 0].min(), 1)
    y_range = max(verts[:, 1].max() - verts[:, 1].min(), 1)
    z_range = max(verts[:, 2].max() - verts[:, 2].min(), 1)

    def _new_axes(elev, azim, exaggerate):
        f = plt.figure(figsize=(8, 8))
        a = f.add_subplot(111, projection="3d")
        a.add_collection3d(Poly3DCollection(faces, facecolor=colors, edgecolor="black", linewidths=0.2))
        a.set_xlim(verts[:, 0].min(), verts[:, 0].max())
        a.set_ylim(verts[:, 1].min(), verts[:, 1].max())
        a.set_zlim(verts[:, 2].min() - 5, verts[:, 2].max() + 5)
        a.set_box_aspect((x_range, y_range, z_range * exaggerate))
        a.set_xlabel("world x"); a.set_ylabel("world z"); a.set_zlabel("height")
        a.view_init(elev=elev, azim=azim)
        return f, a

    fig, ax = _new_axes(elev=35, azim=-60, exaggerate=3)
    ax.set_title(f"{title}\n{len(mesh.vertices)} verts, {len(tris)} real tris")
    plt.tight_layout()
    plt.savefig(path, dpi=120)
    plt.close(fig)

    # Genuine top-down (elev=90 now correctly looks straight down the
    # height axis onto the real XZ footprint) -- the real sanity view:
    # a coarse/root mesh MUST still cover the full square footprint, a
    # sliver here means a real winding/strip bug, not a viewing artifact.
    fig2, ax2 = _new_axes(elev=90, azim=-90, exaggerate=1)
    ax2.set_title(f"{title} (true top-down)\nfootprint should be the full square, not a sliver")
    plt.tight_layout()
    topdown_path = path.replace(".png", "_topdown.png")
    plt.savefig(topdown_path, dpi=120)
    plt.close(fig2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zone", nargs=2, type=int, required=True, metavar=("ZX", "ZY"))
    ap.add_argument("--max-error", type=float, default=2.0)
    ap.add_argument("--hmap", default="game/data/terrain/world_hmap.r16")
    ap.add_argument("--outdir", default="/tmp/claude-1001/-home-rdga1-rdga1prj-monkeydust/e9c60870-ac26-475f-9e9d-84b930cbfe9f/scratchpad/chunklod_mesh")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    print(f"[mesh-spike] loading zone {args.zone[0]},{args.zone[1]} (max_error={args.max_error}m)...")
    atlas = load_atlas_tiled(args.hmap)
    zx, zy = args.zone
    zone_h = atlas[zy, zx]

    hf = Heightfield(zone_h, vertical_scale=1.0)
    hf.root_level = hf.m_log_size - 1  # matches heightfield_chunker() driver: hf.root_level = tree_depth-1, tree_depth=log_size

    size = hf.m_size
    update(hf, args.max_error, 0, size - 1, size - 1, size - 1, 0, 0)
    update(hf, args.max_error, size - 1, 0, 0, 0, size - 1, size - 1)
    for i in range(hf.m_log_size):
        propagate_activation_level(hf, size >> 1, size >> 1, hf.m_log_size - 1, i)
        propagate_activation_level(hf, size >> 1, size >> 1, hf.m_log_size - 1, i)

    # Case A: root node, coarsest level (level = root_level) -- the whole
    # zone as ONE block, minimum detail. Sanity: should be small (near 2
    # triangles) since the tree only refines where error demands it.
    root_log_size = hf.m_log_size
    root_level = hf.root_level
    mesh_root = generate_node_mesh(hf, 0, 0, root_log_size, root_level)
    tris_root = strip_to_triangles(mesh_root)
    print(f"[mesh-spike] ROOT node (level={root_level}, whole zone): "
          f"{len(mesh_root.vertices)} verts, {mesh_root.get_index_count()} strip indices, "
          f"{len(tris_root)} real triangles")

    # Case B: one NW-quadrant child at level-1, half the zone, finer detail.
    half = 1 << (root_log_size - 1)
    mesh_child = generate_node_mesh(hf, 0, 0, root_log_size - 1, root_level - 1)
    tris_child = strip_to_triangles(mesh_child)
    print(f"[mesh-spike] CHILD node (level={root_level - 1}, NW quadrant, half-size {half}): "
          f"{len(mesh_child.vertices)} verts, {mesh_child.get_index_count()} strip indices, "
          f"{len(tris_child)} real triangles")

    # Case C: whole zone at level=0 (finest -- essentially unsimplified,
    # every original heightfield vertex active). This is the real,
    # unambiguous LOD-variation demonstration: level 0 must produce
    # dramatically MORE triangles than the coarsest level for the same
    # footprint, since level 0 forces every vertex active regardless of
    # error. (Case A vs B above was inconclusive by itself -- both
    # landed on the base 2-triangle case because too few vertices in
    # that particular half-zone reached the requested activation level
    # at max_error=2.0; this directly forces the finest level instead.)
    mesh_finest = generate_node_mesh(hf, 0, 0, root_log_size, 0)
    tris_finest = strip_to_triangles(mesh_finest)
    print(f"[mesh-spike] FINEST node (level=0, whole zone, forced max detail): "
          f"{len(mesh_finest.vertices)} verts, {mesh_finest.get_index_count()} strip indices, "
          f"{len(tris_finest)} real triangles")

    # Sanity checks (the actual Phase 2 gate criteria).
    assert len(tris_root) >= 2, "root block must have at least the 2 base triangles"
    assert len(tris_child) >= len(tris_root) // 4, \
        "a half-size, one-level-finer child should not have drastically FEWER triangles than 1/4 of the root's share"
    assert len(tris_finest) > len(tris_root) * 10, \
        "level=0 (finest) must be dramatically more detailed than the coarsest level for the same zone -- if not, the LOD/activation-level gating in generate_quadrant is broken"
    expected_full_res_tris = 2 * (hf.m_size - 1) * (hf.m_size - 1)  # 2 tris per grid cell at full 129x129 resolution
    print(f"[mesh-spike] full-resolution reference: {expected_full_res_tris} triangles "
          f"(2 per {hf.m_size - 1}x{hf.m_size - 1} grid cell) -- finest mesh has {len(tris_finest)}, "
          f"ratio {len(tris_finest) / expected_full_res_tris:.2%}")

    # Skirt sanity: every 'special' vertex (skirt) must be strictly at or
    # below its corresponding real surface vertex at the same (x,z) --
    # skirts hang down, they never poke up through the surface.
    real_by_xz = {}
    for (x, y, z, special) in mesh_root.vertices:
        if not special:
            real_by_xz.setdefault((x, z), []).append(y)
    skirt_violations = 0
    for (x, y, z, special) in mesh_root.vertices:
        if special:
            surf = real_by_xz.get((x, z))
            if surf is not None and y > min(surf):
                skirt_violations += 1
    print(f"[mesh-spike] skirt sanity: {skirt_violations} skirt vertices found ABOVE their surface vertex "
          f"(must be 0 -- skirts hang down by construction)")
    assert skirt_violations == 0, "skirt vertex poking above the surface -- real bug, do not proceed"

    for name, mesh, tris in [("root", mesh_root, tris_root), ("child_nw", mesh_child, tris_child), ("finest", mesh_finest, tris_finest)]:
        obj_path = os.path.join(args.outdir, f"zone{zx}_{zy}_{name}.obj")
        png_path = os.path.join(args.outdir, f"zone{zx}_{zy}_{name}.png")
        write_obj(obj_path, mesh, tris)
        render_png(png_path, mesh, tris, f"zone ({zx},{zy}) {name}")
        print(f"[mesh-spike] wrote {obj_path} and {png_path}")

    print("[mesh-spike] PASS -- all sanity checks passed, see PNG renders for visual confirmation")


if __name__ == "__main__":
    main()
