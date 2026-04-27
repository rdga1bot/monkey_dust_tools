#pragma once
#ifdef MONKEY_DUST_EDITOR

class EditorCameraPanel {
public:
    static EditorCameraPanel& Get() { static EditorCameraPanel inst; return inst; }
    void Draw();
private:
    EditorCameraPanel() = default;
};
#endif
