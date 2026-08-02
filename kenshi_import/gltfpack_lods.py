#!/usr/bin/env python3
"""gltfpack_lods — ASSET_PIPELINE_MASTER_PROMPT.md Phase 4: LOD chain
generation via gltfpack, run over a directory of already-converted GLBs
(blender_convert.py's output).

STOP-GATE status (see tools/kenshi_import/CONVENTIONS.md "Фаза 4" for
full findings): EXT_meshopt_compression (-c/-cc/-cz) is confirmed
UNsupported (no meshoptimizer decoder linked in engine/) -- never
passed here. KHR_mesh_quantization (gltfpack's default, no -noq) was
initially broken (PropMesh never applied glTF node transforms) but is
now fixed (monkey_dust_engine#31) and left ON (that's most of
gltfpack's real size benefit on geometry).

LOD0 = quantization only, no simplification (-si 1, effectively a
no-op ratio but keep the call uniform).
LOD1 = -si 0.5, LOD2 = -si 0.2, LOD3 = -si 0.05 (master prompt's own
suggested ratios) -- NOT independently tuned per mesh in this pass;
visual acceptability of LOD2/LOD3 is NOT verified here (only LOD0 is,
against Phase 2/3's existing verified un-optimized output) --
disclosed, not silently assumed.

Usage:
  python3 tools/kenshi_import/gltfpack_lods.py --in-dir DIR --out-dir DIR [--report FILE]
"""
import argparse
import glob
import json
import os
import re
import subprocess
import time

LOD_RATIOS = [
    ("lod0", None),   # quantization only
    ("lod1", 0.5),
    ("lod2", 0.2),
    ("lod3", 0.05),
]


def run_gltfpack(src, dst, si_ratio):
    cmd = ["gltfpack", "-i", src, "-o", dst, "-v"]
    if si_ratio is not None:
        cmd += ["-si", str(si_ratio)]
    t0 = time.monotonic()
    r = subprocess.run(cmd, capture_output=True, text=True)
    dt = time.monotonic() - t0
    return r.returncode == 0, r.stdout + r.stderr, dt


def parse_triangle_count(gltfpack_output):
    # gltfpack -v prints "output: N mesh primitives (T triangles, V vertices)"
    m = re.search(r'output:.*?\((\d+) triangles', gltfpack_output)
    return int(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--in-dir', required=True)
    ap.add_argument('--out-dir', required=True)
    ap.add_argument('--report', default=None)
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    srcs = sorted(glob.glob(os.path.join(args.in_dir, '*.glb')))
    print(f'{len(srcs)} source GLBs')

    entries = []
    ok, failed = 0, 0
    for src in srcs:
        name = os.path.splitext(os.path.basename(src))[0]
        src_bytes = os.path.getsize(src)
        entry = {"name": name, "src_bytes": src_bytes, "lods": {}}
        all_ok = True
        for lod_name, si in LOD_RATIOS:
            dst = os.path.join(args.out_dir, f'{name}.{lod_name}.glb')
            success, out, dt = run_gltfpack(src, dst, si)
            if not success:
                print(f'  [FAIL {lod_name}] {name}: {out}')
                all_ok = False
                continue
            tri = parse_triangle_count(out)
            dst_bytes = os.path.getsize(dst) if os.path.isfile(dst) else 0
            entry["lods"][lod_name] = {
                "triangles": tri, "bytes": dst_bytes, "seconds": round(dt, 2),
            }
        entries.append(entry)
        if all_ok:
            ok += 1
            print(f'[OK] {name}: ' + ', '.join(
                f'{k}={v["triangles"]}tri/{v["bytes"]}B' for k, v in entry["lods"].items()))
        else:
            failed += 1

    print(f'[SUMMARY] ok={ok} failed={failed} total={len(srcs)}')
    if args.report:
        with open(args.report, 'w', encoding='utf-8') as f:
            json.dump({"ok": ok, "failed": failed, "total": len(srcs), "entries": entries},
                       f, indent=2)
        print(f'[REPORT] wrote {args.report}')


if __name__ == '__main__':
    main()
