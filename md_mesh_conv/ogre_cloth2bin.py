#!/usr/bin/env python3
"""ogre_cloth2bin.py — Convert Kenshi Ogre clothing .mesh → .clothbin
Format (binary, little-endian):
  magic  u32 = 0x544F4C43 ('CLOT')
  nv     u32
  ni     u32
  verts  nv × SkinVertex (stride=52):
    px,py,pz   float32×3 = 12 bytes
    nx,ny,nz   float32×3 = 12 bytes
    u,v        float32×2 =  8 bytes  (0.0 when UV absent)
    j0..j3     uint8×4   =  4 bytes  (bone indices, max 4)
    w0..w3     float32×4 = 16 bytes  (bone weights, normalised)
  idxs   ni × uint16

Usage:
  python3 ogre_cloth2bin.py INPUT.mesh OUTPUT.clothbin
  python3 ogre_cloth2bin.py --batch KENSHI_ITEMS_DIR OUT_DIR ITEMS...
"""
import struct, sys, os, math

VES_POSITION = 1
VES_NORMAL   = 4
VES_TEXCOORD = 7

M_MESH                        = 0x3000
M_SUBMESH                     = 0x4000
M_SUBMESH_OPERATION           = 0x4010
M_SUBMESH_BONE_ASSIGN         = 0x4100
M_GEOMETRY                    = 0x5000
M_GEOMETRY_VERTEX_DECLARATION = 0x5100
M_GEOMETRY_VERTEX_ELEMENT     = 0x5110
M_GEOMETRY_VERTEX_BUFFER      = 0x5200
M_GEOMETRY_VERTEX_BUFFER_DATA = 0x5210
MAGIC = 0x544F4C43  # 'CLOT'

def read_null_string(data, off):
    end = data.index(b'\x00', off)
    return data[off:end].decode('utf-8', errors='replace'), end + 1

def parse_geometry(data, off, end):
    """Returns (verts, norms, uvs) — all lists of tuples. uvs may be all zeros."""
    if off + 4 > end:
        return [], [], []
    nv = struct.unpack_from('<I', data, off)[0]; off += 4
    elements = []
    buffers  = {}

    while off + 6 <= end:
        cid, csz = struct.unpack_from('<HI', data, off)
        if csz < 6 or off+csz > end: break
        cd = data[off+6:off+csz]; off += csz

        if cid == M_GEOMETRY_VERTEX_DECLARATION:
            doff = 0
            while doff + 6 <= len(cd):
                dcid, dcsz = struct.unpack_from('<HI', cd, doff)
                if dcsz < 6: break
                if dcid == M_GEOMETRY_VERTEX_ELEMENT:
                    src, vtype, sem, voff, _ = struct.unpack_from('<5H', cd, doff+6)
                    elements.append((src, vtype, sem, voff))
                doff += dcsz
        elif cid == M_GEOMETRY_VERTEX_ELEMENT:
            src, vtype, sem, voff, _ = struct.unpack_from('<5H', cd)
            elements.append((src, vtype, sem, voff))
        elif cid == M_GEOMETRY_VERTEX_BUFFER:
            bind_idx, vstride = struct.unpack_from('<HH', cd)
            sub = 4
            while sub + 6 <= len(cd):
                scid, scsz = struct.unpack_from('<HI', cd, sub)
                if scsz < 6: break
                if scid == M_GEOMETRY_VERTEX_BUFFER_DATA:
                    buffers[bind_idx] = (cd[sub+6:sub+scsz], vstride)
                sub += scsz

    def extract(sem_id, fmt, n_comp):
        elem = next((e for e in elements if e[2] == sem_id), None)
        if not elem or elem[0] not in buffers:
            return None
        buf, stride = buffers[elem[0]]
        base = elem[3]
        result = []
        for i in range(nv):
            vals = struct.unpack_from(f'<{n_comp}f', buf, i*stride+base)
            result.append(vals)
        return result

    verts = extract(VES_POSITION, 'f', 3) or [(0,0,0)]*nv
    norms = extract(VES_NORMAL,   'f', 3) or [(0,1,0)]*nv
    uvs   = extract(VES_TEXCOORD, 'f', 2) or [(0,0)]*nv
    return verts, norms, uvs

