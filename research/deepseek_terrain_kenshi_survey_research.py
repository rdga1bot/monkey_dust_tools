#!/usr/bin/env python3
"""
deepseek_terrain_kenshi_survey_research.py — 30-річний історичний огляд
прикладів побудови поверхні ландшафту в іграх "в дусі" Kenshi (не
AAA-blockbuster з необмеженим бюджетом, а sandbox/open-world/RPG на
скромнішому рушії) через DeepSeek API.

Контекст: два попередні дослідження цієї сесії (deepseek_terrain_geometry_
research.py, deepseek_terrain_perf_research.py) були вузько сфокусовані на
конкретних технічних проблемах нашого движка. Це третє — ширший історичний/
порівняльний огляд за трьома критеріями (оригінальність, продуктивність,
популярність), щоб зрозуміти контекст: що з відомого за 30 років найбільше
заслуговує на увагу для проєкту схожого профілю з Kenshi.

USAGE:
  python3 tools/research/deepseek_terrain_kenshi_survey_research.py
  python3 tools/research/deepseek_terrain_kenshi_survey_research.py --dry-run
  python3 tools/research/deepseek_terrain_kenshi_survey_research.py --topic 3
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
OUT_FILE = _REPO / "docs" / "research" / "TERRAIN_KENSHI_LIKE_SURVEY_DEEPSEEK_RESEARCH.md"
KEY_FILE = Path("/home/rdga1/rdga1bot-cli-md-deepseek.txt")

API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-reasoner"
MAX_TOKENS = 48000

SYSTEM_PROMPT = (
    "You are a games-industry technical historian and rendering engineer "
    "with deep, accurate knowledge of terrain rendering technology across "
    "the last 30 years (roughly 1996-2026). The context is a small, "
    "low-budget open-world sandbox RPG engine (C++17, SDL_GPU/Vulkan, "
    "targeting low-end Intel HD 520-class integrated GPUs) whose explicit "
    "design goal is parity with the ORIGINAL Kenshi (Lo-Fi Games, "
    "Ogre3D-based, small-team, released 2018 after ~12 years of "
    "development, known for a large, rugged, low-poly-but-readable open "
    "world with modest production values, not photorealistic). Be "
    "concrete and historically accurate: name real games, real engines, "
    "real developers, real release years, real technical papers/GDC talks "
    "where they exist. Do not invent details you are not confident about; "
    "say so explicitly if you are uncertain about a specific claim. "
    "Structure with markdown headers and bullet lists. This is for "
    "engineers doing a literature/prior-art survey, not marketing copy."
)

TOPICS = [
    (
        "1_historical_overview",
        "30-річний хронологічний огляд побудови поверхні ландшафту",
        """Give a chronological survey (roughly 1996 to today) of significant
