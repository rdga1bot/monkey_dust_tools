#pragma once
#ifdef MONKEY_DUST_EDITOR

#include "imgui.h"
#include "imgui_node_editor.h"
namespace ed = ax::NodeEditor;

// Blueprint-style material / logic node graph.
// Library: thedmd/imgui-node-editor (MIT).  Panel index 12.
// Provides: zoom+pan, Bezier links, delete key, right-click context menu.
class EditorNodeGraphPanel {
public:
    static EditorNodeGraphPanel& Get() {
        static EditorNodeGraphPanel i; return i;
    }

    void Init();      // creates ed::EditorContext, called from EditorCore::Init
    void Shutdown();  // destroys context, called from EditorCore::Shutdown
    void Draw();

    // ── Node types (public: used in kMeta table in .cpp) ───────────────────
    enum class NK : uint8_t {
        TexSample=0, Multiply, Add, Lerp, ConstFloat, ConstColor, MatOutput
    };
    static constexpr int NK_COUNT = 7;

private:
    EditorNodeGraphPanel() = default;

    struct Pin {
        int         id;
        ed::PinKind kind;
        char        label[20];
        int         node_id;
    };

    struct Node {
        int    id;
        NK     kind;
        char   label[32];
        int    first_in,  in_cnt;
        int    first_out, out_cnt;
        float  float_val  = 0.f;
        ImVec4 color_val  = {1,1,1,1};
        char   tex_path[64] = {};
        bool   alive = false;
    };

    struct Link {
        int  id, from_pin, to_pin;
        bool alive = false;
    };

    static constexpr int MAX_N = 32;
    static constexpr int MAX_P = 128;
    static constexpr int MAX_L = 64;

    Node  nodes_[MAX_N] = {};
    Pin   pins_[MAX_P]  = {};
    Link  links_[MAX_L] = {};
    int   node_cnt_ = 0;
    int   pin_cnt_  = 0;
    int   link_cnt_ = 0;
    int   next_id_  = 1;

    ed::EditorContext* ctx_ = nullptr;

    int  MakePin(int node_id, ed::PinKind kind, const char* label);
    int  SpawnNode(NK kind);
    void DrawNode(Node& n);
    void HandleCreateDelete();
    void HandleContextMenu();
    void BuildDefaultGraph();
};

#endif // MONKEY_DUST_EDITOR
