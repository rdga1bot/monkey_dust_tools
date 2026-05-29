#pragma once
// character_editor.h v2 — Kenshi-style 3-panel character creator.
// LEFT (160px): race/gender/desc/stats  |  CENTER: 3D preview  |  RIGHT (270px): BODY/FACE/HAIR sliders
// Redesigned to match Kenshi character editor layout from RE analysis.

#include "imgui.h"
#ifdef MD_SDL_GPU
#  include "editor_char_preview_sdlgpu.h"
#else
#  include "char_preview_gl.h"
#endif
#include "bug_capture.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace CharacterEditor {

// ── Window state (required by editor_layout / main.cpp) ──────────────────────
static bool    g_detached  = false;
static ImVec2  g_win_pos   = {160.f, 60.f};
static ImVec2  g_win_size  = {900.f, 640.f};

// ── Morph layout ─────────────────────────────────────────────────────────────
// BODY  [0..17]  18 entries  (body[0..1]=skin params hidden from slider list)
// FACE  [0..23]  24 sliders  (face shape morphs, Kenshi male_editor.cfg order)
// HAIR  [0..4]    5 sliders  (hair + skin colour params)
static constexpr int BODY_N   = 18;
static constexpr int FACE_N   = 24;
static constexpr int HAIR_N   =  5;
static constexpr int FACE_OFF = BODY_N;
static constexpr int HAIR_OFF = BODY_N + FACE_N;
static constexpr int MORPH_TOT = BODY_N + FACE_N + HAIR_N;  // 47

// BODY sliders — exact Kenshi names from male_editor.cfg + RE audit.
// body[0..1] are skin-tone params; they remain in the array for save compat
// but are NOT shown in the BODY tab slider loop (Skin Colour shown as colour picker).
static const char* const kBodyLbl[BODY_N] = {
    "Skin tone",   "Skin tone 2",  "Height",       "Frame",
    "Posture",     "Shoulder set", "Neck position","Leg length",
    "Shoulders",   "Arm bulk",     "Waist",        "Hands",
    "Chest",       "Stomach",      "Breast size",  "Hips",
    "Legs bulk",   "Feet"
};
// Ranges from male_editor.cfg (authoritative — matches Kenshi binary per RE audit)
static const float kBodyLo[BODY_N] = {
     0.f,   // [0] skin param
     0.f,   // [1] skin param
    80.f,   // [2] Height
    80.f,   // [3] Frame
     0.f,   // [4] Posture      (cfg: PostureL=0)
    45.f,   // [5] Shoulder set (cfg: Shoulder setL=45 — not 0)
    25.f,   // [6] Neck position
    85.f,   // [7] Leg length
    90.f,   // [8] Shoulders
    70.f,   // [9] Arm bulk     (cfg: Arm bulkL=70)
    65.f,   // [10] Waist       (cfg: WaistL=65)
    85.f,   // [11] Hands
    80.f,   // [12] Chest       (cfg: ChestL=80 — keep legacy lo)
    70.f,   // [13] Stomach
    70.f,   // [14] Breast size
    75.f,   // [15] Hips        (cfg: HipsL=75 — was 80, now corrected)
    70.f,   // [16] Legs bulk
    80.f,   // [17] Feet
};
static const float kBodyHi[BODY_N] = {
   100.f,  // [0] skin param
   100.f,  // [1] skin param
   120.f,  // [2] Height
   120.f,  // [3] Frame
    70.f,  // [4] Posture      (cfg: Posture=70 — was 99, now corrected)
    90.f,  // [5] Shoulder set
    80.f,  // [6] Neck position
   115.f,  // [7] Leg length
   110.f,  // [8] Shoulders
   145.f,  // [9] Arm bulk     (cfg: Arm bulk=145 — was 135, now corrected)
   130.f,  // [10] Waist
   115.f,  // [11] Hands
   140.f,  // [12] Chest
   190.f,  // [13] Stomach
   130.f,  // [14] Breast size
   145.f,  // [15] Hips        (cfg: Hips=145 — was 110, now corrected)
   130.f,  // [16] Legs bulk
   120.f,  // [17] Feet
};
// Defaults: bone-scale neutrals at 100 (= no deformation). Posture = 0 (straight stance).
static const float kBodyDef[BODY_N] = {
    50.f,   // [0] skin param
    50.f,   // [1] skin param
   100.f,   // [2] Height
   100.f,   // [3] Frame
     0.f,   // [4] Posture:      0 = straight T-pose (no hunch)
    68.f,   // [5] Shoulder set: mid(45,90)
    53.f,   // [6] Neck position:mid(25,80)
   100.f,   // [7] Leg length
   100.f,   // [8] Shoulders
   100.f,   // [9] Arm bulk:     neutral=100 (RE verified: Kenshi inits all sliders to 100)
   100.f,   // [10] Waist:       mid with mid=100
   100.f,   // [11] Hands
   100.f,   // [12] Chest:       mid=100 per cfg
   100.f,   // [13] Stomach:     mid=100 per cfg (range 70-190, center ≠ 100 but Kenshi uses 100)
   100.f,   // [14] Breast size
   100.f,   // [15] Hips:        mid=100 per cfg
   100.f,   // [16] Legs bulk
   100.f,   // [17] Feet
};

