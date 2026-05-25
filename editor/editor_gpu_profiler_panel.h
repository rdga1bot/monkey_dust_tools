#pragma once
#ifdef MONKEY_DUST_EDITOR

class EditorGpuProfilerPanel {
public:
    static EditorGpuProfilerPanel& Get() { static EditorGpuProfilerPanel i; return i; }
    void Draw();
    void DrawContent();
private:
    EditorGpuProfilerPanel() = default;
};

#endif
