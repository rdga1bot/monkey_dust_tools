#pragma once
#include "editor_ui.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

// ─────────────────────────────────────────────────────────
// ItemEditor — CRUD-редагування data/items/items.json
//
// Формат:
//   { "items": [ { "id":N, "name":"...", "weight":N.N } ] }
// ─────────────────────────────────────────────────────────

struct ItemEntry {
    uint32_t id;
    char     name[32];
    float    weight;
};

namespace ItemEditor {

static ItemEntry g_items[32];
static int       g_count = 0;
static int       g_sel   = -1;

// Edit buffers (string representations для TextBox)
static char g_buf_name  [32] = {};
static char g_buf_weight[16] = {};

// ── Мінімальний JSON-парсер ───────────────────────────────

static const char* ie_ws(const char* p) {
    while (*p && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p;
    return p;
}
static const char* ie_str(const char* p, char* buf, int maxlen) {
    if (*p == '"') ++p;
    int i = 0;
    while (*p && *p != '"' && i < maxlen-1) buf[i++] = *p++;
    buf[i] = '\0';
    if (*p == '"') ++p;
    return p;
}
static const char* ie_skip(const char* p, char open, char close) {
    if (*p != open) return p;
    int d = 0;
    while (*p) {
        if (*p == '"') {
            ++p;
            while (*p && *p != '"') { if (*p=='\\' && *(p+1)) ++p; ++p; }
            if (*p) ++p;
            continue;
        }
        if (*p == open) ++d; else if (*p == close) { if (--d == 0) return p+1; }
        ++p;
    }
    return p;
}

// ── Load ──────────────────────────────────────────────────
inline bool Load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[ItemEditor] Cannot open %s\n", path); return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    static char buf[8192];
    if (sz >= (long)sizeof(buf)) { fclose(f); return false; }
    (void)fread(buf, 1, (size_t)sz, f); buf[sz] = '\0'; fclose(f);

    g_count = 0; g_sel = -1;
    const char* p = strstr(buf, "\"items\"");
    if (!p) return false;
    p = strchr(p, '['); if (!p) return false; ++p;

    while (g_count < 32) {
        p = ie_ws(p);
        if (*p == ']' || *p == '\0') break;
        if (*p != '{') { ++p; continue; }
        const char* st = p;
        const char* en = ie_skip(p, '{', '}');

        ItemEntry& it = g_items[g_count];
        memset(&it, 0, sizeof(it));

        const char* v;
        v = strstr(st, "\"id\"");
        if (v && v < en) { v = strchr(v, ':'); if (v) it.id = (uint32_t)strtol(v+1, nullptr, 10); }
        v = strstr(st, "\"name\"");
        if (v && v < en) { v = strchr(v, ':'); if (v) { v=ie_ws(v+1); if(*v=='"') ie_str(v, it.name, 32); } }
        v = strstr(st, "\"weight\"");
        if (v && v < en) { v = strchr(v, ':'); if (v) it.weight = (float)atof(v+1); }

        g_count++;
        p = ie_ws(en); if (*p == ',') ++p;
    }
    fprintf(stdout, "[ItemEditor] Loaded %d items from %s\n", g_count, path);
    return true;
}

// ── Save ──────────────────────────────────────────────────
inline bool Save(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[ItemEditor] Cannot write %s\n", path); return false; }
    fprintf(f, "{\n  \"items\": [\n");
    for (int i = 0; i < g_count; ++i) {
        fprintf(f, "    { \"id\": %u, \"name\": \"%-12s\", \"weight\": %.1f }%s\n",
                g_items[i].id, g_items[i].name, g_items[i].weight,
                (i < g_count-1) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    fprintf(stdout, "[ItemEditor] Saved %d items to %s\n", g_count, path);
    return true;
}

// ── Sync buffers ↔ selected item ──────────────────────────
static void ApplyEdit() {
    if (g_sel < 0 || g_sel >= g_count) return;
    strncpy(g_items[g_sel].name, g_buf_name, 31);
    g_items[g_sel].name[31] = '\0';
    // Trim trailing spaces
    for (int i = 30; i >= 0 && g_items[g_sel].name[i] == ' '; --i)
        g_items[g_sel].name[i] = '\0';
    g_items[g_sel].weight = (float)atof(g_buf_weight);
}
static void FillEdit(int idx) {
    if (idx < 0 || idx >= g_count) { g_buf_name[0] = '\0'; g_buf_weight[0] = '\0'; return; }
    strncpy(g_buf_name, g_items[idx].name, 31); g_buf_name[31] = '\0';
    snprintf(g_buf_weight, sizeof(g_buf_weight), "%.2f", g_items[idx].weight);
}

// ── Draw ──────────────────────────────────────────────────
// list_r — ліва панель, edit_r — права панель.
// Повертає true якщо була успішна операція Save.
inline bool Draw(Rectangle list_r, Rectangle edit_r, const char* path) {
    using namespace EditorUI;

    // ── List panel ────────────────────────────────────────
    Panel(list_r);
    Label((int)list_r.x + 8, (int)list_r.y + 8, "Items", 14, {170, 195, 255, 255});
    HSep((int)list_r.x, (int)list_r.y + 28, (int)list_r.width);

    static constexpr int ROW_H = 28;
    int max_vis = (int)((list_r.height - 36) / ROW_H);
    for (int i = 0; i < g_count && i < max_vis; ++i) {
        Rectangle rr = { list_r.x, list_r.y + 32 + (float)(i * ROW_H),
                         list_r.width, (float)ROW_H };
        bool sel = (i == g_sel);
        bool hov = MouseIn(rr);
        if (sel)       DrawRectangleRec(rr, {48, 68, 120, 255});
        else if (hov)  DrawRectangleRec(rr, {30, 35, 56, 255});

        char rb[48];
        snprintf(rb, sizeof(rb), "%u: %-12s  %.1f kg",
                 g_items[i].id, g_items[i].name, g_items[i].weight);
        DrawText(rb, (int)rr.x + 8, (int)rr.y + 7, 12, sel ? WHITE : Color{195, 200, 215, 255});

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !sel) {
            ApplyEdit();
            g_sel = i;
            FillEdit(i);
            g_active_id = -1;
        }
    }

    // ── Edit panel ────────────────────────────────────────
    Panel(edit_r);
    Label((int)edit_r.x + 12, (int)edit_r.y + 10, "Edit Item", 14, {170, 195, 255, 255});
    HSep((int)edit_r.x, (int)edit_r.y + 30, (int)edit_r.width);

    bool saved = false;

    if (g_sel < 0) {
        Label((int)edit_r.x + 14, (int)edit_r.y + 52,
              "← Select an item from the list", 13, {110, 115, 140, 255});
        return false;
    }

    int ex = (int)edit_r.x + 14;
    int ey = (int)edit_r.y + 42;

    // id (read-only)
    char id_buf[16]; snprintf(id_buf, sizeof(id_buf), "%u", g_items[g_sel].id);
    Label(ex, ey, "id :", 13);
    Label(ex + 40, ey, id_buf, 13, YELLOW);
    ey += 32;

    // name
    Label(ex, ey, "name :", 13);
    TextBox(1001, {(float)(ex + 70), (float)ey, 200, 26}, g_buf_name, 32);
    ey += 38;

    // weight
    Label(ex, ey, "weight :", 13);
    TextBox(1002, {(float)(ex + 70), (float)ey, 90, 26}, g_buf_weight, 16);
    Label(ex + 170, ey + 6, "kg", 12, {130, 135, 155, 255});
    ey += 50;

    HSep(ex, ey, (int)edit_r.width - 28); ey += 16;

    // Buttons row
    if (Button({(float)ex, (float)ey, 80, 28}, "Add")) {
        ApplyEdit();
        if (g_count < 32) {
            ItemEntry& ne = g_items[g_count];
            memset(&ne, 0, sizeof(ne));
            // Next id = max existing + 1
            uint32_t next_id = 0;
            for (int i = 0; i < g_count; ++i) if (g_items[i].id > next_id) next_id = g_items[i].id;
            ne.id = next_id + 1;
            strncpy(ne.name, "new_item", 31);
            ne.weight = 1.0f;
            g_sel = g_count++;
            FillEdit(g_sel);
            g_active_id = 1001; // фокус на ім'я
        }
    }
    if (Button({(float)(ex + 90), (float)ey, 80, 28}, "Delete", {150, 50, 50, 255})) {
        if (g_count > 1) {
            for (int i = g_sel; i < g_count - 1; ++i) g_items[i] = g_items[i+1];
            --g_count;
            g_sel = (g_sel < g_count) ? g_sel : g_count - 1;
            FillEdit(g_sel);
            g_active_id = -1;
        }
    }
    if (Button({(float)(ex + 190), (float)ey, 110, 28}, "Save File", {36, 110, 55, 255})) {
        ApplyEdit();
        saved = Save(path);
    }

    return saved;
}

} // namespace ItemEditor
