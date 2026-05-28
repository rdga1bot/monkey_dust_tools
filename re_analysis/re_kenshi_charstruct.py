#!/usr/bin/env python3
"""
re_kenshi_charstruct.py — Targeted struct reconstruction for Kenshi char editor.

Extracts param_1/param_2 offset accesses from FUN_140015b63 (char editor function)
and nearby string constants to reconstruct the CharacterDef / CharacterEditor struct.

Usage:
  python3 tools/re_analysis/re_kenshi_charstruct.py \
    tmp_md/kenshi/kenshi_x64.exe.c \
    --output tmp_md/kenshi/re_charstruct.md
"""

import re, sys, argparse
from collections import defaultdict, Counter
from pathlib import Path

FUNC_RE  = re.compile(r'^[A-Za-z_][^\n]*(FUN_[0-9a-f]+)\s*\(')
OFFSET_RE = re.compile(
    r'(param_\d+|lVar\d+|puVar\d+|pOVar\d+|this)\s*\+\s*(0x[0-9a-fA-F]+|\d+)'
)
STRING_RE = re.compile(r'"([^"]{2,60})"')
FLOAT_W_RE = re.compile(r'\*\(float\s*\*\)')
BOOL_W_RE  = re.compile(r'!=\s*\\?[\'"]\\?\\0[\'"]|==\s*\\?[\'"]\\?\\1[\'"]')
SETTER_RE  = re.compile(r'setBoneSize|setBonePositionalSize|setPosition|setScale')

# Known slider names (from Level 2.5 analysis)
SLIDER_NAMES = [
    "Height", "Frame", "Leg length", "Chest", "Waist", "Stomach",
    "Arm bulk", "Shoulders", "Hands", "Head size", "Neck width", "Neck length",
    "Bip01 Jaw", "Hips", "Breast size", "Breast height", "Breast spacing",
    "Legs bulk", "Mid-section", "Head shape", "WhiteSkin",
]

def find_function_range(lines, func_name):
    """Find start/end line indices of a named function."""
    for i, line in enumerate(lines):
        if func_name in line and re.match(FUNC_RE, line):
            depth = 0
            found_open = False
            for j in range(i, min(i + 50000, len(lines))):
                depth += lines[j].count('{') - lines[j].count('}')
                if lines[j].count('{') > 0:
                    found_open = True
                if found_open and depth <= 0:
                    return i, j
            return i, min(i + 50000, len(lines) - 1)
    return -1, -1

def analyse_char_editor_struct(lines, start, end):
    """Analyse param_1 offset accesses in char editor function range."""
    offsets = defaultdict(lambda: {
        'count': 0,
        'reads': 0,
        'writes': 0,
        'float': 0,
        'bool': 0,
        'strings_near': Counter(),
        'context_lines': [],
    })

    for i in range(start, end + 1):
        line = lines[i]
        for m in OFFSET_RE.finditer(line):
            base, off_str = m.group(1), m.group(2)
            if base not in ('param_1', 'param_2'):
                continue
            try:
                off = int(off_str, 16) if off_str.startswith('0x') else int(off_str)
            except ValueError:
                continue

            d = offsets[off]
            d['count'] += 1
            try:
                pos = line.index(m.group(0))
                eq_pos = line.index('=', pos)
                if eq_pos > pos:
                    d['reads'] += 1
                else:
                    d['writes'] += 1
            except ValueError:
                d['reads'] += 1
            if FLOAT_W_RE.search(line):
                d['float'] += 1
            if BOOL_W_RE.search(line):
                d['bool'] += 1

            # Collect nearby strings (±3 lines)
            for j in range(max(start, i - 3), min(end + 1, i + 4)):
                for s in STRING_RE.findall(lines[j]):
                    if s in SLIDER_NAMES or any(kw in s for kw in ['Bip01', 'bone', 'Bone']):
                        d['strings_near'][s] += 1

            if len(d['context_lines']) < 3:
                d['context_lines'].append((i + 1, line.rstrip()))

    return offsets

def find_all_setBonePositionalSize(lines, start, end):
    """Find all setBonePositionalSize calls and their Vector3 args."""
    results = []
    for i in range(start, end + 1):
        if 'setBonePositionalSize' in lines[i]:
            ctx = [(i + j + 1, lines[i + j].rstrip()) for j in range(-5, 6)
                   if 0 <= i + j < len(lines)]
            results.append(ctx)
    return results

