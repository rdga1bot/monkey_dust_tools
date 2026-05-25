#pragma once
#ifdef MONKEY_DUST_EDITOR

#include "imgui.h"
#include "imgui_node_editor.h"
namespace ed = ax::NodeEditor;

#include <monkey_dust/world/terrain_chunk.h>
#include <monkey_dust/nodegraph/pcg_graph.h>
#include <monkey_dust/nodegraph/pcg_nodes_base.h>
#include <monkey_dust/nodegraph/terrain_tile_gen.h>

// Terrain node graph panel — panel index 12.
// Library: thedmd/imgui-node-editor (MIT).
// PcgGraph handles topological sort, dirty propagation, and type-safe pins.
// TerrainTileGen handles worker-thread rebuild and PollApply().
class EditorNodeGraphPanel {
public:
    static EditorNodeGraphPanel& Get() {
        static EditorNodeGraphPanel i; return i;
    }

    void Init();
    void Shutdown();
    void Draw();
    void DrawContent();

    // ── Node types ─────────────────────────────────────────────────────────
    enum class NK : uint8_t {
        NoiseFBM=0, DomainWarp, Ridge, Biome, TerrainOutput,
        Add, Multiply, Clamp, Slope, Scatter
    };
    static constexpr int NK_COUNT = 10;

    // Legacy accessor — builds TerrainGenParams from Output node params.
    TerrainGenParams EvalGraph() const;

    // PcgGraph accessor for EditorTerrainPanel
    md::PcgGraph& GetPcgGraph() { return pcg_graph_; }

private:
    EditorNodeGraphPanel() = default;

    struct Pin {
        int              id;
        ed::PinKind      kind;
        md::PcgDataType  data_type;
        char             label[20];
        int              node_id;
    };

    struct Node {
        int    id;
        NK     kind;
        char   label[32];
        int    first_in,  in_cnt;
        int    first_out, out_cnt;
        int    pcg_slot = -1;   // index in pcg_graph_.nodes[]

        // Per-kind parameters
        float  base_scale    = 0.008f;
        float  amplitude     = 40.f;
        int    octaves       = 6;
        float  persistence   = 0.50f;
        float  lacunarity    = 2.00f;
        float  warp_strength = 30.f;
        float  ridge_weight  = 0.8f;
        float  redist_power  = 1.6f;
        float  biome_scale   = 0.00025f;
        int    num_factions  = 6;
        int    seed          = 42;
        float  weight_a      = 1.f;
        float  weight_b      = 1.f;
        float  clamp_lo      = 0.f;
        float  clamp_hi      = 100.f;
        float  density       = 0.02f;
        float  scatter_radius = 4.f;
        uint16_t prefab_id   = 0;
        bool   alive = false;
    };

    struct Link {
        int  id, from_pin, to_pin;
        int  from_node_idx, to_node_idx;
        int  from_pin_local, to_pin_local;  // 0-based pin index within node
        bool alive = false;
    };

    static constexpr int MAX_N = 32;
    static constexpr int MAX_P = 128;
    static constexpr int MAX_L = 64;

    Node  nodes_[MAX_N] = {};
    Pin   pins_ [MAX_P] = {};
    Link  links_[MAX_L] = {};
    int   node_cnt_ = 0;
    int   pin_cnt_  = 0;
    int   link_cnt_ = 0;
    int   next_id_  = 1;

    ed::EditorContext* ctx_ = nullptr;

    // PcgGraph: engine-side DAG with dirty propagation and type-safe execution
    md::PcgGraph pcg_graph_;

    // Concrete node storage (parallel to nodes_[] by pcg_slot)
    md::PcgNoiseNode      pcg_noise_  [MAX_N] = {};
    md::PcgDomainWarpNode pcg_warp_   [MAX_N] = {};
    md::PcgRidgeNode      pcg_ridge_  [MAX_N] = {};
    md::PcgBiomeNode      pcg_biome_  [MAX_N] = {};
    md::PcgOutputNode     pcg_output_ [MAX_N] = {};
    md::PcgAddNode        pcg_add_    [MAX_N] = {};
    md::PcgMultiplyNode   pcg_mul_    [MAX_N] = {};
    md::PcgClampNode      pcg_clamp_  [MAX_N] = {};
    md::PcgSlopeNode      pcg_slope_  [MAX_N] = {};
    md::PcgScatterNode    pcg_scatter_[MAX_N] = {};
    // Per-kind instance counter for index into above arrays
    int cnt_noise_=0, cnt_warp_=0, cnt_ridge_=0, cnt_biome_=0, cnt_out_=0;
    int cnt_add_=0, cnt_mul_=0, cnt_clamp_=0, cnt_slope_=0, cnt_scatter_=0;

    int  MakePin(int node_id, ed::PinKind kind, md::PcgDataType dt, const char* label);
    int  SpawnNode(NK kind);
    md::PcgNode* GetPcgNode(NK kind);       // returns concrete node ptr for SpawnNode
    void SyncParamsToEngine(int node_idx);  // copy UI params → PcgNode fields + MarkDirty
    void DrawNode(Node& n);
    void DrawPinDot(md::PcgDataType dt, bool is_output);
    void HandleCreateDelete();
    void HandleContextMenu();
    void BuildDefaultGraph();
};

#endif // MONKEY_DUST_EDITOR
