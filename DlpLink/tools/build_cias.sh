#!/bin/bash
# build_cias.sh -- the CIAs and their checks (the last step of build_all.sh, after the model and the app). Runs INSIDE the devkitarm container, from the repo root:
#   GUARD=check|register [NO_APP=1] bash DlpLink/tools/build_cias.sh
# 3DS Link is ONE CIA (3dslink.cia). The child that Download Play consoles download is made by the app on the console; the child of makerom built here is its reference.
# Every step keeps its whole output in DlpLink/out/build_cias.log and shows only the lines that matter. A step that FAILS stops the build (exit 1) and shows
# the end of its own output: the old one-liner piped every step into tail/grep and nothing in it noticed a failing step, so a half-built child could be
# copied to dist/ by a build that said FATTO.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/../out"
mkdir -p "$OUT"
LOG="$OUT/build_cias.log"
: >"$LOG"

step() {   # step <label> <regex of the lines to show> <command...>
    local label="$1" show="$2"
    shift 2
    local tmp rc=0
    tmp="$(mktemp)"
    "$@" >"$tmp" 2>&1 || rc=$?
    { echo "== $label (exit $rc)"; cat "$tmp"; } >>"$LOG"
    if [ "$rc" -ne 0 ]; then
        echo "ERRORE: $label non e' riuscito (exit $rc). Fine del suo output (tutto in DlpLink/out/build_cias.log):"
        tail -25 "$tmp"
        rm -f "$tmp"
        exit 1
    fi
    { grep -E "$show" "$tmp" || true; } | { grep -v 'DigitalSignature\|Signature' || true; }
    rm -f "$tmp"
}

step "3dslink.cia" '^OK|VALIDO' bash "$HERE/make_3dslink_cia.sh"
step "figlio di riferimento (3dslink-child.cia, non si consegna)" 'RIEPILOGO|^OK|VALIDO' bash "$HERE/make_dlp_child.sh"
step "catena (modello, app, guida, ciclo, registro)" '^==|^OK|^FAIL|RIEPILOGO|registrat|versione|differ|\(the app' env GUARD="${GUARD:-check}" NO_APP="${NO_APP:-0}" bash "$HERE/check_chain.sh"
step "controlli" '^==|checks,|RIEPILOGO|^OK |VALIDO|FAIL' bash "$HERE/../tests/run_checks.sh"
echo "(output completo: DlpLink/out/build_cias.log)"
