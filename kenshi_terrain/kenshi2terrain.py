#!/usr/bin/env python3
"""
kenshi2terrain.py — Extract 1km×1km terrain heightmaps from Kenshi fullmap.tif.

All 33 named Kenshi zones extracted, each saved as game/data/terrain/<name>.r32.
Format: uint32 width + uint32 height + float32 hmin + float32 hmax + float32[w*h] heights.
Heights are mean-centred (mean=0), range [-hmin..hmax] in metres.

Usage:
    python3 kenshi2terrain.py list         — show all zones with terrain relief
    python3 kenshi2terrain.py all          — extract all 33 zones (~143 MB total)
    python3 kenshi2terrain.py <zone_name>  — extract single zone
    python3 kenshi2terrain.py dramatic     — only zones with spread > 300m (13 zones)

★ = dramatic (spread >300m)   ● = moderate (150-300m)   flat = <150m
"""

import sys, os, struct
from PIL import Image

Image.MAX_IMAGE_PIXELS = None

KENSHI_MAP  = "/run/media/rdga1/win/SteamLibrary/steamapps/common/Kenshi/data/newland/land/fullmap.tif"
OUT_DIR     = "game/data/terrain"
EXTRACT_PX  = 512     # 1024m = 512 px at 2m/px
OUT_SIZE    = 1025    # 1025×1025 vertices (TERRAIN_GRID*16 + 1)
H_RANGE_M   = 2100.0  # raw 0..65535 → -100..+2000 m
H_OFFSET_M  = -100.0

# All 33 named Kenshi zones: (zone_x, zone_z) in 0..63 Kenshi grid
# Each Kenshi zone = 256 px at 2m/px = 512m. We extract 512px = 1024m (2×2 zones).
ZONES = {
    # Great wastes — dramatic terrain ★
    "great_desert":       (28, 16),  # 701m spread  ★  flat sandy with cliff bands
    "ashlands":           (24, 12),  # 923m spread  ★  volcanic north
    "the_pits":           (10, 26),  # 635m spread  ★  far west lowlands
    "shem":               (22, 34),  # 473m spread  ★  swampy south hills
    "skinner_roam":       (26, 22),  # 432m spread  ★  central rolling plains
    "cannibal_plains":    (26, 30),  # 430m spread  ★  dangerous plateau
    "leviathan_coast":    (18, 28),  # 369m spread  ★  rugged coast
    "sonorous_dark":      (16, 40),  # 345m spread  ★  dark southern hills
    "fog_islands":        (14, 22),  # 337m spread  ★  foggy western coast
    "border_zone":        (30, 28),  # 332m spread  ★  rolling start region
    "dreg":               (36, 32),  # 317m spread  ★  eastern midlands
    "howler_maze":        (16, 16),  # 306m spread  ★  chaotic north-west
    "deadlands":          (34, 18),  # 302m spread  ★  barren north

    # Moderate terrain ●
    "the_spine":          (20, 14),  # 297m spread  ●  northern spine ridge
    "floodlands":         (32, 38),  # 264m spread  ●  flooded south
    "bast":               (42, 32),  # 252m spread  ●  eastern midlands
    "stenn_desert":       (44, 16),  # 243m spread  ●  eastern desert
    "stobe_garden":       (36, 12),  # 212m spread  ●  northern valley
    "swamplands":         (20, 38),  # 213m spread  ●  swamp south-west
    "high_bonefields":    (36, 10),  # 227m spread  ●  high northern fields
    "shek_territory":     (40, 22),  # 208m spread  ●  shek highlands
    "iron_valleys":       (30, 34),  # 230m spread  ●  iron mining valleys
    "the_crags":          (46, 28),  # 283m spread  ●  far east crags
    "stobe_gamble":       (32, 14),  # 172m spread  ●  rocky plateau
    "greenbeach":         (12, 36),  # 173m spread  ●  coastal green
    "watcher_rim":        (10, 20),  # 312m spread  ★  watcher's mountains

    # Flatter zones (good for cities / settlements)
    "the_hub":            (32, 28),  # 142m spread     player start town
    "holy_nation":        (36, 24),  # 139m spread     okran's flat lands
    "okran_pride":        (38, 28),  # 95m  spread     holy nation
    "venge":              (28,  8),  # 135m spread     venge far north
    "bonefields":         (40, 16),  # 119m spread     skeleton plains
    "gut":                (24, 38),  # 73m  spread     southern gut
    "skimsands":          (20, 44),  # 63m  spread     flat sandy south
}

# Quality tags for display
def _tag(spread):
    if spread > 300: return "★ dramatic"
    if spread > 150: return "● moderate"
    return "  flat    "


