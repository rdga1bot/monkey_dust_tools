#pragma once
#ifdef MONKEY_DUST_EDITOR

#include "ImSequencer.h"  // third_party/imguizmo/ImSequencer.h

// Animation / cutscene timeline using ImGuizmo ImSequencer (MIT).
// Panel index 13.  Stores up to 32 named clip ranges editable by drag.
class EditorSequencerPanel {
public:
    static EditorSequencerPanel& Get() {
        static EditorSequencerPanel i; return i;
    }
    void Draw();

private:
    EditorSequencerPanel() = default;

    struct SeqEntry {
        char label[32];
        int  frame_start;
        int  frame_end;
        int  color_idx;   // 0–7 selects built-in palette
    };

    // ── ImSequencer adapter (inner struct, no heap) ────────────────────────
    struct MdSeq : public ImSequencer::SequenceInterface {
        SeqEntry* entries;
        int*      count;
        int       frame_min = 0;
        int       frame_max = 240;

        int  GetFrameMin() const override { return frame_min; }
        int  GetFrameMax() const override { return frame_max; }
        int  GetItemCount() const override { return *count; }
        const char* GetItemLabel(int i) const override {
            return (i >= 0 && i < *count) ? entries[i].label : "";
        }
        void Get(int idx, int** start, int** end,
                 int* type, unsigned int* color) override;
        void Add(int type) override;
        void Del(int idx)  override;
        void Duplicate(int idx) override;
    };

    static constexpr int MAX_ENTRIES = 32;
    SeqEntry entries_[MAX_ENTRIES] = {};
    int      entry_count_ = 0;
    MdSeq    seq_;

    int  current_frame_  = 0;
    int  first_frame_    = 0;
    bool expanded_       = true;
    int  selected_entry_ = -1;
    bool initialized_    = false;

    void InitDefaults();
};

#endif // MONKEY_DUST_EDITOR
