#pragma once
// Main Draw() section of character_editor.h's split — the 3-panel character
// creator UI body. Included ONLY from character_editor.h, inside
// namespace CharacterEditor { ... }.

// ── Main Draw ─────────────────────────────────────────────────────────────────
static void Draw(bool kenshi_theme = true) {
    if (kenshi_theme) PushKenshiTheme();
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
    if (kenshi_theme) ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.090f,0.070f,0.043f,1.f});
    ImGui::BeginChild("##cc_left", {left_w, total_h}, true,
                      ImGuiWindowFlags_NoScrollbar);

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

    // ── Gender navigation ─────────────────────────────────────────
    // RE: single_gender_flag @0x7b — Wrought (type 0x5b) have no biological sex
    static const char* kGender[2] = { "Male", "Female" };
    ImGui::TextDisabled("GENDER");
    if (kr.single_gender) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.45f,0.38f,0.28f,1.f});
        ImGui::TextUnformatted("  N/A");
        ImGui::PopStyleColor();
        s_def.sex = 0;  // lock to Male skeleton for single-gender races
    } else {
        bool gl = false, gr = false;
        NavRow("gen", kGender[s_def.sex], true, true, &gl, &gr);
        if (gl || gr) s_def.sex ^= 1;
    }

    ImGui::Spacing();

    // Import / Export
    {
        static char s_io_msg[48] = {};
        static float s_io_msg_t = 0.f;
        float hw = (ImGui::GetContentRegionAvail().x - spc) * 0.5f;
        if (ImGui::Button("IMPORT##cc", {hw, 0.f})) {
            if (LoadJSON(s_path)) { strncpy(s_io_msg, "Loaded.", 47); }
            else                  { strncpy(s_io_msg, "Load FAILED.", 47); }
            s_io_msg_t = 2.5f;
        }
        ImGui::SameLine(0.f, spc);
        if (ImGui::Button("EXPORT##cc", {hw, 0.f})) {
            if (SaveJSON(s_path)) { strncpy(s_io_msg, "Saved.", 47); }
            else                  { strncpy(s_io_msg, "Save FAILED.", 47); }
            s_io_msg_t = 2.5f;
        }
        if (s_io_msg_t > 0.f) {
            s_io_msg_t -= ImGui::GetIO().DeltaTime;
            ImGui::TextDisabled("%s", s_io_msg);
        }
    }

    // Clothes toggle
    {
        bool& vis = CharPreviewSDLGPU::s_clothes_visible;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4.f, 2.f});
        bool vis_was = vis;
        if (vis_was) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.19f,0.35f,0.65f,1.f});
        if (ImGui::Button(vis ? u8"● CLOTHES" : "  CLOTHES",
                          {ImGui::GetContentRegionAvail().x, 0.f}))
            vis = !vis;
        if (vis_was) ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    // ── Clothing slot pickers (only when clothes visible) ────────────────
    if (CharPreviewSDLGPU::s_clothes_visible) {
        ImGui::Spacing();

        struct ClothItem { const char* label; int slot; int item_idx; };
        static const ClothItem kClothMenu[] = {
            // slot 0 — top
            {"None",         0,  0},
            {"Slave Shirt",  0,  1},
            {"Drifter Coat", 0,  2},
            {"Jacket",       0,  3},
            {"Male Coat",    0,  4},
            {"Monk Coat",    0,  5},
            {"Samurai Top",  0,  6},
            // slot 1 — bottom
            {"None",         1,  7},
            {"Drifter Pants",1,  8},
            {"Cargo Pants",  1,  9},
            {"Shorts",       1, 10},
            {"Trousers",     1, 11},
            {"Monk Pants",   1, 12},
            {"Samurai Bot",  1, 13},
            {"Slave Dress",  1, 14},
        };
        static constexpr int kMenuN = (int)(sizeof(kClothMenu)/sizeof(kClothMenu[0]));

        static const char* kSlotLabel[2] = {"Top", "Bottom"};
        for (int sl = 0; sl < 2; ++sl) {
            float lw = 44.f;
            ImGui::TextDisabled("%s", kSlotLabel[sl]);
            ImGui::SameLine(lw);

            // Current label
            int cur_item = CharPreviewSDLGPU::s_cloth_sel[sl];
            const char* cur_lbl = "None";
            for (int i = 0; i < kMenuN; ++i)
                if (kClothMenu[i].slot == sl && kClothMenu[i].item_idx == cur_item)
                    { cur_lbl = kClothMenu[i].label; break; }

            char cid[24]; snprintf(cid, sizeof(cid), "##csl%d", sl);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.f);
            if (ImGui::BeginCombo(cid, cur_lbl)) {
                for (int i = 0; i < kMenuN; ++i) {
                    if (kClothMenu[i].slot != sl) continue;
                    bool sel = (kClothMenu[i].item_idx == cur_item);
                    if (ImGui::Selectable(kClothMenu[i].label, sel))
                        CharPreviewSDLGPU::SetClothingItem(kClothMenu[i].item_idx);
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            // Color swatch for this slot
            ImGui::SameLine(0.f, 3.f);
            char ccid[24]; snprintf(ccid, sizeof(ccid), "##cc%d", sl);
            if (ImGui::ColorEdit3(ccid, CharPreviewSDLGPU::s_cloth_color[sl],
                    ImGuiColorEditFlags_NoLabel|ImGuiColorEditFlags_NoInputs|
                    ImGuiColorEditFlags_NoTooltip)) {
                // color changed — refresh slot with same item
                int idx = CharPreviewSDLGPU::s_cloth_sel[sl];
                CharPreviewSDLGPU::SetClothingItem(idx);
            }
        }
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
    if (kenshi_theme) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.647f,0.510f,0.251f,1.f});
    ImGui::TextUnformatted("RACE DESCRIPTION");
    if (kenshi_theme) ImGui::PopStyleColor();

    {
        float desc_h = total_h * 0.22f;
        if (kenshi_theme) ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.07f,0.055f,0.033f,1.f});
        ImGui::BeginChild("##rc_desc", {0.f, desc_h}, false);
        ImGui::PushTextWrapPos(0.f);
        if (kenshi_theme) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.73f,0.66f,0.51f,1.f});
        ImGui::TextWrapped("%s", kr.desc);
        if (kenshi_theme) ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        if (kenshi_theme) ImGui::PopStyleColor();  // ChildBg for rc_desc
    }

    ImGui::Separator();

    // ── Race stats ───────────────────────────────────────────────
    if (kenshi_theme) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.647f,0.510f,0.251f,1.f});
    ImGui::TextUnformatted("RACE STATS");
    if (kenshi_theme) ImGui::PopStyleColor();

    {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.f, 1.f});
        for (int i = 0; i < 7; ++i)
            StatBar(kStatNames[i], (int)kr.stat_bonus[i]);
        ImGui::PopStyleVar();
    }

    // RE: hunger_rate=0.02/tick, hunger_threshold=0.5 penalty trigger
    // Wrought: no hunger (constructs don't eat)
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.45f,0.38f,0.28f,1.f});
    if (kr.single_gender) {
        ImGui::TextDisabled("Hunger: none (construct)");
    } else {
        ImGui::TextDisabled("Hunger rate: 0.02/tick  |  threshold: 50%%");
    }
    ImGui::PopStyleColor();

    ImGui::Separator();

    // ── KEN-ATTR-1: Free attribute points ───────────────────────
    {
        int remaining = s_def.AttrRemaining();
        if (kenshi_theme) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.647f,0.510f,0.251f,1.f});
        ImGui::Text("FREE ATTRIBUTES [%d]", remaining);
        if (kenshi_theme) ImGui::PopStyleColor();

        float stats_h = total_h - ImGui::GetCursorPosY() - 4.f;
        if (stats_h < 80.f) stats_h = 80.f;
        ImGui::BeginChild("##attr_free", {0.f, stats_h}, false);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {2.f, 2.f});

        for (int i = 0; i < 7; ++i) {
            ImGui::AlignTextToFramePadding();
            // Label (fixed 64px)
            ImGui::TextUnformatted(kStatNames[i]);
            ImGui::SameLine(64.f, 0.f);

            char btn_minus[16], btn_plus[16];
            snprintf(btn_minus, sizeof(btn_minus), "-##am%d", i);
            snprintf(btn_plus,  sizeof(btn_plus),  "+##ap%d", i);

            // [−] button: disabled when nothing spent on this stat
            bool minus_dis = (s_def.attr_spent[i] <= 0);
            if (minus_dis) ImGui::BeginDisabled();
            if (ImGui::SmallButton(btn_minus)) s_def.attr_spent[i]--;
            if (minus_dis) ImGui::EndDisabled();

            ImGui::SameLine(0.f, 2.f);
            ImGui::Text("%2d", (int)s_def.attr_spent[i]);
            ImGui::SameLine(0.f, 2.f);

            // [+] button: disabled when no free points remain
            bool plus_dis = (remaining <= 0);
            if (plus_dis) ImGui::BeginDisabled();
            if (ImGui::SmallButton(btn_plus)) { s_def.attr_spent[i]++; remaining--; }
            if (plus_dis) ImGui::EndDisabled();
        }

        ImGui::PopStyleVar();
        ImGui::EndChild();
    }

    ImGui::EndChild();  // left
    if (kenshi_theme) ImGui::PopStyleColor();  // ChildBg for cc_left

    // ═══════════════════════════════════════════════════════════════
    // CENTER: 3D PREVIEW
    // ═══════════════════════════════════════════════════════════════
    ImGui::SameLine(0.f, spc);
    if (kenshi_theme) ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.055f,0.043f,0.027f,1.f});
    ImGui::BeginChild("##cc_center", {center_w, total_h}, false);

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
            // Reload all active clothing slots with correct sex variant.
            for (int sl = 0; sl < CharPreviewSDLGPU::CLOTH_MAX_SLOTS; ++sl) {
                int idx = CharPreviewSDLGPU::s_cloth_sel[sl];
                if (idx > 0) CharPreviewSDLGPU::SetClothingItem(idx);
            }
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
    if (kenshi_theme) ImGui::PopStyleColor();  // ChildBg for cc_center

    // ═══════════════════════════════════════════════════════════════
    // RIGHT PANEL: BODY / FACE / HAIR
    // ═══════════════════════════════════════════════════════════════
    ImGui::SameLine(0.f, spc);
    if (kenshi_theme) ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.090f,0.070f,0.043f,1.f});
    ImGui::BeginChild("##cc_right", {right_w, total_h}, true,
                      ImGuiWindowFlags_NoScrollbar);

    // ── BODY / FACE / HAIR tab buttons ───────────────────────────
    {
        float bw3 = (ImGui::GetContentRegionAvail().x - spc * 2.f) / 3.f;
        static const char* kTabLbl[3] = { "BODY", "FACE", "HAIR" };
        for (int t = 0; t < 3; ++t) {
            if (t > 0) ImGui::SameLine(0.f, spc);
            bool active = (s_tab == t);
            if (active && kenshi_theme) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.549f,0.431f,0.251f,1.f});
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
            if (active && kenshi_theme) ImGui::PopStyleColor();
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
        // ── HAIR tab ─────────────────────────────────────────────────────────
        // RE: "hide hair" string key — Keth + Wrought have no hair geometry
        if (kr.hide_hair) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.45f,0.38f,0.28f,1.f});
            ImGui::TextWrapped("This race has no hair.\n(RE: \"hide hair\" key)");
            ImGui::PopStyleColor();
        } else {
            // RE: "hair style" string key — hairstyle index
            ImGui::TextUnformatted("Hair Style");
            ImGui::SameLine();
            float avail_w = ImGui::GetContentRegionAvail().x;
            float btnW    = (avail_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button("< Prev", {btnW, 0})) {
                int ns = (CharPreviewSDLGPU::s_hair_style - 1 + CharPreviewSDLGPU::s_hair_style_count)
                          % CharPreviewSDLGPU::s_hair_style_count;
                CharPreviewSDLGPU::LoadHairStyle(ns);
            }
            ImGui::SameLine();
            if (ImGui::Button("Next >", {btnW, 0})) {
                int ns = (CharPreviewSDLGPU::s_hair_style + 1)
                          % CharPreviewSDLGPU::s_hair_style_count;
                CharPreviewSDLGPU::LoadHairStyle(ns);
            }
            ImGui::SetNextItemWidth(-1.f);
            int cur = CharPreviewSDLGPU::s_hair_style;
            if (ImGui::BeginCombo("##hstyle", CharPreviewSDLGPU::s_hair_styles[cur])) {
                for (int i = 0; i < CharPreviewSDLGPU::s_hair_style_count; ++i) {
                    bool sel = (i == cur);
                    if (ImGui::Selectable(CharPreviewSDLGPU::s_hair_styles[i], sel))
                        CharPreviewSDLGPU::LoadHairStyle(i);
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::Spacing();

            // RE: "Hair Color Red/Green/Blue" — stored as hair_rgb[3]
            ImGui::TextDisabled("Hair Colour  (R/G/B)");
            ImGui::SetNextItemWidth(-1.f);
            ImGui::ColorEdit3("##hcol", s_def.hair_rgb);
            ImGui::Spacing();

            // RE: "Hair Contrast" string key (was "2d lightness" in old label)
            KenshiSlider("Contrast",   &s_def.color_strength, 0.f, 1.f);
            // RE: "hair saturation" string key
            KenshiSlider("Saturation", &s_def.hair_f[3], 0.f, 200.f);
            // RE: "hair bright" string key
            KenshiSlider("Brightness", &s_def.hair_f[4], 0.f, 100.f);
            ImGui::Spacing();

            // Skin colour (shared between BODY and HAIR tabs in Kenshi)
            ImGui::TextDisabled("Skin Colour");
            ImGui::SetNextItemWidth(-1.f);
            ImGui::ColorEdit3("##scol", s_def.skin_rgb);
        }
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
            // 92% chance within [def - quarter, def + quarter], 8% full range
            float quarter = (hi - lo) * 0.25f;
            float r01 = (float)(rand() % 1000) / 999.f;
            if (r01 < 0.92f) {
                float a = def - quarter > lo ? def - quarter : lo;
                float b = def + quarter < hi ? def + quarter : hi;
                int rng = (int)(b - a); if (rng < 1) rng = 1;
                return a + (float)(rand() % rng);
            }
            int rng = (int)(hi - lo); if (rng < 1) rng = 1;
            return lo + (float)(rand() % rng);
        };
        if (ImGui::Button("RAND##cc", {bw3, 0.f})) {
            if (s_tab == 0) { for (int i = 2; i < BODY_N; ++i) s_def.body[i] = randDef(kBodyLo[i], kBodyHi[i], kBodyDef[i]); }
            if (s_tab == 1) { for (int i = 0; i < FACE_N; ++i) s_def.face[i] = randDef(kFaceLo[i], kFaceHi[i], kFaceDef[i]); }
            if (s_tab == 2) { for (int i = 0; i < 3; ++i) s_def.hair_f[i] = randDef(0.f, 100.f, kHairDef[i]); }
#ifdef MD_SDL_GPU
            CharPreviewSDLGPU::ResetAnimPhase();
#endif
        }
        ImGui::SameLine(0.f, spc);
        if (ImGui::Button("RAND ALL##cc", {bw3, 0.f})) {
            for (int i = 2; i < BODY_N; ++i) s_def.body[i] = randDef(kBodyLo[i], kBodyHi[i], kBodyDef[i]);
            for (int i = 0; i < FACE_N; ++i) s_def.face[i] = randDef(kFaceLo[i], kFaceHi[i], kFaceDef[i]);
            s_def.skin_rgb[0] = 0.35f + (rand()%100)/100.f * 0.55f;
            s_def.skin_rgb[1] = 0.25f + (rand()%100)/100.f * 0.45f;
            s_def.skin_rgb[2] = 0.15f + (rand()%100)/100.f * 0.35f;
#ifdef MD_SDL_GPU
            CharPreviewSDLGPU::ResetAnimPhase();
#endif
        }
        ImGui::SameLine(0.f, spc);
        if (ImGui::Button("RESET##cc", {bw3, 0.f})) {
            s_def = Def{};
#ifdef MD_SDL_GPU
            CharPreviewSDLGPU::ResetAnimPhase();
#endif
        }

        ImGui::PopStyleVar();
    }

    ImGui::EndChild();  // right
    if (kenshi_theme) ImGui::PopStyleColor();  // ChildBg for cc_right

    ImGui::PopStyleVar(2);
    if (kenshi_theme) PopKenshiTheme();
}
