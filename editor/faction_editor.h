#pragma once
#include "editor_ui.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

// ─────────────────────────────────────────────────────────
// FactionEditor — редагування data/factions/factions.json
//
// Редагуємо: name, default_relation, матрицю relations.
// ─────────────────────────────────────────────────────────

struct FactionEntry {
    uint32_t id;
    char     name[32];
    int      default_rel;
    int      rel_to[8];
    int      rel_count;
};

namespace FactionEditor {

static FactionEntry g_factions[16];
static int          g_count = 0;
static int          g_sel   = -1;

static char g_buf_name  [32]   = {};
static char g_buf_defrel[8]    = {};
static char g_buf_rels  [8][8] = {};

// ── Мінімальний JSON-парсер ───────────────────────────────

static const char* fe_ws(const char* p) {
    while (*p && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p; return p;
}
static const char* fe_str(const char* p, char* buf, int maxlen) {
    if (*p == '"') ++p; int i = 0;
    while (*p && *p != '"' && i < maxlen-1) buf[i++] = *p++;
    buf[i] = '\0'; if (*p == '"') ++p; return p;
}
static const char* fe_skip(const char* p, char open, char close) {
    if (*p != open) return p; int d = 0;
    while (*p) {
        if (*p == '"') { ++p; while(*p&&*p!='"'){if(*p=='\\'&&*(p+1))++p;++p;} if(*p)++p; continue; }
        if (*p==open) ++d; else if (*p==close) { if(--d==0) return p+1; }
        ++p;
    }
    return p;
}

// ── Load ──────────────────────────────────────────────────
inline bool Load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[FactionEditor] Cannot open %s\n", path); return false; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    static char buf[16384];
    if (sz >= (long)sizeof(buf)) { fclose(f); return false; }
    (void)fread(buf, 1, (size_t)sz, f); buf[sz] = '\0'; fclose(f);

    g_count = 0; g_sel = -1;
    const char* p = strstr(buf, "\"factions\"");
    if (!p) return false;
    p = strchr(p, '['); if (!p) return false; ++p;

    while (g_count < 16) {
        p = fe_ws(p);
        if (*p == ']' || *p == '\0') break;
        if (*p != '{') { ++p; continue; }
        const char* st = p;
        const char* en = fe_skip(p, '{', '}');

        FactionEntry& fe = g_factions[g_count];
        memset(&fe, 0, sizeof(fe));

        const char* v;
        v = strstr(st, "\"id\"");
        if (v&&v<en) { v=strchr(v,':'); if(v) fe.id=(uint32_t)strtol(v+1,nullptr,10); }
        v = strstr(st, "\"name\"");
        if (v&&v<en) { v=strchr(v,':'); if(v) { v=fe_ws(v+1); if(*v=='"') fe_str(v,fe.name,32); } }
        v = strstr(st, "\"default_relation\"");
        if (v&&v<en) { v=strchr(v,':'); if(v) fe.default_rel=(int)strtol(v+1,nullptr,10); }

        const char* ra = strstr(st, "\"relations\"");
        if (ra && ra < en) {
            ra = strchr(ra, '['); if (ra && ra < en) { ++ra;
                while (fe.rel_count < 8) {
                    ra = fe_ws(ra);
                    if (*ra == ']' || *ra == '\0' || ra >= en) break;
                    if (*ra != '{') { ++ra; continue; }
                    const char* os = ra;
                    const char* oe = fe_skip(ra, '{', '}');
                    int to_id = 0, val = 0;
                    const char* tv = strstr(os, "\"to\"");
                    if (tv&&tv<oe) { tv=strchr(tv,':'); if(tv) to_id=(int)strtol(tv+1,nullptr,10); }
                    const char* vv = strstr(os, "\"value\"");
                    if (vv&&vv<oe) { vv=strchr(vv,':'); if(vv) val=(int)strtol(vv+1,nullptr,10); }
                    if (to_id >= 1 && to_id <= 8) fe.rel_to[to_id-1] = val;
                    fe.rel_count++;
                    ra = fe_ws(oe); if (*ra == ',') ++ra;
                }
            }
        }

        g_count++;
        p = fe_ws(en); if (*p == ',') ++p;
    }
    fprintf(stdout, "[FactionEditor] Loaded %d factions from %s\n", g_count, path);
    return true;
}

// ── Save ──────────────────────────────────────────────────
inline bool Save(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[FactionEditor] Cannot write %s\n", path); return false; }
    fprintf(f, "{\n  \"factions\": [\n");
    for (int i = 0; i < g_count; ++i) {
        FactionEntry& fe = g_factions[i];
        fprintf(f, "    {\n");
        fprintf(f, "      \"id\": %u,\n", fe.id);
        fprintf(f, "      \"name\": \"%s\",\n", fe.name);
        fprintf(f, "      \"default_relation\": %d,\n", fe.default_rel);
        fprintf(f, "      \"relations\": [\n");
        for (int j = 0; j < g_count; ++j) {
            fprintf(f, "        { \"to\": %d, \"value\": %5d }%s\n",
                    j + 1, fe.rel_to[j], (j < g_count-1) ? "," : "");
        }
        fprintf(f, "      ]\n");
        fprintf(f, "    }%s\n", (i < g_count-1) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    fprintf(stdout, "[FactionEditor] Saved %d factions to %s\n", g_count, path);
    return true;
}

// ── Sync buffers ↔ selected faction ───────────────────────
static void ApplyEdit() {
    if (g_sel < 0 || g_sel >= g_count) return;
    strncpy(g_factions[g_sel].name, g_buf_name, 31);
    g_factions[g_sel].name[31] = '\0';
    g_factions[g_sel].default_rel = (int)strtol(g_buf_defrel, nullptr, 10);
    for (int j = 0; j < g_count && j < 8; ++j)
        g_factions[g_sel].rel_to[j] = (int)strtol(g_buf_rels[j], nullptr, 10);
}
static void FillEdit(int idx) {
    if (idx < 0 || idx >= g_count) { g_buf_name[0]='\0'; g_buf_defrel[0]='\0'; return; }
    strncpy(g_buf_name, g_factions[idx].name, 31); g_buf_name[31] = '\0';
    snprintf(g_buf_defrel, sizeof(g_buf_defrel), "%d", g_factions[idx].default_rel);
    for (int j = 0; j < g_count && j < 8; ++j)
        snprintf(g_buf_rels[j], sizeof(g_buf_rels[j]), "%d", g_factions[idx].rel_to[j]);
}

// ── Draw ──────────────────────────────────────────────────
inline bool Draw(Rectangle list_r, Rectangle edit_r, const char* path) {
    using namespace EditorUI;

    // ── List panel ────────────────────────────────────────
    Panel(list_r);
    Label((int)list_r.x + S(8), (int)list_r.y + S(8), "Factions", 14,
          {170, 195, 255, 255});
    HSep((int)list_r.x, (int)list_r.y + S(28), (int)list_r.width);

    int row_h = S(30);
    for (int i = 0; i < g_count; ++i) {
        Rectangle rr = { list_r.x, list_r.y + S(32) + (float)(i * row_h),
                         list_r.width, (float)row_h };
        bool sel = (i == g_sel);
        bool hov = MouseIn(rr);
        if (sel)      DrawRectangleRec(rr, {48, 68, 120, 255});
        else if (hov) DrawRectangleRec(rr, {30, 35, 56, 255});

        char rb[40]; snprintf(rb, sizeof(rb), "%u: %s",
                              g_factions[i].id, g_factions[i].name);
        UiText(rb, (int)rr.x + S(8), (int)rr.y + S(8), 13,
               sel ? WHITE : Color{195, 200, 215, 255});

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !sel) {
            ApplyEdit(); g_sel = i; FillEdit(i); g_active_id = -1;
        }
    }

    // ── Edit panel ────────────────────────────────────────
    Panel(edit_r);
    Label((int)edit_r.x + S(12), (int)edit_r.y + S(10), "Edit Faction", 14,
          {170, 195, 255, 255});
    HSep((int)edit_r.x, (int)edit_r.y + S(30), (int)edit_r.width);

    bool saved = false;

    if (g_sel < 0) {
        Label((int)edit_r.x + S(14), (int)edit_r.y + S(52),
              "\xe2\x86\x90 Select a faction from the list", 13, {110, 115, 140, 255});
        return false;
    }

    int ex = (int)edit_r.x + S(14);
    int ey = (int)edit_r.y + S(42);

    // id (read-only)
    char id_buf[16]; snprintf(id_buf, sizeof(id_buf), "%u", g_factions[g_sel].id);
    Label(ex, ey, "id :", 13);
    Label(ex + S(40), ey, id_buf, 13, YELLOW);
    ey += S(32);

    // name
    Label(ex, ey, "name :", 13);
    TextBox(2001, {(float)(ex + S(70)), (float)ey, (float)S(200), (float)S(26)},
            g_buf_name, 32);
    ey += S(36);

    // default_relation
    Label(ex, ey, "default_rel :", 13);
    TextBox(2002, {(float)(ex + S(110)), (float)ey, (float)S(72), (float)S(26)},
            g_buf_defrel, 8);
    Label(ex + S(190), ey + S(6), "(-100\xe2\x80\xa6+100)", 11, {100, 105, 130, 255});
    ey += S(44);

    HSep(ex, ey, (int)edit_r.width - S(28)); ey += S(12);

    // Relations matrix
    Label(ex, ey, "Relations to other factions:", 13, {170, 195, 255, 255});
    ey += S(22);

    for (int j = 0; j < g_count && j < 8; ++j) {
        bool is_self = (g_factions[j].id == g_factions[g_sel].id);
        Color lc = is_self ? Color{120, 200, 120, 255} : Color{170, 175, 195, 255};

        char lbl[40];
        snprintf(lbl, sizeof(lbl), "  \xe2\x86\x92 %-14s :", g_factions[j].name);
        Label(ex, ey, lbl, 12, lc);
        int lw = UiMeasure(lbl, 12);

        TextBox(2010 + j,
                {(float)(ex + lw + S(4)), (float)ey, (float)S(65), (float)S(22)},
                g_buf_rels[j], 8);

        int rel_val = (int)strtol(g_buf_rels[j], nullptr, 10);
        Color vc = (rel_val >= 25)  ? Color{80,200,80,255}  :
                   (rel_val <= -25) ? Color{220,70,70,255}  :
                                      Color{180,180,80,255};
        char val_disp[8]; snprintf(val_disp, sizeof(val_disp), "%+d", rel_val);
        UiText(val_disp, ex + lw + S(74), ey + S(4), 11, vc);

        ey += S(26);
    }

    ey += S(10);
    HSep(ex, ey, (int)edit_r.width - S(28)); ey += S(14);

    if (Button({(float)ex, (float)ey, (float)S(110), (float)S(28)},
               "Save File", {36, 110, 55, 255})) {
        ApplyEdit();
        saved = Save(path);
    }

    return saved;
}

} // namespace FactionEditor
