#pragma once
// character_editor.h — Character Creator tab for monkey_dust_editor.
// Header-only: SDL_GPU preview via editor_char_preview_sdlgpu.h (MD_SDL_GPU),
// or OpenGL preview via char_preview_gl.h (legacy path).

#include "imgui.h"
#ifdef MD_SDL_GPU
#  include "editor_char_preview_sdlgpu.h"
#else
#  include "char_preview_gl.h"
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace CharacterEditor {

// ── Data ──────────────────────────────────────────────────────────────────────
static constexpr int MAX_MORPHS = 48;

struct Def {
    char  name[32]       = {};
    uint8_t sex          = 0;
    uint8_t race         = 0;
    // Шар 1 — body shape
    float height         = 1.0f;
    float bulk           = 1.0f;
    // Шар 2 — colour
    float skin[3]        = { 0.82f, 0.65f, 0.52f };
    float hair[3]        = { 0.18f, 0.12f, 0.08f };
    float color_strength = 0.55f;
    // Kenshi-style skintone (RE: character.hlsl)
    float skintone_sat   = 1.0f;   // [0.0, 2.0] saturation  (1=neutral)
    float skintone_bri   = 0.0f;   // [-0.3, 0.3] brightness (0=neutral)
    // Шар 3 — morphs
    float morph_w[MAX_MORPHS] = {};
    int   morph_count    = 0;
};

static Def   s_def;
static char  s_path[256]    = "game/data/chars/player.chardef";
static char  s_morph_names[MAX_MORPHS][48] = {};
static bool  s_morphs_loaded = false;
static bool  g_detached = false;
static ImVec2 g_win_pos  = {160.f, 90.f};
static ImVec2 g_win_size = {780.f, 560.f};

// ── JSON helpers ──────────────────────────────────────────────────────────────
static float parse_float(const char* buf, const char* key) {
    const char* p = strstr(buf, key);
    if (!p) return 0.f;
    p = strchr(p, ':');
    return p ? (float)atof(p + 1) : 0.f;
}
static int parse_int(const char* buf, const char* key) {
    const char* p = strstr(buf, key);
    if (!p) return 0;
    p = strchr(p, ':');
    return p ? atoi(p + 1) : 0;
}
static void parse_str(const char* buf, const char* key, char* out, int sz) {
    const char* p = strstr(buf, key);
    if (!p) { out[0] = '\0'; return; }
    p = strchr(p, ':');
    if (!p) { out[0] = '\0'; return; }
    while (*p && (*p == ':' || *p == ' ' || *p == '"')) ++p;
    int n = 0;
    while (*p && *p != '"' && n < sz - 1) out[n++] = *p++;
    out[n] = '\0';
}

static bool LoadJSON(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8192) { fclose(f); return false; }
    char* buf = (char*)malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f); buf[sz] = '\0'; fclose(f);

    parse_str(buf, "\"name\"", s_def.name, 32);
    s_def.sex            = (uint8_t)parse_int(buf, "\"sex\"");
    s_def.race           = (uint8_t)parse_int(buf, "\"race\"");
    s_def.height         = parse_float(buf, "\"height\"");
    s_def.bulk           = parse_float(buf, "\"bulk\"");
    s_def.skin[0]        = parse_float(buf, "\"skin_r\"");
    s_def.skin[1]        = parse_float(buf, "\"skin_g\"");
    s_def.skin[2]        = parse_float(buf, "\"skin_b\"");
    s_def.hair[0]        = parse_float(buf, "\"hair_r\"");
    s_def.hair[1]        = parse_float(buf, "\"hair_g\"");
    s_def.hair[2]        = parse_float(buf, "\"hair_b\"");
    s_def.color_strength = parse_float(buf, "\"color_strength\"");
    {
        float s = parse_float(buf, "\"skintone_sat\"");
        s_def.skintone_sat = (s > 0.01f) ? s : 1.0f;
        s_def.skintone_bri = parse_float(buf, "\"skintone_bri\"");
    }
    s_def.morph_count    = parse_int(buf, "\"morph_count\"");
    if (s_def.height < 0.5f || s_def.height > 2.f) s_def.height = 1.f;
    if (s_def.bulk   < 0.3f || s_def.bulk   > 2.f) s_def.bulk   = 1.f;
    if (s_def.morph_count < 0) s_def.morph_count = 0;
    if (s_def.morph_count > MAX_MORPHS) s_def.morph_count = MAX_MORPHS;

    const char* mp = strstr(buf, "\"morphs\"");
    if (mp) {
        mp = strchr(mp, '[');
        if (mp) { ++mp;
            for (int i = 0; i < s_def.morph_count; ++i) {
                while (*mp && (*mp == ' ' || *mp == ',')) ++mp;
                if (!*mp || *mp == ']') break;
                s_def.morph_w[i] = (float)atof(mp);
                while (*mp && *mp != ',' && *mp != ']') ++mp;
            }
        }
    }
    free(buf);
    return true;
}

