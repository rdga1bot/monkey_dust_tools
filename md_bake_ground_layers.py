#!/usr/bin/env python3
"""md_bake_ground_layers.py — offline bake of the terrain's FLAT-GROUND
(base/slope/grass/dirt/road) blend into one RGB colour texture, replacing
the live per-pixel 6-layer blend in shaders/terrain_patch.frag's
SampleZoneGroundColor for everything except the cliff channel.

Why baked, not live: purely a performance choice (task #298 measured the
live 4-zone-corner blend as the single largest fragment cost); the BLEND
FORMULA below is an ordinary sequential lerp chain, matching real Kenshi
exactly (see next paragraph) -- baking just moves this same computation
offline, once, instead of doing it every frame per pixel.

The blend formula itself is now a direct, verified port of real Kenshi's
own shipped HLSL source (tmp_/kenshi_re/materials/deferred/terrainfp4.hlsl's
computeBiome(), terrain.hlsl's main_vs) -- NOT a decompile, the actual
shader code. It is a plain sequential lerp chain (base->grass->slope->
dirt->road), each layer at its OWN real per-biome tiling scale (FCS
"tiling X/Y <layer>" fields, extracted by private/md_gen_biome_table.py)
on top of a shared world/5000 base coordinate. An earlier version of this
file used dominant-weight/argmax selection instead, believing that was
Kenshi's real scheme (re_docs/kenshi/terrain.md Subsystem 4) -- reading
the actual shader source this session showed that belief was wrong: real
Kenshi lerps. The "muddy wash" that motivated the argmax detour came from
THIS PROJECT'S own missing details (one global UV scale for every layer +
biome, no per-biome slope thresholds), not from the lerp-chain shape
itself -- both are now fixed via the real per-biome tiling/slope-band
data instead of changing the blend algorithm.

Cliff triplanar blending is explicitly NOT part of this bake — real
Kenshi's own architecture keeps it separate ("NOT the dominant-weight
vertex scheme used for flat ground -- a separate, geometry-driven blend
specific to steep faces", same doc). A top-down 2D bake cannot represent
a vertical cliff face's texture anyway. terrain_patch.frag's existing
live cliff_w/cCliff computation is unchanged; only the flat-ground
portion of BlendGroundLayers (base/slope/grass/dirt/road) is replaced.

Approach (vectorised per-zone, not per-pixel): the SAME zone-corner
bilinear blend the live shader does (zoneCoord = world/CHUNK_SIZE_M -
0.5, 4-corner mix) is decomposed into a weighted splat/accumulate: for
each of the 4096 world zones, compute this zone's own flat-ground colour
across the ~2x2-zone-wide output region where it can act as ANY of the
four corners, weighted by that corner's own bilinear factor, and
accumulate. This keeps every per-zone iteration a simple, fully-
vectorised numpy op (no per-pixel Python loop, no per-pixel fancy-
indexed texture-array gather).

Usage:
  python3 tools/md_bake_ground_layers.py [--out-size 8192] [--test-crop N]

--test-crop N: only bake a small NxN region (world units, centred quarter
of the map) at low output resolution, for fast visual-sanity iteration
before committing to the full 8192x8192 bake (task plan step 1a).
"""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
from md_terrain_erode import load_atlas, FULL_SIZE  # 8193x8193 continuous height field, metres

CHUNK_SIZE_M   = 460.8
ATLAS_ZONES    = 64
BIOME_TABLE    = "game/data/biome_table.txt"
BIOMEMAP_PNG   = "game/data/textures/md_biomemap.png"
OVERLAY_MASK   = "game/data/textures/md_overlay_mask.png"
WORLD_HMAP     = "game/data/terrain/world_hmap.r16"
WORLD_EXTENT   = ATLAS_ZONES * CHUNK_SIZE_M  # 29491.2 m
GROUND_TEX_DIR = "tmp_/kenshi_re/terrain_textures"
CACHE_DIR      = "tmp_/ground_tex_cache"
DDS_UV_SCALE   = 1.0 / 5000.0  # matches shaders/terrain_patch.frag

