#!/usr/bin/env bash
# PERF_MASTER_PROMPT.md FAZA 4 item 3 -- thin wrapper so the exact filename
# the master prompt names exists. Actual logic lives in perf_gate.py
# (Python, matching every other tools/qa/*.py script's convention).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec python3 tools/qa/perf_gate.py "$@"
