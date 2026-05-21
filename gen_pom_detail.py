#!/usr/bin/env python3
"""
gen_pom_detail.py — Generate POM detail textures for monkey_dust terrain.

Takes existing terrain JPEGs and produces RGBA PNGs where:
  RGB = original detail colour (normalized to [0.35, 0.65] range for neutral shader blend)
  A   = height field [0, 1] derived from luminance (bright=raised, dark=sunken)

Usage:
    python3 tools/gen_pom_detail.py
    python3 tools/gen_pom_detail.py --all   # process all terrain/*.jpg

Output: game/data/textures/terrain_pom_<name>.png
"""

import sys
import os
from PIL import Image, ImageFilter, ImageEnhance
import struct

OUT_SIZE  = 512   # output texture size (power of two, 512×512 tiles well)
OUT_DIR   = "game/data/textures"


def luminance(rgb_arr):
    """Convert float32 HxWx3 array to luminance HxW."""
    return 0.299 * rgb_arr[:,:,0] + 0.587 * rgb_arr[:,:,1] + 0.114 * rgb_arr[:,:,2]


def make_height(lum, blur_radius=2.0):
    """
    Generate height map from luminance:
    - Normalize to [0, 1]
    - Blur slightly for smooth POM transitions (avoids sharp stair-stepping)
    - Remap to [0.05, 0.95] to avoid extreme edge clamping in POM loop
    """
    mn, mx = lum.min(), lum.max()
    if mx - mn < 1e-6:
        return (lum * 0 + 0.5).astype("float32")
    h = (lum - mn) / (mx - mn)

    # Blur in PIL for smooth height field
    h_img = Image.fromarray((h * 255).astype("uint8"), mode="L")
    h_img = h_img.filter(ImageFilter.GaussianBlur(radius=blur_radius))
    h = [p / 255.0 for p in h_img.getdata()]
    import array as ar
    h_arr = ar.array("f", h)

    # Convert back to remap [0.05, 0.95]
    result = []
    for v in h:
        result.append(0.05 + v * 0.90)
    return result


def normalize_rgb(rgb_img, target_mid=0.50, strength=0.6):
    """
    Normalize brightness so the texture mid-grey ≈ target_mid.
    Shader does: albedo * detail * 2.0 blended at 0.25 weight.
    If detail = 0.5 → albedo * 0.5 * 2 = albedo (neutral).
    We want the average of the detail to be ~0.5 so POM has no net brightness change.
    """
    enh = ImageEnhance.Brightness(rgb_img)
    import numpy as np
    arr = np.array(rgb_img, dtype="float32") / 255.0
    mean = arr.mean()
    if mean > 1e-4:
        factor = (target_mid / mean) * strength + (1.0 - strength)
        rgb_img = enh.enhance(factor)
    return rgb_img


def process(src_path, dst_path, blur=2.0):
    import numpy as np

    src = Image.open(src_path).convert("RGB")
    # Resize to OUT_SIZE (power-of-two for mipmapping)
    src = src.resize((OUT_SIZE, OUT_SIZE), Image.LANCZOS)
    src = normalize_rgb(src)

    rgb = np.array(src, dtype="float32") / 255.0
    lum = luminance(rgb)

    # Blur lum before height derivation
    lum_img = Image.fromarray((lum * 255).astype("uint8"), mode="L")
    lum_img = lum_img.filter(ImageFilter.GaussianBlur(radius=blur))
    lum = np.array(lum_img, dtype="float32") / 255.0

    # Normalize height to [0.05, 0.95]
    mn, mx = lum.min(), lum.max()
    if mx - mn > 1e-4:
        height = (lum - mn) / (mx - mn)
    else:
        height = lum * 0 + 0.5
    height = 0.05 + height * 0.90

    r = (rgb[:,:,0] * 255).clip(0, 255).astype("uint8")
    g = (rgb[:,:,1] * 255).clip(0, 255).astype("uint8")
    b = (rgb[:,:,2] * 255).clip(0, 255).astype("uint8")
    a = (height     * 255).clip(0, 255).astype("uint8")

    rgba = np.stack([r, g, b, a], axis=2)
    result = Image.fromarray(rgba, mode="RGBA")
    result.save(dst_path, optimize=False)

    h_min, h_max = a.min(), a.max()
    print(f"  [OK] {os.path.basename(src_path):30s} → {os.path.basename(dst_path)}"
          f"  height=[{h_min},{h_max}]  size={OUT_SIZE}×{OUT_SIZE}")
    return dst_path


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root  = os.path.dirname(script_dir)
    terrain_dir = os.path.join(repo_root, "game/data/textures/terrain")
    out_dir     = os.path.join(repo_root, OUT_DIR)

    # Priority textures — one per major Kenshi biome
    priority = [
        ("rock.jpg",             "terrain_pom_rock.png"),          # default (arid zones)
        ("k_desert_rock.jpg",    "terrain_pom_desert_rock.png"),   # Kenshi desert
        ("k_desert_sandstone.jpg","terrain_pom_sandstone.png"),    # sandstone
        ("k_high_rock.jpg",      "terrain_pom_high_rock.png"),     # highlands
        ("k_scrub_rock.jpg",     "terrain_pom_scrub_rock.png"),    # scrublands
        ("scrub_ground.jpg",     "terrain_pom_scrub_ground.png"),  # flat scrub (low POM)
    ]

    # Adjust blur per material: rocky = sharp, ground = smooth
    blur_map = {
        "scrub_ground.jpg": 4.0,   # smooth ground = high blur, gentle height transitions
        "sandstone": 3.0,
    }

    print("\n── POM Detail Texture Generator ────────────────────────────────────")
    created = []
    for src_name, dst_name in priority:
        src = os.path.join(terrain_dir, src_name)
        dst = os.path.join(out_dir, dst_name)
        if not os.path.exists(src):
            print(f"  [SKIP] {src_name} — not found")
            continue
        blur = blur_map.get(src_name, 2.0)
        created.append(process(src, dst, blur=blur))

    if created:
        print(f"\n  Primary for InitPOM(): game/data/textures/{os.path.basename(created[0])}")
    print("────────────────────────────────────────────────────────────────────\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
