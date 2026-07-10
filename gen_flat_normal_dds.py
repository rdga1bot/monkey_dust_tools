#!/usr/bin/env python3
"""Generate a synthetic flat/neutral BC1 (DXT1) normal-map DDS, matching the
dimensions/mip-count of Kenshi's real terrain _NML.dds files, for the two
GroundTexLayer slots (K_VOLC_FLOOR, K_SWAMP_MUD) whose diffuse texture has no
matching _NML.dds in the extracted asset set. Every BC1 block encodes the
same solid colour (128,128,255) -- a neutral tangent-space normal (0,0,1).

Run from repo root: python3 tools/gen_flat_normal_dds.py
"""

SRC = "tmp_/kenshi_re/terrain_textures/Flat_Land_NML.dds"
DST = "tmp_/kenshi_re/terrain_textures/_MD_Flat_Normal_NML.dds"

# BC1 solid-colour block: color0 == color1 (R5G6B5 of 128,128,255), so the
# 2-bit per-texel indices don't matter -- decoded colour is uniform either way.
FLAT_BLOCK = bytes([0x1F, 0x84, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00])

if __name__ == "__main__":
    data = open(SRC, "rb").read()
    header = data[:128]
    payload_len = len(data) - 128
    assert payload_len % 8 == 0, payload_len
    payload = FLAT_BLOCK * (payload_len // 8)
    with open(DST, "wb") as f:
        f.write(header)
        f.write(payload)
    print(f"wrote {DST}: {len(header) + len(payload)} bytes")
