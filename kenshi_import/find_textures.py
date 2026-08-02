#!/usr/bin/env python3
"""find_textures — folder-heuristic diffuse/normal texture lookup for a
Kenshi .mesh file (ASSET_PIPELINE_MASTER_PROMPT.md Phase 2).

Real per-mesh material resolution in Kenshi's own data is NOT a simple
filename match: every static-object .mesh submesh references the OGRE
material name "StaticObject" directly (verified via OgreXMLConverter
output on wall.mesh, moor01-door.mesh -- both say
`material="StaticObject"`), and StaticObject's own definition
(tmp_/kenshi/data/materials/deferred/objects.material) hardcodes
placeholder textures (black.dds/flat.dds/black.dds) into three named
texture units: diffuseMap, normalMap, metalnessMap. There is no
per-instance override anywhere in the .material scripts themselves --
real per-mesh textures must come from Kenshi's own engine-side asset
lookup, which this project does not have access to.

What DOES work, verified against real samples: individual mesh folders
almost always contain their own *_DIF.dds/*_NML.dds (or bare .dds +
_n.dds) pairs sitting next to the mesh, e.g.
newland/Assets/Plants/Big4Seed_Tree1.mesh next to
Big4Seed_Tree1_DIF.dds + Big4Seed_Tree1_NML.dds. This is a DISCLOSED
HEURISTIC (same spirit as the existing terrain pipeline's blendinfo.dat
approximation, see CLAUDE.md) -- not a reimplementation of Kenshi's
real resolution logic, which isn't recoverable from this data alone.
Coverage is measured, not assumed: some folders (e.g.
buildings/misc/'s Split-Rail_Fence*.mesh) have zero texture files at
all and are left with StaticObject's flat placeholder.
"""
import difflib
import os
import re

MIN_SIMILARITY = 0.25  # below this, a same-folder candidate is more likely
                        # an unrelated shared texture than this mesh's own
                        # (measured false-positive: Split-Rail_Fence01.mesh
                        # would otherwise match buildings/misc/watertower.dds
                        # at 0.23 -- real name similarity for genuine pairs
                        # in this dataset is consistently >=0.38)

NORMAL_SUFFIXES = ('_n', '_nml', '_nrm', '_normal')
NON_DIFFUSE_INFIXES = ('_nml', '_nrm', '_normal', '_spec', '_gloss', '_ao',
                        '_rough', '_metal', '_mask', '_alpha')


def _is_normal_map(fname):
    stem = os.path.splitext(fname)[0].lower()
    return any(stem.endswith(suf) for suf in NORMAL_SUFFIXES)


def _is_diffuse_candidate(fname):
    stem = os.path.splitext(fname)[0].lower()
    if _is_normal_map(fname):
        return False
    return not any(infix in stem for infix in NON_DIFFUSE_INFIXES)


def _strip_normal_suffix(stem_lower):
    for suf in NORMAL_SUFFIXES:
        if stem_lower.endswith(suf):
            return stem_lower[:-len(suf)]
    return stem_lower


def find_textures_for_mesh(mesh_path, extra_dirs=None):
    """Returns (diffuse_abs_path_or_None, normal_abs_path_or_None).

    Searches the mesh's own directory first, then any extra_dirs
    (e.g. a sibling 'materials/' folder) in order. Within a directory,
    prefers the diffuse candidate whose name best matches the mesh's
    own basename (difflib.SequenceMatcher ratio) over an arbitrary
    first match -- folders with multiple unrelated textures (e.g. a
    shared prefab folder) are common.
    """
    mesh_stem = os.path.splitext(os.path.basename(mesh_path))[0].lower()
    mesh_stem_clean = re.sub(r'[^a-z0-9]', '', mesh_stem)

    search_dirs = [os.path.dirname(mesh_path)]
    if extra_dirs:
        search_dirs.extend(extra_dirs)

    diffuse_candidates = []  # (path, name_similarity)
    normal_candidates = []

    for d in search_dirs:
        if not os.path.isdir(d):
            continue
        for fname in os.listdir(d):
            if not fname.lower().endswith(('.dds', '.png', '.tga')):
                continue
            fpath = os.path.join(d, fname)
            stem = os.path.splitext(fname)[0].lower()
            stem_clean = re.sub(r'[^a-z0-9]', '', stem)
            if _is_normal_map(fname):
                base_clean = re.sub(r'[^a-z0-9]', '',
                                     _strip_normal_suffix(stem))
                sim = difflib.SequenceMatcher(None, mesh_stem_clean, base_clean).ratio()
                normal_candidates.append((fpath, sim))
            elif _is_diffuse_candidate(fname):
                # strip a trailing _dif/_diffuse too, for fair comparison
                base = re.sub(r'(_dif|_diffuse)$', '', stem)
                base_clean = re.sub(r'[^a-z0-9]', '', base)
                sim = difflib.SequenceMatcher(None, mesh_stem_clean, base_clean).ratio()
                diffuse_candidates.append((fpath, sim))

    diffuse = None
    if diffuse_candidates:
        best = max(diffuse_candidates, key=lambda t: t[1])
        if best[1] >= MIN_SIMILARITY:
            diffuse = best[0]
    normal = None
    if normal_candidates:
        best = max(normal_candidates, key=lambda t: t[1])
        if best[1] >= MIN_SIMILARITY:
            normal = best[0]
    return diffuse, normal


if __name__ == '__main__':
    import sys
    for p in sys.argv[1:]:
        d, n = find_textures_for_mesh(p)
        print(f'{p}\n  diffuse={d}\n  normal={n}')
