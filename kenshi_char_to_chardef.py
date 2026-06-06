#!/usr/bin/env python3
"""
kenshi_char_to_chardef.py — конвертер Kenshi .bod2/.body → MD .chardef

Workflow:
  1. Запустити Kenshi → Character Creation → налаштувати персонажа
  2. Натиснути EXPORT → зберігається як:
       Kenshi/data/character/bodies/export/<Name>.bod2
  3. Запустити цей скрипт:
       python3 tools/kenshi_char_to_chardef.py <input.bod2> <output.chardef>
       python3 tools/kenshi_char_to_chardef.py <input.bod2>              # → stdout

Формат .bod2:
  Бінарний, key-value пари: [uint32 keylen][key bytes][float32 value]
  100 = нейтральне значення для всіх слайдерів (body та face).
"""

import struct
import json
import sys
import os

# ── Skin tone colour presets (з editor_data_human.xml ColourRange "Skin Tone") ──
_SKIN_PRESETS = [
    (255, 255, 255),   # 0%
    (251, 219, 219),   # 20%
    (201, 190, 180),   # 40%
    (137, 119, 102),   # 60%
    ( 97,  73,  61),   # 80%
    ( 67,  64,  69),   # 100%
]

def _skin_tone_to_rgb(val):
    """val: 0..100 → (r, g, b) в [0..1]"""
    t = max(0.0, min(100.0, val)) / 100.0
    idx_f = t * (len(_SKIN_PRESETS) - 1)
    idx_lo = int(idx_f)
    idx_hi = min(idx_lo + 1, len(_SKIN_PRESETS) - 1)
    frac = idx_f - idx_lo
    lo = _SKIN_PRESETS[idx_lo]
    hi = _SKIN_PRESETS[idx_hi]
    r = (lo[0] + frac * (hi[0] - lo[0])) / 255.0
    g = (lo[1] + frac * (hi[1] - lo[1])) / 255.0
    b = (lo[2] + frac * (hi[2] - lo[2])) / 255.0
    return r, g, b


# ── Parser ────────────────────────────────────────────────────────────────────

# Slider ranges for old .body format (normalized [0,1] → actual value)
# Format: key → (min, max)  — from editor_data_human.xml
_BODY_RANGES = {
    "Height": (80, 120), "Frame": (80, 120), "Posture": (0, 70),
    "Shoulder set": (45, 90), "Neck position": (25, 80),
    "Leg length": (85, 115), "Shoulders": (90, 110), "Arm bulk": (70, 145),
    "Hands": (80, 120), "Chest": (55, 180), "Stomach": (70, 200),
    "Waist": (15, 155), "Hips": (75, 145), "Legs bulk": (70, 140),
    "Legs shape": (85, 115), "Feet": (80, 120),
    "Breast size": (0, 150), "Breasts": (0, 100), "Perkiness": (0, 100),
}
_FACE_RANGES = {
    "Head size": (90, 110), "Head shape": (90, 110),
    "Neck": (65, 150), "Neck width": (70, 150), "Neck length": (85, 115),
    "Jaw": (85, 120),
}


def _parse_bod(path):
    """Повертає dict {slider_name: float_value}."""
    with open(path, "rb") as f:
        data = f.read()

    # Всі відомі slider-імена (з editor_data_human.xml + .bod2 legacy names)
    KNOWN_KEYS = [
        # Body sliders
        "Height", "Frame", "Posture", "Shoulder set", "Neck position",
        "Leg length", "Shoulders", "Arm bulk", "Hands", "Chest",
        "Stomach", "Waist", "Hips", "Legs bulk", "Legs shape", "Feet",
        "Breast size", "Breasts", "Perkiness", "Mid-section",
        # Face bone-scale sliders
        "Head size", "Head shape", "Neck", "Neck width", "Neck length", "Jaw",
        # Face morph sliders (editor_data_human.xml names)
        "Cheekbones", "Mouth size", "Mouth width", "Nose width", "Nose length",
        "Nose arch", "Nose position", "Nose tilt", "Brow position", "Brow tilt",
        "Eyes depth", "Eyes narrow", "Eyes closeness", "Eyes tilt", "Eyes height",
        "Eyes size", "Chin depth",
        # Legacy .bod2 names (без множини)
        "Eye height", "Eye shape", "Eye size", "Eye spacing",
        "Brow", "Brow height",
        "Cheekbone", "Cheekbone height",
        "Chin", "Chin protrusion", "Chin width",
        "Lips", "Mouth position",
        # Hair / skin / meta
        "Hair Color Red", "Hair Color Green", "Hair Color Blue",
        "hair color R", "hair color G", "hair color B",
        "Hair Brightness", "Hair Contrast", "Hair Saturation",
        "Skin Tone", "hair style", "Age", "sex female",
    ]

    result = {}
    for key in KNOWN_KEYS:
        kb = key.encode("ascii")
        klen = len(kb)
        pos = 0
        while True:
            idx = data.find(kb, pos)
            if idx == -1:
                break
            # Перевірка: uint32 перед рядком = довжина рядка
            if idx >= 4:
                stored_len = struct.unpack_from("<I", data, idx - 4)[0]
                if stored_len == klen:
                    float_off = idx + klen
                    if float_off + 4 <= len(data):
                        val = struct.unpack_from("<f", data, float_off)[0]
                        if -10000.0 < val < 10000.0:
                            result[key] = val
                            break
            pos = idx + 1

    return result