static bool SaveMorphsJSON(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    const char* fallback[6] = {"tall","fat","muscular","longlegs","bighead","broadshdr"};
    int n = s_def.morph_count < 6 ? s_def.morph_count : 6;
    fprintf(f, "{\n");
    for (int i = 0; i < n; ++i) {
        const char* nm = (s_morphs_loaded && s_morph_names[i][0]) ? s_morph_names[i] : fallback[i];
        fprintf(f, "  \"%s\": %.4f%s\n", nm, s_def.morph_w[i], i < n-1 ? "," : "");
    }
    fprintf(f, "}\n");
    fclose(f);
    return true;
}

static bool SaveJSON(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "{\n  \"name\": \"%s\",\n", s_def.name);
    fprintf(f, "  \"sex\": %d,\n  \"race\": %d,\n", s_def.sex, s_def.race);
    fprintf(f, "  \"height\": %.4f,\n  \"bulk\": %.4f,\n", s_def.height, s_def.bulk);
    fprintf(f, "  \"skin_r\": %.4f,\n  \"skin_g\": %.4f,\n  \"skin_b\": %.4f,\n",
            s_def.skin[0], s_def.skin[1], s_def.skin[2]);
    fprintf(f, "  \"hair_r\": %.4f,\n  \"hair_g\": %.4f,\n  \"hair_b\": %.4f,\n",
            s_def.hair[0], s_def.hair[1], s_def.hair[2]);
    fprintf(f, "  \"color_strength\": %.4f,\n", s_def.color_strength);
    fprintf(f, "  \"skintone_sat\": %.4f,\n  \"skintone_bri\": %.4f,\n",
            s_def.skintone_sat, s_def.skintone_bri);
    fprintf(f, "  \"morph_count\": %d", s_def.morph_count);
    if (s_def.morph_count > 0) {
        fprintf(f, ",\n  \"morphs\": [");
        for (int i = 0; i < s_def.morph_count; ++i)
            fprintf(f, "%s%.4f", i ? "," : "", s_def.morph_w[i]);
        fprintf(f, "]");
    }
    fprintf(f, "\n}\n");
    fclose(f);
    return true;
}

static void LoadMorphNames(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    int n = 0;
    while (n < MAX_MORPHS && fgets(s_morph_names[n], 48, f)) {
        char* nl = strchr(s_morph_names[n], '\n');
        if (nl) *nl = '\0';
        if (s_morph_names[n][0]) ++n;
    }
    fclose(f);
    if (n > s_def.morph_count) s_def.morph_count = n;
    s_morphs_loaded = (n > 0);
}

// ── Helpers: Kenshi-style label|slider table ──────────────────────────────────
static bool BeginPropTable() {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f, 2.0f});
    float w = ImGui::GetContentRegionAvail().x - 8.0f;
    if (!ImGui::BeginTable("##pt", 2,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
            {w, 0})) {
        ImGui::PopStyleVar();
        return false;
    }
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,   120.0f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    return true;
}
static void EndPropTable() { ImGui::EndTable(); ImGui::PopStyleVar(); }

static void PropSlider(const char* label, float* v, float lo, float hi,
                       const char* fmt = "%.2f") {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##v", v, lo, hi, fmt);
    ImGui::PopID();
}

