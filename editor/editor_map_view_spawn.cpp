#include "editor_map_view.h"
#include "imgui.h"
#include <monkey_dust/flare/tile_map.h>
#include <cstdio>
#include <cmath>

// ── Spawn helpers ─────────────────────────────────────────────────────────────

// Map iso coords → viewport-relative screen position.
static void SpawnToScreen(float cx, float cy, float ox, float oy, float sc,
                          float& sx, float& sy) {
    sx = (cx - cy) * 96.0f * sc + ox;
    sy = (cx + cy) * 48.0f * sc + oy;
}

void MapViewPanel::DrawSpawnMarkers(ImVec2 img_pos) {
    if (!loaded_) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Hero spawn marker (always drawn)
    {
        float sx, sy;
        SpawnToScreen(map_.hero_x, map_.hero_y, origin_x_, origin_y_, scale_, sx, sy);
        ImVec2 p = {img_pos.x + sx, img_pos.y + sy};
        bool props = (palette_tab_ == 2);
        dl->AddCircleFilled(p, 9.0f, IM_COL32(50, 180, 255, 200));
        dl->AddCircle(p, 9.5f, IM_COL32(255,255,255, props ? 255 : 160), 0, props ? 2.0f : 1.0f);
        dl->AddText({p.x - 3.5f, p.y - 6.5f}, IM_COL32(255, 255, 255, 255), "H");
        if (props && ImGui::IsWindowHovered()) {
            ImVec2 mp = ImGui::GetIO().MousePos;
            float dx = mp.x - p.x, dy = mp.y - p.y;
            if (dx*dx + dy*dy < 100.0f)
                ImGui::SetTooltip("Hero spawn (%.0f, %.0f)", map_.hero_x, map_.hero_y);
        }
    }

    if (map_.spawn_count == 0) return;

    for (int i = 0; i < map_.spawn_count; i++) {
        const auto& s = map_.spawns[i];
        float sx, sy;
        SpawnToScreen(s.center_x, s.center_y, origin_x_, origin_y_, scale_, sx, sy);
        ImVec2 p = {img_pos.x + sx, img_pos.y + sy};

        bool  selected = (sel_spawn_ == i);
        ImU32 fill = selected ? IM_COL32(255, 220, 40, 230) : IM_COL32(220, 60, 40, 210);
        dl->AddCircleFilled(p, 8.0f, fill);
        dl->AddCircle(p, 8.5f, IM_COL32(255, 255, 255, 200), 0, 1.5f);

        char letter[2] = { s.category[0] ? s.category[0] : '?', '\0' };
        dl->AddText({p.x - 3.5f, p.y - 6.5f}, IM_COL32(255, 255, 255, 255), letter);

        if (palette_tab_ == 1 && ImGui::IsWindowHovered()) {
            ImVec2 mp = ImGui::GetIO().MousePos;
            float  dx = mp.x - p.x, dy = mp.y - p.y;
            if (dx*dx + dy*dy < 100.0f)
                ImGui::SetTooltip("%s  lv%d  ×%d  r%d\n(%.0f, %.0f)",
                    s.category, s.level, s.number_min, s.wander_radius,
                    s.center_x, s.center_y);
        }
    }
}

void MapViewPanel::SpawnInteract(float mx, float my) {
    if (!loaded_) return;

    // Hit-test existing spawns
    auto hit_spawn = [&]() -> int {
        for (int i = 0; i < map_.spawn_count; i++) {
            const auto& s = map_.spawns[i];
            float sx, sy;
            SpawnToScreen(s.center_x, s.center_y, origin_x_, origin_y_, scale_, sx, sy);
            float dx = mx - sx, dy = my - sy;
            if (dx*dx + dy*dy < 100.0f) return i;
        }
        return -1;
    };

    // Click: select existing or add new
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int hit = hit_spawn();
        if (hit >= 0) {
            sel_spawn_      = hit;
            spawn_dragging_ = true;
        } else if (map_.spawn_count < md::flare::MAX_SPAWNS_PER_MAP) {
            float lx = (mx - origin_x_) / scale_;
            float ly = (my - origin_y_) / scale_;
            float cr = lx / 96.0f, cs = ly / 48.0f;
            int   c  = (int)roundf((cr + cs) * 0.5f);
            int   r  = (int)roundf((cs - cr) * 0.5f);
            if (c >= 0 && c < map_.width && r >= 0 && r < map_.height) {
                md::flare::FlareSpawn s = {};
                snprintf(s.category, sizeof(s.category), "goblin");
                s.center_x = (float)c;
                s.center_y = (float)r;
                s.number_min = 1;
                s.level = 1;
                s.wander_radius = 3;
                map_.spawns[map_.spawn_count++] = s;
                sel_spawn_      = map_.spawn_count - 1;
                spawn_dragging_ = true;
            }
        }
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) spawn_dragging_ = false;

    // Drag: move selected spawn
    if (spawn_dragging_ && sel_spawn_ >= 0 &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        float lx = (mx - origin_x_) / scale_;
        float ly = (my - origin_y_) / scale_;
        float cr = lx / 96.0f, cs = ly / 48.0f;
        auto& s = map_.spawns[sel_spawn_];
        s.center_x = roundf((cr + cs) * 0.5f);
        s.center_y = roundf((cs - cr) * 0.5f);
        s.center_x = (s.center_x < 0) ? 0 : (s.center_x >= map_.width  ? map_.width  - 1 : s.center_x);
        s.center_y = (s.center_y < 0) ? 0 : (s.center_y >= map_.height ? map_.height - 1 : s.center_y);
    }

    // Delete key: remove selected spawn
    if (sel_spawn_ >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        for (int i = sel_spawn_; i < map_.spawn_count - 1; i++)
            map_.spawns[i] = map_.spawns[i + 1];
        map_.spawn_count--;
        if (sel_spawn_ >= map_.spawn_count) sel_spawn_ = map_.spawn_count - 1;
    }
}

