#!/usr/bin/env python3
"""Download 4 CC0 terrain textures from Polyhaven at 1K resolution.

Saves to game/data/textures/terrain/{grass,rock,dirt,bark}.jpg
Skips files that already exist.
"""

import os
import sys

try:
    import requests
except ImportError:
    print("ERROR: 'requests' not installed. Run: pip install requests")
    sys.exit(1)

TEXTURES = [
    (
        "grass",
        "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/"
        "aerial_grass_rock/aerial_grass_rock_diff_1k.jpg",
    ),
    (
        "rock",
        "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/"
        "rocky_terrain/rocky_terrain_diff_1k.jpg",
    ),
    (
        "dirt",
        "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/"
        "brown_mud/brown_mud_diff_1k.jpg",
    ),
    (
        "bark",
        "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/"
        "bark_brown_02/bark_brown_02_diff_1k.jpg",
    ),
]

# Resolve path relative to repo root (two levels up from tools/scripts/).
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT   = os.path.join(SCRIPT_DIR, "..", "..")
OUT_DIR     = os.path.normpath(os.path.join(REPO_ROOT, "game", "data", "textures", "terrain"))


def download_file(name: str, url: str, dest: str) -> bool:
    if os.path.exists(dest):
        size = os.path.getsize(dest)
        print(f"  [skip]  {name}.jpg  ({size} bytes, already exists)")
        return True

    print(f"  [fetch] {name}.jpg  ← {url}")
    try:
        r = requests.get(url, timeout=60, stream=True)
        r.raise_for_status()
        total = int(r.headers.get("content-length", 0))
        written = 0
        with open(dest, "wb") as f:
            for chunk in r.iter_content(chunk_size=65536):
                if chunk:
                    f.write(chunk)
                    written += len(chunk)
        print(f"          → {dest}  ({written} bytes)")
        return True
    except Exception as e:
        print(f"  [ERROR] {name}.jpg  download failed: {e}")
        # Remove partial file if any
        if os.path.exists(dest):
            os.remove(dest)
        return False


def main() -> int:
    os.makedirs(OUT_DIR, exist_ok=True)
    print(f"Output directory: {OUT_DIR}")
    print()

    ok = 0
    for name, url in TEXTURES:
        dest = os.path.join(OUT_DIR, f"{name}.jpg")
        if download_file(name, url, dest):
            ok += 1

    print()
    print(f"Done: {ok}/{len(TEXTURES)} textures ready.")
    return 0 if ok == len(TEXTURES) else 1


if __name__ == "__main__":
    sys.exit(main())
