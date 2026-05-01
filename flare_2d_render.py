#!/usr/bin/env python3
"""
Flare 2D software renderer — renders a Flare .txt map exactly as the
original Flare engine would: painter's algorithm, isometric screen coords,
per-tile sprite blitting from the tileset atlas.

Usage:
  python3 tools/flare_2d_render.py [map_name] [output.png] [scale]
  python3 tools/flare_2d_render.py goblin_camp /tmp/flare_2d.png
  python3 tools/flare_2d_render.py goblin_camp /tmp/flare_2d.png 0.5
"""

import sys, os, re
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODS = os.path.join(REPO, "third_party/flare-game/mods")
MODS_ORDER = ("empyrean_campaign", "fantasycore", "minicore")

TILE_W = 192   # isometric tile width in atlas pixels
TILE_H = 96    # isometric tile height (half-diamond)

# Painter's algorithm priority — normalise all known name variants.
def layer_prio(ltype):
    t = ltype.lower().replace("_", "")
    if t == "background":              return 0
    if t in ("backgroundfringe", "fringe"): return 1
    if t == "object":                  return 2
    return 1  # unknown layers: between fringe and object

# ── Map loader ────────────────────────────────────────────────────────────────

def load_map(map_name):
    for mod in MODS_ORDER:
        p = os.path.join(MODS, mod, "maps", map_name + ".txt")
        if os.path.exists(p):
            return p, mod
    raise FileNotFoundError(f"Map '{map_name}' not found")

def parse_map(path):
    with open(path) as f:
        text = f.read()

    w = int(re.search(r"width=(\d+)",  text).group(1))
    h = int(re.search(r"height=(\d+)", text).group(1))

    tsd_match = re.search(r"tileset=(tilesetdefs/[^\n,]+\.txt)", text)
    tsd_rel   = tsd_match.group(1) if tsd_match else None

    layers = []
    for block in re.split(r"\[layer\]", text)[1:]:
        ltype_m = re.search(r"type=(\w+)", block)
        data_m  = re.search(r"data=\n([\d,\n]+)", block)
        if not ltype_m or not data_m:
            continue
        ltype = ltype_m.group(1)
        if ltype in ("collision", "event", "enemy", "npc"):
            continue
        rows = []
        for line in data_m.group(1).strip().split("\n"):
            line = line.strip().rstrip(",")
            if line:
                rows.append([int(x) for x in line.split(",")])
        layers.append({"type": ltype, "tiles": rows})

    return w, h, tsd_rel, layers

def parse_tilesetdef(tsd_rel, _map_mod):
    tsd_path = tsd_mod = None
    for mod in MODS_ORDER:
        p = os.path.join(MODS, mod, tsd_rel)
        if os.path.exists(p):
            tsd_path, tsd_mod = p, mod
            break
    if tsd_path is None:
        print(f"  [warn] tilesetdef not found: {tsd_rel}")
        return {}, {}

    print(f"  tilesetdef → {tsd_path}")
    metas   = {}
    atlases = {}
    cur_idx = -1

    with open(tsd_path) as f:
        for line in f:
            line = line.strip()
            if line == "[tileset]":
                cur_idx += 1
            elif line.startswith("img="):
                img_path = os.path.join(MODS, tsd_mod, line[4:])
                if os.path.exists(img_path):
                    img = Image.open(img_path).convert("RGBA")
                    atlases[cur_idx] = img
                    print(f"  atlas[{cur_idx}] = {line[4:]}  ({img.width}×{img.height})")
                else:
                    print(f"  [warn] atlas not found: {img_path}")
            elif line.startswith("tile="):
                parts = line[5:].split(",")
                if len(parts) >= 7:
                    tid, sx, sy, tw, th, ox, oy = [int(x) for x in parts[:7]]
                    metas[tid] = {
                        "src_x": sx, "src_y": sy,
                        "w": tw,    "h": th,
                        "offset_x": ox, "offset_y": oy,
                        "atlas": cur_idx,
                    }
    return metas, atlases

# ── 2D Flare screen coordinate formula ───────────────────────────────────────
# Sprite top-left on screen = (screen_x - offset_x, screen_y - offset_y)

