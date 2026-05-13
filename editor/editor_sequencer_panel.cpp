#ifdef MONKEY_DUST_EDITOR
#include "editor_sequencer_panel.h"
#include "editor_core.h"
#include <cstring>
#include <cstdio>

// ── MdSeq interface impl ──────────────────────────────────────────────────────

void EditorSequencerPanel::MdSeq::Get(int idx, int** start, int** end,
                                       int* type, unsigned int* color) {
    if (idx < 0 || idx >= *count) return;
    static int s, e;
    s = entries[idx].frame_start;
    e = entries[idx].frame_end;
    if (start) *start = &s;
    if (end)   *end   = &e;
    if (type)  *type  = 0;
    if (color) {
        static const unsigned int kPal[8] = {
            0xFF4080FF, 0xFF40FF80, 0xFFFF8040,
            0xFFFF4080, 0xFF80FF40, 0xFF8040FF,
            0xFFFF8080, 0xFF80FFFF
        };
        *color = kPal[entries[idx].color_idx & 7];
    }
}

void EditorSequencerPanel::MdSeq::Add(int /*type*/) {
    if (*count >= 32) return;
    SeqEntry& e = entries[(*count)++];
    strncpy(e.label, "New Clip", sizeof(e.label) - 1);
    e.frame_start = 0;
    e.frame_end   = 24;
    e.color_idx   = (*count - 1) & 7;
}

void EditorSequencerPanel::MdSeq::Del(int idx) {
    if (idx < 0 || idx >= *count) return;
    for (int i = idx; i < *count - 1; ++i) entries[i] = entries[i + 1];
    --(*count);
}

void EditorSequencerPanel::MdSeq::Duplicate(int idx) {
    if (idx < 0 || idx >= *count || *count >= 32) return;
    entries[*count]             = entries[idx];
    entries[*count].frame_start += 4;
    entries[*count].frame_end   += 4;
    entries[*count].color_idx   = (*count) & 7;
    ++(*count);
}

// ── Default clip set ──────────────────────────────────────────────────────────

void EditorSequencerPanel::InitDefaults() {
    struct { const char* n; int s, e; } defs[] = {
        {"Idle",   0,  23}, {"Walk", 24,  47},
        {"Run",   48,  79}, {"Attack",80, 109},
        {"Die",  110, 140},
    };
    for (auto& d : defs) {
        if (entry_count_ >= MAX_ENTRIES) break;
        SeqEntry& en  = entries_[entry_count_];
        strncpy(en.label, d.n, sizeof(en.label) - 1);
        en.frame_start = d.s;
        en.frame_end   = d.e;
        en.color_idx   = entry_count_ & 7;
        ++entry_count_;
    }
}

// ── Draw ──────────────────────────────────────────────────────────────────────

void EditorSequencerPanel::Draw() {
    if (!EditorCore::Get().panels_visible[13]) return;

    if (!initialized_) {
        seq_.entries   = entries_;
        seq_.count     = &entry_count_;
        seq_.frame_min = 0;
        seq_.frame_max = 240;
        InitDefaults();
        initialized_ = true;
    }

    ImGui::SetNextWindowSize(ImVec2(820, 230), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(200, 590), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sequencer##seq", &EditorCore::Get().panels_visible[13])) {
        ImGui::End(); return;
    }

    ImGui::Text("Frame: %d  |  Clips: %d", current_frame_, entry_count_);
    ImGui::SameLine(ImGui::GetWindowWidth() - 150.f);
    ImGui::SetNextItemWidth(130.f);
    ImGui::SliderInt("Max##seqmax", &seq_.frame_max, 60, 1200);

    ImSequencer::Sequencer(
        &seq_,
        &current_frame_,
        &expanded_,
        &selected_entry_,
        &first_frame_,
        ImSequencer::SEQUENCER_EDIT_STARTEND |
        ImSequencer::SEQUENCER_ADD           |
        ImSequencer::SEQUENCER_DEL           |
        ImSequencer::SEQUENCER_CHANGE_FRAME
    );

    // Inline label editor for selected clip
    if (selected_entry_ >= 0 && selected_entry_ < entry_count_) {
        ImGui::Separator();
        SeqEntry& sel = entries_[selected_entry_];
        ImGui::SetNextItemWidth(200.f);
        ImGui::InputText("Label##seql", sel.label, sizeof(sel.label));
        ImGui::SameLine();
        ImGui::Text("  [%d – %d]  (%d frames)",
            sel.frame_start, sel.frame_end,
            sel.frame_end - sel.frame_start + 1);
    }

    ImGui::End();
}

#endif // MONKEY_DUST_EDITOR
