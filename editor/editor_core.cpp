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
#include <cstring>

static constexpr float DEG2R = 3.14159265f / 180.f;

void EditorCore::Init() {
    EditorConsole::Get().Init();
    EditorNodeGraphPanel::Get().Init();
    for (int i = 0; i < MAX_SELECTED; ++i)
        selected[i] = entt::null;

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

// Right-aligned Detach button pinned to the top-right of the current tab content area.
// Uses SetCursorScreenPos (absolute screen coords) to avoid BeginTabItem cursor ambiguity.
// After the button, cursor is restored to (left, one line below the button).
static bool f3_detach_btn(const char* id) {
    ImVec2 cs  = ImGui::GetCursorScreenPos();   // top-left of content area (screen)
    float  ww  = ImGui::GetWindowSize().x;
    float  wx  = ImGui::GetWindowPos().x;
    float  bw  = ImGui::CalcTextSize("Detach").x + ImGui::GetStyle().FramePadding.x * 2.f;
    float  bh  = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos({wx + ww - bw - 4.f, cs.y});
    bool clicked = ImGui::Button(id);
    ImGui::SetCursorScreenPos({cs.x, cs.y + bh + ImGui::GetStyle().ItemSpacing.y});
    return clicked;
}

void EditorCore::Update(float dt) {
    EditorToolbar::Get().Draw(dt);
#ifndef MONKEY_DUST_STANDALONE_EDITOR
    static bool g_det_scene = false, g_det_ai    = false, g_det_anim  = false;
    static bool g_det_flow  = false, g_det_debug = false, g_det_cam   = false;

    ImGuiIO& io = ImGui::GetIO();
    float toolbar_h = ImGui::GetFrameHeight() + 30.f;

    ImGui::SetNextWindowPos({0.f, toolbar_h});
    ImGui::SetNextWindowSize({io.DisplaySize.x, io.DisplaySize.y - toolbar_h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
    ImGui::Begin("##f3editor", nullptr,
        ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();

    if (ImGui::BeginTabBar("##f3tabs")) {
        if (ImGui::BeginTabItem("Scene")) {
            if (!g_det_scene) {
                if (f3_detach_btn("Detach##scene")) g_det_scene = true;
                ImVec2 av = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("##f3h", {av.x * 0.30f, av.y}, false);
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
                if (f3_detach_btn("Detach##ai")) g_det_ai = true;
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
                if (f3_detach_btn("Detach##anim")) g_det_anim = true;
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
                if (f3_detach_btn("Detach##flow")) g_det_flow = true;
                EditorFlowGraphPanel::Get().DrawContent();
            } else { ImGui::TextDisabled("(detached)"); }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug")) {
            if (!g_det_debug) {
                if (f3_detach_btn("Detach##debug")) g_det_debug = true;
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
                if (f3_detach_btn("Detach##cam")) g_det_cam = true;
                EditorCameraPanel::Get().DrawContent();
            } else { ImGui::TextDisabled("(detached)"); }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    // ── floating panels (rendered outside ##f3editor) ─────────────────────
    if (g_det_scene) {
        ImGui::SetNextWindowPos({50.f, 60.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({700, 500}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Scene##float", &g_det_scene)) {
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
        ImGui::End();
    }
    if (g_det_ai) {
        ImGui::SetNextWindowPos({80.f, 90.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({700, 500}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("AI##float", &g_det_ai)) {
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
        ImGui::End();
    }
    if (g_det_anim) {
        ImGui::SetNextWindowPos({110.f, 120.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({700, 500}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Animation##float", &g_det_anim)) {
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
        ImGui::End();
    }
    if (g_det_flow) {
        ImGui::SetNextWindowPos({140.f, 150.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({700, 500}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("FlowGraph##float", &g_det_flow)) {
            if (ImGui::Button("Dock##flow")) g_det_flow = false;
            ImGui::Separator();
            EditorFlowGraphPanel::Get().DrawContent();
        }
        ImGui::End();
    }
    if (g_det_debug) {
        ImGui::SetNextWindowPos({170.f, 180.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({700, 500}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Debug##float", &g_det_debug)) {
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
        ImGui::End();
    }
    if (g_det_cam) {
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({ds.x - 420.f, 30.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({400, 300}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Camera##float", &g_det_cam)) {
            if (ImGui::Button("Dock##cam")) g_det_cam = false;
            ImGui::Separator();
            EditorCameraPanel::Get().DrawContent();
        }
        ImGui::End();
    }
#endif
}

void EditorCore::Shutdown() {
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
