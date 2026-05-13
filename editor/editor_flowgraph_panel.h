#pragma once
#ifdef MONKEY_DUST_EDITOR

#include "imnodes.h"

class EditorFlowGraphPanel {
public:
    static EditorFlowGraphPanel& Get() { static EditorFlowGraphPanel i; return i; }
    void Draw();
    void Shutdown();

private:
    EditorFlowGraphPanel() = default;
    char            fire_node_name_[64] = {};
    ImNodesContext* imnodes_ctx_        = nullptr;
};

#endif
