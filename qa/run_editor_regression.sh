#!/usr/bin/env bash
# run_editor_regression.sh — EDITOR_AUTOMATION_PLAN_v1.md Phase 4 regression suite.
#
# Runs every tests/editor_scenarios/regress_*.lua scenario against
# monkey_dust_editor --exec, aggregates PASS/FAIL, exits nonzero on any
# failure. Does NOT rebuild — run `ninja -C build monkey_dust_editor`
# yourself first (this suite is meant to be fast/repeatable, not to own
# the build step).
#
# Usage:
#   bash tools/qa/run_editor_regression.sh              # normal
#   bash tools/qa/run_editor_regression.sh --fast        # --fast flag passed through to --exec

set -uo pipefail
cd "$(dirname "$0")/../.."   # repo root

EDITOR_BIN="build/tools/monkey_dust_editor"
SCENARIO_DIR="tests/editor_scenarios"
FAST_FLAG="--fast"
for arg in "$@"; do
  case "$arg" in
    --no-fast) FAST_FLAG="" ;;
  esac
done

if [[ ! -x "$EDITOR_BIN" ]]; then
  echo "FAIL: $EDITOR_BIN not found/executable — build it first (ninja -C build monkey_dust_editor)"
  exit 1
fi

mkdir -p automation_out

PASS=0
FAIL=0
FAILED_NAMES=()

run_scenario() {
  local name="$1"
  local seed="$2"
  local log="automation_out/regress_${name}.log"
  timeout 90 "$EDITOR_BIN" --exec "$SCENARIO_DIR/regress_${name}.lua" \
      $FAST_FLAG --seed "$seed" --max-seconds 60 > "$log" 2>&1
  local code=$?
  if [[ $code -eq 0 ]]; then
    echo "PASS: $name"
    PASS=$((PASS+1))
  else
    echo "FAIL: $name (exit=$code, see $log)"
    FAIL=$((FAIL+1))
    FAILED_NAMES+=("$name")
  fi
}

run_scenario "hot_reload"       1
run_scenario "undo_redo"        2
run_scenario "save_load_dump"   7
run_scenario "path_allowlist"   3

# save_load_dump's real check: the two structural dumps must match after
# stripping known-volatile lines (fps= changes every frame; selection_count=
# differs because Import Scene doesn't restore Spawn Entity's auto-select —
# see regress_save_load_dump.lua's doc comment for both).
if [[ -f automation_out/regress_dump_before.txt && -f automation_out/regress_dump_after.txt ]]; then
  if diff -u \
      <(grep -vE '^\s*(fps|selection_count)=' automation_out/regress_dump_before.txt) \
      <(grep -vE '^\s*(fps|selection_count)=' automation_out/regress_dump_after.txt) \
      > automation_out/regress_dump.diff; then
    echo "PASS: save_load_dump_diff"
    PASS=$((PASS+1))
  else
    echo "FAIL: save_load_dump_diff (see automation_out/regress_dump.diff)"
    FAIL=$((FAIL+1))
    FAILED_NAMES+=("save_load_dump_diff")
  fi
else
  echo "FAIL: save_load_dump_diff (dump files missing — save_load_dump scenario itself must have failed)"
  FAIL=$((FAIL+1))
  FAILED_NAMES+=("save_load_dump_diff")
fi

echo ""
echo "==================================="
echo "Editor regression suite: $PASS passed, $FAIL failed"
if [[ $FAIL -gt 0 ]]; then
  echo "Failed: ${FAILED_NAMES[*]}"
  exit 1
fi
exit 0
