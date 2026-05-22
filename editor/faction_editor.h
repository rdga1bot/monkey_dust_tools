#pragma once
#include "editor_ui.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

// ─────────────────────────────────────────────────────────
// FactionEditor — CRUD для data/factions/factions.json (ImGui UI)
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
static char         g_buf_name  [32]   = {};
static char         g_buf_defrel[8]    = {};
static char         g_buf_rels  [8][8] = {};
static bool         g_detached = false;
static ImVec2       g_win_pos  = {140.f, 80.f};
static ImVec2       g_win_size = {640.f, 500.f};

// ── Мінімальний JSON-парсер ───────────────────────────────
static const char* fe_ws(const char* p){while(*p&&(*p==' '||*p=='\t'||*p=='\n'||*p=='\r'))++p;return p;}
static const char* fe_str(const char* p,char* buf,int maxlen){
    if(*p=='"')++p;int i=0;
    while(*p&&*p!='"'&&i<maxlen-1)buf[i++]=*p++;
    buf[i]='\0';if(*p=='"')++p;return p;}
static const char* fe_skip(const char* p,char open,char close){
    if(*p!=open)return p;int d=0;
    while(*p){
        if(*p=='"'){++p;while(*p&&*p!='"'){if(*p=='\\'&&*(p+1))++p;++p;}if(*p)++p;continue;}
        if(*p==open)++d;else if(*p==close){if(--d==0)return p+1;}++p;}
    return p;}

inline bool Load(const char* path) {
    FILE* f=fopen(path,"rb");if(!f)return false;
    fseek(f,0,SEEK_END);long sz=ftell(f);fseek(f,0,SEEK_SET);
    static char buf[16384];
    if(sz>=(long)sizeof(buf)){fclose(f);return false;}
    (void)fread(buf,1,(size_t)sz,f);buf[sz]='\0';fclose(f);
    g_count=0;g_sel=-1;
    const char* p=strstr(buf,"\"factions\"");if(!p)return false;
    p=strchr(p,'[');if(!p)return false;++p;
    while(g_count<16){
        p=fe_ws(p);if(*p==']'||*p=='\0')break;
        if(*p!='{'){++p;continue;}
        const char* st=p,*en=fe_skip(p,'{','}');
        FactionEntry& fe=g_factions[g_count];memset(&fe,0,sizeof(fe));
        const char* v;
        v=strstr(st,"\"id\"");if(v&&v<en){v=strchr(v,':');if(v)fe.id=(uint32_t)strtol(v+1,nullptr,10);}
        v=strstr(st,"\"name\"");if(v&&v<en){v=strchr(v,':');if(v){v=fe_ws(v+1);if(*v=='"')fe_str(v,fe.name,32);}}
        v=strstr(st,"\"default_relation\"");if(v&&v<en){v=strchr(v,':');if(v)fe.default_rel=(int)strtol(v+1,nullptr,10);}
        const char* ra=strstr(st,"\"relations\"");
        if(ra&&ra<en){ra=strchr(ra,'[');if(ra&&ra<en){++ra;
            while(fe.rel_count<8){ra=fe_ws(ra);if(*ra==']'||*ra=='\0'||ra>=en)break;
                if(*ra!='{'){++ra;continue;}
                const char* os=ra,*oe=fe_skip(ra,'{','}');
                int to_id=0,val=0;
                const char* tv=strstr(os,"\"to\"");if(tv&&tv<oe){tv=strchr(tv,':');if(tv)to_id=(int)strtol(tv+1,nullptr,10);}
                const char* vv=strstr(os,"\"value\"");if(vv&&vv<oe){vv=strchr(vv,':');if(vv)val=(int)strtol(vv+1,nullptr,10);}
                if(to_id>=1&&to_id<=8)fe.rel_to[to_id-1]=val;
                fe.rel_count++;ra=fe_ws(oe);if(*ra==',')++ra;}}}
        g_count++;p=fe_ws(en);if(*p==',')++p;}
    return true;
}

inline bool Save(const char* path) {
    FILE* f=fopen(path,"w");if(!f)return false;
    fprintf(f,"{\n  \"factions\": [\n");
    for(int i=0;i<g_count;++i){
        FactionEntry& fe=g_factions[i];
        fprintf(f,"    {\n");
        fprintf(f,"      \"id\": %u,\n",fe.id);
        fprintf(f,"      \"name\": \"%s\",\n",fe.name);
        fprintf(f,"      \"default_relation\": %d,\n",fe.default_rel);
        fprintf(f,"      \"relations\": [\n");
        for(int j=0;j<g_count;++j)
            fprintf(f,"        { \"to\": %d, \"value\": %5d }%s\n",j+1,fe.rel_to[j],(j<g_count-1)?",":"");
        fprintf(f,"      ]\n");
        fprintf(f,"    }%s\n",(i<g_count-1)?",":"");
    }
    fprintf(f,"  ]\n}\n");fclose(f);return true;
}

