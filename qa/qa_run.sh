#!/usr/bin/env bash
# qa_run.sh — monkey_dust QA runner
#
# Повний цикл:
#   1. Build (Release)
#   2. Запуск гри → автоматичний QA capture (через game_capture.py)
#   3. Генерація QA report
#
# Використання:
#   bash tools/qa/qa_run.sh                # повний запуск
#   bash tools/qa/qa_run.sh --no-build     # пропустити збірку
#   bash tools/qa/qa_run.sh --no-capture   # пропустити запуск гри
#   bash tools/qa/qa_run.sh --no-tests     # пропустити unit tests
#   bash tools/qa/qa_run.sh --open         # відкрити HTML після

set -euo pipefail
cd "$(dirname "$0")/../.."   # repo root

# ── Аргументи ─────────────────────────────────────────────────────────────────
NO_BUILD=0; NO_CAPTURE=0; NO_TESTS=0; OPEN_REPORT=0
for arg in "$@"; do
  case $arg in
    --no-build)   NO_BUILD=1   ;;
    --no-capture) NO_CAPTURE=1 ;;
    --no-tests)   NO_TESTS=1   ;;
    --open)       OPEN_REPORT=1 ;;
  esac
done

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; NC='\033[0m'; BOLD='\033[1m'

log() { echo -e "${CYAN}[qa]${NC} $*"; }
ok()  { echo -e "${GREEN}[ok]${NC} $*"; }
err() { echo -e "${RED}[err]${NC} $*" >&2; }

echo ""
echo -e "${BOLD}╔══════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║       monkey_dust QA Runner              ║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════════╝${NC}"
echo ""

FAIL=0

# ── 1. Build ──────────────────────────────────────────────────────────────────
if [[ $NO_BUILD -eq 0 ]]; then
  log "Build (Release)..."
  if cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DUSE_SDL3=ON \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      --log-level=ERROR \
      2>&1 | grep -E "error:|Error" | head -5; then
    :
  fi
  BUILD_OUT=$(ninja -C build monkey_dust md_tests md_behavior_tests 2>&1) || {
    BUILD_OUT_ERR=$(echo "$BUILD_OUT" | grep "error:" | head -10)
    err "Build failed:\n$BUILD_OUT_ERR"
    FAIL=1
  }
  NINJA_WARN=$(echo "$BUILD_OUT" | grep -c "warning:" || true)
  if [[ $FAIL -eq 0 ]]; then
    ok "Build OK (warnings: $NINJA_WARN)"
  fi
else
  log "Build: пропускаємо (--no-build)"
fi

# ── 2. SPIR-V shaders ─────────────────────────────────────────────────────────
if [[ $NO_BUILD -eq 0 ]] && [[ $FAIL -eq 0 ]]; then
  log "Компіляція шейдерів..."
  if bash scripts/compile_shaders.sh 2>&1 | tail -3 | grep -q "FAIL"; then
    err "Shader compilation failed"
    FAIL=1
  else
    ok "Shaders OK"
  fi
fi

# ── 3. Game capture ───────────────────────────────────────────────────────────
CAPTURE_ID=""
if [[ $NO_CAPTURE -eq 0 ]] && [[ $FAIL -eq 0 ]]; then
  CAPTURE_ID=$(date +%Y%m%d_%H%M%S)
  CAPTURE_DIR="tools/qa/captures/${CAPTURE_ID}"
  mkdir -p "${CAPTURE_DIR}/frames"
  QA_JSONL="${CAPTURE_DIR}/qa_state.jsonl"
  log "QA capture → ${CAPTURE_DIR}"

  if command -v wmctrl &>/dev/null && command -v ffmpeg &>/dev/null; then
    DISPLAY="${DISPLAY:-:0}" MD_QA_STATE="${QA_JSONL}" \
        ./build/game/monkey_dust &
    GAME_PID=$!
    sleep 1

    CAPTURE_OUT=$(python3 scripts/game_capture.py \
        --no-launch --record-fps 10 --frame-fps 1 \
        --output-dir "${CAPTURE_DIR}/frames" 2>&1) || true

    wait $GAME_PID 2>/dev/null || true
    ok "Capture завершено"
  else
    log "Headless режим (без wmctrl/ffmpeg)..."
    DISPLAY="${DISPLAY:-:0}" MD_QA_STATE="${QA_JSONL}" \
        ./build/game/monkey_dust 2>&1 | tail -5 || true
    ok "Game run завершено"
  fi

  [[ -f "${QA_JSONL}" ]] && ok "QA state log: $(wc -l < "${QA_JSONL}") ticks" \
                          || log "QA state log: не згенеровано (headless?)"
else
  [[ $NO_CAPTURE -eq 1 ]] && log "Capture: пропускаємо (--no-capture)"
fi

# ── 4. QA Report ──────────────────────────────────────────────────────────────
log "Генерую QA report..."
REPORT_ARGS=""
[[ $NO_TESTS -eq 1 ]]   && REPORT_ARGS="--no-tests"
[[ $OPEN_REPORT -eq 1 ]] && REPORT_ARGS="$REPORT_ARGS --open"
[[ -n "${CAPTURE_ID}" ]] && REPORT_ARGS="$REPORT_ARGS --capture ${CAPTURE_ID}"

REPORT_OUT=$(python3 tools/qa/qa_report.py $REPORT_ARGS 2>&1) || {
  err "qa_report.py failed:\n$REPORT_OUT"
  FAIL=1
}
echo "$REPORT_OUT"

# ── 4b. BDD ───────────────────────────────────────────────────────────────────
if [[ -n "${CAPTURE_ID}" ]] && [[ -f "tools/qa/qa_bdd.py" ]]; then
  log "BDD перевірки..."
  BDD_ARGS="--capture ${CAPTURE_ID}"
  python3 tools/qa/qa_bdd.py tools/qa/features/ $BDD_ARGS 2>&1 | tail -20 || true
fi

# ── Підсумок ──────────────────────────────────────────────────────────────────
echo ""
if [[ $FAIL -eq 0 ]]; then
  STATUS=$(echo "$REPORT_OUT" | grep "STATUS:" | awk '{print $2}')
  case "$STATUS" in
    PASS) ok "QA STATUS: ${GREEN}PASS${NC}" ;;
    WARN) echo -e "${YELLOW}[qa]${NC} QA STATUS: ${YELLOW}WARN${NC}" ;;
    FAIL) err "QA STATUS: FAIL"; FAIL=1 ;;
    *)    ok "QA завершено" ;;
  esac
else
  err "QA завершено з помилками"
fi

echo ""
exit $FAIL
