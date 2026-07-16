#pragma once
// editor_cmd_file.h — live command-file automation channel (task #123).
//
// Unlike editor_scenario_driver.h's --exec mode (one Lua script, decided at
// process launch, run to completion via LuaSystem::StartScenario/
// ResumeScenario), this lets an ALREADY RUNNING editor instance execute
// arbitrary Lua commands one at a time, sent from outside the process
// without a relaunch — useful for interactive/iterative automation (e.g.
// probing camera angles + screenshots) where each next command depends on
// the previous result.
//
// Protocol (files under tmp_/editor_cmd/, all written by the sender via
// write-to-.tmp + rename to avoid a torn read):
//   cmd.lua     — Lua source of the pending command
//   cmd.seq     — integer, bumped LAST by the sender (signals new work)
//   result.json — {"seq":N,"ok":true|false,"error":"..."}, written by the
//                 editor after executing; seq echoes the command's seq.
//
// EditorCmdFile_Poll() is called once per frame from main.cpp's main loop,
// unconditionally (works whether or not --exec was passed). Cost when idle
// is a single small-file read (cmd.seq) — no directory scan, no malloc.

void EditorCmdFile_Poll();