// FACE sliders — Kenshi male_editor.cfg + RE audit.
// face[0..4] = Head/Neck bone scales (no morph).
// face[5..23] = OGRE pose targets OR bone-only (Jaw[17]).
// face[14] "Eyes depth" → shallow_eyes  (repurposed from unused Cheekbone ht.)
// face[22] "Eyes tilt"  → tiltup/tiltdown_eyes (repurposed from unused Chin width)
// face[23] "Nose pos."  → high_nose     (repurposed from unused Chin protrusion)
static const char* const kFaceLbl[FACE_N] = {
    "Head size",      "Head shape",    "Neck",          "Neck width",
    "Neck length",    "Eye size",      "Eye shape",     "Eye spacing",
    "Eye height",     "Nose width",    "Nose length",   "Nose depth",
    "Nose tip",       "Cheekbone",     "Eyes depth",    "Brow",
    "Brow height",    "Jaw",           "Mouth width",   "Mouth pos.",
    "Lips",           "Chin",          "Eyes tilt",     "Nose pos."
};
// Ranges from male_editor.cfg (lo values), matched to morph encoding where applicable.
// Morph sliders: neutral=100 (Kenshi stores pose weight as 0-200 centred at 100).
// Bone-only sliders: their natural cfg range centred at 100.
static const float kFaceLo[FACE_N] = {
    90.f,   // [0]  Head size      (bone)
    90.f,   // [1]  Head shape     (bone)
    65.f,   // [2]  Neck           (bone)
    70.f,   // [3]  Neck width     (bone)
    90.f,   // [4]  Neck length    (bone)
    50.f,   // [5]  Eye size       → big_eyes
    80.f,   // [6]  Eye shape      → narrow_eyes
    85.f,   // [7]  Eye spacing    → close_eyes
    95.f,   // [8]  Eye height     → high_eyes
    50.f,   // [9]  Nose width     → wide_nose
    70.f,   // [10] Nose length    → long_nose  (cfg: Nose height=70-130)
    90.f,   // [11] Nose depth     → arch_nose
    60.f,   // [12] Nose tip       → tiltup_nose/tiltdown_nose
    90.f,   // [13] Cheekbone      → wide_cheekbones/narrow_cheekbones
    50.f,   // [14] Eyes depth     → shallow_eyes  (cfg: Cheekbone height=50-130)
    50.f,   // [15] Brow           → tiltup_brow/tiltdown_brow
    85.f,   // [16] Brow height    → high_brow/low_brow
    85.f,   // [17] Jaw            (bone)
    40.f,   // [18] Mouth width    → wide_mouth
   -50.f,   // [19] Mouth pos.     (bone-only, no morph in GLB)
    20.f,   // [20] Lips           → big_mouth
    50.f,   // [21] Chin           → overbite/underbite
   -100.f,  // [22] Eyes tilt      → tiltup_eyes/tiltdown_eyes
   -165.f,  // [23] Nose pos.      → high_nose  (cfg: Nose height lo reused)
};
static const float kFaceHi[FACE_N] = {
   110.f,  // [0]  Head size
   110.f,  // [1]  Head shape
   150.f,  // [2]  Neck
   150.f,  // [3]  Neck width
   110.f,  // [4]  Neck length
   150.f,  // [5]  Eye size
   120.f,  // [6]  Eye shape
   115.f,  // [7]  Eye spacing
   105.f,  // [8]  Eye height
   150.f,  // [9]  Nose width
   130.f,  // [10] Nose length
   115.f,  // [11] Nose depth
   150.f,  // [12] Nose tip
   110.f,  // [13] Cheekbone
   130.f,  // [14] Eyes depth
   200.f,  // [15] Brow
   115.f,  // [16] Brow height
   115.f,  // [17] Jaw
   140.f,  // [18] Mouth width
   250.f,  // [19] Mouth pos.
   180.f,  // [20] Lips
   130.f,  // [21] Chin
   100.f,  // [22] Eyes tilt
   165.f,  // [23] Nose pos.
};
// Defaults: bone sliders neutral at 100; morph sliders neutral at mid(lo,hi).
// For morph sliders: def = mid of range (= zero morph weight at neutral).
static const float kFaceDef[FACE_N] = {
   100.f,  // [0]  Head size
   100.f,  // [1]  Head shape
   108.f,  // [2]  Neck:           mid(65,150)
   110.f,  // [3]  Neck width:     mid(70,150)
   100.f,  // [4]  Neck length
   100.f,  // [5]  Eye size:       mid(50,150)
   100.f,  // [6]  Eye shape:      mid(80,120)
   100.f,  // [7]  Eye spacing:    mid(85,115)
   100.f,  // [8]  Eye height:     mid(95,105)
   100.f,  // [9]  Nose width:     mid(50,150)
   100.f,  // [10] Nose length:    mid(70,130)
   103.f,  // [11] Nose depth:     mid(90,115)
   105.f,  // [12] Nose tip:       mid(60,150)
   100.f,  // [13] Cheekbone:      mid(90,110)
    90.f,  // [14] Eyes depth:     mid(50,130)
   125.f,  // [15] Brow:           mid(50,200)
   100.f,  // [16] Brow height:    mid(85,115)
   100.f,  // [17] Jaw
    90.f,  // [18] Mouth width:    mid(40,140)
   100.f,  // [19] Mouth pos.:     mid(-50,250)
   100.f,  // [20] Lips:           mid(20,180)
    90.f,  // [21] Chin:           mid(50,130)
     0.f,  // [22] Eyes tilt:      0 = neutral (no tilt)
     0.f,  // [23] Nose pos.:      0 = neutral (no shift)
};

