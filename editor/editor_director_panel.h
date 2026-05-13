#pragma once
#ifdef MONKEY_DUST_EDITOR

class EditorDirectorPanel {
public:
    static EditorDirectorPanel& Get() { static EditorDirectorPanel i; return i; }
    void Draw();
private:
    EditorDirectorPanel() = default;
    float override_menace_ = -1.0f;  // negative = no override
};

#endif
