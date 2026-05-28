#!/usr/bin/env python3
"""
merge_body_morphs.py
Merges body morph targets from md_human_morphs.glb into md_human_t.glb.

Both files must have exactly 15504 vertices.
Source morphs: tall, fat, muscular, longlegs, bighead, broadshdr  (6 targets)
Dest morphs:   wide_cheekbones, ... (23 face targets)
Result:        29 morph targets total

Run from repo root:
    python3 tools/md_mesh_conv/merge_body_morphs.py
"""

import json
import math
import os
import shutil
import struct
import sys

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROPS_DIR = os.path.join(REPO_ROOT, "game", "data", "props")
SRC_PATH  = os.path.join(PROPS_DIR, "md_human_morphs.glb")
DST_PATH  = os.path.join(PROPS_DIR, "md_human_t.glb")

EXPECTED_VERTS   = 15504
BYTES_PER_VERTEX = 12          # VEC3 FLOAT  (3 × 4 bytes)
MORPH_DATA_BYTES = EXPECTED_VERTS * BYTES_PER_VERTEX   # 186048 bytes per morph


# ---------------------------------------------------------------------------
# GLB low-level I/O
# ---------------------------------------------------------------------------

GLB_MAGIC         = 0x46546C67  # "glTF"
GLB_VERSION       = 2
CHUNK_TYPE_JSON   = 0x4E4F534A  # "JSON"
CHUNK_TYPE_BIN    = 0x004E4942  # "BIN\0"


def _pad4(n: int) -> int:
    return (n + 3) & ~3


def read_glb(path: str):
    """Return (json_dict, bin_bytes, raw_file_bytes)."""
    with open(path, "rb") as fh:
        raw = fh.read()

    if len(raw) < 12:
        raise ValueError(f"{path}: file too short")

    magic, version, total_len = struct.unpack_from("<III", raw, 0)
    if magic != GLB_MAGIC:
        raise ValueError(f"{path}: bad magic 0x{magic:08X}")
    if version != GLB_VERSION:
        raise ValueError(f"{path}: unsupported glTF version {version}")

    # Chunk 0 — JSON
    if len(raw) < 20:
        raise ValueError(f"{path}: truncated before JSON chunk")
    c0_len, c0_type = struct.unpack_from("<II", raw, 12)
    if c0_type != CHUNK_TYPE_JSON:
        raise ValueError(f"{path}: chunk 0 is not JSON (got 0x{c0_type:08X})")
    json_start = 20
    json_end   = json_start + c0_len
    j = json.loads(raw[json_start:json_end])

    # Chunk 1 — BIN (optional but expected)
    bin_data = b""
    bin_offset = json_end
    if bin_offset + 8 <= len(raw):
        c1_len, c1_type = struct.unpack_from("<II", raw, bin_offset)
        if c1_type == CHUNK_TYPE_BIN:
            bin_data = raw[bin_offset + 8 : bin_offset + 8 + c1_len]

    return j, bin_data, raw


def write_glb(path: str, j: dict, bin_data: bytes) -> None:
    """Serialise (json_dict, bin_bytes) back to a GLB file."""
    json_bytes = json.dumps(j, separators=(",", ":")).encode("utf-8")
    # Pad JSON to 4-byte boundary with spaces (0x20)
    json_pad = _pad4(len(json_bytes)) - len(json_bytes)
    json_chunk = json_bytes + b" " * json_pad

    # Pad BIN to 4-byte boundary with zeros
    bin_pad  = _pad4(len(bin_data)) - len(bin_data)
    bin_chunk = bin_data + b"\x00" * bin_pad

    json_chunk_len = len(json_chunk)
    bin_chunk_len  = len(bin_chunk)

    total_len = 12 + 8 + json_chunk_len + 8 + bin_chunk_len

    with open(path, "wb") as fh:
        # Header
        fh.write(struct.pack("<III", GLB_MAGIC, GLB_VERSION, total_len))
        # JSON chunk header + data
        fh.write(struct.pack("<II", json_chunk_len, CHUNK_TYPE_JSON))
        fh.write(json_chunk)
        # BIN chunk header + data
        fh.write(struct.pack("<II", bin_chunk_len, CHUNK_TYPE_BIN))
        fh.write(bin_chunk)


