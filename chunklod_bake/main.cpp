// chunklod_bake — Phase 3, docs/TERRAIN_CHUNKLOD_PORT_PLAN.md.
//
// Standalone C++ port (no SDL3, no engine deps -- mirrors
// tools/kenshi_navmesh_extract's isolation pattern) of the algorithm
// already verified in Python (tools/research/chunklod_spike.py +
// chunklod_mesh_spike.py), which itself is a direct port of the real
// public-domain heightfield_chunker.cpp (tmp_/chunklod_reference/,
// Thatcher Ulrich, tu-testbed).
//
// New in this phase: real 16-bit vertex quantization, exactly matching
// Ulrich's own scheme (mesh::write_vertex, heightfield_chunker.cpp:1496-1527):
//   x,z -- 14-bit fixed point relative to the node's bounding box
//          (compress_factor = (1<<14) / max(1, box_extent))
//   y   -- raw Sint16 discrete height units (metres / vertical_scale)
//   morph_delta -- Sint16 difference vs the same vertex at the next
//          coarser LOD (for runtime lerp-based geomorphing)
//
// This tool bakes ONE zone's ROOT (coarsest) and FINEST (level=0) nodes,
// quantizes them, writes a small binary file, reads it back, dequantizes,
// and re-runs the Phase 2 sanity checks (skirt-below-surface,
// triangle-count match against the known-correct Python output) against
// the DEQUANTIZED data -- the actual Phase 3 gate: confirm the
// quantize/dequantize round-trip doesn't introduce a real bug or an
// error larger than the quantization step's own theoretical bound.
//
// Build (standalone, for fast iteration before CMake registration):
//   g++ -std=c++17 -O2 -o /tmp/chunklod_bake tools/chunklod_bake/main.cpp
//   /tmp/chunklod_bake --zone 23 34 --max-error 2.0

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <map>
#include <algorithm>
#include <string>

// ── world_hmap.r16 reader — mirrors tools/md_hmap_io.py exactly ──────────
namespace hmap {
constexpr int ATLAS_ZONES = 64;
constexpr int ATLAS_VERTS = 129;
constexpr int PNG_SIZE = ATLAS_ZONES * ATLAS_VERTS;  // 8256
constexpr float HEIGHT_MAX_M = 980.0f;
constexpr uint32_t R16_MAGIC = 0x3631524D;

// Returns a (ATLAS_VERTS x ATLAS_VERTS) row-major grid of heights in
// metres for zone (zx,zy), or empty on failure.
std::vector<float> LoadZone(const char* path, int zx, int zy) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return {}; }
    uint32_t header[2];
    if (std::fread(header, sizeof(uint32_t), 2, f) != 2 ||
        header[0] != R16_MAGIC || header[1] != ATLAS_ZONES) {
        std::fprintf(stderr, "bad r16 header in %s\n", path);
        std::fclose(f);
        return {};
    }
    // Full grid is (PNG_SIZE x PNG_SIZE) uint16, row-major:
    //   arr16[zy*ATLAS_VERTS + row, zx*ATLAS_VERTS + col]
    // We only need one zone's block, but the file is one contiguous
    // raster (not per-zone chunked on disk), so we must seek per row.
    std::vector<uint16_t> row(ATLAS_VERTS);
    std::vector<float> out(ATLAS_VERTS * ATLAS_VERTS);
    const long data_start = 8;  // after the 8-byte header
    for (int r = 0; r < ATLAS_VERTS; ++r) {
        long global_row = (long)zy * ATLAS_VERTS + r;
        long global_col0 = (long)zx * ATLAS_VERTS;
        long offset = data_start + (global_row * PNG_SIZE + global_col0) * (long)sizeof(uint16_t);
        if (std::fseek(f, offset, SEEK_SET) != 0 ||
            std::fread(row.data(), sizeof(uint16_t), ATLAS_VERTS, f) != (size_t)ATLAS_VERTS) {
            std::fprintf(stderr, "short read at zone (%d,%d) row %d\n", zx, zy, r);
            std::fclose(f);
            return {};
        }
        for (int c = 0; c < ATLAS_VERTS; ++c) {
            out[r * ATLAS_VERTS + c] = row[c] * (HEIGHT_MAX_M / 65535.0f);
        }
    }
    std::fclose(f);
    return out;
}
}  // namespace hmap

// ── Heightfield: direct port of chunklod_spike.py's Heightfield class ───
// (itself a direct port of heightfield_chunker.cpp's struct heightfield,
// lines 342-424, simplified the same way the Python version was: heights
// stored directly in metres, activation level as a plain int8 grid, no
// mmap-array bit-packing trick.)
struct Heightfield {
    int size = 0, log_size = 0, root_level = 0;
    float vertical_scale = 1.0f;
    std::vector<float> h;       // size*size, row-major [z*size+x]
    std::vector<int8_t> level;  // size*size, -1 = unset
    int64_t activations = 0;

