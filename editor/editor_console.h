#pragma once
#ifdef MONKEY_DUST_EDITOR

#include <cstdint>
#include "TextEditor.h"

class EditorConsole {
public:
    static EditorConsole& Get() { static EditorConsole inst; return inst; }

    void Draw();
    void Log(int level, const char* text);   // called from MdLogHook
    void Init();                              // install MdLogSetHook
    void SetFrameStats(float fps, float dt_ms) { frame_fps_ = fps; frame_dt_ms_ = dt_ms; }

    static constexpr int LINE_LEN     = 160; // public: used in static callback

private:
    EditorConsole() = default;

    static constexpr int MAX_LINES    = 512;
    static constexpr int MAX_HISTORY  = 32;
    static constexpr int MAX_CMD      = 128;

    char log_lines_[MAX_LINES][LINE_LEN] = {};
    int  log_level_[MAX_LINES]           = {};
    int  log_count_  = 0;
    int  log_head_   = 0;   // ring index
    bool scroll_bottom_ = true;

    char input_buf_[MAX_CMD]           = {};
    char history_[MAX_HISTORY][MAX_CMD]= {};
    int  history_count_ = 0;
    int  history_idx_   = -1;

    char  filter_[64]  = {};
    bool  lua_mode_    = false;
    float frame_fps_   = 0.f;
    float frame_dt_ms_ = 0.f;

    TextEditor lua_editor_;
    bool       lua_editor_init_  = false;
    bool       lua_editor_dirty_ = false;

    void ExecCommand(const char* cmd);
    void PushHistory(const char* cmd);
};
#endif
