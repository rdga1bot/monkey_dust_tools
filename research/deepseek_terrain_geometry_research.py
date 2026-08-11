#!/usr/bin/env python3
"""
deepseek_terrain_geometry_research.py — глибоке дослідження публічних методів
побудови ГЕОМЕТРІЇ (не шейдингу) ландшафту через DeepSeek API.

Контекст: користувач незадоволений "загальною формою рельєфу" поточного
термейну (гори виглядають округлими/"blobby", без гострих хребтів) і просить
не поверхневий, а поглиблений огляд публічно доступних методів — окремо
від попереднього (шейдингового) дослідження цієї сесії.

Формат: N незалежних, вузько сфокусованих запитів до deepseek-reasoner
(режим з ланцюжком міркувань — глибше й повніше за deepseek-chat для
техничного синтезу), кожен по одній під-темі. Відповіді зберігаються по
секціях у ОДИН markdown-звіт — resumable (вже готові секції не
перезапитуються при повторному запуску).

USAGE:
  export DEEPSEEK_API_KEY="sk-..."     # опційно; якщо не задано, скрипт
                                         # читає ключ напряму з файлу нижче
                                         # (НІКОЛИ не друкує його)
  python3 tools/research/deepseek_terrain_geometry_research.py
  python3 tools/research/deepseek_terrain_geometry_research.py --dry-run
  python3 tools/research/deepseek_terrain_geometry_research.py --topic 3   # тільки одна тема
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
OUT_FILE = _REPO / "docs" / "research" / "TERRAIN_GEOMETRY_DEEPSEEK_RESEARCH.md"
KEY_FILE = Path("/home/rdga1/rdga1bot-cli-md-deepseek.txt")

API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-reasoner"
# deepseek-reasoner spends completion tokens on hidden reasoning_content
# BEFORE producing the final content field -- a 6000 budget was observed to
# be entirely consumed by reasoning (finish_reason=="length", content=="")
# on these prompts. 32000 leaves headroom for both.
MAX_TOKENS = 48000

SYSTEM_PROMPT = (
    "You are a senior real-time terrain rendering engineer doing exhaustive "
    "technical research for a small open-world RPG engine (C++17, SDL_GPU, "
    "low-end Intel HD 520 class GPU target, real-world DEM-derived heightfield "
    "data, chunked/CDLOD-style LOD, ~460m chunks). Be concrete and technical: "
    "name real algorithms, real engines/games, real papers, real parameter "
    "ranges. Prefer citing specific, verifiable public sources (GDC talks, "
    "SIGGRAPH papers, open-source repos, engine docs) over generic advice. "
    "Do not pad with fluff. Structure with markdown headers and bullet lists. "
    "This is for engineers, not marketing."
)

TOPICS = [
    (
        "1_blobby_root_causes",
        "Діагностика: округла/'blobby' геометрія гір",
        """We have a heightfield terrain built from REAL Kenshi-derived DEM-style
data (genuine ~500m elevation change within a single 460.8m chunk in extreme
zones). Mesh: chunked LOD, 129x129 vertices per chunk at full res (LOD0),
step 2/4/8 for LOD1-3, discrete (non-morphed) LOD switching, per-vertex
normals computed from the heightfield via forward differences. Visually the
mountains look rounded/"blobby"/melted rather than sharp and jagged like
real-world mountains or like Kenshi's actual terrain.

