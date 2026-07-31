#!/usr/bin/env python3
"""Phase 4 probe: blendinfo.dat structure (one-shot, not production code).

Confirmed: magic "KBI1" + two little-endian int32 header fields (both =32
on the reference file). Payload after the 12-byte header is 1,364,340 bytes
-- does NOT divide evenly against any simple WxH*bytes-per-record grid
tried (32/64/128/256/512/1024 sides x 1/2/4/20 bytes/record). 0xFF bytes
recur at an approximate (not exact) 20-byte stride with periodic larger
gaps (~120-160 bytes) -- consistent with variable-length or row-padded
records, not confirmed further. Real record layout: [ПОТРЕБУЄ ПЕРЕВІРКИ].
"""
import struct
import sys


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "tmp_/kenshi/data/newland/land/blendinfo.dat"
    with open(path, "rb") as f:
        data = f.read()
    magic = data[0:4]
    v1, v2 = struct.unpack_from("<ii", data, 4)
    payload = data[12:]
    print(f"magic={magic!r} header_ints=({v1},{v2}) total={len(data)} payload={len(payload)}")
    ff_positions = [i for i, b in enumerate(payload[:2000]) if b == 0xFF]
    diffs = [ff_positions[i + 1] - ff_positions[i] for i in range(len(ff_positions) - 1)]
    print("first 0xFF diffs:", diffs[:40])


if __name__ == "__main__":
    main()
