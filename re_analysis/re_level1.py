#!/usr/bin/env python3
"""
re_level1.py — Level 1 RE analysis: systematic string extraction + named API calls.

Usage:
  python3 tools/re_analysis/re_level1.py <path_to_exe_c> [--output <out.md>]

Produces a Markdown report with:
  1. All string constants (grouped by category)
  2. All named API/library calls (non-FUN_ function names)
  3. Key offset patterns (struct field access)
  4. Function count statistics
"""

import re, sys, os, argparse
from collections import defaultdict, Counter

def parse_args():
    p = argparse.ArgumentParser(description='RE Level 1 analysis')
    p.add_argument('input', help='Path to Ghidra decompiled .c file')
    p.add_argument('--output', '-o', default=None, help='Output Markdown file')
    p.add_argument('--context', '-c', type=int, default=3, help='Context lines around strings (default 3)')
    p.add_argument('--min-str-len', type=int, default=4, help='Minimum string length (default 4)')
    p.add_argument('--max-str-len', type=int, default=120, help='Maximum string length (default 120)')
    return p.parse_args()

# ── String categorisation patterns ────────────────────────────────────────────
CATEGORIES = [
    ('UI/Slider',       r'(height|frame|posture|shoulder|neck|leg |chest|stomach|waist|hips|arm bulk|head size|eye |nose |brow|jaw|mouth|chin|cheek|skin|hair|feet|hands)', re.I),
    ('Bone/Skeleton',   r'(Bip01|Spine|Thigh|Calf|Foot|Toe|UpperArm|Forearm|Clavicle|Pelvis|Head|Jaw|Neck|bone|skeleton)', re.I),
    ('OGRE/Rendering',  r'(Ogre::|ogre|\.mesh|\.skeleton|\.material|\.dds|\.png|shader|render|scene|entity|camera|viewport|light)', re.I),
    ('AI/Behavior',     r'(BehaviorTree|behavior|patrol|wander|attack|flee|idle|combat|squad|faction|NPC|goal|task|BT)', re.I),
    ('Physics',         r'(physics|collision|rigid|body|shape|jolt|bullet|PhysX|velocity|force|gravity|ragdoll)', re.I),
    ('Save/World',      r'(save|load|world|zone|chunk|player|inventory|item|weapon|armor|faction|quest)', re.I),
    ('Debug/Log',       r'(printf|fprintf|debug|error|warning|assert|log|trace|%.)', re.I),
    ('Math/Transform',  r'(matrix|vector|quaternion|transform|scale|rotate|translate|position|bounds|bbox)', re.I),
    ('Audio',           r'(sound|audio|music|volume|pitch|frequency|miniaudio|fmod|wav|ogg)', re.I),
    ('Input/UI',        r'(button|slider|click|mouse|keyboard|window|ImGui|imgui|widget|panel)', re.I),
]

def categorise(s):
    for name, pattern, flags in CATEGORIES:
        if re.search(pattern, s, flags):
            return name
    return 'Other'

# ── Named function call detection ─────────────────────────────────────────────
NAMED_FUNC_RE = re.compile(
    r'\b([A-Z][a-zA-Z0-9_:]+::[a-zA-Z_][a-zA-Z0-9_]*)\s*\('   # Class::Method(
    r'|'
    r'\b(thunk_[A-Za-z_][A-Za-z0-9_]*)\s*\('                   # thunk_FUN_xxx(
    r'|'
    r'\b(FUN_[0-9a-f]+)\s*\(',                                  # FUN_xxx(
)
API_NAMED_RE = re.compile(
    r'\b([A-Z][a-zA-Z]+::[A-Za-z_][A-Za-z0-9_<>]*)\s*\('
)

# ── Struct offset pattern ─────────────────────────────────────────────────────
OFFSET_RE = re.compile(r'\+\s*(0x[0-9a-fA-F]+)\b')