# ---------------------------------------------------------------------------
# Accessor helpers
# ---------------------------------------------------------------------------

COMPONENT_TYPE_FLOAT = 5126
ACCESSOR_TYPE_VEC3   = "VEC3"


def _get_accessor_bytes(j: dict, bin_data: bytes, acc_idx: int) -> bytes:
    """Return the raw bytes for an accessor (dense, no sparse, VEC3 FLOAT assumed)."""
    acc = j["accessors"][acc_idx]
    count = acc["count"]
    bv_idx = acc.get("bufferView")
    if bv_idx is None:
        # All-zero deltas — return zero bytes
        return bytes(count * BYTES_PER_VERTEX)

    bv  = j["bufferViews"][bv_idx]
    byte_offset  = acc.get("byteOffset", 0) + bv.get("byteOffset", 0)
    byte_stride  = bv.get("byteStride", BYTES_PER_VERTEX)

    if acc.get("componentType") != COMPONENT_TYPE_FLOAT:
        raise ValueError(f"Accessor {acc_idx}: expected FLOAT component type")
    if acc.get("type") != ACCESSOR_TYPE_VEC3:
        raise ValueError(f"Accessor {acc_idx}: expected VEC3 type")

    if byte_stride == BYTES_PER_VERTEX:
        # Tightly packed — read as a single slice
        return bin_data[byte_offset : byte_offset + count * BYTES_PER_VERTEX]
    else:
        # Strided — copy element by element
        out = bytearray(count * BYTES_PER_VERTEX)
        for i in range(count):
            src_off = byte_offset + i * byte_stride
            dst_off = i * BYTES_PER_VERTEX
            out[dst_off : dst_off + BYTES_PER_VERTEX] = bin_data[src_off : src_off + BYTES_PER_VERTEX]
        return bytes(out)


def _vertex_count(j: dict) -> int:
    meshes = j.get("meshes", [])
    if not meshes:
        raise ValueError("No meshes in GLB")
    prim = meshes[0]["primitives"][0]
    pos_acc_idx = prim["attributes"]["POSITION"]
    return j["accessors"][pos_acc_idx]["count"]


def _target_names(j: dict) -> list:
    meshes = j.get("meshes", [])
    if not meshes:
        return []
    return meshes[0].get("extras", {}).get("targetNames", [])


# ---------------------------------------------------------------------------
# Main merge logic
# ---------------------------------------------------------------------------

