#!/usr/bin/env python3
"""
md_convert_features.py — batch-converts every unique mesh referenced in
game/data/features_scatter.txt (real Kenshi features.dat scatter-object
placement, see private/md_features_parse.py --export) to a .glb under
game/data/props/features/.

All meshes use --scale 0.1 (verified against two real samples: a
FeatureBluff -> ~140m tall, a generic rock -> ~33m wide -- both plausible
at Kenshi's confirmed 1-world-unit=0.1m convention, same as the terrain
heightmap's horizontal/vertical scale). Per-instance (sx,sy,sz) from the
placement file itself is a dimensionless multiplier applied on top at
placement time, NOT part of this conversion.

Usage:
  python3 tools/md_convert_features.py [--kenshi-dir tmp_/kenshi/data]
"""
import argparse
import os
import re
import subprocess
import sys


def sanitize(mesh_path):
    # ".\data\newland\Assets\Rocks\Generic Rock Cog01.mesh" -> "Generic_Rock_Cog01"
    name = mesh_path.replace('\\', '/').split('/')[-1]
    name = re.sub(r'\.mesh$', '', name, flags=re.IGNORECASE)
    name = re.sub(r'[^A-Za-z0-9_.-]', '_', name)
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scatter-file', default='game/data/features_scatter.txt')
    ap.add_argument('--kenshi-dir', default='tmp_/kenshi/data')
    ap.add_argument('--out-dir', default='game/data/props/features')
    ap.add_argument('--tmp-dir', default='tmp_/features_convert_xml')
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    os.makedirs(args.tmp_dir, exist_ok=True)

    meshes = set()
    with open(args.scatter_file, encoding='utf-8') as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            meshes.add(line.split('|')[0])

    print(f'{len(meshes)} unique meshes to convert')
    ok, skipped, failed = 0, 0, 0
    for mesh_path in sorted(meshes):
        name = sanitize(mesh_path)
        out_glb = os.path.join(args.out_dir, name + '.glb')
        if os.path.exists(out_glb):
            ok += 1
            continue

        rel = mesh_path.replace('.\\data\\', '').replace('\\', '/')
        src = os.path.join(args.kenshi_dir, rel)
        if not os.path.exists(src):
            print(f'  [skip] source not found: {src}')
            skipped += 1
            continue

        xml_out = os.path.join(args.tmp_dir, name + '.mesh.xml')
        r = subprocess.run(['OgreXMLConverter', src, xml_out],
                            capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(xml_out):
            print(f'  [FAIL convert] {mesh_path}\n{r.stdout}\n{r.stderr}')
            failed += 1
            continue

        r2 = subprocess.run([sys.executable, 'tools/md_convert_static.py',
                              xml_out, out_glb, '--scale', '0.1'],
                             capture_output=True, text=True)
        if r2.returncode != 0 or not os.path.exists(out_glb):
            print(f'  [FAIL glb] {mesh_path}\n{r2.stdout}\n{r2.stderr}')
            failed += 1
            continue
        ok += 1

    print(f'done: ok={ok} skipped={skipped} failed={failed}')


if __name__ == '__main__':
    main()