def tile_screen_pos(col, row, meta):
    sx = (col - row) * (TILE_W // 2)
    sy = (col + row) * (TILE_H // 2)
    return sx - meta["offset_x"], sy - meta["offset_y"]

# ── Safe blit ─────────────────────────────────────────────────────────────────

def safe_composite(canvas, sprite, dx, dy):
    """alpha_composite with clipping — handles sprites that overlap canvas edge."""
    cw, ch = canvas.size
    sw, sh = sprite.size
    # Clip source rectangle to fit within canvas
    src_x = max(0, -dx);  dst_x = max(0, dx)
    src_y = max(0, -dy);  dst_y = max(0, dy)
    blit_w = min(sw - src_x, cw - dst_x)
    blit_h = min(sh - src_y, ch - dst_y)
    if blit_w <= 0 or blit_h <= 0:
        return
    if src_x or src_y or blit_w < sw or blit_h < sh:
        sprite = sprite.crop((src_x, src_y, src_x + blit_w, src_y + blit_h))
    canvas.alpha_composite(sprite, (dst_x, dst_y))

# ── Main renderer ─────────────────────────────────────────────────────────────

def render(map_name, out_path, scale=1.0):
    map_path, mod = load_map(map_name)
    print(f"Map:  {map_path}")

    mw, mh, tsd_rel, layers = parse_map(map_path)
    print(f"Size: {mw}×{mh},  layers: {[l['type'] for l in layers]}")

    metas, atlases = {}, {}
    if tsd_rel:
        metas, atlases = parse_tilesetdef(tsd_rel, mod)
        print(f"Tilesetdef: {len(metas)} tiles, {len(atlases)} atlases")

    # Top margin: tallest sprite that could be anchored at screen_y=0 (row=0 tiles).
    # offset_y is the distance from sprite top-edge to the tile's ground anchor.
    max_oy    = max((m["offset_y"] for m in metas.values()), default=96)
    top_margin = max_oy + 64   # extra buffer

    canvas_w = (mw + mh) * (TILE_W // 2) + TILE_W
    canvas_h = (mw + mh) * (TILE_H // 2) + top_margin + 256
    origin_x  = mh * (TILE_W // 2)
    origin_y  = top_margin

    # Transparent canvas so auto-trim works correctly.
    canvas = Image.new("RGBA", (canvas_w, canvas_h), (0, 0, 0, 0))

    # Collect + sort all tiles: primary key = depth band, secondary = layer prio.
    tiles = []
    for layer in layers:
        ltype = layer["type"]
        prio  = layer_prio(ltype)
        for row, row_data in enumerate(layer["tiles"]):
            for col, tid in enumerate(row_data):
                if tid == 0:
                    continue
                meta = metas.get(tid)
                if meta is None:
                    continue
                depth = (col + row) * 3 + prio
                tiles.append((depth, prio, col, row, meta))

    tiles.sort(key=lambda t: (t[0], t[1]))
    print(f"Drawing {len(tiles)} tiles...")

    for depth, prio, col, row, meta in tiles:
        atlas = atlases.get(meta["atlas"])
        if atlas is None:
            continue
        sx, sy = meta["src_x"], meta["src_y"]
        sw, sh = meta["w"],     meta["h"]
        if sx + sw > atlas.width or sy + sh > atlas.height:
            continue
        sprite = atlas.crop((sx, sy, sx + sw, sy + sh))
        dx, dy = tile_screen_pos(col, row, meta)
        safe_composite(canvas, sprite, dx + origin_x, dy + origin_y)

    # Auto-trim transparent/empty border.
    bbox = canvas.getbbox()
    if bbox:
        canvas = canvas.crop(bbox)

    # Composite onto dark background.
    bg = Image.new("RGBA", canvas.size, (20, 20, 30, 255))
    bg.alpha_composite(canvas)
    canvas = bg

    if scale != 1.0:
        w2 = max(1, int(canvas.width  * scale))
        h2 = max(1, int(canvas.height * scale))
        canvas = canvas.resize((w2, h2), Image.LANCZOS)

    canvas.save(out_path)
    print(f"Saved → {out_path}  ({canvas.width}×{canvas.height} px)")

if __name__ == "__main__":
    map_name = sys.argv[1] if len(sys.argv) > 1 else "goblin_camp"
    out_path = sys.argv[2] if len(sys.argv) > 2 else "/tmp/flare_2d.png"
    scale    = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
    render(map_name, out_path, scale)
