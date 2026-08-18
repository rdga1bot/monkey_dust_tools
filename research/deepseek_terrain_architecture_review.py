#!/usr/bin/env python3
"""
deepseek_terrain_architecture_review.py -- full architectural review of
monkey_dust's terrain rendering, comparing our current systems against
alternatives and against REAL Kenshi's own (RE-verified) approach, with an
explicit simplification goal.

Context (2026-08-18 session): two structurally DIFFERENT terrain LOD
rewrites this session (GEOCLIPMAP toroidal clipmap with per-tile culling +
baked normals; then a non-indexed Geomipmapping patch-grid with skirts +
world-wide baked normals) BOTH still show a persistent visible artifact the
user describes as "square bands around the camera" -- despite RenderDoc
numerically proving zero normal-delta at LOD-level boundaries in the first
rewrite. Mid-investigation we found real Kenshi's own terrain.hlsl (RE'd,
HIGH confidence) uses per-vertex CDLOD-style geomorphing
(`lerp(position.y, position.w, morph)` + `lerp(normal, normal1, morph)`),
and that THIS PROJECT had a working CDLOD geomorph implementation on an
earlier (now-deleted) TerrainPatchGrid, removed on 2026-08-16 based on a
STATIC 3-pose crack-only test that would not have caught a moving-camera
LOD-popping artifact. Neither of this session's two rewrites re-added
geomorph. User now wants a full architecture review, comparison against
alternatives, maximum simplification, and closeness to real Kenshi -- not
another guess-and-rewrite cycle.

USAGE:
  python3 tools/research/deepseek_terrain_architecture_review.py
  python3 tools/research/deepseek_terrain_architecture_review.py --dry-run
  python3 tools/research/deepseek_terrain_architecture_review.py --topic 2
"""

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent.parent
OUT_FILE = _REPO / "docs" / "research" / "TERRAIN_ARCHITECTURE_REVIEW_DEEPSEEK_RESEARCH.md"
KEY_FILE = Path("/home/rdga1/rdga1bot-cli-md-deepseek.txt")

API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-reasoner"
MAX_TOKENS = 48000

SYSTEM_PROMPT = (
    "You are a senior real-time terrain rendering engineer doing exhaustive "
    "technical research for a small open-world RPG engine (C++17, SDL_GPU "
    "Vulkan backend, low-end Intel HD 520 (Gen9) integrated GPU target, "
    "1280x720 @ 60fps, real-world Kenshi-derived DEM heightfield data, "
    "~29.5km square world). Be concrete and technical: name real algorithms, "
    "real engines/games, real papers, real parameter ranges. Prefer citing "
    "specific, verifiable public sources (GDC talks, SIGGRAPH/I3D papers, "
    "open-source repos, engine docs, Ogre3D's own source) over generic "
    "advice. Do not pad with fluff. Structure with markdown headers and "
    "bullet lists. This is for engineers making an architecture DECISION, "
    "not a survey for its own sake -- always end with a clear recommendation "
    "and the concrete reasoning for it."
)

