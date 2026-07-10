#!/usr/bin/env python3
"""
md_convert_static.py — OGRE mesh.xml -> GLB converter for simple STATIC
(non-skeletal) props: ground clutter, rocks, plants. Unlike md_convert.py
(character mesh + skeleton + animation), this only emits POSITION/NORMAL/
TEXCOORD_0 + indices — no skin, no material/image (clutter_gen.cpp's
load_source_glb never reads GLB materials; texture layer is supplied
separately at placement time).

Usage:
  OgreXMLConverter <mesh> /tmp/out.mesh.xml
  python3 tools/md_convert_static.py /tmp/out.mesh.xml game/data/props/out.glb --scale 0.05

Prints the raw-mesh AABB (before --scale is applied) so the caller can pick
a correct per-mesh scale factor by measurement, not guesswork -- same
practice used for every other Kenshi-derived prop in this project.
"""
import struct, json, sys, argparse
import xml.etree.ElementTree as ET  # trusted local OgreXMLConverter output only


def parse_vertexbuffer(vb_el):
    has_pos  = vb_el.get("positions") == "true"
    has_norm = vb_el.get("normals") == "true"
    has_uv   = vb_el.get("texture_coords") not in (None, "0")
    verts = []
    for v in vb_el.findall("vertex"):
        p = v.find("position")
        n = v.find("normal")
        t = v.find("texcoord")
        px, py, pz = (float(p.get("x")), float(p.get("y")), float(p.get("z"))) if has_pos and p is not None else (0.0, 0.0, 0.0)
        nx, ny, nz = (float(n.get("x")), float(n.get("y")), float(n.get("z"))) if has_norm and n is not None else (0.0, 1.0, 0.0)
        u, w = (float(t.get("u")), float(t.get("v"))) if has_uv and t is not None else (0.0, 0.0)
        verts.append((px, py, pz, nx, ny, nz, u, w))
    return verts


def parse_mesh_xml(path):
    root = ET.parse(path).getroot()
    shared_verts = []
    shared_el = root.find("sharedgeometry")
    if shared_el is not None:
        vc = int(shared_el.get("vertexcount", "0"))
        for vb in shared_el.findall("vertexbuffer"):
            vs = parse_vertexbuffer(vb)
            if len(vs) == vc:
                shared_verts = vs
                break

    all_verts = []
    all_idx = []
    subs_el = root.find("submeshes")
    if subs_el is None:
        raise SystemExit("no <submeshes> found")
    for sub in subs_el.findall("submesh"):
        uses_shared = sub.get("usesharedvertices") == "true"
        if uses_shared:
            verts = shared_verts
            base = 0
        else:
            geo = sub.find("geometry")
            verts = []
            if geo is not None:
                for vb in geo.findall("vertexbuffer"):
                    vs = parse_vertexbuffer(vb)
                    if vs:
                        verts = vs
                        break
            base = len(all_verts)
            all_verts.extend(verts)
        faces_el = sub.find("faces")
        if faces_el is None:
            continue
        for f in faces_el.findall("face"):
            v1, v2, v3 = int(f.get("v1")), int(f.get("v2")), int(f.get("v3"))
            all_idx.extend([base + v1, base + v2, base + v3])
    if uses_shared_any(subs_el) and not all_verts:
        all_verts = shared_verts
    return all_verts, all_idx


def uses_shared_any(subs_el):
    return any(s.get("usesharedvertices") == "true" for s in subs_el.findall("submesh"))


def aabb(verts):
    xs = [v[0] for v in verts]; ys = [v[1] for v in verts]; zs = [v[2] for v in verts]
    return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))


def write_glb(verts, idx, out_path, scale):
    # Interleaved POSITION(f3)+NORMAL(f3)+TEXCOORD_0(f2) = 32 bytes/vertex.
    vtx_bytes = bytearray()
    minp = [1e30, 1e30, 1e30]; maxp = [-1e30, -1e30, -1e30]
    for (px, py, pz, nx, ny, nz, u, w) in verts:
        sx, sy, sz = px * scale, py * scale, pz * scale
        for i, val in enumerate((sx, sy, sz)):
            minp[i] = min(minp[i], val); maxp[i] = max(maxp[i], val)
        vtx_bytes += struct.pack("<3f3f2f", sx, sy, sz, nx, ny, nz, u, w)

    use32 = len(verts) > 65535
    idx_fmt = "<I" if use32 else "<H"
    idx_bytes = bytearray()
    for i in idx:
        idx_bytes += struct.pack(idx_fmt, i)
    # 4-byte align index buffer (glTF bufferView alignment convenience).
    while len(idx_bytes) % 4 != 0:
        idx_bytes += b"\x00"

    bin_blob = bytes(vtx_bytes) + bytes(idx_bytes)
    gltf = {
        "asset": {"version": "2.0", "generator": "md_convert_static.py"},
        "scenes": [{"nodes": [0]}],
        "scene": 0,
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
            "indices": 3,
        }]}],
        "buffers": [{"byteLength": len(bin_blob)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(vtx_bytes), "byteStride": 32, "target": 34962},
            {"buffer": 0, "byteOffset": len(vtx_bytes), "byteLength": len(idx_bytes), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "byteOffset": 0,  "componentType": 5126, "count": len(verts), "type": "VEC3", "min": minp, "max": maxp},
            {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": len(verts), "type": "VEC3"},
            {"bufferView": 0, "byteOffset": 24, "componentType": 5126, "count": len(verts), "type": "VEC2"},
            {"bufferView": 1, "byteOffset": 0,  "componentType": 5123 if not use32 else 5125, "count": len(idx), "type": "SCALAR"},
        ],
    }
    json_bytes = json.dumps(gltf).encode("utf-8")
    while len(json_bytes) % 4 != 0:
        json_bytes += b" "

    def chunk(ctype, data):
        return struct.pack("<II", len(data), ctype) + data

    body = chunk(0x4E4F534A, json_bytes) + chunk(0x004E4942, bytes(bin_blob))
    header = struct.pack("<III", 0x46546C67, 2, 12 + len(body))
    with open(out_path, "wb") as f:
        f.write(header + body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mesh_xml")
    ap.add_argument("out_glb")
    ap.add_argument("--scale", type=float, default=1.0)
    args = ap.parse_args()

    verts, idx = parse_mesh_xml(args.mesh_xml)
    if not verts:
        raise SystemExit(f"no vertices parsed from {args.mesh_xml}")
    lo, hi = aabb(verts)
    size = tuple(hi[i] - lo[i] for i in range(3))
    print(f"[md_convert_static] {args.mesh_xml}: {len(verts)} verts, {len(idx)//3} tris, "
          f"raw AABB size = ({size[0]:.3f}, {size[1]:.3f}, {size[2]:.3f}) raw units")
    write_glb(verts, idx, args.out_glb, args.scale)
    print(f"[md_convert_static] wrote {args.out_glb} (scale={args.scale})")


if __name__ == "__main__":
    main()
