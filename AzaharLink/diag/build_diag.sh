#!/bin/bash
# build_diag.sh -- the DIAGNOSTIC plugin (AzaharLink-diag.3gx), from the repository root, with Docker:
#   bash AzaharLink/diag/build_diag.sh
# It is the delivered plugin plus -DAZL_BOOT_DIAG (Includes/ctr/boot_diag.hpp says what that adds), built in a COPY of the plugin's folders under
# AzaharLink/diag/work, so that the delivered plugin's Build/, AzaharLink.elf and AzaharLink.3gx are never touched and the product Makefile has no
# diagnostic switch in it. It is NOT part of build_all.sh and NOT a deliverable of dist/: it goes to one console, by hand (README.md here).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN="$(cd "$HERE/.." && pwd)"
WORK="$HERE/work"
rm -rf "$WORK"
mkdir -p "$WORK"
cp -r "$PLUGIN/Sources" "$PLUGIN/Includes" "$WORK/"
cp "$PLUGIN/3gx.ld" "$PLUGIN/CTRPluginFramework.plgInfo" "$PLUGIN/Makefile" "$PLUGIN/VERSION" "$WORK/"
# the one change: the flag (the product Makefile keeps its own line)
sed -i 's/-DAZL_USE_CTRPF/-DAZL_USE_CTRPF -DAZL_BOOT_DIAG/' "$WORK/Makefile"
grep -q -- '-DAZL_BOOT_DIAG' "$WORK/Makefile" || { echo "the flag did not go into the copy of the Makefile" >&2; exit 1; }
MSYS_NO_PATHCONV=1 docker run --rm -v "$(cd "$WORK" && pwd -W 2>/dev/null || pwd):/src" -w /src pablomk7/libctrpf:0.8.0 make 2>&1 | grep -E "warning|error|Error|3gx" | tail -12 || true
test -s "$WORK/AzaharLink.3gx" || { echo "no AzaharLink.3gx: the build failed" >&2; exit 1; }
cp "$WORK/AzaharLink.3gx" "$HERE/AzaharLink-diag.3gx"
grep -q "DIAGNOSTIC build" "$HERE/AzaharLink-diag.3gx" || { echo "the diagnostic text is not in the plugin: the flag did not reach the sources" >&2; exit 1; }
echo "OK: $HERE/AzaharLink-diag.3gx ($(stat -c %s "$HERE/AzaharLink-diag.3gx") bytes, sha256 $(sha256sum "$HERE/AzaharLink-diag.3gx" | cut -c1-16))"