terrain SURFACE CONSTRUCTION techniques (heightfield/mesh geometry and LOD
specifically -- not shading/texturing, which is a separate topic) used in
games, with a DELIBERATE focus on titles with a similar profile to Kenshi:
small-to-mid budget, open-world or large-map sandbox/RPG/survival games,
not top-tier AAA blockbusters with unlimited budgets. For each era/
technique, name: the real game(s) that introduced or popularized it, the
approximate year, the core algorithm (ROAM, geomipmapping, chunked LOD,
CDLOD, geometry clipmaps, quadtree, brushed/sculpted heightfields, voxel/
marching-cubes terrain, etc.), and why it mattered at the time. Cover at
minimum: the Tribes/Genesis3D-era chunked terrain LOD wave (~1998-2002),
Trespasser's terrain (technically ambitious but infamous), early Bethesda/
Gamebryo-style terrain (Morrowind/Oblivion era), the ROAM/geomipmapping
academic techniques and which shipped games actually used them, mid-2000s
Source/CryEngine/Frostbite terrain, the indie/small-team terrain wave
(Minecraft's voxel approach as a genre-defining outlier, Don't Starve/
Stardew-adjacent simpler approaches, Kenshi's own Ogre3D terrain ~2013-2018
development), and 2010s-2020s CDLOD-descendant systems (Horizon Zero Dawn,
Ghost of Tsushima, No Man's Sky's procedural approach as a contrast case).
Be explicit about which of these were built by small teams on modest
budgets (Kenshi's actual peer group) versus which required AAA-scale
resources.""",
    ),
    (
        "2_originality",
        "Найоригінальніші підходи (за 30 років)",
        """Rank the most ORIGINAL/innovative terrain surface-construction
approaches of the last 30 years -- meaning genuinely novel algorithmic or
architectural ideas at the time they shipped or were published, not just
competent reuse of existing techniques. For each: what specifically was
novel (compare explicitly to what came before), the real game/engine/paper
it came from, and whether the idea was later widely copied or remained a
one-off curiosity. Consider strong candidates such as: ROAM (Duchaineau et
al. 1997, academic but hugely influential), geometry clipmaps (Losasso &
Hoppe 2004 / Microsoft, used in Flight Simulator), Trespasser's terrain
deformation and physics-driven approach (1998, ambitious but poorly
executed), Minecraft's fully voxel/destructible terrain as a genre-defining
original idea (2009-2011), No Man's Sky's fully procedural planet-scale
terrain (2016), Dwarf Fortress's abstracted/simulation-first terrain
(originality in service of simulation depth over visual fidelity, relevant
given Kenshi's own simulation-heavy design philosophy), and any other
genuinely original approach you have real knowledge of, including from
smaller/lesser-known titles. Be honest about which "original" ideas
actually solved a real problem well versus which were original but
ultimately not that useful.""",
    ),
    (
        "3_performance",
        "Найпродуктивніші підходи (найкраще співвідношення якість/вартість)",
        """Rank terrain surface-construction approaches from the last 30
years by PERFORMANCE -- specifically, the best achieved visual quality per
unit of CPU/GPU cost, with particular attention to techniques that worked
well on LOW-TO-MID-RANGE hardware of their era (not the top-end GPU of the
day). This is the most directly relevant criterion for our low-end Intel
HD 520-class target. For each candidate: the real technique, the game(s)
that shipped it, and concrete evidence of its efficiency (real triangle/
draw-call budgets, target hardware, frame-rate achieved, or direct
engineering commentary from the developers if you have real knowledge of
it). Consider: geomipmapping's original appeal (cheap, simple, ran on late-
90s/early-2000s hardware), chunked LOD (Ulrich, used in various indie/mid-
budget titles precisely because it was cheap to implement and run),
Minecraft's greedy-meshing voxel optimization (a real, well-documented
performance technique for a very different terrain representation),
mobile/handheld terrain techniques (which had to be extremely frugal --
any notable examples), and any small-team open-world game you know shipped
convincing large-scale terrain on genuinely modest hardware budgets similar
to ours. Also flag which "performant" techniques from this list are the
best FIT for a CDLOD-style, vertex-texture-fetch, discrete-tier system
like ours specifically (as opposed to requiring a fundamentally different
architecture).""",
    ),
    (
        "4_popularity",
        "Найпопулярніші/найбільш поширені підходи",
        """Rank terrain surface-construction approaches from the last 30
years by POPULARITY -- meaning real, verifiable breadth of adoption: how
many shipped games used it, whether it became a de facto industry
standard, whether it was cloned/reimplemented by many other engines
(commercial and open-source), and whether it is still the default choice
today. This is different from originality or raw performance -- a
technique can be popular because it is simply the well-understood, safe,
widely-taught choice. Consider: geomipmapping/chunked-LOD-family techniques
as the most widely reimplemented pattern in open-source engines (OGRE,
Urho3D/rbfx, Godot's various terrain plugins, countless tutorials and
hobbyist implementations) precisely because they are simple to explain and
implement, CDLOD (Strugar) specifically as a technique that became a
near-default reference point cited by a disproportionate number of later
implementations relative to how recently it was published, Unity Terrain
and Unreal Landscape as the two most POPULAR terrain systems by sheer
number of shipped titles (because they are the built-in defaults in the
two most widely used commercial engines, not necessarily because they are
the most sophisticated), and voxel-based terrain (Minecraft-inspired) as
its own separate popularity wave especially in the indie space post-2011.
Be explicit about the distinction between "popular because genuinely best"
versus "popular because it's the default in a popular engine".""",
    ),
    (
        "5_kenshi_peer_group",
        "Проєкти, найближчі за духом до Kenshi",
        """Narrow the focus specifically to terrain systems from games with
