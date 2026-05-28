#!/usr/bin/env python3
"""
re_level3_struct.py — Level 3 RE analysis: data structure reconstruction.

Reconstructs structs/classes from:
  - Offset access patterns (param_1+0x28 → field at 0x28)
  - String key lookups near offset accesses
  - vtable patterns (vptr at offset 0)
  - Size patterns from malloc/new calls

Usage:
  python3 tools/re_analysis/re_level3_struct.py <exe_c> --focus "char" --output structs.md
  python3 tools/re_analysis/re_level3_struct.py <exe_c> --focus "bone" --min-refs 5
"""

import re, sys, argparse
from collections import defaultdict, Counter

OFFSET_ACCESS_RE = re.compile(
    r'(?:param_\d+|this|lVar\d+|puVar\d+)\s*(?:\[(\d+)\]|\+\s*(0x[0-9a-fA-F]+|\d+))\b'
)
STRING_NEAR_RE   = re.compile(r'"([^"]{3,60})"')
FUNC_START_RE    = re.compile(r'^(?:void|int|float|bool|longlong|[a-zA-Z_*\s]+)\s+(FUN_[0-9a-f]+)\s*\(')
VTABLE_RE        = re.compile(r'\*\(.*?\+\s*0x0\)')
MALLOC_SIZE_RE   = re.compile(r'operator_new\((\d+)\)')

# Type hints from context patterns
TYPE_HINTS = {
    r'0x([0-9a-f]+)\s*\)\s*!=\s*0': 'bool/flag',
    r'=\s*\(float\)\s*\*\(int\s*\*\)': 'float',
    r'=\s*\(int\)\s*\*\(': 'int32',
    r'=\s*\*\(longlong': 'ptr/int64',
    r'=\s*\*\(undefined4': 'uint32',
    r'=\s*\*\(byte': 'byte/bool',
}

def analyse_structs(path, focus_pattern=None, min_refs=3, context_window=5):
    print(f'Analysing structs in {path} ...', file=sys.stderr, flush=True)

    # Per-offset data: {offset: {count, strings_near, type_hints, functions_seen}}
    struct_data = defaultdict(lambda: {
        'count': 0,
        'strings': Counter(),
        'types': Counter(),
        'funcs': set(),
        'params': Counter(),   # which param_ variable it's applied to
    })

    func_structs = defaultdict(set)   # func → set of offsets used
    func_alloc_sizes = {}             # func → struct size from operator_new

    current_func = None
    current_lines = []

    with open(path, 'r', errors='ignore') as f:
        for i, line in enumerate(f):
            if i % 200000 == 0:
                print(f'  {i:,} lines...', file=sys.stderr, flush=True)

            # Detect function start/end
            m = FUNC_START_RE.match(line)
            if m:
                current_func = m.group(1)
                current_lines = []

            if current_func is None:
                continue

            current_lines.append(line)

            # Filter by focus
            if focus_pattern and not re.search(focus_pattern, ''.join(current_lines[-20:]), re.I):
                pass  # still process, but only store if relevant

            # Extract offset accesses
            for m in OFFSET_ACCESS_RE.finditer(line):
                idx = m.group(1)   # array index [N]
                off = m.group(2)   # pointer offset +0xNN

                if idx is not None:
                    offset_key = f'[{idx}]'
                elif off is not None:
                    offset_key = off if off.startswith('0x') else hex(int(off))
                else:
                    continue

                # Extract param name
                param_m = re.match(r'(param_\d+|this)', m.group(0))
                if param_m:
                    struct_data[offset_key]['params'][param_m.group(1)] += 1

                struct_data[offset_key]['count'] += 1
                struct_data[offset_key]['funcs'].add(current_func)
                func_structs[current_func].add(offset_key)

                # Nearby strings (from context)
                ctx_start = max(0, len(current_lines)-context_window)
                ctx = ''.join(current_lines[ctx_start:])
                for sm in STRING_NEAR_RE.finditer(ctx):
                    if focus_pattern and not re.search(focus_pattern, sm.group(1), re.I):
                        continue
                    struct_data[offset_key]['strings'][sm.group(1)] += 1

                # Type hints
                for pattern, type_name in TYPE_HINTS.items():
                    if re.search(pattern.replace('0x([0-9a-f]+)', offset_key), line, re.I):
                        struct_data[offset_key]['types'][type_name] += 1

            # Allocation sizes
            m = MALLOC_SIZE_RE.search(line)
            if m:
                func_alloc_sizes[current_func] = int(m.group(1))

    return struct_data, func_structs, func_alloc_sizes

def render_struct_report(struct_data, func_structs, func_alloc_sizes, focus=None, min_refs=3):
    lines = []
    w = lines.append

    # Filter and sort
    filtered = {k: v for k, v in struct_data.items() if v['count'] >= min_refs}
    if focus:
        focused = {k: v for k, v in filtered.items()
                   if any(re.search(focus, s, re.I) for s in v['strings'])}
        if focused:
            filtered = focused

    sorted_offsets = sorted(filtered.items(), key=lambda x: -x[1]['count'])

    w(f'## Struct Field Analysis (min {min_refs} refs, focus={focus})')
    w('')
    w('| Offset | Count | Likely Type | Nearby Strings | Functions |')
    w('|--------|-------|-------------|----------------|-----------|')

    for off, data in sorted_offsets[:100]:
        count = data['count']
        types = ', '.join(f'{t}({c})' for t, c in data['types'].most_common(2))
        strings = '; '.join(f'"{s[:20]}"' for s, _ in data['strings'].most_common(3))
        funcs = len(data['funcs'])
        w(f'| `{off}` | {count} | {types or "?"} | {strings} | {funcs} funcs |')

    w('')

    # Allocation sizes (struct sizes)
    if func_alloc_sizes:
        w('## Allocation Sizes (struct sizeof candidates)')
        w('')
        size_groups = defaultdict(list)
        for func, size in func_alloc_sizes.items():
            size_groups[size].append(func)
        w('| Size (bytes) | Count | Functions |')
        w('|-------------|-------|-----------|')
        for size in sorted(size_groups.keys()):
            funcs = size_groups[size]
            w(f'| {size} (0x{size:x}) | {len(funcs)} | {", ".join(funcs[:3])} |')
        w('')

    return '\n'.join(lines)

def main():
    p = argparse.ArgumentParser()
    p.add_argument('input')
    p.add_argument('--focus', '-f', default=None, help='Focus on strings matching pattern')
    p.add_argument('--min-refs', '-m', type=int, default=5)
    p.add_argument('--output', '-o', default=None)
    args = p.parse_args()

    struct_data, func_structs, func_alloc_sizes = analyse_structs(
        args.input, args.focus, args.min_refs)

    header = f'# RE Level 3 Struct Analysis — {args.input}\n\n'
    body = render_struct_report(struct_data, func_structs, func_alloc_sizes, args.focus, args.min_refs)
    result = header + body

    if args.output:
        with open(args.output, 'w') as f:
            f.write(result)
        print(f'Written to {args.output}', file=sys.stderr)
    else:
        print(result)

if __name__ == '__main__':
    main()
