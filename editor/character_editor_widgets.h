#pragma once
// Reusable ImGui widget helpers section of character_editor.h's split
// (theme, slider, nav-row, stat-bar). Included ONLY from character_editor.h,
// inside namespace CharacterEditor { ... }.

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
