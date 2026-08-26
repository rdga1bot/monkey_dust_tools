#!/usr/bin/env python3
"""
deepseek_render_vs_granite_research.py -- compares our C++17/SDL_GPU render
layer against Hans-Kristian "Themaister" Arntzen's real Granite Vulkan
renderer (https://github.com/Themaister/Granite), via DeepSeek. Same
methodology and same data-handling boundary as
deepseek_render_vs_godot_research.py -- see that file's own doc comment
for the full rationale (short version: an AI agent must not autonomously
send this repo's proprietary render architecture to an external API;
running this script requires the human to type the command themselves).

Reference Granite source was fetched read-only via `gh api
repos/Themaister/Granite/contents/...` (public repo, MIT-style license,
matches this project's own "real reference beats invented comparison"
convention already used for the Godot comparison) into a local scratchpad
snapshot -- see GRANITE_REF below. Not committed to this repo.

Context for why Granite specifically: this project's own "Granite terrain
migration" (2026-08-xx, since superseded twice -- see scene_render.h's own
doc comment on TerrainQuadtreeRenderer) borrowed ONLY the name from this
real engine as naming-convention inspiration, never its code. This
comparison checks whether there's anything real Granite does that's
actually worth adopting, now that the borrowed-name confusion has been
raised directly.

USAGE (run this yourself, e.g. via `! python3 ...` in the Claude Code
session, or from a plain terminal):
  python3 tools/research/deepseek_render_vs_granite_research.py --dry-run
  python3 tools/research/deepseek_render_vs_granite_research.py
  python3 tools/research/deepseek_render_vs_granite_research.py --topic 1
"""

import argparse
import glob as globmod
import itertools
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent.parent
GRANITE_REF = Path("/tmp/claude-1001/-home-rdga1-rdga1prj-monkeydust/e9c60870-ac26-475f-9e9d-84b930cbfe9f/scratchpad/granite_ref")
OUT_FILE = _REPO / "docs" / "research" / "RENDER_VS_GRANITE_DEEPSEEK_RESEARCH.md"
KEY_FILE = Path("/home/rdga1/rdga1bot-cli-md-deepseek.txt")

API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-reasoner"
MAX_TOKENS = 8000
MAX_CHARS_PER_SIDE = 16000
MAX_LINES_PER_FILE = 300