def merge(src_path: str, dst_path: str) -> int:
    """
    Merge body morph targets from src into dst.
    Returns the final number of morph targets in the updated dst file.
    """
    print(f"Reading source:      {src_path}")
    src_j, src_bin, _ = read_glb(src_path)

    print(f"Reading destination: {dst_path}")
    dst_j, dst_bin, _ = read_glb(dst_path)

    # Vertex-count sanity check
    src_vc = _vertex_count(src_j)
    dst_vc = _vertex_count(dst_j)
    print(f"Vertex counts — src: {src_vc}, dst: {dst_vc}")
    if src_vc != EXPECTED_VERTS:
        raise ValueError(f"Source has {src_vc} vertices, expected {EXPECTED_VERTS}")
    if dst_vc != EXPECTED_VERTS:
        raise ValueError(f"Destination has {dst_vc} vertices, expected {EXPECTED_VERTS}")
    if src_vc != dst_vc:
        raise ValueError("Vertex count mismatch between source and destination")

    # Identify morphs to copy
    src_names = _target_names(src_j)
    dst_names = _target_names(dst_j)
    print(f"Source morphs ({len(src_names)}):      {src_names}")
    print(f"Destination morphs ({len(dst_names)}): {dst_names}")

    src_prim = src_j["meshes"][0]["primitives"][0]
    src_targets = src_prim.get("targets", [])

    to_add = []
    for i, name in enumerate(src_names):
        if name in dst_names:
            print(f"  SKIP '{name}' (already in destination)")
        else:
            to_add.append((i, name))

    if not to_add:
        print("Nothing to merge — destination already has all source morphs.")
        return len(dst_names)

    print(f"\nMorphs to merge: {[n for _, n in to_add]}")

    # Work on mutable copies of dst JSON structures
    dst_accessors   = dst_j.setdefault("accessors", [])
    dst_buffer_views = dst_j.setdefault("bufferViews", [])
    dst_prim        = dst_j["meshes"][0]["primitives"][0]
    dst_targets_list = dst_prim.setdefault("targets", [])
    dst_extras       = dst_j["meshes"][0].setdefault("extras", {})
    dst_target_names = dst_extras.setdefault("targetNames", [])

    # Binary data to append (grows as we add morphs)
    new_bin = bytearray(dst_bin)

    for src_idx, name in to_add:
        src_target = src_targets[src_idx]
        pos_acc_idx = src_target.get("POSITION")
        if pos_acc_idx is None:
            print(f"  WARNING: morph '{name}' has no POSITION accessor — skipping")
            continue

        # Read the delta bytes from source
        delta_bytes = _get_accessor_bytes(src_j, src_bin, pos_acc_idx)
        assert len(delta_bytes) == MORPH_DATA_BYTES, (
            f"Expected {MORPH_DATA_BYTES} bytes for '{name}', got {len(delta_bytes)}"
        )

        # Append to destination binary
        bv_byte_offset = len(new_bin)
        new_bin += delta_bytes

        # New bufferView
        new_bv_idx = len(dst_buffer_views)
        dst_buffer_views.append({
            "buffer": 0,
            "byteOffset": bv_byte_offset,
            "byteLength": MORPH_DATA_BYTES,
            "target": 34962,    # ARRAY_BUFFER
        })

        # New accessor
        new_acc_idx = len(dst_accessors)
        dst_accessors.append({
            "bufferView":    new_bv_idx,
            "byteOffset":    0,
            "componentType": COMPONENT_TYPE_FLOAT,
            "count":         EXPECTED_VERTS,
            "type":          ACCESSOR_TYPE_VEC3,
            # min/max omitted — not strictly required for morph deltas
        })

        # Add to primitive targets
        dst_targets_list.append({"POSITION": new_acc_idx})
        dst_target_names.append(name)

        print(f"  ADDED '{name}': bufferView={new_bv_idx}, accessor={new_acc_idx}, "
              f"byteOffset={bv_byte_offset}")

    # Update buffer byteLength
    dst_j["buffers"][0]["byteLength"] = len(new_bin)

    # Backup
    bak_path = dst_path + ".bak"
    print(f"\nBacking up destination to: {bak_path}")
    shutil.copy2(dst_path, bak_path)

    # Write updated GLB
    print(f"Writing updated GLB: {dst_path}")
    write_glb(dst_path, dst_j, bytes(new_bin))

    final_count = len(dst_target_names)
    print(f"\nDone. Final morph count: {final_count}")
    return final_count


# ---------------------------------------------------------------------------
# Verification
# ---------------------------------------------------------------------------

def verify(path: str, expected_count: int) -> None:
    print(f"\n--- Verification ---")
    j, _, _ = read_glb(path)
    names = _target_names(j)
    vc    = _vertex_count(j)
    prim  = j["meshes"][0]["primitives"][0]
    n_targets = len(prim.get("targets", []))
    print(f"File:           {path}")
    print(f"Vertex count:   {vc}")
    print(f"Morph targets:  {n_targets}  (targetNames list: {len(names)})")
    print(f"Target names:   {names}")
    if n_targets == expected_count:
        print(f"PASS: morph count == {expected_count}")
    else:
        print(f"FAIL: expected {expected_count} morphs, got {n_targets}", file=sys.stderr)
        sys.exit(1)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    try:
        final = merge(SRC_PATH, DST_PATH)
        verify(DST_PATH, 29)
    except Exception as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)
        sys.exit(1)
