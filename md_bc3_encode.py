#!/usr/bin/env python3
"""md_bc3_encode.py — numpy-vectorized BC3/DXT5 encoder + DDS header writer.

No BC3 encoder tool is available in this environment (checked: nvcompress,
compressonatorcli, texconv all absent; Pillow has no DXT write support) —
this is a from-scratch implementation of the standard, well-documented BC3
block format (see Microsoft's DDS/BC reference), not an experimental
technique. Verified round-trip on a real Kenshi colour tile: mean abs
error 2.7/255, p95 7/255, visually indistinguishable at native zoom
(2026-07-25 test, see task history).

Range-fit endpoint selection (per-block per-channel min/max, not a
PCA/least-squares optimal fit) — standard fast/simple BC1-family technique,
good enough quality for this asset (a photographic aerial overlay, not a
normal map or anything precision-sensitive).

Alpha is hardcoded to a constant fully-opaque block (alpha0=alpha1=255,
zero indices) — valid ONLY because every caller of encode_bc3_with_mips
in this codebase feeds an RGBA array whose alpha channel is always 255
(md_stitch_terrain.py converts RGB->RGBA purely to match GpuTexture's
RGBA8 upload path, never for real transparency). Not a general-purpose
BC3 encoder for arbitrary alpha content.
"""
import struct
import numpy as np


def _pack565(r, g, b):
    r5 = (r.astype(np.uint32) >> 3) & 0x1F
    g6 = (g.astype(np.uint32) >> 2) & 0x3F
    b5 = (b.astype(np.uint32) >> 3) & 0x1F
    return (r5 << 11) | (g6 << 5) | b5


