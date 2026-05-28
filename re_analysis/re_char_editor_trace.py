#!/usr/bin/env python3
"""
re_char_editor_trace.py — Kenshi char editor reverse engineering.

For each known slider string (Chest, Waist, Stomach, etc.), extracts:
  1. The containing FUN_ function
  2. All SetBone*/OldBone/OGRE::Skeleton calls within that function
  3. The axis values passed (vec3 or float)

Usage:
  python3 tools/re_analysis/re_char_editor_trace.py \
    tmp_md/kenshi/kenshi_x64.exe.c \
    --output tmp_md/kenshi/re_char_editor.md
"""

import re
import sys
from pathlib import Path

SLIDER_STRINGS = [
    "Height", "Frame", "Leg length", "Chest", "Waist", "Stomach",
    "Arm bulk", "Shoulders", "Hands", "Head size", "Neck width",
    "Neck length", "Bip01 Jaw", "Hips", "Breast height",
    "hair style", "hairMap", "hairMult", "hairAlpha", "hairColor",
    "WhiteSkin", "Posture", "shoulder set", "shoulder lift",
]

BONE_PATTERNS = [
    r'setBoneSize',   r'setBone',   r'OldBone',
    r'setScale',      r'setPosition', r'getScale',
    r'Bip01',         r'createBone',  r'getBone',
    r'Skeleton',      r'Bone\b',
]
BONE_RE = re.compile('|'.join(BONE_PATTERNS), re.IGNORECASE)

FUN_RE   = re.compile(r'^[A-Za-z_][A-Za-z0-9_:*~<> ]*\s+\*?\*?\s*(FUN_[0-9a-f]+)\s*\(', re.MULTILINE)
VEC3_RE  = re.compile(r'vec3\s*\(([\s\S]*?)\)', re.DOTALL)
FLOAT_RE = re.compile(r'[-+]?[0-9]*\.?[0-9]+[eEfF]?')


def load_lines(path: str):
    print(f"Loading {path} ...", flush=True)
    with open(path, 'r', errors='replace') as f:
        return f.readlines()


def find_fun_boundaries(lines):
    """Return list of (fun_name, start_line, end_line) using brace counting."""
    boundaries = []
    i = 0
    n = len(lines)
    while i < n:
        m = re.match(r'^[A-Za-z_][^\n]*\s+(FUN_[0-9a-f]+)\s*\(', lines[i])
        if m:
            fun = m.group(1)
            # Find opening brace
            j = i
            while j < min(i + 10, n) and '{' not in lines[j]:
                j += 1
            if j >= min(i + 10, n):
                i += 1
                continue
            depth = 0
            start = j
            end = j
            for k in range(j, min(j + 50000, n)):
                depth += lines[k].count('{') - lines[k].count('}')
                if depth <= 0:
                    end = k
                    break
            boundaries.append((fun, i, end))
            i = end + 1
        else:
            i += 1
    return boundaries


def extract_context(lines, line_idx, window=80):
    """Return lines[line_idx-window:line_idx+window]"""
    lo = max(0, line_idx - window)
    hi = min(len(lines), line_idx + window)
    return lines[lo:hi], lo


def find_function_for_line(boundaries, line_idx):
    for fun, start, end in boundaries:
        if start <= line_idx <= end:
            return fun, start, end
    return None, -1, -1


def extract_bone_ops(lines, start, end):
    """Extract all bone-related lines from a function range."""
    ops = []
    for i in range(start, min(end + 1, len(lines))):
        if BONE_RE.search(lines[i]):
            ops.append((i + 1, lines[i].rstrip()))
    return ops


def extract_vec3_values(line_str):
    """Try to pull float values from line context."""
    vecs = VEC3_RE.findall(line_str)
    result = []
    for v in vecs:
        floats = FLOAT_RE.findall(v)
        result.append(floats)
    return result