    void Init(const std::vector<float>& grid, int n, float vscale) {
        size = n;
        log_size = (int)std::lround(std::log2((double)(size - 1)));
        if ((1 << log_size) + 1 != size) {
            std::fprintf(stderr, "heightfield size %d is not (2^N)+1\n", size);
            std::exit(1);
        }
        vertical_scale = vscale;
        h = grid;
        level.assign((size_t)size * size, -1);
    }
    float Height(int x, int z) const { return h[(size_t)z * size + x]; }
    int Level(int x, int z) const { return level[(size_t)z * size + x]; }
    void SetLevel(int x, int z, int lev) { level[(size_t)z * size + x] = (int8_t)lev; }
    void Activate(int x, int z, int lev) {
        int cur = Level(x, z);
        if (lev > cur) {
            if (cur == -1) activations++;
            SetLevel(x, z, lev);
        }
    }
};

// Direct port of heightfield_chunker.cpp's update() -- lines 750-782.
static void Update(Heightfield& hf, float base_max_error, int ax, int az, int rx, int rz, int lx, int lz) {
    int dx = lx - rx, dz = lz - rz;
    if (std::abs(dx) <= 1 && std::abs(dz) <= 1) return;

    int bx = rx + (dx >> 1), bz = rz + (dz >> 1);
    float error = std::fabs(hf.Height(bx, bz) - (hf.Height(lx, lz) + hf.Height(rx, rz)) / 2.0f) * hf.vertical_scale;
    if (error >= base_max_error) {
        int activation_level = (int)std::floor(std::log2(error / base_max_error) + 0.5);
        hf.Activate(bx, bz, activation_level);
    }
    Update(hf, base_max_error, bx, bz, ax, az, rx, rz);
    Update(hf, base_max_error, bx, bz, lx, lz, ax, az);
}

// Direct port of propagate_activation_level() -- lines 863-915.
static void PropagateActivationLevel(Heightfield& hf, int cx, int cz, int lvl, int target_level) {
    int half = 1 << lvl, quarter = half >> 1;
    if (lvl > target_level) {
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i)
                PropagateActivationLevel(hf, cx - quarter + half * i, cz - quarter + half * j, lvl - 1, target_level);
        return;
    }
    if (lvl > 0) {
        int lev;
        lev = hf.Level(cx + quarter, cz - quarter); hf.Activate(cx + half, cz, lev); hf.Activate(cx, cz - half, lev);
        lev = hf.Level(cx - quarter, cz - quarter); hf.Activate(cx, cz - half, lev); hf.Activate(cx - half, cz, lev);
        lev = hf.Level(cx - quarter, cz + quarter); hf.Activate(cx - half, cz, lev); hf.Activate(cx, cz + half, lev);
        lev = hf.Level(cx + quarter, cz + quarter); hf.Activate(cx, cz + half, lev); hf.Activate(cx + half, cz, lev);
    }
    hf.Activate(cx, cz, hf.Level(cx + half, cz));
    hf.Activate(cx, cz, hf.Level(cx, cz - half));
    hf.Activate(cx, cz, hf.Level(cx, cz + half));
    hf.Activate(cx, cz, hf.Level(cx - half, cz));
}

static int LowestOne(int x) {
    if (x == 0) return 32;
    int i = 0;
    while ((x & 1) == 0) { x >>= 1; ++i; }
    return i;
}
static int MinimumEdgeLod(const Heightfield& hf, int coord) {
    int l1 = LowestOne(coord);
    int depth = hf.log_size - l1 - 1;
    return std::clamp(hf.root_level - depth, 0, hf.root_level);
}
static int64_t HeightQueryRec(const Heightfield& hf, int level, int x, int z, int ax, int az, int rx, int rz, int lx, int lz) {
    if ((x == ax && z == az) || (x == rx && z == rz) || (x == lx && z == lz)) return (int64_t)std::lround(hf.Height(x, z));
    int dx = lx - rx, dz = lz - rz;
    if (std::abs(dx) <= 1 && std::abs(dz) <= 1) return (int64_t)std::lround(hf.Height(ax, az));
    int bx = rx + (dx >> 1), bz = rz + (dz >> 1);
    double edge_len_sq = (dx * (double)dx + dz * (double)dz) / 2.0;
    double sr = ((x - ax) * (double)(rx - ax) + (z - az) * (double)(rz - az)) / edge_len_sq;
    double sl = ((x - ax) * (double)(lx - ax) + (z - az) * (double)(lz - az)) / edge_len_sq;
    if (hf.Level(bx, bz) >= level) {
        return sr >= sl ? HeightQueryRec(hf, level, x, z, bx, bz, ax, az, rx, rz)
                         : HeightQueryRec(hf, level, x, z, bx, bz, lx, lz, ax, az);
    }
    double ay = hf.Height(ax, az);
    double dr = hf.Height(rx, rz) - ay, dl = hf.Height(lx, lz) - ay;
    return (int64_t)std::floor(ay + sl * dl + sr * dr + 0.5);
}
static int64_t GetHeightAtLOD(const Heightfield& hf, int level, int x, int z) {
    int n = hf.size;
    if (z > x) return HeightQueryRec(hf, level, x, z, 0, n - 1, n - 1, n - 1, 0, 0);
    return HeightQueryRec(hf, level, x, z, n - 1, 0, 0, 0, n - 1, n - 1);
}