# ── Mapping Kenshi → chardef ──────────────────────────────────────────────────

# body[] layout (18 elements):
#   0,1: константи 50 (не використовуються в CharCustomization)
#   2: Height  3: Frame  4: Posture  5: Shoulder set  6: Neck position
#   7: Leg length  8: Shoulders  9: Arm bulk  10: Waist  11: Hands
#   12: Chest  13: Stomach  14: Legs shape  15: Hips  16: Legs bulk  17: Feet

_BODY_MAP = {
    2:  ("Height",       100.0),
    3:  ("Frame",        100.0),
    4:  ("Posture",       35.0),
    5:  ("Shoulder set",  68.0),
    6:  ("Neck position", 53.0),
    7:  ("Leg length",   100.0),
    8:  ("Shoulders",    100.0),
    9:  ("Arm bulk",     100.0),
    10: ("Waist",        100.0),
    11: ("Hands",        100.0),
    12: ("Chest",        100.0),
    13: ("Stomach",      100.0),
    14: ("Legs shape",   100.0),
    15: ("Hips",         100.0),
    16: ("Legs bulk",    100.0),
    17: ("Feet",         100.0),
}

# face[] layout (24 elements), нейтраль = 100:
#   0: Head size  1: Head shape  2: Neck  3: Neck width  4: Neck length
#   5: Eyes size  6: Eyes narrow  7: Eyes closeness  8: Eyes height
#   9: Nose width  10: Nose length  11: Nose arch  12: Nose tilt
#   13: Cheekbones  14: Eyes depth  15: Brow position  16: Brow tilt
#   17: Jaw  18: Mouth width  19: Mouth position  20: Lips(Mouth size)
#   21: Chin depth  22: Eyes tilt  23: Nose position

# Альтернативні імена (legacy .bod2)
_FACE_ALT = {
    "Eyes size":      "Eye size",
    "Eyes narrow":    "Eye shape",
    "Eyes closeness": "Eye spacing",
    "Eyes height":    "Eye height",
    "Eyes depth":     None,
    "Eyes tilt":      None,
    "Cheekbones":     "Cheekbone",
    "Brow position":  "Brow",
    "Brow tilt":      "Brow height",
    "Mouth size":     "Lips",
    "Chin depth":     "Chin",
    "Nose position":  None,
}

_FACE_MAP = {
    0:  ("Head size",      100.0),
    1:  ("Head shape",     100.0),
    2:  ("Neck",           100.0),
    3:  ("Neck width",     100.0),
    4:  ("Neck length",    100.0),
    5:  ("Eyes size",      100.0),
    6:  ("Eyes narrow",    100.0),
    7:  ("Eyes closeness", 100.0),
    8:  ("Eyes height",    100.0),
    9:  ("Nose width",     100.0),
    10: ("Nose length",    100.0),
    11: ("Nose arch",      100.0),
    12: ("Nose tilt",      100.0),
    13: ("Cheekbones",     100.0),
    14: ("Eyes depth",     100.0),
    15: ("Brow position",  100.0),
    16: ("Brow tilt",      100.0),
    17: ("Jaw",            100.0),
    18: ("Mouth width",    100.0),
    19: ("Mouth position", 100.0),
    20: ("Mouth size",     100.0),
    21: ("Chin depth",     100.0),
    22: ("Eyes tilt",      100.0),
    23: ("Nose position",  100.0),
}

def _lookup(kv, primary, default):
    """Шукає primary, потім alt name, повертає default якщо не знайдено."""
    if primary in kv:
        return kv[primary]
    alt = _FACE_ALT.get(primary)
    if alt and alt in kv:
        return kv[alt]
    return default


# ── Converter ─────────────────────────────────────────────────────────────────

