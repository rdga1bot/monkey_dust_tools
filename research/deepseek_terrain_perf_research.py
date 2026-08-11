#!/usr/bin/env python3
"""
deepseek_terrain_perf_research.py — глибоке дослідження публічних методів
кешування/оптимізації термейн-ШЕЙДИНГУ (не геометрії) через DeepSeek API.

Контекст: наша власна VT (virtual texturing) спроба закешувати дорогий
per-pixel ShadeTerrainGround (TS_ComputeGroundAlbedo, до ~27 семплів/px
через 4-кутовий Kenshi-zone-boundary blend + cliff triplanar) у сторінковий
атлас ПРОВАЛИЛАСЬ 2026-08-09 (коміт c59d360, engine repo): артефакти шва
(різкий перепад чіткості на межах сусідніх сторінок) лишались навіть після
Phase 0-4 + 4 документовані спроби на tier 0 + окремі спроби на tiers 1-3.
Користувач наказав вимкнути кешування ПОВНІСТЮ (MIN_CACHEABLE_TIER=99) --
зараз кожен піксель термейну йде через живий per-pixel шлях щокадру, що є
підтвердженим домінантним GPU-костом (~14мс FenceWait поблизу термейну на
"The Eye", живе A/B профілювання цієї сесії).

Формат: N незалежних запитів до deepseek-reasoner, кожен по одній під-темі,
resumable (як deepseek_terrain_geometry_research.py, той самий патерн).

USAGE:
  python3 tools/research/deepseek_terrain_perf_research.py
  python3 tools/research/deepseek_terrain_perf_research.py --dry-run
  python3 tools/research/deepseek_terrain_perf_research.py --topic 3
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
OUT_FILE = _REPO / "docs" / "research" / "TERRAIN_PERF_DEEPSEEK_RESEARCH.md"
KEY_FILE = Path("/home/rdga1/rdga1bot-cli-md-deepseek.txt")

API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-reasoner"
MAX_TOKENS = 48000

SYSTEM_PROMPT = (
    "You are a senior real-time rendering engineer doing exhaustive technical "
    "research for a small open-world RPG engine (C++17, SDL_GPU/Vulkan HAL, "
    "low-end Intel HD 520 / Gen9 integrated GPU target, single-color-"
    "attachment-per-pipeline HAL limitation, D32_FLOAT-only depth, no "
    "hardware MRT). The terrain shading is expensive per-pixel (up to ~27 "
    "texture samples/pixel worst case: overlay sample + up to 4 corner-"
    "blended zone ground-layer samples with up to 4 detail layers each, plus "
    "cliff triplanar blending). A prior attempt to cache this into a sparse "
    "virtual-texture page atlas (baking the shading once per page, reading "
    "it back via an indirection texture) FAILED specifically because of "
    "visible hard-sharpness discontinuities at page boundaries (adjacent "
    "independently-baked pages disagree enough that the seam is visible), "
    "even after adding cross-page bilinear blending and within-page bilinear "
    "filtering -- both fixes helped but did not eliminate the seam, and the "
    "whole caching system was disabled by explicit decision rather than ship "
    "a visible artifact. Be concrete and technical: name real algorithms, "
    "real engines/games, real papers, real parameter ranges, and real "
    "open-source code you have genuine knowledge of. Prefer citing specific, "
    "verifiable public sources over generic advice. Do not pad with fluff. "
    "Structure with markdown headers and bullet lists. This is for engineers."
)

TOPICS = [
    (
        "1_seamless_vt_paging",
        "Чому виникають шви на межах VT-сторінок і як їх реально усувають",
        """We baked expensive per-pixel terrain ground shading into a sparse
virtual-texture page cache (fixed-size square pages, each baked
independently via a compute shader that runs the SAME shading function used
by the live path, written into a shared physical atlas, addressed via an
indirection texture). Even after adding (a) cross-page bilinear blending in
the sampling shader (blending the 4 nearest pages' texels at page
boundaries) and (b) real within-page bilinear filtering (replacing
texelFetch with a clamped textureLod sample), a visible hard discontinuity
in perceived sharpness/detail persisted at page boundaries, especially at
the finest cache tier (page texel density ~21x coarser than the live path's
effective detail-texture sampling density).

