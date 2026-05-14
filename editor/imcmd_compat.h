#pragma once
// Compatibility shim for imgui-command-palette built against older ImGui API.
// ImGui 1.87+ removed GetKeyIndex(); 1.89+ renamed ImFont::FontSize → ImFont::Size.
#include "imgui.h"
#ifndef FontSize
#define FontSize Size
#endif
namespace ImGui {
    inline int GetKeyIndex(ImGuiKey key) { return (int)key; }
}
