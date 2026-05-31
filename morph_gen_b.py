#!/usr/bin/env python3
"""
morph_gen_b.py — Path B: generate procedural morph targets for md_human.glb
by computing per-vertex deltas from bone weight patterns.

No artist, no Blender required — all deltas are computed mathematically
from the existing joint weights baked into the GLB.

Output: game/data/props/md_human_morphs.glb  (same mesh + morph targets)
        game/data/chars/morph_names.txt       (one name per line)

Morphs generated:
  0  tall      — scale Y proportional to spine chain weight
  1  fat       — push verts outward along normal × belly/torso weight
  2  muscular  — push outward × arm/shoulder weight (less belly)
  3  longlegs  — scale Y of leg vertices
  4  bighead   — scale head bone-weighted verts by 1.15
  5  broadshdr — widen shoulder verts along X

Usage:
  python3 tools/morph_gen_b.py [glb_path]
  # default: game/data/props/md_human.glb
"""
import struct, json, os, sys, math, copy

GLB_IN    = sys.argv[1] if len(sys.argv) > 1 else "game/data/props/md_human.glb"
GLB_OUT   = GLB_IN   # overwrite in-place: md_human.glb = mesh + face morphs + body morphs
NAMES_OUT = "game/data/chars/morph_names.txt"

MORPH_NAMES = ["tall", "fat", "muscular", "longlegs", "bighead", "broadshdr"]

# md_human.glb joint indices (CLAUDE.md canonical list):
#   0=Bip01  1=Pelvis
#   2=L Thigh  3=L Calf  4=L Foot  5=L Toe0  6=L Toe0Nub
#   7=R Thigh  8=R Calf  9=R Foot 10=R Toe0 11=R Toe0Nub
#  12=Spine  13=Spine1  14=Spine2
#  15=L Clavicle 16=L UpperArm 17=L Forearm 18=L Hand  19=Prop1
#  20=Neck   21=Head    22=HeadNub 23=Jaw    24=JawNub
#  25=R Clavicle 26=R UpperArm 27=R Forearm 28=R Hand  29=Prop2
_SPINE  = frozenset({0, 1, 12, 13, 14})
_BELLY  = frozenset({1, 12, 13})
_ARM    = frozenset({15, 16, 17, 18, 25, 26, 27, 28})
_CLAV   = frozenset({15, 25})
_HEAD   = frozenset({20, 21, 22, 23, 24})
_LEG    = frozenset({2, 3, 4, 5, 6, 7, 8, 9, 10, 11})

# ── GLB reader (minimal — only what we need) ──────────────────────────────────
def read_glb(path):
    with open(path, 'rb') as f:
        magic, ver, total = struct.unpack('<III', f.read(12))
        assert magic == 0x46546C67, "Not a GLB file"
        # JSON chunk
        jlen, jtype = struct.unpack('<II', f.read(8))
        json_bytes = f.read(jlen)
        gltf = json.loads(json_bytes.decode('utf-8'))
        # BIN chunk
        blen, btype = struct.unpack('<II', f.read(8))
        bin_data = bytearray(f.read(blen))
    return gltf, bin_data


def write_glb(path, gltf, bin_data):
    json_bytes = json.dumps(gltf, separators=(',', ':')).encode('utf-8')
    # pad JSON to 4-byte alignment
    while len(json_bytes) % 4:
        json_bytes += b' '
    # pad BIN to 4-byte alignment
    while len(bin_data) % 4:
        bin_data += b'\x00'
    total = 12 + 8 + len(json_bytes) + 8 + len(bin_data)
    with open(path, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, total))
        f.write(struct.pack('<II', len(json_bytes), 0x4E4F534A))  # JSON
        f.write(json_bytes)
        f.write(struct.pack('<II', len(bin_data), 0x004E4942))    # BIN
        f.write(bin_data)


