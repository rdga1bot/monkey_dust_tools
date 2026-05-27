#!/usr/bin/env python3
"""
morph_extractor_a.py — Path A: extract OGRE pose morphs from Kenshi human_male.mesh
via OgreXMLConverter → XML → JSON deltas.

Outputs:
  game/data/chars/morphs_a.json   — per-morph vertex delta arrays
  game/data/chars/morph_names.txt — one pose name per line (for editor sliders)

Usage:
  python3 tools/morph_extractor_a.py [kenshi_mesh_path]
  # default: .../Kenshi/data/character/meshes/human/human_male.mesh

Requirements: OgreXMLConverter must be in PATH (ogre-14.5.2 package).
"""
import json, os, sys, subprocess, tempfile, xml.etree.ElementTree as ET

KENSHI_MESH = (
    sys.argv[1] if len(sys.argv) > 1
    else "/run/media/rdga1/win/SteamLibrary/steamapps/common/Kenshi/data/"
         "character/meshes/human/human_male.mesh"
)
OUT_JSON  = "game/data/chars/morphs_a.json"
OUT_NAMES = "game/data/chars/morph_names.txt"


def mesh_to_xml(mesh_path, xml_path):
    result = subprocess.run(
        ["OgreXMLConverter", mesh_path, xml_path],
        capture_output=True, text=True
    )
    if result.returncode != 0 or not os.path.exists(xml_path):
        print(f"[A] OgreXMLConverter failed:\n{result.stderr}")
        return False
    return True


def parse_poses_xml(xml_path):
    tree = ET.parse(xml_path)
    root = tree.getroot()

    poses_elem = root.find('.//poses')
    if poses_elem is None:
        print("[A] No <poses> element in XML.")
        return []

    poses = []
    for pose in poses_elem.findall('pose'):
        name = pose.get('name', '')
        verts = []
        for poff in pose.findall('poseoffset'):
            idx = int(poff.get('index'))
            x   = float(poff.get('x', 0))
            y   = float(poff.get('y', 0))
            z   = float(poff.get('z', 0))
            verts.append([idx, x, y, z])
        poses.append({'name': name, 'verts': verts})
    return poses


def main():
    if not os.path.exists(KENSHI_MESH):
        print(f"[A] Kenshi mesh not found: {KENSHI_MESH}")
        print("    Provide path as argument or install Kenshi.")
        return

    with tempfile.NamedTemporaryFile(suffix='.mesh.xml', delete=False) as tf:
        xml_path = tf.name

    try:
        print(f"[A] Converting {os.path.basename(KENSHI_MESH)} → XML ...")
        if not mesh_to_xml(KENSHI_MESH, xml_path):
            return

        print(f"[A] Parsing poses from XML ...")
        poses = parse_poses_xml(xml_path)
    finally:
        if os.path.exists(xml_path):
            os.unlink(xml_path)

    if not poses:
        print("[A] No poses found.")
        return

    print(f"[A] Found {len(poses)} poses:")
    for p in poses:
        print(f"    {p['name']:40s}  {len(p['verts'])} verts")

    os.makedirs("game/data/chars", exist_ok=True)

    with open(OUT_NAMES, 'w') as f:
        for p in poses:
            f.write(p['name'] + '\n')
    print(f"[A] Wrote {OUT_NAMES}")

    with open(OUT_JSON, 'w') as f:
        json.dump(poses, f, separators=(',', ':'))
    print(f"[A] Wrote {OUT_JSON}  ({len(poses)} morphs)")
    print("[A] Next: EditorCharPanel::Get().LoadMorphNames(\"game/data/chars/morph_names.txt\")")


if __name__ == '__main__':
    main()