// ── Mesh: direct port of the `mesh` namespace, lines 1381-1695 ──────────
struct Vert { int x, z; double y; bool special; };
struct Mesh {
    std::vector<Vert> vertices;
    std::vector<int> indices;
    std::map<std::pair<int, int>, int> index_table;

    int GetVertexIndex(const Heightfield& hf, int x, int z) {
        auto key = std::make_pair(x, z);
        auto it = index_table.find(key);
        if (it != index_table.end()) return it->second;
        int idx = (int)vertices.size();
        vertices.push_back({x, z, hf.Height(x, z), false});
        index_table[key] = idx;
        return idx;
    }
    int SpecialVertexIndex(int x, double y, int z) {
        int idx = (int)vertices.size();
        vertices.push_back({x, z, y, true});
        return idx;
    }
    void EmitVertex(const Heightfield& hf, int x, int z) { indices.push_back(GetVertexIndex(hf, x, z)); }
    void EmitSpecialVertex(int x, double y, int z) { indices.push_back(SpecialVertexIndex(x, y, z)); }
    void EmitPreviousVertex() { indices.push_back(indices.back()); }
    int IndexCount() const { return (int)indices.size(); }
};

struct GenState {
    int buf[2][2] = {{-1, -1}, {-1, -1}};
    int activation_level = 0, ptr = 0, previous_level = 0;
    bool InBuffer(int x, int z) const { return (x == buf[0][0] && z == buf[0][1]) || (x == buf[1][0] && z == buf[1][1]); }
    void SetBuffer(int x, int z) { buf[ptr][0] = x; buf[ptr][1] = z; }
};

// Direct port of generate_quadrant() -- lines 1199-1228.
static void GenerateQuadrant(const Heightfield& hf, Mesh& mesh, GenState& s, int lx, int lz, int tx, int tz, int rx, int rz, int recursion_level) {
    if (recursion_level <= 0) return;
    if (hf.Level(tx, tz) >= s.activation_level) {
        int bx = (lx + rx) >> 1, bz = (lz + rz) >> 1;
        GenerateQuadrant(hf, mesh, s, lx, lz, bx, bz, tx, tz, recursion_level - 1);
        if (!s.InBuffer(tx, tz)) {
            if ((recursion_level + s.previous_level) & 1) {
                s.ptr ^= 1;
            } else {
                int x = s.buf[1 - s.ptr][0], z = s.buf[1 - s.ptr][1];
                mesh.EmitVertex(hf, x, z);
            }
            mesh.EmitVertex(hf, tx, tz);
            s.SetBuffer(tx, tz);
            s.previous_level = recursion_level;
        }
        GenerateQuadrant(hf, mesh, s, tx, tz, bx, bz, rx, rz, recursion_level - 1);
    }
}

// Direct port of generate_block() -- lines 1139-1196.
static void GenerateBlock(const Heightfield& hf, Mesh& mesh, int activation_level, int log_size, int cx, int cz) {
    int hs = 1 << (log_size - 1);
    int q[4][2] = {{cx + hs, cz + hs}, {cx + hs, cz - hs}, {cx - hs, cz - hs}, {cx - hs, cz + hs}};
    GenState s;
    s.activation_level = activation_level;
    mesh.EmitVertex(hf, q[0][0], q[0][1]);
    s.SetBuffer(q[0][0], q[0][1]);
    for (int i = 0; i < 4; ++i) {
        if ((s.previous_level & 1) == 0) {
            s.ptr ^= 1;
        } else {
            int x = s.buf[1 - s.ptr][0], z = s.buf[1 - s.ptr][1];
            mesh.EmitVertex(hf, x, z);
        }
        mesh.EmitVertex(hf, q[i][0], q[i][1]);
        s.SetBuffer(q[i][0], q[i][1]);
        s.previous_level = 2 * log_size + 1;
        GenerateQuadrant(hf, mesh, s, q[i][0], q[i][1], cx, cz, q[(i + 1) & 3][0], q[(i + 1) & 3][1], 2 * log_size);
    }
    if (!s.InBuffer(q[0][0], q[0][1])) mesh.EmitVertex(hf, q[0][0], q[0][1]);
}

