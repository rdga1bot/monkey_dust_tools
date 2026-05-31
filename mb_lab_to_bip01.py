#!/usr/bin/env python3
"""
mb_lab_to_bip01.py  —  Convert MB-Lab / CharMorph / Rigify GLB → monkey_dust Bip01 GLB

What this does:
  1. Rename Rigify/MB-Lab bones → Bip01 names  (JSON only)
  2. Reorder skin.joints to match BIP01_ORDER
  3. Update JOINTS_0 vertex attribute  (in-place binary remap)
  4. Rebuild inverseBindMatrices in Bip01 order (appended to binary)
  5. Add identity nodes for missing Bip01 bones (Prop1, HeadNub, etc.)

Usage:
  python3 mb_lab_to_bip01.py --list input.glb          # show bone names + proposed mapping
  python3 mb_lab_to_bip01.py input.glb output.glb      # convert
"""

import struct, json, sys, copy
from pathlib import Path

# ── Bone name map: source (MB-Lab/Rigify) → target (Bip01) ──────────────────
# Run with --list first to see what names YOUR export actually has.
# Edit entries here to match — different MB-Lab versions use different names.
BONE_MAP = {
    # Root
    "root":                  "Bip01",
    "Root":                  "Bip01",
    "DEF-root":              "Bip01",
    "master":                "Bip01",

    # Pelvis
    "DEF-spine":             "Bip01 Pelvis",
    "pelvis":                "Bip01 Pelvis",
    "Pelvis":                "Bip01 Pelvis",
    "hips":                  "Bip01 Pelvis",
    "Hips":                  "Bip01 Pelvis",
    "torso":                 "Bip01 Pelvis",

    # Spine
    "DEF-spine.001":         "Bip01 Spine",
    "spine":                 "Bip01 Spine",
    "Spine":                 "Bip01 Spine",
    "spine_01":              "Bip01 Spine",
    "spine_lower":           "Bip01 Spine",
    "DEF-spine.002":         "Bip01 Spine1",
    "spine.001":             "Bip01 Spine1",
    "Spine1":                "Bip01 Spine1",
    "spine_02":              "Bip01 Spine1",
    "spine_mid":             "Bip01 Spine1",
    "DEF-spine.003":         "Bip01 Spine2",
    "spine.002":             "Bip01 Spine2",
    "Spine2":                "Bip01 Spine2",
    "spine_03":              "Bip01 Spine2",
    "chest":                 "Bip01 Spine2",
    "spine_upper":           "Bip01 Spine2",

    # Neck / Head / Jaw
    "DEF-neck":              "Bip01 Neck",
    "neck":                  "Bip01 Neck",
    "Neck":                  "Bip01 Neck",
    "DEF-head":              "Bip01 Head",
    "head":                  "Bip01 Head",
    "Head":                  "Bip01 Head",
    "DEF-jaw":               "Bip01 Jaw",
    "jaw":                   "Bip01 Jaw",
    "jaw_master":            "Bip01 Jaw",
    "jaw_M":                 "Bip01 Jaw",

    # Left arm
    "DEF-shoulder.L":        "Bip01 L Clavicle",
    "shoulder.L":            "Bip01 L Clavicle",
    "clavicle_L":            "Bip01 L Clavicle",
    "shoulder_L":            "Bip01 L Clavicle",
    "LeftShoulder":          "Bip01 L Clavicle",
    "DEF-upper_arm.L":       "Bip01 L UpperArm",
    "upper_arm.L":           "Bip01 L UpperArm",
    "upper_arm_L":           "Bip01 L UpperArm",
    "upperarm_L":            "Bip01 L UpperArm",
    "LeftArm":               "Bip01 L UpperArm",
    "DEF-upper_arm.L.001":   "Bip01 L UpperArm",
    "DEF-forearm.L":         "Bip01 L Forearm",
    "forearm.L":             "Bip01 L Forearm",
    "forearm_L":             "Bip01 L Forearm",
    "LeftForeArm":           "Bip01 L Forearm",
    "DEF-forearm.L.001":     "Bip01 L Forearm",
    "DEF-hand.L":            "Bip01 L Hand",
    "hand.L":                "Bip01 L Hand",
    "hand_L":                "Bip01 L Hand",
    "LeftHand":              "Bip01 L Hand",

    # Right arm
    "DEF-shoulder.R":        "Bip01 R Clavicle",
    "shoulder.R":            "Bip01 R Clavicle",
    "clavicle_R":            "Bip01 R Clavicle",
    "shoulder_R":            "Bip01 R Clavicle",
    "RightShoulder":         "Bip01 R Clavicle",
    "DEF-upper_arm.R":       "Bip01 R UpperArm",
    "upper_arm.R":           "Bip01 R UpperArm",
    "upper_arm_R":           "Bip01 R UpperArm",
    "upperarm_R":            "Bip01 R UpperArm",
    "RightArm":              "Bip01 R UpperArm",
    "DEF-upper_arm.R.001":   "Bip01 R UpperArm",
    "DEF-forearm.R":         "Bip01 R Forearm",
    "forearm.R":             "Bip01 R Forearm",
    "forearm_R":             "Bip01 R Forearm",
    "RightForeArm":          "Bip01 R Forearm",
    "DEF-forearm.R.001":     "Bip01 R Forearm",
    "DEF-hand.R":            "Bip01 R Hand",
    "hand.R":                "Bip01 R Hand",
    "hand_R":                "Bip01 R Hand",
    "RightHand":             "Bip01 R Hand",

    # Left leg
    "DEF-thigh.L":           "Bip01 L Thigh",
    "thigh.L":               "Bip01 L Thigh",
    "thigh_L":               "Bip01 L Thigh",
    "upleg_L":               "Bip01 L Thigh",
    "LeftUpLeg":             "Bip01 L Thigh",
    "DEF-thigh.L.001":       "Bip01 L Thigh",
    "DEF-shin.L":            "Bip01 L Calf",
    "shin.L":                "Bip01 L Calf",
    "shin_L":                "Bip01 L Calf",
    "leg_L":                 "Bip01 L Calf",
    "LeftLeg":               "Bip01 L Calf",
    "DEF-shin.L.001":        "Bip01 L Calf",
    "DEF-foot.L":            "Bip01 L Foot",
    "foot.L":                "Bip01 L Foot",
    "foot_L":                "Bip01 L Foot",
    "LeftFoot":              "Bip01 L Foot",
    "DEF-toe.L":             "Bip01 L Toe0",
    "toe.L":                 "Bip01 L Toe0",
    "toe_L":                 "Bip01 L Toe0",
    "LeftToeBase":           "Bip01 L Toe0",

    # Right leg
    "DEF-thigh.R":           "Bip01 R Thigh",
    "thigh.R":               "Bip01 R Thigh",
    "thigh_R":               "Bip01 R Thigh",
    "upleg_R":               "Bip01 R Thigh",
    "RightUpLeg":            "Bip01 R Thigh",
    "DEF-thigh.R.001":       "Bip01 R Thigh",
    "DEF-shin.R":            "Bip01 R Calf",
    "shin.R":                "Bip01 R Calf",
    "shin_R":                "Bip01 R Calf",
    "leg_R":                 "Bip01 R Calf",
    "RightLeg":              "Bip01 R Calf",
    "DEF-shin.R.001":        "Bip01 R Calf",
    "DEF-foot.R":            "Bip01 R Foot",
    "foot.R":                "Bip01 R Foot",
    "foot_R":                "Bip01 R Foot",
    "RightFoot":             "Bip01 R Foot",
    "DEF-toe.R":             "Bip01 R Toe0",
    "toe.R":                 "Bip01 R Toe0",
    "toe_R":                 "Bip01 R Toe0",
    "RightToeBase":          "Bip01 R Toe0",
}

