#ifdef MONKEY_DUST_EDITOR
#include "editor_asset_browser.h"
#include "editor_core.h"
#include <monkey_dust/save/save_system.h>
#include <monkey_dust/platform/md_log.h>
#include <dirent.h>
#include <cstring>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
static bool StrEndsWith(const char* s, const char* ext) {
    int sl = (int)strlen(s), el = (int)strlen(ext);
    if (sl < el) return false;
    return strcmp(s + sl - el, ext) == 0;
}

static bool IsHidden(const char* name) {
    return name[0] == '.';
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorAssetBrowser::ScanDirectory() {
    dir_count_  = 0;
    file_count_ = 0;

    DIR* d = opendir(current_path_);
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (IsHidden(ent->d_name)) continue;
        if (ent->d_type == DT_DIR) {
            if (dir_count_ < MAX_DIRS)
                snprintf(dirs_[dir_count_++], MAX_PATH, "%s", ent->d_name);
        } else {
            if (file_count_ < MAX_FILES)
                snprintf(files_[file_count_++], MAX_PATH, "%s", ent->d_name);
        }
    }
    closedir(d);
    needs_refresh_ = false;
}

// ─────────────────────────────────────────────────────────────────────────────
const char* EditorAssetBrowser::FileIcon(const char* name) const {
    if (StrEndsWith(name, ".json"))  return "[J]";
    if (StrEndsWith(name, ".lua"))   return "[L]";
    if (StrEndsWith(name, ".mdsave"))return "[S]";
    if (StrEndsWith(name, ".wav"))   return "[A]";
    if (StrEndsWith(name, ".ogg"))   return "[A]";
    if (StrEndsWith(name, ".mp3"))   return "[A]";
    if (StrEndsWith(name, ".obj"))   return "[M]";
    if (StrEndsWith(name, ".gltf"))  return "[M]";
    if (StrEndsWith(name, ".glb"))   return "[M]";
    if (StrEndsWith(name, ".png"))   return "[T]";
    if (StrEndsWith(name, ".jpg"))   return "[T]";
    if (StrEndsWith(name, ".bin"))   return "[B]";
    return "[?]";
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorAssetBrowser::DrawFileEntry(const char* name, int idx) {
    const char* icon = FileIcon(name);
    char label[MAX_PATH + 8];
    snprintf(label, sizeof(label), "%s %s##f%d", icon, name, idx);

    bool is_lua   = StrEndsWith(name, ".lua");
    bool is_save  = StrEndsWith(name, ".mdsave");
    bool is_audio = StrEndsWith(name, ".wav") || StrEndsWith(name, ".ogg");

    ImVec4 col = {0.85f, 0.85f, 0.85f, 1.f};
    if (is_lua)   col = {0.4f, 0.8f, 1.f, 1.f};
    if (is_save)  col = {1.f, 0.8f, 0.3f, 1.f};
    if (is_audio) col = {0.6f, 1.f, 0.6f, 1.f};

    ImGui::PushStyleColor(ImGuiCol_Text, col);
    bool clicked = ImGui::Selectable(label, false,
        ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 0));
    ImGui::PopStyleColor();

    if (clicked && ImGui::IsMouseDoubleClicked(0)) {
        if (is_save) {
            char full[MAX_PATH * 2];
            snprintf(full, sizeof(full), "%s%s", current_path_, name);
            SaveSystem::Get().Load(full);
            MD_LOG(MD_LOG_INFO, "[AssetBrowser] Loaded save: %s", full);
        } else if (is_lua) {
            MD_LOG(MD_LOG_INFO, "[AssetBrowser] Open script: %s", name);
        } else if (is_audio) {
            MD_LOG(MD_LOG_INFO, "[AssetBrowser] Preview audio: %s (stub)", name);
        }
    }

    // Right-click context menu
    if (ImGui::BeginPopupContextItem()) {
        char full[MAX_PATH * 2];
        snprintf(full, sizeof(full), "%s%s", current_path_, name);
        if (ImGui::MenuItem("Copy Path")) {
            ImGui::SetClipboardText(full);
        }
        if (is_save && ImGui::MenuItem("Load Save")) {
            SaveSystem::Get().Load(full);
        }
        ImGui::EndPopup();
    }

    // Tooltip with full path
    if (ImGui::IsItemHovered()) {
        char full[MAX_PATH * 2];
        snprintf(full, sizeof(full), "%s%s", current_path_, name);
        ImGui::SetTooltip("%s", full);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorAssetBrowser::Draw() {
    if (!EditorCore::Get().panels_visible[2]) return;

    ImGui::SetNextWindowSize(ImVec2(280, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(0, 590), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Assets##AssetBrowser", &EditorCore::Get().panels_visible[2])) {
        ImGui::End();
        return;
    }

    // Toolbar
    if (ImGui::SmallButton("[<]")) {
        // Back: strip last segment
        char tmp[MAX_PATH];
        snprintf(tmp, sizeof(tmp), "%s", current_path_);
        int len = (int)strlen(tmp);
        if (len > 0 && tmp[len-1] == '/') tmp[--len] = '\0'; // strip trailing /
        char* slash = strrchr(tmp, '/');
        if (slash && slash != tmp) {
            *(slash+1) = '\0';
            snprintf(current_path_, MAX_PATH, "%s", tmp);
        } else {
            snprintf(current_path_, MAX_PATH, "data/");
        }
        needs_refresh_ = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("[R]")) needs_refresh_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", current_path_);

    // Search filter
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##filter_ab", filter_, sizeof(filter_));
    ImGui::Separator();

    if (needs_refresh_) ScanDirectory();

    ImGui::BeginChild("##ABScroll", ImVec2(0, -26), false);

    // Directories
    for (int i = 0; i < dir_count_; ++i) {
        if (filter_[0] != '\0' && strstr(dirs_[i], filter_) == nullptr) continue;
        char label[MAX_PATH + 8];
        snprintf(label, sizeof(label), "[D] %s/##d%d", dirs_[i], i);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.85f, 0.4f, 1.f));
        if (ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(0)) {
                char tmp[MAX_PATH];
                snprintf(tmp, sizeof(tmp), "%s%s/", current_path_, dirs_[i]);
                snprintf(current_path_, MAX_PATH, "%s", tmp);
                needs_refresh_ = true;
            }
        }
        ImGui::PopStyleColor();
    }

    // Files
    for (int i = 0; i < file_count_; ++i) {
        if (filter_[0] != '\0' && strstr(files_[i], filter_) == nullptr) continue;
        DrawFileEntry(files_[i], i);
    }

    ImGui::EndChild();

    // Status bar
    ImGui::Separator();
    ImGui::TextDisabled("%d dirs  %d files", dir_count_, file_count_);
    ImGui::SameLine();
#ifdef __linux__
    if (ImGui::SmallButton("[Open]")) {
        char cmd[MAX_PATH + 32];
        snprintf(cmd, sizeof(cmd), "xdg-open %s &", current_path_);
        system(cmd);
    }
#endif

    ImGui::End();
}
#endif
