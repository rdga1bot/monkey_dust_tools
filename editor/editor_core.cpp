#ifdef MONKEY_DUST_EDITOR
#include "editor_core.h"
#include <monkey_dust/ecs/md_registry.h>
#include "editor_toolbar.h"
#include "editor_hierarchy.h"
#ifndef MONKEY_DUST_STANDALONE_EDITOR
#include "editor_inspector.h"
#endif
#include "editor_camera_panel.h"
#include "editor_console.h"
#include "editor_animation_panel.h"
#include "editor_flare_browser.h"
#include "editor_viewcone_panel.h"
#include "editor_flowgraph_panel.h"
#include "editor_sequencer_panel.h"
#include "editor_director_panel.h"
#include "editor_gpu_profiler_panel.h"
#include "editor_node_graph.h"
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/platform/input.h>
#include <monkey_dust/platform/window.h>  // _wnd::ptr() for RelativeMouseMode
#include <monkey_dust/world/terrain_gen.h>
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstring>

static constexpr float DEG2R = 3.14159265f / 180.f;

static constexpr const char* F3_LAYOUT_PATH = "data/editor_f3_layout.json";

// Version tag written by f3_save; file without it is from a different/stale session — ignored.
static constexpr const char* F3_VERSION_TAG = "\"v\":1";

struct F3SettingsState { bool det; ImVec2 pos, siz; };

static float f3_parse_f(const char* buf, const char* key) {
    const char* p = strstr(buf, key); if (!p) return 0.f;
    p += strlen(key);
    while (*p == ':' || *p == ' ') ++p;
    return (float)atof(p);
}

static void f3_load(bool det[6], ImVec2 pos[6], ImVec2 siz[6],
                    float* out_fspd = nullptr, F3SettingsState* out_set = nullptr) {
    FILE* f = fopen(F3_LAYOUT_PATH, "rb");
    if (!f) return;
    char buf[600]; size_t n = fread(buf, 1, 599, f); buf[n] = '\0'; fclose(f);
    // Reject files not written by this code (no version tag = stale/foreign file).
    if (!strstr(buf, F3_VERSION_TAG)) return;
    const char* dk[6] = {"\"scene\"","\"ai\"","\"anim\"","\"flow\"","\"debug\"","\"cam\""};
    for (int i = 0; i < 6; ++i) {
        const char* p = strstr(buf, dk[i]); if (!p) continue;
        p += strlen(dk[i]); while (*p == ':' || *p == ' ') ++p;
        det[i] = (*p == '1');
    }
    // pos/size keys per panel; only applied when width > 0 (defaults kept on first run)
    static const char* pk[6][4] = {
        {"\"scx\"","\"scy\"","\"scw\"","\"sch\""},
        {"\"aix\"","\"aiy\"","\"aiw\"","\"aih\""},
        {"\"anx\"","\"any\"","\"anw\"","\"anh\""},
        {"\"flx\"","\"fly\"","\"flw\"","\"flh\""},
        {"\"dbx\"","\"dby\"","\"dbw\"","\"dbh\""},
        {"\"cmx\"","\"cmy\"","\"cmw\"","\"cmh\""},
    };
    for (int i = 0; i < 6; ++i) {
        float w = f3_parse_f(buf, pk[i][2]);
        if (w > 0.f) {
            pos[i] = { f3_parse_f(buf, pk[i][0]), f3_parse_f(buf, pk[i][1]) };
            siz[i] = { w, f3_parse_f(buf, pk[i][3]) };
        }
    }
    if (out_fspd) {
        float v = f3_parse_f(buf, "\"fspd\"");
        if (v > 0.f) *out_fspd = v;
    }
    if (out_set) {
        const char* p = strstr(buf, "\"sdet\"");
        if (p) {
            p += 6; while (*p == ':' || *p == ' ') ++p;
            out_set->det = (*p == '1');
        }
        float w = f3_parse_f(buf, "\"stw\"");
        if (w > 0.f) {
            out_set->pos = { f3_parse_f(buf, "\"stx\""), f3_parse_f(buf, "\"sty\"") };
            out_set->siz = { w, f3_parse_f(buf, "\"sth\"") };
        }
    }
}

