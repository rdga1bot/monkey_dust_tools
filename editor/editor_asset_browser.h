#pragma once
#ifdef MONKEY_DUST_EDITOR

class EditorAssetBrowser {
public:
    static EditorAssetBrowser& Get() { static EditorAssetBrowser inst; return inst; }

    void Draw();

private:
    EditorAssetBrowser() = default;

    static constexpr int MAX_DIRS  = 32;
    static constexpr int MAX_FILES = 256;
    static constexpr int MAX_PATH  = 128;

    char current_path_[MAX_PATH]       = "data/";
    char dirs_[MAX_DIRS][MAX_PATH]     = {};
    char files_[MAX_FILES][MAX_PATH]   = {};
    int  dir_count_  = 0;
    int  file_count_ = 0;
    char filter_[64] = {};
    bool needs_refresh_ = true;

    void ScanDirectory();
    void DrawFileEntry(const char* name, int idx);
    const char* FileIcon(const char* name) const;
};
#endif