// HAIR sliders
static const char* const kHairLbl[HAIR_N] = {
    "Colour",   "Grow amount",  "2d lightness",  "Saturation",  "Brightness"
};
static const float kHairDef[HAIR_N] = { 40, 40, 40, 100, 50 };

// ── Race data ─────────────────────────────────────────────────────────────────
// stat_bonus[7]: Athletics / Strength / Toughness / Melee / Ranged / Thievery / Perception
struct KRace {
    const char* name;
    const char* subrace;   // nullptr = no subrace display
    uint8_t     race_idx;  // maps to RaceType enum
    const char* desc;
    int8_t      stat_bonus[7];
};

static const KRace kRaces[] = {
    { "Human", "Greenlander", 0,
      "Greenlanders tend to value money above all else. They make natural traders and adaptable "
      "mercenaries, comfortable in most professions. Despite lacking the raw power of other races, "
      "they compensate through versatility, resourcefulness and an eye for opportunity.",
      { 0, 0, 0, 0, 0, 0, 0 } },

    { "Human", "Scorchlander", 1,
      "Primarily from outcast cultures, Scorchlanders are fierce and hardened desert survivors. "
      "They have a flair for the dramatic and excel at stealth and perception, making excellent "
      "scouts, thieves and wanderers across the harshest regions of the land.",
      { 0, 0, 0, 0, 0, 5, 10 } },

    { "Grahl", nullptr, 2,
      "Hulking warriors shaped by generations of harsh living. Grahl are renowned for their "
      "physical dominance and a code of honour that prizes strength above all else. Blunt in "
      "speech and direct in action, they are poor infiltrators but devastating in open combat.",
      { 0, 20, 15, 0, 0, -15, -5 } },

    { "Keth", "Drone", 3,
      "Small colony-born drones of the Keth swarm. Physically slight but extraordinarily nimble, "
      "they move where larger beings cannot and perceive threats before others notice them. "
      "They thrive in support roles — scouts, thieves, and survivalists in any terrain.",
      { 15, -20, 0, 0, 0, 20, 0 } },

    { "Keth", "Guard", 4,
      "Keth Guards are the armoured fighting caste of the swarm, bred for front-line endurance. "
      "Tougher than their drone kin and trained from birth in coordinated combat, they form "
      "the backbone of Keth military strength without sacrificing their innate quickness.",
      { 0, 0, 10, 10, 0, 0, 0 } },

    { "Wrought", nullptr, 5,
      "Autonomous constructs of forgotten manufacture, built from alloys that no living smith "
      "can reproduce. They need no sustenance, no rest, and shrug off wounds that would kill "
      "flesh-and-blood beings — but only an engineer's tools can restore what battle damages.",
      { 0, 10, 0, 0, 0, 0, 0 } },
};
static constexpr int RACE_COUNT = (int)(sizeof(kRaces) / sizeof(kRaces[0]));

static const char* const kStatNames[7] = {
    "Athletics", "Strength", "Toughness", "Melee Atk", "Ranged", "Thievery", "Perception"
};

// ── Data struct ───────────────────────────────────────────────────────────────
struct Def {
    char    name[32]       = {};
    uint8_t sex            = 0;   // 0=Male, 1=Female
    uint8_t race_row       = 0;   // index into kRaces[]
    float   body[BODY_N];         // BODY morphs (0–100 scale)
    float   face[FACE_N];         // FACE morphs
    float   hair_f[HAIR_N];       // HAIR params
    float   skin_rgb[3]    = { 0.82f, 0.65f, 0.52f };
    float   hair_rgb[3]    = { 0.18f, 0.12f, 0.08f };
    float   color_strength = 0.55f;

    Def() {
        memset(name, 0, sizeof(name));
        strncpy(name, "Player", 7);
        for (int i = 0; i < BODY_N; ++i) body[i] = kBodyDef[i];
        for (int i = 0; i < FACE_N; ++i) face[i] = kFaceDef[i];
        for (int i = 0; i < HAIR_N; ++i) hair_f[i] = kHairDef[i];
    }

    // Derived parameters for renderer.
    // All body neutrals = kBodyDef[i] (midpoint of Kenshi range); scale=1.0 at neutral.
    float skintone_sat() const {
        float s = hair_f[3] / 100.f;   // Saturation 0-200 → 0.0-2.0
        return s < 0.f ? 0.f : (s > 2.5f ? 2.5f : s);
    }
    float skintone_bri() const {
        float b = (hair_f[4] / 100.f - 0.5f) * 0.8f;  // Brightness 0-100 → -0.4..+0.4
        return b < -0.5f ? -0.5f : (b > 0.5f ? 0.5f : b);
    }

