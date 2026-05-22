#pragma once
#include "editor_ui.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

// ─────────────────────────────────────────────────────────
// ItemEditor — CRUD для data/items/items.json  (ImGui UI)
// Формат: { "items": [ { "id":N, "name":"...", "weight":N.N } ] }
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
static char      g_buf_name  [32] = {};
static char      g_buf_weight[16] = {};
static bool      g_detached = false;
static ImVec2    g_win_pos  = {120.f, 70.f};
static ImVec2    g_win_size = {680.f, 520.f};

// ── Мінімальний JSON-парсер (без змін) ───────────────────
static const char* ie_ws(const char* p) {
    while (*p && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p; return p;
}
static const char* ie_str(const char* p, char* buf, int maxlen) {
    if (*p == '"') ++p; int i = 0;
    while (*p && *p != '"' && i < maxlen-1) buf[i++] = *p++;
    buf[i] = '\0'; if (*p == '"') ++p; return p;
}
static const char* ie_skip(const char* p, char open, char close) {
    if (*p != open) return p; int d = 0;
    while (*p) {
        if (*p == '"') { ++p; while(*p&&*p!='"'){if(*p=='\\'&&*(p+1))++p;++p;} if(*p)++p; continue; }
        if (*p==open) ++d; else if (*p==close){if(--d==0)return p+1;} ++p;
    }
    return p;
}

inline bool Load(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return false;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    static char buf[8192];
    if (sz>=(long)sizeof(buf)){fclose(f);return false;}
    (void)fread(buf,1,(size_t)sz,f); buf[sz]='\0'; fclose(f);
    g_count=0; g_sel=-1;
    const char* p=strstr(buf,"\"items\""); if(!p) return false;
    p=strchr(p,'['); if(!p) return false; ++p;
    while (g_count<32) {
        p=ie_ws(p); if(*p==']'||*p=='\0') break;
        if(*p!='{'){++p;continue;}
        const char* st=p, *en=ie_skip(p,'{','}');
        ItemEntry& it=g_items[g_count]; memset(&it,0,sizeof(it));
        const char* v;
        v=strstr(st,"\"id\"");     if(v&&v<en){v=strchr(v,':');if(v) it.id=(uint32_t)strtol(v+1,nullptr,10);}
        v=strstr(st,"\"name\"");   if(v&&v<en){v=strchr(v,':');if(v){v=ie_ws(v+1);if(*v=='"')ie_str(v,it.name,32);}}
        v=strstr(st,"\"weight\""); if(v&&v<en){v=strchr(v,':');if(v) it.weight=(float)atof(v+1);}
        g_count++; p=ie_ws(en); if(*p==',')++p;
    }
    return true;
}

inline bool Save(const char* path) {
    FILE* f=fopen(path,"w"); if(!f) return false;
    fprintf(f,"{\n  \"items\": [\n");
    for(int i=0;i<g_count;++i)
        fprintf(f,"    { \"id\": %u, \"name\": \"%-12s\", \"weight\": %.1f }%s\n",
                g_items[i].id,g_items[i].name,g_items[i].weight,(i<g_count-1)?",":"");
    fprintf(f,"  ]\n}\n"); fclose(f); return true;
}

static void ApplyEdit() {
    if(g_sel<0||g_sel>=g_count) return;
    strncpy(g_items[g_sel].name,g_buf_name,31); g_items[g_sel].name[31]='\0';
    for(int i=30;i>=0&&g_items[g_sel].name[i]==' ';--i) g_items[g_sel].name[i]='\0';
    g_items[g_sel].weight=(float)atof(g_buf_weight);
}
static void FillEdit(int idx) {
    if(idx<0||idx>=g_count){g_buf_name[0]='\0';g_buf_weight[0]='\0';return;}
    strncpy(g_buf_name,g_items[idx].name,31); g_buf_name[31]='\0';
    snprintf(g_buf_weight,sizeof(g_buf_weight),"%.2f",g_items[idx].weight);
}

// ── Draw (ImGui) ──────────────────────────────────────────
// Повертає true якщо збережено.
inline bool Draw(const char* path) {
    bool saved = false;

    if (g_detached) {
        ImGui::SetNextWindowPos(g_win_pos,   ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(g_win_size, ImGuiCond_Appearing);
        bool open = true;
        if (!ImGui::Begin("Items##float", &open)) {
            ImGui::End();
            if (!open) g_detached = false;
            ImGui::Dummy({0,0});
            return saved;
        }
        g_win_pos  = ImGui::GetWindowPos();
        g_win_size = ImGui::GetWindowSize();
    }

    // Detach / Dock button (right-aligned toolbar row)
    {
        const char* lbl = g_detached ? "Dock##items" : "Detach##items";
        float btn_w = ImGui::CalcTextSize(lbl).x + ImGui::GetStyle().FramePadding.x * 2.f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btn_w);
        ImGui::PushStyleColor(ImGuiCol_Button,
            g_detached ? ImVec4(0.25f,0.45f,0.65f,1.f) : ImVec4(0.18f,0.18f,0.28f,1.f));
        if (ImGui::Button(lbl)) g_detached = !g_detached;
        ImGui::PopStyleColor();
    }

    // ── Список ────────────────────────────────────────────
    ImGui::BeginChild("##item_list", {280, 0}, ImGuiChildFlags_Borders);

    if (EditorUI::font_bold) ImGui::PushFont(EditorUI::font_bold);
    ImGui::Text("Items");
    if (EditorUI::font_bold) ImGui::PopFont();
    ImGui::Separator();

    for (int i = 0; i < g_count; ++i) {
        char label[64];
        snprintf(label, sizeof(label), "%u:  %s\t%.1f kg",
                 g_items[i].id, g_items[i].name, g_items[i].weight);
        bool sel = (i == g_sel);
        if (ImGui::Selectable(label, sel, ImGuiSelectableFlags_AllowOverlap)) {
            if (!sel) { ApplyEdit(); g_sel = i; FillEdit(i); }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ── Форма редагування ─────────────────────────────────
    ImGui::BeginChild("##item_edit", {0, 0}, ImGuiChildFlags_Borders);

    if (EditorUI::font_bold) ImGui::PushFont(EditorUI::font_bold);
    ImGui::Text("Edit Item");
    if (EditorUI::font_bold) ImGui::PopFont();
    ImGui::Separator();

    if (g_sel < 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("← Оберіть item зі списку");
    } else {
        ImGui::Spacing();

        // id (read-only)
        if (EditorUI::font_mono) ImGui::PushFont(EditorUI::font_mono);
        ImGui::Text("id:     %u", g_items[g_sel].id);
        if (EditorUI::font_mono) ImGui::PopFont();
        ImGui::Spacing();

        // name
        ImGui::SetNextItemWidth(240);
        if (EditorUI::font_mono) ImGui::PushFont(EditorUI::font_mono);
        ImGui::InputText("name", g_buf_name, sizeof(g_buf_name));

        // weight
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("weight (kg)", g_buf_weight, sizeof(g_buf_weight));
        if (EditorUI::font_mono) ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Add
        if (ImGui::Button("Add", {70, 0})) {
            ApplyEdit();
            if (g_count < 32) {
                ItemEntry& ne = g_items[g_count]; memset(&ne, 0, sizeof(ne));
                uint32_t next = 0;
                for (int i = 0; i < g_count; ++i) if (g_items[i].id > next) next = g_items[i].id;
                ne.id = next + 1;
                strncpy(ne.name, "new_item", 31); ne.weight = 1.0f;
                g_sel = g_count++; FillEdit(g_sel);
            }
        }
        ImGui::SameLine();

        // Delete
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.59f, 0.18f, 0.18f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.76f, 0.26f, 0.26f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.44f, 0.12f, 0.12f, 1.0f});
        if (ImGui::Button("Delete", {70, 0}) && g_count > 1) {
            for (int i = g_sel; i < g_count - 1; ++i) g_items[i] = g_items[i+1];
            --g_count; g_sel = (g_sel < g_count) ? g_sel : g_count - 1;
            FillEdit(g_sel);
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();

        // Save File
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.14f, 0.43f, 0.22f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.20f, 0.58f, 0.30f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.10f, 0.32f, 0.16f, 1.0f});
        if (ImGui::Button("Save File", {90, 0})) {
            ApplyEdit(); saved = Save(path);
        }
        ImGui::PopStyleColor(3);
    }

    ImGui::EndChild();
    if (g_detached) { ImGui::End(); ImGui::Dummy({0,0}); }
    return saved;
}

} // namespace ItemEditor
