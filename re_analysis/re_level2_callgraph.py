#!/usr/bin/env python3
"""
re_level2_callgraph.py — Level 2 RE analysis: call graph + function tracing.

Builds a reverse call graph from the Ghidra decompile:
  - For each named function (OGRE::, etc.), find which FUN_ functions call it
  - For any target string or function, trace up N levels to find semantic context
  - Reconstruct subsystem clusters

Usage:
  python3 tools/re_analysis/re_level2_callgraph.py <exe_c> --trace "setBoneSize" --depth 3
  python3 tools/re_analysis/re_level2_callgraph.py <exe_c> --subsystem bones
  python3 tools/re_analysis/re_level2_callgraph.py <exe_c> --all --output callgraph.md
"""

import re, sys, argparse
from collections import defaultdict, deque

FUNC_START_RE = re.compile(r'^(?:void|int|float|bool|undefined\d*|longlong|char|[a-zA-Z_*]+\s+\*?)\s+(FUN_[0-9a-f]+)\s*\(')
CALL_RE = re.compile(r'\b(FUN_[0-9a-f]+|thunk_FUN_[0-9a-f]+)\s*\(')
STRING_ASSIGN_RE = re.compile(r'"([^"]{3,80})"')
NAMED_CALL_RE = re.compile(r'\b([A-Z][a-zA-Z]+::[A-Za-z_][A-Za-z0-9_<>*&]*)\s*\(')

SUBSYSTEMS = {
    'bones':     [r'setBoneSize', r'setBonePositionalSize', r'getBone', r'Bone\b'],
    'morph':     [r'getPoseList', r'getPoseIterator', r'setPoseWeight', r'ManualPoseControl'],
    'render':    [r'Ogre::Root', r'RenderSystem', r'SceneManager', r'Viewport', r'RenderTarget'],
    'physics':   [r'btRigidBody', r'Jolt', r'PhysicsSystem', r'CharacterVirtual'],
    'ai':        [r'BehaviorTree', r'Squad', r'Patrol', r'AiGoal', r'NpcGoal'],
    'character': [r'setBoneSize', r'male_editor', r'female_editor', r'CharacterCreat'],
    'combat':    [r'damage', r'attack', r'hit', r'bleed', r'wound', r'severed'],
    'ui':        [r'ImGui', r'Slider', r'Button', r'Window', r'Panel'],
    'audio':     [r'Sound', r'Audio', r'miniaudio', r'FMOD'],
}

def build_graph(path):
    """Build: callee→callers and caller→callees mapping, plus function→strings."""
    print(f'Building call graph from {path}...', file=sys.stderr)

    callee_to_callers = defaultdict(set)   # FUN_X → set of FUN_Y that call it
    caller_to_callees = defaultdict(set)   # FUN_X → set of FUN_Y it calls
    func_strings = defaultdict(list)       # FUN_X → strings found inside it
    func_named_calls = defaultdict(set)    # FUN_X → named APIs called

    current_func = None
    with open(path, 'r', errors='ignore') as f:
        for i, line in enumerate(f):
            if i % 200000 == 0:
                print(f'  {i:,} lines...', file=sys.stderr, flush=True)

            # Detect function start
            m = FUNC_START_RE.match(line)
            if m:
                current_func = m.group(1)
                continue

            if current_func is None:
                continue

            # Collect calls
            for m in CALL_RE.finditer(line):
                callee = m.group(1)
                if callee != current_func:
                    callee_to_callers[callee].add(current_func)
                    caller_to_callees[current_func].add(callee)

            # Collect strings
            for m in STRING_ASSIGN_RE.finditer(line):
                s = m.group(1)
                if len(s) >= 3:
                    func_strings[current_func].append(s)

            # Named API calls
            for m in NAMED_CALL_RE.finditer(line):
                func_named_calls[current_func].add(m.group(1))

    print(f'  Graph built: {len(callee_to_callers)} callee nodes', file=sys.stderr)
    return callee_to_callers, caller_to_callees, func_strings, func_named_calls