    float eff_height() const {
        float h = body[2] / 100.f;                      // Height neutral=100
        h += (body[7] / 100.f - 1.0f) * 0.07f;         // Leg length  ±0.07
        h += (body[4] / 35.f  - 1.0f) * 0.03f;         // Posture: neutral=35 (mid 0-70) ±0.03
        return h < 0.55f ? 0.55f : (h > 1.45f ? 1.45f : h);
    }
    float eff_frame() const {
        float b = body[3] / 100.f;                      // Frame neutral=100
        b += (body[8]  / 100.f  - 1.0f) * 0.11f;       // Shoulders   ±0.11 neutral=100
        b += (body[9]  / 100.f  - 1.0f) * 0.05f;        // Arm bulk    ±0.05 neutral=100 (RE)
        b += (body[12] / 100.f  - 1.0f) * 0.05f;       // Chest       ±0.05 neutral=100
        b += (body[15] / 100.f  - 1.0f) * 0.04f;       // Hips        ±0.04 neutral=100
        return b < 0.55f ? 0.55f : (b > 1.55f ? 1.55f : b);
    }
    float muscular() const {
        float arm = body[9]  / 100.f;    // Arm bulk neutral=100 (RE)
        float stm = body[13] / 100.f;    // Stomach neutral=100 (mid=100 per cfg)
        float m = arm * 0.5f + stm * 0.5f - 0.8f;
        return m < 0.f ? 0.f : (m > 1.f ? 1.f : m);
    }
};

static Def   s_def;
static char  s_path[256] = "game/data/chars/player.chardef";
static int   s_tab       = 0;   // 0=BODY 1=FACE 2=HAIR

// ── JSON save/load ────────────────────────────────────────────────────────────
static float s_parse_f(const char* b, const char* k) {
    const char* p = strstr(b, k); if (!p) return 0.f;
    p = strchr(p, ':'); return p ? (float)atof(p + 1) : 0.f;
}
static int s_parse_i(const char* b, const char* k) {
    const char* p = strstr(b, k); if (!p) return 0;
    p = strchr(p, ':'); return p ? atoi(p + 1) : 0;
}
static void s_parse_str(const char* b, const char* k, char* out, int sz) {
    const char* p = strstr(b, k); if (!p) { out[0]='\0'; return; }
    p = strchr(p, ':'); if (!p) { out[0]='\0'; return; }
    while (*p && (*p==':'||*p==' '||*p=='"')) ++p;
    int n=0; while (*p && *p!='"' && n<sz-1) out[n++]=*p++;
    out[n]='\0';
}
static void LoadMorphNames(const char* /*path*/) {}  // names are hardcoded; file ignored
static bool LoadJSON(const char* path) {
    FILE* f = fopen(path, "r"); if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16384) { fclose(f); return false; }
    char* buf = (char*)malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f); buf[sz] = '\0'; fclose(f);

    s_parse_str(buf, "\"name\"", s_def.name, 32);
    s_def.sex      = (uint8_t)s_parse_i(buf, "\"sex\"");
    s_def.race_row = (uint8_t)s_parse_i(buf, "\"race_row\"");
    if (s_def.race_row >= RACE_COUNT) s_def.race_row = 0;
    s_def.skin_rgb[0]    = s_parse_f(buf, "\"skin_r\"");
    s_def.skin_rgb[1]    = s_parse_f(buf, "\"skin_g\"");
    s_def.skin_rgb[2]    = s_parse_f(buf, "\"skin_b\"");
    s_def.hair_rgb[0]    = s_parse_f(buf, "\"hair_r\"");
    s_def.hair_rgb[1]    = s_parse_f(buf, "\"hair_g\"");
    s_def.hair_rgb[2]    = s_parse_f(buf, "\"hair_b\"");
    s_def.color_strength = s_parse_f(buf, "\"color_str\"");
    if (s_def.color_strength < 0.01f) s_def.color_strength = 0.55f;

    auto load_arr = [&](const char* key, float* arr, int n, const float* defs) {
        const char* p = strstr(buf, key);
        if (p) { p = strchr(p, '['); if (p) { ++p;
            for (int i = 0; i < n; ++i) {
                while (*p && (*p==' '||*p==',')) ++p;
                if (!*p || *p == ']') break;
                arr[i] = (float)atof(p);
                while (*p && *p!=',' && *p!=']') ++p;
            }
        }} else { for (int i=0;i<n;++i) arr[i]=defs[i]; }
    };
    load_arr("\"body\"",   s_def.body,   BODY_N, kBodyDef);
    load_arr("\"face\"",   s_def.face,   FACE_N, kFaceDef);
    load_arr("\"hair_f\"", s_def.hair_f, HAIR_N, kHairDef);

    // Clamp loaded values to valid Kenshi ranges; resets stale saves to neutral
    for (int i = 0; i < BODY_N; ++i)
        if (s_def.body[i] < kBodyLo[i] || s_def.body[i] > kBodyHi[i])
            s_def.body[i] = kBodyDef[i];
    for (int i = 0; i < FACE_N; ++i)
        if (s_def.face[i] < kFaceLo[i] || s_def.face[i] > kFaceHi[i])
            s_def.face[i] = kFaceDef[i];

    free(buf); return true;
}
static bool SaveJSON(const char* path) {
    FILE* f = fopen(path, "w"); if (!f) return false;
    fprintf(f, "{\n  \"name\": \"%s\",\n  \"sex\": %d,\n  \"race_row\": %d,\n",
            s_def.name, s_def.sex, s_def.race_row);
    fprintf(f, "  \"skin_r\": %.4f, \"skin_g\": %.4f, \"skin_b\": %.4f,\n",
            s_def.skin_rgb[0], s_def.skin_rgb[1], s_def.skin_rgb[2]);
    fprintf(f, "  \"hair_r\": %.4f, \"hair_g\": %.4f, \"hair_b\": %.4f,\n",
            s_def.hair_rgb[0], s_def.hair_rgb[1], s_def.hair_rgb[2]);
    fprintf(f, "  \"color_str\": %.4f,\n", s_def.color_strength);
    auto save_arr = [&](const char* key, const float* arr, int n) {
        fprintf(f, "  \"%s\": [", key);
        for (int i=0;i<n;++i) fprintf(f, "%s%.2f", i?",":"", arr[i]);
        fprintf(f, "]%s\n", n<HAIR_N?",":" ");
    };
    save_arr("body",   s_def.body,   BODY_N);
    save_arr("face",   s_def.face,   FACE_N);
    save_arr("hair_f", s_def.hair_f, HAIR_N);
    fprintf(f, "}\n"); fclose(f); return true;
}

