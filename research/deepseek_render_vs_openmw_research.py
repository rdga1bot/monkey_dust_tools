#!/usr/bin/env python3
"""
deepseek_render_vs_openmw_research.py — full render+terrain comparison
(our C++/SDL_GPU render+terrain layer vs real OpenMW master branch
source), via DeepSeek. Companion to the Claude-direct architecture
research (docs/research/RENDER_VS_OPENMW_RESEARCH.md) done by two
parallel research agents reading the real OpenMW source directly.

WHY THIS SCRIPT EXISTS INSTEAD OF CLAUDE CALLING THE API DIRECTLY: the
platform's auto-mode classifier hard-blocks an AI agent from autonomously
sending this repo's proprietary render/terrain architecture to an external
API without a human directly executing that specific action (same block
hit earlier this session for the Godot comparison). Running this script
requires the human (you) to type the command yourself -- same DeepSeek
account, same deepseek-reasoner model already validated this session,
same repo you already own and control. Claude did not and will not
execute this script.

OpenMW source was sparse-checked-out (real, current master branch, GPL)
into a scratchpad directory before this script was written -- see
OPENMW_REF below. If that path no longer exists (new session, scratchpad
cleared), re-fetch it first:
  mkdir -p /tmp/openmw_ref && cd /tmp/openmw_ref && git init -q && \
  git remote add origin https://github.com/OpenMW/openmw.git && \
  git config core.sparseCheckout true && mkdir -p .git/info && \
  printf 'apps/openmw/mwrender/\ncomponents/terrain/\ncomponents/esmterrain/\ncomponents/sceneutil/\ncomponents/shader/\nfiles/shaders/\n' > .git/info/sparse-checkout && \
  git pull --depth 1 origin master
  (then update OPENMW_REF below to match, or pass --openmw-ref)

USAGE (run this yourself, e.g. via `! python3 ...` in the Claude Code
session, or from a plain terminal):
  python3 tools/research/deepseek_render_vs_openmw_research.py --dry-run
  python3 tools/research/deepseek_render_vs_openmw_research.py
  python3 tools/research/deepseek_render_vs_openmw_research.py --topic 1
"""

import argparse
import glob as globmod
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent.parent
OPENMW_REF = Path("/tmp/claude-1001/-home-rdga1-rdga1prj-monkeydust/e9c60870-ac26-475f-9e9d-84b930cbfe9f/scratchpad/openmw_ref")
OUT_FILE = _REPO / "docs" / "research" / "RENDER_VS_OPENMW_DEEPSEEK_RESEARCH.md"
KEY_FILE = Path("/home/rdga1/rdga1bot-cli-md-deepseek.txt")

API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-reasoner"
MAX_TOKENS = 8000
MAX_CHARS_PER_SIDE = 16000
MAX_LINES_PER_FILE = 300

SYSTEM_PROMPT = (
    "You are a rendering engineer comparing a small custom C++17/SDL_GPU "
    "game engine (Intel HD 520 target, single-color-attachment-per-pipeline "
    "HAL limitation, direct GPU command-buffer building, no scene-graph "
    "abstraction) against OpenMW's real, current renderer -- an open-source "
    "(GPL) reimplementation of Morrowind built on OpenSceneGraph (OSG), a "
    "scene-graph-based rendering architecture very different in paradigm "
    "from a direct GPU-HAL renderer. You will be given real source excerpts "
    "from both codebases. Be concrete: cite real file/class/function names "
    "from what you were shown, do not invent APIs you were not shown. Give "
    "an honest verdict on whether porting the technique/algorithm (not the "
    "whole system, and not the OSG scene-graph paradigm itself) is worth it "
    "given the constraints described."
)


def _brace_expand(pattern: str):
    m = re.search(r"\{([^{}]+)\}", pattern)
    if not m:
        return [pattern]
    options = m.group(1).split(",")
    out = []
    for opt in options:
        out.extend(_brace_expand(pattern[:m.start()] + opt + pattern[m.end():]))
    return out


def _read_files(patterns, max_chars):
    chunks = []
    total = 0
    seen = set()
    paths = []
    for pat in patterns:
        for expanded in _brace_expand(pat):
            paths.extend(sorted(globmod.glob(expanded)))
    for p in paths:
        if p in seen or total >= max_chars:
            continue
        seen.add(p)
        try:
            text = Path(p).read_text(errors="replace")
        except OSError:
            continue
        lines = text.splitlines()[:MAX_LINES_PER_FILE]
        body = "\n".join(lines)
        header = f"\n--- FILE: {p} ---\n"
        remaining = max_chars - total
        if len(header) + len(body) > remaining:
            body = body[: max(0, remaining - len(header))]
        chunks.append(header + body)
        total += len(header) + len(body)
    return "".join(chunks), sorted(seen)