def _img():
    return Image.open(KENSHI_MAP)


def extract(zone_name, zx, zz, img=None):
    if img is None:
        img = _img()
    px0, pz0 = zx * 256, zz * 256
    px1 = min(px0 + EXTRACT_PX, 16384)
    pz1 = min(pz0 + EXTRACT_PX, 16384)

    region = img.crop((px0, pz0, px1, pz1))
    region = region.resize((OUT_SIZE, OUT_SIZE), Image.BICUBIC)

    raw    = struct.unpack(f'<{OUT_SIZE * OUT_SIZE}H', region.tobytes())
    hm     = [(v / 65535.0) * H_RANGE_M + H_OFFSET_M for v in raw]
    mean_h = sum(hm) / len(hm)
    hm     = [h - mean_h for h in hm]

    hmin, hmax = min(hm), max(hm)
    out  = os.path.join(OUT_DIR, f"{zone_name}.r32")
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(out, 'wb') as f:
        f.write(struct.pack('<IIff', OUT_SIZE, OUT_SIZE, hmin, hmax))
        f.write(struct.pack(f'<{len(hm)}f', *hm))
    return hmin, hmax, out


def cmd_list(img=None):
    if img is None:
        img = _img()
    print(f"\n{'Zone slug':28s}  {'zx':>3s} {'zz':>3s}  {'spread':>7s}  {'tag'}")
    print("-" * 65)
    for name, (zx, zz) in sorted(ZONES.items(), key=lambda x: x[0]):
        px0, pz0 = zx * 256, zz * 256
        r = img.crop((px0, pz0, min(px0+512, 16384), min(pz0+512, 16384)))
        raw = struct.unpack(f'<{r.size[0]*r.size[1]}H', r.tobytes())
        hm  = [(v/65535)*H_RANGE_M + H_OFFSET_M for v in raw]
        mn  = sum(hm) / len(hm)
        spread = max(hm) - min(hm) - mn + mn  # max-min
        spread = max(hm) - min(hm)
        print(f"  {name:26s}  {zx:3d} {zz:3d}  {spread:7.0f}m  {_tag(spread)}")
    print(f"\nTotal: {len(ZONES)} zones")


def cmd_extract_all(img=None):
    if img is None:
        print("Loading fullmap.tif …", flush=True)
        img = _img()
    ok = 0
    for name, (zx, zz) in sorted(ZONES.items()):
        print(f"  {name:26s} … ", end='', flush=True)
        hmin, hmax, path = extract(name, zx, zz, img)
        spread = hmax - hmin
        print(f"h=[{hmin:6.0f}..{hmax:5.0f}]m spread={spread:4.0f}m  {_tag(spread)}")
        ok += 1
    print(f"\n✓ Extracted {ok}/{len(ZONES)} zones → {OUT_DIR}/")


if __name__ == '__main__':
    arg = sys.argv[1] if len(sys.argv) > 1 else 'list'

    if arg == 'list':
        cmd_list()
    elif arg == 'all':
        cmd_extract_all()
    elif arg == 'dramatic':
        img = _img()
        dramatic = {k: v for k, v in ZONES.items()}
        # only zones with spread > 300m
        for name, (zx, zz) in sorted(dramatic.items()):
            px0, pz0 = zx * 256, zz * 256
            r = img.crop((px0, pz0, min(px0+512, 16384), min(pz0+512, 16384)))
            raw = struct.unpack(f'<{r.size[0]*r.size[1]}H', r.tobytes())
            hm = [(v/65535)*H_RANGE_M + H_OFFSET_M for v in raw]
            spread = max(hm) - min(hm)
            if spread > 300:
                print(f"  {name:26s} … ", end='', flush=True)
                hmin, hmax, _ = extract(name, zx, zz, img)
                print(f"h=[{hmin:6.0f}..{hmax:5.0f}]m  ★")
    elif arg in ZONES:
        img = _img()
        hmin, hmax, path = extract(arg, *ZONES[arg], img)
        print(f"✓ {arg}: h=[{hmin:.0f}..{hmax:.0f}]m → {path}")
    else:
        # Try partial match
        matches = [k for k in ZONES if arg.lower() in k.lower()]
        if matches:
            img = _img()
            for m in matches:
                hmin, hmax, path = extract(m, *ZONES[m], img)
                print(f"✓ {m}: h=[{hmin:.0f}..{hmax:.0f}]m → {path}")
        else:
            print(f"Unknown zone '{arg}'. Use 'list' to see all zones.")
            sys.exit(1)
