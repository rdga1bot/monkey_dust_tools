#pragma once
#include "imgui.h"

// ─────────────────────────────────────────────────────────
// EditorUI — ImGui font globals + WickedEngine-style theme.
//
// Три шрифти (завантажуються в main.cpp):
//   font_regular — Arimo Regular  (body text, labels)
//   font_bold    — Arimo Bold     (panel headers, titles)
//   font_mono    — Ubuntu Mono    (InputText, IDs, values)
//
// Кирилиця підтримується через кастомний glyph range.
// ─────────────────────────────────────────────────────────

namespace EditorUI {

static ImFont* font_regular = nullptr;
static ImFont* font_bold    = nullptr;
static ImFont* font_mono    = nullptr;
static float   ui_scale     = 1.0f;

// Basic Latin + Latin Supplement + Cyrillic
inline const ImWchar* GlyphRanges() {
    static const ImWchar r[] = {
        0x0020, 0x00FF,
        0x0400, 0x04FF,
        0,
    };
    return r;
}

inline void SetupTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 4.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 3.0f;
    s.ScrollbarRounding = 3.0f;
    s.GrabRounding      = 3.0f;
    s.TabRounding       = 4.0f;
    s.PopupRounding     = 3.0f;

    s.FramePadding      = {6, 4};
    s.ItemSpacing       = {8, 6};
    s.ItemInnerSpacing  = {4, 4};
    s.WindowPadding     = {12, 10};
    s.ScrollbarSize     = 14.0f;
    s.GrabMinSize       = 8.0f;
    s.IndentSpacing     = 16.0f;
    s.SeparatorTextPadding = {6, 6};

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = {0.87f, 0.89f, 0.95f, 1.00f};
    c[ImGuiCol_TextDisabled]          = {0.42f, 0.46f, 0.58f, 1.00f};
    c[ImGuiCol_WindowBg]              = {0.09f, 0.11f, 0.17f, 1.00f};
    c[ImGuiCol_ChildBg]               = {0.07f, 0.09f, 0.14f, 1.00f};
    c[ImGuiCol_PopupBg]               = {0.09f, 0.11f, 0.17f, 0.96f};
    c[ImGuiCol_Border]                = {0.20f, 0.24f, 0.38f, 1.00f};
    c[ImGuiCol_BorderShadow]          = {0.00f, 0.00f, 0.00f, 0.00f};
    c[ImGuiCol_FrameBg]               = {0.13f, 0.16f, 0.25f, 1.00f};
    c[ImGuiCol_FrameBgHovered]        = {0.18f, 0.23f, 0.36f, 1.00f};
    c[ImGuiCol_FrameBgActive]         = {0.22f, 0.29f, 0.46f, 1.00f};
    c[ImGuiCol_TitleBg]               = {0.07f, 0.09f, 0.14f, 1.00f};
    c[ImGuiCol_TitleBgActive]         = {0.12f, 0.16f, 0.28f, 1.00f};
    c[ImGuiCol_TitleBgCollapsed]      = {0.07f, 0.09f, 0.14f, 0.75f};
    c[ImGuiCol_MenuBarBg]             = {0.07f, 0.09f, 0.14f, 1.00f};
    c[ImGuiCol_ScrollbarBg]           = {0.07f, 0.09f, 0.14f, 1.00f};
    c[ImGuiCol_ScrollbarGrab]         = {0.18f, 0.27f, 0.46f, 1.00f};
    c[ImGuiCol_ScrollbarGrabHovered]  = {0.26f, 0.38f, 0.60f, 1.00f};
    c[ImGuiCol_ScrollbarGrabActive]   = {0.35f, 0.50f, 0.75f, 1.00f};
    c[ImGuiCol_CheckMark]             = {0.40f, 0.70f, 1.00f, 1.00f};
    c[ImGuiCol_SliderGrab]            = {0.30f, 0.56f, 0.90f, 1.00f};
    c[ImGuiCol_SliderGrabActive]      = {0.45f, 0.70f, 1.00f, 1.00f};
    c[ImGuiCol_Button]                = {0.19f, 0.35f, 0.65f, 1.00f};
    c[ImGuiCol_ButtonHovered]         = {0.29f, 0.50f, 0.85f, 1.00f};
    c[ImGuiCol_ButtonActive]          = {0.14f, 0.27f, 0.52f, 1.00f};
    c[ImGuiCol_Header]                = {0.19f, 0.35f, 0.65f, 0.55f};
    c[ImGuiCol_HeaderHovered]         = {0.24f, 0.43f, 0.75f, 0.80f};
    c[ImGuiCol_HeaderActive]          = {0.24f, 0.43f, 0.75f, 1.00f};
    c[ImGuiCol_Separator]             = {0.20f, 0.24f, 0.38f, 1.00f};
    c[ImGuiCol_SeparatorHovered]      = {0.30f, 0.48f, 0.80f, 1.00f};
    c[ImGuiCol_SeparatorActive]       = {0.40f, 0.60f, 0.90f, 1.00f};
    c[ImGuiCol_ResizeGrip]            = {0.19f, 0.35f, 0.65f, 0.30f};
    c[ImGuiCol_ResizeGripHovered]     = {0.26f, 0.46f, 0.80f, 0.60f};
    c[ImGuiCol_ResizeGripActive]      = {0.35f, 0.58f, 0.90f, 1.00f};
    c[ImGuiCol_Tab]                   = {0.10f, 0.12f, 0.20f, 1.00f};
    c[ImGuiCol_TabHovered]            = {0.24f, 0.43f, 0.75f, 1.00f};
    c[ImGuiCol_TabSelected]           = {0.16f, 0.28f, 0.54f, 1.00f};
    c[ImGuiCol_TabSelectedOverline]   = {0.30f, 0.56f, 0.90f, 1.00f};
    c[ImGuiCol_TabDimmed]             = {0.07f, 0.09f, 0.15f, 1.00f};
    c[ImGuiCol_TabDimmedSelected]     = {0.12f, 0.18f, 0.34f, 1.00f};
}

} // namespace EditorUI
