#ifdef MONKEY_DUST_EDITOR
#include "editor_flare_browser.h"
#include "editor_core.h"
#include <monkey_dust/combat/power_manager.h>
#include <monkey_dust/platform/md_log.h>
#include "imgui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ── JSON field extractors (strstr pattern, same as PowerManager) ─────────────

static int jint(const char* line, const char* key) {
    const char* p = strstr(line, key);
    return p ? (int)strtol(p + strlen(key), nullptr, 10) : 0;
}
static float jfloat(const char* line, const char* key) {
    const char* p = strstr(line, key);
    return p ? strtof(p + strlen(key), nullptr) : 0.0f;
}
static void jstr(const char* line, const char* key, char* dst, int dstsz) {
    const char* p = strstr(line, key);
    if (!p) { dst[0] = '\0'; return; }
    p += strlen(key);
    int n = 0;
    while (*p && *p != '"' && n < dstsz - 1) dst[n++] = *p++;
    dst[n] = '\0';
}

// ── filter (case-insensitive substring) ──────────────────────────────────────

bool EditorFlareBrowser::Match(const char* text, const char* f) {
    if (!f[0]) return true;
    char lt[64], lf[64]; int i = 0;
    for (; text[i] && i < 63; ++i)
        lt[i] = (text[i] >= 'A' && text[i] <= 'Z') ? text[i] + 32 : text[i];
    lt[i] = '\0'; i = 0;
    for (; f[i] && i < 63; ++i)
        lf[i] = (f[i] >= 'A' && f[i] <= 'Z') ? f[i] + 32 : f[i];
    lf[i] = '\0';
    return strstr(lt, lf) != nullptr;
}

// ── Load (lazy, called once on first Draw with panel open) ───────────────────

void EditorFlareBrowser::Load() {
    static char buf[131072];

    {
        FILE* f = fopen("data/flare/items.json", "r");
        if (f) {
            size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = '\0';
            for (char* line = buf; *line && items_cnt_ < MAX_ITEMS; ) {
                char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
                if (strstr(line, "\"id\":")) {
                    ItemEntry& e = items_[items_cnt_];
                    e.id      = jint  (line, "\"id\":");
                    e.level   = jint  (line, "\"level\":");
                    e.price   = jint  (line, "\"price\":");
                    e.dmg_min = jint  (line, "\"dmg_min\":");
                    e.dmg_max = jint  (line, "\"dmg_max\":");
                    jstr(line, "\"name\":\"",     e.name,     sizeof(e.name));
                    jstr(line, "\"dmg_type\":\"", e.dmg_type, sizeof(e.dmg_type));
                    if (e.id > 0) ++items_cnt_;
                }
                if (!nl) break; line = nl + 1;
            }
        }
    }

    {
        FILE* f = fopen("data/flare/enemies.json", "r");
        if (f) {
            size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = '\0';
            for (char* line = buf; *line && enemies_cnt_ < MAX_ENEMIES; ) {
                char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
                if (strstr(line, "\"category\":")) {
                    EnemyEntry& e = enemies_[enemies_cnt_];
                    e.level   = jint  (line, "\"level\":");
                    e.hp      = jint  (line, "\"hp\":");
                    e.dmg_min = jint  (line, "\"dmg_melee_min\":");
                    e.dmg_max = jint  (line, "\"dmg_melee_max\":");
                    e.speed   = jfloat(line, "\"speed\":");
                    jstr(line, "\"category\":\"", e.category, sizeof(e.category));
                    jstr(line, "\"name\":\"",     e.name,     sizeof(e.name));
                    if (e.category[0]) ++enemies_cnt_;
                }
                if (!nl) break; line = nl + 1;
            }
        }
    }

    PowerManager::Get().LoadFromJson("data/flare/powers.json");

    MD_LOG(MD_LOG_INFO, "EditorFlareBrowser: %d items, %d enemies, %d powers",
             items_cnt_, enemies_cnt_, PowerManager::Get().Count());
    loaded_ = true;
}

// ── Items tab ─────────────────────────────────────────────────────────────────

void EditorFlareBrowser::DrawItems() {
    ImGui::BeginChild("##items_list", ImVec2(0, -110), false);
    ImGui::Columns(4, "items_cols", true);
    ImGui::SetColumnWidth(0, 44);
    ImGui::SetColumnWidth(1, 200);
    ImGui::SetColumnWidth(2, 40);
    ImGui::SetColumnWidth(3, 60);
    ImGui::TextDisabled("ID");    ImGui::NextColumn();
    ImGui::TextDisabled("Name");  ImGui::NextColumn();
    ImGui::TextDisabled("Lv");    ImGui::NextColumn();
    ImGui::TextDisabled("Price"); ImGui::NextColumn();
    ImGui::Separator();
    for (int i = 0; i < items_cnt_; ++i) {
        const ItemEntry& it = items_[i];
        if (!Match(it.name, filter_)) continue;
        char idbuf[8]; snprintf(idbuf, 8, "%d", it.id);
        if (ImGui::Selectable(idbuf, sel_[0] == i,
                ImGuiSelectableFlags_SpanAllColumns)) sel_[0] = i;
        ImGui::NextColumn();
        ImGui::Text("%s", it.name);  ImGui::NextColumn();
        ImGui::Text("%d", it.level); ImGui::NextColumn();
        ImGui::Text("%d", it.price); ImGui::NextColumn();
    }
    ImGui::Columns(1);
    ImGui::EndChild();

    ImGui::Separator();
    if (sel_[0] >= 0 && sel_[0] < items_cnt_) {
        const ItemEntry& it = items_[sel_[0]];
        ImGui::Text("id=%d  lv=%d  price=%d", it.id, it.level, it.price);
        if (it.dmg_type[0])
            ImGui::Text("dmg: %s  %d-%d", it.dmg_type, it.dmg_min, it.dmg_max);
        else
            ImGui::TextDisabled("(no weapon damage)");
    } else {
        ImGui::TextDisabled("select an item");
    }
}