def trace_from_seed(seed_pattern, callee_to_callers, func_strings, func_named_calls,
                    depth=3):
    """BFS upward from any FUN_ or thunk_ matching seed_pattern."""
    # Find seed nodes
    seeds = set()
    for node in callee_to_callers.keys():
        if re.search(seed_pattern, node, re.I):
            seeds.add(node)
    # Also check func_strings
    for node, strings in func_strings.items():
        for s in strings:
            if re.search(seed_pattern, s, re.I):
                seeds.add(node)

    if not seeds:
        return {}

    visited = {}  # node → depth
    queue = deque((s, 0) for s in seeds)
    while queue:
        node, d = queue.popleft()
        if node in visited:
            continue
        visited[node] = d
        if d < depth:
            for caller in callee_to_callers.get(node, set()):
                if caller not in visited:
                    queue.append((caller, d+1))
    return visited

def find_subsystem_funcs(subsystem, callee_to_callers, func_strings, func_named_calls):
    """Find all FUN_ functions associated with a subsystem."""
    patterns = SUBSYSTEMS.get(subsystem, [])
    found = set()
    for pattern in patterns:
        for node, strings in func_strings.items():
            for s in strings:
                if re.search(pattern, s, re.I):
                    found.add(node)
        for node, calls in func_named_calls.items():
            for c in calls:
                if re.search(pattern, c, re.I):
                    found.add(node)
        # Also search callee names
        for callee in callee_to_callers.keys():
            if re.search(pattern, callee, re.I):
                found.add(callee)
    return found

def render_trace(seed, visited, func_strings, func_named_calls):
    lines = [f'### Trace from: `{seed}`', '']
    by_depth = defaultdict(list)
    for node, d in visited.items():
        by_depth[d].append(node)
    for d in sorted(by_depth.keys()):
        lines.append(f'**Depth {d}:**')
        for node in sorted(by_depth[d]):
            strings = func_strings.get(node, [])[:5]
            named = sorted(func_named_calls.get(node, set()))[:5]
            detail = ''
            if strings:
                detail += ' strings=[' + ', '.join(f'"{s[:30]}"' for s in strings) + ']'
            if named:
                detail += ' calls=[' + ', '.join(named[:3]) + ']'
            lines.append(f'- `{node}`{detail}')
        lines.append('')
    return '\n'.join(lines)

def main():
    p = argparse.ArgumentParser()
    p.add_argument('input')
    p.add_argument('--trace', '-t', default=None, help='Trace from string/function pattern')
    p.add_argument('--depth', '-d', type=int, default=3)
    p.add_argument('--subsystem', '-s', default=None, choices=list(SUBSYSTEMS.keys()))
    p.add_argument('--output', '-o', default=None)
    p.add_argument('--all', action='store_true', help='Analyse all subsystems')
    args = p.parse_args()

    callee_to_callers, caller_to_callees, func_strings, func_named_calls = build_graph(args.input)

    out_lines = [f'# RE Level 2 Call Graph — {args.input}', '']

    if args.trace:
        visited = trace_from_seed(args.trace, callee_to_callers, func_strings, func_named_calls, args.depth)
        out_lines.append(render_trace(args.trace, visited, func_strings, func_named_calls))

    if args.subsystem or args.all:
        subs = list(SUBSYSTEMS.keys()) if args.all else [args.subsystem]
        for sub in subs:
            funcs = find_subsystem_funcs(sub, callee_to_callers, func_strings, func_named_calls)
            out_lines.append(f'## Subsystem: {sub} ({len(funcs)} functions)')
            out_lines.append('')
            for fn in sorted(funcs)[:50]:
                strings = func_strings.get(fn, [])[:3]
                named = sorted(func_named_calls.get(fn, set()))[:3]
                detail = ''
                if strings: detail += ' | ' + '; '.join(f'"{s[:25]}"' for s in strings)
                if named:   detail += ' | ' + '; '.join(named[:2])
                out_lines.append(f'- `{fn}`{detail}')
            out_lines.append('')

    result = '\n'.join(out_lines)
    if args.output:
        with open(args.output, 'w') as f:
            f.write(result)
        print(f'Written to {args.output}', file=sys.stderr)
    else:
        print(result)

if __name__ == '__main__':
    main()