TOPICS = [
    (
        "1_geomorph_vs_discrete_lod_verdict",
        "Geomorph vs discrete-tier LOD: чи це справжня причина шва",
        """We have a chunked/patched heightfield terrain (Kenshi-derived real DEM
data, ~29.5km world, patches/tiles selected into discrete LOD tiers by
camera distance, arranged in roughly concentric rings/bands around the
camera by construction). We tried TWO different discrete-LOD architectures
back to back:

(A) A toroidal geometry clipmap (GEOCLIPMAP/Losasso-Hoppe style, 8 nested
levels, shared mesh redrawn per level with per-level texel offset, per-tile
frustum culling, a WORLD-WIDE baked normal texture sampled at a level-
independent fixed native-heightmap-texel step specifically so that two
adjacent clip levels compute IDENTICAL vertex normals at their shared
boundary -- verified via RenderDoc post-vertex-shader dump: normal delta
0.0000 across all 7 level boundaries, positions matched exactly).

(B) A flat Geomipmapping-style patch grid (300m patches, discrete LOD
tier per patch via neighbor-cascade capping adjacent-tier delta to 1,
vertical skirt geometry hanging skirt_depth=height_range+margin below each
patch border to hide any T-junction gap regardless of tier delta, same
world-wide baked normal texture as (A) reused for normal continuity).

BOTH (A) and (B) still show a persistent visible artifact the user
describes as "square/rectangular bands visible around the camera" that
moved with the camera as they walked/flew around -- unchanged by either
architecture, despite (A)'s hard numeric proof of zero normal disagreement
at level boundaries in a STATIC captured frame.

Neither (A) nor (B) implements per-vertex geomorphing (CDLOD-style
`lerp(position.y, morphTargetY, morph)` + `lerp(normal, morphTargetNormal,
morph)` blended smoothly as the camera moves and a tier's distance
threshold is crossed). We have separately confirmed the REAL Kenshi engine's
own terrain.hlsl (Ogre3D-based) DOES use exactly this per-vertex geomorph
technique, pre-baking a LOD-1 height/normal into spare vertex channels at
mesh-build time and blending via a single `morph` scalar uniform.

Question: is a MISSING geomorph (not a normal-agreement bug, not a T-
junction gap -- both already hard-proven fixed/absent) a plausible root
cause of a persistent "banding around the camera" artifact that is
UNCHANGED by two otherwise-correct discrete-tier architectures? Explain
precisely, with the real underlying math/perception reasoning, what visual
artifact discrete (non-morphed) LOD tier switching produces even when:
(a) there is zero geometric gap (skirts/matched topology already handle
this), and (b) vertex normals agree exactly at the boundary in a static
frame. Cover specifically: triangle density discontinuity under
specular/Fresnel lighting response (does a denser triangle mesh vs a
coarser one at the SAME normal produce a different rasterized appearance
under real-time shading -- confirm or refute with real reasoning, not
speculation), silhouette popping as the boundary itself sweeps past under
camera motion, and any other real, named phenomenon in the terrain-
rendering literature that matches "static-frame proof passed but the
artifact is still visible, especially moving/near the camera." End with a
clear verdict: is this consistent with a missing-geomorph explanation, or
does the described symptom (visible in the specific screenshots -- teal/
dark speckled patches and salmon discoloration on steep cliff faces at
close range, not obviously ring-shaped in single frames) point somewhere
else entirely (e.g. ground-texture/clutter shading, not LOD geometry at
all)? Be honest if the evidence doesn't cleanly support the geomorph
hypothesis -- do not force a conclusion.""",
    ),
    (
        "2_simplify_toward_real_kenshi",
        "Максимальне спрощення: наближення до реальної архітектури Kenshi",
        """We have RE-verified (HIGH confidence, direct decompile + shader source)
facts about real Kenshi's own terrain system: built on Ogre3D's Terrain
component (Ogre::Terrain + manual LOD entities, NOT a custom engine),
LOD strategy "distance_sphere" (Ogre's built-in strategy, range [0,1],
bias unlimited), patch size 16 (standard) or 64 (high-quality flag),
setDetailThreshold(8.0, 8.0, 0.0) hardcoded fallback with slider-driven
formula param=(1-slider)*30+5, setOgreBuildLimits(150,2) light / (250,100)
full-quality throttle pairs, height data raw uint16 258x258 tiles
downsampled 2:1 to 129x129 floats with a fixed 0.14953841 scale constant,
and CRITICALLY per-vertex geomorph baked into spare vertex channels
(position.w = pre-baked coarser-LOD height, a BINORMAL-slot vec3 = pre-
baked coarser-LOD normal, blended via `lerp(.., .., morph)` in the vertex
shader). Terrain horizontal extent 294912 world units, vertical range 9800
world units.

We have spent significant engineering effort this session building TWO
increasingly complex, fully custom terrain LOD architectures (a toroidal
geometry clipmap with 8 nested levels and per-tile culling; a non-indexed
"vertex-buffer-less" Geomipmapping patch-grid using gl_VertexIndex-only
procedural vertex generation with zero CPU-uploaded vertex/index buffers)
-- NEITHER of which matches what Kenshi itself actually does, and both
still show visible artifacts.

Given the user's explicit goal is now MAXIMUM SIMPLIFICATION and closeness
to Kenshi's real, working, shipped technique (not novel invention), answer
directly:

1. Is Ogre3D's actual open-source Terrain component (MIT-licensed, source
available) a viable REFERENCE IMPLEMENTATION to port/adapt the LOD
selection + geomorph logic from, rather than re-deriving it from papers?
What would the real scope of that port be (roughly how many files/how much
logic, based on what's publicly known about Ogre::Terrain's architecture)?

2. Classic Geomipmapping (de Boer 2000) with CDLOD-style continuous
geomorph (as Kenshi's own shader does) -- described as simply and minimally
as possible for a C++17/SDL_GPU/Vulkan engine targeting Intel Gen9 HD 520:
what is the SMALLEST correct implementation (patch grid + LOD selection +
geomorph vertex shader + skirts as a structural crack-avoidance backstop,
NOT relying on the cascade/geomorph alone) -- and how does its complexity
compare to what we already built (the toroidal clipmap, and the non-indexed
patch-grid)?

3. Are there any REAL, well-known reasons a geometry clipmap (Losasso-Hoppe
2004, what our system (A) already is) would be considered UNNECESSARY
complexity for a game whose real reference implementation (Kenshi) uses
plain chunked/patched Geomipmapping with geomorph instead? When IS a
geometry clipmap actually justified over simpler patch-based Geomipmapping
(e.g. specific to unbounded/procedural terrain streaming, which Kenshi's
FIXED 29.5km world does not need)?

4. Give a concrete, ranked recommendation: given our real constraints
(Intel HD 520, C++17, SDL_GPU, fixed-size real-world heightfield already
fully resident in memory, no streaming requirement), what is the SIMPLEST
architecture that would both (a) structurally guarantee zero seams and (b)
closely match what Kenshi's own shipped, working implementation actually
does? Be willing to recommend DELETING complexity we've already built if a
simpler approach is genuinely both correct and closer to the reference.""",
    ),
    (
        "3_novbo_alu_verdict_given_findings",
        "Чи виправдана vertex-buffer-less/non-indexed складність з огляду на нові дані",
        """We measured (real hardware, Intel HD 520/Gen9, RenderDoc-adjacent
fence-timed A/B, 4 independent runs with interleaved timing to rule out
thermal-throttle order bias) that non-indexed "vertex-buffer-less"
rendering (gl_VertexIndex-only procedural quad-grid generation, zero
vertex/index buffers, 6 vertex-shader invocations per quad with NO
post-transform vertex cache reuse) is STABLY 1.48-2.59x SLOWER than
conventional indexed rendering (shared-vertex IBO, full post-transform
cache reuse) at EXACTLY equal footprint and quad density on this exact
target GPU. This was inspired by OGRE-Next's Terra terrain system, which
uses this technique specifically to avoid a vertex-buffer memory-bandwidth
cost on more capable hardware.

Given (a) this measured, confirmed 1.5-2.6x GPU-side regression on our
actual low-end target hardware, and (b) real Kenshi's own reference
implementation (Ogre3D Terrain component) uses conventional INDEXED
geometry with no such vertex-buffer-less technique at all -- was adopting
the vertex-buffer-less technique ever justified for this specific project's
constraints (low-end integrated GPU, ALU-bound already per Gen9's known
weak ALU throughput vs its comparatively fine memory bandwidth for this
workload size), or is this a case of importing a technique from a different
hardware target (Terra was designed with more capable discrete GPUs and a
DIFFERENT specific bandwidth bottleneck in mind) without verifying it
transfers? Give a direct verdict, and if the verdict is "not justified,"
explain what class of engine/hardware profile WOULD actually benefit from
Terra's specific technique, so it's clear this isn't a blanket dismissal of
the source technique, just a specific-hardware mismatch.""",
    ),
]


