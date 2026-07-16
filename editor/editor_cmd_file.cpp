#include "editor_cmd_file.h"
#include <monkey_dust/scripting/lua_system.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char* kDir       = "tmp_/editor_cmd";
constexpr const char* kSeqPath   = "tmp_/editor_cmd/cmd.seq";
constexpr const char* kCmdPath   = "tmp_/editor_cmd/cmd.lua";
constexpr const char* kResultTmp = "tmp_/editor_cmd/result.json.tmp";
constexpr const char* kResultPath = "tmp_/editor_cmd/result.json";

// Reads a small integer from a text file. Returns 0 if the file doesn't
// exist or is empty (0 is never a valid sender-assigned seq — senders start
// at 1 — so "not present" and "seq 0" are indistinguishable, and both mean
// "no command yet").
long ReadSeqFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    long v = 0;
    if (fscanf(f, "%ld", &v) != 1) v = 0;
    fclose(f);
    return v;
}

} // namespace

void EditorCmdFile_Poll() {
    static bool s_dir_ready = false;
    static long s_last_seq  = 0;
    static char s_cmd_buf[1 << 20]; // 1MB cap, no heap — mirrors main.cpp's --exec script buffer

    if (!s_dir_ready) {
        // Lazy mkdir -p — same pattern EditorConsole::ExecCommand already
        // uses for /reload-shaders' shell invocation.
        char mkdir_cmd[64];
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", kDir);
        system(mkdir_cmd);
        s_dir_ready = true;
    }

    long seq = ReadSeqFile(kSeqPath);
    if (seq <= s_last_seq) return;

    FILE* cf = fopen(kCmdPath, "rb");
    if (!cf) { s_last_seq = seq; return; } // seq bumped but cmd.lua missing/not-yet-visible — skip this seq, don't retry it forever
    fseek(cf, 0, SEEK_END);
    long sz = ftell(cf);
    fseek(cf, 0, SEEK_SET);
    if (sz <= 0 || sz >= (long)sizeof(s_cmd_buf)) {
        fclose(cf);
        s_last_seq = seq;
        return;
    }
    size_t nread = fread(s_cmd_buf, 1, (size_t)sz, cf);
    s_cmd_buf[nread] = '\0';
    fclose(cf);

    char err[400] = {};
    bool ok = LuaSystem::Get().Exec(s_cmd_buf, err, sizeof(err));
    s_last_seq = seq;

    // Hand-formatted JSON (CLAUDE.md: no external JSON library) — fixed,
    // known-shape 3-field object, no escaping needed beyond the error
    // string (Lua error messages can contain quotes/newlines; replace them
    // with spaces so the fixed-format JSON stays parseable without a real
    // escaper).
    for (char* p = err; *p; ++p) if (*p == '"' || *p == '\n') *p = ' ';

    FILE* rf = fopen(kResultTmp, "wb");
    if (rf) {
        fprintf(rf, "{\"seq\":%ld,\"ok\":%s,\"error\":\"%s\"}\n", seq, ok ? "true" : "false", err);
        fclose(rf);
        rename(kResultTmp, kResultPath);
    }
}