# Target bone order — must match monkey_dust skin.joints indices
BIP01_ORDER = [
    "Bip01",            # 0
    "Bip01 Pelvis",     # 1
    "Bip01 L Thigh",    # 2
    "Bip01 L Calf",     # 3
    "Bip01 L Foot",     # 4
    "Bip01 L Toe0",     # 5
    "Bip01 L Toe0Nub",  # 6  placeholder
    "Bip01 R Thigh",    # 7
    "Bip01 R Calf",     # 8
    "Bip01 R Foot",     # 9
    "Bip01 R Toe0",     # 10
    "Bip01 R Toe0Nub",  # 11 placeholder
    "Bip01 Spine",      # 12
    "Bip01 Spine1",     # 13
    "Bip01 Spine2",     # 14
    "Bip01 L Clavicle", # 15
    "Bip01 L UpperArm", # 16
    "Bip01 L Forearm",  # 17
    "Bip01 L Hand",     # 18
    "Bip01 Prop1",      # 19 placeholder
    "Bip01 Neck",       # 20
    "Bip01 Head",       # 21
    "Bip01 HeadNub",    # 22 placeholder
    "Bip01 Jaw",        # 23
    "Bip01 JawNub",     # 24 placeholder
    "Bip01 R Clavicle", # 25
    "Bip01 R UpperArm", # 26
    "Bip01 R Forearm",  # 27
    "Bip01 R Hand",     # 28
    "Bip01 Prop2",      # 29 placeholder
]

