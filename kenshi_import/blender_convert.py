"""blender_convert — headless Blender driver for ASSET_PIPELINE_MASTER_PROMPT.md
Phase 2/3: imports one or more Kenshi .mesh files via Kenshi_IO_Continued,
assigns real diffuse/normal textures via find_textures.find_textures_for_mesh
(folder heuristic, see that module's docstring for why a full material-name
resolution isn't possible from this data), bakes the same 0.1 world-unit
scale already established and verified by tools/md_convert_features.py
(--scale 0.1, "1 world unit = 0.1m", confirmed against two real samples),
and exports one GLB per mesh.

Not a Blender addon -- invoked via `blender -b --python`. Meshes to convert
come from a manifest file (one Kenshi-style path per line, e.g.
".\\data\\buildings\\wall.mesh") passed after `--`:

  blender -b --python tools/kenshi_import/blender_convert.py -- \\
      --manifest manifest.txt --kenshi-dir tmp_/kenshi/data \\
      --out-dir game/data/props/buildings \\
      --xml-converter /usr/bin/OgreXMLConverter

Each line of the manifest is converted independently; a failure on one
mesh is logged and does not abort the batch (real per-file try/except,
not a blanket assumption every mesh will import cleanly -- see Phase 0's
own findings for how varied real addon failure modes are).
"""
import argparse
import os
import sys
import traceback

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from find_textures import find_textures_for_mesh  # noqa: E402

WORLD_SCALE = 0.1  # verified convention, see this file's own docstring


def sanitize(mesh_path):
    import re
    name = mesh_path.replace('\\', '/').split('/')[-1]
    name = re.sub(r'\.mesh$', '', name, flags=re.IGNORECASE)
    name = re.sub(r'[^A-Za-z0-9_.-]', '_', name)
    return name


def ensure_addon():
    if "Kenshi_IO_Continued" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="Kenshi_IO_Continued")


def clear_scene():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    # orphan image/material data from the previous mesh would otherwise
    # accumulate across a batch run and bloat memory + risk name collisions
    for block_list in (bpy.data.materials, bpy.data.images, bpy.data.meshes):
        for block in list(block_list):
            if block.users == 0:
                block_list.remove(block)


def assign_textures(obj, diffuse_path, normal_path):
    if not diffuse_path and not normal_path:
        return
    mat = bpy.data.materials.new(name=obj.name + "_mat")
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    bsdf = nodes.get("Principled BSDF")
    if bsdf is None:
        return

    if diffuse_path and os.path.isfile(diffuse_path):
        img = bpy.data.images.load(diffuse_path, check_existing=True)
        tex_node = nodes.new("ShaderNodeTexImage")
        tex_node.image = img
        links.new(tex_node.outputs["Color"], bsdf.inputs["Base Color"])

    if normal_path and os.path.isfile(normal_path):
        img_n = bpy.data.images.load(normal_path, check_existing=True)
        img_n.colorspace_settings.name = 'Non-Color'
        tex_n_node = nodes.new("ShaderNodeTexImage")
        tex_n_node.image = img_n
        normal_map_node = nodes.new("ShaderNodeNormalMap")
        links.new(tex_n_node.outputs["Color"], normal_map_node.inputs["Color"])
        links.new(normal_map_node.outputs["Normal"], bsdf.inputs["Normal"])

    obj.data.materials.clear()
    obj.data.materials.append(mat)


def convert_one(mesh_path, out_glb, xml_converter):
    clear_scene()
    result = bpy.ops.import_scene.mesh(
        filepath=os.path.dirname(mesh_path) + os.sep,
        files=[{"name": os.path.basename(mesh_path)}],
        xml_converter='custom',
        custom_xml_converter=xml_converter,
    )
    if 'FINISHED' not in result:
        return False, "import did not report FINISHED"

    mesh_objs = [o for o in bpy.data.objects if o.type == 'MESH']
    if not mesh_objs:
        return False, "no mesh objects in scene after import"

    diffuse, normal = find_textures_for_mesh(mesh_path)
    for obj in mesh_objs:
        assign_textures(obj, diffuse, normal)
        obj.scale = (WORLD_SCALE, WORLD_SCALE, WORLD_SCALE)

    bpy.ops.object.select_all(action='SELECT')
    bpy.context.view_layer.objects.active = mesh_objs[0]
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)

    bpy.ops.export_scene.gltf(filepath=out_glb, export_format='GLB')
    return True, f"diffuse={os.path.basename(diffuse) if diffuse else None} normal={os.path.basename(normal) if normal else None}"


def main():
    argv = sys.argv
    argv = argv[argv.index('--') + 1:] if '--' in argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--kenshi-dir', default='tmp_/kenshi/data')
    ap.add_argument('--out-dir', required=True)
    ap.add_argument('--xml-converter', default='/usr/bin/OgreXMLConverter')
    args = ap.parse_args(argv)

    os.makedirs(args.out_dir, exist_ok=True)
    ensure_addon()
    with open(args.manifest, encoding='utf-8') as f:
        mesh_lines = [ln.strip() for ln in f if ln.strip() and not ln.startswith('#')]

    ok, failed = 0, 0
    for mesh_path_kenshi in mesh_lines:
        rel = mesh_path_kenshi.replace('.\\data\\', '').replace('\\', '/')
        src = os.path.join(args.kenshi_dir, rel)
        name = sanitize(mesh_path_kenshi)
        out_glb = os.path.join(args.out_dir, name + '.glb')
        if not os.path.isfile(src):
            print(f'[SKIP] source not found: {src}')
            failed += 1
            continue
        try:
            success, info = convert_one(src, out_glb, args.xml_converter)
        except Exception as e:
            success, info = False, f"{e}\n{traceback.format_exc()}"
        if success:
            print(f'[OK] {name}: {info}')
            ok += 1
        else:
            print(f'[FAIL] {name}: {info}')
            failed += 1

    print(f'[SUMMARY] ok={ok} failed={failed} total={len(mesh_lines)}')


if __name__ == '__main__':
    main()
