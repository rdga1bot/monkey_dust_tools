#pragma once
#ifdef MONKEY_DUST_EDITOR

class EditorGraphicsPanel {
public:
    static EditorGraphicsPanel& Get() { static EditorGraphicsPanel inst; return inst; }
    void Draw();
private:
    EditorGraphicsPanel() = default;
};
#endif