// ── Enemies tab ───────────────────────────────────────────────────────────────

void EditorFlareBrowser::DrawEnemies() {
    ImGui::BeginChild("##enemies_list", ImVec2(0, -110), false);
    ImGui::Columns(4, "enemies_cols", true);
    ImGui::SetColumnWidth(0, 170);
    ImGui::SetColumnWidth(1, 40);
    ImGui::SetColumnWidth(2, 50);
    ImGui::SetColumnWidth(3, 55);
    ImGui::TextDisabled("Category"); ImGui::NextColumn();
    ImGui::TextDisabled("Lv");       ImGui::NextColumn();
    ImGui::TextDisabled("HP");       ImGui::NextColumn();
    ImGui::TextDisabled("Speed");    ImGui::NextColumn();
    ImGui::Separator();
    for (int i = 0; i < enemies_cnt_; ++i) {
        const EnemyEntry& en = enemies_[i];
        if (!Match(en.category, filter_) && !Match(en.name, filter_)) continue;
        if (ImGui::Selectable(en.category, sel_[1] == i,
                ImGuiSelectableFlags_SpanAllColumns)) sel_[1] = i;
        ImGui::NextColumn();
        ImGui::Text("%d", en.level);     ImGui::NextColumn();
        ImGui::Text("%d", en.hp);        ImGui::NextColumn();
        ImGui::Text("%.1f", en.speed);   ImGui::NextColumn();
    }
    ImGui::Columns(1);
    ImGui::EndChild();

    ImGui::Separator();
    if (sel_[1] >= 0 && sel_[1] < enemies_cnt_) {
        const EnemyEntry& en = enemies_[sel_[1]];
        ImGui::Text("%s  \"%s\"  lv=%d  hp=%d  dmg=%d-%d  spd=%.1f",
                    en.category, en.name,
                    en.level, en.hp, en.dmg_min, en.dmg_max, en.speed);
    } else {
        ImGui::TextDisabled("select an enemy");
    }
}

// ── Powers tab ────────────────────────────────────────────────────────────────

void EditorFlareBrowser::DrawPowers() {
    const PowerManager& pm = PowerManager::Get();
    ImGui::BeginChild("##powers_list", ImVec2(0, -110), false);
    ImGui::Columns(4, "powers_cols", true);
    ImGui::SetColumnWidth(0, 44);
    ImGui::SetColumnWidth(1, 200);
    ImGui::SetColumnWidth(2, 55);
    ImGui::SetColumnWidth(3, 65);
    ImGui::TextDisabled("ID");    ImGui::NextColumn();
    ImGui::TextDisabled("Name");  ImGui::NextColumn();
    ImGui::TextDisabled("Dmg");   ImGui::NextColumn();
    ImGui::TextDisabled("CD ms"); ImGui::NextColumn();
    ImGui::Separator();
    for (int i = 0; i < pm.Count(); ++i) {
        const PowerDef* p = pm.GetAt(i);
        if (!p || !Match(p->name, filter_)) continue;
        char idbuf[8]; snprintf(idbuf, 8, "%d", p->id);
        if (ImGui::Selectable(idbuf, sel_[2] == i,
                ImGuiSelectableFlags_SpanAllColumns)) sel_[2] = i;
        ImGui::NextColumn();
        ImGui::Text("%s", p->name); ImGui::NextColumn();
        ImGui::Text("%s", p->dmg_type[0] ? p->dmg_type : "-"); ImGui::NextColumn();
        ImGui::Text("%d", p->cooldown_ms); ImGui::NextColumn();
    }
    ImGui::Columns(1);
    ImGui::EndChild();

    ImGui::Separator();
    if (sel_[2] >= 0) {
        const PowerDef* p = pm.GetAt(sel_[2]);
        if (p)
            ImGui::Text("id=%d  %s  dmg=%s  r=%.1fm  cd=%dms",
                        p->id, p->name,
                        p->dmg_type[0] ? p->dmg_type : "-",
                        p->radius, p->cooldown_ms);
    } else {
        ImGui::TextDisabled("select a power");
    }
}

// ── Draw ──────────────────────────────────────────────────────────────────────

void EditorFlareBrowser::Draw() {
    if (!EditorCore::Get().panels_visible[7]) return;
    if (!loaded_) Load();

    if (!ImGui::Begin("FLARE Browser##FlareBrowser",
                      &EditorCore::Get().panels_visible[7])) {
        ImGui::End(); return;
    }

    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(w - 36);
    ImGui::InputText("##flare_filter", filter_, sizeof(filter_));
    ImGui::SameLine();
    if (ImGui::SmallButton("X")) filter_[0] = '\0';

    if (ImGui::BeginTabBar("##flare_tabs")) {
        if (ImGui::BeginTabItem("Items"))   { DrawItems();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Enemies")) { DrawEnemies(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Powers"))  { DrawPowers();  ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
#endif