// Direct port of generate_edge_data() -- lines 1231-1377.
static void GenerateEdgeData(const Heightfield& hf, Mesh& mesh, int x0, int z0, int x1, int z1, int level) {
    int dx = 0, dz = 0, steps = 0;
    if (x0 < x1) { dx = 1; steps = x1 - x0 + 1; }
    else if (x0 > x1) { dx = -1; steps = x0 - x1 + 1; }
    else if (z0 < z1) { dz = 1; steps = z1 - z0 + 1; }
    else { dz = -1; steps = z0 - z1 + 1; }

    const int MAX_NEIGHBOR_DIFF = 2;
    std::vector<double> vert_minimums;
    double current_min = hf.Height(x0, z0);
    int x = x0, z = z0;
    for (int i = 0; i < steps; ++i) {
        current_min = std::min(current_min, (double)hf.Height(x, z));
        if (hf.Level(x, z) >= level) {
            int major_coord = (dz == 0) ? x0 : z0;
            int min_edge_lod = MinimumEdgeLod(hf, major_coord);
            int level_diff = std::min(std::min(min_edge_lod + 1, hf.root_level) - level, MAX_NEIGHBOR_DIFF);
            for (int lod = level; lod <= level + level_diff; ++lod) {
                double lod_h = (double)GetHeightAtLOD(hf, lod, x, z);
                current_min = std::min(current_min, lod_h);
            }
            if (current_min > -32768) current_min -= 1;
            vert_minimums.push_back(current_min);
            current_min = hf.Height(x, z);
        }
        x += dx; z += dz;
    }

    mesh.EmitPreviousVertex();
    if ((mesh.IndexCount() & 1) == 0) mesh.EmitPreviousVertex();

    int vert_index = 0;
    x = x0; z = z0;
    for (int i = 0; i < steps; ++i) {
        if (hf.Level(x, z) >= level) {
            double min_h = vert_minimums[vert_index];
            if ((int)vert_minimums.size() > vert_index + 1) min_h = std::min(min_h, vert_minimums[vert_index + 1]);
            mesh.EmitVertex(hf, x, z);
            if (i == 0) mesh.EmitPreviousVertex();
            mesh.EmitSpecialVertex(x, min_h, z);
            vert_index++;
        }
        x += dx; z += dz;
    }
}

static Mesh GenerateNodeMesh(Heightfield& hf, int x0, int z0, int log_size, int level) {
    int size = 1 << log_size, half = size >> 1;
    int cx = x0 + half, cz = z0 + half;
    hf.Activate(x0 + size, z0, level);
    hf.Activate(x0, z0, level);
    hf.Activate(x0, z0 + size, level);
    hf.Activate(x0 + size, z0 + size, level);

    Mesh mesh;
    GenerateBlock(hf, mesh, level, log_size, cx, cz);
    GenerateEdgeData(hf, mesh, cx + half, cz + half, cx + half, cz - half, level);
    GenerateEdgeData(hf, mesh, cx + half, cz - half, cx - half, cz - half, level);
    GenerateEdgeData(hf, mesh, cx - half, cz - half, cx - half, cz + half, level);
    GenerateEdgeData(hf, mesh, cx - half, cz + half, cx + half, cz + half, level);
    return mesh;
}

struct Tri { int a, b, c; };
static std::vector<Tri> StripToTriangles(const Mesh& mesh) {
    std::vector<Tri> tris;
    const auto& idx = mesh.indices;
    for (size_t i = 0; i + 2 < idx.size(); ++i) {
        int a = idx[i], b = idx[i + 1], c = idx[i + 2];
        if (a == b || b == c || a == c) continue;
        if (i % 2 == 0) tris.push_back({a, b, c});
        else tris.push_back({a, c, b});
    }
    return tris;
}

// ── Quantization: exact port of mesh::write_vertex, lines 1496-1527 ─────
struct QuantVert { int16_t x, y, z, morph_delta; bool special; };