Give an EXHAUSTIVE, concrete diagnostic checklist of the real, technically
distinct root causes that produce this specific "rounded/blobby" look in
heightfield-based terrain, ranked by how commonly they are the actual cause
in production engines. For each cause: (a) exactly how to detect/confirm it
is happening in our specific case, (b) the standard fix, (c) any tradeoff.
Cover at minimum: source data downsampling/interpolation method (bilinear vs
bicubic vs Lanczos), vertex density vs feature wavelength (Nyquist limit for
terrain, i.e. how many meters/vertex is needed to represent an X-meter-wide
ridge), normal computation/filtering (forward-diff vs Sobel vs multi-sample
averaging, and normal smoothing across too wide a kernel), LOD blending
artifacts, and any real GPU tessellation/displacement approach. Real
numbers where possible.""",
    ),
    (
        "2_real_dem_ridge_preserving",
        "Обробка реальних DEM-даних без втрати гострих хребтів",
        """We have real-world elevation data (originally 16385x16385 uint16
heightmap, subsampled ~2x to build our runtime chunks, i.e. real downsampling
happens). Real DEM/heightmap data, once downsampled or interpolated for a
mesh, tends to lose sharp ridgelines and produce rounded peaks.

Research real, publicly documented techniques used by GIS tools and terrain
engines specifically to preserve or restore sharp ridges/valleys when
downsampling or interpolating real elevation data: curvature-aware or
feature-preserving downsampling, ridge-preserving resampling algorithms
(e.g. used in USGS/GIS pipelines), edge-preserving filters applied to
heightfields (bilateral filter on height, anisotropic diffusion), and any
technique used by game engines specifically (not just GIS) to keep real-world
DEM-sourced terrain looking sharp after LOD reduction. Name real
algorithms/papers/tools (e.g. GDAL resampling methods, specific SIGGRAPH/
I3D papers on terrain simplification that preserve features) with enough
detail that an engineer could implement them from your description.""",
    ),
    (
        "3_erosion_for_sharpening",
        "Ерозія, що ЗАГОСТРЮЄ (а не згладжує) рельєф",
        """Distinguish clearly between erosion simulation that SMOOTHS terrain
(most thermal/talus-angle erosion, mass-conserving cascade like
plate-tectonics' plate::erode(), which we already tried and which did NOT
fix a rounded-mountain look) versus erosion or post-processing techniques
that actually INCREASE perceived sharpness/ruggedness of a heightfield:
hydraulic erosion with sediment/channel carving (which carves sharp gullies
even while depositing softly elsewhere), Infinigen's Particle::cascade()
(mass-conserving, 8-connected, per-material talus angle — explain exactly
why this differs from a naive 4-connected version and whether it would
plausibly sharpen vs smooth), ridged multifractal noise blended additively
atop real DEM data specifically along ridgelines/high-curvature areas, and
any other real technique specifically for making REAL (not purely
procedural) heightfield data look craggier without fabricating unrealistic
detail. For each: real algorithm name, complexity class, and realistic
parameter ranges. Also state plainly which of these are known to NOT help
(so we don't repeat known-ineffective attempts) and why plate::erode()-style
smoothing specifically fails to fix a "rounded" look (hypothesis: it only
redistributes mass locally, doesn't add high-frequency detail — confirm or
refute with real numerical reasoning).""",
    ),
    (
        "4_procedural_detail_injection",
        "Домішування процедурної деталі поверх реальних даних",
        """Real open-world games rarely ship pure raw DEM data — they blend real
macro-shape height data with procedural micro/meso-detail noise
(domain-warped ridged multifractal, Worley/curl noise, etc.) to add
sharpness the source data lacks at vertex resolution. Research and describe,
with real technical specifics, how known games/engines do this blend:
No Man's Sky's procedural terrain, Ghost of Tsushima's terrain pipeline
(any public GDC material), Unreal Engine Landscape's noise/erosion layers,
id Tech's terrain, and any open-source reference (e.g. terrain generation in
open-source voxel/heightfield engines). Specifically address: at what
frequency/octave range is the procedural detail blended in (should it only
affect high-frequency content so it doesn't distort the real macro shape?),
how is blend weight modulated by slope/curvature (steeper = more added
crag detail is a common pattern — confirm/detail), and whether this is
applied at content-authoring time (offline, baked into a static heightmap)
vs at runtime (shader-side displacement). Give concrete guidance for a case
where we must preserve the REAL Kenshi-derived macro shape exactly (project
goal is Kenshi parity) but want to add sharpness only as a small
high-frequency detail layer.""",
    ),
    (
        "5_lod_mesh_vs_normalmap",
        "Геометрична деталізація vs normal/detail-map ілюзія",
        """For a low-end GPU target (Intel HD 520, integrated Gen9, ~1280x720-
1920x1080, budget: currently 129x129 verts per 460.8m chunk at full LOD,
i.e. ~3.6m/vertex at closest range), determine when actual GEOMETRIC vertex
density increase is necessary to fix a "rounded mountain" look versus when
normal-mapping / detail-mapping / parallax occlusion mapping can create
equivalent PERCEIVED sharpness without adding real triangles. Cover: at what
viewing distance does silhouette-level geometric roundness become
objectionable regardless of normal-map trickery (silhouette edges always
reveal true geometry, this has a hard limit), realistic vertex/meter
targets used by other engines' near-LOD terrain (CDLOD reference
implementation by Filip Strugar, Frostbite's terrain, any concrete numbers
from GDC/SIGGRAPH talks), and whether GPU tessellation/vertex-texture-fetch
displacement (VTF, which we already use for our terrain — heightmap sampled
in vertex shader) is a realistic path to add silhouette-affecting detail
without doubling/quadrupling static vertex counts. Be concrete about
triangle budgets that are realistic on Gen9 integrated graphics.""",
    ),
    (
        "6_low_poly_rugged_aesthetic",
        "Гострий/рваний вигляд при обмеженому бюджеті трикутників",
        """Research real games/engines that achieve a sharp, rugged, "gritty"
