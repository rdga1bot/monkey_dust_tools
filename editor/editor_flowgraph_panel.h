#pragma once
#ifdef MONKEY_DUST_EDITOR

class EditorFlowGraphPanel {
public:
    static EditorFlowGraphPanel& Get() { static EditorFlowGraphPanel i; return i; }
    void Draw();
private:
    EditorFlowGraphPanel() = default;
    char fire_node_name_[64] = {};
};

#endif
