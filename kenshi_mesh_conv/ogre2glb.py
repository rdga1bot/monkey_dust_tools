#!/usr/bin/env python3
"""Convert Kenshi/Ogre binary .mesh (v1.100) to GLB for monkey_dust PropMesh."""
import struct, json, sys, os, math

# Ogre vertex element semantics
VES_POSITION  = 1
VES_NORMAL    = 4
VES_TEXCOORD  = 7

# Ogre vertex element types
VET_FLOAT1 = 0
VET_FLOAT2 = 1
VET_FLOAT3 = 2
VET_FLOAT4 = 3

ELEM_SIZES = { VET_FLOAT1: 4, VET_FLOAT2: 8, VET_FLOAT3: 12, VET_FLOAT4: 16 }

# Ogre chunk IDs
M_MESH                        = 0x3000
M_SUBMESH                     = 0x4000
M_SUBMESH_OPERATION           = 0x4010
M_GEOMETRY                    = 0x5000
M_GEOMETRY_VERTEX_DECLARATION = 0x5100
M_GEOMETRY_VERTEX_ELEMENT     = 0x5110
M_GEOMETRY_VERTEX_BUFFER      = 0x5200
M_GEOMETRY_VERTEX_BUFFER_DATA = 0x5210
M_MESH_BOUNDS                 = 0x9000


def read_null_string(data, off):
    end = data.index(b'\x00', off)
    return data[off:end].decode('utf-8', errors='replace'), end + 1


def parse_geometry(data, off, end, verts_out, norms_out):
    """Parse M_GEOMETRY block content (off = byte immediately after chunk header)."""
    if off + 4 > end:
        return
    nv = struct.unpack_from('<I', data, off)[0]
    off += 4

    elements = []   # list of (source, type, semantic, byte_offset)
    buffers  = {}   # source → (bytes, stride)

    while off + 6 <= end:
        cid = struct.unpack_from('<H', data, off)[0]
        csz = struct.unpack_from('<I', data, off + 2)[0]
        if csz < 6 or off + csz > end:
            break
        cd  = data[off + 6: off + csz]
        off += csz

        if cid == M_GEOMETRY_VERTEX_DECLARATION:
            # Elements are wrapped inside this container chunk
            doff = 0
            while doff + 6 <= len(cd):
                dcid = struct.unpack_from('<H', cd, doff)[0]
                dcsz = struct.unpack_from('<I', cd, doff + 2)[0]
                if dcsz < 6 or doff + dcsz > len(cd):
                    break
                if dcid == M_GEOMETRY_VERTEX_ELEMENT:
                    src, vtype, semantic, voff, _ = struct.unpack_from('<5H', cd, doff + 6)
                    elements.append((src, vtype, semantic, voff))
                doff += dcsz

        elif cid == M_GEOMETRY_VERTEX_ELEMENT:
            # Older exporters emit elements directly at geometry level
            src, vtype, semantic, voff, _ = struct.unpack_from('<5H', cd)
            elements.append((src, vtype, semantic, voff))

        elif cid == M_GEOMETRY_VERTEX_BUFFER:
            # bindIndex(u16) vertexSize(u16) then nested M_GEOMETRY_VERTEX_BUFFER_DATA
            bind_idx, vstride = struct.unpack_from('<HH', cd)
            sub_off = 4
            while sub_off + 6 <= len(cd):
                scid = struct.unpack_from('<H', cd, sub_off)[0]
                scsz = struct.unpack_from('<I', cd, sub_off + 2)[0]
                if scsz < 6 or sub_off + scsz > len(cd):
                    break
                if scid == M_GEOMETRY_VERTEX_BUFFER_DATA:
                    buffers[bind_idx] = (cd[sub_off + 6: sub_off + scsz], vstride)
                sub_off += scsz

    # Extract positions and normals from vertex buffers
    pos_elem  = next((e for e in elements if e[2] == VES_POSITION), None)
    norm_elem = next((e for e in elements if e[2] == VES_NORMAL),   None)

    if pos_elem and pos_elem[0] in buffers:
        buf, stride = buffers[pos_elem[0]]
        base = pos_elem[3]
        for vi in range(nv):
            x, y, z = struct.unpack_from('<3f', buf, vi * stride + base)
            verts_out.append((x, y, z))

    if norm_elem and norm_elem[0] in buffers:
        buf, stride = buffers[norm_elem[0]]
        base = norm_elem[3]
        for vi in range(nv):
            nx, ny, nz = struct.unpack_from('<3f', buf, vi * stride + base)
            norms_out.append((nx, ny, nz))
    elif pos_elem:
        # No normals — pad with up-vector
        for _ in range(nv):
            norms_out.append((0.0, 1.0, 0.0))