static void ApplyEdit(){
    if(g_sel<0||g_sel>=g_count)return;
    strncpy(g_factions[g_sel].name,g_buf_name,31);g_factions[g_sel].name[31]='\0';
    g_factions[g_sel].default_rel=(int)strtol(g_buf_defrel,nullptr,10);
    for(int j=0;j<g_count&&j<8;++j)
        g_factions[g_sel].rel_to[j]=(int)strtol(g_buf_rels[j],nullptr,10);
}
static void FillEdit(int idx){
    if(idx<0||idx>=g_count){g_buf_name[0]='\0';g_buf_defrel[0]='\0';return;}
    strncpy(g_buf_name,g_factions[idx].name,31);g_buf_name[31]='\0';
    snprintf(g_buf_defrel,sizeof(g_buf_defrel),"%d",g_factions[idx].default_rel);
    for(int j=0;j<g_count&&j<8;++j)
        snprintf(g_buf_rels[j],sizeof(g_buf_rels[j]),"%d",g_factions[idx].rel_to[j]);
}

// ── Draw (ImGui) ──────────────────────────────────────────
inline bool Draw(const char* path) {
    bool saved = false;

    if (g_detached) {
        ImGui::SetNextWindowPos(g_win_pos,   ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(g_win_size, ImGuiCond_Appearing);
        bool open = true;
        if (!ImGui::Begin("Factions##float", &open)) {
            ImGui::End();
            if (!open) g_detached = false;
            return saved;
        }
        g_win_pos  = ImGui::GetWindowPos();
        g_win_size = ImGui::GetWindowSize();
    }

    // Detach / Dock button (right-aligned toolbar row)
    {
        const char* lbl = g_detached ? "Dock##fac" : "Detach##fac";
        float btn_w = ImGui::CalcTextSize(lbl).x + ImGui::GetStyle().FramePadding.x * 2.f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btn_w);
        ImGui::PushStyleColor(ImGuiCol_Button,
            g_detached ? ImVec4(0.25f,0.45f,0.65f,1.f) : ImVec4(0.18f,0.18f,0.28f,1.f));
        if (ImGui::Button(lbl)) g_detached = !g_detached;
        ImGui::PopStyleColor();
    }

    // ── Список ────────────────────────────────────────────
    ImGui::BeginChild("##faction_list", {240, 0}, ImGuiChildFlags_Borders);

    if (EditorUI::font_bold) ImGui::PushFont(EditorUI::font_bold);
    ImGui::Text("Factions");
    if (EditorUI::font_bold) ImGui::PopFont();
    ImGui::Separator();

    for (int i = 0; i < g_count; ++i) {
        char label[48];
        snprintf(label, sizeof(label), "%u:  %s", g_factions[i].id, g_factions[i].name);
        if (ImGui::Selectable(label, g_sel == i)) {
            if (g_sel != i) { ApplyEdit(); g_sel = i; FillEdit(i); }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ── Форма редагування ─────────────────────────────────
    ImGui::BeginChild("##faction_edit", {0, 0}, ImGuiChildFlags_Borders);

    if (EditorUI::font_bold) ImGui::PushFont(EditorUI::font_bold);
    ImGui::Text("Edit Faction");
    if (EditorUI::font_bold) ImGui::PopFont();
    ImGui::Separator();

    if (g_sel < 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("← Оберіть фракцію зі списку");
    } else {
        ImGui::Spacing();

        // id read-only
        if (EditorUI::font_mono) ImGui::PushFont(EditorUI::font_mono);
        ImGui::Text("id:  %u", g_factions[g_sel].id);
        if (EditorUI::font_mono) ImGui::PopFont();
        ImGui::Spacing();

        // name
        if (EditorUI::font_mono) ImGui::PushFont(EditorUI::font_mono);
        ImGui::SetNextItemWidth(220);
        ImGui::InputText("name", g_buf_name, sizeof(g_buf_name));

        // default_rel
        ImGui::SetNextItemWidth(80);
        ImGui::InputText("default_rel (-100…+100)", g_buf_defrel, sizeof(g_buf_defrel));
        if (EditorUI::font_mono) ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Relations matrix
        if (EditorUI::font_bold) ImGui::PushFont(EditorUI::font_bold);
        ImGui::Text("Relations");
        if (EditorUI::font_bold) ImGui::PopFont();
        ImGui::Spacing();

        for (int j = 0; j < g_count && j < 8; ++j) {
            bool is_self = (g_factions[j].id == g_factions[g_sel].id);
            int  rel_val = (int)strtol(g_buf_rels[j], nullptr, 10);

            // Колір значення відношення
            ImVec4 vc = (rel_val >= 25)  ? ImVec4{0.31f, 0.78f, 0.31f, 1.0f}
                      : (rel_val <= -25) ? ImVec4{0.86f, 0.27f, 0.27f, 1.0f}
                                         : ImVec4{0.70f, 0.70f, 0.31f, 1.0f};
            if (is_self) vc = {0.47f, 0.78f, 0.47f, 1.0f};

            ImGui::TextColored(vc, "→ %-14s", g_factions[j].name);
            ImGui::SameLine(180);

            char field_id[16]; snprintf(field_id, sizeof(field_id), "##rel%d", j);
            if (EditorUI::font_mono) ImGui::PushFont(EditorUI::font_mono);
            ImGui::SetNextItemWidth(70);
            ImGui::InputText(field_id, g_buf_rels[j], sizeof(g_buf_rels[j]));
            if (EditorUI::font_mono) ImGui::PopFont();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

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
    if (g_detached) ImGui::End();
    return saved;
}

} // namespace FactionEditor
