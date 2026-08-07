#pragma once
// character_editor.h v2 — Kenshi-style 3-panel character creator.
// LEFT (160px): race/gender/desc/stats  |  CENTER: 3D preview  |  RIGHT (270px): BODY/FACE/HAIR sliders
// Layout informed by a reference-game UX study of similar character creators.
//
// Split into per-concern fragment headers (character_editor_data.h/_io.h/
// _widgets.h/_draw.h) — all pasted here inside namespace CharacterEditor,
// so each of this header's two includers (tools/editor/main.cpp,
// tools/editor/editor_panels_entry.cpp) still sees one flat namespace with
// identical contents/behavior to the pre-split single file.

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

#include "character_editor_data.h"
#include "character_editor_io.h"
#include "character_editor_widgets.h"
#include "character_editor_draw.h"

} // namespace CharacterEditor
