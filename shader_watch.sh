#!/usr/bin/env bash
# shader_watch.sh — автоматична перекомпіляція SPIR-V при зміні .vert/.frag/.comp
#
# Використання:
#   bash tools/shader_watch.sh          # запустити в окремому терміналі поряд з грою/редактором
#   bash tools/shader_watch.sh --once   # скомпілювати один раз і вийти
#
# Після компіляції: введіть /reload-shaders у editor console для hot-reload без рестарту.
#
# Залежності: inotifywait (пакет inotify-tools)
#   Arch: sudo pacman -S inotify-tools

set -euo pipefail
cd "$(dirname "$0")/.."   # repo root

CYAN='\033[0;36m'; GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'
log()  { echo -e "${CYAN}[watch]${NC} $*"; }
ok()   { echo -e "${GREEN}[ok]${NC} $*"; }
err()  { echo -e "${RED}[err]${NC} $*"; }

# ── One-shot compile ──────────────────────────────────────────────────────────
compile() {
    local changed="${1:-all}"
    log "Changed: $changed — recompiling SPIR-V..."
    if bash scripts/compile_shaders.sh 2>&1 | tail -3; then
        ok "SPIR-V ready. Type /reload-shaders in editor console to hot-reload."
        touch shaders/spirv/.reload_needed
    else
        err "Compilation FAILED — fix errors above"
    fi
}

if [[ "${1:-}" == "--once" ]]; then
    compile "all"; exit 0
fi

# ── Watch loop ────────────────────────────────────────────────────────────────
if ! command -v inotifywait &>/dev/null; then
    err "inotifywait not found. Install: sudo pacman -S inotify-tools"
    err "Falling back to polling mode (5s interval)..."
    while true; do
        sleep 5
        # Check if any .frag/.vert/.comp is newer than the newest .spv
        newest_src=$(find shaders/ -name '*.vert' -o -name '*.frag' -o -name '*.comp' \
                     2>/dev/null | xargs ls -t 2>/dev/null | head -1)
        newest_spv=$(find shaders/spirv/ -name '*.spv' 2>/dev/null | xargs ls -t 2>/dev/null | head -1)
        if [[ -n "$newest_src" && ( -z "$newest_spv" || "$newest_src" -nt "$newest_spv" ) ]]; then
            compile "$newest_src"
        fi
    done
fi

log "Watching shaders/ for changes... (Ctrl+C to stop)"
inotifywait -m -r shaders/ -e close_write --include '.*\.(vert|frag|comp)$' \
    --format '%w%f' 2>/dev/null |
while read -r changed_file; do
    compile "$changed_file"
done