terrain silhouette (not smooth/rounded) while deliberately staying at a
MODEST triangle budget suitable for low-end/integrated GPUs — i.e. NOT
photorealistic megascan-driven AAA terrain, but stylistically sharp terrain
achieved cheaply. Consider: Kenshi itself (Ogre3D-based, we already have
some RE notes on its real terrain.hlsl/ground_type system — but focus here
specifically on how ITS geometry avoids looking rounded despite presumably
modest vertex density, if any public info exists), other indie open-world
RPGs on similarly modest engines, and any general technique (e.g.
deliberately using flat-shaded or reduced-normal-smoothing faceted terrain
near cliffs specifically to read as "jagged" rather than smoothing normals
across the whole surface) that trades geometric smoothness AT THE SHADING
level (not adding more triangles) for a sharper visual read. Be honest about
what is realistically achievable without adding real vertex density.""",
    ),
    (
        "7_open_source_reference_impls",
        "Референсні open-source реалізації геометрії термейну",
        """List concrete, currently-public open-source terrain rendering
implementations (with repo names/URLs where you're confident they're real
and still public) whose GEOMETRY construction (not shading) is directly
comparable to a chunked-CDLOD, vertex-texture-fetch, real-heightfield-driven
system, and that are known to produce SHARP (not rounded) mountain
silhouettes from real or real-like elevation data. For each: how they compute
vertex positions/normals, whether/how they preserve sharp features through
their LOD scheme, and any specific parameter or technique that differs from
a naive forward-difference-normal + uniform-grid-subdivision approach (which
is what we currently do and which produces the rounded look). Include, if
you have real knowledge of it: Filip Strugar's CDLOD reference (paper +
any public sample code), Godot's Terrain3D plugin's geometry pipeline
specifically (as opposed to its shading, which we already researched),
rbfx's terrain component, and any real-DEM-based open-source flight-sim or
GIS-to-game terrain pipeline (e.g. FlightGear's terrain, or any specific
project you have real knowledge of that ingests real elevation data and
renders it without a rounded look).""",
    ),
]


def read_api_key() -> str:
    env_key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
    if env_key:
        return env_key
    if KEY_FILE.exists():
        return KEY_FILE.read_text().strip()
    print(f"ERROR: no DEEPSEEK_API_KEY env var and {KEY_FILE} not found", file=sys.stderr)
    sys.exit(1)


def call_deepseek(api_key: str, user_prompt: str) -> str:
    body = json.dumps({
        "model": MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": user_prompt},
        ],
        "max_tokens": MAX_TOKENS,
        "temperature": 0.3,
    }).encode()
    req = urllib.request.Request(
        API_URL,
        data=body,
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    last_err = None
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=300) as resp:
                data = json.loads(resp.read())
                choice = data["choices"][0]
                content = choice["message"]["content"].strip()
                if not content:
                    raise RuntimeError(
                        f"empty content (finish_reason={choice.get('finish_reason')}, "
                        f"usage={data.get('usage')}) -- raise MAX_TOKENS"
                    )
                if choice.get("finish_reason") == "length":
                    raise RuntimeError(
                        f"truncated mid-answer (finish_reason=length, "
                        f"usage={data.get('usage')}) -- raise MAX_TOKENS"
                    )
                return content
        except urllib.error.HTTPError as e:
            last_err = f"HTTP {e.code}: {e.read().decode(errors='replace')[:500]}"
            if e.code == 429:
                time.sleep(15 * (attempt + 1))
                continue
            break
        except Exception as e:
            last_err = str(e)
            time.sleep(5 * (attempt + 1))
    raise RuntimeError(f"DeepSeek call failed after retries: {last_err}")


def load_existing_sections() -> dict:
    if not OUT_FILE.exists():
        return {}
    text = OUT_FILE.read_text()
    sections = {}
    for m in re.finditer(r"<!-- SECTION:(\S+) -->\n(.*?)(?=\n<!-- SECTION:|\Z)", text, re.S):
        sections[m.group(1)] = m.group(2).strip()
    return sections


def write_report(sections: dict):
    OUT_FILE.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# TERRAIN_GEOMETRY_DEEPSEEK_RESEARCH.md — глибоке дослідження геометрії термейну (DeepSeek)",
        "",
        "> Згенеровано `tools/research/deepseek_terrain_geometry_research.py` "
        "(deepseek-reasoner). Тема: чому наш термейн виглядає округлим/"
        "\"blobby\" геометрично (не про шейдинг/текстури — це окреме дослідження "
        "цієї ж сесії, див. terrain_research/ARCHITECTURE_AUDIT_2026_08_04.md "
        "та docs/OSS_TERRAIN_METHODS.md). Це СИРІ відповіді моделі — не факти "
        "проекту, потребують верифікації перед впровадженням будь-якого фіксу.",
        "",
    ]
    for key, title, _prompt in TOPICS:
        if key in sections:
            lines.append(f"## {title}")
            lines.append("")
            lines.append(f"<!-- SECTION:{key} -->")
            lines.append(sections[key])
            lines.append("")
    OUT_FILE.write_text("\n".join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--topic", type=int, default=None, help="1-based index, only run this one")
    ap.add_argument("--force", action="store_true", help="re-run even if section already saved")
    args = ap.parse_args()

    sections = load_existing_sections()

    todo = TOPICS
    if args.topic is not None:
        todo = [TOPICS[args.topic - 1]]

    print(f"Report: {OUT_FILE}")
    print(f"Topics: {len(todo)}  (already done: {sum(1 for k,_,_ in TOPICS if k in sections)}/{len(TOPICS)})")

    if args.dry_run:
        for key, title, prompt in todo:
            status = "DONE" if key in sections else "pending"
            print(f"  [{status}] {key}: {title} ({len(prompt)} chars prompt)")
        return

    api_key = read_api_key()

    for key, title, prompt in todo:
        if key in sections and not args.force:
            print(f"  skip (already done): {title}")
            continue
        print(f"  querying: {title} ...", flush=True)
        t0 = time.monotonic()
        try:
            answer = call_deepseek(api_key, prompt)
        except Exception as e:
            print(f"    ERROR: {e}")
            continue
        sections[key] = answer
        write_report(sections)
        print(f"    done in {time.monotonic()-t0:.0f}s, {len(answer)} chars — saved")

    print(f"\n✅ Report written: {OUT_FILE}")


if __name__ == "__main__":
    main()
