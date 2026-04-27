#pragma once
#ifdef MONKEY_DUST_EDITOR

// Phase 39: browse FLARE item/enemy/power data loaded from data/flare/*.json.
// Uses panels_visible[7]. Lazy-loaded on first open.

class EditorFlareBrowser {
public:
    static EditorFlareBrowser& Get() { static EditorFlareBrowser inst; return inst; }
    void Draw();

private:
    EditorFlareBrowser() = default;

    struct ItemEntry {
        int  id, level, price, dmg_min, dmg_max;
        char name[48];
        char dmg_type[8];
    };
    struct EnemyEntry {
        int   level, hp, dmg_min, dmg_max;
        float speed;
        char  category[32];
        char  name[48];
    };

    static constexpr int MAX_ITEMS   = 1024;
    static constexpr int MAX_ENEMIES = 256;

    ItemEntry  items_[MAX_ITEMS]     = {};
    EnemyEntry enemies_[MAX_ENEMIES] = {};
    int  items_cnt_   = 0;
    int  enemies_cnt_ = 0;
    bool loaded_      = false;

    int  sel_[3]      = {-1, -1, -1};   // selected row per tab
    char filter_[64]  = {};

    void Load();
    void DrawItems();
    void DrawEnemies();
    void DrawPowers();

    static bool Match(const char* text, const char* f);
};
#endif
