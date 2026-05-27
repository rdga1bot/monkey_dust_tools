#!/usr/bin/env python3
"""
md_convert.py  —  OGRE XML → GLB converter for Kenshi character assets.
Usage:
  python3 tools/md_convert.py                      # converts human_male
  python3 tools/md_convert.py --anim walk,run,idle # include named anims
  python3 tools/md_convert.py --all-anims          # include all 174 anims

Output: game/data/props/md_human.glb
"""

try:
    import defusedxml.ElementTree as ET  # pip install defusedxml
except ImportError:
    import xml.etree.ElementTree as ET  # trusted local OgreXMLConverter output only
import struct, json, math, sys, argparse
from collections import defaultdict

MESH_XML = "/tmp/md_mesh/human_male.mesh.xml"
SKEL_XML = "/tmp/md_mesh/male_skeleton.skeleton.xml"
OUT_GLB  = "game/data/props/md_human.glb"

# Scale Kenshi units → world units. Kenshi char height ≈ 17 units → ~1.7m
SCALE = 0.1

# ── Quaternion helpers ──────────────────────────────────────────────────────

def axis_angle_to_quat(ax, ay, az, angle):
    s = math.sin(angle * 0.5)
    ln = math.sqrt(ax*ax + ay*ay + az*az)
    if ln < 1e-9: return (0.0, 0.0, 0.0, 1.0)
    return (ax/ln*s, ay/ln*s, az/ln*s, math.cos(angle*0.5))

def quat_mul(a, b):
    ax,ay,az,aw = a
    bx,by,bz,bw = b
    return (
        aw*bx + ax*bw + ay*bz - az*by,
        aw*by - ax*bz + ay*bw + az*bx,
        aw*bz + ax*by - ay*bx + az*bw,
        aw*bw - ax*bx - ay*by - az*bz,
    )

def quat_norm(q):
    l = math.sqrt(sum(v*v for v in q))
    if l < 1e-9: return (0,0,0,1)
    return tuple(v/l for v in q)

def mat4_from_trs(tx,ty,tz, qx,qy,qz,qw, sx,sy,sz):
    x2,y2,z2 = qx*2,qy*2,qz*2
    xx,yy,zz = qx*x2,qy*y2,qz*z2
    xy,xz,yz = qx*y2,qx*z2,qy*z2
    wx,wy,wz = qw*x2,qw*y2,qw*z2
    m = [0.0]*16
    m[0]=(1-(yy+zz))*sx; m[1]=(xy+wz)*sx;   m[2]=(xz-wy)*sx;   m[3]=0
    m[4]=(xy-wz)*sy;     m[5]=(1-(xx+zz))*sy;m[6]=(yz+wx)*sy;   m[7]=0
    m[8]=(xz+wy)*sz;     m[9]=(yz-wx)*sz;   m[10]=(1-(xx+yy))*sz;m[11]=0
    m[12]=tx;             m[13]=ty;            m[14]=tz;            m[15]=1
    return m

def mat4_inv(m):
    # 4×4 general inverse using cofactor expansion
    def sub3(r0,c0):
        rows = [i for i in range(4) if i!=r0]
        cols = [j for j in range(4) if j!=c0]
        return [[m[rows[i]*4+cols[j]] for j in range(3)] for i in range(3)]
    def det3(a):
        return (a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])
               -a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])
               +a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]))
    cofac = []
    for r in range(4):
        for c in range(4):
            cofac.append(((-1)**(r+c)) * det3(sub3(r,c)))
    det = sum(m[c]*cofac[c] for c in range(4))
    if abs(det) < 1e-12: return list(m)
    adj = [cofac[r*4+c] for c in range(4) for r in range(4)]
    return [v/det for v in adj]

def mat4_mul(a, b):
    out = [0.0]*16
    for c in range(4):
        for r in range(4):
            out[c*4+r] = sum(a[k*4+r] * b[c*4+k] for k in range(4))
    return out

# ── GLB builder ─────────────────────────────────────────────────────────────