# Same thresholds as shaders/terrain_patch.frag
SLOPE_MIN, SLOPE_MAX, SLOPE_BLEND = 0.15, 0.55, 0.12
CLIFF_MIN, CLIFF_MAX, CLIFF_BLEND = 0.50, 1.00, 0.15


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def parse_biome_table(path):
    tex_paths = {}
    biomes = []  # list of dict(slug, base, slope, cliff, grass, dirt, road, legend_rgb)
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            if line.startswith("tex "):
                rest = line[4:]
                idx_str, dif, _nml = rest.split("|", 2)
                tex_paths[int(idx_str)] = dif
            elif line.startswith("biome "):
                parts = line[6:].split()
                if len(parts) < 16:
                    continue
                slug = parts[0]
                base, slope, cliff, grass, dirt, road = (int(x) for x in parts[1:7])
                legend_rgb = tuple(int(x) for x in parts[13:16])
                # Real per-biome tiling scale + slope/cliff thresholds appended
                # by private/md_gen_biome_table.py after the original 16
                # fields (confirmed against terrainfp4.hlsl -- see that
                # script's own comment). Missing (older biome_table.txt
                # without this session's regen) -> this project's prior
                # hardcoded constants, so nothing silently breaks.
                if len(parts) >= 34:
                    nums = [float(x) for x in parts[16:34]]
                    tiling = dict(base=(nums[0], nums[1]), slope=(nums[2], nums[3]),
                                  cliff=(nums[4], nums[5]), dirt=(nums[6], nums[7]),
                                  grass=(nums[8], nums[9]), road=(nums[10], nums[11]))
                    slope_band = (nums[12], nums[13], nums[14])
                    cliff_band = (nums[15], nums[16], nums[17])
                else:
                    tiling = dict(base=(1.0, 1.0), slope=(1.0, 1.0), cliff=(1.0, 1.0),
                                  dirt=(1.0, 1.0), grass=(1.0, 1.0), road=(1.0, 1.0))
                    slope_band = (SLOPE_MIN, SLOPE_MAX, SLOPE_BLEND)
                    cliff_band = (CLIFF_MIN, CLIFF_MAX, CLIFF_BLEND)
                biomes.append(dict(slug=slug, base=base, slope=slope, cliff=cliff,
                                    grass=grass, dirt=dirt, road=road, legend_rgb=legend_rgb,
                                    tiling=tiling, slope_band=slope_band, cliff_band=cliff_band))
    return tex_paths, biomes


def resolve_zone_biomes(biomemap_png, biomes):
    """Returns (64,64) array of biome-list indices, one per world zone,
    matching engine/src/world/terrain_gen.cpp's s_resolve_biome: majority
    colour within the zone's own block of the biomemap, nearest-legend
    match (squared RGB distance)."""
    bm = np.array(Image.open(biomemap_png).convert("RGB"))
    h, w = bm.shape[0], bm.shape[1]
    legends = np.array([b["legend_rgb"] for b in biomes], dtype=np.int32)  # (75,3)

    zone_biome_idx = np.zeros((ATLAS_ZONES, ATLAS_ZONES), dtype=np.int32)
    for zy in range(ATLAS_ZONES):
        y0 = int(zy * h / ATLAS_ZONES); y1 = int((zy + 1) * h / ATLAS_ZONES)
        y1 = max(y1, y0 + 1)
        for zx in range(ATLAS_ZONES):
            x0 = int(zx * w / ATLAS_ZONES); x1 = int((zx + 1) * w / ATLAS_ZONES)
            x1 = max(x1, x0 + 1)
            block = bm[y0:y1, x0:x1].reshape(-1, 3)
            colours, counts = np.unique(block, axis=0, return_counts=True)
            majority = colours[np.argmax(counts)].astype(np.int32)
            d2 = np.sum((legends - majority) ** 2, axis=1)
            zone_biome_idx[zy, zx] = int(np.argmin(d2))
    return zone_biome_idx


