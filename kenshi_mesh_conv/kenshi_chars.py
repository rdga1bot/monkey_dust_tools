#!/usr/bin/env python3
"""
Convert Kenshi character meshes to GLB props.
Kenshi uses decimetres (1 unit = 0.1 m). We scale vertex positions by 0.1
so the resulting GLB is in metres (1.95 m tall human, etc.)
"""
import struct, json, sys, os, math
sys.path.insert(0, os.path.dirname(__file__))
import ogre2glb as o

KENSHI = "/run/media/rdga1/win/SteamLibrary/steamapps/common/Kenshi/data"
OUT    = "game/data/props"

UNIT_SCALE = 0.1   # Kenshi decimetres → metres


def scale_verts(verts, s):
    return [(x * s, y * s, z * s) for x, y, z in verts]


def merge_parts(mesh_paths):
    """Merge multiple Ogre meshes into one vertex/normal/index list."""
    all_v, all_n, all_i = [], [], []
    base = 0
    for path in mesh_paths:
        if not os.path.exists(path):
            print(f"  MISSING: {path}")
            continue
        v, n, i = o.parse_ogre_mesh(path)
        if not v:
            continue
        # Scale from Kenshi units to metres
        all_v.extend(scale_verts(v, UNIT_SCALE))
        all_n.extend(n)
        all_i.extend(idx + base for idx in i)
        base += len(v)
    return all_v, all_n, all_i


def convert(name, mesh_paths, out_path):
    print(f"  {name} ...", end='', flush=True)
    v, n, i = merge_parts(mesh_paths)
    if not v or not i:
        print("  SKIP (empty)")
        return False
    assert len(i) % 3 == 0, f"index count {len(i)} not divisible by 3"
    v = o.ground_verts(v)   # Y_min → 0 so characters stand on terrain
    nv, nt = o.write_glb(v, n, i, out_path)
    # Print bbox for sanity
    ys = [p[1] for p in v]
    print(f"  {nv}v {nt}t  height={max(ys)-min(ys):.2f}m  OK")
    return True


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)

    H = f"{KENSHI}/character/meshes/human"

    targets = [
        # Full human body (torso + merged limbs) → npc_human
        ("kenshi_human", [
            f"{H}/human_male.mesh",
            f"{H}/human_left_arm.mesh",
            f"{H}/human_right_arm.mesh",
            f"{H}/human_left_leg.mesh",
            f"{H}/human_right_leg.mesh",
        ], f"{OUT}/kenshi_human.glb"),

        # Body only (player distinct colour in code, same mesh)
        ("kenshi_human_f", [
            f"{H}/human_female.mesh",
            f"{H}/human_left_arm.mesh",
            f"{H}/human_right_arm.mesh",
            f"{H}/human_left_leg.mesh",
            f"{H}/human_right_leg.mesh",
        ], f"{OUT}/kenshi_human_f.glb"),

        # Robot skeleton (bandits, enemies)
        ("kenshi_skeleton", [
            f"{KENSHI}/character/meshes/bone/bone_male.mesh",
        ], f"{OUT}/kenshi_skeleton.glb"),
    ]

    ok = 0
    for name, paths, dst in targets:
        if convert(name, paths, dst):
            ok += 1
    print(f"\nConverted {ok}/{len(targets)}")