static std::vector<QuantVert> QuantizeMesh(const Heightfield& hf, const Mesh& mesh, int level, float sample_spacing) {
    double minx = 1e30, maxx = -1e30, minz = 1e30, maxz = -1e30;
    for (auto& v : mesh.vertices) {
        minx = std::min(minx, (double)v.x); maxx = std::max(maxx, (double)v.x);
        minz = std::min(minz, (double)v.z); maxz = std::max(maxz, (double)v.z);
    }
    double box_center_x = (minx + maxx) * 0.5 * sample_spacing;
    double box_center_z = (minz + maxz) * 0.5 * sample_spacing;
    double box_extent_x = (maxx - minx) * 0.5 * sample_spacing;
    double box_extent_z = (maxz - minz) * 0.5 * sample_spacing;
    double compress_x = (1 << 14) / std::max(1.0, box_extent_x);
    double compress_z = (1 << 14) / std::max(1.0, box_extent_z);

    std::vector<QuantVert> out;
    out.reserve(mesh.vertices.size());
    for (auto& v : mesh.vertices) {
        QuantVert q{};
        q.x = (int16_t)std::floor(((v.x * sample_spacing - box_center_x) * compress_x) + 0.5);
        q.z = (int16_t)std::floor(((v.z * sample_spacing - box_center_z) * compress_z) + 0.5);
        q.y = (int16_t)std::lround(v.y);  // height already in our own real unit (metres); Sint16 covers 0..980m at 1 unit=1m easily
        q.special = v.special;
        double lerped = v.special ? v.y : (double)GetHeightAtLOD(hf, level + 1, v.x, v.z);
        double morph = lerped - (double)q.y;
        q.morph_delta = (int16_t)std::lround(morph);
        out.push_back(q);
    }
    return out;
}

// Dequantize back to world-space floats (mirrors chunklod.cpp's
// morph_vertices(), but f=0 -- fully at this node's own LOD, no morph
// blend applied, since the Phase 3 gate is about quantization error,
// not the morph blend itself).
struct DequantVert { double x, y, z; bool special; };
static std::vector<DequantVert> DequantizeMesh(const std::vector<QuantVert>& qverts, double box_center_x, double box_center_z, double box_extent_x, double box_extent_z) {
    double sx = box_extent_x / (1 << 14), sz = box_extent_z / (1 << 14);
    std::vector<DequantVert> out;
    out.reserve(qverts.size());
    for (auto& q : qverts) {
        out.push_back({box_center_x + q.x * sx, (double)q.y, box_center_z + q.z * sz, q.special});
    }
    return out;
}

static void WriteQuantFile(const char* path, const std::vector<QuantVert>& verts, const std::vector<Tri>& tris) {
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path); return; }
    uint32_t vcount = (uint32_t)verts.size(), tcount = (uint32_t)tris.size();
    std::fwrite(&vcount, sizeof(vcount), 1, f);
    std::fwrite(&tcount, sizeof(tcount), 1, f);
    std::fwrite(verts.data(), sizeof(QuantVert), verts.size(), f);
    std::fwrite(tris.data(), sizeof(Tri), tris.size(), f);
    std::fclose(f);
}
static bool ReadQuantFile(const char* path, std::vector<QuantVert>& verts, std::vector<Tri>& tris) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    uint32_t vcount = 0, tcount = 0;
    if (std::fread(&vcount, sizeof(vcount), 1, f) != 1 || std::fread(&tcount, sizeof(tcount), 1, f) != 1) { std::fclose(f); return false; }
    verts.resize(vcount);
    tris.resize(tcount);
    bool ok = std::fread(verts.data(), sizeof(QuantVert), vcount, f) == vcount &&
              std::fread(tris.data(), sizeof(Tri), tcount, f) == tcount;
    std::fclose(f);
    return ok;
}

// ── Engine-facing mesh export (Phase 4, runtime spike) ───────────────────
// Real (non-quantized -- Phase 3 already proved quantization round-trips
// correctly, this export exists to feed a live GPU comparison, not to
// re-test compression) float32 position + per-vertex normal, in the
// mesh's own LOCAL space (not tied to any real-world zone-origin
// convention -- docs/kenshi/03_reconciled_model.md explicitly leaves the
// world-space origin offset as [UNKNOWN], so this spike places the mesh
// at a self-chosen debug location in the editor rather than attempting
// to reverse-engineer that separately, per the port plan's Phase 4
// scoping).
struct EngineVertex { float x, y, z, nx, ny, nz; };

