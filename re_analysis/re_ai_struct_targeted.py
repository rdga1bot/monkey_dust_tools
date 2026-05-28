#!/usr/bin/env python3
"""
re_ai_struct_targeted.py — Targeted struct analysis for AI.exe.c / kenshi AI.

Finds struct offset patterns in specific named-string-bearing functions.
For each function that contains a given "anchor string" (e.g. "BEHAVIOR_MANAGER"),
extracts all param_1/this/lVar offset accesses and nearby context.

Usage:
  python3 tools/re_analysis/re_ai_struct_targeted.py \
    tmp_md/AI/AI.exe.c \
    --output tmp_md/AI/re_ai_structs.md
"""

import re, sys, argparse
from collections import defaultdict
from pathlib import Path

# Anchor strings → subsystem name
ANCHORS = {
    "BEHAVIOR_MANAGER":    "BehaviorManager",
    "squad_id":            "NpcSquad",
    "num_used_squads":     "NpcSquadManager",
    "AI_MEM_A_CHARACTER":  "NpcMemory",
    "AI_MEM_A_ENTITIES":   "NpcMemory",
    "AI_MEM_A_JOBS":       "NpcMemory",
    "NpcSquad":            "NpcSquad",
    "proxy_disable":       "NpcProxy",
    "proxy_enable":        "NpcProxy",
    "m_coordinator_entities": "SquadCoordinator",
    "m_squad_notify_latest":  "SquadCoordinator",
    "FreeCamera":          "AIDebug",
    "memories":            "NpcMemory",
}

FUNC_RE   = re.compile(r'^(?:void|int|float|bool|longlong|undefined\d*|[a-zA-Z_*\s]+)\s+(FUN_[0-9a-f]+)\s*\(')
OFFSET_RE = re.compile(r'(param_\d+|this|lVar\d+|puVar\d+|piVar\d+)\s*\+\s*(0x[0-9a-fA-F]+|\d+)')
STRING_RE = re.compile(r'"([^"]{3,50})"')
FLOAT_RE  = re.compile(r'\(float\s*\*\)')
BOOL_RE   = re.compile(r"!=\s*'\\0'|==\s*'\\0'")
PTR_RE    = re.compile(r'\(longlong\s*\*\)|\(undefined8\s*\*\)')

def find_fun_boundaries_near(lines, target_set, window=20000):
    """For each anchor string, find the containing FUN_ and its line range."""
    results = {}  # anchor → (fun_name, start, end, first_hit_line)

    def find_enclosing_fun(idx):
        for i in range(idx, max(0, idx - 5000), -1):
            m = FUNC_RE.match(lines[i])
            if m:
                return m.group(1), i
        return None, -1

    def find_fun_end(fun_start):
        depth = 0
        found = False
        for i in range(fun_start, min(fun_start + window, len(lines))):
            depth += lines[i].count('{') - lines[i].count('}')
            if lines[i].count('{') > 0:
                found = True
            if found and depth <= 0:
                return i
        return min(fun_start + window, len(lines) - 1)

    found_funs = {}  # fun_name → (start, end)

    for i, line in enumerate(lines):
        for anchor in target_set:
            if f'"{anchor}"' in line:
                fun_name, fun_start = find_enclosing_fun(i)
                if fun_name is None:
                    continue
                if fun_name not in found_funs:
                    fun_end = find_fun_end(fun_start)
                    found_funs[fun_name] = (fun_start, fun_end)
                if anchor not in results:
                    results[anchor] = (fun_name, found_funs[fun_name][0],
                                       found_funs[fun_name][1], i)
    return results, found_funs

def analyse_function_offsets(lines, start, end, base_vars=None):
    """Extract offset access patterns from a function."""
    if base_vars is None:
        base_vars = {'param_1', 'param_2', 'this'}

    offsets = defaultdict(lambda: {
        'count': 0, 'reads': 0, 'writes': 0,
        'float': 0, 'bool': 0, 'ptr': 0,
        'context': [],
    })

    for i in range(start, end + 1):
        line = lines[i]
        for m in OFFSET_RE.finditer(line):
            base, off_str = m.group(1), m.group(2)
            if base not in base_vars:
                continue
            try:
                off = int(off_str, 16) if off_str.startswith('0x') else int(off_str)
            except ValueError:
                continue
            if off > 0x2000:
                continue

            d = offsets[off]
            d['count'] += 1
            if FLOAT_RE.search(line):
                d['float'] += 1
            if BOOL_RE.search(line):
                d['bool'] += 1
            if PTR_RE.search(line):
                d['ptr'] += 1

            if len(d['context']) < 2:
                strs = STRING_RE.findall(line)
                d['context'].append((i + 1, line.rstrip(), strs))

    return offsets

def format_type(d):
    if d['float'] > 0:
        return 'float'
    if d['bool'] > 0:
        return 'bool'
    if d['ptr'] > 0:
        return 'ptr'
    return 'uint32'

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input')
    ap.add_argument('--output', default='re_ai_structs.md')
    args = ap.parse_args()

    print(f'Loading {args.input} ...', flush=True)
    with open(args.input, errors='replace') as f:
        lines = f.readlines()
    print(f'  {len(lines):,} lines.', flush=True)

    print('Finding anchor-bearing functions ...', flush=True)
    anchor_hits, found_funs = find_fun_boundaries_near(lines, set(ANCHORS.keys()))
    print(f'  {len(found_funs)} unique functions found.', flush=True)

    out = []
    w = out.append
    w('# AI.exe.c — Targeted Struct Analysis (Level 3)')
    w(f'> Source: `{args.input}` ({len(lines):,} lines)')
    w('')

    # Group functions by subsystem
    subsystem_funs = defaultdict(list)
    for anchor, (fun_name, start, end, hit_line) in anchor_hits.items():
        subsys = ANCHORS[anchor]
        subsystem_funs[subsys].append((fun_name, start, end, anchor, hit_line))

    for subsys in sorted(subsystem_funs.keys()):
        w(f'## {subsys}')
        w('')
        seen_funs = set()
        for fun_name, start, end, anchor, hit_line in subsystem_funs[subsys]:
            if fun_name in seen_funs:
                continue
            seen_funs.add(fun_name)

            w(f'### `{fun_name}` (lines {start+1}–{end+1}, anchor: `"{anchor}"`)')
            w('')

            # Context around anchor string
            lo = max(0, hit_line - 5)
            hi = min(len(lines), hit_line + 15)
            w('**Anchor context:**')
            w('```c')
            for j in range(lo, hi):
                w(f'{j+1:6d}  {lines[j].rstrip()}')
            w('```')
            w('')

            # Struct offset analysis
            offsets = analyse_function_offsets(lines, start, end)
            if offsets:
                w('**Struct offsets accessed:**')
                w('')
                w('| Offset | Count | Type | Sample line |')
                w('|--------|-------|------|-------------|')
                for off in sorted(offsets.keys()):
                    d = offsets[off]
                    if d['count'] < 2:
                        continue
                    sample = d['context'][0][1].strip()[:80] if d['context'] else ''
                    w(f'| `0x{off:x}` | {d["count"]} | {format_type(d)} | `{sample}` |')
            w('')
        w('---')
        w('')

    Path(args.output).write_text('\n'.join(out))
    print(f'Written: {args.output}', flush=True)

if __name__ == '__main__':
    main()