def _omw(*rel):
    return str(OPENMW_REF / Path(*rel))


TOPICS = [
    (
        "terrain_geometry_lod",
        "Terrain geometry/LOD: quadtree, chunking, skirts/morphing",
        [
            f"{_REPO}/engine/include/monkey_dust/render/terrain_quadtree_renderer.h",
            f"{_REPO}/engine/src/render/terrain_quadtree_renderer.cpp",
            f"{_REPO}/game/src/world/terrain_quadtree.cpp",
            f"{_REPO}/shaders/terrain_quadtree.vert",
        ],
        [
            _omw("components/terrain/quadtreeworld.hpp"),
            _omw("components/terrain/quadtreeworld.cpp"),
            _omw("components/terrain/quadtreenode.hpp"),
            _omw("components/terrain/quadtreenode.cpp"),
            _omw("components/terrain/chunkmanager.hpp"),
            _omw("components/terrain/chunkmanager.cpp"),
            _omw("components/terrain/terraindrawable.hpp"),
        ],
        "Our quadtree recomputes SelectVisible from scratch every frame "
        "(no persistent tree state, no paging -- whole world is one "
        "resident heightmap texture). OpenMW's Storage/LandManager DOES "
        "page real Morrowind cell data. Note this difference explicitly.",
    ),
    (
        "terrain_texturing_splat",
        "Terrain ground-texture blending / splat system",
        [
            f"{_REPO}/engine/include/monkey_dust/render/terrain_shading_projected.h",
            f"{_REPO}/engine/include/monkey_dust/render/terrain_vt_page_cache.h",
            f"{_REPO}/shaders/terrain_shading_screenspace.frag",
            f"{_REPO}/shaders/terrain_shading_common.glsl",
        ],
        [
            _omw("components/terrain/material.hpp"),
            _omw("components/terrain/material.cpp"),
            _omw("components/terrain/compositemaprenderer.hpp"),
            _omw("components/terrain/compositemaprenderer.cpp"),
            _omw("components/terrain/texturemanager.hpp"),
            _omw("files/shaders/compatibility/terrain.frag"),
            _omw("files/shaders/compatibility/terrain_composite.frag"),
        ],
        "OpenMW's CompositeMapRenderer bakes per-cell composite splat "
        "textures (its closest analog to a virtual-texture/clipmap "
        "system). Compare that specifically against our TerrainVtPageCache "
        "(currently disabled, MIN_CACHEABLE_TIER=99 -- see its own doc "
        "comment for why) and TerrainShadingProjected's screen-space "
        "G-buffer resolve approach.",
    ),
    (
        "render_orchestration",
        "Per-frame render orchestration (scene-graph vs direct command-buffer)",
        [
            f"{_REPO}/game/src/render/npc_render_frame_prep.cpp",
            f"{_REPO}/game/src/render/npc_render_draw_scene.cpp",
            f"{_REPO}/game/src/render/npc_render_deferred.cpp",
            f"{_REPO}/game/src/render/scene_render.h",
        ],
        [
            _omw("apps/openmw/mwrender/renderingmanager.hpp"),
            _omw("apps/openmw/mwrender/renderingmanager.cpp"),
            _omw("apps/openmw/mwrender/vismask.hpp"),
            _omw("apps/openmw/mwrender/renderbin.hpp"),
        ],
        "This is the biggest paradigm difference in the whole comparison: "
        "OSG scene-graph traversal (cull then draw, replaying render bins) "
        "vs our hand-written imperative SDL_GPU command-buffer sequence "
        "with hard, Intel-ANV-specific ordering rules baked in. Do not "
        "recommend adopting a scene-graph -- assess whether any narrower "
        "organizational idea (e.g. render-bin-style pass grouping) is "
        "extractable without the whole paradigm shift.",
    ),
    (
        "postprocess",
        "Post-processing: SSAO/Bloom/CAS/MotionBlur/SMAA + tonemapping",
        [
            f"{_REPO}/engine/include/monkey_dust/render/{{ssao_system,bloom_system,cas_pass,motion_blur,smaa_system,color_grade}}.h",
            f"{_REPO}/shaders/{{ssao_main,bloom_composite,cas,motion_blur,smaa_final}}.frag",
        ],
        [
            _omw("apps/openmw/mwrender/postprocessor.hpp"),
            _omw("apps/openmw/mwrender/postprocessor.cpp"),
            _omw("apps/openmw/mwrender/pingpongcanvas.hpp"),
            _omw("apps/openmw/mwrender/pingpongcanvas.cpp"),
            _omw("apps/openmw/mwrender/luminancecalculator.hpp"),
            _omw("apps/openmw/mwrender/luminancecalculator.cpp"),
        ],
        "OpenMW's postprocessor.cpp is a user/mod-extensible shader "
        "framework (Morrowind modders write their own post shaders). Ours "
        "is a fixed, hand-tuned pipeline. Compare the ping-pong buffer "
        "management specifically, not the extensibility question.",
    ),
    (
        "lighting_shadows",
        "Lighting model + shadow mapping",
        [
            f"{_REPO}/engine/include/monkey_dust/render/{{deferred_lighting,point_light_system,strip_light_system,gbuffer,ambient_probe,light_system,evsm_shadow,shadow_system}}.h",
            f"{_REPO}/shaders/{{shadow_csm,shadow_blur}}.frag",
        ],
        [
            _omw("components/sceneutil/lightmanager.hpp"),
            _omw("components/sceneutil/lightmanager.cpp"),
            _omw("components/sceneutil/clusteredlighting.hpp"),
            _omw("components/sceneutil/depth.hpp"),
            _omw("components/sceneutil/depth.cpp"),
        ],
        None,
    ),
    (
        "water_weather_sky",
        "Water, weather, sky, fog",
        [
            f"{_REPO}/shaders/{{water,sky}}.{{vert,frag}}",
            f"{_REPO}/game/src/render/scene_render.h",
        ],
        [
            _omw("apps/openmw/mwrender/water.hpp"),
            _omw("apps/openmw/mwrender/water.cpp"),
            _omw("apps/openmw/mwrender/sky.hpp"),
            _omw("apps/openmw/mwrender/sky.cpp"),
            _omw("apps/openmw/mwrender/fogmanager.hpp"),
            _omw("apps/openmw/mwrender/fogmanager.cpp"),
            _omw("apps/openmw/mwrender/ripplesimulation.hpp"),
        ],
        "OpenMW is also a Morrowind-weather reimplementation (weather "
        "transitions, real ripple simulation) -- expect a much richer "
        "system than ours. Focus the verdict on any narrow, portable "
        "technique, not a full weather-system port.",
    ),
    (
        "shader_material_system",
        "Shader/material system: permutations, resource layer",
        [
            f"{_REPO}/engine/include/monkey_dust/render/{{material_desc,md_shader,md_mesh,md_texture,asset_cache}}.h",
            f"{_REPO}/engine/src/render/{{material_desc,md_shader}}.cpp",
        ],
        [
            _omw("components/shader/shadermanager.hpp"),
            _omw("components/shader/shadermanager.cpp"),
            _omw("components/shader/shadervisitor.hpp"),
            _omw("components/shader/shadervisitor.cpp"),
        ],
        "Compare shadermanager.cpp's GLSL-permutation approach against our "
        "shader_features bitmask + parent-inheritance MaterialDesc. Note "
        "specifically whether OpenMW does any binding-layout reflection "
        "(we have none, and it's a documented real footgun on Intel ANV: "
        "a resource-count mismatch against SPIR-V set=N bindings is silent "
        "garbage, not a validation error).",
    ),
    (
        "object_paging_foliage",
        "Distant object paging + foliage/groundcover rendering",
        [
            f"{_REPO}/engine/include/monkey_dust/render/{{prop_renderer,vegetation_renderer,clutter_renderer,instancer}}.h",
            f"{_REPO}/engine/src/render/{{prop_renderer,vegetation_renderer,clutter_renderer}}.cpp",
        ],
        [
            _omw("apps/openmw/mwrender/objectpaging.hpp"),
            _omw("apps/openmw/mwrender/objectpaging.cpp"),
            _omw("apps/openmw/mwrender/groundcover.hpp"),
            _omw("apps/openmw/mwrender/groundcover.cpp"),
        ],
        "This pairs with the already-known finding that our PropRenderer "
        "issues one draw call per instance (confirmed anti-pattern vs "
        "GPU instancing, see docs/research/RENDER_VS_GODOT_RESEARCH.md's "
        "props_vegetation section). Check whether OpenMW's objectpaging/ "
        "groundcover use real instancing and how their distant-object "
        "merge/simplify approach compares.",
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
        "temperature": 0.2,
    }).encode()
    req = urllib.request.Request(
        API_URL,
        data=body,
        headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
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
                    raise RuntimeError(f"empty content (finish_reason={choice.get('finish_reason')})")
                if choice.get("finish_reason") == "length":
                    raise RuntimeError("truncated (finish_reason=length) -- raise MAX_TOKENS")
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


def build_prompt(title: str, our_patterns, omw_files, note):
    our_text, our_paths = _read_files(our_patterns, MAX_CHARS_PER_SIDE)
    if omw_files:
        omw_text, omw_paths = _read_files(omw_files, MAX_CHARS_PER_SIDE)
    else:
        omw_text, omw_paths = "", []
    parts = [
        f'Subsystem: "{title}"',
        "",
        f"Our real code ({len(our_paths)} files read):",
        our_text if our_text else "(no matching files found)",
        "",
    ]
    if omw_paths:
        parts += [f"OpenMW real code ({len(omw_paths)} files read):", omw_text, ""]
    else:
        parts += ["OpenMW: no source provided for this subsystem -- see note below.", ""]
    if note:
        parts += [f"Note: {note}", ""]
    parts += [
        "Produce, in this exact order with these exact headers:",
        "## Our approach",
        "## OpenMW approach",
        "## Comparison",
        "## Verdict",
        "(one of: worth_porting / partially_worth_porting / not_worth_porting / no_analog, plus one sentence why)",
        "## Recommendation",
        "(concrete next step if worth porting, or why not)",
    ]
    return "\n".join(parts)


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
        "# RENDER_VS_OPENMW_DEEPSEEK_RESEARCH.md — full render+terrain comparison (DeepSeek)",
        "",
        "> Companion to the Claude-direct architecture research in",
        "> docs/research/RENDER_VS_OPENMW_RESEARCH.md (two parallel research",
        "> agents reading real OpenMW master-branch source directly).",
        "> Generated by `tools/research/deepseek_render_vs_openmw_research.py`",
        "> (deepseek-reasoner), run directly by the human user (not by Claude)",
        "> per this project's data-handling boundary for this class of action.",
        "> Raw model output -- verify before acting on any recommendation.",
        "",
    ]
    for key, title, _p, _g, _n in TOPICS:
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
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--openmw-ref", type=str, default=None, help="override OPENMW_REF path")
    args = ap.parse_args()

    global OPENMW_REF
    if args.openmw_ref:
        OPENMW_REF = Path(args.openmw_ref)
    if not OPENMW_REF.exists():
        print(f"ERROR: OpenMW reference checkout not found at {OPENMW_REF}", file=sys.stderr)
        print("See this script's module docstring for the sparse-checkout command to re-fetch it.", file=sys.stderr)
        sys.exit(1)

    sections = load_existing_sections()
    todo = TOPICS if args.topic is None else [TOPICS[args.topic - 1]]

    print(f"Report: {OUT_FILE}")
    print(f"OpenMW ref: {OPENMW_REF}")
    print(f"Topics: {len(todo)}  (already done: {sum(1 for k,_,_,_,_ in TOPICS if k in sections)}/{len(TOPICS)})")

    if args.dry_run:
        for key, title, our_patterns, omw_files, _note in todo:
            our_text, our_paths = _read_files(our_patterns, MAX_CHARS_PER_SIDE)
            omw_text, omw_paths = _read_files(omw_files, MAX_CHARS_PER_SIDE) if omw_files else ("", [])
            status = "DONE" if key in sections else "pending"
            print(f"  [{status}] {key}: {title}")
            print(f"      our files: {len(our_paths)} ({len(our_text)} chars)")
            print(f"      openmw files: {len(omw_paths)} ({len(omw_text)} chars)")
        return

    api_key = read_api_key()

    for key, title, our_patterns, omw_files, note in todo:
        if key in sections and not args.force:
            print(f"  skip (already done): {title}")
            continue
        print(f"  building prompt + querying: {title} ...", flush=True)
        prompt = build_prompt(title, our_patterns, omw_files, note)
        t0 = time.monotonic()
        try:
            answer = call_deepseek(api_key, prompt)
        except Exception as e:
            print(f"    ERROR: {e}")
            continue
        sections[key] = answer
        write_report(sections)
        print(f"    done in {time.monotonic()-t0:.0f}s, {len(answer)} chars -- saved")

    print(f"\nReport written: {OUT_FILE}")


if __name__ == "__main__":
    main()
