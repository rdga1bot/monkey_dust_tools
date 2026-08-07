#pragma once
// Data section of character_editor.h's split — window state, morph slider
// tables, race data, and the Def save/load struct. Included ONLY from
// character_editor.h, inside namespace CharacterEditor { ... }.

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
    99.f,  // [4] Posture      (cfg: Posture=99 — Kenshi male_editor.cfg all races)
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
// Defaults: midpoints from editor_data_human.xml (Kenshi loadEditorXMLData).
// Posture(0-70)→mid=35, ShoulderSet(45-90)→mid=68, NeckPos(25-80)→mid=53.
static const float kBodyDef[BODY_N] = {
    50.f,   // [0] skin param
    50.f,   // [1] skin param
   100.f,   // [2] Height
   100.f,   // [3] Frame
    35.f,   // [4] Posture:      mid(0,70)=35 per editor_data_human.xml
    68.f,   // [5] Shoulder set: mid(45,90)=68
    53.f,   // [6] Neck position:mid(25,80)=53
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
  -100.f,   // [22] Eyes tilt      → tiltup_eyes/tiltdown_eyes
  -165.f,   // [23] Nose pos.      → high_nose  (cfg: Nose height lo reused)
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
    const char* subrace;       // nullptr = no subrace display
    uint8_t     race_idx;      // maps to RaceType enum
    const char* desc;
    int8_t      stat_bonus[7];
    bool        single_gender; // RE: single_gender_flag @0x7b — constructs/robots have no sex
    bool        hide_hair;     // RE: "hide hair" string key — insectoids/robots have no hair
};

static const KRace kRaces[] = {
    // name         subrace          idx  desc                                            stats                        sg     hide_hair
    { "Human", "Greenlander", 0,
      "Greenlanders tend to value money above all else. They make natural traders and adaptable "
      "mercenaries, comfortable in most professions. Despite lacking the raw power of other races, "
      "they compensate through versatility, resourcefulness and an eye for opportunity.",
      { 0, 0, 0, 0, 0, 0, 0 }, false, false },

    { "Human", "Scorchlander", 1,
      "Primarily from outcast cultures, Scorchlanders are fierce and hardened desert survivors. "
      "They have a flair for the dramatic and excel at stealth and perception, making excellent "
      "scouts, thieves and wanderers across the harshest regions of the land.",
      { 0, 0, 0, 0, 0, 5, 10 }, false, false },

    { "Grahl", nullptr, 2,
      "Hulking warriors shaped by generations of harsh living. Grahl are renowned for their "
      "physical dominance and a code of honour that prizes strength above all else. Blunt in "
      "speech and direct in action, they are poor infiltrators but devastating in open combat.",
      { 0, 20, 15, 0, 0, -15, -5 }, false, false },

    { "Keth", "Drone", 3,
      "Small colony-born drones of the Keth swarm. Physically slight but extraordinarily nimble, "
      "they move where larger beings cannot and perceive threats before others notice them. "
      "They thrive in support roles — scouts, thieves, and survivalists in any terrain.",
      // RE: "hide hair" key — Keth are insectoid, no hair geometry
      { 15, -20, 0, 0, 0, 20, 0 }, false, true },

    { "Keth", "Guard", 4,
      "Keth Guards are the armoured fighting caste of the swarm, bred for front-line endurance. "
      "Tougher than their drone kin and trained from birth in coordinated combat, they form "
      "the backbone of Keth military strength without sacrificing their innate quickness.",
      { 0, 0, 10, 10, 0, 0, 0 }, false, true },

    { "Wrought", nullptr, 5,
      "Autonomous constructs of forgotten manufacture, built from alloys that no living smith "
      "can reproduce. They need no sustenance, no rest, and shrug off wounds that would kill "
      "flesh-and-blood beings — but only an engineer's tools can restore what battle damages.",
      // RE: single_gender_flag @0x7b (type 0x5b = robot/skeleton); "hide hair" = no hair geometry
      { 0, 10, 0, 0, 0, 0, 0 }, true, true },
};
static constexpr int RACE_COUNT = (int)(sizeof(kRaces) / sizeof(kRaces[0]));

static const char* const kStatNames[7] = {
    "Athletics", "Strength", "Toughness", "Melee Atk", "Ranged", "Thievery", "Perception"
};

// ── Data struct ───────────────────────────────────────────────────────────────
// KEN-ATTR-1: free attribute points at char creation (Kenshi RE: char+0x198).
static constexpr int ATTR_FREE_TOTAL = 15;  // points to distribute

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
    // KEN-ATTR-1: points spent per stat at char creation (indices match kStatNames[7])
    int8_t  attr_spent[7]  = {};

    Def() {
        memset(name, 0, sizeof(name));
        strncpy(name, "Player", 7);
        for (int i = 0; i < BODY_N; ++i) body[i] = kBodyDef[i];
        for (int i = 0; i < FACE_N; ++i) face[i] = kFaceDef[i];
        for (int i = 0; i < HAIR_N; ++i) hair_f[i] = kHairDef[i];
        for (int i = 0; i < 7; ++i) attr_spent[i] = 0;
    }

    int AttrRemaining() const {
        int spent = 0;
        for (int i = 0; i < 7; ++i) spent += attr_spent[i];
        return ATTR_FREE_TOTAL - spent;
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
