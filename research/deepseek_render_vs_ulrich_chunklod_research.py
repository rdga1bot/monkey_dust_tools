#!/usr/bin/env python3
"""
deepseek_render_vs_ulrich_chunklod_research.py -- a focused, deeper
comparison of Thatcher Ulrich's real public-domain "Chunked LOD" terrain
algorithm (source: tmp_/chunklod_reference/{chunklod,heightfield_chunker}.cpp,
the tu-testbed reference this project already ported and then rejected --
see docs/TERRAIN_CHUNKLOD_PORT_PLAN.md's CLOSED note) against our OWN
current terrain system (TerrainQuadtreeRenderer, CDLOD-family quadtree +
geomorph). Same methodology/data-handling boundary as the Godot/Granite
comparison scripts in this directory -- see deepseek_render_vs_godot_
research.py's own doc comment for the full rationale (short version: an
AI agent must not autonomously send this repo's proprietary render
architecture to an external API; running this script requires the human
to type the command themselves).

WHY A SEPARATE, DEEPER PASS: the earlier chunklod port attempt (Phase 1-5,
docs/TERRAIN_CHUNKLOD_PORT_PLAN.md) was rejected for concrete, narrow
reasons (debug-only fragment shader with no real ground texturing, no
per-node distance LOD, a cross-zone seam gap) -- none of which are
fundamental to Ulrich's ALGORITHM itself, they were gaps in how far this
project's spike implemented it. This pass asks DeepSeek to compare the
real reference algorithm (not our incomplete port of it) against our
real, currently-shipping CDLOD-style system, substantively -- LOD
selection criteria, activation-level propagation, edge/skirt handling,
memory layout -- rather than re-litigating the already-closed spike.

USAGE (run this yourself, e.g. via `! python3 ...` in the Claude Code
session, or from a plain terminal):
  python3 tools/research/deepseek_render_vs_ulrich_chunklod_research.py --dry-run
  python3 tools/research/deepseek_render_vs_ulrich_chunklod_research.py
  python3 tools/research/deepseek_render_vs_ulrich_chunklod_research.py --topic 1
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
ULRICH_REF = _REPO / "tmp_" / "chunklod_reference"
OUT_FILE = _REPO / "docs" / "research" / "RENDER_VS_ULRICH_CHUNKLOD_DEEPSEEK_RESEARCH.md"
KEY_FILE = Path("/home/rdga1/rdga1bot-cli-md-deepseek.txt")

API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-reasoner"
MAX_TOKENS = 8000
MAX_CHARS_PER_SIDE = 20000  # Ulrich's real files are dense C++, give this pass more budget
MAX_LINES_PER_FILE = 500

SYSTEM_PROMPT = (
    "You are a terrain-rendering engineer doing a substantive technical "
    "comparison of two REAL, complete terrain LOD algorithms: (1) Thatcher "
    "Ulrich's public-domain 'Chunked LOD' (Gamasutra/Game Developer "
    "Magazine 2000, 'Continuous LOD Terrain Meshing Using Adaptive "
    "Quadtrees' -- the tu-testbed reference implementation you are given "
    "verbatim), and (2) a custom C++17/SDL_GPU engine's own CDLOD-family "
    "quadtree terrain (Intel HD 520 target, no vertex-stage texture "
    "sampling risk-free, geomorph-based crack avoidance already "
    "implemented and shipping). Do not evaluate any prior PORT ATTEMPT of "
    "Ulrich's algorithm -- evaluate the ALGORITHM ITSELF as given in the "
    "reference source, on its own terms, against our real shipping "
    "system. Cite real function/struct names from what you were shown. "
    "Be substantive about the actual LOD-selection math, activation-level "
    "propagation, and edge/crack-avoidance mechanism differences -- this "
    "is a deeper technical pass, not a summary."
)


def _read_files(patterns, max_chars):
    chunks = []
    total = 0
    seen = set()
    paths = []
    for pat in patterns:
        paths.extend(sorted(globmod.glob(pat)))
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


TOPICS = [
    (
        "lod_selection_math",
        "LOD selection criteria: Ulrich's screen-space error metric vs our distance-band morph",
        [
            str(ULRICH_REF / "chunklod.cpp"),
            str(ULRICH_REF / "chunklod.h"),
        ],
        [
            f"{_REPO}/engine/src/world/terrain_quadtree.cpp",
            f"{_REPO}/engine/include/monkey_dust/world/terrain_quadtree.h",
        ],
        "Ulrich's compute_lod/can_split use a real projected screen-space vertical-error bound against a Chunk::error field baked per node at build time. Ours uses fixed camera-distance bands (TerrainQuadtree::SelectVisible) with a continuous morph factor. Compare accuracy-vs-simplicity trade-offs concretely, not just 'both are quadtree LOD'.",
    ),
    (
        "activation_level_propagation",
        "Ulrich's activation-level neighbor propagation vs our per-node independent selection",
        [
            str(ULRICH_REF / "chunklod.cpp"),
        ],
        [
            f"{_REPO}/engine/src/world/terrain_quadtree.cpp",
        ],
        "Ulrich propagates an activation_level up the quadtree so a node's LOD is never more than one level different from its neighbors (avoids T-junctions structurally, not via skirts). Ours relies on skirts (vertical drop quads at node borders) plus geomorph, not activation-level propagation. Assess whether Ulrich's structural neighbor-consistency guarantee is worth adopting to REPLACE or SUPPLEMENT our skirt-based approach, given our real HD 520 constraints (no vertex-stage texture sampling risk to reintroduce).",
    ),
    (
        "geometry_representation",
        "Baked mesh chunks (Ulrich) vs runtime height-texture sampling (ours)",
        [
            str(ULRICH_REF / "heightfield_chunker.cpp"),
        ],
        [
            f"{_REPO}/shaders/terrain_quadtree.vert",
            f"{_REPO}/engine/include/monkey_dust/render/terrain_world_heightmap.h",
        ],
        "Ulrich pre-bakes each chunk's vertex/index geometry offline into files loaded at runtime (real geometry, not procedurally sampled). We keep ONE world heightmap texture and sample it per-vertex at runtime in terrain_quadtree.vert (vertex-buffer-less, gl_VertexIndex + shared IBO). This is the single biggest structural difference -- assess memory/streaming/edit-ability trade-offs honestly, including why runtime sampling avoids the exact per-node fragment-shading gap that sank this project's own earlier chunklod port attempt (no baked ground-texture data per chunk file).",
    ),
    (
        "memory_streaming_model",
        "Per-tile file streaming (Ulrich, designed for it) vs full-world-resident heightmap (ours)",
        [
            str(ULRICH_REF / "chunklod.h"),
        ],
        [
            f"{_REPO}/engine/include/monkey_dust/render/terrain_world_heightmap.h",
        ],
        "Ulrich's own design target was worlds far larger than fit in RAM (per-quadrant streaming from disk). Our world (29.5km, real Kenshi scale) fits entirely resident as one heightmap texture already -- state explicitly whether Ulrich's streaming design solves a problem we don't have at our world size, or whether there's a real memory/load-time win being left on the table.",
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


def build_prompt(title: str, ulrich_files, our_files, note):
    ulrich_text, ulrich_paths = _read_files(ulrich_files, MAX_CHARS_PER_SIDE)
    our_text, our_paths = _read_files(our_files, MAX_CHARS_PER_SIDE)
    parts = [
        f'Topic: "{title}"',
        "",
        f"Ulrich's real Chunked LOD reference ({len(ulrich_paths)} files read):",
        ulrich_text if ulrich_text else "(no matching files found)",
        "",
        f"Our real code ({len(our_paths)} files read):",
        our_text if our_text else "(no matching files found)",
        "",
    ]
    if note:
        parts += [f"Note: {note}", ""]
    parts += [
        "Produce, in this exact order with these exact headers:",
        "## Ulrich's approach",
        "## Our approach",
        "## Technical comparison",
        "(be specific: cite the actual functions/fields/formulas from both sides)",
        "## Verdict",
        "(one of: worth_adopting / partially_worth_adopting / not_worth_adopting), plus one sentence why",
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
        "# RENDER_VS_ULRICH_CHUNKLOD_DEEPSEEK_RESEARCH.md",
        "",
        "> Focused, deeper comparison of Thatcher Ulrich's real Chunked LOD",
        "> algorithm (tmp_/chunklod_reference/, the tu-testbed public-domain",
        "> reference this project already ported once and rejected -- see",
        "> docs/TERRAIN_CHUNKLOD_PORT_PLAN.md's CLOSED note) against our OWN",
        "> current terrain (TerrainQuadtreeRenderer). Evaluates the",
        "> ALGORITHM on its own terms, not the earlier incomplete port",
        "> attempt.",
        "> Generated by `tools/research/deepseek_render_vs_ulrich_chunklod_",
        "> research.py` (deepseek-reasoner), run directly by the human user",
        "> (not by Claude) per this project's data-handling boundary for",
        "> this class of action.",
        "> Raw model output -- verify before acting on any recommendation.",
        "",
    ]
    for key, title, _u, _o, _n in TOPICS:
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
        for key, title, ulrich_files, our_files, _note in todo:
            ulrich_text, ulrich_paths = _read_files(ulrich_files, MAX_CHARS_PER_SIDE)
            our_text, our_paths = _read_files(our_files, MAX_CHARS_PER_SIDE)
            status = "DONE" if key in sections else "pending"
            print(f"  [{status}] {key}: {title}")
            print(f"      ulrich files: {len(ulrich_paths)} ({len(ulrich_text)} chars)")
            print(f"      our files: {len(our_paths)} ({len(our_text)} chars)")
        return

    if not ULRICH_REF.exists():
        print(f"ERROR: {ULRICH_REF} not found -- expected the real Ulrich reference source there", file=sys.stderr)
        sys.exit(1)

    api_key = read_api_key()

    for key, title, ulrich_files, our_files, note in todo:
        if key in sections and not args.force:
            print(f"  skip (already done): {title}")
            continue
        print(f"  building prompt + querying: {title} ...", flush=True)
        prompt = build_prompt(title, ulrich_files, our_files, note)
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
