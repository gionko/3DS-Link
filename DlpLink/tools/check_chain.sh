#!/bin/bash
# check_chain.sh -- the checks that tie the pieces of the build together (step 3 of build_cias.sh, after the reference child). Runs INSIDE the devkitarm container, from the
# repo root:   GUARD=check|register [NO_APP=1] bash DlpLink/tools/check_chain.sh
# 3DS Link is ONE CIA: the app carries the model of the Download Play child (the child without its RomFS), the plugin (AzaharLink.3gx: the app puts it on the SD card by itself, and
# the child is composed from it) and the guide "How to get boot9.bin"; the installer child carries the guide too; the child of makerom (make_dlp_child.sh) is only the reference of the tests. Each line says what it proves; the tool is check_chain.py.
#   1. the model: valid (the rules of the composer), TMD version = VERSION, the right variant
#   2. the model in the app is the model of THIS build (the file the app was linked with is $OUT/child_model.bin; with NO_APP=1, build_all.sh --no-app, the app was not built:
#      then it has to be the same model outside the fields makerom draws at random, which means the installer did not change)
#   3. the app (ELF, .3dsx and the CIA's code) carries the model and the seven pictures of the guide
#   3b. the app (ELF, .3dsx and the CIA's code) carries exactly the plugin of THIS build (size and SHA-256), the resource data/AzaharLink.3gx is that file, and its header says VERSION
#   4. the installer (ELF, .3dsx, the model and the reference child's code) carries the pictures of the guide
#   5. the cycle: the model of the reference child (made with the real app in its RomFS) is the model that was made without it, outside the random fields
#   6. the guard of what is delivered (the app's CIA + the model + the plugin, under the TMD version): blocks a version that was delivered with other contents
. "$(dirname "$0")/common.sh"
OUT_MODEL="$OUT/child_model.bin"; OUT_MODEL_EMU="$OUT/child_model.emu.bin"
APP_DIR="$ROOT/AzaharLink/ConfigApp"; APP_MODEL="$APP_DIR/data/child_model.bin"; APP_PLUGIN="$APP_DIR/data/AzaharLink.3gx"
CHILD_DIR="$PKG/child"
CHK="python3 $TOOLS/check_chain.py"
for f in "$OUT_MODEL" "$OUT_MODEL_EMU" "$APP_MODEL" "$APP_PLUGIN" "$PLUGIN_3GX" "$OUT/3dslink.cia" "$OUT/3dslink-child.cia" "$ELF_LINK" "$ELF_CHILD"; do
  [ -f "$f" ] || { echo "MANCA: $f (eseguire build_all.sh)"; exit 2; }
done
rc=0
echo "== the model"
$CHK model "$OUT_MODEL" --version16 "$VER16" || rc=1
$CHK model "$OUT_MODEL_EMU" --version16 "$VER16" --emu || rc=1
echo "== the model in the app"
if cmp -s "$OUT_MODEL" "$APP_MODEL"; then
  echo "OK   ConfigApp/data/child_model.bin is DlpLink/out/child_model.bin of this build (sha256 $(sha256sum "$APP_MODEL" | cut -c1-16))"
elif [ "${NO_APP:-0}" = 1 ]; then
  $CHK agree "$OUT_MODEL" "$APP_MODEL" || rc=1
  echo "     (the app was not built: it keeps the model it was linked with, which is the model of this installer)"
else
  echo "FAIL ConfigApp/data/child_model.bin is not the model of this build: the app was linked with another one (make the app again)"; rc=1
fi
echo "== the app carries the model and the guide"
$CHK carries "$APP_MODEL" "$APP_DIR"/build/step[1-7].t3x --in "$ELF_LINK" "$APP_DIR/3DSLink.3dsx" "$OUT/3dslink.cia" || rc=1
echo "== the app carries the plugin of this build"
$CHK plugin "$PLUGIN_3GX" --version "$VERSION" --copy "$APP_PLUGIN" --in "$ELF_LINK" "$APP_DIR/3DSLink.3dsx" "$OUT/3dslink.cia" || rc=1
echo "== the installer carries the guide"
$CHK carries "$CHILD_DIR"/build/step[1-7].t3x --in "$ELF_CHILD" "$CHILD_DIR/child.3dsx" "$OUT_MODEL" "$OUT/3dslink-child.cia" || rc=1
echo "== the cycle: the model does not depend on the app"
python3 "$TOOLS/make_model.py" "$OUT/3dslink-child.cia" "$OUT/model_seed/model_of_reference.bin" | cut -c1-150 || rc=1
$CHK agree "$OUT_MODEL" "$OUT/model_seed/model_of_reference.bin" || rc=1
echo "== what a version stands for (the guard of the registry)"
if [ "${GUARD:-check}" = "register" ]; then
  python3 "$TOOLS/version.py" guard "$OUT/3dslink.cia" "$OUT_MODEL" "$PLUGIN_3GX" "$PKG/releases.tsv" || rc=1
else
  python3 "$TOOLS/version.py" guard "$OUT/3dslink.cia" "$OUT_MODEL" "$PLUGIN_3GX" "$PKG/releases.tsv" --check-only || rc=1
fi
[ "$rc" = 0 ] && echo "RIEPILOGO catena: tutto torna" || echo "RIEPILOGO catena: ALMENO UN CONTROLLO NON TORNA"
exit $rc
