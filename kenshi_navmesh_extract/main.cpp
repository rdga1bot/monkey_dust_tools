// kenshi_navmesh_extract — pulls real per-zone walkable-surface geometry
// (vertices + face polygons) out of Kenshi's Havok AI navmesh tiles
// (game data/newland/land/navtiles/tile{X}.{Y}.hkt) and writes one Wavefront
// OBJ file per zone.
//
// Why: monkey_dust's terrain texture atlas (md_terrain.png, world_hmap.r32)
// tops out at a few metres per pixel. The REAL in-game navmesh — generated
// by Kenshi from its actual 3D terrain — carries genuine per-zone geometry
// at a much finer, non-uniform resolution (walkable-surface polygons, not a
// regular grid). This tool surfaces that geometry for inspection / future
// terrain-detail cross-referencing. See tools/third_party/hkxparse/
// README_MONKEYDUST.md for the full parser reverse-engineering writeup.
//
// Usage: kenshi_navmesh_extract <navtiles_dir> <out_dir>
//   Scans <navtiles_dir> for tile*.hkt, writes <out_dir>/zone_GX_GZ.obj for
//   each tile whose filename matches "tile<gx>.<gz>.hkt".
#include <hkxparse/HKXMapping.h>
#include <hkxparse/HKXTagfileParser.h>
#include <hkxparse/HKXTypes.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace hkxparse;

// monkey_dust: an earlier version tried to mute the vendored parser's
// leftover printf/fprintf tracing per-file via dup2()-swapping fd 1/2 --
// crashed (SIGSEGV inside printf() itself, confirmed via gdb) after a few
// dozen files, most likely glibc's buffered FILE* internal state getting
// corrupted by having its underlying fd repeatedly swapped out from under
// it thousands of times. Simplest robust fix: don't fight stdio internals
// -- run this tool with its OWN stdout redirected to /dev/null from the
// shell (`kenshi_navmesh_extract dir outdir >/dev/null`); our own
// progress/summary output goes to stderr exclusively, unaffected.

static void findNavMeshesInStruct(const HKXStruct &st, std::vector<const HKXStruct*> &found,
                                  std::set<const HKXStruct*> &visitedStructs,
                                  std::set<const HKXStruct*> &visitedRefs);

// monkey_dust: previous version wrapped `**ref` in a freshly-constructed
// local HKXVariant and recursed into it -- that local temporary is
// destroyed when the recursive call returns, leaving `found` holding
// dangling pointers into it (confirmed via gdb: crash inside
// unordered_map::find() on a `st` that pointed into a destroyed stack
// frame). Fixed by recursing directly on the HKXStruct itself (a real,
// stable object owned by the shared_ptr) -- no copy, no dangling pointer.
static void findNavMeshes(const HKXVariant &v, std::vector<const HKXStruct*> &found,
                          std::set<const HKXStruct*> &visitedStructs,
                          std::set<const HKXStruct*> &visitedRefs) {
    if (auto *st = std::get_if<HKXStruct>(&v)) {
        findNavMeshesInStruct(*st, found, visitedStructs, visitedRefs);
    } else if (auto *ref = std::get_if<HKXStructRef>(&v)) {
        if (*ref && visitedRefs.insert(ref->get()).second) {
            findNavMeshesInStruct(**ref, found, visitedStructs, visitedRefs);
        }
    } else if (auto *arr = std::get_if<HKXArray>(&v)) {
        for (auto &item : arr->values) findNavMeshes(item, found, visitedStructs, visitedRefs);
    }
}

static void findNavMeshesInStruct(const HKXStruct &st, std::vector<const HKXStruct*> &found,
                                  std::set<const HKXStruct*> &visitedStructs,
                                  std::set<const HKXStruct*> &visitedRefs) {
    if (!visitedStructs.insert(&st).second) return;
    for (auto &cn : st.classNames) {
        if (cn == "hkaiNavMesh") { found.push_back(&st); break; }
    }
    for (auto &field : st.fields) findNavMeshes(field.second, found, visitedStructs, visitedRefs);
}

static bool getUint(const HKXStruct &st, const char *field, uint64_t &out) {
    auto it = st.fields.find(field);
    if (it == st.fields.end()) return false;
    if (auto *val = std::get_if<uint64_t>(&it->second)) { out = *val; return true; }
    return false;
}

struct ExtractedMesh {
    std::vector<HKXVector4> vertices;
    std::vector<std::vector<int>> faces;  // vertex-index polygon loops
};