// ── Kenshi color theme ────────────────────────────────────────────────────────
static constexpr int K_THEME_N = 28;
static void PushKenshiTheme() {
    ImGui::PushStyleColor(ImGuiCol_WindowBg,          {0.094f,0.071f,0.043f,1.f}); //  1
    ImGui::PushStyleColor(ImGuiCol_ChildBg,           {0.110f,0.086f,0.055f,1.f}); //  2
    ImGui::PushStyleColor(ImGuiCol_PopupBg,           {0.125f,0.098f,0.063f,1.f}); //  3
    ImGui::PushStyleColor(ImGuiCol_Border,            {0.353f,0.271f,0.165f,1.f}); //  4
    ImGui::PushStyleColor(ImGuiCol_FrameBg,           {0.150f,0.118f,0.071f,1.f}); //  5
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,    {0.220f,0.173f,0.102f,1.f}); //  6
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,     {0.290f,0.224f,0.133f,1.f}); //  7
    ImGui::PushStyleColor(ImGuiCol_TitleBg,           {0.094f,0.071f,0.043f,1.f}); //  8
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,     {0.150f,0.118f,0.071f,1.f}); //  9
    ImGui::PushStyleColor(ImGuiCol_Button,            {0.235f,0.188f,0.114f,1.f}); // 10
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,     {0.361f,0.290f,0.173f,1.f}); // 11
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,      {0.490f,0.392f,0.235f,1.f}); // 12
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,        {0.647f,0.510f,0.251f,1.f}); // 13
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,  {0.796f,0.639f,0.345f,1.f}); // 14
    ImGui::PushStyleColor(ImGuiCol_Text,              {0.847f,0.780f,0.627f,1.f}); // 15
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,      {0.455f,0.380f,0.275f,1.f}); // 16
    ImGui::PushStyleColor(ImGuiCol_Separator,         {0.353f,0.271f,0.165f,1.f}); // 17
    ImGui::PushStyleColor(ImGuiCol_Header,            {0.290f,0.224f,0.133f,1.f}); // 18
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,     {0.388f,0.310f,0.184f,1.f}); // 19
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,      {0.490f,0.392f,0.235f,1.f}); // 20
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,       {0.094f,0.071f,0.043f,1.f}); // 21
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,     {0.353f,0.271f,0.165f,1.f}); // 22
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered,{0.490f,0.392f,0.235f,1.f}); // 23
    ImGui::PushStyleColor(ImGuiCol_Tab,               {0.196f,0.157f,0.094f,1.f}); // 24
    ImGui::PushStyleColor(ImGuiCol_TabHovered,        {0.388f,0.310f,0.184f,1.f}); // 25
    ImGui::PushStyleColor(ImGuiCol_TabActive,         {0.549f,0.431f,0.251f,1.f}); // 26
    ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive,{0.451f,0.353f,0.208f,1.f}); // 27
    ImGui::PushStyleColor(ImGuiCol_CheckMark,         {0.796f,0.639f,0.345f,1.f}); // 28
}
static void PopKenshiTheme() { ImGui::PopStyleColor(K_THEME_N); }

// ── Kenshi-style slider: [Label           ] [---●-------] [80] ─────────────
static bool KenshiSlider(const char* lbl, float* v, float lo, float hi) {
    ImGui::PushID(lbl);
    bool changed = false;
    float avail  = ImGui::GetContentRegionAvail().x;
    float lbl_w  = 96.f;
    float val_w  = 30.f;
    float spc    = ImGui::GetStyle().ItemSpacing.x;
    float sld_w  = avail - lbl_w - val_w - spc * 2.f;
    if (sld_w < 40.f) sld_w = 40.f;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(lbl);
    ImGui::SameLine(lbl_w, 0.f);
    ImGui::SetNextItemWidth(sld_w);
    changed = ImGui::SliderFloat("##s", v, lo, hi, "");
    ImGui::SameLine(0.f, 4.f);
    ImGui::SetNextItemWidth(val_w);

    // Right-aligned integer value in dimmed colour
    char vbuf[8]; snprintf(vbuf, sizeof(vbuf), "%d", (int)*v);
    float vw = ImGui::CalcTextSize(vbuf).x;
    float ox = val_w - vw;
    if (ox > 0.f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ox);
    ImGui::TextDisabled("%s", vbuf);

    ImGui::PopID();
    return changed;
}