static std::vector<EngineVertex> ComputeSmoothNormals(const Mesh& mesh, const std::vector<Tri>& tris, float sample_spacing) {
    std::vector<EngineVertex> verts(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        auto& v = mesh.vertices[i];
        verts[i] = {v.x * sample_spacing, (float)v.y, v.z * sample_spacing, 0, 0, 0};
    }
    for (auto& t : tris) {
        float ax = verts[t.a].x, ay = verts[t.a].y, az = verts[t.a].z;
        float bx = verts[t.b].x, by = verts[t.b].y, bz = verts[t.b].z;
        float cx = verts[t.c].x, cy = verts[t.c].y, cz = verts[t.c].z;
        float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
        float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
        // Cross product e1 x e2 -- winding matches StripToTriangles' own
        // (a,b,c)/(a,c,b) alternation, chosen so this points +Y (up) for
        // a normal heightfield triangle.
        float nx = e1y * e2z - e1z * e2y;
        float ny = e1z * e2x - e1x * e2z;
        float nz = e1x * e2y - e1y * e2x;
        for (int idx : {t.a, t.b, t.c}) { verts[idx].nx += nx; verts[idx].ny += ny; verts[idx].nz += nz; }
    }
    for (auto& v : verts) {
        float len = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
        if (len > 1e-6f) { v.nx /= len; v.ny /= len; v.nz /= len; }
        else { v.nx = 0; v.ny = 1; v.nz = 0; }
        if (v.ny < 0) { v.nx = -v.nx; v.ny = -v.ny; v.nz = -v.nz; }  // heightfield: normal always points up
    }
    return verts;
}

static void WriteEngineMesh(const char* path, const std::vector<EngineVertex>& verts, const std::vector<Tri>& tris) {
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path); return; }
    uint32_t vcount = (uint32_t)verts.size(), icount = (uint32_t)tris.size() * 3;
    std::fwrite(&vcount, sizeof(vcount), 1, f);
    std::fwrite(&icount, sizeof(icount), 1, f);
    std::fwrite(verts.data(), sizeof(EngineVertex), verts.size(), f);
    std::fwrite(tris.data(), sizeof(uint32_t), icount, f);  // Tri{a,b,c} as 3 packed uint32 -- same layout
    std::fclose(f);
    std::printf("[chunklod_bake] wrote engine mesh: %u verts, %u indices -> %s\n", vcount, icount, path);
}

// ── Phase 5: full-map bake ────────────────────────────────────────────────
// Bakes all ATLAS_ZONES x ATLAS_ZONES zones' FINEST (level=0) mesh to
// <out_dir>/zone_<zx>_<zy>.mesh (WriteEngineMesh format), for the game-side
// ChunkLodWorld streaming manager to load on demand. No runtime distance-LOD
// switching -- each zone is baked at ONE fixed max_error, same simplification
// this whole phase has been honest about (docs/TERRAIN_CHUNKLOD_PORT_PLAN.md
// Phase 5's own scope note): a real chunklod runtime keeps the WHOLE
// activation-level node tree and switches nodes per-frame based on screen-space
// error (chunklod.cpp's compute_lod); this spike bakes only the single
// finest-LOD mesh per zone that Phase 2-4 already validated.
static int BakeAll(const char* hmap_path, const char* out_dir, float max_error, float sample_spacing) {
    char cmd[512];
    std::snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", out_dir);
    if (std::system(cmd) != 0) { std::fprintf(stderr, "mkdir -p %s failed\n", out_dir); return 1; }

    int64_t total_verts = 0, total_tris = 0, zones_written = 0;
    for (int zy = 0; zy < hmap::ATLAS_ZONES; ++zy) {
        for (int zx = 0; zx < hmap::ATLAS_ZONES; ++zx) {
            auto grid = hmap::LoadZone(hmap_path, zx, zy);
            if (grid.empty()) { std::fprintf(stderr, "[chunklod_bake] FAILED zone %d,%d -- aborting bake-all\n", zx, zy); return 1; }

            Heightfield hf;
            hf.Init(grid, hmap::ATLAS_VERTS, 1.0f);
            hf.root_level = hf.log_size - 1;
            int n = hf.size;
            Update(hf, max_error, 0, n - 1, n - 1, n - 1, 0, 0);
            Update(hf, max_error, n - 1, 0, 0, 0, n - 1, n - 1);
            for (int i = 0; i < hf.log_size; ++i) {
                PropagateActivationLevel(hf, n >> 1, n >> 1, hf.log_size - 1, i);
                PropagateActivationLevel(hf, n >> 1, n >> 1, hf.log_size - 1, i);
            }

            Mesh mesh_finest = GenerateNodeMesh(hf, 0, 0, hf.log_size, 0);
            auto tris_finest = StripToTriangles(mesh_finest);
            auto engine_verts = ComputeSmoothNormals(mesh_finest, tris_finest, sample_spacing);

            char path[600];
            std::snprintf(path, sizeof(path), "%s/zone_%d_%d.mesh", out_dir, zx, zy);
            FILE* f = std::fopen(path, "wb");
            if (!f) { std::fprintf(stderr, "cannot write %s\n", path); return 1; }
            uint32_t vcount = (uint32_t)engine_verts.size(), icount = (uint32_t)tris_finest.size() * 3;
            std::fwrite(&vcount, sizeof(vcount), 1, f);
            std::fwrite(&icount, sizeof(icount), 1, f);
            std::fwrite(engine_verts.data(), sizeof(EngineVertex), engine_verts.size(), f);
            std::fwrite(tris_finest.data(), sizeof(uint32_t), icount, f);
            std::fclose(f);

            total_verts += (int64_t)vcount;
            total_tris  += (int64_t)tris_finest.size();
            ++zones_written;
        }
        std::printf("[chunklod_bake] row %d/%d done (%lld zones, %lld verts, %lld tris so far)\n",
                     zy + 1, hmap::ATLAS_ZONES, (long long)zones_written, (long long)total_verts, (long long)total_tris);
    }
    std::printf("[chunklod_bake] bake-all DONE: %lld zones -> %s (total %lld verts, %lld tris, max_error=%.2fm)\n",
                (long long)zones_written, out_dir, (long long)total_verts, (long long)total_tris, max_error);
    return 0;
}