# ── GLB I/O ──────────────────────────────────────────────────────────────────

def read_glb(path):
    with open(path, 'rb') as f:
        magic, version, _ = struct.unpack('<III', f.read(12))
        assert magic == 0x46546C67, "Not a GLB file"
        json_len, json_type = struct.unpack('<II', f.read(8))
        assert json_type == 0x4E4F534A, "Expected JSON chunk"
        gltf = json.loads(f.read(json_len))
        bin_data = bytearray()
        chunk = f.read()
        if len(chunk) >= 8:
            bin_len, bin_type = struct.unpack('<II', chunk[:8])
            assert bin_type == 0x004E4942, "Expected BIN chunk"
            bin_data = bytearray(chunk[8:8 + bin_len])
    return gltf, bin_data

def write_glb(path, gltf, bin_data):
    jb = json.dumps(gltf, separators=(',', ':')).encode('utf-8')
    jb += b' ' * ((4 - len(jb) % 4) % 4)
    bb = bytes(bin_data) + b'\x00' * ((4 - len(bin_data) % 4) % 4)
    total = 12 + 8 + len(jb) + (8 + len(bb) if bb else 0)
    with open(path, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, total))
        f.write(struct.pack('<II', len(jb), 0x4E4F534A))
        f.write(jb)
        if bb:
            f.write(struct.pack('<II', len(bb), 0x004E4942))
            f.write(bb)

# ── Bone helpers ─────────────────────────────────────────────────────────────

def get_skin_bone_names(gltf):
    skin = gltf['skins'][0]
    return [gltf['nodes'][ni].get('name', f'node_{ni}') for ni in skin['joints']]

def apply_bone_map(name):
    return BONE_MAP.get(name, name)

# ── JOINTS_0 binary update ───────────────────────────────────────────────────

COMP_FMT  = {5121: ('B', 1), 5123: ('H', 2)}

