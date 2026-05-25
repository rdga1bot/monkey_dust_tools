#ifdef MONKEY_DUST_EDITOR
#include "editor_core.h"
#include "editor_toolbar.h"
#include "editor_hierarchy.h"
#include "editor_inspector.h"
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
#include <cmath>
#include <cstdio>
#include <cstring>

static constexpr float DEG2R = 3.14159265f / 180.f;

static constexpr const char* F3_LAYOUT_PATH = "data/editor_f3_layout.json";

// Version tag written by f3_save; file without it is from a different/stale session — ignored.
static constexpr const char* F3_VERSION_TAG = "\"v\":1";

static float f3_parse_f(const char* buf, const char* key) {
    const char* p = strstr(buf, key); if (!p) return 0.f;
    p += strlen(key);
    while (*p == ':' || *p == ' ') ++p;
    return (float)atof(p);
}

static void f3_load(bool det[6], ImVec2 pos[6], ImVec2 siz[6]) {
    FILE* f = fopen(F3_LAYOUT_PATH, "rb");
    if (!f) return;
    char buf[512]; size_t n = fread(buf, 1, 511, f); buf[n] = '\0'; fclose(f);
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
}

static void f3_save(const bool det[6], const ImVec2 pos[6], const ImVec2 siz[6]) {
    FILE* f = fopen(F3_LAYOUT_PATH, "w"); if (!f) return;
    fprintf(f,
        "{\"v\":1,\"scene\":%d,\"ai\":%d,\"anim\":%d,\"flow\":%d,\"debug\":%d,\"cam\":%d,"
        "\"scx\":%.0f,\"scy\":%.0f,\"scw\":%.0f,\"sch\":%.0f,"
        "\"aix\":%.0f,\"aiy\":%.0f,\"aiw\":%.0f,\"aih\":%.0f,"
        "\"anx\":%.0f,\"any\":%.0f,\"anw\":%.0f,\"anh\":%.0f,"
        "\"flx\":%.0f,\"fly\":%.0f,\"flw\":%.0f,\"flh\":%.0f,"
        "\"dbx\":%.0f,\"dby\":%.0f,\"dbw\":%.0f,\"dbh\":%.0f,"
        "\"cmx\":%.0f,\"cmy\":%.0f,\"cmw\":%.0f,\"cmh\":%.0f}\n",
        (int)det[0],(int)det[1],(int)det[2],(int)det[3],(int)det[4],(int)det[5],
        pos[0].x,pos[0].y,siz[0].x,siz[0].y,
        pos[1].x,pos[1].y,siz[1].x,siz[1].y,
        pos[2].x,pos[2].y,siz[2].x,siz[2].y,
        pos[3].x,pos[3].y,siz[3].x,siz[3].y,
        pos[4].x,pos[4].y,siz[4].x,siz[4].y,
        pos[5].x,pos[5].y,siz[5].x,siz[5].y);
    fclose(f);
}