def convert(bod_path, out_name=None):
    kv = _parse_bod(bod_path)

    # Detect old .body format (normalized [0,1]) vs .bod2 (0-200 scale).
    # If Height or Frame is in (0,5) range → old format, denormalize.
    is_old_body = False
    for key in ("Height", "Frame"):
        if key in kv and 0.0 < kv[key] < 5.0:
            is_old_body = True
            break

    if is_old_body:
        denorm = {}
        for key, val in kv.items():
            if key in _BODY_RANGES:
                lo, hi = _BODY_RANGES[key]
                denorm[key] = lo + val * (hi - lo)
            elif key in _FACE_RANGES:
                lo, hi = _FACE_RANGES[key]
                denorm[key] = lo + val * (hi - lo)
            elif key == "sex female":
                denorm[key] = val
            elif 0.0 <= val <= 1.5:
                # Face morph normalized: 0.5=neutral=100
                denorm[key] = 100.0 + (val - 0.5) * 200.0
            else:
                denorm[key] = val
        kv = denorm

    # --- Ім'я ---
    base = os.path.splitext(os.path.basename(bod_path))[0]
    name = out_name or base

    # --- Стать ---
    sex = 1 if kv.get("sex female", 0.0) > 0.5 else 0

    # --- Skin tone ---
    skin_tone = kv.get("Skin Tone", 50.0)
    skin_r, skin_g, skin_b = _skin_tone_to_rgb(skin_tone)

    # --- Hair color ---
    # .bod2 новий формат (Hair Color R/G/B, scale 0-100)
    hair_r = kv.get("Hair Color Red",   kv.get("hair color R", 20.0)) / 100.0
    hair_g = kv.get("Hair Color Green", kv.get("hair color G", 15.0)) / 100.0
    hair_b = kv.get("Hair Color Blue",  kv.get("hair color B", 10.0)) / 100.0

    # Застосувати Brightness (−100..+100)
    brightness = kv.get("Hair Brightness", 0.0)
    bright_mult = max(0.05, (100.0 + brightness) / 100.0)
    hair_r = min(1.0, hair_r * bright_mult)
    hair_g = min(1.0, hair_g * bright_mult)
    hair_b = min(1.0, hair_b * bright_mult)

    # color_str — насиченість кольору (0=сіра, 1=насичена)
    saturation = kv.get("Hair Saturation", kv.get("hair saturation", 50.0))
    color_str = max(0.0, min(1.0, saturation / 100.0))

    # --- body[] (18 елементів) ---
    body = [50.0, 50.0] + [0.0] * 16
    for idx, (key, default) in _BODY_MAP.items():
        body[idx] = kv.get(key, default)

    # --- face[] (24 елементи) ---
    face = [100.0] * 24
    for idx, (key, default) in _FACE_MAP.items():
        face[idx] = _lookup(kv, key, default)

    # --- hair_f[] (5 елементів): стиль/розмір, поки заглушка ---
    hair_style = kv.get("hair style", 0.0)
    hair_f = [40.0, 40.0, 40.0, 100.0, 50.0]
    hair_f[4] = hair_style if hair_style > 0 else 50.0

    # --- Збірка chardef ---
    chardef = {
        "name":     name,
        "sex":      sex,
        "race_row": 0,
        "skin_r":   round(skin_r, 4),
        "skin_g":   round(skin_g, 4),
        "skin_b":   round(skin_b, 4),
        "hair_r":   round(hair_r, 4),
        "hair_g":   round(hair_g, 4),
        "hair_b":   round(hair_b, 4),
        "color_str": round(color_str, 4),
        "body":     [round(v, 2) for v in body],
        "face":     [round(v, 2) for v in face],
        "hair_f":   [round(v, 2) for v in hair_f],
    }
    return chardef


# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 kenshi_char_to_chardef.py <input.bod2> [output.chardef]")
        print()
        print("Exports Kenshi character (data/character/bodies/export/<Name>.bod2)")
        print("to MD .chardef JSON format.")
        print()
        print("Workflow:")
        print("  1. Запустити Kenshi → Character Creation → EXPORT")
        print("  2. Файл зберігається у Kenshi/data/character/bodies/export/")
        print("  3. python3 tools/kenshi_char_to_chardef.py export/MyChar.bod2 game/data/chars/my_char.chardef")
        sys.exit(1)

    bod_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else None

    if not os.path.exists(bod_path):
        print(f"Error: file not found: {bod_path}", file=sys.stderr)
        sys.exit(1)

    chardef = convert(bod_path)

    out_json = json.dumps(chardef, indent=2, ensure_ascii=False)

    if out_path:
        os.makedirs(os.path.dirname(out_path), exist_ok=True) if os.path.dirname(out_path) else None
        with open(out_path, "w") as f:
            f.write(out_json)
            f.write("\n")
        print(f"Written: {out_path}")
    else:
        print(out_json)


if __name__ == "__main__":
    main()