def remap_joints(gltf, bin_data, mesh_prim, old_to_new):
    """Remap JOINTS_0 indices in-place. old_to_new: dict[int→int]."""
    attrs = mesh_prim.get('attributes', {})
    if 'JOINTS_0' not in attrs:
        return
    acc_idx = attrs['JOINTS_0']
    acc  = gltf['accessors'][acc_idx]
    bv   = gltf['bufferViews'][acc['bufferView']]
    ct   = acc['componentType']
    fmt, sz = COMP_FMT[ct]
    count   = acc['count']
    base    = bv.get('byteOffset', 0) + acc.get('byteOffset', 0)
    stride  = bv.get('byteStride', sz * 4)
    for i in range(count):
        off = base + i * stride
        j0,j1,j2,j3 = struct.unpack_from(f'<4{fmt}', bin_data, off)
        j0 = old_to_new.get(j0, j0)
        j1 = old_to_new.get(j1, j1)
        j2 = old_to_new.get(j2, j2)
        j3 = old_to_new.get(j3, j3)
        struct.pack_into(f'<4{fmt}', bin_data, off, j0, j1, j2, j3)

# ── inverseBindMatrices rebuild ──────────────────────────────────────────────

IDENTITY_MAT4 = (
    1.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0,
)

def read_inv_bind(gltf, bin_data, acc_idx):
    acc = gltf['accessors'][acc_idx]
    bv  = gltf['bufferViews'][acc['bufferView']]
    base = bv.get('byteOffset', 0) + acc.get('byteOffset', 0)
    return [struct.unpack_from('<16f', bin_data, base + i*64) for i in range(acc['count'])]

def append_inv_bind(gltf, bin_data, matrices):
    """Append new inverseBindMatrices at end of binary, return new accessor index."""
    # New bufferView
    bv_offset = len(bin_data)
    raw = b''.join(struct.pack('<16f', *m) for m in matrices)
    bin_data += bytearray(raw)

    bv_idx = len(gltf['bufferViews'])
    gltf['bufferViews'].append({
        'buffer': 0,
        'byteOffset': bv_offset,
        'byteLength': len(raw),
    })
    acc_idx = len(gltf['accessors'])
    gltf['accessors'].append({
        'bufferView': bv_idx,
        'byteOffset': 0,
        'componentType': 5126,   # FLOAT
        'count': len(matrices),
        'type': 'MAT4',
    })
    # Update buffer byteLength
    gltf['buffers'][0]['byteLength'] = len(bin_data)
    return acc_idx

# ── Placeholder node creation ────────────────────────────────────────────────

def make_placeholder_node(name):
    """Identity-transform node used for bones with no MB-Lab equivalent."""
    return {
        'name': name,
        'translation': [0.0, 0.0, 0.0],
        'rotation':    [0.0, 0.0, 0.0, 1.0],
        'scale':       [1.0, 1.0, 1.0],
    }

# ── Main conversion ──────────────────────────────────────────────────────────

