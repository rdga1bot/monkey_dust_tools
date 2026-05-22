#pragma once
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

// Saves/loads per-panel detach state + window position/size.
// Format: { "items":{...}, "factions":{...}, "npcs":{...}, "chars":{...}, "settings":{...} }

namespace EditorLayout {

struct Panel { bool detached; ImVec2 pos; ImVec2 size; };

static bool el_getf(const char* buf, const char* key, float& out) {
    const char* p = strstr(buf, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == ':' || *p == ' ') p++;
    out = (float)atof(p);
    return true;
}

inline bool Load(const char* path,
    Panel& items, Panel& factions, Panel& npcs, Panel& chars, Panel& settings)
{
    FILE* f = fopen(path, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    static char buf[4096];
    if (sz >= (long)sizeof(buf)) { fclose(f); return false; }
    (void)fread(buf, 1, (size_t)sz, f); buf[sz] = '\0'; fclose(f);

    auto load_one = [&](const char* name, Panel& ps) {
        char key[64]; snprintf(key, sizeof(key), "\"%s\"", name);
        const char* p = strstr(buf, key); if (!p) return;
        p += strlen(key);
        while (*p == ':' || *p == ' ') p++;
        if (*p != '{') return;
        // extract content between braces
        char sec[512]; int len = 0;
        const char* q = p + 1;
        while (*q && *q != '}' && len < 511) sec[len++] = *q++;
        sec[len] = '\0';
        float v;
        if (el_getf(sec, "\"det\"", v)) ps.detached = (v != 0.f);
        if (el_getf(sec, "\"x\"",   v)) ps.pos.x    = v;
        if (el_getf(sec, "\"y\"",   v)) ps.pos.y    = v;
        if (el_getf(sec, "\"w\"",   v) && v > 0.f) ps.size.x = v;
        if (el_getf(sec, "\"h\"",   v) && v > 0.f) ps.size.y = v;
    };

    load_one("items",    items);
    load_one("factions", factions);
    load_one("npcs",     npcs);
    load_one("chars",    chars);
    load_one("settings", settings);
    return true;
}

inline bool Save(const char* path,
    const Panel& items, const Panel& factions, const Panel& npcs,
    const Panel& chars, const Panel& settings)
{
    FILE* f = fopen(path, "w"); if (!f) return false;
    auto wr = [&](const char* name, const Panel& ps, bool last) {
        fprintf(f, "  \"%s\":{\"det\":%d,\"x\":%.0f,\"y\":%.0f,\"w\":%.0f,\"h\":%.0f}%s\n",
            name, ps.detached ? 1 : 0,
            ps.pos.x, ps.pos.y, ps.size.x, ps.size.y,
            last ? "" : ",");
    };
    fprintf(f, "{\n");
    wr("items",    items,    false);
    wr("factions", factions, false);
    wr("npcs",     npcs,     false);
    wr("chars",    chars,    false);
    wr("settings", settings, true);
    fprintf(f, "}\n");
    fclose(f); return true;
}

} // namespace EditorLayout
