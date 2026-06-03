#pragma once
// font_loader.h — Loads embedded project fonts into ImGui atlas.
// All 8 TTF fonts are compiled into engine lib (embedded_fonts.cpp).
// Call MdFonts::Load() once after ImGui::CreateContext().

#include "imgui.h"
#include <monkey_dust/platform/embedded_fonts.h>

namespace MdFonts {

// Loaded font pointers — valid after Load(), null before.
inline ImFont* regular         = nullptr;
inline ImFont* bold            = nullptr;
inline ImFont* italic          = nullptr;
inline ImFont* bold_italic     = nullptr;
inline ImFont* mono            = nullptr;
inline ImFont* mono_bold       = nullptr;
inline ImFont* mono_italic     = nullptr;
inline ImFont* mono_bold_italic= nullptr;

// Load all 8 embedded fonts. Call once after ImGui::CreateContext().
// ui_size  = pixel size for Arimo (UI text)
// mono_size = pixel size for UbuntuMono (console / code)
inline void Load(float ui_size = 14.f, float mono_size = 13.f) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // static arrays — ImGui must NOT free them

    regular         = io.Fonts->AddFontFromMemoryTTF(
        (void*)md_font::Arimo_Regular,         (int)md_font::Arimo_Regular_size,         ui_size,   &cfg);
    bold            = io.Fonts->AddFontFromMemoryTTF(
        (void*)md_font::Arimo_Bold,            (int)md_font::Arimo_Bold_size,            ui_size,   &cfg);
    italic          = io.Fonts->AddFontFromMemoryTTF(
        (void*)md_font::Arimo_Italic,          (int)md_font::Arimo_Italic_size,          ui_size,   &cfg);
    bold_italic     = io.Fonts->AddFontFromMemoryTTF(
        (void*)md_font::Arimo_BoldItalic,      (int)md_font::Arimo_BoldItalic_size,      ui_size,   &cfg);
    mono            = io.Fonts->AddFontFromMemoryTTF(
        (void*)md_font::UbuntuMono_Regular,    (int)md_font::UbuntuMono_Regular_size,    mono_size, &cfg);
    mono_bold       = io.Fonts->AddFontFromMemoryTTF(
        (void*)md_font::UbuntuMono_Bold,       (int)md_font::UbuntuMono_Bold_size,       mono_size, &cfg);
    mono_italic     = io.Fonts->AddFontFromMemoryTTF(
        (void*)md_font::UbuntuMono_Italic,     (int)md_font::UbuntuMono_Italic_size,     mono_size, &cfg);
    mono_bold_italic= io.Fonts->AddFontFromMemoryTTF(
        (void*)md_font::UbuntuMono_BoldItalic, (int)md_font::UbuntuMono_BoldItalic_size, mono_size, &cfg);

    io.FontDefault = regular;  // Arimo Regular as global default
}

} // namespace MdFonts