class GLBBuilder:
    def __init__(self):
        self.accessors  = []
        self.buffer_views = []
        self.bin_data   = bytearray()
        self.meshes     = []
        self.nodes      = []
        self.skins      = []
        self.animations = []
        self.materials  = []

    def _align(self, n=4):
        while len(self.bin_data) % n:
            self.bin_data += b'\x00'

    def add_buffer(self, data: bytes, target=None):
        self._align(4)
        offset = len(self.bin_data)
        self.bin_data += data
        bv = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target: bv["target"] = target
        idx = len(self.buffer_views)
        self.buffer_views.append(bv)
        return idx

    def add_accessor(self, bv_idx, comp_type, count, atype, min_=None, max_=None, byte_offset=0):
        a = {"bufferView": bv_idx, "componentType": comp_type,
             "count": count, "type": atype, "byteOffset": byte_offset}
        if min_ is not None: a["min"] = min_
        if max_ is not None: a["max"] = max_
        idx = len(self.accessors)
        self.accessors.append(a)
        return idx

    def pack_vec3(self, vecs):
        return struct.pack(f'<{len(vecs)*3}f', *[v for xyz in vecs for v in xyz])

    def pack_vec4(self, vecs):
        return struct.pack(f'<{len(vecs)*4}f', *[v for xyzw in vecs for v in xyzw])

    def pack_uvec4(self, vecs):
        return bytes(v for xyzw in vecs for v in xyzw)

    def pack_mat4(self, mats):
        return struct.pack(f'<{len(mats)*16}f', *[v for m in mats for v in m])

    def pack_float(self, vals):
        return struct.pack(f'<{len(vals)}f', *vals)

    def pack_u16(self, vals):
        return struct.pack(f'<{len(vals)}H', *vals)

    def to_glb(self):
        # Only root nodes (not referenced as any child) go into scene.nodes
        all_children = set()
        for nd in self.nodes:
            for c in nd.get("children", []):
                all_children.add(c)
        root_nodes = [i for i in range(len(self.nodes)) if i not in all_children]

        gltf = {
            "asset": {"version": "2.0", "generator": "md_convert.py"},
            "scene": 0, "scenes": [{"nodes": root_nodes}],
            "nodes":        self.nodes,
            "meshes":       self.meshes,
            "accessors":    self.accessors,
            "bufferViews":  self.buffer_views,
            "buffers":      [{"byteLength": len(self.bin_data)}],
        }
        if self.skins:      gltf["skins"]      = self.skins
        if self.animations: gltf["animations"]  = self.animations
        if self.materials:  gltf["materials"]   = self.materials

        json_bytes = json.dumps(gltf, separators=(',',':')).encode('utf-8')
        while len(json_bytes) % 4: json_bytes += b' '
        while len(self.bin_data) % 4: self.bin_data += b'\x00'

        total = 12 + 8 + len(json_bytes) + 8 + len(self.bin_data)
        out = bytearray()
        out += struct.pack('<III', 0x46546C67, 2, total)          # header
        out += struct.pack('<II', len(json_bytes), 0x4E4F534A)    # JSON chunk
        out += json_bytes
        out += struct.pack('<II', len(self.bin_data), 0x004E4942) # BIN chunk
        out += self.bin_data
        return bytes(out)

# ── Parse mesh XML ───────────────────────────────────────────────────────────

def parse_mesh(xml_path):
    tree = ET.parse(xml_path)
    root = tree.getroot()
    sm   = root.find('submeshes/submesh')
    geo  = sm.find('geometry')
    vbs  = geo.findall('vertexbuffer')

    positions, normals, uvs = [], [], []
    for v in vbs[0]:   # buf 0: positions + normals
        p = v.find('position')
        n = v.find('normal')
        positions.append((float(p.get('x'))*SCALE, float(p.get('y'))*SCALE, float(p.get('z'))*SCALE))
        normals.append((float(n.get('x')), float(n.get('y')), float(n.get('z'))))

    if len(vbs) > 1:   # buf 1: UVs (texcoords) + tangents
        for v in vbs[1]:
            tc = v.find('texcoord')
            if tc is not None:
                uvs.append((float(tc.get('u')), float(tc.get('v'))))
            else:
                uvs.append((0.0, 0.0))
    else:
        uvs = [(0.0, 0.0)] * len(positions)

    indices = []
    for f in sm.find('faces'):
        indices += [int(f.get('v1')), int(f.get('v2')), int(f.get('v3'))]

    # Bone assignments: collect all weights per vertex, keep top 4
    weights_map = defaultdict(list)
    for a in sm.find('boneassignments'):
        vi = int(a.get('vertexindex'))
        bi = int(a.get('boneindex'))
        w  = float(a.get('weight'))
        weights_map[vi].append((w, bi))

    joints, weights = [], []
    nv = len(positions)
    for vi in range(nv):
        ws = sorted(weights_map.get(vi,[]), reverse=True)[:4]
        while len(ws) < 4: ws.append((0.0, 0))
        total = sum(w for w,_ in ws) or 1.0
        joints.append(tuple(b for _,b in ws))
        weights.append(tuple(w/total for w,_ in ws))

    return positions, normals, uvs, indices, joints, weights

