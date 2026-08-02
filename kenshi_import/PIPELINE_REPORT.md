# PIPELINE_REPORT.md — Kenshi `.mesh` → GLB, ASSET_PIPELINE_MASTER_PROMPT.md

Blender 5.2.0 (native) + `Kenshi_IO_Continued` (2 compat patches) +
`OgreXMLConverter` → GLB. Real per-mesh textures via a same-directory
diffuse/normal heuristic (`find_textures.py`), 0.1 world-scale bake,
gltfpack LOD chains. Two real engine gaps found and fixed along the
way: `PropMesh` didn't apply glTF node transforms (broke gltfpack
quantization) and never read embedded GLB materials at all (both
`monkey_dust_engine#31`/`#32`).

## Numbers (200-mesh "moor" building kit)

- Conversion: 200/200 (100%), 152s total, 27.9MB `.mesh` → 28.0MB `.glb`
- Texture hit rate: 9.5% (19/200) — most sub-pieces of a modular kit
  don't have their own co-located texture; disclosed, not extrapolated
- LOD chain (gltfpack): 200/200, quantization gives the main win
  (28.04MB → 21.04MB, ~25%); `-si` simplification cuts triangles ~23%
  further but bytes only ~4% more (dataset already near geometric
  minimum under the default `-se 0.01` error limit)
- Collision: 9/200 (4.5%) have authored collision, in NVIDIA PhysX
  2.8.5 NxuStream2 XML (not OGRE) — 22 box shapes vs 1 convex hull vs
  0 trimesh in 6 spot-checked files

## Known limitations

- Skeletal meshes: out of scope this pass (static props/buildings only)
- 190/200 moor meshes render untextured (`StaticObject` gray fallback)
  — real texture only found for meshes with a co-located `_DIF`/`_NML`
  pair; no cross-folder resolution attempted after one that was tried
  produced mismatched diffuse/normal pairs (documented, reverted)
- LOD1-3 visual acceptability not independently verified — only LOD0
  geometric parity against the un-optimized source was checked live
- `EXT_meshopt_compression` unsupported (no decoder in engine) — never
  enabled
- 95.5% of props have no authored collision; no fallback proposed yet

## Reproduce (one city)

```bash
blender -b --python tools/kenshi_import/blender_convert.py -- \
  --manifest tools/kenshi_import/phase3_moor_manifest.txt \
  --kenshi-dir tmp_/kenshi/data --out-dir OUT_DIR \
  --xml-converter /usr/bin/OgreXMLConverter --report report.json

python3 tools/kenshi_import/gltfpack_lods.py \
  --in-dir OUT_DIR --out-dir LOD_OUT_DIR --report lod_report.json
```

Full findings, patches, and reasoning: `CONVENTIONS.md`/`PROGRESS.md` in
this directory.