def parse_submesh(data, off, end):
    """Returns (verts, norms, uvs, bone_assignments, indices).
    bone_assignments: list of lists, one per vertex, each [(bone_idx, weight), ...]"""
    _, off = read_null_string(data, off)
    if off + 5 > end:
        return [], [], [], [], []
    num_idx = struct.unpack_from('<I', data, off)[0]; off += 4
    idx32   = struct.unpack_from('<?', data, off)[0]; off += 1
    idx_bytes = num_idx * (4 if idx32 else 2)
    fmt = f'<{num_idx}{"I" if idx32 else "H"}'
    raw_idx = list(struct.unpack_from(fmt, data, off)); off += idx_bytes

    verts, norms, uvs = [], [], []
    bone_assign = {}  # {vert_idx: [(bone_idx, weight), ...]}

    while off + 6 <= end:
        cid, csz = struct.unpack_from('<HI', data, off)
        if csz < 6 or off+csz > end: break
        cd_off = off+6; cd_end = off+csz; off = cd_end

        if cid == M_GEOMETRY:
            verts, norms, uvs = parse_geometry(data, cd_off, cd_end)
        elif cid == M_SUBMESH_BONE_ASSIGN:
            # One assignment per chunk: vert_idx(u32) bone_idx(u16) weight(f32)
            if cd_end - cd_off >= 10:
                vi, bi, w = struct.unpack_from('<IHf', data, cd_off)
                bone_assign.setdefault(vi, []).append((bi, w))

    # Build per-vertex bone list (top-4 by weight, normalised)
    nv = len(verts)
    joints4  = [(0,0,0,0)] * nv
    weights4 = [(0,0,0,0)] * nv
    for vi in range(nv):
        assigns = sorted(bone_assign.get(vi, []), key=lambda x: -x[1])[:4]
        bx = [a[0] for a in assigns] + [0]*(4-len(assigns))
        wx = [a[1] for a in assigns] + [0.0]*(4-len(assigns))
        total = sum(wx)
        if total > 1e-6:
            wx = [w/total for w in wx]
        joints4[vi]  = tuple(bx[:4])
        weights4[vi] = tuple(wx[:4])

    return verts, norms, uvs, joints4, weights4, raw_idx

def convert_mesh(src_path):
    """Parse Ogre .mesh → (verts, norms, uvs, joints4, weights4, indices)."""
    data = open(src_path, 'rb').read()
    if struct.unpack_from('<H', data, 0)[0] != 0x1000:
        raise ValueError(f"Not an Ogre mesh: {src_path}")
    nl = data.index(b'\n', 2); off = nl + 1

    cid, csz = struct.unpack_from('<HI', data, off)
    if cid != M_MESH:
        raise ValueError("No M_MESH chunk")
    mesh_end = off+csz; off += 6

    all_v, all_n, all_u, all_j, all_w, all_i = [], [], [], [], [], []
    base = 0

    sub_off = off + 1  # skip skeletallyAnimated byte
    while sub_off + 6 <= mesh_end:
        scid, scsz = struct.unpack_from('<HI', data, sub_off)
        if scsz < 6 or sub_off+scsz > mesh_end: break
        s_cd = sub_off+6; s_end = sub_off+scsz; sub_off = s_end

        if scid == M_SUBMESH:
            sv, sn, su, sj, sw, si = parse_submesh(data, s_cd, s_end)
            all_i.extend(idx + base for idx in si)
            all_v.extend(sv); all_n.extend(sn); all_u.extend(su)
            all_j.extend(sj); all_w.extend(sw)
            base += len(sv)

    return all_v, all_n, all_u, all_j, all_w, all_i

def write_clothbin(verts, norms, uvs, joints4, weights4, indices, out_path):
    nv = len(verts); ni = len(indices)
    # Use u16 indices (clamp to 65535)
    use_u32 = any(i > 65535 for i in indices)
    idx_fmt = 'I' if use_u32 else 'H'
    idx_sz  = 4 if use_u32 else 2
    # flags: bit0 = u32 indices
    flags = 1 if use_u32 else 0

    SCALE = 0.1  # Kenshi units → metric (same factor as hair/body conversion)
    with open(out_path, 'wb') as f:
        f.write(struct.pack('<IIII', MAGIC, nv, ni, flags))
        for i in range(nv):
            px,py,pz = verts[i]
            nx,ny,nz = norms[i]
            u,v       = uvs[i]
            j0,j1,j2,j3 = joints4[i]
            w0,w1,w2,w3 = weights4[i]
            f.write(struct.pack('<3f3f2f4B4f',
                px*SCALE, py*SCALE, pz*SCALE,
                nx, ny, nz,  # normals: no scale
                u, v,
                j0&0xFF, j1&0xFF, j2&0xFF, j3&0xFF,
                w0, w1, w2, w3))
        f.write(struct.pack(f'<{ni}{idx_fmt}', *indices))

def convert(src, dst):
    verts, norms, uvs, joints, weights, indices = convert_mesh(src)
    nv = len(verts); ni = len(indices)
    if nv == 0 or ni == 0:
        print(f"SKIP {os.path.basename(src)}: no geometry"); return False
    write_clothbin(verts, norms, uvs, joints, weights, indices, dst)
    print(f"OK   {os.path.basename(src):40s}  {nv}v {ni//3}t")
    return True

if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('input')
    ap.add_argument('output')
    args = ap.parse_args()
    convert(args.input, args.output)
