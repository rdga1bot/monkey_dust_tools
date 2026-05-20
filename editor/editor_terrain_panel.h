#pragma once
#ifdef MONKEY_DUST_EDITOR

class EditorTerrainPanel {
public:
    static EditorTerrainPanel& Get() { static EditorTerrainPanel inst; return inst; }
    void Draw(float dt);
private:
    EditorTerrainPanel() = default;
    char save_status_[64] = {};
    float save_status_timer_ = 0.f;
};
#endif
