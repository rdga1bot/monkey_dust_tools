#!/usr/bin/env python3
"""chunklod_spike.py — Phase 1 isolated spike, docs/TERRAIN_CHUNKLOD_PORT_PLAN.md.

Ports the real activation-level / error-bound algorithm from Thatcher
Ulrich's public-domain heightfield_chunker.cpp (tmp_/chunklod_reference/,
fetched verbatim from tu-testbed/SourceForge) to Python, and runs it
against one real Kenshi zone read from the project's own canonical
world_hmap.r16 atlas (via tools/md_hmap_io.py — NOT re-reading fullmap.tif,
per .claude/rules/asset-pipeline-paths.md: one canonical artifact
location).

Zero engine risk: standalone script, reads existing canonical data,
writes nothing back. Purpose is solely to verify the error-bound math
is correctly ported before trusting it on real terrain (gate 1 of the
port plan) -- not to produce a usable chunk file yet (that's Phase 2,
the C++ bake tool).

Algorithm reference (read directly, not paraphrased):
  tmp_/chunklod_reference/heightfield_chunker.cpp
    update()                     -- lines 750-782
    propagate_activation_level() -- lines 863-915
    heightfield::activate/get_level/set_level -- lines 386-424

Usage:
  python3 tools/research/chunklod_spike.py --test-flat
  python3 tools/research/chunklod_spike.py --zone ZX ZY [--max-error E]
"""
import sys, math, argparse
import numpy as np

sys.path.insert(0, "tools")
from md_hmap_io import ATLAS_ZONES, ATLAS_VERTS, load_atlas_tiled  # noqa: E402


class Heightfield:
    """Direct port of heightfield_chunker.cpp's `struct heightfield`.

    m_size must be (2**log_size)+1 -- ATLAS_VERTS=129 = 2**7+1, so one
    Kenshi zone maps exactly onto one Ulrich chunklod root tile with no
    padding/extension needed (a real, checked fit, not assumed).
    """

    def __init__(self, height_grid: np.ndarray, vertical_scale: float = 1.0):
        assert height_grid.ndim == 2 and height_grid.shape[0] == height_grid.shape[1]
        size = height_grid.shape[0]
        log_size = round(math.log2(size - 1))
        assert size == (1 << log_size) + 1, (
            f"heightfield size {size} is not (2^N)+1 -- got log_size={log_size}, "
            f"(1<<{log_size})+1={(1 << log_size) + 1}"
        )
        self.m_size = size
        self.m_log_size = log_size
        self.root_level = log_size - 1  # set by caller normally; default matches tree_depth=log_size
        self.vertical_scale = vertical_scale
        # heights stored directly in metres here (unlike the original's
        # quantized Sint16 store) -- this spike checks the *algorithm*,
        # not the quantization; quantization is a Phase 2 concern.
        self._h = height_grid.astype(np.float64)
        # activation level per vertex, -1 == unset (mirrors the original's
        # 0x0F sentinel-in-nibble packing, done here as a plain int8 array
        # since we don't need the original's mmap-array memory trick).
        self._level = np.full((size, size), -1, dtype=np.int8)
        self.activations = 0

    def height(self, x: int, z: int) -> float:
        return float(self._h[z, x])

    def get_level(self, x: int, z: int) -> int:
        return int(self._level[z, x])

    def set_level(self, x: int, z: int, lev: int):
        assert -1 <= lev < 15
        self._level[z, x] = lev

    def activate(self, x: int, z: int, lev: int):
        # Direct port of heightfield::activate() -- heightfield_chunker.cpp:412-424
        current = self.get_level(x, z)
        if lev > current:
            if current == -1:
                self.activations += 1
            self.set_level(x, z, lev)


def update(hf: Heightfield, base_max_error: float, ax, az, rx, rz, lx, lz):
    """Direct port of heightfield_chunker.cpp update() -- lines 750-782."""
    dx = lx - rx
    dz = lz - rz
    if abs(dx) <= 1 and abs(dz) <= 1:
        return  # base level reached

    bx = rx + (dx >> 1)
    bz = rz + (dz >> 1)

    error = abs(hf.height(bx, bz) - (hf.height(lx, lz) + hf.height(rx, rz)) / 2.0) * hf.vertical_scale
    assert error >= 0
    if error >= base_max_error:
        activation_level = math.floor(math.log2(error / base_max_error) + 0.5)
        hf.activate(bx, bz, activation_level)

    update(hf, base_max_error, bx, bz, ax, az, rx, rz)  # base, apex, right
    update(hf, base_max_error, bx, bz, lx, lz, ax, az)  # base, left, apex


def propagate_activation_level(hf: Heightfield, cx, cz, level, target_level):
    """Direct port of heightfield_chunker.cpp propagate_activation_level() -- lines 863-915."""
    half_size = 1 << level
    quarter_size = half_size >> 1

    if level > target_level:
        for j in range(2):
            for i in range(2):
                propagate_activation_level(
                    hf,
                    cx - quarter_size + half_size * i,
                    cz - quarter_size + half_size * j,
                    level - 1, target_level,
                )
        return

    if level > 0:
        lev = hf.get_level(cx + quarter_size, cz - quarter_size)  # ne
        hf.activate(cx + half_size, cz, lev)
        hf.activate(cx, cz - half_size, lev)

        lev = hf.get_level(cx - quarter_size, cz - quarter_size)  # nw
        hf.activate(cx, cz - half_size, lev)
        hf.activate(cx - half_size, cz, lev)

        lev = hf.get_level(cx - quarter_size, cz + quarter_size)  # sw
        hf.activate(cx - half_size, cz, lev)
        hf.activate(cx, cz + half_size, lev)

        lev = hf.get_level(cx + quarter_size, cz + quarter_size)  # se
        hf.activate(cx, cz + half_size, lev)
        hf.activate(cx + half_size, cz, lev)

    hf.activate(cx, cz, hf.get_level(cx + half_size, cz))
    hf.activate(cx, cz, hf.get_level(cx, cz - half_size))
    hf.activate(cx, cz, hf.get_level(cx, cz + half_size))
    hf.activate(cx, cz, hf.get_level(cx - half_size, cz))