// ── Arrow + centred label navigator (RACE / GENDER etc.) ─────────────────────
static void NavRow(const char* label_id, const char* val,
                   bool can_left, bool can_right,
                   bool* pressed_left, bool* pressed_right) {
    float aw  = ImGui::GetContentRegionAvail().x;
    float bw  = 18.f;
    float pad = ImGui::GetStyle().WindowPadding.x;
    float ts  = ImGui::CalcTextSize(val).x;

    char lid[48], rid[48];
    snprintf(lid, sizeof(lid), "<##nl_%s", label_id);
    snprintf(rid, sizeof(rid), ">##nr_%s", label_id);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {1.f, 2.f});

    // < button at left edge
    if (!can_left) ImGui::BeginDisabled();
    *pressed_left = ImGui::Button(lid, {bw, 0.f});
    if (!can_left) ImGui::EndDisabled();

    // Text: absolutely centred on the SAME ROW as the buttons
    ImGui::SameLine(0.f, 0.f);
    float center_x = pad + aw * 0.5f;
    float text_x   = center_x - ts * 0.5f;
    float min_x    = pad + bw + 2.f;
    ImGui::SetCursorPosX(text_x > min_x ? text_x : min_x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(val);

    // > button pinned to right edge
    ImGui::SameLine(pad + aw - bw);
    if (!can_right) ImGui::BeginDisabled();
    *pressed_right = ImGui::Button(rid, {bw, 0.f});
    if (!can_right) ImGui::EndDisabled();

    ImGui::PopStyleVar();
}

// ── Stat bar (green/red, centred zero) ──────────────────────────────────────
static void StatBar(const char* stat_name, int val) {
    float avail = ImGui::GetContentRegionAvail().x;
    float lbl_w = 76.f;
    float num_w = 26.f;
    float bar_w = avail - lbl_w - num_w - ImGui::GetStyle().ItemSpacing.x * 2.f;
    if (bar_w < 20.f) bar_w = 20.f;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(stat_name);
    ImGui::SameLine(lbl_w, 0.f);

    float bar_h = ImGui::GetTextLineHeight() * 0.65f;
    float pad_y = (ImGui::GetFrameHeight() - bar_h) * 0.5f;
    ImVec2 p    = { ImGui::GetCursorScreenPos().x,
                    ImGui::GetCursorScreenPos().y + pad_y };
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(p, {p.x + bar_w, p.y + bar_h}, IM_COL32(40,30,18,255), 2.f);
    // Filled portion (centred at 0)
    if (val != 0) {
        float cx    = p.x + bar_w * 0.5f;
        float fill  = fabsf((float)val) / 30.f * (bar_w * 0.5f);
        if (fill > bar_w * 0.5f) fill = bar_w * 0.5f;
        ImU32 col   = val > 0 ? IM_COL32(55,155,55,255) : IM_COL32(165,50,50,255);
        if (val > 0)
            dl->AddRectFilled({cx, p.y}, {cx + fill, p.y + bar_h}, col, 2.f);
        else
            dl->AddRectFilled({cx - fill, p.y}, {cx, p.y + bar_h}, col, 2.f);
    }
    // Centre line
    float cx = p.x + bar_w * 0.5f;
    dl->AddLine({cx, p.y}, {cx, p.y + bar_h}, IM_COL32(90,70,40,220));

    ImGui::Dummy({bar_w, ImGui::GetFrameHeight()});
    ImGui::SameLine(0.f, 4.f);

    if (val > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, {0.40f, 0.85f, 0.40f, 1.f});
        ImGui::Text("+%d", val);
        ImGui::PopStyleColor();
    } else if (val < 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, {0.85f, 0.38f, 0.38f, 1.f});
        ImGui::Text("%d", val);
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled(" 0");
    }
}