def accessor_data(gltf, bin_data, acc_idx):
    """Return list of tuples for the given accessor."""
    acc  = gltf['accessors'][acc_idx]
    bv   = gltf['bufferViews'][acc['bufferView']]
    off  = bv.get('byteOffset', 0) + acc.get('byteOffset', 0)
    cnt  = acc['count']
    comp_type = acc['componentType']   # 5126=float, 5123=u16, 5121=u8
    atype     = acc['type']            # SCALAR VEC2 VEC3 VEC4 MAT4

    comp_sizes = {5126: 4, 5123: 2, 5121: 1, 5125: 4, 5120: 1, 5122: 2}
    fmt_chars  = {5126: 'f', 5123: 'H', 5121: 'B', 5125: 'I', 5120: 'b', 5122: 'h'}
    dims       = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}

    dim  = dims[atype]
    cs   = comp_sizes[comp_type]
    fc   = fmt_chars[comp_type]
    stride = bv.get('byteStride', dim * cs)
    result = []
    for i in range(cnt):
        base = off + i * stride
        vals = struct.unpack_from(f'<{dim}{fc}', bin_data, base)
        result.append(vals if dim > 1 else vals[0])
    return result


def append_accessor(gltf, bin_data, data_list, component_type, atype):
    """Append data to bin_data and create accessor + bufferView. Returns acc idx."""
    dims = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}
    comp_sizes = {5126: 4, 5123: 2, 5121: 1}
    fmt_chars  = {5126: 'f', 5123: 'H', 5121: 'B'}
    dim  = dims[atype]
    cs   = comp_sizes[component_type]
    fc   = fmt_chars[component_type]

    # align to 4 bytes
    while len(bin_data) % 4:
        bin_data += b'\x00'
    byte_offset = len(bin_data)

    raw = bytearray()
    for item in data_list:
        vals = item if hasattr(item, '__len__') else (item,)
        raw += struct.pack(f'<{dim}{fc}', *vals)
    bin_data += raw

    bv_idx = len(gltf.get('bufferViews', []))
    gltf.setdefault('bufferViews', []).append({
        'buffer': 0,
        'byteOffset': byte_offset,
        'byteLength': len(raw)
    })
    acc_idx = len(gltf.get('accessors', []))
    entry = {
        'bufferView': bv_idx,
        'componentType': component_type,
        'count': len(data_list),
        'type': atype
    }
    if atype != 'SCALAR' or component_type == 5126:
        # compute min/max for position-like VEC3
        if atype == 'VEC3' and component_type == 5126:
            mn = [min(v[i] for v in data_list) for i in range(3)]
            mx = [max(v[i] for v in data_list) for i in range(3)]
            entry['min'] = mn
            entry['max'] = mx
    gltf.setdefault('accessors', []).append(entry)
    return acc_idx


# ── Morph delta computation ───────────────────────────────────────────────────
def _bw(j, w, bone_set):
    total = 0.0
    for k in range(4):
        if j[k] in bone_set: total += w[k]
    return total

