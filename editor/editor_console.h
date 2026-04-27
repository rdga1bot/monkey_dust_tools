#pragma once
#ifdef MONKEY_DUST_EDITOR

class EditorConsole {
public:
    static EditorConsole& Get() { static EditorConsole inst; return inst; }

    void Draw();
    void Log(int level, const char* text);   // called from TraceLog callback
    void Init();                              // install TraceLog callback

    static constexpr int LINE_LEN     = 160; // public: used in static callback

private:
    EditorConsole() = default;

    static constexpr int MAX_LINES    = 512;
    static constexpr int MAX_HISTORY  = 32;
    static constexpr int MAX_CMD      = 128;

    char log_lines_[MAX_LINES][LINE_LEN] = {};
    int  log_level_[MAX_LINES]           = {};  // raylib LOG_* values
    int  log_count_  = 0;
    int  log_head_   = 0;   // ring index
    bool scroll_bottom_ = true;

    char input_buf_[MAX_CMD]           = {};
    char history_[MAX_HISTORY][MAX_CMD]= {};
    int  history_count_ = 0;
    int  history_idx_   = -1;

    char filter_[64] = {};
    bool lua_mode_   = false;

    void ExecCommand(const char* cmd);
    void PushHistory(const char* cmd);
};
#endif
