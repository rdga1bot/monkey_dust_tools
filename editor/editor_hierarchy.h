#pragma once
#ifdef MONKEY_DUST_EDITOR
#include "imgui.h"
#include "editor_core.h"
#include <entt/entt.hpp>

class EditorHierarchy {
public:
    static EditorHierarchy& Get() { static EditorHierarchy inst; return inst; }

    void Draw();

private:
    EditorHierarchy() = default;

    static constexpr int MAX_CACHE = 2048;
    entt::entity entity_cache_[MAX_CACHE] = {};
    int          entity_cache_count_      = 0;
    int          cache_refresh_counter_   = 0;
    char         entity_filter_[64]       = {};

    entt::entity ctx_entity_ = entt::null; // right-click target

    void RefreshCache();
    void DrawContextMenu(entt::entity e);
    const char* EntityIcon(entt::entity e) const;
    void        EntityLabel(entt::entity e, char* buf, int len) const;
    ImVec4      EntityColor(entt::entity e) const;
};
#endif