# ── Parse OGRE pose animations (face morph targets) ──────────────────────────

def parse_poses(xml_path):
    """Return list of (name, {vert_idx: (dx,dy,dz)}) from OGRE <poses> section."""
    tree = ET.parse(xml_path)
    root = tree.getroot()
    poses_el = None
    for el in root.iter('poses'):
        poses_el = el
        break
    if poses_el is None:
        return []
    result = []
    seen = set()
    for pose in poses_el:
        name = pose.get('name', '')
        if name in seen:
            continue
        seen.add(name)
        offsets = {}
        for po in pose.findall('poseoffset'):
            vi = int(po.get('index'))
            dx = float(po.get('x', 0)) * SCALE
            dy = float(po.get('y', 0)) * SCALE
            dz = float(po.get('z', 0)) * SCALE
            offsets[vi] = (dx, dy, dz)
        result.append((name, offsets))
    return result

# ── Parse skeleton XML ───────────────────────────────────────────────────────

def parse_skeleton(xml_path, wanted_anims=None):
    tree = ET.parse(xml_path)
    root = tree.getroot()

    bone_list = list(root.find('bones'))
    bone_names = [b.get('name') for b in bone_list]
    bone_idx   = {n: i for i,n in enumerate(bone_names)}
    n_bones    = len(bone_list)

    # Bind pose: local translation + rotation (axis-angle) per bone
    bind_trans = []
    bind_quat  = []
    for b in bone_list:
        p  = b.find('position')
        r  = b.find('rotation')
        ax = r.find('axis')
        tx = float(p.get('x'))*SCALE
        ty = float(p.get('y'))*SCALE
        tz = float(p.get('z'))*SCALE
        angle = float(r.get('angle'))
        q = axis_angle_to_quat(float(ax.get('x')), float(ax.get('y')), float(ax.get('z')), angle)
        bind_trans.append((tx, ty, tz))
        bind_quat.append(q)

    # Build parent array
    parents = [-1] * n_bones
    for pe in root.find('bonehierarchy'):
        child  = pe.get('bone')
        parent = pe.get('parent')
        if child in bone_idx and parent in bone_idx:
            parents[bone_idx[child]] = bone_idx[parent]

    # World bind matrices (for inverse bind matrix computation)
    world_mats = [None] * n_bones
    def build_world(bi):
        if world_mats[bi] is not None: return world_mats[bi]
        tx,ty,tz = bind_trans[bi]
        qx,qy,qz,qw = bind_quat[bi]
        local = mat4_from_trs(tx,ty,tz, qx,qy,qz,qw, 1,1,1)
        pi = parents[bi]
        if pi < 0:
            world_mats[bi] = local
        else:
            world_mats[bi] = mat4_mul(build_world(pi), local)
        return world_mats[bi]
    for i in range(n_bones): build_world(i)
    inv_bind = [mat4_inv(m) for m in world_mats]

    # Parse animations
    anims = []
    for anim_el in root.find('animations'):
        name = anim_el.get('name')
        if wanted_anims is not None and name not in wanted_anims:
            continue
        length = float(anim_el.get('length', 0))
        tracks = {}
        for track_el in anim_el.find('tracks'):
            bname = track_el.get('bone')
            if bname not in bone_idx: continue
            bi = bone_idx[bname]
            kf_list = []
            for kf in track_el.find('keyframes'):
                t  = float(kf.get('time'))
                tr = kf.find('translate')
                ro = kf.find('rotate')
                tx_k = float(tr.get('x'))*SCALE if tr is not None else 0.0
                ty_k = float(tr.get('y'))*SCALE if tr is not None else 0.0
                tz_k = float(tr.get('z'))*SCALE if tr is not None else 0.0
                if ro is not None:
                    ax = ro.find('axis')
                    ang = float(ro.get('angle'))
                    q_k = axis_angle_to_quat(float(ax.get('x')), float(ax.get('y')), float(ax.get('z')), ang)
                    # Accumulate on top of bind pose rotation
                    q_k = quat_norm(quat_mul(bind_quat[bi], q_k))
                else:
                    q_k = bind_quat[bi]
                # Translation is additive to bind pose
                btx,bty,btz = bind_trans[bi]
                kf_list.append((t, (btx+tx_k, bty+ty_k, btz+tz_k), q_k))
            if kf_list:
                tracks[bi] = kf_list
        anims.append({"name": name, "length": length, "tracks": tracks})

    return bone_names, parents, bind_trans, bind_quat, inv_bind, anims

# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--anim', default='walk lower,run lower,idle_stand_normal,breathing,combatstance,jog lower')
    parser.add_argument('--all-anims', action='store_true')
    args = parser.parse_args()

    wanted = None if args.all_anims else set(args.anim.split(','))
    print(f"Converting {MESH_XML}")
    positions, normals, uvs, indices, joints, weights = parse_mesh(MESH_XML)
    print(f"  Vertices={len(positions)}  Faces={len(indices)//3}  UVs={'yes' if uvs[0]!=(0,0) else 'none'}")

    print(f"Converting {SKEL_XML}")
    bone_names, parents, bind_trans, bind_quat, inv_bind, anims = parse_skeleton(SKEL_XML, wanted)
    print(f"  Bones={len(bone_names)}  Animations={len(anims)}")

    b = GLBBuilder()
    nv = len(positions)

    # ── Geometry buffers ──────────────────────────────────────────────────
    pos_data = b.pack_vec3(positions)
    nrm_data = b.pack_vec3(normals)
    uv_data  = b.pack_vec3([(u, v, 0.0) for u, v in uvs])  # pack as VEC2 but store F2
    uv_data  = struct.pack(f'<{nv*2}f', *[x for uv in uvs for x in uv])
    jnt_data = b.pack_uvec4(joints)
    wgt_data = b.pack_vec4(weights)
    idx_data = b.pack_u16(indices)

    bv_pos = b.add_buffer(pos_data, target=34962)
    bv_nrm = b.add_buffer(nrm_data, target=34962)
    bv_uv  = b.add_buffer(uv_data,  target=34962)
    bv_jnt = b.add_buffer(jnt_data, target=34962)
    bv_wgt = b.add_buffer(wgt_data, target=34962)
    bv_idx = b.add_buffer(idx_data, target=34963)

    xs = [p[0] for p in positions]; ys = [p[1] for p in positions]; zs = [p[2] for p in positions]
    acc_pos = b.add_accessor(bv_pos, 5126, nv, "VEC3", [min(xs),min(ys),min(zs)], [max(xs),max(ys),max(zs)])
    acc_nrm = b.add_accessor(bv_nrm, 5126, nv, "VEC3")
    acc_uv  = b.add_accessor(bv_uv,  5126, nv, "VEC2")
    acc_jnt = b.add_accessor(bv_jnt, 5121, nv, "VEC4")
    acc_wgt = b.add_accessor(bv_wgt, 5126, nv, "VEC4")
    acc_idx = b.add_accessor(bv_idx, 5123, len(indices), "SCALAR")

    # ── Morph targets (OGRE pose animations → GLB blend shapes) ──────────────
    print(f"Extracting poses from {MESH_XML}")
    poses = parse_poses(MESH_XML)
    pose_names = [p[0] for p in poses]
    print(f"  Poses={len(poses)}: {pose_names}")

    morph_targets = []
    for pose_name, offsets in poses:
        # Sparse → dense: build nv-length delta array, fill non-zero entries
        deltas = [(0.0, 0.0, 0.0)] * nv
        for vi, (dx, dy, dz) in offsets.items():
            if vi < nv:
                deltas[vi] = (dx, dy, dz)
        delta_data = b.pack_vec3(deltas)
        bv_delta   = b.add_buffer(delta_data, target=34962)
        dxs = [d[0] for d in deltas]
        dys = [d[1] for d in deltas]
        dzs = [d[2] for d in deltas]
        acc_delta = b.add_accessor(bv_delta, 5126, nv, "VEC3",
                                   [min(dxs), min(dys), min(dzs)],
                                   [max(dxs), max(dys), max(dzs)])
        morph_targets.append({"POSITION": acc_delta})

    prim = {"attributes": {
        "POSITION":   acc_pos, "NORMAL":    acc_nrm,
        "TEXCOORD_0": acc_uv,
        "JOINTS_0":   acc_jnt, "WEIGHTS_0": acc_wgt,
    }, "indices": acc_idx}
    if morph_targets:
        prim["targets"] = morph_targets

    mesh_entry = {"primitives": [prim]}
    if pose_names:
        mesh_entry["extras"]  = {"targetNames": pose_names}
        mesh_entry["weights"] = [0.0] * len(pose_names)
    b.meshes.append(mesh_entry)

    # ── Skeleton nodes ─────────────────────────────────────────────────────
    n_bones = len(bone_names)
    bone_node_base = 1  # node 0 = mesh node
    bone_nodes = []
    for i, (name, (tx,ty,tz), (qx,qy,qz,qw)) in enumerate(zip(bone_names, bind_trans, bind_quat)):
        children = [bone_node_base + j for j,p in enumerate(parents) if p == i]
        nd = {"name": name,
              "translation": [tx, ty, tz],
              "rotation":    [qx, qy, qz, qw]}
        if children: nd["children"] = children
        bone_nodes.append(nd)

    # Root bone nodes (parent == -1)
    root_bones = [bone_node_base + i for i,p in enumerate(parents) if p < 0]

    # Mesh node
    b.nodes.append({"mesh": 0, "skin": 0, "children": root_bones})
    b.nodes.extend(bone_nodes)

    # Inverse bind matrices
    ibm_data = b.pack_mat4(inv_bind)
    bv_ibm   = b.add_buffer(ibm_data)
    acc_ibm  = b.add_accessor(bv_ibm, 5126, n_bones, "MAT4")
    b.skins.append({"joints": list(range(bone_node_base, bone_node_base + n_bones)),
                    "inverseBindMatrices": acc_ibm})

    # ── Animations ────────────────────────────────────────────────────────
    for anim in anims:
        channels = []
        samplers = []
        for bi, kf_list in anim["tracks"].items():
            times   = [kf[0] for kf in kf_list]
            trans   = [kf[1] for kf in kf_list]
            quats   = [kf[2] for kf in kf_list]

            t_data  = b.pack_float(times)
            tr_data = b.pack_vec3(trans)
            ro_data = b.pack_vec4(quats)

            bv_t  = b.add_buffer(t_data)
            bv_tr = b.add_buffer(tr_data)
            bv_ro = b.add_buffer(ro_data)
            nt = len(times)
            acc_t  = b.add_accessor(bv_t,  5126, nt, "SCALAR", [times[0]], [times[-1]])
            acc_tr = b.add_accessor(bv_tr, 5126, nt, "VEC3")
            acc_ro = b.add_accessor(bv_ro, 5126, nt, "VEC4")

            si_t  = len(samplers); samplers.append({"input": acc_t, "output": acc_tr, "interpolation": "LINEAR"})
            si_r  = len(samplers); samplers.append({"input": acc_t, "output": acc_ro, "interpolation": "LINEAR"})
            nidx  = bone_node_base + bi
            channels += [
                {"sampler": si_t, "target": {"node": nidx, "path": "translation"}},
                {"sampler": si_r, "target": {"node": nidx, "path": "rotation"}},
            ]
        if samplers:
            b.animations.append({"name": anim["name"], "channels": channels, "samplers": samplers})

    glb = b.to_glb()
    with open(OUT_GLB, 'wb') as f:
        f.write(glb)
    sz = len(glb)/1024
    print(f"Written {OUT_GLB}  ({sz:.0f} KB)")
    print(f"  Anims exported: {[a['name'] for a in anims]}")

if __name__ == '__main__':
    main()