static void f3_save(const bool det[6], const ImVec2 pos[6], const ImVec2 siz[6],
                    float fspd, F3SettingsState set) {
    FILE* f = fopen(F3_LAYOUT_PATH, "w"); if (!f) return;
    fprintf(f,
        "{\"v\":1,\"scene\":%d,\"ai\":%d,\"anim\":%d,\"flow\":%d,\"debug\":%d,\"cam\":%d,"
        "\"fspd\":%.1f,\"sdet\":%d,"
        "\"scx\":%.0f,\"scy\":%.0f,\"scw\":%.0f,\"sch\":%.0f,"
        "\"aix\":%.0f,\"aiy\":%.0f,\"aiw\":%.0f,\"aih\":%.0f,"
        "\"anx\":%.0f,\"any\":%.0f,\"anw\":%.0f,\"anh\":%.0f,"
        "\"flx\":%.0f,\"fly\":%.0f,\"flw\":%.0f,\"flh\":%.0f,"
        "\"dbx\":%.0f,\"dby\":%.0f,\"dbw\":%.0f,\"dbh\":%.0f,"
        "\"cmx\":%.0f,\"cmy\":%.0f,\"cmw\":%.0f,\"cmh\":%.0f,"
        "\"stx\":%.0f,\"sty\":%.0f,\"stw\":%.0f,\"sth\":%.0f}\n",
        (int)det[0],(int)det[1],(int)det[2],(int)det[3],(int)det[4],(int)det[5],
        fspd,(int)set.det,
        pos[0].x,pos[0].y,siz[0].x,siz[0].y,
        pos[1].x,pos[1].y,siz[1].x,siz[1].y,
        pos[2].x,pos[2].y,siz[2].x,siz[2].y,
        pos[3].x,pos[3].y,siz[3].x,siz[3].y,
        pos[4].x,pos[4].y,siz[4].x,siz[4].y,
        pos[5].x,pos[5].y,siz[5].x,siz[5].y,
        set.pos.x,set.pos.y,set.siz.x,set.siz.y);
    fclose(f);
}

void EditorCore::Init() {
    EditorConsole::Get().Init();
    EditorNodeGraphPanel::Get().Init();
    for (int i = 0; i < MAX_SELECTED; ++i)
        selected[i] = MdEntity::Null();

    bool det6[6] = {};
    ImVec2 pos6[6] = { f3_pos_scene, f3_pos_ai, f3_pos_anim, f3_pos_flow, f3_pos_debug, f3_pos_cam };
    ImVec2 siz6[6] = { f3_size_scene, f3_size_ai, f3_size_anim, f3_size_flow, f3_size_debug, f3_size_cam };
    F3SettingsState set6{f3_det_settings, f3_pos_settings, f3_size_settings};
    f3_load(det6, pos6, siz6, &cam_speed, &set6);
    f3_det_scene = det6[0];  f3_det_ai    = det6[1]; f3_det_anim  = det6[2];
    f3_det_flow  = det6[3];  f3_det_debug = det6[4]; f3_det_cam   = det6[5];
    f3_det_settings = set6.det;
    f3_pos_scene = pos6[0];  f3_pos_ai    = pos6[1]; f3_pos_anim  = pos6[2];
    f3_pos_flow  = pos6[3];  f3_pos_debug = pos6[4]; f3_pos_cam   = pos6[5];
    f3_pos_settings  = set6.pos;  f3_size_settings = set6.siz;
    f3_size_scene = siz6[0]; f3_size_ai   = siz6[1]; f3_size_anim = siz6[2];
    f3_size_flow  = siz6[3]; f3_size_debug = siz6[4]; f3_size_cam = siz6[5];

#ifdef MONKEY_DUST_STANDALONE_EDITOR
    // Standalone editor has its own tab-based layout.
    // Wicked-style in-game panels (Hierarchy, Inspector, Assets, etc.) start hidden;
    // main.cpp enables only the panels relevant to the standalone tool.
    for (int i = 0; i < 15; ++i) panels_visible[i] = false;
#endif

    editor_cam.pos    = { 0.f, 35.f, 35.f };
    editor_cam.target = cam_target;
    editor_cam.up     = { 0.f, 1.f, 0.f };
    editor_cam.fovy   = 60.0f;
}


