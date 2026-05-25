#pragma once
#include <cstdio>
#include <ctime>

// BugCapture — dumps debug state to tmp_md/bug_<label>_YYYYMMDD_HHMMSS.txt.
// Usage: FILE* f = BugCapture::Open("chars", path, sizeof(path));
//        // write state...
//        BugCapture::Close(f);

namespace BugCapture {

inline FILE* Open(const char* label, char* out_path = nullptr, int out_sz = 0) {
    char path[256];
    time_t now = time(nullptr);
    struct tm* lt = localtime(&now);
    snprintf(path, sizeof(path),
        "tmp_md/bug_%s_%04d%02d%02d_%02d%02d%02d.txt",
        label,
        lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
        lt->tm_hour, lt->tm_min, lt->tm_sec);
    if (out_path && out_sz > 0)
        snprintf(out_path, (size_t)out_sz, "%s", path);
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[BugCapture] Cannot create %s\n", path); return nullptr; }
    fprintf(f, "# monkey_dust BugCapture — %s\n", label);
    fprintf(f, "# %04d-%02d-%02d %02d:%02d:%02d\n\n",
        lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
        lt->tm_hour, lt->tm_min, lt->tm_sec);
    return f;
}

inline void Close(FILE* f) { if (f) fclose(f); }

} // namespace BugCapture