static bool extractNavMesh(const HKXStruct &navMesh, ExtractedMesh &out) {
    auto vertsIt = navMesh.fields.find("vertices");
    if (vertsIt == navMesh.fields.end()) return false;
    auto *vertsArr = std::get_if<HKXArray>(&vertsIt->second);
    if (!vertsArr) return false;
    for (auto &v : vertsArr->values) {
        if (auto *vec = std::get_if<HKXVector4>(&v)) out.vertices.push_back(*vec);
    }
    if (out.vertices.empty()) return false;

    std::vector<std::pair<int,int>> edgeAB;
    auto edgesIt = navMesh.fields.find("edges");
    if (edgesIt != navMesh.fields.end()) {
        if (auto *edgesArr = std::get_if<HKXArray>(&edgesIt->second)) {
            for (auto &e : edgesArr->values) {
                if (auto *est = std::get_if<HKXStruct>(&e)) {
                    uint64_t a = 0, b = 0;
                    getUint(*est, "a", a);
                    getUint(*est, "b", b);
                    edgeAB.emplace_back((int)a, (int)b);
                }
            }
        }
    }

    auto facesIt = navMesh.fields.find("faces");
    if (facesIt != navMesh.fields.end()) {
        if (auto *facesArr = std::get_if<HKXArray>(&facesIt->second)) {
            for (auto &f : facesArr->values) {
                if (auto *fst = std::get_if<HKXStruct>(&f)) {
                    uint64_t startEdge = 0, numEdges = 0;
                    getUint(*fst, "startEdgeIndex", startEdge);
                    getUint(*fst, "numEdges", numEdges);
                    std::vector<int> loop;
                    for (uint64_t i = 0; i < numEdges; ++i) {
                        size_t ei = (size_t)startEdge + i;
                        if (ei < edgeAB.size()) loop.push_back(edgeAB[ei].first);
                    }
                    if (loop.size() >= 3) out.faces.push_back(std::move(loop));
                }
            }
        }
    }
    return true;
}

static bool writeObj(const std::string &path, const std::vector<ExtractedMesh> &meshes) {
    std::ofstream out(path);
    if (!out) return false;
    out << "# kenshi_navmesh_extract -- real per-zone walkable-surface geometry\n";
    out << "# " << meshes.size() << " navmesh object(s) in this tile\n";
    size_t vertBase = 1;  // OBJ indices are 1-based
    for (auto &m : meshes) {
        out << "o navmesh\n";
        for (auto &v : m.vertices)
            out << "v " << v.x << " " << v.y << " " << v.z << "\n";
        for (auto &face : m.faces) {
            out << "f";
            for (int idx : face) out << " " << (vertBase + (size_t)idx);
            out << "\n";
        }
        vertBase += m.vertices.size();
    }
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        std::cerr << "usage: kenshi_navmesh_extract <navtiles_dir> <out_dir>\n";
        return 1;
    }
    fs::path inDir = argv[1];
    fs::path outDir = argv[2];
    fs::create_directories(outDir);

    int okCount = 0, failCount = 0, emptyCount = 0;
    std::vector<std::string> failedFiles;

    for (auto &entry : fs::directory_iterator(inDir)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        if (name.rfind("tile", 0) != 0 || entry.path().extension() != ".hkt") continue;

        int gx = -1, gz = -1;
        if (sscanf(name.c_str(), "tile%d.%d.hkt", &gx, &gz) != 2) continue;

        std::ifstream stream(entry.path(), std::ios::in | std::ios::binary);
        if (!stream) { failCount++; failedFiles.push_back(name); continue; }
        stream.seekg(0, std::ios::end);
        auto size = (size_t)stream.tellg();
        stream.seekg(0);
        HKXMapping mapping(size);
        stream.read(reinterpret_cast<char*>(mapping.data()), size);

        HKXStructRef root;
        bool parseOk = true;
        {
            try {
                HKXTagfileParser parser(mapping);
                root = parser.parse();
            } catch (const std::exception &e) {
                std::cerr << "[fail] " << name << ": " << e.what() << "\n";
                parseOk = false;
            } catch (...) {
                std::cerr << "[fail] " << name << ": unknown exception\n";
                parseOk = false;
            }
        }

        if (!parseOk || !root) { failCount++; failedFiles.push_back(name); continue; }

        std::vector<const HKXStruct*> navMeshes;
        std::set<const HKXStruct*> visitedStructs, visitedRefs;
        findNavMeshesInStruct(*root, navMeshes, visitedStructs, visitedRefs);

        std::vector<ExtractedMesh> meshes;
        for (auto *nm : navMeshes) {
            ExtractedMesh m;
            if (extractNavMesh(*nm, m) && !m.vertices.empty()) meshes.push_back(std::move(m));
        }

        if (meshes.empty()) { emptyCount++; continue; }

        char outName[128];
        snprintf(outName, sizeof(outName), "zone_%d_%d.obj", gx, gz);
        writeObj((outDir / outName).string(), meshes);
        okCount++;
    }

    std::cerr << "Extracted: " << okCount << "  Empty(no geometry): " << emptyCount
              << "  Failed: " << failCount << "\n";
    if (!failedFiles.empty()) {
        std::cerr << "Failed files:";
        for (size_t i = 0; i < failedFiles.size() && i < 20; ++i) std::cerr << " " << failedFiles[i];
        if (failedFiles.size() > 20) std::cerr << " ... (+" << (failedFiles.size()-20) << " more)";
        std::cerr << "\n";
    }
    return 0;
}
