#pragma once
#ifdef MONKEY_DUST_EDITOR

class EditorTerrainPanel {
public:
    static EditorTerrainPanel& Get() { static EditorTerrainPanel inst; return inst; }
    void Draw(float dt);

private:
    EditorTerrainPanel() = default;

    // Sculpt save status
    char  save_status_[64]  = {};
    float save_status_timer_= 0.f;

    // PCG Generate state
    bool  auto_rebuild_         = false;
    float rebuild_debounce_s_   = 0.f;
    int   preview_res_idx_      = 2;    // 0=64, 1=128, 2=256
};
#endif