def load_ground_tex(dds_path):
    """DDS -> cached PNG -> (H,W,3) float32 [0,1] numpy array. Uses
    ImageMagick (only available DDS decoder in this environment, per
    tools/md_stitch_terrain.py's own doc comment)."""
    base = os.path.splitext(os.path.basename(dds_path))[0]
    cache_path = os.path.join(CACHE_DIR, base + ".png")
    if not os.path.exists(cache_path):
        os.makedirs(CACHE_DIR, exist_ok=True)
        subprocess.run(["magick", "convert", dds_path, cache_path], check=True,
                        capture_output=True)
    img = np.array(Image.open(cache_path).convert("RGB"), dtype=np.float32) / 255.0
    return img


def _downsample_box2x2(tex):
    """2x2 box-filter downsample, float32 RGB. Same correct box-filter
    approach as tools/md_bc3_encode.py's _downsample_box (verified
    mathematically sound this session while investigating a DIFFERENT
    hypothesis) -- pads odd dims by edge-replication first."""
    h, w = tex.shape[0], tex.shape[1]
    if h % 2: tex = np.concatenate([tex, tex[-1:]], axis=0); h += 1
    if w % 2: tex = np.concatenate([tex, tex[:, -1:]], axis=1); w += 1
    return tex.reshape(h // 2, 2, w // 2, 2, -1).mean(axis=(1, 3))


def build_mip_chain(tex, min_size=4):
    """[mip0 (full-res), mip1 (half), mip2, ...] down to >= min_size."""
    chain = [tex]
    cur = tex
    while min(cur.shape[0], cur.shape[1]) > min_size:
        cur = _downsample_box2x2(cur)
        chain.append(cur)
    return chain


def pick_prefiltered_mip(mip_chain, target_res):
    """Nearest mip whose resolution is >= target_res (never sharper than
    what the bake's own sampling rate can actually resolve)."""
    best = mip_chain[-1]
    for m in mip_chain:
        best = m
        if m.shape[0] <= target_res:
            break
    return best


def sample_bilinear_wrap(tex, u, v):
    """tex: (H,W,3) float32. u,v: same-shape float arrays, any range
    (wrapped, GL_REPEAT). Bilinear, matches GPU LINEAR+REPEAT sampling."""
    h, w = tex.shape[0], tex.shape[1]
    fu = (u % 1.0) * w - 0.5
    fv = (v % 1.0) * h - 0.5
    x0 = np.floor(fu).astype(np.int64); x1 = x0 + 1
    y0 = np.floor(fv).astype(np.int64); y1 = y0 + 1
    tx = fu - x0; ty = fv - y0
    x0m = x0 % w; x1m = x1 % w
    y0m = y0 % h; y1m = y1 % h
    c00 = tex[y0m, x0m]; c10 = tex[y0m, x1m]
    c01 = tex[y1m, x0m]; c11 = tex[y1m, x1m]
    tx = tx[..., None]; ty = ty[..., None]
    top = c00 * (1 - tx) + c10 * tx
    bot = c01 * (1 - tx) + c11 * tx
    return top * (1 - ty) + bot * ty


def sample_bilinear_clamp(tex, u, v):
    """tex: (H,W,C) float32. u,v: same-shape float arrays in [0,1].
    Bilinear, matches GPU LINEAR+CLAMP_TO_EDGE sampling (used for
    tex_overlay_mask, InitOverlayMask in terrain_renderer.cpp)."""
    h, w = tex.shape[0], tex.shape[1]
    fu = np.clip(u, 0.0, 1.0) * w - 0.5
    fv = np.clip(v, 0.0, 1.0) * h - 0.5
    x0 = np.floor(fu).astype(np.int64); x1 = x0 + 1
    y0 = np.floor(fv).astype(np.int64); y1 = y0 + 1
    tx = fu - x0; ty = fv - y0
    x0m = np.clip(x0, 0, w - 1); x1m = np.clip(x1, 0, w - 1)
    y0m = np.clip(y0, 0, h - 1); y1m = np.clip(y1, 0, h - 1)
    c00 = tex[y0m, x0m]; c10 = tex[y0m, x1m]
    c01 = tex[y1m, x0m]; c11 = tex[y1m, x1m]
    tx = tx[..., None]; ty = ty[..., None]
    top = c00 * (1 - tx) + c10 * tx
    bot = c01 * (1 - tx) + c11 * tx
    return top * (1 - ty) + bot * ty


def box_blur_2d(a, r):
    """Separable box blur, edge-padded, via cumulative sums (O(N), not
    O(N*r)) -- (2r+1)x(2r+1) window average."""
    k = 2 * r + 1
    ap_ = np.pad(a, r, mode="edge")
    c = np.cumsum(ap_, axis=0)
    c = np.vstack([np.zeros((1, c.shape[1])), c])
    s = c[k:] - c[:-k]
    c2 = np.cumsum(s, axis=1)
    c2 = np.hstack([np.zeros((c2.shape[0], 1)), c2])
    s2 = c2[:, k:] - c2[:, :-k]
    return s2 / (k * k)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-size", type=int, default=8192)
    ap.add_argument("--test-crop", type=float, default=0.0,
                     help="world-units size of a centred test crop (0 = full map)")
    ap.add_argument("--out", default="tmp_/ground_bake_test.png")
    args = ap.parse_args()

    print("Loading biome_table.txt / biomemap / overlay mask / heightmap...")
    tex_paths, biomes = parse_biome_table(BIOME_TABLE)
    zone_biome_idx = resolve_zone_biomes(BIOMEMAP_PNG, biomes)
    overlay_mask = np.array(Image.open(OVERLAY_MASK).convert("RGBA"), dtype=np.float32) / 255.0
    height = load_atlas(WORLD_HMAP)  # (FULL_SIZE, FULL_SIZE) metres, FULL_SIZE=8193

    # task terrain-bake-ribbing (2026-07-31): the "vein" ribbing turned out
    # to live in neither ground texture (both individually clean once
    # prefiltered -- see get_prefiltered_tex above) but in slope_w ITSELF:
    # steepness is a per-texel central-difference on the RAW heightmap
    # (3.6m/sample -- genuine, fine erosion-channel-scale relief in the
    # real Kenshi data), and this biome's slope threshold is low/narrow
    # enough (e.g. floodwastes_2's smin=0.08) that almost any small ripple
    # flips slope_w between ~0 and ~1, tracing that fine relief as sharp
    # branching bands wherever cBase/cSlope differ in tone. Confirmed
    # directly: isolating cBase alone (slope_w forced to 0) was completely
    # clean; the slope_w mask alone reproduced the exact vein pattern.
    # Real Kenshi's live shader computes its slope weight from a much
    # coarser PER-VERTEX mesh normal (tens of metres between vertices),
    # never resolving this fine a signal -- our per-texel heightmap-derived
    # steepness is higher-frequency than any real mesh could ever be, so it
    # needs deliberate smoothing to represent the same "generally sloped or
    # not" classification instead of tracing every small bump. Blur radius
    # 16 texels (~58m) picked empirically (tmp_/diag_slope_ribbing.py-style
    # A/B: r=8 still showed the pattern faintly, r=16 removed it while
    # keeping real ridge/cliff-edge transitions intact). GEOMETRY (the
    # actual heightmap used elsewhere) is untouched -- this smoothed copy
    # is used ONLY for the slope_w material-blend classification below.
    height_smooth = box_blur_2d(height, 16)

    out_size = args.out_size
    if args.test_crop > 0:
        crop = args.test_crop
        origin_x = (WORLD_EXTENT - crop) * 0.5
        origin_z = (WORLD_EXTENT - crop) * 0.5
        extent = crop
    else:
        origin_x = 0.0
        origin_z = 0.0
        extent = WORLD_EXTENT

    print(f"Output canvas: {out_size}x{out_size}, world region "
          f"[{origin_x:.1f},{origin_x+extent:.1f}) x [{origin_z:.1f},{origin_z+extent:.1f})")

    accum = np.zeros((out_size, out_size, 3), dtype=np.float32)
    wsum  = np.zeros((out_size, out_size, 1), dtype=np.float32)

    tex_cache = {}
    def get_tex(idx):
        if idx not in tex_cache:
            tex_cache[idx] = load_ground_tex(tex_paths[idx])
        return tex_cache[idx]

    # task terrain-bake-aliasing (2026-07-31): root-caused the "vertical
    # ribbing" seen in-game on sloped terrain -- NOT the shader, NOT the
    # colour overlay (both suspected and ruled out earlier this session) --
    # to THIS bake sampling raw ground textures with a straight
    # sample_bilinear_wrap (a 4-texel point sample, no area/mip filtering)
    # at a tiling frequency far higher than the bake's own output
    # resolution can resolve. Confirmed directly: isolating the baked
    # colour alone (fragColor=cFlatBaked in the shader) reproduced the
    # ribbing exactly, and the desert biome's real "tex_base" asset
    # (Sand01bc.dds) has genuine fine vertical grain that aliases into
    # exaggerated Moire bands once undersampled this way. Real Kenshi never
    # hits this because it samples ground textures live, every frame, with
    # real GPU mip/anisotropic filtering -- our OFFLINE bake has to build
    # its own equivalent mip chain and sample from the correctly-filtered
    # level instead of always the full-res source.
    meters_per_texel = extent / out_size
    mip_cache = {}
    def get_prefiltered_tex(idx, tiling_x):
        repeat_span_m = 5000.0 / max(tiling_x, 1e-6)
        samples_per_repeat = repeat_span_m / meters_per_texel
        target_res = max(4, samples_per_repeat)
        key = (idx, int(round(np.log2(max(target_res, 1.0)))))
        if key not in mip_cache:
            chain = build_mip_chain(get_tex(idx))
            mip_cache[key] = pick_prefiltered_mip(chain, target_res)
        return mip_cache[key]

    # texel->world coordinate helpers
    def world_to_texel(wx, wz):
        tx = (wx - origin_x) / extent * out_size
        ty = (wz - origin_z) / extent * out_size
        return tx, ty

    # Only iterate zones that can possibly touch the output canvas.
    zx_min = max(0, int((origin_x - CHUNK_SIZE_M) / CHUNK_SIZE_M) - 1)
    zx_max = min(ATLAS_ZONES - 1, int((origin_x + extent + CHUNK_SIZE_M) / CHUNK_SIZE_M) + 1)
    zz_min = max(0, int((origin_z - CHUNK_SIZE_M) / CHUNK_SIZE_M) - 1)
    zz_max = min(ATLAS_ZONES - 1, int((origin_z + extent + CHUNK_SIZE_M) / CHUNK_SIZE_M) + 1)
    n_zones = (zx_max - zx_min + 1) * (zz_max - zz_min + 1)
    print(f"Processing zones zx[{zx_min}..{zx_max}] zz[{zz_min}..{zz_max}] ({n_zones} total)...")

    done = 0
    for zy in range(zz_min, zz_max + 1):
        for zx in range(zx_min, zx_max + 1):
            b = biomes[zone_biome_idx[zy, zx]]

            # World-space region where THIS zone can act as any of the 4
            # corners of the shader's zoneCoord=world/CHUNK_SIZE_M-0.5
            # bilinear blend: zone (zx,zy) is a corner for any zoneCoord
            # in [zx-1, zx+1) x [zy-1, zy+1), i.e. world in
            # [(zx-0.5)*CHUNK_SIZE_M, (zx+1.5)*CHUNK_SIZE_M) per axis.
            wx0 = (zx - 0.5) * CHUNK_SIZE_M
            wx1 = (zx + 1.5) * CHUNK_SIZE_M
            wz0 = (zy - 0.5) * CHUNK_SIZE_M
            wz1 = (zy + 1.5) * CHUNK_SIZE_M
            tx0, tz0 = world_to_texel(wx0, wz0)
            tx1, tz1 = world_to_texel(wx1, wz1)
            ix0, ix1 = max(0, int(np.floor(tx0))), min(out_size, int(np.ceil(tx1)))
            iz0, iz1 = max(0, int(np.floor(tz0))), min(out_size, int(np.ceil(tz1)))
            if ix1 <= ix0 or iz1 <= iz0:
                continue

            # Per-texel world position for this region
            tex_x = np.arange(ix0, ix1)
            tex_z = np.arange(iz0, iz1)
            wx = origin_x + (tex_x + 0.5) / out_size * extent          # (W,)
            wz = origin_z + (tex_z + 0.5) / out_size * extent          # (H,)
            WX, WZ = np.meshgrid(wx, wz)                                # (H,W)

            # Bilinear zone-corner weight: how much THIS zone (as a
            # corner) contributes at each texel, matching the shader's
            # zoneFrac mix exactly (triangular tent function, width 1
            # zone, peak at this zone's own centre-ish region).
            zc_x = WX / CHUNK_SIZE_M - 0.5
            zc_z = WZ / CHUNK_SIZE_M - 0.5
            wgt_x = np.clip(1.0 - np.abs(zc_x - zx), 0.0, 1.0)
            wgt_z = np.clip(1.0 - np.abs(zc_z - zy), 0.0, 1.0)
            weight = (wgt_x * wgt_z)[..., None]  # (H,W,1)
            if not np.any(weight > 0.0):
                continue

            # Heightmap-derived steepness (central difference on the
            # SMOOTHED height field -- see height_smooth's doc comment
            # above for why the raw per-texel field aliased into visible
            # ribbing here).
            hx = np.clip(WX / WORLD_EXTENT * (FULL_SIZE - 1), 1, FULL_SIZE - 2).astype(np.int64)
            hz = np.clip(WZ / WORLD_EXTENT * (FULL_SIZE - 1), 1, FULL_SIZE - 2).astype(np.int64)
            h_xp = height_smooth[hz, hx + 1]; h_xn = height_smooth[hz, hx - 1]
            h_zp = height_smooth[hz + 1, hx]; h_zn = height_smooth[hz - 1, hx]
            step_m = WORLD_EXTENT / (FULL_SIZE - 1) * 2.0
            dhdx = (h_xp - h_xn) / step_m
            dhdz = (h_zp - h_zn) / step_m
            n_len = np.sqrt(dhdx * dhdx + dhdz * dhdz + 1.0)
            n_y = 1.0 / n_len
            steepness = 1.0 - n_y

            # Per-biome slope-band threshold (real FCS "slope min/max/fade 1",
            # confirmed against terrainfp4.hlsl's weights.x) -- falls back to
            # the module-level SLOPE_MIN/MAX/BLEND constants for any biome
            # missing the new fields (see parse_biome_table).
            smin, smax, sblend = b["slope_band"]
            slope_w = smoothstep(smin - sblend, smin, steepness) * \
                      smoothstep(smax + sblend, smax, steepness)

            # Overlay mask (grass/dirt/road), sampled at overlay UV. Reuse
            # the SAME world->overlay UV convention as tex_colour: assume
            # overlay covers the full WORLD_EXTENT for this bake pass
            # (matches how the live shader's world_params maps vWorldPos
            # -> overlay_uv over the same world span).
            ou = np.clip(WX / WORLD_EXTENT, 0.0, 1.0)
            ov = np.clip(WZ / WORLD_EXTENT, 0.0, 1.0)
            mask = sample_bilinear_clamp(overlay_mask, ou, ov)  # (H,W,4)
            grass_w = np.maximum(mask[..., 0], mask[..., 1])
            dirt_w  = mask[..., 2]
            road_w  = mask[..., 3]

            # Real per-biome, per-layer tiling scale (terrainfp4.hlsl:
            # texCoords.xy * scales<N>.xy) -- world/5000 is the SHARED base
            # coordinate (terrain.hlsl's main_vs oTex0.xy), each layer then
            # multiplies its OWN tiling scale on top. This project used one
            # global DDS_UV_SCALE for every layer + every biome before this
            # fix -- confirmed wrong against the real shader source.
            tl = b["tiling"]
            u_base,  v_base  = WX * DDS_UV_SCALE * tl["base"][0],  WZ * DDS_UV_SCALE * tl["base"][1]
            u_slope, v_slope = WX * DDS_UV_SCALE * tl["slope"][0], WZ * DDS_UV_SCALE * tl["slope"][1]
            u_grass, v_grass = WX * DDS_UV_SCALE * tl["grass"][0], WZ * DDS_UV_SCALE * tl["grass"][1]
            u_dirt,  v_dirt  = WX * DDS_UV_SCALE * tl["dirt"][0],  WZ * DDS_UV_SCALE * tl["dirt"][1]
            u_road,  v_road  = WX * DDS_UV_SCALE * tl["road"][0],  WZ * DDS_UV_SCALE * tl["road"][1]

            c_base  = sample_bilinear_wrap(get_prefiltered_tex(b["base"],  tl["base"][0]),  u_base,  v_base)
            c_slope = sample_bilinear_wrap(get_prefiltered_tex(b["slope"], tl["slope"][0]), u_slope, v_slope)
            c_grass = sample_bilinear_wrap(get_prefiltered_tex(b["grass"], tl["grass"][0]), u_grass, v_grass)
            c_dirt  = sample_bilinear_wrap(get_prefiltered_tex(b["dirt"],  tl["dirt"][0]),  u_dirt,  v_dirt)
            c_road  = sample_bilinear_wrap(get_prefiltered_tex(b["road"],  tl["road"][0]),  u_road,  v_road)

            # Real Kenshi's flat-ground blend (terrainfp4.hlsl's computeBiome,
            # confirmed against the actual shipped HLSL source, not a
            # decompile) is an ORDINARY SEQUENTIAL LERP CHAIN, not dominant-
            # weight/argmax -- the earlier belief that Kenshi uses per-texel
            # argmax for flat ground (re_docs/kenshi/terrain.md Subsystem 4)
            # was a misreading; this session found the real shader and it
            # lerps straight through:
            #   albedo = lerp(cBase,  cGrass, map.r)
            #   albedo = lerp(albedo, cSlope, weights.x)
            #   albedo = lerp(albedo, cDirt,  map.b)
            #   albedo = lerp(albedo, cRoad,  map.a)
            # (cliff excluded here -- stays live/triplanar, unchanged.) The
            # "muddy" look that originally motivated switching to argmax came
            # from this project's OWN wrong details (one global UV scale for
            # every layer, no per-biome slope thresholds), not from the
            # lerp-chain approach itself -- fixed above via real per-biome
            # tiling scale + slope band, not by changing the blend shape.
            gw = grass_w[..., None]; sw = slope_w[..., None]
            dw = dirt_w[..., None];  rw = road_w[..., None]
            col = c_base * (1.0 - gw) + c_grass * gw
            col = col     * (1.0 - sw) + c_slope * sw
            col = col     * (1.0 - dw) + c_dirt  * dw
            col = col     * (1.0 - rw) + c_road  * rw

            accum[iz0:iz1, ix0:ix1] += col * weight
            wsum[iz0:iz1, ix0:ix1]  += weight

            done += 1
            if done % 200 == 0:
                print(f"  {done}/{n_zones} zones...", end="\r", flush=True)

    print(f"\n{done}/{n_zones} zones processed.")
    wsum_safe = np.maximum(wsum, 1e-6)
    final = np.clip(accum / wsum_safe, 0.0, 1.0)
    covered = wsum[..., 0] > 1e-6
    print(f"Coverage: {100.0*np.mean(covered):.1f}% of canvas has >=1 zone contribution")

    out_img = (final * 255.0).astype(np.uint8)
    Image.fromarray(out_img, "RGB").save(args.out)
    print(f"Saved: {args.out}")


if __name__ == "__main__":
    main()
