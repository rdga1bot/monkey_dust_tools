#pragma once
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

// Per-panel detach + window geometry, persisted to JSON.
// JSON: { "items":{det,x,y,w,h}, ..., "settings":{...} }

namespace EditorLayout {

struct Panel { bool detached; ImVec2 pos; ImVec2 size; };

struct Layout {
    Panel items     = {false, {120.f,  70.f}, {680.f, 520.f}};
    Panel factions  = {false, {140.f,  80.f}, {640.f, 500.f}};
    Panel map       = {false, {80.f,  100.f}, {800.f, 600.f}};
    Panel world     = {false, {80.f,  100.f}, {800.f, 600.f}};
    Panel terrain   = {false, {80.f,  100.f}, {900.f, 600.f}};
    Panel world3d   = {false, {80.f,  100.f}, {1000.f,700.f}};
    Panel npcs      = {false, {80.f,   60.f}, {660.f, 540.f}};
    Panel chars     = {false, {160.f,  60.f}, {900.f, 640.f}};
    Panel settings  = {false, {620.f, 110.f}, {560.f, 440.f}};
};

static bool el_getf(const char* buf, const char* key, float& out) {
    const char* p = strstr(buf, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == ':' || *p == ' ') p++;
    out = (float)atof(p);
    return true;
}

inline bool Load(const char* path, Layout& lay) {
    FILE* f = fopen(path, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    static char buf[8192];
    if (sz >= (long)sizeof(buf)) { fclose(f); return false; }
    (void)fread(buf, 1, (size_t)sz, f); buf[sz] = '\0'; fclose(f);

    auto load_one = [&](const char* name, Panel& ps) {
        char key[64]; snprintf(key, sizeof(key), "\"%s\"", name);
        const char* p = strstr(buf, key); if (!p) return;
        p += strlen(key);
        while (*p == ':' || *p == ' ') p++;
        if (*p != '{') return;
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

    load_one("items",     lay.items);
    load_one("factions",  lay.factions);
    load_one("map",       lay.map);
    load_one("world",     lay.world);
    load_one("terrain",   lay.terrain);
    load_one("world3d",   lay.world3d);
    load_one("npcs",      lay.npcs);
    load_one("chars",     lay.chars);
    load_one("settings",  lay.settings);
    return true;
}

inline bool Save(const char* path, const Layout& lay) {
    FILE* f = fopen(path, "w"); if (!f) return false;
    auto wr = [&](const char* name, const Panel& ps, bool last) {
        fprintf(f, "  \"%s\":{\"det\":%d,\"x\":%.0f,\"y\":%.0f,\"w\":%.0f,\"h\":%.0f}%s\n",
            name, ps.detached ? 1 : 0,
            ps.pos.x, ps.pos.y, ps.size.x, ps.size.y,
            last ? "" : ",");
    };
    fprintf(f, "{\n");
    wr("items",     lay.items,     false);
    wr("factions",  lay.factions,  false);
    wr("map",       lay.map,       false);
    wr("world",     lay.world,     false);
    wr("terrain",   lay.terrain,   false);
    wr("world3d",   lay.world3d,   false);
    wr("npcs",      lay.npcs,      false);
    wr("chars",     lay.chars,     false);
    wr("settings",  lay.settings,  true);
    fprintf(f, "}\n");
    fclose(f); return true;
}

} // namespace EditorLayout
