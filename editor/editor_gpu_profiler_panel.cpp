#ifdef MONKEY_DUST_EDITOR
#include "editor_gpu_profiler_panel.h"
#include "editor_core.h"
#include "imgui.h"
#include <monkey_dust/render/gpu_profiler.h>

void EditorGpuProfilerPanel::Draw() {
    if (!EditorCore::Get().panels_visible[11]) return;

    ImGui::SetNextWindowSize(ImVec2(320, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(980, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("GPU Profiler##gp", &EditorCore::Get().panels_visible[11])) {
        ImGui::End(); return;
    }

    auto& prof = md::GpuProfiler::Get();
    int   n    = prof.ResultCount();
    float tot  = prof.TotalMs();

    ImGui::Text("Total frame: %.2f ms  (%.0f FPS est.)",
                tot, tot > 0.001f ? 1000.0f / tot : 0.0f);
    ImGui::Separator();

    if (n == 0) {
        ImGui::TextDisabled("No passes recorded.");
        ImGui::TextDisabled("Call GpuProfiler::Get().BeginPass/EndPass");
        ImGui::TextDisabled("from render systems to populate.");
        ImGui::End(); return;
    }

    // Budget reference: 16.67 ms = 60 FPS
    constexpr float BUDGET_MS = 16.67f;

    for (int i = 0; i < n; ++i) {
        const auto& r = prof.GetResult(i);
        float frac = (tot > 0.001f) ? (r.cpu_ms / tot) : 0.0f;
        float budget_frac = r.cpu_ms / BUDGET_MS;

        // Color: green < 33% budget, yellow < 66%, red ≥ 66%
        ImVec4 col = budget_frac < 0.33f ? ImVec4(0.2f, 0.9f, 0.3f, 1.f) :
                     budget_frac < 0.66f ? ImVec4(1.0f, 0.8f, 0.1f, 1.f) :
                                           ImVec4(1.0f, 0.2f, 0.1f, 1.f);

        char label[64];
        snprintf(label, sizeof(label), "%.2f ms  %s", r.cpu_ms, r.name);

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(frac, ImVec2(-1, 16), label);
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::Text("Budget (60 FPS): 16.67 ms");

    ImGui::End();
}
#endif
