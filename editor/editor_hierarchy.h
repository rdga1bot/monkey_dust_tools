#pragma once
#include <monkey_dust/ecs/md_entity.h>
#ifdef MONKEY_DUST_EDITOR
#include "imgui.h"
#include "editor_core.h"
#include <entt/entt.hpp>

class EditorHierarchy {
public:
    static EditorHierarchy& Get() { static EditorHierarchy inst; return inst; }

    void Draw();
    void DrawContent();

private:
    EditorHierarchy() = default;

    static constexpr int MAX_CACHE = 2048;
    MdEntity entity_cache_[MAX_CACHE] = {};
    int          entity_cache_count_      = 0;
    int          cache_refresh_counter_   = 0;
    char         entity_filter_[64]       = {};

    MdEntity ctx_entity_ = entt::null; // right-click target

    void RefreshCache();
    void DrawContextMenu(MdEntity e);
    const char* EntityIcon(MdEntity e) const;
    void        EntityLabel(MdEntity e, char* buf, int len) const;
    ImVec4      EntityColor(MdEntity e) const;
};
#endif