// ── Preview silhouette ────────────────────────────────────────────────────────
static void DrawSilhouette(float W, float H) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float cx = p0.x + W * 0.5f;

    dl->AddRectFilled(p0, {p0.x + W, p0.y + H}, IM_COL32(28, 28, 34, 255), 4.f);

    ImU32 sc = ImGui::ColorConvertFloat4ToU32(
        {s_def.skin[0], s_def.skin[1], s_def.skin[2], 1.f});
    ImU32 hc = ImGui::ColorConvertFloat4ToU32(
        {s_def.hair[0], s_def.hair[1], s_def.hair[2], 1.f});

    float fig_h = H * 0.80f * s_def.height;
    float fig_w = W * 0.26f * s_def.bulk;
    float base_y = p0.y + H * 0.92f;

    float leg_h = fig_h * 0.42f, leg_w = fig_w * 0.22f;
    dl->AddRectFilled({cx - leg_w*1.3f, base_y - leg_h}, {cx - leg_w*0.1f, base_y}, sc, 3.f);
    dl->AddRectFilled({cx + leg_w*0.1f, base_y - leg_h}, {cx + leg_w*1.3f, base_y}, sc, 3.f);

    float torso_h = fig_h * 0.32f, torso_y = base_y - leg_h - torso_h;
    dl->AddRectFilled({cx - fig_w*0.5f, torso_y}, {cx + fig_w*0.5f, base_y - leg_h}, sc, 4.f);

    float arm_w = fig_w * 0.18f, arm_h = torso_h * 0.95f;
    dl->AddRectFilled({cx - fig_w*0.5f - arm_w*1.1f, torso_y},
                      {cx - fig_w*0.5f - arm_w*0.1f, torso_y + arm_h}, sc, 3.f);
    dl->AddRectFilled({cx + fig_w*0.5f + arm_w*0.1f, torso_y},
                      {cx + fig_w*0.5f + arm_w*1.1f, torso_y + arm_h}, sc, 3.f);

    float head_r = fig_w * 0.33f, head_cy = torso_y - head_r * 1.1f;
    dl->AddCircleFilled({cx, head_cy}, head_r, sc, 32);
    dl->AddCircleFilled({cx, head_cy - head_r * 0.45f}, head_r * 0.72f, hc, 24);

    ImGui::Dummy({W, H});
}