SYSTEM_PROMPT = (
    "You are a rendering engineer comparing a small custom C++17/SDL_GPU "
    "game engine (Intel HD 520 target, single-color-attachment-per-pipeline "
    "HAL limitation, no tessellation stage, vert_samplers>0 + frag_samplers>0 "
    "combos are a documented GPU-hang risk on this hardware) against Hans-"
    "Kristian Arntzen's real Granite Vulkan renderer (github.com/Themaister/"
    "Granite). You will be given real source excerpts from both codebases. "
    "Be concrete: cite real file/class/function names from what you were "
    "shown, do not invent APIs you were not shown. Granite targets modern "
    "desktop Vulkan (bindless, mesh shaders, RTAS) -- explicitly flag when a "
    "Granite technique assumes hardware/features this Intel HD 520 target "
    "does not have, rather than recommending it anyway. Give an honest "
    "verdict on whether porting the technique/algorithm (not the whole "
    "system) is worth it given the constraints described."
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
        rel = p
        header = f"\n--- FILE: {rel} ---\n"
        remaining = max_chars - total
        if len(header) + len(body) > remaining:
            body = body[: max(0, remaining - len(header))]
        chunks.append(header + body)
        total += len(header) + len(body)
    return "".join(chunks), sorted(seen)


TOPICS = [
    (
        "gpu_hal",
        "GPU HAL: device/command-buffer abstraction, pipeline caching",
        [
            f"{_REPO}/engine/include/monkey_dust/render/{{gpu_hal,gpu_device,gpu_pipeline}}.h",
            f"{_REPO}/engine/src/render/{{gpu_hal,gpu_hal_commands,gpu_device,gpu_pipeline}}.cpp",
        ],
        [
            str(GRANITE_REF / "vulkan/device.hpp"),
            str(GRANITE_REF / "vulkan/command_buffer.hpp"),
        ],
        "Our HAL wraps SDL_GPU (one color attachment/pipeline, no bindless) -- Granite drives raw Vulkan directly with bindless descriptor indexing and bespoke pipeline caching. Note explicitly which Granite conveniences require bindless/raw-Vulkan control we don't have via SDL_GPU.",
    ),
    (
        "terrain",
        "Terrain: quadtree LOD + forward/deferred shading vs Granite's Ground/GroundPatch",
        [
            f"{_REPO}/engine/include/monkey_dust/render/{{terrain_quadtree_renderer,terrain_renderer,terrain_shading_projected,terrain_world_heightmap}}.h",
            f"{_REPO}/engine/src/render/{{terrain_quadtree_renderer,terrain_renderer,terrain_shading_projected}}.cpp",
            f"{_REPO}/shaders/{{terrain_quadtree.vert,terrain_quadtree_forward.frag,terrain_gbuffer_mini.frag}}",
        ],
        [
            str(GRANITE_REF / "renderer/ground.hpp"),
            str(GRANITE_REF / "renderer/ground.cpp"),
        ],
        "Granite's Ground is a real, shipping CDLOD-family quadtree-of-patches terrain (see GroundPatch::get_render_info, patch_lods, per-patch LOD bias/neighbor pointers) -- the closest real-world analog to our own TerrainQuadtreeRenderer. Compare LOD-selection/morph/neighbor-stitching approach specifically, not just the high-level shape.",
    ),
    (
        "lighting_clustered",
        "Lighting: deferred point/strip lights vs Granite's clustered forward+ lighting",
        [
            f"{_REPO}/engine/include/monkey_dust/render/{{deferred_lighting,light_system,point_light_system,strip_light_system,ambient_probe,gbuffer}}.h",
            f"{_REPO}/engine/src/render/{{deferred_lighting,point_light_system,strip_light_system,ambient_probe,gbuffer}}.cpp",
        ],
        [
            str(GRANITE_REF / "renderer/lights/clusterer.hpp"),
            str(GRANITE_REF / "renderer/lights/clusterer.cpp"),
        ],
        "Granite's Clusterer is compute-driven clustered forward+ (light-list-per-froxel). We're classic deferred G-buffer. These are different architectural families, not a drop-in swap -- assess whether clustering ideas (light culling granularity) are portable to a deferred G-buffer without adopting the whole forward+ model.",
    ),
    (
        "postprocess",
        "Post-processing: bloom/SSAO/SMAA/motion-blur chain",
        [
            f"{_REPO}/engine/include/monkey_dust/render/{{bloom_system,ssao_system,smaa_system,motion_blur_system}}.h",
            f"{_REPO}/engine/src/render/{{bloom_system,ssao_system,smaa_system,motion_blur_system}}.cpp",
            f"{_REPO}/shaders/{{bloom_*,ssao_*,smaa_*,motion_*}}.frag",
        ],
        [
            str(GRANITE_REF / "renderer/post/ssao.hpp"),
            str(GRANITE_REF / "renderer/post/ssao.cpp"),
            str(GRANITE_REF / "renderer/post/smaa.hpp"),
            str(GRANITE_REF / "renderer/post/temporal.hpp"),
        ],
        None,
    ),
    (
        "scene_orchestration",
        "Per-frame scene/render-pass orchestration vs Granite's RenderGraph",
        [
            f"{_REPO}/game/src/render/{{npc_render_deferred,npc_render_draw_scene,npc_render_frame_prep,scene_render}}.cpp",
            f"{_REPO}/game/src/render/{{npc_render,npc_render_internal,scene_render}}.h",
        ],
        [
            str(GRANITE_REF / "renderer/render_graph.hpp"),
        ],
        "Granite's RenderGraph is a real automatic pass-dependency-graph/resource-aliasing system (declared passes, automatic barrier insertion, transient resource aliasing). We hand-order passes explicitly in fixed C++ call sequences with manually-tracked shared resources (e.g. the shared `depth` texture). Assess honestly whether a render-graph abstraction is worth the complexity at our project's scale, or whether explicit ordering remains the right call for a small fixed pass list.",
    ),
    (
        "mesh_renderer",
        "Static/prop mesh rendering vs Granite's mesh_util + Renderer",
        [
            f"{_REPO}/engine/include/monkey_dust/render/{{prop_mesh,prop_renderer,md_mesh}}.h",
            f"{_REPO}/engine/src/render/{{prop_mesh,prop_renderer,md_mesh}}.cpp",
        ],
        [
            str(GRANITE_REF / "renderer/mesh_util.hpp"),
            str(GRANITE_REF / "renderer/renderer.hpp"),
        ],
        None,
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


def build_prompt(title: str, our_patterns, granite_files, note):
    our_text, our_paths = _read_files(our_patterns, MAX_CHARS_PER_SIDE)
    if granite_files:
        granite_text, granite_paths = _read_files(granite_files, MAX_CHARS_PER_SIDE)
    else:
        granite_text, granite_paths = "", []
    parts = [
        f'Subsystem: "{title}"',
        "",
        f"Our real code ({len(our_paths)} files read):",
        our_text if our_text else "(no matching files found)",
        "",
    ]
    if granite_paths:
        parts += [f"Granite real code ({len(granite_paths)} files read):", granite_text, ""]
    else:
        parts += ["Granite: no source provided for this subsystem -- see note below.", ""]
    if note:
        parts += [f"Note: {note}", ""]
    parts += [
        "Produce, in this exact order with these exact headers:",
        "## Our approach",
        "## Granite approach",
        "## Comparison",
        "## Verdict",
        "(one of: worth_porting / partially_worth_porting / not_worth_porting / no_analog),"
        " plus one sentence why",
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
        "# RENDER_VS_GRANITE_DEEPSEEK_RESEARCH.md",
        "",
        "> Compares our render layer against the REAL Themaister/Granite",
        "> Vulkan renderer (github.com/Themaister/Granite) -- prompted by a",
        "> direct user question about whether this project's borrowed",
        "> \"Granite\" naming convention (TerrainQuadtreeRenderer's ancestry,",
        "> GraniteAbsCam, etc.) has any real relationship to that engine's",
        "> actual code (it does not -- name only, confirmed in scene_render.h's",
        "> own doc comment; this report checks if there's anything real Granite",
        "> does that's independently worth adopting).",
        "> Generated by `tools/research/deepseek_render_vs_granite_research.py`",
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
    args = ap.parse_args()

    sections = load_existing_sections()
    todo = TOPICS if args.topic is None else [TOPICS[args.topic - 1]]

    print(f"Report: {OUT_FILE}")
    print(f"Topics: {len(todo)}  (already done: {sum(1 for k,_,_,_,_ in TOPICS if k in sections)}/{len(TOPICS)})")

    if args.dry_run:
        for key, title, our_patterns, granite_files, _note in todo:
            our_text, our_paths = _read_files(our_patterns, MAX_CHARS_PER_SIDE)
            granite_text, granite_paths = _read_files(granite_files, MAX_CHARS_PER_SIDE) if granite_files else ("", [])
            status = "DONE" if key in sections else "pending"
            print(f"  [{status}] {key}: {title}")
            print(f"      our files: {len(our_paths)} ({len(our_text)} chars)")
            print(f"      granite files: {len(granite_paths)} ({len(granite_text)} chars)")
        return

    api_key = read_api_key()

    for key, title, our_patterns, granite_files, note in todo:
        if key in sections and not args.force:
            print(f"  skip (already done): {title}")
            continue
        print(f"  building prompt + querying: {title} ...", flush=True)
        prompt = build_prompt(title, our_patterns, granite_files, note)
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
