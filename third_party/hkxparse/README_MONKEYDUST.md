# hkxparse (patched) — vendored subset

Source: https://github.com/exyorha/hkxparse (MIT license, see LICENSE).

Only the **tagfile** parsing path is vendored (`HKXMapping`, `HKXTagfileParser`,
`HKXTypes`, `Deserializer`, `TagfileTypes`, `LayoutRules`). The packfile path
(`HKXPackfileLoader`, `PackfileTypes`) and `HKXFile`/`PrettyPrinter` are
**not** vendored — packfile parsing needs a Ruby-generated "packfile layout"
file (Skyrim-specific, unpublished tool per upstream's README) we don't need,
since Kenshi's `.hkt` navmesh tiles are tagfiles, not packfiles.
`kenshi_navmesh_extract/main.cpp` talks to `HKXMapping`/`HKXTagfileParser`
directly instead of going through `HKXFile`.

## Patches applied (upstream is Windows/MSVC-oriented; this is a Linux/GCC build)

- `msvc_compat.h` (new file, force-included via `-include`): GCC-portable
  `_byteswap_ushort/_ulong/_uint64` (only exercised on the big-endian branch,
  never taken for Kenshi's little-endian PC files) + `<vector>`/`<cstring>`/
  `<cstddef>` (MSVC's STL transitively pulls these in; libstdc++ doesn't).

- `HKXTagfileParser.cpp`:
  - `TagFileInfo` version 5 support: reads the havok-version string like v4,
    then 6 more single-byte varints (values empirically observed as
    `-10 0 0 0 10 0`; meaning not understood, only the byte alignment against
    real Kenshi `.hkt` files is verified).
  - Added `TagObject`/`TagObjectBackref`/`TagObjectNull` handling — these are
    real, documented Havok tagfile tag values (confirmed against the real
    Havok SDK header, `hkBinaryTagfileCommon.h`, via a Project Anarchy source
    mirror) that upstream's switch never handled at all.
  - **Real bug fix**: a negative array length (silently produced for large/
    complex navmeshes — confirmed via a 30-file batch test spanning
    1.8KB-380KB) was passed straight to `std::vector::resize()`, underflowing
    to a huge value and crashing (`std::length_error`). Clamped to 0.
  - **Recovery heuristic** (`tryResync`): large parts of Kenshi's `.hkt`
    tagfiles (specifically: whatever immediately follows a
    fully-and-correctly-parsed `hkaiNavMesh` object) could not be reverse
    engineered from any available documentation. Rather than abort, on an
    unrecognized tag (or a tag that resolves but produces garbled/non-ASCII
    field names — a coincidental false-tag match, confirmed to happen in
    practice on `TagMetadata` too, not just unhandled tags) the parser scans
    forward for the next position where a **trial parse** of a candidate
    `TagObjectRemember` object succeeds cleanly (real rollback of
    `m_stringPool`/`m_objects`/`m_nextAllocatedObject`/`m_stream` on
    failure), or where `TagFileEnd` appears. This sacrifices whatever the
    skipped bytes actually meant, but salvages every object that follows.
    Verified: the core navmesh geometry (`hkaiNavMesh.vertices` /
    `.faces` / `.edges` / `.aabb`) is read BEFORE this heuristic ever
    triggers in all tested files — it only affects secondary/auxiliary
    objects (e.g. the pathfinding "Graph", `hkaiDirectedGraphExplicitCost`),
    which occasionally remain unresolved (logged as a warning, not fatal).

  - **TagFileInfo(=1) can also occur as coincidental garbage**, not just
    TagMetadata: a stray byte deep in a navmesh's undocumented trailing data
    can decode to tag=1. Since a real tagfile has exactly one TagFileInfo (at
    the very start), any later occurrence is now rejected immediately and
    sent through `tryResync` instead of being trusted as a second header
    (previously crashed with an uncaught "out of bounds read" — found via 5
    files that failed the first full 3995-tile run: `tile8.4.hkt`,
    `tile11.16.hkt`, `tile14.60.hkt`, `tile45.22.hkt`, `tile48.25.hkt`).
  - **Debug tracing gated behind `HKX_TRACE` (default 0)**: upstream leaves
    per-field `printf`/`fprintf` tracing on unconditionally. On the full
    3995-tile dataset this produced tens of millions of lines (2GB+ for a
    partial run) — generating/writing that much text, not a parser hang or
    leak, is what destabilized the run environment on the first attempts.
    Flip `HKX_TRACE` to 1 (recompile) only for single-file debugging.

  Batch-tested against the FULL Kenshi dataset —
  **3995/3995 tiles extracted, 0 failed, 0 empty** (~60s wall time,
  `tmp_/kenshi_navmesh_obj/zone_X_Y.obj`, 303MB total). Earlier in this
  session a 30-tile size-representative sample also passed 30/30 before the
  full run surfaced the two issues above.
