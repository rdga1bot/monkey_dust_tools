#!/usr/bin/env python3
"""
deepseek_cathode_renderer_research.py — reconstruct how the Cathode-engine
renderer works from raw Ghidra-decompiled pseudo-C, for two real shipped
titles built on it: Viking: Battle for Asgard (2008, D3D9, PS3/X360) and
Alien: Isolation (2014, D3D11/D3D10, the engine's mature form). Companion
research to this project's own FPS/resolution investigation (South Hive
FPS bug, terrain-shading cost measurements) -- the motivating question is
concrete: both titles hit 1080p60-class targets on hardware not
meaningfully better than our own Intel HD 520 target, so what does their
actual per-frame architecture look like, and does any of it transfer.

WHY THIS SCRIPT EXISTS INSTEAD OF CLAUDE CALLING THE API DIRECTLY: same
platform-level block as tools/research/deepseek_render_vs_openmw_research.py
-- an AI agent is hard-blocked from autonomously sending source (here:
decompiled binary source, arguably more sensitive) to an external API.
Claude prepared this script and the excerpt files below; a human must run
it. Same DeepSeek account/model already used this session.

SOURCE MATERIAL: re_/VBfA/viking.exe.c (470K lines, raw Ghidra output, zero
prior renaming -- ~10.7K anonymous FUN_xxxxxxxx functions) and
re_/AI/AI.exe.c (2.7M lines, same situation). Neither file is small enough
to hand to a model whole, and neither is indexed by this project's
jCodemunch code-navigation MCP (both come back with zero symbols -- too
large / not treated as a real "language" by the indexer). Claude manually
traced call graphs from known anchor points (Direct3DCreate9 /
D3D11CreateDeviceAndSwapChain, the Win32 message-pump functions, the
per-tick subsystem-dispatch function in VBfA) and extracted the resulting
excerpts to the files below. It also pulled real engine-internal string
literals (RenderPass_PreHILOAlpha, GBuffer, setShadowMapResolution,
VelocityPass, RayleighMieSkyShader, etc.) via grep -- these are the
engine's OWN names, not Ghidra's auto-naming, and are the highest-signal
evidence extracted so far.

USAGE (run this yourself, e.g. via `! python3 ...` in the Claude Code
session, or from a plain terminal):
  python3 tools/research/deepseek_cathode_renderer_research.py --dry-run
  python3 tools/research/deepseek_cathode_renderer_research.py
  python3 tools/research/deepseek_cathode_renderer_research.py --topic 1
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
SCRATCH = Path(
    "/tmp/claude-1001/-home-rdga1-rdga1prj-monkeydust/"
    "e9c60870-ac26-475f-9e9d-84b930cbfe9f/scratchpad/cathode_re"
)
OUT_FILE = _REPO / "docs" / "research" / "CATHODE_RENDERER_DEEPSEEK_RESEARCH.md"
KEY_FILE = Path("/home/rdga1/rdga1bot-cli-md-deepseek.txt")

API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-reasoner"
MAX_TOKENS = 65536
MAX_CHARS = 24000

SYSTEM_PROMPT = (
    "You are a reverse-engineering and rendering-architecture expert. You "
    "will be given raw Ghidra-decompiled pseudo-C excerpts (unnamed "
    "FUN_xxxxxxxx functions, no debug symbols) from a shipped Windows game "
    "binary built on Creative Assembly's 'Cathode' engine, plus a list of "
    "real engine-internal string literals recovered from the same binary "
    "(these ARE the engine's own names, trustworthy unlike the Ghidra "
    "auto-names). Your job: reconstruct as much of the real renderer "
    "architecture and per-frame flow as the evidence supports. Be explicit "
    "about confidence -- distinguish 'directly shown in the excerpt' from "
    "'inferred from Win32/D3D API semantics' from 'speculative from string "
    "names alone'. Do not invent function purposes you cannot support. "
    "Where the excerpt is truncated or a called function's body wasn't "
    "provided, say so rather than guessing its contents.\n\n"
    "Context for why this matters: the requester maintains a small custom "
    "C++17/SDL_GPU indie engine (Vulkan via SDL_GPU, direct command-buffer "
    "HAL, no scene-graph, deferred+forward hybrid, screen-space terrain "
    "shading) targeting Intel HD 520 (2015 integrated GPU, 24 EU, ~15W "
    "package power budget shared between CPU+GPU) at 1920x1080 60fps for "
    "an open-world game with real terrain, NPCs, foliage, and shadows. "
    "Measured current reality: GPU pins at 100% (976-1000/1000MHz) at "
    "1920x1056 with an almost-empty test scene (bare terrain, no foliage/"
    "buildings, minimal shadow casters), yielding only 34-47fps -- a ~25-35% "
    "GPU-cost overshoot versus the 16.6ms/frame budget, worsened by CPU "
    "downclocking (2300->1800MHz) under the same shared power budget. "
    "The titles you are analyzing shipped real open-world (Viking) or "
    "densely-detailed (Alien: Isolation) content at comparable or better "
    "resolution/framerate targets on hardware from their own era that was "
    "not dramatically more powerful in raw FLOPS than a modern low-power "
    "iGPU. Where the evidence supports it, call out anything that looks "
    "like a deliberate CPU/GPU balance decision, and anything that looks "
    "like it would NOT be safe or sensible on a shared-power-budget iGPU."
)


def _read(path: str, max_lines: int = 2000) -> str:
    p = Path(path)
    if not p.exists():
        return f"(missing: {path})"
    lines = p.read_text(errors="replace").splitlines()[:max_lines]
    return "\n".join(lines)


TOPICS = [
    (
        "vbfa_render_arch",
        "Viking: Battle for Asgard (2008, D3D9) — renderer init + per-frame flow",
        str(SCRATCH / "vbfa_excerpts.txt"),
        "This excerpt contains: (1) the Direct3DCreate9 call site and its "
        "enclosing init function, (2) the Win32 message-pump function "
        "(FUN_004f6ae0, note the SleepEx(1,1) at its end), (3) a per-tick "
        "dispatcher function (FUN_005a97b0) that calls the pump followed by "
        "~20 subsystem calls in a fixed order (several repeated calls to "
        "FUN_00424570 -- figure out from ITS body what kind of dispatcher "
        "that repetition implies, e.g. a job queue drained N times, a "
        "per-viewport call, etc.), and (4) the full bodies of every distinct "
        "function that dispatcher calls. Known real string literals from "
        "this binary: \"PixelShader\", \"RayleighMieSkyShader\", \"shaders\" "
        "-- note the Rayleigh-Mie atmospheric sky scattering model is a "
        "real, deliberate, relatively expensive-looking technique choice "
        "for a 2008 console title; comment on why that was evidently "
        "affordable in their per-frame budget.",
    ),
    (
        "ai_render_arch",
        "Alien: Isolation (2014, D3D11/D3D10) — renderer init + architecture signals",
        str(SCRATCH / "ai_excerpts.txt"),
        "This excerpt contains: (1) the real D3D11CreateDeviceAndSwapChain "
        "call site with DXGI factory adapter enumeration (feature-level "
        "fallback logic visible), (2) the window-creation function (note "
        "the literal \"Alien: Isolation\" window title, confirming this is "
        "genuinely that binary), (3) the per-frame Win32 message-pump "
        "function with cursor-idle/gamepad-vs-mouse detection logic, (4) a "
        "loading/menu state-machine loop that also calls the pump (labelled "
        "here as a loading-screen candidate, NOT confirmed as the real "
        "in-game frame loop -- say so), and (5) a list of real engine "
        "string literals recovered via grep, including GBuffer, "
        "RenderPass_PreHILOAlpha, VelocityPass, DepthBufferCopy, "
        "setScreenSpaceAmbientOcclusion(Data), setShadowMapping(Data), "
        "setShadowMapResolution(Data), particleRenderAfterCompute_mGPU, "
        "StSampleAndBlendPipeline, ScaleformCommandBuffer, and hkp-prefixed "
        "Havok physics symbols. From these names alone, reconstruct the "
        "likely high-level pass structure (deferred G-buffer + SSAO + "
        "shadow pass with a RUNTIME-CONFIGURABLE resolution + velocity/"
        "motion-blur pass + compute-based particles), and be explicit that "
        "this is name-inference, not something read directly from a call "
        "site. The runtime-configurable shadow map resolution "
        "(setShadowMapResolution/Data as a distinct settable pair) is "
        "specifically relevant -- compare against a fixed-resolution "
        "shadow system.",
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


def build_prompt(title: str, excerpt_path: str, note: str) -> str:
    excerpt = _read(excerpt_path)
    if len(excerpt) > MAX_CHARS:
        excerpt = excerpt[:MAX_CHARS] + "\n... (truncated)"
    parts = [
        f'Subsystem: "{title}"',
        "",
        f"Decompiled excerpt ({excerpt_path}):",
        excerpt,
        "",
        f"Note: {note}",
        "",
        "Produce, in this exact order with these exact headers:",
        "## What's directly shown",
        "(only what the excerpt literally demonstrates -- init sequence, call order, control flow)",
        "## Inferred architecture",
        "(reasoned from API semantics + string literals; mark confidence per claim)",
        "## Relevance to a modern low-power-iGPU indie engine",
        "(concrete, honest -- what (if anything) transfers to a shared-power-budget "
        "Intel HD 520 target chasing 1080p60; call out anything that looks "
        "unsafe or inapplicable on that hardware class, not just what looks appealing)",
        "## Open questions",
        "(what would need further tracing -- e.g. specific FUN_ bodies not yet extracted -- to confirm)",
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
        "# CATHODE_RENDERER_DEEPSEEK_RESEARCH.md — VBfA + Alien: Isolation renderer RE (DeepSeek)",
        "",
        "> Generated by `tools/research/deepseek_cathode_renderer_research.py`",
        "> (deepseek-reasoner), run directly by the human user (not by Claude)",
        "> per this project's data-handling boundary for this class of action.",
        "> Source: re_/VBfA/viking.exe.c, re_/AI/AI.exe.c (raw Ghidra decompiles,",
        "> no prior symbol renaming). Raw model output -- verify before acting.",
        "",
    ]
    for key, title, _p, _n in TOPICS:
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
    print(f"Topics: {len(todo)}  (already done: {sum(1 for k,_,_,_ in TOPICS if k in sections)}/{len(TOPICS)})")

    if args.dry_run:
        for key, title, excerpt_path, _note in todo:
            excerpt = _read(excerpt_path)
            status = "DONE" if key in sections else "pending"
            print(f"  [{status}] {key}: {title}")
            print(f"      excerpt: {excerpt_path} ({len(excerpt)} chars)")
        return

    api_key = read_api_key()

    for key, title, excerpt_path, note in todo:
        if key in sections and not args.force:
            print(f"  skip (already done): {title}")
            continue
        print(f"  building prompt + querying: {title} ...", flush=True)
        prompt = build_prompt(title, excerpt_path, note)
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