void MapViewPanel::DrawSpawnPanel() {
    if (!loaded_) { ImGui::TextDisabled("Load a map"); return; }
    ImGui::TextDisabled("%d spawns", map_.spawn_count);
    ImGui::SameLine();
    if (ImGui::SmallButton("+##sp")) {
        if (map_.spawn_count < md::flare::MAX_SPAWNS_PER_MAP) {
            md::flare::FlareSpawn s = {};
            snprintf(s.category, sizeof(s.category), "goblin");
            s.center_x = map_.width  * 0.5f;
            s.center_y = map_.height * 0.5f;
            s.number_min = 1; s.level = 1; s.wander_radius = 3;
            map_.spawns[map_.spawn_count++] = s;
            sel_spawn_ = map_.spawn_count - 1;
        }
    }
    ImGui::SameLine();
    if (sel_spawn_ >= 0 && sel_spawn_ < map_.spawn_count && ImGui::SmallButton("Del")) {
        for (int i = sel_spawn_; i < map_.spawn_count - 1; i++)
            map_.spawns[i] = map_.spawns[i + 1];
        map_.spawn_count--;
        if (sel_spawn_ >= map_.spawn_count) sel_spawn_ = map_.spawn_count - 1;
    }
    ImGui::Separator();

    // Spawn list (scrollable)
    ImGui::BeginChild("##splist", ImVec2(-1, 120), false);
    for (int i = 0; i < map_.spawn_count; i++) {
        const auto& s = map_.spawns[i];
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "%s (%d,%d)##s%d",
                 s.category, (int)s.center_x, (int)s.center_y, i);
        if (ImGui::Selectable(lbl, sel_spawn_ == i)) sel_spawn_ = i;
    }
    ImGui::EndChild();

    // Inspector for selected spawn
    if (sel_spawn_ >= 0 && sel_spawn_ < map_.spawn_count) {
        ImGui::Separator();
        auto& s = map_.spawns[sel_spawn_];
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##cat", s.category, sizeof(s.category));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Category");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##lv", &s.level, 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Level");
        if (s.level < 0) s.level = 0;
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##cnt", &s.number_min, 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Count");
        if (s.number_min < 1) s.number_min = 1;
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##rad", &s.wander_radius, 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Wander radius");
        if (s.wander_radius < 0) s.wander_radius = 0;
        ImGui::TextDisabled("(%.0f, %.0f)", s.center_x, s.center_y);
    }
}

// ── Props panel (M9.9) ───────────────────────────────────────────────────────

void MapViewPanel::DrawPropsPanel() {
    if (!loaded_) { ImGui::TextDisabled("Load a map"); return; }

    ImGui::TextDisabled("Map Properties");
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##title", map_.title, sizeof(map_.title));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Title");

    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##music", map_.music_path, sizeof(map_.music_path));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Music path");

    ImGui::Separator();
    ImGui::TextDisabled("Hero spawn:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputFloat("##hx", &map_.hero_x, 1.0f, 0.0f, "X: %.0f");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputFloat("##hy", &map_.hero_y, 1.0f, 0.0f, "Y: %.0f");
    map_.hero_x = (map_.hero_x < 0) ? 0 : (map_.hero_x >= map_.width  ? (float)(map_.width -1) : map_.hero_x);
    map_.hero_y = (map_.hero_y < 0) ? 0 : (map_.hero_y >= map_.height ? (float)(map_.height-1) : map_.hero_y);

    ImGui::Separator();
    ImGui::TextDisabled("Size: %d × %d", map_.width, map_.height);
    ImGui::TextDisabled("Tilesetdef:");
    ImGui::TextWrapped("%s", map_.tileset_def);
    ImGui::Spacing();
    ImGui::TextColored({0.5f,0.8f,0.5f,1.f}, "LMB = set hero spawn");
}

void MapViewPanel::PropsInteract(float mx, float my) {
    if (!loaded_) return;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        float lx = (mx - origin_x_) / scale_;
        float ly = (my - origin_y_) / scale_;
        float cr = lx / 96.0f, cs = ly / 48.0f;
        float c = (cr + cs) * 0.5f, r = (cs - cr) * 0.5f;
        c = (c < 0) ? 0 : (c >= map_.width  ? (float)(map_.width -1) : c);
        r = (r < 0) ? 0 : (r >= map_.height ? (float)(map_.height-1) : r);
        map_.hero_x = roundf(c);
        map_.hero_y = roundf(r);
    }
}
