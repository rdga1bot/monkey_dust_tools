#!/usr/bin/env bash
# char_preview_qa.sh — QA для char preview: зберігає baseline або порівнює з ним.
#
# Використання:
#   bash tools/qa/char_preview_qa.sh --save   NAME    # зберегти поточний baseline
#   bash tools/qa/char_preview_qa.sh --compare NAME   # порівняти з baseline
#   bash tools/qa/char_preview_qa.sh --list           # список наявних baselines
#
# Workflow:
#   1. Відкрийте редактор → Characters tab → знайдіть потрібний кут камери
#   2. Натисніть PrintScreen або зробіть скріншот у ~/Screenshot_*.png
#   3. bash tools/qa/char_preview_qa.sh --save hair_front
#   4. Після змін: bash tools/qa/char_preview_qa.sh --compare hair_front
#
# Автоматичне capture через editor (якщо реалізовано Ctrl+Shift+S):
#   Baseline зберігається у tools/qa/baselines/char_preview_YYYYMMDD.png
#
# Залежності: imagemagick (compare, convert)
#   Arch: sudo pacman -S imagemagick

set -euo pipefail
cd "$(dirname "$0")/../.."   # repo root

BASEDIR="tools/qa/baselines/char_preview"
mkdir -p "$BASEDIR"

CYAN='\033[0;36m'; GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()  { echo -e "${CYAN}[char_qa]${NC} $*"; }
ok()   { echo -e "${GREEN}[ok]${NC} $*"; }
err()  { echo -e "${RED}[err]${NC} $*"; }
warn() { echo -e "${YELLOW}[warn]${NC} $*"; }

case "${1:-}" in
--list)
    log "Baselines in $BASEDIR/:"
    ls -1 "$BASEDIR"/*.png 2>/dev/null | xargs -I{} basename {} .png || echo "  (none)"
    ;;

--save)
    NAME="${2:?Usage: --save NAME}"
    # Find latest screenshot
    LATEST=$(ls -t ~/Screenshot_*.png 2>/dev/null | head -1 || true)
    if [[ -z "$LATEST" ]]; then
        err "No ~/Screenshot_*.png found. Take a screenshot first."
        exit 1
    fi
    cp "$LATEST" "$BASEDIR/${NAME}.png"
    ok "Saved baseline: $BASEDIR/${NAME}.png (from $LATEST)"
    ;;

--compare)
    NAME="${2:?Usage: --compare NAME}"
    BASELINE="$BASEDIR/${NAME}.png"
    if [[ ! -f "$BASELINE" ]]; then
        err "Baseline not found: $BASELINE"
        err "Run: bash tools/qa/char_preview_qa.sh --save $NAME first"
        exit 1
    fi
    LATEST=$(ls -t ~/Screenshot_*.png 2>/dev/null | head -1 || true)
    if [[ -z "$LATEST" ]]; then
        err "No ~/Screenshot_*.png found. Take a screenshot first."
        exit 1
    fi
    if ! command -v compare &>/dev/null; then
        err "ImageMagick not found: sudo pacman -S imagemagick"
        exit 1
    fi
    DIFF_OUT="/tmp/char_preview_diff.png"
    METRIC=$(compare -metric RMSE "$BASELINE" "$LATEST" "$DIFF_OUT" 2>&1 | head -1 || true)
    log "Baseline: $BASELINE"
    log "Current:  $LATEST"
    log "RMSE: $METRIC"
    RMSE_VAL=$(echo "$METRIC" | grep -oP '[\d.]+' | head -1)
    if (( $(echo "$RMSE_VAL < 0.02" | bc -l 2>/dev/null || echo 0) )); then
        ok "PASS — diff $RMSE_VAL < 0.02 threshold"
    else
        warn "DIFF — $RMSE_VAL exceeds threshold. Diff saved: $DIFF_OUT"
        warn "Open diff: xdg-open $DIFF_OUT"
    fi
    ;;

*)
    echo "Usage:"
    echo "  bash tools/qa/char_preview_qa.sh --save    NAME   # save baseline from latest screenshot"
    echo "  bash tools/qa/char_preview_qa.sh --compare NAME   # compare current screenshot vs baseline"
    echo "  bash tools/qa/char_preview_qa.sh --list          # list saved baselines"
    ;;
esac