// ── Main Draw ─────────────────────────────────────────────────────────────────
static void Draw() {
    PushKenshiTheme();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {4.f, 4.f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   {4.f, 3.f});

    const float total_w  = ImGui::GetContentRegionAvail().x;
    const float total_h  = ImGui::GetContentRegionAvail().y;
    const float left_w   = 160.f;
    const float right_w  = 270.f;
    const float spc      = ImGui::GetStyle().ItemSpacing.x;
    float center_w = total_w - left_w - right_w - spc * 2.f;
    if (center_w < 40.f) center_w = 40.f;

    const KRace& kr = kRaces[s_def.race_row];

    // ═══════════════════════════════════════════════════════════════
    // LEFT PANEL
    // ═══════════════════════════════════════════════════════════════
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.090f,0.070f,0.043f,1.f});
    ImGui::BeginChild("##cc_left", {left_w, total_h}, true,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    // Name field
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputText("##cc_name", s_def.name, sizeof(s_def.name));
    ImGui::Spacing();

    // ── Race navigation ──────────────────────────────────────────
    ImGui::TextDisabled("RACE");
    bool pl = false, pr = false;
    // Collect unique race group names by "name" field
    // Navigate by cycling kRaces[] rows
    NavRow("race", kr.name, true, true, &pl, &pr);
    if (pl) s_def.race_row = (uint8_t)((s_def.race_row + RACE_COUNT - 1) % RACE_COUNT);
    if (pr) s_def.race_row = (uint8_t)((s_def.race_row + 1) % RACE_COUNT);

    // ── Subrace (shown only when race has a subrace) ─────────────
    ImGui::TextDisabled("SUBRACE");
    const char* sub_val = kr.subrace ? kr.subrace : "—";
    ImGui::PushStyleColor(ImGuiCol_Text, kr.subrace
        ? ImVec4{0.847f,0.780f,0.627f,1.f}
        : ImVec4{0.455f,0.380f,0.275f,1.f});
    {
        float px = (left_w - 8.f - ImGui::CalcTextSize(sub_val).x) * 0.5f;
        if (px > 0.f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(sub_val);
    }
    ImGui::PopStyleColor();

    // ── Gender navigation ────────────────────────────────────────
    static const char* kGender[2] = { "Male", "Female" };
    ImGui::TextDisabled("GENDER");
    bool gl = false, gr = false;
    NavRow("gen", kGender[s_def.sex], true, true, &gl, &gr);
    if (gl || gr) s_def.sex ^= 1;

    ImGui::Spacing();

    // Import / Export
    {
        float hw = (ImGui::GetContentRegionAvail().x - spc) * 0.5f;
        if (ImGui::Button("IMPORT##cc", {hw, 0.f})) LoadJSON(s_path);
        ImGui::SameLine(0.f, spc);
        if (ImGui::Button("EXPORT##cc", {hw, 0.f})) SaveJSON(s_path);
    }

    // Clothes toggle
    {
        static bool s_clothes = true;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.f, 2.f});
        if (s_clothes) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.45f,0.35f,0.20f,1.f});
        if (ImGui::Button(s_clothes ? u8"● CLOTHES" : "  CLOTHES",
                          {ImGui::GetContentRegionAvail().x, 0.f}))
            s_clothes = !s_clothes;
        if (s_clothes) ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    ImGui::Spacing();

    // Character navigation (single char; UI-complete for future multi-char)
    ImGui::TextDisabled("CHANGE CHARACTER");
    {
        bool dummy_l = false, dummy_r = false;
        NavRow("char", "1 / 1", false, false, &dummy_l, &dummy_r);
    }

    ImGui::Separator();

    // ── Race description ─────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.647f,0.510f,0.251f,1.f});
    ImGui::TextUnformatted("RACE DESCRIPTION");
    ImGui::PopStyleColor();

    {
        float desc_h = total_h * 0.22f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.07f,0.055f,0.033f,1.f});
        ImGui::BeginChild("##rc_desc", {0.f, desc_h}, false);
        ImGui::PopStyleColor();
        ImGui::PushTextWrapPos(0.f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.73f,0.66f,0.51f,1.f});
        ImGui::TextWrapped("%s", kr.desc);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
    }

    ImGui::Separator();

    // ── Race stats ───────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.647f,0.510f,0.251f,1.f});
    ImGui::TextUnformatted("RACE STATS");
    ImGui::PopStyleColor();

    {
        float stats_h = total_h - ImGui::GetCursorPosY() - 4.f;
        if (stats_h < 60.f) stats_h = 60.f;
        ImGui::BeginChild("##rc_stats", {0.f, stats_h}, false);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.f, 1.f});
        for (int i = 0; i < 7; ++i)
            StatBar(kStatNames[i], (int)kr.stat_bonus[i]);
        ImGui::PopStyleVar();
        ImGui::EndChild();
    }

    ImGui::EndChild();  // left

    // ═══════════════════════════════════════════════════════════════
    // CENTER: 3D PREVIEW
    // ═══════════════════════════════════════════════════════════════
    ImGui::SameLine(0.f, spc);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.055f,0.043f,0.027f,1.f});
    ImGui::BeginChild("##cc_center", {center_w, total_h}, false);
    ImGui::PopStyleColor();

    {
        static int s_prev_sex = -1;
        if (s_prev_sex != (int)s_def.sex) {
            s_prev_sex = (int)s_def.sex;
            const char* glb = (s_def.sex == 0)
                ? "game/data/props/md_human_t.glb"
                : "game/data/props/md_human_female_t.glb";
            const char* tex = (s_def.sex == 0)
                ? "game/data/textures/md_human_body.png"
                : "game/data/textures/md_human_female_body.png";
#ifdef MD_SDL_GPU
            CharPreviewSDLGPU::Init(glb, tex);
#else
            CharPreviewGL::Init(glb, tex);
#endif
        }
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
#ifdef MD_SDL_GPU
    CharPreviewSDLGPU::SetBoneScalesFromDef(s_def.body, s_def.face);
    CharPreviewSDLGPU::SetMorphWeightsFromFace(s_def.face, kFaceDef, kFaceLo, kFaceHi);
    CharPreviewSDLGPU::SetBodyMorphWeights(s_def.body, s_def.face);
    CharPreviewSDLGPU::DrawInImGui(
        avail.x, avail.y,
        s_def.eff_height(), s_def.eff_frame(),
        s_def.skin_rgb, s_def.color_strength,
        s_def.skintone_sat(), s_def.skintone_bri(),
        s_def.muscular(), s_def.hair_rgb);
#else
    CharPreviewGL::DrawInImGui(
        avail.x, avail.y,
        s_def.eff_height(), s_def.eff_frame(),
        s_def.skin_rgb, s_def.color_strength,
        s_def.skintone_sat(), s_def.skintone_bri());
#endif

    ImGui::EndChild();  // center

    // ═══════════════════════════════════════════════════════════════
    // RIGHT PANEL: BODY / FACE / HAIR
    // ═══════════════════════════════════════════════════════════════
    ImGui::SameLine(0.f, spc);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.090f,0.070f,0.043f,1.f});
    ImGui::BeginChild("##cc_right", {right_w, total_h}, true,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    // ── BODY / FACE / HAIR tab buttons ───────────────────────────
    {
        float bw3 = (ImGui::GetContentRegionAvail().x - spc * 2.f) / 3.f;
        static const char* kTabLbl[3] = { "BODY", "FACE", "HAIR" };
        for (int t = 0; t < 3; ++t) {
            if (t > 0) ImGui::SameLine(0.f, spc);
            bool active = (s_tab == t);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.549f,0.431f,0.251f,1.f});
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {2.f, 4.f});
            if (ImGui::Button(kTabLbl[t], {bw3, 0.f})) {
                if (s_tab != t) {
                    s_tab = t;
#ifdef MD_SDL_GPU
                    CharPreviewSDLGPU::SetCameraForTab(t);
#endif
                }
            }
            ImGui::PopStyleVar();
            if (active) ImGui::PopStyleColor();
        }
    }
    ImGui::Separator();

    // ── Slider list (scrollable) ─────────────────────────────────
    const float bottom_h = ImGui::GetFrameHeightWithSpacing() + 10.f;
    ImGui::BeginChild("##sliders", {0.f, -bottom_h}, false);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.f, 2.f});

    if (s_tab == 0) {
        // Skin colour picker at top (matches Kenshi BODY tab)
        ImGui::TextDisabled("Skin Colour");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::ColorEdit3("##scol2", s_def.skin_rgb);
        ImGui::Spacing();
        // Body morphs: skip body[0..1] (skin params), skip Breast size for Male
        for (int i = 2; i < BODY_N; ++i) {
            if (i == 14 && s_def.sex == 0) continue;
            KenshiSlider(kBodyLbl[i], &s_def.body[i], kBodyLo[i], kBodyHi[i]);
        }
    } else if (s_tab == 1) {
        for (int i = 0; i < FACE_N; ++i)
            KenshiSlider(kFaceLbl[i], &s_def.face[i], kFaceLo[i], kFaceHi[i]);
    } else {
        // HAIR tab: colour params + extra sliders
        for (int i = 0; i < 3; ++i)
            KenshiSlider(kHairLbl[i], &s_def.hair_f[i], 0.f, 100.f);
        ImGui::Spacing();
        ImGui::TextDisabled("Hair Colour");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::ColorEdit3("##hcol", s_def.hair_rgb);
        ImGui::Spacing();
        ImGui::TextDisabled("Skin Colour");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::ColorEdit3("##scol", s_def.skin_rgb);
        ImGui::Spacing();
        // Saturation / Brightness map to hair_f[3/4]
        KenshiSlider("Saturation", &s_def.hair_f[3], 0.f, 200.f);
        KenshiSlider("Brightness", &s_def.hair_f[4], 0.f, 100.f);
    }

    ImGui::PopStyleVar();
    ImGui::EndChild();

    // ── Bottom: RAND / RAND ALL / RESET ALL ─────────────────────
    ImGui::Separator();
    {
        float bw3 = (ImGui::GetContentRegionAvail().x - spc * 2.f) / 3.f;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {2.f, 3.f});

        // Kenshi-style: bias toward centre of range (±1/3 of range from neutral),
        // not pure uniform random — prevents all-extreme values simultaneously.
        auto randDef = [](float lo, float hi, float def) -> float {
            // 80% chance within [def - third, def + third], 20% full range
            float third = (hi - lo) * 0.33f;
            float r01 = (float)(rand() % 1000) / 999.f;
            if (r01 < 0.80f) {
                float a = def - third > lo ? def - third : lo;
                float b = def + third < hi ? def + third : hi;
                int rng = (int)(b - a); if (rng < 1) rng = 1;
                return a + (float)(rand() % rng);
            }
            int rng = (int)(hi - lo); if (rng < 1) rng = 1;
            return lo + (float)(rand() % rng);
        };
        if (ImGui::Button("RAND##cc", {bw3, 0.f})) {
            if (s_tab == 0) { for (int i = 2; i < BODY_N; ++i) s_def.body[i] = randDef(kBodyLo[i], kBodyHi[i], kBodyDef[i]); }
            if (s_tab == 1) { for (int i = 0; i < FACE_N; ++i) s_def.face[i] = randDef(kFaceLo[i], kFaceHi[i], kFaceDef[i]); }
        }
        ImGui::SameLine(0.f, spc);
        if (ImGui::Button("RAND ALL##cc", {bw3, 0.f})) {
            for (int i = 2; i < BODY_N; ++i) s_def.body[i] = randDef(kBodyLo[i], kBodyHi[i], kBodyDef[i]);
            for (int i = 0; i < FACE_N; ++i) s_def.face[i] = randDef(kFaceLo[i], kFaceHi[i], kFaceDef[i]);
            s_def.skin_rgb[0] = 0.35f + (rand()%100)/100.f * 0.55f;
            s_def.skin_rgb[1] = 0.25f + (rand()%100)/100.f * 0.45f;
            s_def.skin_rgb[2] = 0.15f + (rand()%100)/100.f * 0.35f;
        }
        ImGui::SameLine(0.f, spc);
        if (ImGui::Button("RESET##cc", {bw3, 0.f})) { s_def = Def{}; }

        ImGui::PopStyleVar();
    }

    ImGui::EndChild();  // right

    ImGui::PopStyleVar(2);
    PopKenshiTheme();
}

} // namespace CharacterEditor
