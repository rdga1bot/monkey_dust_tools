#!/usr/bin/env python3
"""
md_doctor.py — mechanical drift checks between *.md documentation and the
real repo state (PROMPT_DOC_RESTRUCTURE.md, Phase 1).

Axiom this whole tool exists to enforce: code is truth, docs are a
hypothesis until mechanically confirmed (see docs/DOC_RECON.md Phase 0,
which found >=8 confirmed drifts by hand -- this tool makes that
repeatable instead of a one-off audit).

Each DOC-XX check is intentionally NARROW rather than clever: Phase 0
found that naive greps (bare "IsKeyDown", bare "MINIAUDIO_IMPLEMENTATION")
produce false positives against legitimate code (ImGui::IsKeyPressed, a
lone #define vs mere mentions) -- every check here was written against
that lesson, verified against the real repo before being called "done".

Usage:
  tools/md_doctor.py [--json] [--build-dir build]

Output: "N passed, M failed" on stdout, one line per failure:
  "DOC-XX: <file:line> <what>"
Exit code: 0 if M==0, else 1.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent

# .md files in scope: main-repo root + docs/** (matches DOC_RECON.md Phase 0
# item 1's own scope -- NOT re/re_docs/, tmp_/, private/, docs/re_assets/,
# which are out of the prompt's §0/§2 perimeter).
def _md_files():
    files = sorted(REPO_ROOT.glob("*.md"))
    files += sorted((REPO_ROOT / "docs").rglob("*.md"))
    files = [f for f in files if "re_assets" not in f.parts]
    return files


def _read(path):
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


# ── DOC-01: every backtick-quoted path-looking string in *.md exists ────────
# Deliberately narrow (first cut checked ANY slash-or-extension backtick
# token -> 853 findings, almost all noise: template placeholders like
# `TIMESTAMP`/`CAPTURE_ID`/`X.Y`, bare script names mentioned in prose with
# no path context, and legitimately-external paths like the Kenshi Steam
# mount or gitignored source data files described as such). Narrowed to:
# must contain '/', must start with a REAL top-level repo dir, must not
# contain an all-caps placeholder segment.
_PATH_RE = re.compile(r"`([A-Za-z0-9_./-]+/[A-Za-z0-9_./-]+)`")
_KNOWN_TOP_DIRS = {
    "engine", "game", "tools", "docs", "shaders", "tests", "data",
    "re_docs", "private", "temp_", "tmp_", ".claude",
}
_PLACEHOLDER_SEGMENT_RE = re.compile(r"^(?:[A-Z][A-Z_]*|X|Y|N|ID)$")

def check_doc01_paths_exist(md_files):
    fails = []
    for f in md_files:
        text = _read(f)
        for lineno, line in enumerate(text.splitlines(), 1):
            for m in _PATH_RE.finditer(line):
                token = m.group(1)
                if "://" in token:  # URL, not a repo path
                    continue
                segments = token.split("/")
                if segments[0] not in _KNOWN_TOP_DIRS:
                    continue  # not claiming to be a repo-relative path at all
                if any(_PLACEHOLDER_SEGMENT_RE.match(s) for s in segments):
                    continue  # template placeholder (TIMESTAMP, CAPTURE_ID, X.Y, ...)
                # "foo/bar.h/.cpp" convention used throughout this repo's docs
                # means "both foo/bar.h AND foo/bar.cpp exist", not a literal
                # single path containing that slash -- split and check both.
                m2 = re.match(r"^(.*)\.([A-Za-z0-9]+)/\.([A-Za-z0-9]+)$", token)
                if m2:
                    base, ext1, ext2 = m2.groups()
                    variants = [f"{base}.{ext1}", f"{base}.{ext2}"]
                else:
                    variants = [token]
                missing = [v for v in variants if not (REPO_ROOT / v).exists()]
                if missing:
                    fails.append(f"DOC-01: {f.relative_to(REPO_ROOT)}:{lineno} path `{token}` -- missing: {missing}")
    return fails


# ── DOC-02: no *.md references a file outside the repo ──────────────────────
_HOME_RE = re.compile(r"(~/[A-Za-z0-9_./-]+|/home/[A-Za-z0-9_./-]+)")

def check_doc02_no_external_refs(md_files):
    fails = []
    for f in md_files:
        text = _read(f)
        for lineno, line in enumerate(text.splitlines(), 1):
            for m in _HOME_RE.finditer(line):
                fails.append(f"DOC-02: {f.relative_to(REPO_ROOT)}:{lineno} references path outside repo: `{m.group(1)}`")
    return fails


# ── DOC-03: test count in *.md == real ctest -N / gtest count ───────────────
_TEST_COUNT_RE = re.compile(r"(\d+)\s*(?:тест[іио]в?|gtest|tests?)\b", re.IGNORECASE)

def _real_test_count(build_dir):
    gtest_bin = REPO_ROOT / build_dir / "tests" / "md_tests"
    behavior_bin = REPO_ROOT / build_dir / "tests" / "md_behavior_tests"
    counts = {}
    for name, bin_path in (("gtest", gtest_bin), ("behavior", behavior_bin)):
        if not bin_path.exists():
            continue
        try:
            out = subprocess.run([str(bin_path), "--gtest_list_tests"],
                                  capture_output=True, text=True, timeout=30).stdout
            # gtest --gtest_list_tests prints "Suite." then indented test
            # names -- count indented lines (2-space prefix), not suite headers.
            counts[name] = sum(1 for l in out.splitlines() if l.startswith("  ") and not l.startswith("   "))
        except (subprocess.TimeoutExpired, OSError):
            pass
    return counts

# Files whose ROLE is historical record (append-only changelog) or a dated
# point-in-time snapshot, not a current-state claim -- a past test count
# quoted there is correct BY DEFINITION for that point in time, not drift.
# (Phase 0's own STATE-vs-HISTORY classification, docs/DOC_RECON.md item 1.)
_HISTORICAL_FILES = {"CLAUDE_HISTORY.md"}
_HISTORICAL_NAME_RE = re.compile(r"AUDIT|_20\d{2}-\d{2}-\d{2}")

def _is_historical(f):
    if f.name in _HISTORICAL_FILES:
        return True
    return bool(_HISTORICAL_NAME_RE.search(f.name))

def check_doc03_test_count(md_files, build_dir):
    real = _real_test_count(build_dir)
    if not real:
        return ["DOC-03: SKIP -- no built test binaries found under " + str(build_dir) + "/tests/ (build first)"]
    real_total = sum(real.values())
    fails = []
    for f in md_files:
        if _is_historical(f):
            continue
        text = _read(f)
        for lineno, line in enumerate(text.splitlines(), 1):
            for m in _TEST_COUNT_RE.finditer(line):
                claimed = int(m.group(1))
                # Only flag claims in the plausible range for a total test
                # count (avoids false positives on unrelated small numbers
                # like "8 тестів" in an unrelated sentence about something
                # else entirely) -- narrow window around real_total.
                if claimed > 50 and abs(claimed - real_total) > 0:
                    fails.append(f"DOC-03: {f.relative_to(REPO_ROOT)}:{lineno} claims {claimed} tests, real count is {real_total} ({real})")
    return fails


# ── DOC-04: stack library names in *.md confirmed via grep ──────────────────
# (library_name_as_written_in_docs, positive_grep_pattern, must_be_absent_pattern_or_None)
_STACK_CLAIMS = [
    ("EnTT", None, r"entt::"),          # EnTT should NOT appear functionally if flecs replaced it
    ("flecs", r"flecs::", None),
    ("Raylib", None, r"#include\s*[<\"]raylib\.h"),
    ("Jolt", r"JoltPhysics|Jph::", None),
]

def _grep_count(pattern, where):
    try:
        out = subprocess.run(["grep", "-rlE", pattern] + [str(REPO_ROOT / w) for w in where],
                              capture_output=True, text=True, timeout=30).stdout
        lines = [l for l in out.splitlines() if "third_party" not in l]
        return len(lines)
    except OSError:
        return 0

def check_doc04_stack_names(md_files):
    fails = []
    where = ["engine/src", "engine/include", "game/src", "tools/editor"]
    for name, must_exist, must_be_absent in _STACK_CLAIMS:
        mentioned_in = [f for f in md_files if re.search(r"\b" + re.escape(name) + r"\b", _read(f))]
        if not mentioned_in:
            continue
        if must_exist and _grep_count(must_exist, where) == 0:
            fails.append(f"DOC-04: '{name}' claimed in {len(mentioned_in)} doc(s) (e.g. {mentioned_in[0].relative_to(REPO_ROOT)}) but 0 code matches for `{must_exist}`")
        if must_be_absent and _grep_count(must_be_absent, where) > 0:
            fails.append(f"DOC-04: '{name}' claimed in {len(mentioned_in)} doc(s) (e.g. {mentioned_in[0].relative_to(REPO_ROOT)}) but code still contains `{must_be_absent}`")
    return fails


# ── DOC-05: canonical file registry vs disk (provisional -- see DOC_RECON.md
# item "2 structural gaps": no single owner exists yet for this list until
# Phase 2 resolves it, so this check reports against the union of every
# "Ключові файли"-style bullet path currently in CLAUDE.md, informationally.
def check_doc05_file_registry(md_files):
    claude_md = REPO_ROOT / "CLAUDE.md"
    if not claude_md.exists():
        return ["DOC-05: SKIP -- CLAUDE.md not found"]
    text = _read(claude_md)
    listed = set(re.findall(r"`((?:engine|game|tools)/[A-Za-z0-9_./-]+\.(?:cpp|h|hpp))`", text))
    real = set()
    for d in ("engine/src", "engine/include", "game/src"):
        real |= {str(p.relative_to(REPO_ROOT)) for p in (REPO_ROOT / d).rglob("*") if p.suffix in (".cpp", ".h", ".hpp")}
    missing = real - listed
    fails = []
    if missing:
        sample = sorted(missing)[:5]
        fails.append(f"DOC-05: {len(missing)} .cpp/.h files not in CLAUDE.md's file listing (provisional check, expected to fail until Phase 2 assigns a single owner -- sample: {sample})")
    return fails


# ── DOC-06: no 📋-marked (planned) item has a matching file on disk ─────────
_PLANNED_RE = re.compile(r"📋.*?`([A-Za-z0-9_./-]+\.(?:cpp|h|hpp|py))`")

def check_doc06_planned_not_built(md_files):
    fails = []
    for f in md_files:
        text = _read(f)
        for lineno, line in enumerate(text.splitlines(), 1):
            m = _PLANNED_RE.search(line)
            if m and (REPO_ROOT / m.group(1)).exists():
                fails.append(f"DOC-06: {f.relative_to(REPO_ROOT)}:{lineno} marks `{m.group(1)}` 📋 (planned) but the file already exists")
    return fails


# ── DOC-07: ADR pairs from docs/DOC_ADR_PAIRS.txt ────────────────────────────
def check_doc07_adr_pairs():
    pairs_file = REPO_ROOT / "docs" / "DOC_ADR_PAIRS.txt"
    if not pairs_file.exists():
        return ["DOC-07: SKIP -- docs/DOC_ADR_PAIRS.txt not found (Phase 0 deliverable)"]
    history = _read(REPO_ROOT / "CLAUDE_HISTORY.md")
    fails = []
    for line in _read(pairs_file).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split("|", 2)]
        if len(parts) != 3:
            continue
        adr_a, adr_b, note = parts
        if adr_b == "CODE":
            # mechanical checks embedded in the note as "grep ... -- if X, DRIFT"
            if "flecs::" in note and _grep_count("flecs::", ["engine/src", "engine/include", "game/src"]) > 0:
                fails.append(f"DOC-07: {adr_a} -- {note.split('--')[0].strip()} (mechanically confirmed via grep, see docs/DOC_ADR_PAIRS.txt)")
            elif "raylib" in note.lower() and _grep_count(r"#include\s*[<\"]raylib\.h", ["engine/src", "engine/include"]) == 0:
                pass  # ADR-1 lives, no fail
        elif "ozz_animation_offline" in note:
            try:
                cmakelists = _read(REPO_ROOT / "engine" / "CMakeLists.txt")
                if "ozz_animation_offline" in cmakelists:
                    fails.append(f"DOC-07: {adr_a} vs {adr_b} -- ozz_animation_offline IS linked in engine/CMakeLists.txt, contradicting {adr_b}'s 'tools/ only' premise (docs/DOC_ADR_PAIRS.txt)")
            except OSError:
                pass
        if adr_a not in history:
            fails.append(f"DOC-07: {adr_a} referenced in docs/DOC_ADR_PAIRS.txt but not found in CLAUDE_HISTORY.md")
    return fails


# ── DOC-08: header date not older than file's last git-commit date ──────────
_DATE_RE = re.compile(r"(20\d{2}-\d{2}-\d{2})")

def _git_last_commit_date(path):
    try:
        out = subprocess.run(["git", "log", "-1", "--format=%ad", "--date=short", "--", str(path)],
                              cwd=REPO_ROOT, capture_output=True, text=True, timeout=10).stdout.strip()
        return out or None
    except OSError:
        return None

def check_doc08_header_date(md_files):
    fails = []
    for f in md_files:
        text = _read(f)
        head = "\n".join(text.splitlines()[:5])
        m = _DATE_RE.search(head)
        if not m:
            continue
        header_date = m.group(1)
        commit_date = _git_last_commit_date(f)
        if commit_date and header_date > commit_date:
            fails.append(f"DOC-08: {f.relative_to(REPO_ROOT)} header date {header_date} is AFTER last git commit date {commit_date} (impossible -- header likely hand-edited without a commit, or copy-pasted)")
    return fails


# ── DOC-09: binary formats mentioned in docs have magic+version in code ─────
_KNOWN_FORMATS = {
    ".r16": ("tools/md_hmap_io.py", None),   # raw, no magic by design (documented as such)
    ".mdsave": ("game/src/save/world_serializer.h", r"SaveVersion|kSaveVersion|MAGIC"),
}

def check_doc09_binary_formats(md_files):
    fails = []
    for ext, (owner_file, version_pattern) in _KNOWN_FORMATS.items():
        if version_pattern is None:
            continue
        owner = REPO_ROOT / owner_file
        if not owner.exists():
            fails.append(f"DOC-09: {ext} format's documented owner {owner_file} does not exist")
            continue
        if not re.search(version_pattern, _read(owner)):
            fails.append(f"DOC-09: {ext} format owner {owner_file} has no version/magic constant matching `{version_pattern}`")
    return fails


# ── DOC-10: root CLAUDE.md <= line limit ─────────────────────────────────────
def check_doc10_root_line_limit(limit):
    claude_md = REPO_ROOT / "CLAUDE.md"
    if not claude_md.exists():
        return ["DOC-10: SKIP -- CLAUDE.md not found"]
    n = len(_read(claude_md).splitlines())
    if n > limit:
        return [f"DOC-10: CLAUDE.md is {n} lines, limit is {limit} (expected to fail before Phase 2 runs)"]
    return []


# ── Knowledge Library checks (DOC-11/12/13) ──────────────────────────────────
# Broader corpus than _md_files() (root+docs/** only): the knowledge library
# covers the full public+private corpus -- root, docs/**, re/re_docs/** (private
# RE data; DOC-01..10 exclude it, but frontmatter/dedup checks are metadata-
# only and don't touch its content) and each submodule's own root .md files.
# See docs/KNOWLEDGE_LIBRARY.md for the schema these three checks validate.
def _kb_files():
    files = sorted(REPO_ROOT.glob("*.md"))
    files += sorted((REPO_ROOT / "docs").rglob("*.md"))
    files = [f for f in files if "re_assets" not in f.parts]
    re_docs_dir = REPO_ROOT / "re" / "re_docs"
    if re_docs_dir.exists():
        files += sorted(re_docs_dir.rglob("*.md"))
    for sub in ("engine", "tools"):
        sub_dir = REPO_ROOT / sub
        if sub_dir.exists():
            files += sorted(sub_dir.glob("*.md"))
    return files


_FRONTMATTER_RE = re.compile(r"^---\n(.*?)\n---\n", re.DOTALL)

def _parse_frontmatter(text):
    """None = no frontmatter block at all (opt-in: DOC-11/12/13 silently
    skip these, matching the knowledge library's incremental-rollout design
    -- nobody is forced to migrate). {} (falsy, not None) = a --- block
    exists but failed to parse as YAML, or parsed to a non-dict -- callers
    must treat that as a real DOC-11 finding, not a skip."""
    m = _FRONTMATTER_RE.match(text)
    if not m:
        return None
    try:
        data = yaml.safe_load(m.group(1))
        return data if isinstance(data, dict) else {}
    except yaml.YAMLError:
        return {}


# ── DOC-11: frontmatter completeness (opt-in per file -- see _parse_
# frontmatter's own doc comment for why files with none are skipped, not
# flagged) ────────────────────────────────────────────────────────────────
_KB_REQUIRED_FIELDS = ("id", "type", "status", "date", "updated", "repo", "tags", "summary")
_KB_TYPE_ENUM = {"investigation", "decision", "reference", "architecture", "research", "historical"}
_KB_STATUS_ENUM = {"active", "archived", "superseded", "historical"}

def check_doc11_frontmatter_complete(kb_files):
    fails = []
    for f in kb_files:
        text = _read(f)
        if not text.startswith("---\n"):
            continue
        fm = _parse_frontmatter(text)
        if fm is None:
            continue
        rel = f.relative_to(REPO_ROOT)
        if not fm:
            fails.append(f"DOC-11: {rel} has a --- block that isn't valid YAML (or isn't a mapping)")
            continue
        missing = [field for field in _KB_REQUIRED_FIELDS if field not in fm or fm[field] in (None, "")]
        if missing:
            fails.append(f"DOC-11: {rel} frontmatter missing/empty required field(s): {missing}")
        if "type" in fm and fm["type"] not in _KB_TYPE_ENUM:
            fails.append(f"DOC-11: {rel} frontmatter 'type: {fm['type']}' not in {sorted(_KB_TYPE_ENUM)}")
        if "status" in fm and fm["status"] not in _KB_STATUS_ENUM:
            fails.append(f"DOC-11: {rel} frontmatter 'status: {fm['status']}' not in {sorted(_KB_STATUS_ENUM)}")
    return fails


# ── DOC-12: link integrity -- related/superseded_by targets exist on disk ───
def check_doc12_link_integrity(kb_files):
    fails = []
    for f in kb_files:
        text = _read(f)
        if not text.startswith("---\n"):
            continue
        fm = _parse_frontmatter(text)
        if not fm:
            continue
        rel = f.relative_to(REPO_ROOT)
        for field in ("related", "superseded_by"):
            val = fm.get(field)
            if not val:
                continue
            targets = val if isinstance(val, list) else [val]
            for t in targets:
                if not isinstance(t, str) or "://" in t:
                    continue  # not a string, or a URL not a repo path
                if not (REPO_ROOT / t).exists():
                    fails.append(f"DOC-12: {rel} frontmatter '{field}' references `{t}` which does not exist")
    return fails


# ── DOC-13: undetected-duplicate heuristic -- the actual gap this whole
# schema exists to close. DOC-01..10 only ever check doc-vs-code drift;
# nothing previously checked doc-vs-doc overlap (e.g. re/re_docs/AI/*.md
# silently duplicating re/re_docs/kenshi/*.md under identical filenames with
# no deprecation marker, found by hand this session -- this check is meant
# to catch that class mechanically next time). Deliberately narrow and
# heuristic (expected false positives -- see .claude/agents/doc-doctor.md's
# own triage bucket for this check, same treatment as DOC-04/DOC-07):
# among files WITH frontmatter and status=active, flag any pair sharing
# >=2 tags with no related-link between them in EITHER direction. Umbrella/
# methodology tags (broad enough that sharing them alone is not a duplication
# signal -- confirmed noisy at 120-file corpus scale, doc-doctor triage
# 2026-08-25: 467/493 findings were pairs sharing ONLY these) are excluded
# from the threshold count; a pair must still share >=2 tags outside this
# stoplist to fire. ───────────────────────────────────────────────────────
_DOC13_STOPLIST_TAGS = {
    "kenshi", "re", "reverse-engineering", "alien-isolation", "reference",
    "impl-status", "research", "sweep-findings", "stale-warning", "deepseek",
}


def check_doc13_undetected_duplicates(kb_files):
    entries = []
    for f in kb_files:
        text = _read(f)
        if not text.startswith("---\n"):
            continue
        fm = _parse_frontmatter(text)
        if not fm or fm.get("status") != "active":
            continue
        tags = fm.get("tags") or []
        if not isinstance(tags, list) or len(tags) < 2:
            continue
        related = fm.get("related") or []
        related = related if isinstance(related, list) else [related]
        entries.append((str(f.relative_to(REPO_ROOT)), set(tags), set(related)))

    fails = []
    for i, (path_a, tags_a, related_a) in enumerate(entries):
        for path_b, tags_b, related_b in entries[i + 1:]:
            shared = (tags_a & tags_b) - _DOC13_STOPLIST_TAGS
            if len(shared) < 2:
                continue
            if path_b in related_a or path_a in related_b:
                continue  # already cross-referenced -- not undetected
            fails.append(f"DOC-13: {path_a} and {path_b} share tags {sorted(shared)}, both status=active, no related-link between them -- possible undetected duplicate/overlap")
    return fails


CHECKS = [
    ("DOC-01", lambda md, bd, lim: check_doc01_paths_exist(md)),
    ("DOC-02", lambda md, bd, lim: check_doc02_no_external_refs(md)),
    ("DOC-03", lambda md, bd, lim: check_doc03_test_count(md, bd)),
    ("DOC-04", lambda md, bd, lim: check_doc04_stack_names(md)),
    ("DOC-05", lambda md, bd, lim: check_doc05_file_registry(md)),
    ("DOC-06", lambda md, bd, lim: check_doc06_planned_not_built(md)),
    ("DOC-07", lambda md, bd, lim: check_doc07_adr_pairs()),
    ("DOC-08", lambda md, bd, lim: check_doc08_header_date(md)),
    ("DOC-09", lambda md, bd, lim: check_doc09_binary_formats(md)),
    ("DOC-10", lambda md, bd, lim: check_doc10_root_line_limit(lim)),
    ("DOC-11", lambda md, bd, lim: check_doc11_frontmatter_complete(_kb_files())),
    ("DOC-12", lambda md, bd, lim: check_doc12_link_integrity(_kb_files())),
    ("DOC-13", lambda md, bd, lim: check_doc13_undetected_duplicates(_kb_files())),
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--root-line-limit", type=int, default=80)
    args = ap.parse_args()

    md_files = _md_files()
    all_fails = []
    per_check = {}
    for check_id, fn in CHECKS:
        fails = fn(md_files, args.build_dir, args.root_line_limit)
        real_fails = [f for f in fails if "SKIP" not in f]
        skips = [f for f in fails if "SKIP" in f]
        per_check[check_id] = {"failed": len(real_fails), "skipped": len(skips)}
        all_fails.extend(fails)

    total_failed = sum(1 for f in all_fails if "SKIP" not in f)
    total_skipped = sum(1 for f in all_fails if "SKIP" in f)
    total_checks_run = len(CHECKS) * 1  # each DOC-XX is one "check" regardless of finding count

    if args.json:
        print(json.dumps({
            "checks": total_checks_run,
            "failed_findings": total_failed,
            "skipped": total_skipped,
            "per_check": per_check,
            "findings": all_fails,
        }, indent=2))
    else:
        for line in all_fails:
            print(line)
        print(f"\n{total_checks_run - total_skipped} checks ran, {total_failed} findings, {total_skipped} skipped")

    return 1 if total_failed > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