def parse_submesh(data, off, end, all_verts, all_norms, all_indices, base_vertex):
    """Parse M_SUBMESH block — reads index list + embedded geometry."""
    # material name (null-terminated string, may contain \n before \0)
    _, off = read_null_string(data, off)

    # Ogre v1.100 M_SUBMESH layout (NO use_shared flat field here):
    #   indexCount  uint32
    #   idx32Bit    uint8 bool
    #   indices[]   uint16 or uint32 × indexCount
    #   sub-chunks  (M_GEOMETRY if vertices not shared, M_SUBMESH_OPERATION, …)
    if off + 5 > end:
        return base_vertex
    num_idx = struct.unpack_from('<I', data, off)[0]
    off += 4
    idx32 = struct.unpack_from('<?', data, off)[0]
    off += 1

    idx_bytes = num_idx * (4 if idx32 else 2)
    if off + idx_bytes > end:
        return base_vertex
    fmt = f'<{num_idx}{"I" if idx32 else "H"}'
    raw_idx = struct.unpack_from(fmt, data, off)
    off += idx_bytes

    # Rebase indices
    for i in raw_idx:
        all_indices.append(i + base_vertex)

    # Walk nested chunks for embedded geometry (use_shared == False)
    local_verts = []
    local_norms = []
    while off + 6 <= end:
        cid = struct.unpack_from('<H', data, off)[0]
        csz = struct.unpack_from('<I', data, off + 2)[0]
        if csz < 6 or off + csz > end:
            break
        cd_off = off + 6
        cd_end = off + csz
        off    = cd_end

        if cid == M_GEOMETRY:
            parse_geometry(data, cd_off, cd_end, local_verts, local_norms)
        elif cid == M_SUBMESH_OPERATION:
            pass  # ignore
        elif cid in (0x4100, 0x4200, 0x4300):
            pass  # bone assignments / aliases — skip

    all_verts  .extend(local_verts)
    all_norms  .extend(local_norms)
    return base_vertex + len(local_verts)


def parse_ogre_mesh(path):
    data = open(path, 'rb').read()

    # Verify M_HEADER (0x1000) at start
    if struct.unpack_from('<H', data, 0)[0] != 0x1000:
        raise ValueError("Not an Ogre mesh (missing 0x1000 header)")

    # Version string ends with \n; the following \x00 is first byte of M_MESH id (0x3000)
    nl = data.index(b'\n', 2)
    off = nl + 1   # points to \x00 = first byte of 0x3000

    all_verts   = []
    all_norms   = []
    all_indices = []
    shared_verts = []
    shared_norms = []

    while off + 6 <= len(data):
        cid = struct.unpack_from('<H', data, off)[0]
        csz = struct.unpack_from('<I', data, off + 2)[0]
        if csz < 6 or off + csz > len(data):
            break
        cd_off = off + 6
        cd_end = off + csz

        if cid == M_MESH:
            # bool skeletallyAnimated (1 byte), then sub-chunks
            sub_off = cd_off + 1
            base_v  = 0
            while sub_off + 6 <= cd_end:
                scid = struct.unpack_from('<H', data, sub_off)[0]
                scsz = struct.unpack_from('<I', data, sub_off + 2)[0]
                if scsz < 6 or sub_off + scsz > cd_end:
                    break
                s_cd_off = sub_off + 6
                s_cd_end = sub_off + scsz
                sub_off  = s_cd_end

                if scid == M_SUBMESH:
                    base_v = parse_submesh(data, s_cd_off, s_cd_end,
                                           all_verts, all_norms, all_indices, base_v)
                elif scid == M_GEOMETRY:
                    parse_geometry(data, s_cd_off, s_cd_end, shared_verts, shared_norms)
                    all_verts.extend(shared_verts)
                    all_norms.extend(shared_norms)
                    base_v += len(shared_verts)

        off = cd_end

    return all_verts, all_norms, all_indices