def _unpack565(v):
    v = v.astype(np.uint32)
    r5 = (v >> 11) & 0x1F
    g6 = (v >> 5) & 0x3F
    b5 = v & 0x1F
    r = (r5 * 255 // 31).astype(np.uint8)
    g = (g6 * 255 // 63).astype(np.uint8)
    b = (b5 * 255 // 31).astype(np.uint8)
    return r, g, b


def encode_bc3(rgb, max_rows_per_chunk=1024):
    """rgb: (H,W,3) uint8, H and W multiples of 4. Returns raw BC3 byte block
    stream (16 bytes/block, row-major block order — matches DDS convention
    and engine/src/render/gpu_hal_buffers.cpp's GpuTexture::InitFromDDS
    read order).

    Processes in horizontal row-chunks (max_rows_per_chunk, rounded to a
    multiple of 4) rather than vectorizing the whole image in one pass —
    the full-image version's per-texel x per-candidate distance array
    (H/4 x W/4 x 4 x 4 x 4 candidates x int32) hits ~4.3GB for the real
    16384x16384 atlas, which drove this machine into heavy swapping
    (measured: 4.6GB swap in use, encoder progressing only ~6s of CPU time
    over 4+ minutes wall-clock — a real, observed problem, not a
    theoretical one) rather than the ~65s the per-block cost alone
    predicts. Chunking bounds peak memory to whatever one chunk needs
    regardless of total image size, at the cost of a bit of python-level
    loop overhead between chunks (negligible next to the vectorized work
    inside each chunk)."""
    H, W, _ = rgb.shape
    assert H % 4 == 0 and W % 4 == 0, (H, W)
    rows_per_chunk = max(4, (max_rows_per_chunk // 4) * 4)
    if H <= rows_per_chunk:
        return _encode_bc3_chunk(rgb)
    out = bytearray()
    for y0 in range(0, H, rows_per_chunk):
        y1 = min(y0 + rows_per_chunk, H)
        out += _encode_bc3_chunk(rgb[y0:y1])
    return bytes(out)


def _encode_bc3_chunk(rgb):
    H, W, _ = rgb.shape
    bh, bw = H // 4, W // 4
    blocks = rgb.reshape(bh, 4, bw, 4, 3).transpose(0, 2, 1, 3, 4)  # (bh,bw,4,4,3)
    ch = blocks.astype(np.int16)

    rmin = ch[..., 0].min(axis=(2, 3)); rmax = ch[..., 0].max(axis=(2, 3))
    gmin = ch[..., 1].min(axis=(2, 3)); gmax = ch[..., 1].max(axis=(2, 3))
    bmin = ch[..., 2].min(axis=(2, 3)); bmax = ch[..., 2].max(axis=(2, 3))

    c0_565 = _pack565(rmax.astype(np.uint8), gmax.astype(np.uint8), bmax.astype(np.uint8))
    c1_565 = _pack565(rmin.astype(np.uint8), gmin.astype(np.uint8), bmin.astype(np.uint8))

    swap  = c0_565 <= c1_565
    equal = c0_565 == c1_565
    c0f = np.where(swap, c1_565, c0_565)
    c1f = np.where(swap, c0_565, c1_565)
    c0f = np.where(equal, np.minimum(c1f + 1, 0xFFFF), c0f)

    r0, g0, b0 = _unpack565(c0f)
    r1, g1, b1 = _unpack565(c1f)
    r2 = ((2 * r0.astype(np.int32) + r1) // 3); g2 = ((2 * g0.astype(np.int32) + g1) // 3); b2 = ((2 * b0.astype(np.int32) + b1) // 3)
    r3 = ((r0.astype(np.int32) + 2 * r1) // 3); g3 = ((g0.astype(np.int32) + 2 * g1) // 3); b3 = ((b0.astype(np.int32) + 2 * b1) // 3)

    cand_r = np.stack([r0, r1, r2, r3], axis=-1).astype(np.int16)
    cand_g = np.stack([g0, g1, g2, g3], axis=-1).astype(np.int16)
    cand_b = np.stack([b0, b1, b2, b3], axis=-1).astype(np.int16)

    # (bh,bw,4,4,4cand) arrays are the real memory cost — keep dr/dg/db at
    # int16 (diffs are -255..255, fits) and only promote to int32 for the
    # sum-of-squares itself (255^2*3 would overflow int16).
    px_r, px_g, px_b = ch[..., 0], ch[..., 1], ch[..., 2]
    dr = px_r[..., None] - cand_r[:, :, None, None, :]
    dg = px_g[..., None] - cand_g[:, :, None, None, :]
    db = px_b[..., None] - cand_b[:, :, None, None, :]
    dist = dr.astype(np.int32) ** 2
    dist += dg.astype(np.int32) ** 2
    dist += db.astype(np.int32) ** 2
    idx = dist.argmin(axis=-1).astype(np.uint32)

    idx_flat = idx.reshape(bh, bw, 16)
    packed_idx = np.zeros((bh, bw), dtype=np.uint32)
    for t in range(16):
        packed_idx |= (idx_flat[:, :, t] << (t * 2))

    out = np.zeros((bh, bw, 16), dtype=np.uint8)
    out[..., 0] = 255  # alpha0 (constant-opaque, see module doc comment)
    out[..., 1] = 255  # alpha1
    out[..., 2:8] = 0  # alpha indices (irrelevant when alpha0==alpha1)
    # BC1/BC3 spec: color0 (offset 8-9, the numerically LARGER of the two,
    # per the swap above) MUST come before color1 (offset 10-11) — this is
    # what forces the standard 4-color interpolation mode; the reverse
    # order would make a real GPU/hardware BC3 decoder treat color0<=color1
    # as BC1's 3-color+transparent-black mode instead (index 3 -> black),
    # a real bug caught before shipping via a from-scratch decoder cross-
    # check, not assumed correct just because a self-consistent round-trip
    # test (encoder+matching decoder) passed.
    out[..., 8]  = (c0f & 0xFF).astype(np.uint8)
    out[..., 9]  = ((c0f >> 8) & 0xFF).astype(np.uint8)
    out[..., 10] = (c1f & 0xFF).astype(np.uint8)
    out[..., 11] = ((c1f >> 8) & 0xFF).astype(np.uint8)
    out[..., 12] = (packed_idx & 0xFF).astype(np.uint8)
    out[..., 13] = ((packed_idx >> 8) & 0xFF).astype(np.uint8)
    out[..., 14] = ((packed_idx >> 16) & 0xFF).astype(np.uint8)
    out[..., 15] = ((packed_idx >> 24) & 0xFF).astype(np.uint8)
    return out.tobytes()


def _downsample_box(rgb):
    """2x2 box-filter downsample. Pads odd dimensions by edge-replication
    first (only matters for the last couple of mip levels, e.g. 2x2->1x1)."""
    h, w, _ = rgb.shape
    if h % 2: rgb = np.concatenate([rgb, rgb[-1:]], axis=0); h += 1
    if w % 2: rgb = np.concatenate([rgb, rgb[:, -1:]], axis=1); w += 1
    r = rgb.astype(np.uint16).reshape(h // 2, 2, w // 2, 2, 3).mean(axis=(1, 3))
    return r.astype(np.uint8)


def _pad_to_multiple_of_4(rgb):
    h, w, _ = rgb.shape
    ph = (-h) % 4
    pw = (-w) % 4
    if ph: rgb = np.concatenate([rgb, np.repeat(rgb[-1:], ph, axis=0)], axis=0)
    if pw: rgb = np.concatenate([rgb, np.repeat(rgb[:, -1:], pw, axis=1)], axis=1)
    return rgb


MRGB_DDS_MAGIC_FOURCC_DXT5 = 0x35545844  # 'DXT5' little-endian, matches
# engine's s_parse_dds fourcc check (gpu_hal_buffers.cpp)


def encode_bc3_dds_with_mips(rgb):
    """rgb: (H,W,3) uint8 at the base (mip0) resolution, H==W a power of 2.
    Returns a complete DDS file (header + full BC3 mip chain, base down to
    1x1) as bytes. Mip pyramid built via box-filter downsampling of the
    ORIGINAL rgb data at each level (not re-downsampling already-lossy BC3
    output), matching how a real mipmap chain should be generated."""
    h, w, _ = rgb.shape
    assert h == w and (h & (h - 1)) == 0, "expected square power-of-2 base size"

    levels = []
    cur = rgb
    dim = h
    while True:
        levels.append((dim, dim, cur))
        if dim == 1:
            break
        cur = _downsample_box(cur)
        dim = cur.shape[0]

    payload = bytearray()
    for (lw, lh, ldata) in levels:
        padded = _pad_to_multiple_of_4(ldata)
        payload += encode_bc3(padded)

    mip_count = len(levels)
    DDSD_CAPS_HEIGHT_WIDTH_PIXELFORMAT_LINEARSIZE_MIPMAPCOUNT = 0x1 | 0x2 | 0x4 | 0x80000 | 0x20000
    DDPF_FOURCC = 0x4
    DDSCAPS_TEXTURE = 0x1000
    DDSCAPS_MIPMAP  = 0x400000
    DDSCAPS_COMPLEX = 0x8
    bw0, bh0 = (h + 3) // 4, (w + 3) // 4
    linear_size = bw0 * bh0 * 16  # base-level BC3 byte size

    header = struct.pack(
        '<4s31I', b'DDS ',
        124,
        DDSD_CAPS_HEIGHT_WIDTH_PIXELFORMAT_LINEARSIZE_MIPMAPCOUNT,
        h, w,
        linear_size,
        0,          # dwDepth
        mip_count,
        *([0] * 11),
        32,                 # pixelformat dwSize
        DDPF_FOURCC,        # pixelformat dwFlags
        MRGB_DDS_MAGIC_FOURCC_DXT5,
        0, 0, 0, 0, 0,      # dwRGBBitCount + 4 masks, unused for FOURCC formats
        DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX,
        0, 0, 0,
        0,
    )
    return bytes(header) + bytes(payload)
