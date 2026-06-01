#pragma once
// Shared RMB delta between UpdateEditorCamera (producer) and CharPreviewSDLGPU (consumer).
//
// Two cases:
//   A) UpdateEditorCamera runs first (game F3, cam_game_mode=false):
//      - reads SDL_GetRelativeMouseState, stores in rdx/rdy, sets delta_consumed=true
//      - DrawInImGui reads rdx/rdy from cache, resets delta_consumed=false
//   B) UpdateEditorCamera does NOT run (tools editor, game cam_game_mode=true):
//      - delta_consumed stays false
//      - DrawInImGui reads SDL_GetRelativeMouseState itself into rdx/rdy
namespace EditorCharMouse {
    inline float  rdx             = 0.f;   // relative X delta this frame
    inline float  rdy             = 0.f;   // relative Y delta this frame
    inline bool   dragging        = false;  // true when char preview owns RMB drag
    inline bool   delta_consumed  = false;  // set by UpdateEditorCamera when it reads SDL
}