def convert(gltf, bin_data, verbose=True):
    gltf = copy.deepcopy(gltf)

    # ── Step 1: rename nodes that are in a skin ──────────────────────────────
    skin = gltf['skins'][0]
    name_to_node_idx = {}     # bip01_name → node index (after rename)
    unmapped = []

    for ni in skin['joints']:
        node = gltf['nodes'][ni]
        orig = node.get('name', '')
        mapped = apply_bone_map(orig)
        if mapped != orig:
            if verbose:
                print(f"  rename  {orig:40s} → {mapped}")
            node['name'] = mapped
        elif mapped in BIP01_ORDER:
            pass  # already a Bip01 name
        else:
            unmapped.append(orig)
        if mapped in BIP01_ORDER:
            name_to_node_idx[mapped] = ni

    if unmapped and verbose:
        print(f"\n  WARNING: {len(unmapped)} unmapped bone(s): {unmapped[:5]}{'...' if len(unmapped)>5 else ''}")
        print("  Add entries to BONE_MAP for these, then re-run.\n")

    # ── Step 2: add placeholder nodes for missing Bip01 bones ───────────────
    for bname in BIP01_ORDER:
        if bname not in name_to_node_idx:
            ni = len(gltf['nodes'])
            gltf['nodes'].append(make_placeholder_node(bname))
            name_to_node_idx[bname] = ni
            if verbose:
                print(f"  placeholder  {bname}")

    # ── Step 3: read old inverseBindMatrices ─────────────────────────────────
    old_joints   = skin['joints']           # list of node indices (old order)
    old_ib_idx   = skin.get('inverseBindMatrices')
    old_mats     = {}  # node_idx → 4x4 matrix
    if old_ib_idx is not None:
        raw_mats = read_inv_bind(gltf, bin_data, old_ib_idx)
        for i, ni in enumerate(old_joints):
            old_mats[ni] = raw_mats[i]

    # ── Step 4: build new skin.joints in BIP01_ORDER ─────────────────────────
    new_joint_node_idxs = [name_to_node_idx[b] for b in BIP01_ORDER]

    # old joint position → new joint position (for JOINTS_0 remap)
    old_pos_to_new = {}
    old_node_to_old_pos = {ni: i for i, ni in enumerate(old_joints)}
    for new_pos, ni in enumerate(new_joint_node_idxs):
        old_pos = old_node_to_old_pos.get(ni)
        if old_pos is not None:
            old_pos_to_new[old_pos] = new_pos
        # unmapped bones default to index 0 in JOINTS_0 (Bip01 root = no deform)

    # ── Step 5: remap JOINTS_0 for every mesh primitive ─────────────────────
    for mesh in gltf.get('meshes', []):
        for prim in mesh.get('primitives', []):
            remap_joints(gltf, bin_data, prim, old_pos_to_new)

    # ── Step 6: rebuild inverseBindMatrices in new order ────────────────────
    new_mats = []
    for ni in new_joint_node_idxs:
        new_mats.append(old_mats.get(ni, IDENTITY_MAT4))
    new_ib_idx = append_inv_bind(gltf, bin_data, new_mats)

    # ── Step 7: update skin ──────────────────────────────────────────────────
    skin['joints']               = new_joint_node_idxs
    skin['inverseBindMatrices']  = new_ib_idx
    if 'skeleton' in skin:
        skin['skeleton'] = name_to_node_idx.get("Bip01", new_joint_node_idxs[0])

    if verbose:
        print(f"\n  Skin now has {len(new_joint_node_idxs)} joints in Bip01 order.")

    return gltf, bin_data

# ── CLI ──────────────────────────────────────────────────────────────────────

def cmd_list(path):
    gltf, _ = read_glb(path)
    if not gltf.get('skins'):
        print("No skins found."); return
    skin = gltf['skins'][0]
    print(f"Bones in {Path(path).name} ({len(skin['joints'])} total):\n")
    print(f"  {'idx':>4}  {'current name':40s}  proposed mapping")
    print("  " + "-"*75)
    for i, ni in enumerate(skin['joints']):
        name   = gltf['nodes'][ni].get('name', f'<node {ni}>')
        mapped = apply_bone_map(name)
        status = "✓" if mapped in BIP01_ORDER else "?"
        if mapped == name:
            mapped_str = f"(unchanged — {'in order' if mapped in BIP01_ORDER else 'ADD TO BONE_MAP'})"
        else:
            mapped_str = f"→  {mapped}"
        print(f"  {i:>4}  {name:40s}  {status} {mapped_str}")

def main():
    args = sys.argv[1:]
    if '--list' in args:
        args.remove('--list')
        if not args:
            print("Usage: mb_lab_to_bip01.py --list input.glb"); sys.exit(1)
        cmd_list(args[0]); return

    if len(args) < 2:
        print(__doc__); sys.exit(1)

    src, dst = args[0], args[1]
    print(f"Converting {src} → {dst}")
    gltf, bin_data = read_glb(src)

    if not gltf.get('skins'):
        print("ERROR: no skins (rigged character) found in file."); sys.exit(1)
    if not gltf.get('meshes'):
        print("ERROR: no meshes found."); sys.exit(1)

    gltf, bin_data = convert(gltf, bin_data, verbose=True)
    write_glb(dst, gltf, bin_data)
    print(f"\nDone: {dst}")

if __name__ == '__main__':
    main()