Give an exhaustive, technically precise explanation of EXACTLY why
independently-baked VT/clipmap pages produce visible seams even with
bilinear filtering at and across boundaries, and the REAL techniques
production virtual texturing systems (id Tech's MegaTexture/virtual
texturing as documented by John Carmack's talks and the Sean Barrett/id
Software papers, Adaptive Virtual Texturing as used by multiple UE4/UE5
titles, RAGE's texture streaming) use to eliminate them: page border/skirt
padding (baking each page with N extra texels of overlap sampled from
neighboring content, not just clamping), mip-map dilation, feathered/
cross-fade blending across a WIDE transition band (not just the boundary
texel), ensuring neighboring pages are baked from a CONSISTENT shared
high-frequency noise/detail source (so adjacent independent bakes don't
diverge because of different random phase), and any other real documented
technique. Be specific about how much border padding (in texels) is
typically used and why a narrow blend (only the boundary pixel) is
insufficient -- give the real mathematical reasoning for why the blend
needs to span multiple texels/a full page fraction to be imperceptible.""",
    ),
    (
        "2_baked_detail_texture_consistency",
        "Узгодженість детальних шарів між незалежно запеченими сторінками",
        """Our per-pixel terrain shading blends a small set of tiling detail
textures (grass/dirt/road/cliff layers, DETAIL_TILING repeat period ~55.6m)
based on slope/curvature/zone masks. When two adjacent virtual-texture pages
(each ~75m or ~150m square, baked independently by a compute shader) are
baked, each page's detail-texture sampling naturally lands at different,
uncorrelated phases of that repeating detail texture relative to its
neighbor, because the detail texture's own repeat period is comparable to
the page footprint. Confirm or refute this as the most likely leading cause
of an unavoidable seam (as opposed to plain resolution mismatch), and give
a real, concrete engineering solution: is the standard fix to always sample
detail/noise textures using GLOBAL WORLD-SPACE coordinates (not per-page
local UVs) so phase is inherently consistent across ANY page boundary
regardless of how the world is chunked into pages? If so, explain precisely
why that alone should make the seam disappear (continuous function sampled
at a shared coordinate space, independent of how you later chop the domain
into cache tiles), and identify any subtlety that could still break it (e.g.
texture filtering/mip selection differing per page because of different
per-page LOD/distance context, false seams from that instead). Also address:
is baking two neighboring pages at DIFFERENT tiers (our system caches
different-sized footprints at different quadtree tiers) an additional,
distinct source of seams beyond the phase-consistency issue, and what is
the standard fix for that specific case (e.g. always evaluate content at a
FIXED base resolution regardless of tier, only vary how coarsely you
resample/downsample it per tier, versus evaluating fresh noise at each
tier's own resolution).""",
    ),
    (
        "3_bake_zone_blend_to_texture",
        "Запікання zone-boundary blend в один суцільний world-space атлас (offline, раз назавжди)",
        """Independent of the live per-pixel shading cost, we have a
structural inefficiency: our terrain is divided into a fixed grid of 4096
independent "zones" (500m each, inherited 1:1 from the original game's save
architecture, where each zone historically was a separate file with its own
ground-layer texture assignment), and every terrain pixel does a 4-corner
bilinear blend across the 4 nearest zone corners' ground-layer data at
runtime, EVERY FRAME, to hide hard zone boundaries. This is confirmed (from
prior research this session) to be the single largest structural cost driver
compared to reference open-source terrain shaders (Godot's Terrain3D
plugin, rbfx) which use one continuous world-space ground-layer texture
with zero per-pixel zone-boundary branching/blending.

Since the zone ground-layer ASSIGNMENT DATA itself is static (baked at
world-authoring time, does not change at runtime -- it's fixed source data,
not gameplay-mutable), give a concrete, real technique for converting this
runtime 4-corner-blend-every-frame system into a ONE-TIME offline bake: how
would a real engine take N independent per-zone material/layer-weight grids
(with hard boundaries between them) and pre-bake a single continuous
world-space texture (or texture array) with the blending ALREADY resolved,
so the live shader just does ONE texture sample instead of a 4-corner blend
+ per-layer texture reads? Cover: what resolution/format such a bake would
need (trade-offs of a single giant baked albedo/weight texture for a ~2km x
2km-scale world at meaningful ground texel density), whether this is
better done as a baked WEIGHT/splat-map texture (keep the actual detail
textures tiling live, just bake the resolved BLEND WEIGHTS not the final
color) versus baking fully resolved albedo (loses per-pixel detail tiling
at close range -- explain this trade-off precisely), and cite any real
production precedent for baking large-scale terrain splat/weight maps
offline (Ghost Recon Wildlands, Just Cause, or any other open-world game
with public GDC material on this specific technique of eliminating runtime
region-boundary blending via an offline bake).""",
    ),
    (
        "4_gen9_shader_cost_reduction",
        "Скорочення вартості per-pixel термейн-шейдера конкретно на Intel Gen9",
        """Our terrain ground-shading fragment shader can do up to ~27 texture
samples per pixel in the worst case (steep cliffs with multiple active
detail layers). This runs on Intel HD 520 (Gen9, 24 execution units,
integrated GPU, ~300 GFLOPS). Give concrete, Gen9-specific (not generic)
guidance on reducing the real cost of a texture-fetch-heavy fragment shader
on this exact GPU class: known Gen9 texture sampler throughput/latency
characteristics, whether combining multiple small texture reads into fewer
larger reads (e.g. packing multiple mask channels into one RGBA texture vs.
separate textures) meaningfully helps on this architecture specifically,
whether reducing dynamic branching (our shader has an `if (detailFade > eps
&& cliff_w < 0.999)` early-out already) has measurable payoff on Gen9's
SIMD execution model versus just always computing and discarding, and any
documented real-world case study of reducing terrain/material shader cost
specifically on Intel integrated graphics (Intel's own developer
documentation/GDC talks on Gen9 optimization, any open-source game
postmortem that specifically targeted Intel HD-class hardware). Also address:
given our engine's own documented constraint that this project's GPU HAL
wrapper only supports ONE color attachment per pipeline (no true MRT), is
there a realistic compute-shader-based alternative to fragment-shader
terrain shading (write results to a storage image/SSBO instead of a render
target) that could reduce cost on Gen9 specifically, or is that a dead end
for this hardware class.""",
    ),
    (
        "5_fixed_world_precompute_case_studies",
        "Кейс-стаді: рушії з фіксованим (не процедурним) світом, що уникають runtime-текстурування",
        """Unlike infinite procedural-world games, our world is FIXED and
finite (~2km x 2km scale, hand-authored/imported from a real source game's
data, does not change at runtime except rare player-driven edits in an
editor tool). Research real, publicly documented open-world or large-map
games/engines that have a SIMILARLY FIXED, FINITE world and that
deliberately chose heavy OFFLINE precomputation of terrain material/shading
data specifically to minimize runtime per-pixel cost (as opposed to
procedural/infinite-world games like No Man's Sky that must shade
everything live because nothing is precomputable). For each: what exactly
did they precompute (baked terrain albedo textures? baked splat/weight
maps? baked ambient occlusion/shadow? baked normal maps at full
resolution?), at what practical resolution/texel density for a comparable
map scale, and what real engine/GDC/technical-blog source documents it.
Consider: Ghost Recon Wildlands/Breakpoint (Ubisoft, has public GDC terrain
material), Just Cause series (Avalanche Engine, large fixed open world),
Horizon Zero Dawn (already partially researched this session for LOD
density but not for its terrain material baking specifically -- cover that
angle here), Kenshi itself if any public info exists on whether it does any
offline terrain texture baking, and any other real fixed-world game you
have genuine knowledge of. The goal: identify whether "just bake it once,
offline, at world-authoring time" is a proven, low-risk pattern for a fixed
world like ours, as opposed to the runtime virtual-texture caching we tried
and failed at.""",
    ),
    (
        "6_realistic_budget_case_studies",
        "Реалістичний бюджет per-pixel термейн-шейдингу на low-end/mobile-class GPU",
        """Give real, concrete numbers (not vague guidance) for what a
realistic per-pixel terrain/ground shading cost budget looks like on
low-end integrated or mobile-class GPUs (Intel Gen9 HD 520 class, ~300
GFLOPS, or comparable mobile GPUs of similar power). Specifically: how many
texture samples per pixel is considered acceptable/expensive/prohibitive
for a terrain ground shader on this GPU class at 1080p, any real measured
frame-time-per-texture-sample or ALU-cost figures you have genuine
knowledge of for Gen9 or similar architectures, and case studies of
shipped games with visually rich, multi-layer terrain texturing (multiple
blended ground materials, cliff/slope-dependent texturing) that specifically
targeted low-end/integrated/mobile GPUs and therefore had to solve this
exact cost problem (not AAA PC/console terrain systems that assume a
discrete high-end GPU). Also address directly: given our confirmed real
measurement of ~14ms GPU time (FenceWait) attributable specifically to this
terrain shading resolve pass on Intel HD 520 at a worst-case steep-terrain
camera position, is that consistent with what you'd expect for a ~27-
sample-worst-case terrain shader on this GPU class, or does that number
suggest something is unusually inefficient beyond the raw sample count
(e.g. sampler/texture format choice, unnecessary dependent texture reads,
poor cache locality from the specific texture layout) that we should
suspect and investigate before assuming the sample count itself is the only
lever available.""",
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
            with urllib.request.urlopen(req, timeout=600) as resp:
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
        "# TERRAIN_PERF_DEEPSEEK_RESEARCH.md — глибоке дослідження термейн-шейдинг продуктивності (DeepSeek)",
        "",
        "> Згенеровано `tools/research/deepseek_terrain_perf_research.py` "
        "(deepseek-reasoner). Контекст: наша VT page-cache спроба (Phase 0-4 "
        "+ 4 tier-0 attempts) провалилась через шви на межах сторінок і "
        "була вимкнена повністю (engine commit c59d360, 2026-08-09, явне "
        "рішення користувача). Живий per-pixel шлях лишається єдиним, "
        "виміряний домінантний GPU-кост ~14мс FenceWait на \"The Eye\". "
        "Це СИРІ відповіді моделі — потребують верифікації перед "
        "впровадженням будь-якого фіксу.",
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