def load_api_key() -> str:
    env_key = os.environ.get("DEEPSEEK_API_KEY")
    if env_key:
        return env_key.strip()
    if KEY_FILE.exists():
        return KEY_FILE.read_text().strip()
    print(f"[ERROR] No DEEPSEEK_API_KEY env var and no key file at {KEY_FILE}", file=sys.stderr)
    sys.exit(1)


def call_deepseek(api_key: str, user_prompt: str) -> str:
    payload = {
        "model": MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": user_prompt},
        ],
        "max_tokens": MAX_TOKENS,
        "stream": False,
    }
    req = urllib.request.Request(
        API_URL,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        },
        method="POST",
    )
    last_err = None
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=600) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                choice = data["choices"][0]
                content = choice["message"].get("content", "")
                finish = choice.get("finish_reason", "")
                if not content:
                    print(f"  [WARN] empty content, finish_reason={finish}", file=sys.stderr)
                return content
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", errors="replace")
            last_err = f"HTTP {e.code}: {body[:500]}"
            print(f"  [WARN] attempt {attempt+1} failed: {last_err}", file=sys.stderr)
            time.sleep(5 * (attempt + 1))
        except Exception as e:
            last_err = str(e)
            print(f"  [WARN] attempt {attempt+1} failed: {last_err}", file=sys.stderr)
            time.sleep(5 * (attempt + 1))
    raise RuntimeError(f"DeepSeek call failed after 3 attempts: {last_err}")


