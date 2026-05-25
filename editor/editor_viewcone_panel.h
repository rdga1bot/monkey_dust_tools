#pragma once
#ifdef MONKEY_DUST_EDITOR

class EditorViewConePanel {
public:
    static EditorViewConePanel& Get() { static EditorViewConePanel i; return i; }
    void Draw();
    void DrawContent();
private:
    EditorViewConePanel() = default;
};

#endif