def analyze_kenshi_char_editor(src_path, out_path):
    lines = load_lines(src_path)
    total = len(lines)
    print(f"  {total:,} lines loaded.", flush=True)

    # Step 1: find all slider string line numbers
    print("  Finding slider strings...", flush=True)
    slider_hits = {}  # slider_str -> [line_idx]
    for i, line in enumerate(lines):
        for sl in SLIDER_STRINGS:
            if f'"{sl}"' in line or f'"{sl} "' in line:
                slider_hits.setdefault(sl, []).append(i)

    print(f"  Found hits for {len(slider_hits)} sliders.", flush=True)

    # Step 2: build function boundaries (expensive — sample around hits only)
    # Instead of full boundary scan, use context window approach:
    # Find enclosing FUN_ by scanning backwards from each hit
    def find_enclosing_fun(line_idx):
        # Scan backward for function signature
        for i in range(line_idx, max(0, line_idx - 5000), -1):
            m = re.match(r'^[A-Za-z_][^\n]*\s+(FUN_[0-9a-f]+)\s*\(', lines[i])
            if m:
                return m.group(1), i
        return None, -1

    def find_fun_end(fun_start):
        depth = 0
        found_open = False
        for i in range(fun_start, min(fun_start + 30000, total)):
            for ch in lines[i]:
                if ch == '{':
                    depth += 1
                    found_open = True
                elif ch == '}':
                    depth -= 1
            if found_open and depth <= 0:
                return i
        return min(fun_start + 30000, total - 1)

    # Step 3: for each slider, get context + bone ops
    print("  Tracing bone operations per slider...", flush=True)
    results = {}
    seen_funs = {}

    for sl, hits in sorted(slider_hits.items()):
        fun_data = []
        for hit_line in hits[:3]:  # max 3 hits per slider
            fun_name, fun_start = find_enclosing_fun(hit_line)
            if fun_name is None:
                continue
            if fun_name in seen_funs:
                fun_end, bone_ops = seen_funs[fun_name]
            else:
                fun_end = find_fun_end(fun_start)
                bone_ops = extract_bone_ops(lines, fun_start, fun_end)
                seen_funs[fun_name] = (fun_end, bone_ops)

            # Context around the slider string (±15 lines)
            ctx_lo = max(0, hit_line - 5)
            ctx_hi = min(total, hit_line + 20)
            ctx = [(ctx_lo + j + 1, lines[ctx_lo + j].rstrip())
                   for j in range(ctx_hi - ctx_lo)]

            fun_data.append({
                'fun': fun_name,
                'fun_start': fun_start + 1,
                'hit_line': hit_line + 1,
                'context': ctx,
                'bone_ops': bone_ops[:40],
            })
        if fun_data:
            results[sl] = fun_data

    # Step 4: write markdown report
    print(f"  Writing report to {out_path} ...", flush=True)
    out = []
    w = out.append

    w('# Kenshi Char Editor — Slider → Bone Mapping RE')
    w(f'> Source: `{src_path}`')
    w(f'> Lines: {total:,} | Sliders analyzed: {len(results)}')
    w('')
    w('---')
    w('')
    w('## Key Findings')
    w('')
    w('> This document maps each character editor slider to its OGRE bone operations.')
    w('> Format: slider string → FUN_ → bone calls with axis/value context.')
    w('')

    for sl in SLIDER_STRINGS:
        if sl not in results:
            w(f'## `{sl}` — NOT FOUND')
            w('')
            continue
        w(f'## `{sl}`')
        for entry in results[sl]:
            w(f'')
            w(f'**Function:** `{entry["fun"]}` (line {entry["fun_start"]})')
            w(f'**Slider reference at line:** {entry["hit_line"]}')
            w('')
            w('### Context (±15 lines around slider)')
            w('```c')
            for ln, text in entry['context']:
                w(f'{ln:6d}  {text}')
            w('```')
            w('')
            if entry['bone_ops']:
                w('### Bone/Scale operations in this function')
                w('```c')
                for ln, text in entry['bone_ops'][:30]:
                    w(f'{ln:6d}  {text}')
                w('```')
            else:
                w('> No bone operations found in this function — may be a UI-layer function.')
            w('')
        w('---')
        w('')

    Path(out_path).write_text('\n'.join(out))
    print(f"  Done. Report: {out_path}", flush=True)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    src = sys.argv[1]
    out = sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == '--output' else \
          str(Path(src).parent / 're_char_editor.md')
    analyze_kenshi_char_editor(src, out)