def compute_deltas(positions, normals, joints, weights, morph_idx):
    n = len(positions)
    deltas = [(0.0, 0.0, 0.0)] * n
    SCALE = 0.3

    for i in range(n):
        p  = positions[i]
        nm = normals[i]
        j  = joints[i]
        w  = weights[i]

        if morph_idx == 0:   # tall — elongate spine-weighted verts along Y
            sw = _bw(j, w, _SPINE)
            deltas[i] = (0.0, p[1] * sw * 0.18, 0.0)

        elif morph_idx == 1:  # fat — push belly/torso outward along normal
            bw = _bw(j, w, _BELLY) + _bw(j, w, _SPINE) * 0.4
            mag = bw * SCALE
            deltas[i] = (nm[0]*mag, nm[1]*mag*0.3, nm[2]*mag)

        elif morph_idx == 2:  # muscular — arm/shoulder bulge outward
            aw = _bw(j, w, _ARM)
            mag = aw * SCALE * 0.7
            deltas[i] = (nm[0]*mag, nm[1]*mag, nm[2]*mag)

        elif morph_idx == 3:  # longlegs — stretch leg verts along Y
            lw = _bw(j, w, _LEG)
            deltas[i] = (0.0, p[1] * lw * 0.22, 0.0)

        elif morph_idx == 4:  # bighead — expand head verts along normal
            hw = _bw(j, w, _HEAD)
            mag = hw * SCALE * 0.6
            deltas[i] = (nm[0]*mag, nm[1]*mag, nm[2]*mag)

        elif morph_idx == 5:  # broadshdr — widen clavicle/arm region along X
            # clavicle verts get full push; arm verts get half
            cv = _bw(j, w, _CLAV)
            av = _bw(j, w, _ARM) * 0.5
            sign = 1.0 if p[0] > 0 else -1.0
            deltas[i] = (sign * (cv + av) * SCALE * 0.5, 0.0, 0.0)

    return deltas


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    if not os.path.exists(GLB_IN):
        print(f"[B] GLB not found: {GLB_IN}")
        return

    print(f"[B] Loading {GLB_IN} ...")
    gltf, bin_data = read_glb(GLB_IN)
    bin_data = bytearray(bin_data)

    # Find the skinned mesh primitive
    mesh = gltf['meshes'][0]
    prim = mesh['primitives'][0]
    attrs = prim['attributes']

    positions = accessor_data(gltf, bin_data, attrs['POSITION'])
    normals   = accessor_data(gltf, bin_data, attrs['NORMAL'])
    joints    = accessor_data(gltf, bin_data, attrs['JOINTS_0'])
    weights_a = accessor_data(gltf, bin_data, attrs['WEIGHTS_0'])

    # Normalise weights to [0,1]
    weights_n = []
    for w in weights_a:
        s = sum(w)
        weights_n.append(tuple(x/s if s > 0 else 0.0 for x in w))

    n_verts = len(positions)
    print(f"[B] {n_verts} vertices, generating {len(MORPH_NAMES)} morphs ...")

    # Collect existing face morph targets + names (from md_convert.py face pipeline)
    existing_targets = prim.get('targets', [])
    existing_names   = (prim.get('extras') or {}).get('targetNames', []) or \
                       (mesh.get('extras') or {}).get('targetNames', [])
    # Drop any stale body morphs (re-generate fresh)
    keep_n = len(existing_names)
    keep_t = existing_targets[:keep_n]
    for name in MORPH_NAMES:
        if name in existing_names:
            idx = existing_names.index(name)
            keep_n = idx
            keep_t = existing_targets[:idx]
            existing_names = existing_names[:idx]
            break

    new_targets = []
    for mi, name in enumerate(MORPH_NAMES):
        deltas  = compute_deltas(positions, normals, joints, weights_n, mi)
        pos_acc = append_accessor(gltf, bin_data, deltas, 5126, 'VEC3')
        new_targets.append({'POSITION': pos_acc})
        nz = sum(1 for d in deltas if any(abs(v) > 1e-6 for v in d))
        print(f"  [{mi}] {name:12s}  non-zero verts: {nz}")

    all_targets = keep_t + new_targets
    all_names   = list(existing_names) + MORPH_NAMES

    prim['targets'] = all_targets
    prim.setdefault('extras', {})['targetNames'] = all_names
    mesh['weights'] = [0.0] * len(all_targets)
    mesh.setdefault('extras', {})['targetNames'] = all_names
    print(f"[B] Total morphs: {len(all_names)} ({len(existing_names)} face + {len(MORPH_NAMES)} body)")

    # Update buffer byteLength
    gltf['buffers'][0]['byteLength'] = len(bin_data)

    os.makedirs("game/data/chars", exist_ok=True)
    write_glb(GLB_OUT, gltf, bin_data)
    print(f"[B] Wrote {GLB_OUT}")

    with open(NAMES_OUT, 'w') as f:
        for name in MORPH_NAMES:
            f.write(name + '\n')
    print(f"[B] Wrote {NAMES_OUT}")
    print("[B] Pipeline: md_convert.py → md_human.glb (face morphs) → morph_gen_b.py → md_human.glb (face + body morphs)")


if __name__ == '__main__':
    main()
