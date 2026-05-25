#pragma once
#ifdef MONKEY_DUST_EDITOR

class EditorAnimationPanel {
public:
    static EditorAnimationPanel& Get() { static EditorAnimationPanel inst; return inst; }
    void Draw();
    void DrawContent();
private:
    EditorAnimationPanel() = default;
};
#endif