def analyse(path, context_lines=3, min_len=4, max_len=120):
    print(f'Reading {path} ...', file=sys.stderr, flush=True)
    with open(path, 'r', errors='ignore') as f:
        lines = f.readlines()

    total_lines = len(lines)
    print(f'  {total_lines:,} lines loaded', file=sys.stderr, flush=True)

    # ── Extract strings ───────────────────────────────────────────────────────
    strings = []  # (line_no, string, category, context_before, context_after)
    STRING_RE = re.compile(r'"([^"\\]{' + str(min_len) + r',})"')

    print('  Extracting strings...', file=sys.stderr, flush=True)
    seen_strings = set()
    for i, line in enumerate(lines):
        for m in STRING_RE.finditer(line):
            s = m.group(1)
            if len(s) > max_len: continue
            if s in seen_strings: continue
            seen_strings.add(s)
            cat = categorise(s)
            ctx_before = [l.rstrip() for l in lines[max(0,i-context_lines):i]]
            ctx_after  = [l.rstrip() for l in lines[i+1:min(total_lines,i+1+context_lines)]]
            strings.append((i+1, s, cat, ctx_before, ctx_after))

    # ── Count named API calls ─────────────────────────────────────────────────
    print('  Counting named API calls...', file=sys.stderr, flush=True)
    api_counter = Counter()
    thunk_counter = Counter()
    fun_counter = Counter()
    for line in lines:
        for m in API_NAMED_RE.finditer(line):
            api_counter[m.group(1)] += 1
        if 'thunk_' in line:
            for m in re.finditer(r'thunk_FUN_[0-9a-f]+', line):
                thunk_counter[m.group()] += 1

    # Count FUN_ occurrences (as callers/callees)
    for line in lines:
        for m in re.finditer(r'FUN_[0-9a-f]+', line):
            fun_counter[m.group()] += 1

    # ── Offset frequency analysis ─────────────────────────────────────────────
    print('  Analysing struct offsets...', file=sys.stderr, flush=True)
    offset_counter = Counter()
    for line in lines:
        for m in OFFSET_RE.finditer(line):
            val = int(m.group(1), 16)
            if val < 0x1000:  # small offsets = struct fields
                offset_counter[m.group(1)] += 1

    # ── Statistics ────────────────────────────────────────────────────────────
    func_count = sum(1 for l in lines if l.strip().startswith('void FUN_') or
                                          re.match(r'^[a-z].*FUN_[0-9a-f]+\(', l.strip()))
    named_func_count = sum(1 for l in lines if re.search(r'^[a-zA-Z_*].*::', l))

    return {
        'path': path,
        'total_lines': total_lines,
        'func_count': func_count,
        'named_func_count': named_func_count,
        'strings': strings,
        'api_counter': api_counter,
        'thunk_counter': thunk_counter,
        'fun_counter': fun_counter,
        'offset_counter': offset_counter,
    }

def render_markdown(data):
    path = data['path']
    fname = os.path.basename(path)
    lines_out = []
    w = lines_out.append

    w(f'# RE Level 1 Analysis — {fname}')
    w(f'> Source: `{path}`')
    w(f'> Lines: {data["total_lines"]:,} | Unique strings: {len(data["strings"])}')
    w(f'> Estimated FUN_ functions: {data["func_count"]:,}')
    w('')
    w('---')
    w('')

    # 1. String table by category
    by_cat = defaultdict(list)
    for item in data['strings']:
        by_cat[item[2]].append(item)

    w('## 1. String Constants by Category')
    w('')
    for cat, _pat, _flags in CATEGORIES:
        items = by_cat.get(cat, [])
        if not items: continue
        w(f'### {cat} ({len(items)} strings)')
        w('')
        w('| Line | String |')
        w('|------|--------|')
        for (lineno, s, c, ctx_b, ctx_a) in sorted(items, key=lambda x: x[0])[:100]:
            s_esc = s.replace('|', '\\|')
            w(f'| {lineno} | `{s_esc}` |')
        if len(items) > 100:
            w(f'| ... | *+{len(items)-100} more* |')
        w('')

    other = by_cat.get('Other', [])
    if other:
        w(f'### Other ({len(other)} strings)')
        w('')
        w('| Line | String |')
        w('|------|--------|')
        for (lineno, s, c, ctx_b, ctx_a) in sorted(other, key=lambda x: x[0])[:50]:
            s_esc = s.replace('|', '\\|')
            w(f'| {lineno} | `{s_esc}` |')
        w('')

    # 2. Named API calls
    w('## 2. Named API/Library Calls (Top 100)')
    w('')
    w('| Count | Function |')
    w('|-------|----------|')
    for func, cnt in data['api_counter'].most_common(100):
        w(f'| {cnt} | `{func}` |')
    w('')

    # 3. Most-called thunk functions
    w('## 3. Most-Called Thunk Functions (Top 50)')
    w('')
    w('| Count | Thunk |')
    w('|-------|-------|')
    for thunk, cnt in data['thunk_counter'].most_common(50):
        w(f'| {cnt} | `{thunk}` |')
    w('')

    # 4. Most-referenced FUN_ addresses
    w('## 4. Most-Referenced FUN_ Addresses (Top 50)')
    w('')
    w('| Count | FUN_ address |')
    w('|-------|--------------|')
    for fun, cnt in data['fun_counter'].most_common(50):
        w(f'| {cnt} | `{fun}` |')
    w('')

    # 5. Common struct offsets
    w('## 5. Most Common Struct Offsets (field access patterns)')
    w('')
    w('| Count | Offset |')
    w('|-------|--------|')
    for off, cnt in data['offset_counter'].most_common(60):
        w(f'| {cnt} | `{off}` |')
    w('')

    return '\n'.join(lines_out)

def main():
    args = parse_args()
    data = analyse(args.input, args.context, args.min_str_len, args.max_str_len)
    md = render_markdown(data)

    if args.output:
        with open(args.output, 'w') as f:
            f.write(md)
        print(f'Written to {args.output}', file=sys.stderr)
    else:
        print(md)

if __name__ == '__main__':
    main()
