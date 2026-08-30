#!/usr/bin/env python3
"""
deepseek_render_vs_sdl_gpu_examples_research.py -- compares our C++17/
SDL_GPU render HAL against the real TheSpydog/SDL_gpu_examples repo
(https://github.com/TheSpydog/SDL_gpu_examples), a technique-demo
collection for SDL3's GPU API. Same methodology and same data-handling
boundary as deepseek_render_vs_granite_research.py -- see that file's own
doc comment for the full rationale (short version: an AI agent must not
autonomously send this repo's proprietary render architecture to an
external API; running this script requires the human to type the command
themselves).

Reference SDL_gpu_examples source was fetched read-only via `gh api
repos/TheSpydog/SDL_gpu_examples/contents/Examples/...` (public repo) into
a local scratchpad snapshot -- see SDL_GPU_EXAMPLES_REF below. Not
committed to this repo.

Context for why these two examples specifically: a live comparison pass
this session (2026-08-29) already grepped our own codebase and confirmed
compute shaders / mipmap generation / texture arrays are already
well-covered (ssao_system.cpp, rd_texture.cpp, gpu_hal_buffers_dds_array.cpp)
-- NOT gaps, so not worth a DeepSeek pass. Two real gaps survived that
triage:
  1. MSAA -- zero `sample_count`/SDL_GPU_SAMPLECOUNT hits anywhere in our
     pipeline creation code (gpu_hal.h/gpu_hal_pipeline.cpp).
  2. Indirect draw -- GpuDrawIndexedIndirect exists in the HAL (gpu_hal.h:
     469-472) but is dead code with zero call sites; NPC culling tried and
     reverted it to CPU-driven direct draws (npc_render_init.cpp, tasks
     #380/#383, comment "CPU-driven, not indirect, since 2026-08-09"). The
     terrain batched-node path (TerrainQuadtreeRenderer::DrawBatched) uses
     plain CPU-driven instanced draws, not GPU-generated indirect.

USAGE (run this yourself, e.g. via `! python3 ...` in the Claude Code
session, or from a plain terminal):
  python3 tools/research/deepseek_render_vs_sdl_gpu_examples_research.py --dry-run
  python3 tools/research/deepseek_render_vs_sdl_gpu_examples_research.py
  python3 tools/research/deepseek_render_vs_sdl_gpu_examples_research.py --topic 1
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
SDL_GPU_EXAMPLES_REF = Path("/tmp/claude-1001/-home-rdga1-rdga1prj-monkeydust/e9c60870-ac26-475f-9e9d-84b930cbfe9f/scratchpad/sdl_gpu_examples_ref")
OUT_FILE = _REPO / "docs" / "research" / "RENDER_VS_SDL_GPU_EXAMPLES_DEEPSEEK_RESEARCH.md"
KEY_FILE = Path("/home/rdga1/rdga1bot-cli-md-deepseek.txt")

API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-reasoner"
MAX_TOKENS = 32000
MAX_CHARS_PER_SIDE = 16000
MAX_LINES_PER_FILE = 300

SYSTEM_PROMPT = (
    "You are a rendering engineer evaluating whether specific SDL3 GPU API "
    "(SDL_GPU) techniques, demonstrated in the official TheSpydog/"
    "SDL_gpu_examples repo, are worth adopting in a small custom C++17 "
    "game engine. Target hardware is Intel HD 520 (Skylake-U GT2, Vulkan "
    "ANV driver) at 1280x720/60fps -- an old, low-end iGPU with several "
    "documented driver-specific hazards on this exact chip: D24_UNORM "
    "depth format causes a full GPU hang (must use D32_FLOAT); combining "
    "vert_storage_bufs>0 with frag_samplers>0 in one pipeline silently "
    "fails; combining vert_samplers>0 with frag_samplers>0 in one pipeline "
    "causes a full OS freeze (not just a silent fail) -- this exact "
    "combo is actively avoided today by splitting a terrain pipeline that "
    "needs vertex-stage texture reads (VTF) into its own pipeline with "
    "frag_samplers forced to 0. You will be given real source excerpts "
    "from both codebases. Be concrete: cite real file/function names from "
    "what you were shown, do not invent APIs you were not shown. Explicitly "
    "flag when adopting a technique would require the vert_samplers+"
    "frag_samplers combo (or another already-known hazard) on this "
    "hardware, rather than recommending it anyway. Give an honest verdict "
    "on whether this specific technique is worth adopting given these "
    "constraints, or whether a prior architectural decision in the shown "
    "code already rejected the same idea for a documented reason."
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
        "msaa",
        "MSAA: hardware multisample AA vs our current AA chain (SMAA only)",
        [
            f"{_REPO}/engine/include/monkey_dust/render/gpu_hal.h",
            f"{_REPO}/engine/src/render/gpu_hal_pipeline.cpp",
            f"{_REPO}/engine/include/monkey_dust/render/smaa_system.h",
            f"{_REPO}/engine/src/render/smaa_system.cpp",
        ],
        [
            str(SDL_GPU_EXAMPLES_REF / "TriangleMSAA.c"),
            str(SDL_GPU_EXAMPLES_REF / "Common.c"),
        ],
        "We have zero sample_count/MSAA usage anywhere in pipeline creation today -- our only AA is SMAA (post-process, engine/src/render/smaa_system.cpp), which is gated OFF on Forward tier (RenderTierSystem::IsDeferred() check) on exactly this HD 520 hardware -- meaning on our actual dev/target hardware there is currently NO anti-aliasing at all on the main forward-tier path. Assess whether SDL_GPU MSAA (as shown in TriangleMSAA.c: resolve texture, sample_count field on pipeline/texture create info) is a viable AA source specifically for the Forward-tier no-AA gap, and whether it risks any of the known Intel HD 520 SDL_GPU/ANV driver bugs described in the system prompt.",
    ),
    (
        "indirect_draw_terrain",
        "GPU indirect draw vs our CPU-driven batched terrain instancing",
        [
            f"{_REPO}/engine/include/monkey_dust/render/terrain_quadtree_renderer.h",
            f"{_REPO}/engine/src/render/terrain_quadtree_renderer.cpp",
            f"{_REPO}/engine/include/monkey_dust/render/gpu_hal.h",
        ],
        [
            str(SDL_GPU_EXAMPLES_REF / "DrawIndirect.c"),
            str(SDL_GPU_EXAMPLES_REF / "InstancedIndexed.c"),
            str(SDL_GPU_EXAMPLES_REF / "Common.c"),
        ],
        "TerrainQuadtreeRenderer::DrawBatched already does CPU-driven instanced draws (SDL_DrawGPUIndexedPrimitives with instance_count=count) reading per-node data from a texture -- not GPU-generated indirect draws. GpuDrawIndexedIndirect exists as a dead HAL primitive (gpu_hal.h:469-472, zero call sites) after NPC culling explicitly reverted an indirect-draw approach to CPU-driven direct draws (npc_render_init.cpp, tasks #380/#383) for a documented reason on this hardware. Assess honestly whether SDL_gpu_examples' DrawIndirect/InstancedIndexed patterns would offer anything DrawBatched's existing CPU-driven instancing does not already get us, or whether this is the same rejected idea in different clothing -- and if genuinely different, is it worth revisiting given the prior NPC-path rejection was for a different subsystem.",
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


def build_prompt(title: str, our_patterns, ref_files, note):
    our_text, our_paths = _read_files(our_patterns, MAX_CHARS_PER_SIDE)
    if ref_files:
        ref_text, ref_paths = _read_files(ref_files, MAX_CHARS_PER_SIDE)
    else:
        ref_text, ref_paths = "", []
    parts = [
        f'Subsystem: "{title}"',
        "",
        f"Our real code ({len(our_paths)} files read):",
        our_text if our_text else "(no matching files found)",
        "",
    ]
    if ref_paths:
        parts += [f"SDL_gpu_examples real code ({len(ref_paths)} files read):", ref_text, ""]
    else:
        parts += ["SDL_gpu_examples: no source provided for this subsystem -- see note below.", ""]
    if note:
        parts += [f"Note: {note}", ""]
    parts += [
        "Produce, in this exact order with these exact headers:",
        "## Our approach",
        "## SDL_gpu_examples approach",
        "## Comparison",
        "## Verdict",
        "(one of: worth_adopting / partially_worth_adopting / not_worth_adopting / already_equivalent),"
        " plus one sentence why",
        "## Recommendation",
        "(concrete next step if worth adopting, or why not)",
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
        "# RENDER_VS_SDL_GPU_EXAMPLES_DEEPSEEK_RESEARCH.md",
        "",
        "> Compares our render layer against the REAL TheSpydog/",
        "> SDL_gpu_examples technique-demo repo (github.com/TheSpydog/",
        "> SDL_gpu_examples) -- prompted by a direct user question about",
        "> whether SDL_GPU's own capabilities are under-used in this engine.",
        "> A prior triage pass (2026-08-29) ruled out compute/mipmaps/",
        "> texture-arrays as already-covered and identified MSAA + indirect",
        "> draw as the only two real gaps worth a deep pass.",
        "> Generated by `tools/research/deepseek_render_vs_sdl_gpu_examples_research.py`",
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
        for key, title, our_patterns, ref_files, _note in todo:
            our_text, our_paths = _read_files(our_patterns, MAX_CHARS_PER_SIDE)
            ref_text, ref_paths = _read_files(ref_files, MAX_CHARS_PER_SIDE) if ref_files else ("", [])
            status = "DONE" if key in sections else "pending"
            print(f"  [{status}] {key}: {title}")
            print(f"      our files: {len(our_paths)} ({len(our_text)} chars)")
            print(f"      ref files: {len(ref_paths)} ({len(ref_text)} chars)")
        return

    api_key = read_api_key()

    for key, title, our_patterns, ref_files, note in todo:
        if key in sections and not args.force:
            print(f"  skip (already done): {title}")
            continue
        print(f"  building prompt + querying: {title} ...", flush=True)
        prompt = build_prompt(title, our_patterns, ref_files, note)
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