a genuinely similar PROFILE to Kenshi (Lo-Fi Games): built by a small team
(not hundreds of developers), released with a large open world but modest
production values, prioritizing simulation depth / systemic gameplay over
visual fidelity, terrain that reads as functional and "gritty"/readable
rather than photorealistic, likely built on a licensed or open-source
engine rather than a fully custom AAA engine. Identify real games that fit
this profile and describe (to the extent you have real knowledge) how
their terrain was actually built -- even if public technical detail is
thin, say what IS publicly known versus what is reasonable inference.
Consider: other Ogre3D-based indie open-world titles from a similar era,
Mount & Blade series (own custom engine, similarly rugged/functional
terrain, small original team), Dwarf Fortress (abstracted terrain but
extreme simulation depth, the philosophical peer even if not visually
comparable), RimWorld (much smaller scale but same small-team-simulation-
first design philosophy), Project Zomboid, Wurm Online/Unlimited (small
team, large persistent open world, notably janky but functional terrain --
a real cautionary/comparison case), and any other small-team open-world
sandbox/RPG you have genuine knowledge of. Explicitly rank how Kenshi's
OWN terrain approach (Ogre3D built-in Terrain component + custom ground_type
splatting, per prior RE research this project has done) compares to this
peer group -- was it ahead, behind, or typical for its class and era.""",
    ),
    (
        "6_final_synthesis",
        "Фінальний синтез: топ-10-15 і що вивчати нам",
        """Synthesize the previous analysis into a single consolidated
ranked list of the 10-15 terrain surface-construction examples from the
last 30 years that most deserve attention, combining originality,
performance, and popularity into an overall judgment (state explicitly
which criterion dominates for each entry -- don't just average blindly).
For each entry: one-paragraph justification citing the specific criteria
it excels at. Then, given our own project's real constraints (C++17,
SDL_GPU/Vulkan HAL with a single-color-attachment-per-pipeline limitation,
Intel HD 520-class integrated GPU target, CDLOD-style discrete-tier
vertex-texture-fetch terrain we already have working, explicit goal of
Kenshi visual/gameplay parity, small solo-or-tiny-team development
capacity), give a direct, opinionated recommendation: which 2-3 entries
from your top-10-15 list are MOST worth us actually studying in more
technical depth next (source code, papers, GDC talks), and which ones --
even if historically important -- are a poor fit for our specific
situation and not worth further investigation. Be decisive, not wishy-
washy; this is meant to guide real engineering priority, not just be an
encyclopedia entry.""",
    ),
    (
        "7_open_source_engines_studyable",
        "Open-source рушії з реально доступним кодом термейну (O3DE та інші)",
        """Separate from the popularity/originality/performance rankings
above, give a focused survey specifically of CURRENTLY PUBLIC, OPEN-SOURCE
game engines whose terrain surface-construction code we could actually
read and study directly (source available, not just papers/talks about a
closed engine). For each: the real repo, whether it is still actively
maintained (2025-2026), the terrain system's actual architecture (heightfield/
mesh representation, LOD scheme, whether it is CDLOD-descended or something
else), and how mature/production-proven it is (shipped real games, or
mostly a tech demo). Cover explicitly:
- O3DE (Open 3D Engine, Linux Foundation, originally Amazon Lumberyard/
  CryEngine-derived) -- its terrain gem/system specifically: architecture,
  how it differs from CryEngine's original terrain (which O3DE's lineage
  traces back to), whether it's a good reference despite being a much
  larger/heavier engine than ours.
- Godot's Terrain3D plugin (already partially covered in this session's
  prior geometry research, but revisit specifically for how mature/
  production-proven it is as of 2025-2026, and any real games shipped with it).
- rbfx / Urho3D's Terrain component (already covered in this session's
  prior research -- do not repeat in depth, just note it for completeness).
- Bevy Engine's terrain ecosystem (Rust, ECS-based, ecosystem of third-party
  terrain plugins rather than one built-in system -- name the most credible
  ones if you have real knowledge).
- Stride3D (formerly Xenko) -- terrain support if any.
- Any other genuinely real, currently public open-source engine with a
  terrain system worth knowing about that you have real knowledge of
  (do not pad the list with engines you are unsure still exist or are
  unsure have real terrain code).
For each, explicitly assess: is this a realistic thing for a small project
like ours (C++17, custom SDL_GPU HAL, not adopting a whole external engine)
to actually mine ideas/code from, or is it too architecturally entangled
with its own engine to extract cleanly? Be honest if an engine is not
actually a good fit despite being well-known.""",
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
        "# TERRAIN_KENSHI_LIKE_SURVEY_DEEPSEEK_RESEARCH.md — 30-річний огляд "
        "прикладів побудови ландшафту в дусі Kenshi (DeepSeek)",
        "",
        "> Згенеровано `tools/research/deepseek_terrain_kenshi_survey_"
        "research.py` (deepseek-reasoner). Ширший історичний/порівняльний "
        "огляд (не вузько наша конкретна проблема) за трьома критеріями: "
        "оригінальність, продуктивність, популярність. Це СИРІ відповіді "
        "моделі — потребують верифікації перед будь-якими висновками.",
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