def run_chunker(hf: Heightfield, base_max_error: float):
    """Mirrors heightfield_chunker()'s driver logic (lines 673-747), minus
    the mesh/.chu output -- just the error/activation computation."""
    size = hf.m_size
    update(hf, base_max_error, 0, size - 1, size - 1, size - 1, 0, 0)          # sw half
    update(hf, base_max_error, size - 1, 0, 0, 0, size - 1, size - 1)          # ne half

    for i in range(hf.m_log_size):
        propagate_activation_level(hf, size >> 1, size >> 1, hf.m_log_size - 1, i)
        propagate_activation_level(hf, size >> 1, size >> 1, hf.m_log_size - 1, i)


def level_histogram(hf: Heightfield) -> dict:
    levels, counts = np.unique(hf._level, return_counts=True)
    return dict(zip(levels.tolist(), counts.tolist()))


def test_flat(args):
    """Gate check: a perfectly flat heightfield should activate NOTHING
    above the coarsest level -- zero error means the coarsest possible
    mesh (2 triangles) already represents it exactly. If this fails, the
    port has a bug, full stop -- do not proceed to real terrain."""
    size = (1 << 7) + 1  # 129, matches one real Kenshi zone
    flat = np.full((size, size), 100.0, dtype=np.float32)
    hf = Heightfield(flat, vertical_scale=1.0)
    run_chunker(hf, base_max_error=args.max_error)

    hist = level_histogram(hf)
    activated = {k: v for k, v in hist.items() if k >= 0}
    total_activated = sum(activated.values())

    print(f"[test-flat] size={size} max_error={args.max_error}")
    print(f"[test-flat] level histogram: {hist}")
    print(f"[test-flat] total activated (level>=0): {total_activated}")

    if total_activated == 0:
        print("[test-flat] PASS -- flat plane activates zero vertices above coarsest level, as expected")
        return True
    else:
        print("[test-flat] FAIL -- flat plane should have zero error everywhere; port has a bug")
        return False


def test_zone(args):
    """Run the real algorithm against one real Kenshi zone (world_hmap.r16,
    already-converted canonical atlas -- see tools/tif_to_r32.py)."""
    print(f"[test-zone] loading world_hmap.r16 (zone {args.zone[0]},{args.zone[1]})...")
    atlas = load_atlas_tiled(args.hmap)  # (ATLAS_ZONES, ATLAS_ZONES, ATLAS_VERTS, ATLAS_VERTS), metres
    zx, zy = args.zone
    assert 0 <= zx < ATLAS_ZONES and 0 <= zy < ATLAS_ZONES, f"zone ({zx},{zy}) out of [0,{ATLAS_ZONES}) range"
    zone_h = atlas[zy, zx]  # (129, 129), metres
    print(f"[test-zone] zone height range: {zone_h.min():.1f}m .. {zone_h.max():.1f}m "
          f"(mean {zone_h.mean():.1f}m, std {zone_h.std():.1f}m)")

    hf = Heightfield(zone_h, vertical_scale=1.0)  # already in metres, vertical_scale=1
    run_chunker(hf, base_max_error=args.max_error)

    hist = level_histogram(hf)
    activated = {k: v for k, v in sorted(hist.items()) if k >= 0}
    unset = hist.get(-1, 0)
    total = hf.m_size * hf.m_size

    print(f"[test-zone] max_error={args.max_error}m")
    print(f"[test-zone] level histogram (level: vertex_count): {activated}")
    print(f"[test-zone] unset (level=-1, i.e. below coarsest level's need): {unset} / {total}")

    # Sanity checks -- these are the actual gate-1 criteria, not just "it ran".
    assert unset >= 0
    max_level_seen = max(activated.keys()) if activated else -1
    print(f"[test-zone] deepest activation level seen: {max_level_seen} "
          f"(tree needs at least {max_level_seen + 1} LOD levels to represent this zone "
          f"within {args.max_error}m error)")

    # Rough per-level cumulative vertex estimate (upper bound on chunk
    # resolution needed at that LOD -- real triangle-strip mesh generation
    # is Phase 2, this is just a sanity-check count).
    cumulative = 0
    for lev in sorted(activated.keys(), reverse=True):
        cumulative += activated[lev]
        print(f"[test-zone]   level {lev}: {activated[lev]} verts activated at this level, "
              f"{cumulative} cumulative at level<={lev}")

    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--test-flat", action="store_true", help="gate check: flat plane must activate zero verts")
    ap.add_argument("--zone", nargs=2, type=int, metavar=("ZX", "ZY"), help="run against one real Kenshi zone")
    ap.add_argument("--max-error", type=float, default=1.0, help="base_max_error in metres (default 1.0, matches Ulrich's own default)")
    ap.add_argument("--hmap", default="game/data/terrain/world_hmap.r16")
    args = ap.parse_args()

    if not args.test_flat and not args.zone:
        ap.error("specify --test-flat and/or --zone ZX ZY")

    ok = True
    if args.test_flat:
        ok = test_flat(args) and ok
    if args.zone:
        ok = test_zone(args) and ok

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
