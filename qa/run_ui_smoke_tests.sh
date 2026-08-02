#!/usr/bin/env bash
# run_ui_smoke_tests.sh — EDITOR_AUTOMATION_PLAN_v1.md Phase 5.
#
# Runs the ImGui Test Engine panel smoke tests (9 real F3 editor panels:
# open tab -> 2 frames -> detach -> dock -> no crash/assert). Requires a
# monkey_dust_editor built with -DMD_UI_TESTS=ON (not the default —
# vendors third_party/imgui_test_engine, see AUTOMATION_FINDINGS.md).
#
# Usage:
#   cmake -S . -B build -G Ninja -DUSE_SDL3=ON -DMD_UI_TESTS=ON
#   ninja -C build monkey_dust_editor
#   bash tools/qa/run_ui_smoke_tests.sh

set -uo pipefail
cd "$(dirname "$0")/../.."   # repo root

EDITOR_BIN="build/tools/monkey_dust_editor"
if [[ ! -x "$EDITOR_BIN" ]]; then
  echo "FAIL: $EDITOR_BIN not found/executable — build with -DMD_UI_TESTS=ON first"
  exit 1
fi

timeout 90 "$EDITOR_BIN" --ui-tests
code=$?

if [[ $code -eq 0 ]]; then
  echo "PASS: ui_smoke_tests"
else
  echo "FAIL: ui_smoke_tests (exit=$code)"
fi
exit $code
