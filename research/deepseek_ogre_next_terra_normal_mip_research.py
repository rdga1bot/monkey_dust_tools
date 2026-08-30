#!/usr/bin/env python3
"""
deepseek_ogre_next_terra_normal_mip_research.py -- deep follow-up to task
#437's DeepSeek research on OGRE-Next's Terra terrain system, now scoped
tightly to ONE concrete, verified structural difference this session found
by reading real source on both sides (not the earlier, partly-unverified
"from memory" pass -- see docs/research/OGRE_NEXT_TERRA_DEEPSEEK_RESEARCH.md
in git history, commit 81a9cf06, self-flagged as working from memory
rather than a checkout).

Same methodology and same data-handling boundary as
deepseek_render_vs_granite_research.py -- see that file's own doc comment
for the full rationale (short version: an AI agent must not autonomously
send this repo's proprietary render architecture to an external API;
running this script requires the human to type the command themselves).

VERIFIED FACTS (real source read this session, 2026-08-29):
  - Our TerrainWorldHeightmap::normal_tex_ (engine/src/render/
    terrain_world_heightmap.cpp) is created with `nti.num_levels = 1` --
    NO mip chain at all, RG8_SNORM, baked once by
    shaders/terrain_worldmap_normal_bake.comp. The sampler
    (normal_sampler_) sets mipmap_mode=NEAREST, which is moot with only
    one level -- every LOD/distance always samples the exact same
    full-native-resolution normal texel grid.
  - By contrast, our HEIGHT texture (tex_, same file) DOES get a full mip
    chain (`ti.num_levels` = floor(log2(N))+1, filled via
    SDL_GenerateMipmapsForGPUTexture) and IS sampled via hardware
    trilinear mip sampling at a per-vertex computed fractional LOD (per
    terrain_world_heightmap.h's own doc comment: "hardware trilinear mip
    sampling ... never from a per-window texture").
  - Real OGRE-Next Terra.cpp (Samples/2.0/Tutorials/Tutorial_Terrain/src/
    Terra/Terra.cpp, createNormalTexture(), fetched via `gh api` this
    session) does the opposite: `m_normalMapTex->setNumMipmaps(
    PixelFormatGpuUtils::getMaxMipmapCount(...))` gives the normal texture
    a FULL mip chain matched to the height texture's resolution, using
    TextureFlags::AllowAutomipmaps on a temp RTT (box-filtered downsample
    of the ENCODED unit-normal texture, not re-derived from height per
    mip), then explicitly copies each mip level in
    (`for i in 0..numMipmaps: tmpRtt->copyTo(m_normalMapTex, ..., i, ...)`).
  - Open project bug: task #556 "Cliff speckle: live repro + shader
    root-cause" (pending) -- reproduced visually via
    tests/editor_scenarios/editor_verify_bw_pattern_ab.lua /
    editor_verify_bw_pattern_z34_36.lua ("black/white speckle" on steep
    cliff faces). Root cause not yet confirmed.
  - A prior, different session already investigated and flagged a
    DIFFERENT candidate fix for the same symptom class: storing a
    2-channel (dh/dx, dh/dz) slope map instead of a unit-normal texture,
    reasoning that slopes filter linearly under mip averaging while unit
    normals can cancel toward garbage on downsample -- this was flagged
    as *DeepSeek's own suggestion*, not something read from real Terra
    source, and has NOT been falsification-tested yet.

USAGE (run this yourself, e.g. via `! python3 ...` in the Claude Code
session, or from a plain terminal):
  python3 tools/research/deepseek_ogre_next_terra_normal_mip_research.py --dry-run
  python3 tools/research/deepseek_ogre_next_terra_normal_mip_research.py
  python3 tools/research/deepseek_ogre_next_terra_normal_mip_research.py --topic 1
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
OGRE_NEXT_TERRA_REF = Path("/tmp/claude-1001/-home-rdga1-rdga1prj-monkeydust/e9c60870-ac26-475f-9e9d-84b930cbfe9f/scratchpad/ogre_next_terra_ref")
OUT_FILE = _REPO / "docs" / "research" / "OGRE_NEXT_TERRA_NORMAL_MIP_DEEPSEEK_RESEARCH.md"
KEY_FILE = Path("/home/rdga1/rdga1bot-cli-md-deepseek.txt")

API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-reasoner"
MAX_TOKENS = 32000
MAX_CHARS_PER_SIDE = 16000
MAX_LINES_PER_FILE = 400  # Terra.cpp is ~935 lines; keep enough of createNormalTexture() and neighbors

SYSTEM_PROMPT = (
    "You are a terrain rendering engineer diagnosing a real, currently-open "
    "visual bug: 'black/white speckle' artifacts on steep cliff faces in a "
    "custom C++17/SDL_GPU terrain renderer (Intel HD 520 target). You will "
    "be given real source excerpts from our engine and from the real "
    "OGRE-Next Terra terrain system (github.com/OGRECave/ogre-next). Be "
    "concrete: cite real file/function/field names from what you were "
    "shown, do not invent APIs you were not shown. The central verified "
    "fact driving this investigation: our baked normal texture has "
    "num_levels=1 (no mip chain at all, always sampled at native "
    "resolution regardless of camera distance or LOD tier), while our "
    "height texture DOES have a full mip chain sampled via a per-vertex "
    "fractional LOD, AND real OGRE-Next Terra also gives its normal "
    "texture a full mip chain matched to height resolution (via "
    "AllowAutomipmaps + explicit per-mip RTT copy in createNormalTexture()). "
    "Assess whether this height-LOD/normal-single-level mismatch is a "
    "plausible or likely root cause of the speckle symptom -- reason "
    "through the actual mechanism (what happens when a low-poly, "
    "coarse-height-LOD triangle on a steep cliff is shaded using a normal "
    "sampled at full native-texel resolution, texel-for-texel, with no "
    "distance-appropriate smoothing). Also assess honestly whether "
    "copying OGRE-Next's exact fix (box-filtered automipmaps of the "
    "encoded normal texture) is itself correct, or whether it has its own "
    "known flattening risk on steep slopes (unit normals can partially "
    "cancel under naive box-filter averaging) that would trade one "
    "artifact for another -- and if so, compare against the untested "
    "alternative already on file: storing a 2-channel slope map "
    "(dh/dx, dh/dz) instead of a unit-normal texture, since slopes filter "
    "linearly under mip averaging. Give a genuinely honest verdict, not a "
    "reflexive 'yes, adopt this real engine's technique.'"
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
        "normal_mip_chain",
        "Normal texture LOD handling: single-level (ours) vs full automipmapped chain (Terra)",
        [
            f"{_REPO}/engine/include/monkey_dust/render/terrain_world_heightmap.h",
            f"{_REPO}/engine/src/render/terrain_world_heightmap.cpp",
            f"{_REPO}/shaders/terrain_worldmap_normal_bake.comp",
        ],
        [
            str(OGRE_NEXT_TERRA_REF / "Terra.h"),
            str(OGRE_NEXT_TERRA_REF / "Terra.cpp"),
        ],
        "Focus specifically on TerrainWorldHeightmap's normal_tex_ creation (num_levels=1, RG8_SNORM, mipmap_mode=NEAREST sampler) vs Terra::createNormalTexture()/destroyNormalTexture() (full mip chain via AllowAutomipmaps, per-mip RTT copy loop). Also note our height texture (tex_, same file) DOES get a full SDL_GenerateMipmapsForGPUTexture chain -- the asymmetry is normal-vs-height, not us-vs-Terra on height.",
    ),
    (
        "cliff_speckle_hypothesis",
        "Is the normal-mip mismatch the root cause of task #556's cliff speckle?",
        [
            f"{_REPO}/engine/include/monkey_dust/render/terrain_world_heightmap.h",
            f"{_REPO}/engine/src/render/terrain_world_heightmap.cpp",
            f"{_REPO}/engine/include/monkey_dust/render/terrain_quadtree_renderer.h",
        ],
        [
            str(OGRE_NEXT_TERRA_REF / "Terra.cpp"),
        ],
        "This is the highest-value question: does the described mismatch (coarse-LOD triangle geometry + always-full-resolution single-level normal sample) actually produce a speckle-like artifact specifically on STEEP faces (where per-texel normal variance from the source heightmap's fine detail is largest), consistent with the symptom already reproduced in tests/editor_scenarios/editor_verify_bw_pattern_ab.lua and editor_verify_bw_pattern_z34_36.lua ('black/white speckle', 'salmon discoloration on steep cliff faces')? If yes, is Terra's own fix (box-filtered automipmapped normal texture) actually the right one, or does it just trade this artifact for a different flattening artifact on the same steep faces? Compare against the untested slope-map (dh/dx, dh/dz) alternative and give a concrete, falsifiable recommendation for task #556's next step.",
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
        parts += [f"OGRE-Next Terra real code ({len(ref_paths)} files read):", ref_text, ""]
    else:
        parts += ["OGRE-Next Terra: no source provided for this subsystem -- see note below.", ""]
    if note:
        parts += [f"Note: {note}", ""]
    parts += [
        "Produce, in this exact order with these exact headers:",
        "## Our approach",
        "## OGRE-Next Terra approach",
        "## Mechanism analysis",
        "(reason through what actually happens on a steep cliff face, step by step)",
        "## Verdict",
        "(one of: confirmed_likely_cause / plausible_contributing_factor / unlikely_cause / cannot_determine),"
        " plus one sentence why",
        "## Recommendation",
        "(concrete, falsifiable next step for task #556 -- e.g. what to test first, "
        "and whether Terra's fix or the slope-map alternative is the better bet)",
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
        "# OGRE_NEXT_TERRA_NORMAL_MIP_DEEPSEEK_RESEARCH.md",
        "",
        "> Tight follow-up to task #437's OGRE-Next Terra research, scoped",
        "> to ONE verified structural difference found by reading real",
        "> source on both sides this session (2026-08-29): our baked normal",
        "> texture has num_levels=1 (no mip chain), while both our own",
        "> height texture AND real OGRE-Next Terra's normal texture have a",
        "> full mip chain matched to LOD. Directly relevant to the still-open",
        "> task #556 (cliff speckle, root cause not yet confirmed).",
        "> Generated by `tools/research/deepseek_ogre_next_terra_normal_mip_research.py`",
        "> (deepseek-reasoner), run directly by the human user (not by Claude)",
        "> per this project's data-handling boundary for this class of action.",
        "> Raw model output -- verify before acting on any recommendation,",
        "> and falsification-test before implementing (per feedback_terrain_visual_guessing).",
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