void EditorCore::Update(float dt) {
    EditorToolbar::Get().Draw(dt);
#ifndef MONKEY_DUST_STANDALONE_EDITOR
    bool& g_det_scene = f3_det_scene, &g_det_ai    = f3_det_ai,    &g_det_anim  = f3_det_anim;
    bool& g_det_flow  = f3_det_flow,  &g_det_debug = f3_det_debug, &g_det_cam   = f3_det_cam;

    ImGuiIO& io = ImGui::GetIO();
    // s_overlay_top: shared with editor_toolbar.cpp (extern float).
    // Defined in main.cpp; editor_core.cpp just reads it.
    extern float s_overlay_top;
    float toolbar_h = s_overlay_top + ImGui::GetFrameHeight() + 30.f;

    // If the active tab's panel is floating, make ##f3editor transparent so the
    // game is visible behind the detached panel (one-frame lag is imperceptible).
    static int  s_f3_active_tab = 0;
    active_tab = s_f3_active_tab;
    ImGuiWindowFlags f3_flags =
        ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;
    if (s_f3_active_tab == 5 && g_det_cam) f3_flags |= ImGuiWindowFlags_NoBackground;
    if (f3_passthrough) f3_flags |= ImGuiWindowFlags_NoMouseInputs;

    ImGui::SetNextWindowPos({0.f, toolbar_h});
    ImGui::SetNextWindowSize({io.DisplaySize.x, io.DisplaySize.y - toolbar_h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
    ImGui::Begin("##f3editor", nullptr, f3_flags);
    ImGui::PopStyleVar();

    // min_y: floating title bars must not overlap the tab bar strip.
    const float min_y = toolbar_h + ImGui::GetFrameHeight() + 4.f;
    // task editor-dock-button-menubar (2026-08-01): Dock used to be a
    // SmallButton sitting alone on its own full-width content row (a
    // Separator below it, nothing else sharing the row) -- correctly sized
    // but wasting an entire empty row's height for a single small control
    // (confirmed clumsy via user screenshots). MenuBar folds it into the
    // window's own title-bar strip instead -- zero extra rows.
    static constexpr ImGuiWindowFlags FLOAT_FLAGS =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_MenuBar;

    // Helper: track pos/size after every frame and clamp y so the title bar
    // never sits under the tab bar.
    auto f3_end = [&](ImVec2& pos, ImVec2& sz) {
        pos = ImGui::GetWindowPos();
        if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
        sz = ImGui::GetWindowSize();
        ImGui::End();
    };

    // ALL tabs stay in the tab bar regardless of detach state.
    // A detached panel's floating window is created INSIDE its BeginTabItem scope —
    // so it only exists (and is visible) while that tab is active.  Switching to
    // another tab makes ImGui hide the floating window automatically.
    // Right-align Detach/Dock buttons in every panel.
    auto det_right = [](const char* lbl) -> bool {
        float bw = ImGui::CalcTextSize(lbl).x + ImGui::GetStyle().FramePadding.x * 2 + 2;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - bw);
        return ImGui::SmallButton(lbl);
    };
    if (ImGui::BeginTabBar("##f3tabs")) {
        if (ImGui::BeginTabItem("Scene")) { s_f3_active_tab = 0;
            if (!g_det_scene) {
                if (det_right("Detach##scene")) g_det_scene = true;
                ImGui::Separator();
                ImVec2 av = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("##f3h", {300.f, av.y}, false);
                EditorHierarchy::Get().DrawContent();
                ImGui::EndChild();
                ImGui::SameLine(0, 4);
#ifndef MONKEY_DUST_STANDALONE_EDITOR
                ImGui::BeginChild("##f3i", {0.f, av.y}, false);
                EditorInspector::Get().DrawContent();
                ImGui::EndChild();
#endif
            } else {
                if (f3_pos_scene.y < min_y) f3_pos_scene.y = min_y;
                ImGui::SetNextWindowPos(f3_pos_scene, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(f3_size_scene, ImGuiCond_Appearing);
                if (ImGui::Begin("Scene##float", &g_det_scene, FLOAT_FLAGS)) {
                    if (ImGui::BeginMenuBar()) {
                        if (det_right("Dock##scene")) g_det_scene = false;
                        ImGui::EndMenuBar();
                    }
                    ImVec2 av = ImGui::GetContentRegionAvail();
                    ImGui::BeginChild("##fh", {av.x * 0.30f, av.y}, false);
                    EditorHierarchy::Get().DrawContent();
                    ImGui::EndChild();
#ifndef MONKEY_DUST_STANDALONE_EDITOR
                    ImGui::SameLine(0, 4);
                    ImGui::BeginChild("##fi", {0.f, av.y}, false);
                    EditorInspector::Get().DrawContent();
                    ImGui::EndChild();
#endif
                }
                f3_end(f3_pos_scene, f3_size_scene);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("AI")) { s_f3_active_tab = 1;
            if (!g_det_ai) {
                if (det_right("Detach##ai")) g_det_ai = true;
                ImGui::Separator();
                ImVec2 av = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("##f3dir", {av.x * 0.50f, av.y}, false);
                EditorDirectorPanel::Get().DrawContent();
                ImGui::EndChild();
                ImGui::SameLine(0, 4);
                ImGui::BeginChild("##f3vc", {0.f, av.y}, false);
                EditorViewConePanel::Get().DrawContent();
                ImGui::EndChild();
            } else {
                if (f3_pos_ai.y < min_y) f3_pos_ai.y = min_y;
                ImGui::SetNextWindowPos(f3_pos_ai, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(f3_size_ai, ImGuiCond_Appearing);
                if (ImGui::Begin("AI##float", &g_det_ai, FLOAT_FLAGS)) {
                    if (ImGui::BeginMenuBar()) {
                        if (det_right("Dock##ai")) g_det_ai = false;
                        ImGui::EndMenuBar();
                    }
                    ImVec2 av = ImGui::GetContentRegionAvail();
                    ImGui::BeginChild("##fdir", {av.x * 0.50f, av.y}, false);
                    EditorDirectorPanel::Get().DrawContent();
                    ImGui::EndChild();
                    ImGui::SameLine(0, 4);
                    ImGui::BeginChild("##fvc", {0.f, av.y}, false);
                    EditorViewConePanel::Get().DrawContent();
                    ImGui::EndChild();
                }
                f3_end(f3_pos_ai, f3_size_ai);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Animation")) { s_f3_active_tab = 2;
            if (!g_det_anim) {
                if (det_right("Detach##anim")) g_det_anim = true;
                ImGui::Separator();
                ImVec2 av = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("##f3an", {av.x, 180.f}, false);
                EditorAnimationPanel::Get().DrawContent();
                ImGui::EndChild();
                ImGui::BeginChild("##f3sq", {av.x, 0.f}, false);
                EditorSequencerPanel::Get().DrawContent();
                ImGui::EndChild();
            } else {
                if (f3_pos_anim.y < min_y) f3_pos_anim.y = min_y;
                ImGui::SetNextWindowPos(f3_pos_anim, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(f3_size_anim, ImGuiCond_Appearing);
                if (ImGui::Begin("Animation##float", &g_det_anim, FLOAT_FLAGS)) {
                    if (ImGui::BeginMenuBar()) {
                        if (det_right("Dock##anim")) g_det_anim = false;
                        ImGui::EndMenuBar();
                    }
                    ImVec2 av = ImGui::GetContentRegionAvail();
                    ImGui::BeginChild("##fan", {av.x, 180.f}, false);
                    EditorAnimationPanel::Get().DrawContent();
                    ImGui::EndChild();
                    ImGui::BeginChild("##fsq", {av.x, 0.f}, false);
                    EditorSequencerPanel::Get().DrawContent();
                    ImGui::EndChild();
                }
                f3_end(f3_pos_anim, f3_size_anim);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("FlowGraph")) { s_f3_active_tab = 3;
            if (!g_det_flow) {
                if (det_right("Detach##flow")) g_det_flow = true;
                ImGui::Separator();
                EditorFlowGraphPanel::Get().DrawContent();
            } else {
                if (f3_pos_flow.y < min_y) f3_pos_flow.y = min_y;
                ImGui::SetNextWindowPos(f3_pos_flow, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(f3_size_flow, ImGuiCond_Appearing);
                if (ImGui::Begin("FlowGraph##float", &g_det_flow, FLOAT_FLAGS)) {
                    if (ImGui::BeginMenuBar()) {
                        if (det_right("Dock##flow")) g_det_flow = false;
                        ImGui::EndMenuBar();
                    }
                    EditorFlowGraphPanel::Get().DrawContent();
                }
                f3_end(f3_pos_flow, f3_size_flow);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug")) { s_f3_active_tab = 4;
            if (!g_det_debug) {
                if (det_right("Detach##debug")) g_det_debug = true;
                ImGui::Separator();
                ImVec2 av = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("##f3con", {av.x * 0.60f, av.y}, false);
                EditorConsole::Get().DrawContent();
                ImGui::EndChild();
                ImGui::SameLine(0, 4);
                ImGui::BeginChild("##f3gpu", {0.f, av.y}, false);
                EditorGpuProfilerPanel::Get().DrawContent();
                ImGui::EndChild();
            } else {
                if (f3_pos_debug.y < min_y) f3_pos_debug.y = min_y;
                ImGui::SetNextWindowPos(f3_pos_debug, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(f3_size_debug, ImGuiCond_Appearing);
                if (ImGui::Begin("Debug##float", &g_det_debug, FLOAT_FLAGS)) {
                    if (ImGui::BeginMenuBar()) {
                        if (det_right("Dock##debug")) g_det_debug = false;
                        ImGui::EndMenuBar();
                    }
                    ImVec2 av = ImGui::GetContentRegionAvail();
                    ImGui::BeginChild("##fcon", {av.x * 0.60f, av.y}, false);
                    EditorConsole::Get().DrawContent();
                    ImGui::EndChild();
                    ImGui::SameLine(0, 4);
                    ImGui::BeginChild("##fgpu", {0.f, av.y}, false);
                    EditorGpuProfilerPanel::Get().DrawContent();
                    ImGui::EndChild();
                }
                f3_end(f3_pos_debug, f3_size_debug);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Camera")) { s_f3_active_tab = 5;
            if (!g_det_cam) {
                if (det_right("Detach##cam")) g_det_cam = true;
                ImGui::Separator();
                EditorCameraPanel::Get().DrawContent();
            } else {
                if (f3_pos_cam.y < min_y) f3_pos_cam.y = min_y;
                ImGui::SetNextWindowPos(f3_pos_cam, ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(f3_size_cam, ImGuiCond_Appearing);
                if (ImGui::Begin("Camera##float", &g_det_cam, FLOAT_FLAGS)) {
                    if (ImGui::BeginMenuBar()) {
                        if (det_right("Dock##cam")) g_det_cam = false;
                        ImGui::EndMenuBar();
                    }
                    EditorCameraPanel::Get().DrawContent();
                }
                f3_end(f3_pos_cam, f3_size_cam);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) { s_f3_active_tab = 6;
            bool&   g_det_set  = f3_det_settings;
            ImVec2& f3_pos_set = f3_pos_settings;
            ImVec2& f3_size_set = f3_size_settings;
            static char   s_st_msg[64] = {};
            static float  s_st_t = 0.f;
            if (s_st_t > 0.f) s_st_t -= io.DeltaTime;
            auto draw_settings = [&]() {
                ImGui::TextUnformatted("F3 Flythrough Camera");
                ImGui::Separator();
                ImGui::SetNextItemWidth(220.f);
                ImGui::SliderFloat("Fly speed (m/s)##fset", &cam_speed, 5.f, 5000.f, "%.0f",
                                   ImGuiSliderFlags_Logarithmic);
                if (ImGui::Button("Save config##fset")) {
                    bool det[6]   = {f3_det_scene, f3_det_ai, f3_det_anim, f3_det_flow, f3_det_debug, f3_det_cam};
                    ImVec2 pos[6] = {f3_pos_scene, f3_pos_ai, f3_pos_anim, f3_pos_flow, f3_pos_debug, f3_pos_cam};
                    ImVec2 siz[6] = {f3_size_scene, f3_size_ai, f3_size_anim, f3_size_flow, f3_size_debug, f3_size_cam};
                    F3SettingsState ss{f3_det_settings, f3_pos_settings, f3_size_settings};
                    f3_save(det, pos, siz, cam_speed, ss);
                    snprintf(s_st_msg, sizeof(s_st_msg), "Saved");
                    s_st_t = 2.f;
                }
                if (s_st_t > 0.f) {
                    ImGui::SameLine();
                    ImGui::TextColored({0.4f, 0.9f, 0.4f, 1.f}, "%s", s_st_msg);
                }
            };
            if (!g_det_set) {
                if (det_right("Detach##set")) g_det_set = true;
                ImGui::Separator();
                draw_settings();
            } else {
                if (f3_pos_set.y < min_y) f3_pos_set.y = min_y;
                ImGui::SetNextWindowPos(f3_pos_set,  ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(f3_size_set, ImGuiCond_Appearing);
                if (ImGui::Begin("Settings##float", &g_det_set, FLOAT_FLAGS)) {
                    if (ImGui::BeginMenuBar()) {
                        if (det_right("Dock##set")) g_det_set = false;
                        ImGui::EndMenuBar();
                    }
                    draw_settings();
                }
                f3_end(f3_pos_set, f3_size_set);
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
#endif
}

void EditorCore::Shutdown() {
    bool det6[6]  = { f3_det_scene, f3_det_ai, f3_det_anim, f3_det_flow, f3_det_debug, f3_det_cam };
    ImVec2 pos6[6] = { f3_pos_scene, f3_pos_ai, f3_pos_anim, f3_pos_flow, f3_pos_debug, f3_pos_cam };
    ImVec2 siz6[6] = { f3_size_scene, f3_size_ai, f3_size_anim, f3_size_flow, f3_size_debug, f3_size_cam };
    F3SettingsState set6{f3_det_settings, f3_pos_settings, f3_size_settings};
    f3_save(det6, pos6, siz6, cam_speed, set6);
    EditorNodeGraphPanel::Get().Shutdown();
    EditorFlowGraphPanel::Get().Shutdown();
    DeselectAll();
}

// ── Selection ─────────────────────────────────────────────────
MdEntity EditorCore::GetPrimary() const {
    return (selected_count > 0) ? selected[0] : MdEntity::Null();
}

void EditorCore::Select(MdEntity e, bool add) {
    if (!add) DeselectAll();
    if (IsSelected(e)) return;
    if (selected_count >= MAX_SELECTED) return;
    selected[selected_count++] = e;
}

void EditorCore::Deselect(MdEntity e) {
    for (int i = 0; i < selected_count; ++i) {
        if (selected[i] == e) {
            selected[i] = selected[--selected_count];
            selected[selected_count] = MdEntity::Null();
            return;
        }
    }
}

void EditorCore::DeselectAll() {
    for (int i = 0; i < selected_count; ++i) selected[i] = MdEntity::Null();
    selected_count = 0;
}

bool EditorCore::IsSelected(MdEntity e) const {
    for (int i = 0; i < selected_count; ++i)
        if (selected[i] == e) return true;
    return false;
}

// ── Editor Camera ─────────────────────────────────────────────
void EditorCore::UpdateEditorCamera(float dt, bool viewport_hovered) {
    (void)viewport_hovered;

    ImGuiIO& io = ImGui::GetIO();

    // Relative mouse mode for unlimited 360° rotation on RMB (no window-edge clamping).
    bool rmb_down = io.MouseDown[ImGuiMouseButton_Right];
    {
        static bool s_rel = false;
        if (rmb_down != s_rel) {
            SDL_SetWindowRelativeMouseMode(_wnd::ptr(), rmb_down);
            s_rel = rmb_down;
        }
    }
    // SDL raw delta — unlimited, not clamped to window edges.
    float rdx = 0.f, rdy = 0.f;
    SDL_GetRelativeMouseState(&rdx, &rdy);
    if (!rmb_down) { rdx = 0.f; rdy = 0.f; }

    if (cam_flying) {
        // ── Exact copy of editor_world_3d_sdlgpu handle_input() ──────────────
        // RMB = look; fly_yaw/fly_pitch in RADIANS (same as 3D World s_yaw/s_pitch)
        if (rmb_down) {
            fly_yaw   -= rdx * 0.003f;
            fly_pitch += rdy * 0.002f;
            if (fly_pitch < -0.3f) fly_pitch = -0.3f;
            if (fly_pitch >  1.3f) fly_pitch =  1.3f;
        }
        float sy = sinf(fly_yaw), cy2 = cosf(fly_yaw);
        const bool* kb = (const bool*)SDL_GetKeyboardState(nullptr);
        bool shift = kb[SDL_SCANCODE_LSHIFT] || kb[SDL_SCANCODE_RSHIFT];
        float sp = cam_speed * dt;
        if (kb[SDL_SCANCODE_W]||kb[SDL_SCANCODE_UP])       { cam_target.x+=sp*sy; cam_target.z+=sp*cy2; }
        if (kb[SDL_SCANCODE_S]||kb[SDL_SCANCODE_DOWN])     { cam_target.x-=sp*sy; cam_target.z-=sp*cy2; }
        if (kb[SDL_SCANCODE_A])                            { cam_target.x+=sp*cy2; cam_target.z-=sp*sy; }
        if (kb[SDL_SCANCODE_D])                            { cam_target.x-=sp*cy2; cam_target.z+=sp*sy; }
        if (kb[SDL_SCANCODE_Q]||kb[SDL_SCANCODE_PAGEDOWN]) cam_target.y -= sp;
        if (kb[SDL_SCANCODE_E]||kb[SDL_SCANCODE_PAGEUP])   cam_target.y += sp;
        // Use input_get_scroll_y() — io.MouseWheel is 0 here because
        // UpdateEditorCamera() runs BEFORE imgui_new_frame() in the game loop.
        float wheel = input_get_scroll_y();
        if (wheel == 0.f) wheel = io.MouseWheel; // fallback for standalone editor
        if (wheel != 0.f) {
            if (shift) {
                cam_speed = (wheel > 0)
                    ? fminf(cam_speed * 1.25f, 80000.f)
                    : fmaxf(cam_speed * 0.80f, 10.f);
            } else {
                float step = cam_target.y * 0.03f * wheel;
                cam_target.x += step * sy; cam_target.z += step * cy2;
                cam_target.y = (wheel > 0)
                    ? fmaxf(cam_target.y * 0.94f, 1.f)
                    : fminf(cam_target.y * 1.06f, 150000.f);
            }
        }
        if (TerrainAtlas_Loaded()) {
            float th = TerrainAtlas_SampleWorld(cam_target.x, cam_target.z);
            if (cam_target.y < th + 1.5f) cam_target.y = th + 1.5f;
        }
        float cp = cosf(fly_pitch), sp2 = sinf(fly_pitch);
        editor_cam.pos    = cam_target;
        editor_cam.target = { cam_target.x + sy*cp,
                              cam_target.y - sp2,
                              cam_target.z + cy2*cp };
        editor_cam.up     = { 0.f, 1.f, 0.f };
        return;
    } else {
        // Orbit mode — unlimited yaw (360°), pitch clamped near-vertical.
        if (rmb_down) {
            cam_yaw   -= rdx * 0.4f;
            cam_pitch  += rdy * 0.4f;
            if (cam_pitch >  89.f) cam_pitch =  89.f;
            if (cam_pitch < -89.f) cam_pitch = -89.f;
        }
        // MMB pan
        if (io.MouseDown[ImGuiMouseButton_Middle]) {
            float yaw_r = cam_yaw * DEG2R;
            Vec3 right = { cosf(yaw_r), 0.f, -sinf(yaw_r) };
            Vec3 up    = { 0.f, 1.f, 0.f };
            float pan = cam_dist * 0.002f;
            cam_target = vec3_sub(cam_target, vec3_scale(right, io.MouseDelta.x * pan));
            cam_target = vec3_add(cam_target, vec3_scale(up,    io.MouseDelta.y * pan));
        }
        // Scroll zoom
        cam_dist -= io.MouseWheel * cam_dist * 0.1f;
        if (cam_dist < 1.f)   cam_dist = 1.f;
        if (cam_dist > 500.f) cam_dist = 500.f;
    }

    float yaw_r   = cam_yaw   * DEG2R;
    float pitch_r = cam_pitch * DEG2R;
    editor_cam.pos = {
        cam_target.x + cam_dist * cosf(pitch_r) * sinf(yaw_r),
        cam_target.y + cam_dist * sinf(pitch_r),
        cam_target.z + cam_dist * cosf(pitch_r) * cosf(yaw_r)
    };
    // Terrain floor clamp — prevent eye clipping through surface in both
    // Flythrough (cam_target moves via WASD) and Orbit (cam_dist shrinks) modes.
    if (TerrainAtlas_Loaded()) {
        static constexpr float MIN_ABOVE_TERRAIN = 1.5f;
        float th = TerrainAtlas_SampleWorld(editor_cam.pos.x, editor_cam.pos.z);
        if (editor_cam.pos.y < th + MIN_ABOVE_TERRAIN) {
            float dy = (th + MIN_ABOVE_TERRAIN) - editor_cam.pos.y;
            editor_cam.pos.y = th + MIN_ABOVE_TERRAIN;
            cam_target.y    += dy;  // shift target with eye so look direction is preserved
        }
    }
    editor_cam.target = cam_target;
    editor_cam.up     = { 0.f, 1.f, 0.f };
}

void EditorCore::FocusOnSelected() {
    if (selected_count == 0) return;
    auto& reg = MdRegistry::Get();
    MdEntity e = GetPrimary();
    if (!reg.Valid(e)) return;
    if (!(reg.Handle(e).has<WorldTransform>())) return;
    const auto& tr = reg.Handle(e).get_mut<WorldTransform>();
    cam_target = Vec3{ tr.x, 0.f, tr.z };
    cam_dist   = 15.f;
}

// ── History ───────────────────────────────────────────────────
void EditorCore::Undo() {
    history.Undo(MdRegistry::Get());
}

void EditorCore::Redo() {
    history.Redo(MdRegistry::Get());
}
#endif