// ── Main Draw ─────────────────────────────────────────────────────────────────
static void Draw() {
    if (g_detached) {
        ImGui::SetNextWindowPos(g_win_pos,   ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(g_win_size, ImGuiCond_Appearing);
        bool open = true;
        if (!ImGui::Begin("Characters##float", &open)) {
            ImGui::End();
            if (!open) g_detached = false;
            ImGui::Dummy({0,0});
            return;
        }
        g_win_pos  = ImGui::GetWindowPos();
        g_win_size = ImGui::GetWindowSize();
    }

    // Detach / Dock button (right-aligned)
    {
        const char* lbl = g_detached ? "Dock##chars" : "Detach##chars";
        float btn_w = ImGui::CalcTextSize(lbl).x + ImGui::GetStyle().FramePadding.x * 2.f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btn_w);
        ImGui::PushStyleColor(ImGuiCol_Button,
            g_detached ? ImVec4(0.25f,0.45f,0.65f,1.f) : ImVec4(0.18f,0.18f,0.28f,1.f));
        if (ImGui::Button(lbl)) g_detached = !g_detached;
        ImGui::PopStyleColor();
    }

    float total_w = ImGui::GetContentRegionAvail().x;
    float left_w  = total_w * 0.30f;

    // ── Left panel: identity, colours, file ──────────────────────────────────
    ImGui::BeginChild("##cc_left", {left_w, 0}, false);

    ImGui::SeparatorText("Identity");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##cc_name", s_def.name, sizeof(s_def.name));
    ImGui::RadioButton("Male##cc",   (int*)&s_def.sex, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Female##cc", (int*)&s_def.sex, 1);
    const char* races[] = { "Human", "Shek", "Hive Worker", "Hive Prince", "Skeleton" };
    int ri = s_def.race;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##cc_race", &ri, races, 5)) s_def.race = (uint8_t)ri;
    ImGui::SameLine(); ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Race");

    ImGui::SeparatorText("Colours");
    ImGui::SetNextItemWidth(-1);
    ImGui::ColorEdit3("Skin##cc", s_def.skin);
    ImGui::SetNextItemWidth(-1);
    ImGui::ColorEdit3("Hair##cc", s_def.hair);
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("Tint##cc", &s_def.color_strength, 0.f, 1.f, "%.2f");
    // Kenshi-style skintone (RE: character.hlsl colorise — sat/bri applied in shader)
    if (BeginPropTable()) {
        PropSlider("Saturation", &s_def.skintone_sat, 0.0f, 2.0f, "%.2f");
        PropSlider("Brightness", &s_def.skintone_bri, -0.3f, 0.3f, "%.2f");
        EndPropTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("Rand Body##cc")) {
        s_def.height = 0.88f + (rand()%100)/100.f*0.28f;
        s_def.bulk   = 0.80f + (rand()%100)/100.f*0.48f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Rand Skin##cc")) {
        s_def.skin[0] = 0.35f+(rand()%100)/100.f*0.55f;
        s_def.skin[1] = 0.25f+(rand()%100)/100.f*0.45f;
        s_def.skin[2] = 0.15f+(rand()%100)/100.f*0.35f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##cc")) { s_def = Def{}; strncpy(s_def.name,"Player",7); }

    ImGui::SeparatorText("File");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##ccpath", s_path, sizeof(s_path));
    if (ImGui::Button("Save##cc"))       SaveJSON(s_path);
    ImGui::SameLine();
    if (ImGui::Button("Load##cc"))       LoadJSON(s_path);

    // ── BODY / FACE / HAIR tabs — ліва панель, під File ──────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::BeginTabBar("##cc_tabs")) {

        if (ImGui::BeginTabItem("BODY")) {
            ImGui::Spacing();
            if (BeginPropTable()) {
                PropSlider("Height", &s_def.height, 0.80f, 1.20f);
                PropSlider("Bulk",   &s_def.bulk,   0.70f, 1.40f);
                EndPropTable();
            }
            // Named body morphs (tall/fat/muscular/longlegs/bighead/broadshdr)
            if (s_morphs_loaded && s_def.morph_count >= 6) {
                ImGui::Spacing();
                ImGui::SeparatorText("Shape");
                if (BeginPropTable()) {
                    for (int i = 0; i < 6; ++i)
                        PropSlider(s_morph_names[i], &s_def.morph_w[i], -1.f, 1.f, "%.2f");
                    EndPropTable();
                }
                ImGui::Spacing();
                if (ImGui::Button("Export Morphs##cc"))
                    SaveMorphsJSON("game/data/chars/player_morphs.json");
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("FACE")) {
            ImGui::Spacing();
            int n = s_def.morph_count < MAX_MORPHS ? s_def.morph_count : MAX_MORPHS;
            if (n > 0 && BeginPropTable()) {
                for (int i = 0; i < n; ++i) {
                    const char* lbl = s_morphs_loaded ? s_morph_names[i] : "morph";
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(lbl);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushID(i);
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::SliderFloat("##m", &s_def.morph_w[i], -1.f, 1.f, "%.2f");
                    ImGui::PopID();
                }
                EndPropTable();
            } else if (n == 0) {
                ImGui::Spacing();
                ImGui::TextDisabled("No morphs loaded.");
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("HAIR")) {
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-1);
            ImGui::ColorEdit3("Colour##hair_cc", s_def.hair);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndChild();

    // ── Right panel: full-height 3D preview ──────────────────────────────────
    ImGui::SameLine();
    ImGui::BeginChild("##cc_right", {0, 0}, false);

    // Init 3D preview on first use
    {
        static bool s_preview_init = false;
        if (!s_preview_init) {
            s_preview_init = true;
#ifdef MD_SDL_GPU
            CharPreviewSDLGPU::Init(
                "game/data/props/md_human.glb",
                "game/data/textures/md_human_body.png");
#else
            CharPreviewGL::Init(
                "game/data/props/md_human.glb",
                "game/data/textures/md_human_body.png");
#endif
        }
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float skin_rgb[3] = { s_def.skin[0], s_def.skin[1], s_def.skin[2] };

    // Map body morphs → effective height/bulk for the preview
    // tall[0] + longlegs[3] boost height; fat[1] + muscular[2] + broadshdr[5] boost bulk
    auto mw = [&](int i) { return (s_def.morph_count > i) ? s_def.morph_w[i] : 0.f; };
    float eff_h = s_def.height * (1.f + mw(0)*0.20f + mw(3)*0.12f);
    float eff_b = s_def.bulk   * (1.f + mw(1)*0.25f + mw(2)*0.15f + mw(5)*0.10f);
    if (eff_h < 0.5f) eff_h = 0.5f; if (eff_h > 1.8f) eff_h = 1.8f;
    if (eff_b < 0.4f) eff_b = 0.4f; if (eff_b > 2.0f) eff_b = 2.0f;

#ifdef MD_SDL_GPU
    CharPreviewSDLGPU::DrawInImGui(
        avail.x, avail.y,
        eff_h, eff_b,
        skin_rgb, s_def.color_strength,
        s_def.skintone_sat, s_def.skintone_bri);
#else
    CharPreviewGL::DrawInImGui(
        avail.x, avail.y,
        eff_h, eff_b,
        skin_rgb, s_def.color_strength,
        s_def.skintone_sat, s_def.skintone_bri);
#endif

    ImGui::EndChild();
    if (g_detached) { ImGui::End(); ImGui::Dummy({0,0}); }
}

} // namespace CharacterEditor