def write_glb(verts, norms, indices, out_path):
    """Write a minimal GLB with POSITION + NORMAL + indices."""
    nv = len(verts)
    ni = len(indices)
    assert nv == len(norms), f"vert/norm count mismatch {nv} vs {len(norms)}"
    assert ni % 3 == 0, f"index count {ni} not divisible by 3"

    # Determine index format
    use_u32 = nv > 65535
    idx_fmt  = '<' + ('I' * ni if use_u32 else 'H' * ni)
    idx_size = ni * (4 if use_u32 else 2)
    idx_component = 5125 if use_u32 else 5123   # UNSIGNED_INT or UNSIGNED_SHORT

    # Pack binary data: positions | normals | (align to 4) | indices
    pos_bytes  = struct.pack('<' + 'fff' * nv, *[c for v in verts for c in v])
    norm_bytes = struct.pack('<' + 'fff' * nv, *[c for n in norms for c in n])
    idx_bytes  = struct.pack(idx_fmt, *indices)

    # Pad idx_bytes to 4-byte boundary
    idx_pad = (4 - len(idx_bytes) % 4) % 4
    idx_bytes_padded = idx_bytes + b'\x00' * idx_pad

    bin_data = pos_bytes + norm_bytes + idx_bytes_padded
    bin_len  = len(bin_data)

    # AABB for accessor min/max
    xs = [v[0] for v in verts]; ys = [v[1] for v in verts]; zs = [v[2] for v in verts]

    # JSON
    bv_pos_off  = 0
    bv_norm_off = nv * 12
    bv_idx_off  = nv * 24
    gltf = {
        "asset": {"version": "2.0", "generator": "monkey_dust ogre2glb"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1},
            "indices": 2,
            "mode": 4
        }]}],
        "accessors": [
            {
                "bufferView": 0, "byteOffset": 0,
                "componentType": 5126, "count": nv, "type": "VEC3",
                "min": [min(xs), min(ys), min(zs)],
                "max": [max(xs), max(ys), max(zs)]
            },
            {
                "bufferView": 1, "byteOffset": 0,
                "componentType": 5126, "count": nv, "type": "VEC3"
            },
            {
                "bufferView": 2, "byteOffset": 0,
                "componentType": idx_component, "count": ni, "type": "SCALAR"
            }
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": bv_pos_off,  "byteLength": nv * 12, "target": 34962},
            {"buffer": 0, "byteOffset": bv_norm_off, "byteLength": nv * 12, "target": 34962},
            {"buffer": 0, "byteOffset": bv_idx_off,  "byteLength": len(idx_bytes_padded), "target": 34963}
        ],
        "buffers": [{"byteLength": bin_len}]
    }

    json_bytes = json.dumps(gltf, separators=(',', ':')).encode('utf-8')
    json_pad   = (4 - len(json_bytes) % 4) % 4
    json_bytes_padded = json_bytes + b' ' * json_pad

    total_len = 12 + 8 + len(json_bytes_padded) + 8 + bin_len

    with open(out_path, 'wb') as f:
        # GLB header
        f.write(struct.pack('<III', 0x46546C67, 2, total_len))
        # JSON chunk
        f.write(struct.pack('<II', len(json_bytes_padded), 0x4E4F534A))
        f.write(json_bytes_padded)
        # BIN chunk
        f.write(struct.pack('<II', bin_len, 0x004E4942))
        f.write(bin_data)

    return nv, ni // 3


def ground_verts(verts):
    """Shift vertices so Y_min = 0 (mesh stands on ground, not half-buried)."""
    if not verts:
        return verts
    y_min = min(v[1] for v in verts)
    if abs(y_min) < 1e-4:
        return verts
    return [(x, y - y_min, z) for x, y, z in verts]


def convert(src, dst):
    print(f"  {os.path.basename(src)} → {os.path.basename(dst)}", end='', flush=True)
    try:
        verts, norms, indices = parse_ogre_mesh(src)
        if not verts or not indices:
            print(f"  SKIP (empty mesh)")
            return False
        verts = ground_verts(verts)
        nv, nt = write_glb(verts, norms, indices, dst)
        print(f"  {nv}v {nt}t OK")
        return True
    except Exception as e:
        print(f"  ERROR: {e}")
        return False


if __name__ == '__main__':
    kenshi = "/run/media/rdga1/win/SteamLibrary/steamapps/common/Kenshi/data"
    out_dir = "game/data/props"
    os.makedirs(out_dir, exist_ok=True)

    targets = [
        (f"{kenshi}/foliage/rocks/Rock_Round.mesh",       f"{out_dir}/rock_round.glb"),
        (f"{kenshi}/foliage/rocks/rockformation.mesh",    f"{out_dir}/rock_formation.glb"),
        (f"{kenshi}/foliage/rocks/rock_M_round.mesh",     f"{out_dir}/rock_m_round.glb"),
        (f"{kenshi}/foliage/rocks/canyonrock.mesh",       f"{out_dir}/canyon_rock.glb"),
        (f"{kenshi}/foliage/rocks/hatrock.mesh",          f"{out_dir}/hat_rock.glb"),
        (f"{kenshi}/foliage/Trees/deadtree01.mesh",       f"{out_dir}/dead_tree.glb"),
        (f"{kenshi}/foliage/Trees/shrub.mesh",            f"{out_dir}/shrub.glb"),
        (f"{kenshi}/foliage/Trees/YuccaBig.mesh",         f"{out_dir}/yucca.glb"),
        (f"{kenshi}/foliage/Trees/yucca.mesh",            f"{out_dir}/yucca_small.glb"),
        (f"{kenshi}/foliage/Trees/juniper.mesh",          f"{out_dir}/juniper.glb"),
    ]

    ok = 0
    for src, dst in targets:
        if not os.path.exists(src):
            print(f"  MISSING: {src}")
            continue
        if convert(src, dst):
            ok += 1

    print(f"\nConverted {ok}/{len(targets)}")