void EditorCore::Init() {
    EditorConsole::Get().Init();
    EditorNodeGraphPanel::Get().Init();
    for (int i = 0; i < MAX_SELECTED; ++i)
        selected[i] = entt::null;

    bool det6[6] = {};
    ImVec2 pos6[6] = { f3_pos_scene, f3_pos_ai, f3_pos_anim, f3_pos_flow, f3_pos_debug, f3_pos_cam };
    ImVec2 siz6[6] = { f3_size_scene, f3_size_ai, f3_size_anim, f3_size_flow, f3_size_debug, f3_size_cam };
    f3_load(det6, pos6, siz6);
    f3_det_scene = det6[0];  f3_det_ai    = det6[1]; f3_det_anim  = det6[2];
    f3_det_flow  = det6[3];  f3_det_debug = det6[4]; f3_det_cam   = det6[5];
    f3_pos_scene = pos6[0];  f3_pos_ai    = pos6[1]; f3_pos_anim  = pos6[2];
    f3_pos_flow  = pos6[3];  f3_pos_debug = pos6[4]; f3_pos_cam   = pos6[5];
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
    float toolbar_h = ImGui::GetFrameHeight() + 30.f;

    // Fullscreen editor — same layout as monkey_dust_editor standalone.
    // Solid background, tab bar with embedded content, Detach/Dock per panel.
    ImGui::SetNextWindowPos({0.f, toolbar_h});
    ImGui::SetNextWindowSize({io.DisplaySize.x, io.DisplaySize.y - toolbar_h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
    ImGui::Begin("##f3editor", nullptr,
        ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    if (ImGui::BeginTabBar("##f3tabs")) {
        if (ImGui::BeginTabItem("Scene")) {
            if (!g_det_scene) {
                if (ImGui::SmallButton("Detach##scene")) g_det_scene = true;
                ImGui::Separator();
                ImVec2 av = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("##f3h", {300.f, av.y}, false);
                EditorHierarchy::Get().DrawContent();
                ImGui::EndChild();
                ImGui::SameLine(0, 4);
                ImGui::BeginChild("##f3i", {0.f, av.y}, false);
                EditorInspector::Get().DrawContent();
                ImGui::EndChild();
            } else { ImGui::TextDisabled("(detached)"); }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("AI")) {
            if (!g_det_ai) {
                if (ImGui::SmallButton("Detach##ai")) g_det_ai = true;
                ImGui::Separator();
                ImVec2 av = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("##f3dir", {av.x * 0.50f, av.y}, false);
                EditorDirectorPanel::Get().DrawContent();
                ImGui::EndChild();
                ImGui::SameLine(0, 4);
                ImGui::BeginChild("##f3vc", {0.f, av.y}, false);
                EditorViewConePanel::Get().DrawContent();
                ImGui::EndChild();
            } else { ImGui::TextDisabled("(detached)"); }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Animation")) {
            if (!g_det_anim) {
                if (ImGui::SmallButton("Detach##anim")) g_det_anim = true;
                ImGui::Separator();
                ImVec2 av = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("##f3an", {av.x, 180.f}, false);
                EditorAnimationPanel::Get().DrawContent();
                ImGui::EndChild();
                ImGui::BeginChild("##f3sq", {av.x, 0.f}, false);
                EditorSequencerPanel::Get().DrawContent();
                ImGui::EndChild();
            } else { ImGui::TextDisabled("(detached)"); }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("FlowGraph")) {
            if (!g_det_flow) {
                if (ImGui::SmallButton("Detach##flow")) g_det_flow = true;
                ImGui::Separator();
                EditorFlowGraphPanel::Get().DrawContent();
            } else { ImGui::TextDisabled("(detached)"); }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug")) {
            if (!g_det_debug) {
                if (ImGui::SmallButton("Detach##debug")) g_det_debug = true;
                ImGui::Separator();
                ImVec2 av = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("##f3con", {av.x * 0.60f, av.y}, false);
                EditorConsole::Get().DrawContent();
                ImGui::EndChild();
                ImGui::SameLine(0, 4);
                ImGui::BeginChild("##f3gpu", {0.f, av.y}, false);
                EditorGpuProfilerPanel::Get().DrawContent();
                ImGui::EndChild();
            } else { ImGui::TextDisabled("(detached)"); }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Camera")) {
            if (!g_det_cam) {
                if (ImGui::SmallButton("Detach##cam")) g_det_cam = true;
                ImGui::Separator();
                EditorCameraPanel::Get().DrawContent();
            } else { ImGui::TextDisabled("(detached)"); }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    // ── floating panels (rendered outside ##f3editor) ─────────────────────
    // min_y: panel title bars must never overlap the toolbar + tab bar strip.
    // Without this clamp the title bar sits on top of the tab bar and the user
    // accidentally drags the panel when trying to click a tab.
    const float min_y = toolbar_h + ImGui::GetFrameHeight() + 4.f;
    static constexpr ImGuiWindowFlags FLOAT_FLAGS =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

    auto f3_end = [&](ImVec2& pos, ImVec2& sz) {
        pos = ImGui::GetWindowPos();
        if (pos.y < min_y) { pos.y = min_y; ImGui::SetWindowPos(pos); }
        sz = ImGui::GetWindowSize();
        ImGui::End();
    };

    if (g_det_scene) {
        if (f3_pos_scene.y < min_y) f3_pos_scene.y = min_y;
        ImGui::SetNextWindowPos(f3_pos_scene, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(f3_size_scene, ImGuiCond_Appearing);
        if (ImGui::Begin("Scene##float", &g_det_scene, FLOAT_FLAGS)) {
            if (ImGui::Button("Dock##scene")) g_det_scene = false;
            ImGui::Separator();
            ImVec2 av = ImGui::GetContentRegionAvail();
            ImGui::BeginChild("##fh", {av.x * 0.30f, av.y}, false);
            EditorHierarchy::Get().DrawContent();
            ImGui::EndChild();
            ImGui::SameLine(0, 4);
            ImGui::BeginChild("##fi", {0.f, av.y}, false);
            EditorInspector::Get().DrawContent();
            ImGui::EndChild();
        }
        f3_end(f3_pos_scene, f3_size_scene);
    }
    if (g_det_ai) {
        if (f3_pos_ai.y < min_y) f3_pos_ai.y = min_y;
        ImGui::SetNextWindowPos(f3_pos_ai, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(f3_size_ai, ImGuiCond_Appearing);
        if (ImGui::Begin("AI##float", &g_det_ai, FLOAT_FLAGS)) {
            if (ImGui::Button("Dock##ai")) g_det_ai = false;
            ImGui::Separator();
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
    if (g_det_anim) {
        if (f3_pos_anim.y < min_y) f3_pos_anim.y = min_y;
        ImGui::SetNextWindowPos(f3_pos_anim, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(f3_size_anim, ImGuiCond_Appearing);
        if (ImGui::Begin("Animation##float", &g_det_anim, FLOAT_FLAGS)) {
            if (ImGui::Button("Dock##anim")) g_det_anim = false;
            ImGui::Separator();
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
    if (g_det_flow) {
        if (f3_pos_flow.y < min_y) f3_pos_flow.y = min_y;
        ImGui::SetNextWindowPos(f3_pos_flow, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(f3_size_flow, ImGuiCond_Appearing);
        if (ImGui::Begin("FlowGraph##float", &g_det_flow, FLOAT_FLAGS)) {
            if (ImGui::Button("Dock##flow")) g_det_flow = false;
            ImGui::Separator();
            EditorFlowGraphPanel::Get().DrawContent();
        }
        f3_end(f3_pos_flow, f3_size_flow);
    }
    if (g_det_debug) {
        if (f3_pos_debug.y < min_y) f3_pos_debug.y = min_y;
        ImGui::SetNextWindowPos(f3_pos_debug, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(f3_size_debug, ImGuiCond_Appearing);
        if (ImGui::Begin("Debug##float", &g_det_debug, FLOAT_FLAGS)) {
            if (ImGui::Button("Dock##debug")) g_det_debug = false;
            ImGui::Separator();
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
    if (g_det_cam) {
        if (f3_pos_cam.y < min_y) f3_pos_cam.y = min_y;
        ImGui::SetNextWindowPos(f3_pos_cam, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(f3_size_cam, ImGuiCond_Appearing);
        if (ImGui::Begin("Camera##float", &g_det_cam, FLOAT_FLAGS)) {
            if (ImGui::Button("Dock##cam")) g_det_cam = false;
            ImGui::Separator();
            EditorCameraPanel::Get().DrawContent();
        }
        f3_end(f3_pos_cam, f3_size_cam);
    }
#endif
}

void EditorCore::Shutdown() {
    bool det6[6]  = { f3_det_scene, f3_det_ai, f3_det_anim, f3_det_flow, f3_det_debug, f3_det_cam };
    ImVec2 pos6[6] = { f3_pos_scene, f3_pos_ai, f3_pos_anim, f3_pos_flow, f3_pos_debug, f3_pos_cam };
    ImVec2 siz6[6] = { f3_size_scene, f3_size_ai, f3_size_anim, f3_size_flow, f3_size_debug, f3_size_cam };
    f3_save(det6, pos6, siz6);
    EditorNodeGraphPanel::Get().Shutdown();
    EditorFlowGraphPanel::Get().Shutdown();
    DeselectAll();
}

// ── Selection ─────────────────────────────────────────────────
entt::entity EditorCore::GetPrimary() const {
    return (selected_count > 0) ? selected[0] : entt::null;
}

void EditorCore::Select(entt::entity e, bool add) {
    if (!add) DeselectAll();
    if (IsSelected(e)) return;
    if (selected_count >= MAX_SELECTED) return;
    selected[selected_count++] = e;
}

void EditorCore::Deselect(entt::entity e) {
    for (int i = 0; i < selected_count; ++i) {
        if (selected[i] == e) {
            selected[i] = selected[--selected_count];
            selected[selected_count] = entt::null;
            return;
        }
    }
}

void EditorCore::DeselectAll() {
    for (int i = 0; i < selected_count; ++i) selected[i] = entt::null;
    selected_count = 0;
}

bool EditorCore::IsSelected(entt::entity e) const {
    for (int i = 0; i < selected_count; ++i)
        if (selected[i] == e) return true;
    return false;
}

// ── Editor Camera ─────────────────────────────────────────────
void EditorCore::UpdateEditorCamera(float dt, bool viewport_hovered) {
    (void)viewport_hovered;

    ImGuiIO& io = ImGui::GetIO();

    if (cam_flying) {
        if (io.MouseDown[ImGuiMouseButton_Right]) {
            cam_yaw   -= io.MouseDelta.x * 0.3f;
            cam_pitch  += io.MouseDelta.y * 0.3f;
            if (cam_pitch >  89.f) cam_pitch =  89.f;
            if (cam_pitch < -89.f) cam_pitch = -89.f;

            float yaw_r   = cam_yaw   * DEG2R;
            float pitch_r = cam_pitch * DEG2R;
            Vec3 fwd = {
                cosf(pitch_r) * sinf(yaw_r),
                sinf(pitch_r),
                cosf(pitch_r) * cosf(yaw_r)
            };
            Vec3 right = vec3_norm(vec3_cross(fwd, {0.f, 1.f, 0.f}));

            float speed = cam_speed * dt;
            if (input_key_down(KEY_W)) cam_target = vec3_add(cam_target, vec3_scale(fwd, -speed));
            if (input_key_down(KEY_S)) cam_target = vec3_add(cam_target, vec3_scale(fwd,  speed));
            if (input_key_down(KEY_A)) cam_target = vec3_add(cam_target, vec3_scale(right, speed));
            if (input_key_down(KEY_D)) cam_target = vec3_add(cam_target, vec3_scale(right,-speed));
        }
    } else {
        // Orbit mode
        if (io.MouseDown[ImGuiMouseButton_Right]) {
            cam_yaw   -= io.MouseDelta.x * 0.4f;
            cam_pitch  += io.MouseDelta.y * 0.4f;
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
    editor_cam.target = cam_target;
    editor_cam.up     = { 0.f, 1.f, 0.f };
}

void EditorCore::FocusOnSelected() {
    if (selected_count == 0) return;
    auto& reg = Registry::Get();
    entt::entity e = GetPrimary();
    if (!reg.valid(e)) return;
    if (!reg.all_of<WorldTransform>(e)) return;
    const auto& tr = reg.get<WorldTransform>(e);
    cam_target = Vec3{ tr.x, 0.f, tr.z };
    cam_dist   = 15.f;
}

// ── History ───────────────────────────────────────────────────
void EditorCore::Undo() {
    history.Undo(Registry::Get());
}

void EditorCore::Redo() {
    history.Redo(Registry::Get());
}
#endif