def load_existing_sections() -> dict:
    if not OUT_FILE.exists():
        return {}
    text = OUT_FILE.read_text()
    sections = {}
    for m in re.finditer(r"<!-- SECTION:(\S+) -->\n(.*?)(?=\n<!-- SECTION:|\Z)", text, re.DOTALL):
        sections[m.group(1)] = m.group(2)
    return sections


def write_report(sections_in_order: list, section_content: dict):
    parts = [
        "# Terrain Architecture Review — DeepSeek Deep Research\n",
        "_Auto-generated by tools/research/deepseek_terrain_architecture_review.py "
        "(deepseek-reasoner). Resumable: existing sections are not re-queried._\n",
    ]
    for key, title, _ in sections_in_order:
        if key in section_content:
            parts.append(f"\n<!-- SECTION:{key} -->\n## {title}\n\n{section_content[key]}\n")
    OUT_FILE.parent.mkdir(parents=True, exist_ok=True)
    OUT_FILE.write_text("\n".join(parts))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dry-run", action="store_true", help="print topics, don't call API")
    ap.add_argument("--topic", type=int, default=None, help="only run this 1-based topic index")
    args = ap.parse_args()

    if args.dry_run:
        for i, (key, title, prompt) in enumerate(TOPICS, 1):
            print(f"{i}. [{key}] {title} ({len(prompt)} chars)")
        return 0

    api_key = load_api_key()
    existing = load_existing_sections()

    selected = TOPICS if args.topic is None else [TOPICS[args.topic - 1]]
    for i, (key, title, prompt) in enumerate(selected, 1):
        if key in existing:
            print(f"[{i}/{len(selected)}] SKIP (already have) {key}")
            continue
        print(f"[{i}/{len(selected)}] Querying: {title} ...")
        content = call_deepseek(api_key, prompt)
        existing[key] = content
        write_report(TOPICS, existing)
        print(f"  -> wrote {len(content)} chars, report updated at {OUT_FILE}")

    print(f"\nDone. Report: {OUT_FILE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