def find_known_offsets(lines, start, end):
    """Extract offsets that appear near slider name strings."""
    known = {}  # offset → slider_name, context_line
    for i in range(start, end + 1):
        for sn in SLIDER_NAMES:
            if f'"{sn}"' in lines[i]:
                # Check ±10 lines for param_1 writes
                for j in range(max(start, i - 5), min(end + 1, i + 15)):
                    for m in OFFSET_RE.finditer(lines[j]):
                        base, off_str = m.group(1), m.group(2)
                        if base != 'param_1':
                            continue
                        try:
                            off = int(off_str, 16) if off_str.startswith('0x') else int(off_str)
                        except ValueError:
                            continue
                        if '=' in lines[j]:
                            known.setdefault(off, []).append((sn, j + 1, lines[j].rstrip()))
    return known

def write_report(offsets, known_offsets, positional_calls, out_path, func_start, func_end, total_lines):
    out = []
    w = out.append

    w('# Kenshi CharEditor Struct — Level 3 RE')
    w(f'> Function: FUN_140015b63 (lines {func_start}–{func_end})')
    w(f'> Source: kenshi_x64.exe.c ({total_lines:,} lines total)')
    w('')
    w('---')
    w('')
    w('## param_1 Struct Layout (CharacterDef / CharEditorState)')
    w('')
    w('| Offset | R/W | Type | Likely Field | Context |')
    w('|--------|-----|------|-------------|---------|')

    for off in sorted(offsets.keys()):
        d = offsets[off]
        typ = 'float' if d['float'] > d['bool'] else ('bool' if d['bool'] > 0 else 'unknown')
        rw = f"R:{d['reads']}/W:{d['writes']}"
        strings = ', '.join(f'`{s}`' for s in list(d['strings_near'].keys())[:3])
        ctx_line = d['context_lines'][0][1][:80].strip() if d['context_lines'] else ''
        w(f'| `0x{off:x}` | {rw} | {typ} | {strings} | `{ctx_line}` |')

    w('')
    w('---')
    w('')
    w('## Slider → param_1 offset mapping (confirmed writes)')
    w('')
    for off in sorted(known_offsets.keys()):
        entries = known_offsets[off]
        w(f'### `param_1 + 0x{off:x}`')
        for sn, lineno, ctx in entries[:3]:
            w(f'- Slider: **{sn}** (line {lineno})')
            w(f'  ```c')
            w(f'  {ctx.strip()}')
            w(f'  ```')
        w('')

    w('---')
    w('')
    w('## setBonePositionalSize calls (attachment point moves)')
    w('')
    for i, ctx in enumerate(positional_calls[:10]):
        w(f'### Call {i+1}')
        w('```c')
        for lineno, text in ctx:
            w(f'{lineno:6d}  {text}')
        w('```')
        w('')

    Path(out_path).write_text('\n'.join(out))
    print(f'Written: {out_path}', flush=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input')
    ap.add_argument('--output', default='re_charstruct.md')
    ap.add_argument('--func', default='FUN_140015b63')
    args = ap.parse_args()

    print(f'Loading {args.input} ...', flush=True)
    with open(args.input, errors='replace') as f:
        lines = f.readlines()
    print(f'  {len(lines):,} lines.', flush=True)

    print(f'Finding {args.func} ...', flush=True)
    start, end = find_function_range(lines, args.func)
    if start < 0:
        print(f'ERROR: {args.func} not found', file=sys.stderr)
        sys.exit(1)
    print(f'  Lines {start+1}–{end+1} ({end-start} lines)', flush=True)

    print('Analysing offset accesses ...', flush=True)
    offsets = analyse_char_editor_struct(lines, start, end)
    print(f'  {len(offsets)} unique offsets found', flush=True)

    print('Mapping sliders to offsets ...', flush=True)
    known = find_known_offsets(lines, start, end)
    print(f'  {len(known)} slider-offset mappings', flush=True)

    print('Finding setBonePositionalSize calls ...', flush=True)
    positional = find_all_setBonePositionalSize(lines, start, end)
    print(f'  {len(positional)} setBonePositionalSize calls', flush=True)

    write_report(offsets, known, positional, args.output, start+1, end+1, len(lines))

if __name__ == '__main__':
    main()
