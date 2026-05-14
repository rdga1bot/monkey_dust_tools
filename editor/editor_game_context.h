#pragma once

// Minimal callback bridge between editor and game-specific systems
// (DialogSystem, QuestSystem) that depend on GameState.
// game/src/main.cpp fills these before opening the editor.
struct EditorGameContext {
    void (*reload_dialogs)(const char* path) = nullptr;
    void (*reload_quests)(const char*  path) = nullptr;

    static EditorGameContext& Get() {
        static EditorGameContext inst;
        return inst;
    }
};