int main(int argc, char** argv) {
    int zx = -1, zy = -1;
    float max_error = 2.0f;
    const char* hmap_path = "game/data/terrain/world_hmap.r16";
    const char* out_path = "/tmp/chunklod_bake_zone.bin";
    const char* mesh_out_path = nullptr;
    const char* bake_all_dir = nullptr;
    // 3.6 m/span between ATLAS_VERTS samples -- NOT the raw 1.8 m/px TIF
    // resolution. world_hmap.r16's 129 verts/zone are a 2:1 downsample of
    // the raw 258x258 fetch (docs/kenshi/03_reconciled_model.md §2: "129x129
    // ... the PhysX heightfield collision resolution per zone, 3.6 m/span
    // ... not the visual rendering resolution"). 128 spans * 3.6m = 460.8m
    // = CHUNK_SIZE_M exactly. An earlier version of this tool (and the
    // Phase 1/2 Python spikes) used 1.8 here -- harmless there since those
    // never placed geometry in real editor world space, only checked
    // relative topology/error-metric correctness, but a real bug once
    // this mesh needs to align with TerrainQuadtreeRenderer's real footprint.
    const float sample_spacing = 3.6f;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--zone") && i + 2 < argc) { zx = std::atoi(argv[++i]); zy = std::atoi(argv[++i]); }
        else if (!std::strcmp(argv[i], "--max-error") && i + 1 < argc) { max_error = (float)std::atof(argv[++i]); }
        else if (!std::strcmp(argv[i], "--hmap") && i + 1 < argc) { hmap_path = argv[++i]; }
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) { out_path = argv[++i]; }
        else if (!std::strcmp(argv[i], "--write-mesh") && i + 1 < argc) { mesh_out_path = argv[++i]; }
        else if (!std::strcmp(argv[i], "--bake-all") && i + 1 < argc) { bake_all_dir = argv[++i]; }
    }

    if (bake_all_dir) return BakeAll(hmap_path, bake_all_dir, max_error, sample_spacing);

    if (zx < 0 || zy < 0) { std::fprintf(stderr, "usage: %s --zone ZX ZY [--max-error E] [--hmap path] [--out path]\n"
                                                   "   or: %s --bake-all OUT_DIR [--max-error E] [--hmap path]\n", argv[0], argv[0]); return 1; }

    std::printf("[chunklod_bake] loading zone %d,%d (max_error=%.2fm)...\n", zx, zy, max_error);
    auto grid = hmap::LoadZone(hmap_path, zx, zy);
    if (grid.empty()) return 1;

    Heightfield hf;
    hf.Init(grid, hmap::ATLAS_VERTS, 1.0f);
    hf.root_level = hf.log_size - 1;

    int n = hf.size;
    Update(hf, max_error, 0, n - 1, n - 1, n - 1, 0, 0);
    Update(hf, max_error, n - 1, 0, 0, 0, n - 1, n - 1);
    for (int i = 0; i < hf.log_size; ++i) {
        PropagateActivationLevel(hf, n >> 1, n >> 1, hf.log_size - 1, i);
        PropagateActivationLevel(hf, n >> 1, n >> 1, hf.log_size - 1, i);
    }

    // Root (coarsest) and finest -- same two cases as the Python spike,
    // for a direct, exact cross-check of both ports.
    Mesh mesh_root = GenerateNodeMesh(hf, 0, 0, hf.log_size, hf.root_level);
    auto tris_root = StripToTriangles(mesh_root);
    std::printf("[chunklod_bake] ROOT node (level=%d): %zu verts, %d strip indices, %zu real triangles\n",
                hf.root_level, mesh_root.vertices.size(), mesh_root.IndexCount(), tris_root.size());

    Mesh mesh_finest = GenerateNodeMesh(hf, 0, 0, hf.log_size, 0);
    auto tris_finest = StripToTriangles(mesh_finest);
    std::printf("[chunklod_bake] FINEST node (level=0): %zu verts, %d strip indices, %zu real triangles\n",
                mesh_finest.vertices.size(), mesh_finest.IndexCount(), tris_finest.size());

    if (mesh_out_path) {
        auto engine_verts = ComputeSmoothNormals(mesh_finest, tris_finest, sample_spacing);
        WriteEngineMesh(mesh_out_path, engine_verts, tris_finest);
    }

    // ── Quantize + round-trip the FINEST mesh (most vertices = most
    // exposure to a real quantization bug) ──────────────────────────
    double minx = 1e30, maxx = -1e30, minz = 1e30, maxz = -1e30;
    for (auto& v : mesh_finest.vertices) {
        minx = std::min(minx, (double)v.x); maxx = std::max(maxx, (double)v.x);
        minz = std::min(minz, (double)v.z); maxz = std::max(maxz, (double)v.z);
    }
    double box_center_x = (minx + maxx) * 0.5 * sample_spacing, box_center_z = (minz + maxz) * 0.5 * sample_spacing;
    double box_extent_x = (maxx - minx) * 0.5 * sample_spacing, box_extent_z = (maxz - minz) * 0.5 * sample_spacing;

    auto qverts = QuantizeMesh(hf, mesh_finest, 0, sample_spacing);
    WriteQuantFile(out_path, qverts, tris_finest);

    std::vector<QuantVert> qverts_read;
    std::vector<Tri> tris_read;
    if (!ReadQuantFile(out_path, qverts_read, tris_read)) { std::fprintf(stderr, "round-trip read failed\n"); return 1; }
    auto deq = DequantizeMesh(qverts_read, box_center_x, box_center_z, box_extent_x, box_extent_z);

    // Precision check: compare dequantized (x,z) against the true
    // sample_spacing-scaled original position. Theoretical bound is
    // box_extent / 2^14 per axis.
    double max_xz_err = 0, max_y_err = 0;
    for (size_t i = 0; i < deq.size(); ++i) {
        double true_x = mesh_finest.vertices[i].x * sample_spacing;
        double true_z = mesh_finest.vertices[i].z * sample_spacing;
        double true_y = mesh_finest.vertices[i].y;
        max_xz_err = std::max({max_xz_err, std::fabs(deq[i].x - true_x), std::fabs(deq[i].z - true_z)});
        max_y_err = std::max(max_y_err, std::fabs(deq[i].y - true_y));
    }
    double theoretical_xz_bound = std::max(box_extent_x, box_extent_z) / (1 << 14);
    std::printf("[chunklod_bake] quantization round-trip: max XZ error=%.6fm (theoretical bound %.6fm), max Y error=%.6fm (expected ~0.5m -- Y quantized to whole metres via round-to-nearest)\n",
                max_xz_err, theoretical_xz_bound, max_y_err);

    // Skirt sanity on the DEQUANTIZED mesh -- the actual Phase 3 gate:
    // does quantization break the skirt-below-surface invariant that
    // Phase 2 verified on raw floats?
    std::map<std::pair<int64_t, int64_t>, double> real_min_y;
    for (size_t i = 0; i < deq.size(); ++i) {
        if (!deq[i].special) {
            auto key = std::make_pair((int64_t)std::lround(deq[i].x * 1000), (int64_t)std::lround(deq[i].z * 1000));
            auto it = real_min_y.find(key);
            if (it == real_min_y.end() || deq[i].y < it->second) real_min_y[key] = deq[i].y;
        }
    }
    int violations = 0;
    for (size_t i = 0; i < deq.size(); ++i) {
        if (deq[i].special) {
            auto key = std::make_pair((int64_t)std::lround(deq[i].x * 1000), (int64_t)std::lround(deq[i].z * 1000));
            auto it = real_min_y.find(key);
            if (it != real_min_y.end() && deq[i].y > it->second) violations++;
        }
    }
    std::printf("[chunklod_bake] post-quantization skirt sanity: %d violations (must be 0)\n", violations);

    bool ok = tris_root.size() >= 2 && tris_finest.size() > tris_root.size() * 10 &&
              max_xz_err <= theoretical_xz_bound + 1e-6 && max_y_err <= 0.5 + 1e-6 && violations == 0;
    std::printf(ok ? "[chunklod_bake] PASS\n" : "[chunklod_bake] FAIL\n");
    return ok ? 0 : 1;
}
