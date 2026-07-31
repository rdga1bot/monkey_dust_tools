#!/usr/bin/env python3
"""Phase 4 probe: blendinfo.dat structure hypotheses (one-shot, not production code).

Extends probe_blendinfo.py's magic-number finding with a systematic sweep
of plausible record structures. All results below are NEGATIVE (no
hypothesis matched exactly) -- documented honestly per the master plan's
own rule against inventing file structures. See
terrain_research/02_KENSHI_FORMAT.md Sec.4.2 for the write-up.

Findings on the reference file (tmp_/kenshi/data/newland/land/blendinfo.dat):
  - payload (post 12-byte header) = 1,364,340 bytes = 2^2 x 3 x 5 x 22739,
    where 22739 is PRIME -- rules out any fixed WxH grid (1024^2, 512^2,
    8192, etc. all fail to divide evenly).
  - uint32-count-prefixed and uint16-count-prefixed "32x32 blocks" hypotheses
    (entry_size swept 1-32 bytes): no exact match.
  - generic len-prefixed record scan: fails after one record.
  - byte histogram: only 66/256 values used, ~34% zero bytes -- inconsistent
    with raw float32 weights (would show near-uniform mantissa byte spread).
  - 0xFFFFFFFF sentinel (4-byte aligned): 0 occurrences.

Conclusion: true record layout is [NEEDS VERIFICATION] -- likely requires
RE of the BiomeSplitter generator tool itself, out of scope for a one-shot
probe script.
"""
import struct
import sys


def factorize(n):
    divs = []
    i = 1
    while i * i <= n:
        if n % i == 0:
            divs.append(i)
            divs.append(n // i)
        i += 1
    return sorted(set(divs))


def sweep_count_prefixed(payload, n, count_fmt, count_size, num_blocks=32 * 32):
    matches = []
    for entry_size in range(1, 33):
        off = 0
        ok = True
        for _ in range(num_blocks):
            if off + count_size > n:
                ok = False
                break
            cnt = struct.unpack_from(count_fmt, payload, off)[0]
            off += count_size
            if cnt > 100000:
                ok = False
                break
            off += cnt * entry_size
            if off > n:
                ok = False
                break
        if ok and off == n:
            matches.append(entry_size)
    return matches


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "tmp_/kenshi/data/newland/land/blendinfo.dat"
    with open(path, "rb") as f:
        data = f.read()
    magic = data[0:4]
    v1, v2 = struct.unpack_from("<ii", data, 4)
    payload = data[12:]
    n = len(payload)
    print(f"magic={magic!r} header=({v1},{v2}) total={len(data)} payload={n}")

    divs = factorize(n)
    print(f"payload factor count: {len(divs)}, max divisor <= sqrt(n): {divs[-1] if divs else None}")
    print("divisors:", divs)

    m32 = sweep_count_prefixed(payload, n, "<I", 4)
    print("uint32-count-prefixed 32x32-block exact matches (entry_size list):", m32)
    m16 = sweep_count_prefixed(payload, n, "<H", 2)
    print("uint16-count-prefixed 32x32-block exact matches (entry_size list):", m16)

    import collections
    hist = collections.Counter(payload)
    print(f"distinct byte values used: {len(hist)} / 256")
    print("top 10 byte values:", hist.most_common(10))

    sentinel_count = payload.count(b"\xff\xff\xff\xff")
    print("0xFFFFFFFF 4-byte sentinel count:", sentinel_count)


if __name__ == "__main__":
    main()
