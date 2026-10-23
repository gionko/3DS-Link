#!/bin/bash
# ship_check.sh -- the proof that the model that SHIPS works. Run_checks.sh calls it (step 7); alone, after build_all.sh:
#   MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/src" -w /src devkitpro/devkitarm bash DlpLink/tests/ship_check.sh
# dlpc_check.sh (step 6) proves the composer with the models that make_model.py cuts out of the REFERENCE children (made with the real app inside), so that the composed child is that
# reference byte for byte. The model that the app carries is another one: make_child_model.sh makes it without the app (a seed child with stand-in files), so that the build has no cycle.
# This is the proof that it does what it must, with the composer of the app (the same dlpc.c) run on the PC by dlpc_compose.c:
#   1. the shipped model + the app's CIA + the plugin compose a child (both variants) that verify_cia.py calls VALIDO, with the three files of the RomFS equal to the originals
#   2. the child is the model and nothing but the RomFS: make_model.py on the composed child gives back the shipped model BYTE FOR BYTE (the fields that makerom draws at random are
#      the model's own, carried through)
#   3. with ANOTHER app's CIA (a second makerom run of the same ELF: other bytes, same title and version) the child is another file (its RomFS differs) and its model is STILL the
#      same bytes: the model does not depend on the app, which is what the cycle of the build needs
# Environment (all optional): SHIP_WORK (scratch folder, default /tmp/ship_work), SHIP_APP, SHIP_PLUGIN, SHIP_MODEL, SHIP_MODEL_EMU (other files than the ones of DlpLink/out), and
# SHIP_KEEP=DIR: copy the two composed children there (child.cia, child.emu.cia) and their SHA-256, for the emulator pass.
. "$(dirname "$0")/../tools/common.sh"
W="${SHIP_WORK:-/tmp/ship_work}"
APP="${SHIP_APP:-$OUT/3dslink.cia}"; PLUGIN="${SHIP_PLUGIN:-$PLUGIN_3GX}"
MODEL="${SHIP_MODEL:-$OUT/child_model.bin}"; MODEL_EMU="${SHIP_MODEL_EMU:-$OUT/child_model.emu.bin}"
D="$ROOT/AzaharLink/ConfigApp"; TS="$D/titleshare/common"
for f in "$APP" "$PLUGIN" "$MODEL" "$MODEL_EMU" "$ELF_LINK" "$ELF_CHILD"; do
  [ -f "$f" ] || { echo "MANCA: $f (eseguire build_all.sh)"; exit 2; }
done
rm -rf "$W"; mkdir -p "$W"
rc=0
EXPECT=("$CHILD_NAME" "$CHILD_DESC" "$CHILD_AUTHOR")
echo "== 7. the model that ships (made without the app) composes the child"
gcc -O2 -std=gnu11 -Wall -Wextra -I "$D/dlpc" -I "$TS" "$PKG/tests/dlpc_compose.c" "$D/dlpc/dlpc.c" "$TS/sha256.c" "$TS/aes.c" -o "$W/dlpc_compose" || exit 1

stage_for() {   # stage_for <dir> <app.cia>: the three files the composer must have put in the RomFS (the same recipe as make_dlp_child.sh)
  mkdir -p "$1"; cp "$2" "$1/3dslink.cia"; cp "$PLUGIN" "$1/AzaharLink.3gx"
  { echo "version=$VERSION"; echo "version16=$VER16"; echo "tid=00040000$TID_LO"
    for f in 3dslink.cia AzaharLink.3gx; do echo "$f=$(stat -c %s "$1/$f") $(sha256sum "$1/$f" | cut -d' ' -f1)"; done; } > "$1/manifest.txt"
}
same_bytes() {   # same_bytes <a> <b> <what>
  if cmp -s "$1" "$2"; then echo "OK   $3 ($(stat -c %s "$1") bytes, sha256 $(sha256sum "$1" | cut -c1-16))"; else echo "FAIL $3: the files differ"; rc=1; fi
}

stage_for "$W/stage" "$APP"
"$W/dlpc_compose" "$MODEL" "$APP" "$PLUGIN" "$VERSION" "$W/child.cia" || rc=1
"$W/dlpc_compose" "$MODEL_EMU" "$APP" "$PLUGIN" "$VERSION" "$W/child.emu.cia" --emu || rc=1
echo "-- 1. verify_cia.py on the children the shipped models compose"
python3 "$TOOLS/verify_cia.py" "$W/child.cia" --kind child --tid "00040001$TID_LO" --version "$VER16" --romfs-dir "$W/stage" --elf "$ELF_CHILD" \
  --expect-strings "${EXPECT[@]}" | tail -1 || rc=1
python3 "$TOOLS/verify_cia.py" "$W/child.emu.cia" --kind child --emu --tid "00040001$TID_LO" --version "$VER16" --romfs-dir "$W/stage" \
  --expect-strings "${EXPECT[@]}" | tail -1 || rc=1
echo "-- 2. the model is the child without its RomFS: make_model.py on the composed child gives the shipped model back"
python3 "$TOOLS/make_model.py" "$W/child.cia" "$W/m1.bin" | cut -c1-150 || rc=1
python3 "$TOOLS/make_model.py" "$W/child.emu.cia" "$W/m1.emu.bin" --emu | cut -c1-150 || rc=1
same_bytes "$W/m1.bin" "$MODEL" "the model of the composed child is the shipped model (real variant)"
same_bytes "$W/m1.emu.bin" "$MODEL_EMU" "the model of the composed child is the shipped model (emulator variant)"
echo "-- 3. another app, the same model: a second makerom run of the same ELF is another CIA of the same title and version"
mkdir -p "$W/app2"
OUT="$W/app2" bash "$TOOLS/make_3dslink_cia.sh" > "$W/app2/make.log" 2>&1 || { echo "FAIL the second app CIA could not be made (see $W/app2/make.log)"; tail -5 "$W/app2/make.log"; rc=1; }
if [ -f "$W/app2/3dslink.cia" ]; then
  if cmp -s "$APP" "$W/app2/3dslink.cia"; then echo "FAIL the second app CIA is the same file as the first one: nothing was proven"; rc=1
  else echo "OK   the second app CIA is another file ($(stat -c %s "$W/app2/3dslink.cia") bytes against $(stat -c %s "$APP"), sha256 $(sha256sum "$W/app2/3dslink.cia" | cut -c1-16) against $(sha256sum "$APP" | cut -c1-16))"; fi
  stage_for "$W/stage2" "$W/app2/3dslink.cia"
  "$W/dlpc_compose" "$MODEL" "$W/app2/3dslink.cia" "$PLUGIN" "$VERSION" "$W/child2.cia" || rc=1
  python3 "$TOOLS/verify_cia.py" "$W/child2.cia" --kind child --tid "00040001$TID_LO" --version "$VER16" --romfs-dir "$W/stage2" --elf "$ELF_CHILD" \
    --expect-strings "${EXPECT[@]}" | tail -1 || rc=1
  if cmp -s "$W/child.cia" "$W/child2.cia"; then echo "FAIL the two children are the same file"; rc=1; else echo "OK   the two children are two files (their RomFS carries two apps)"; fi
  python3 "$TOOLS/make_model.py" "$W/child2.cia" "$W/m2.bin" | cut -c1-150 || rc=1
  same_bytes "$W/m2.bin" "$MODEL" "the model of the child of the OTHER app is the shipped model too: it does not depend on the app"
fi
if [ -n "${SHIP_KEEP:-}" ]; then
  mkdir -p "$SHIP_KEEP"
  cp "$W/child.cia" "$W/child.emu.cia" "$SHIP_KEEP/"
  ( cd "$SHIP_KEEP" && sha256sum child.cia child.emu.cia > SHA256.txt && cat SHA256.txt )
fi
[ "$rc" = 0 ] && echo "RIEPILOGO modello consegnato: tutto torna" || echo "RIEPILOGO modello consegnato: ALMENO UN CONTROLLO NON TORNA"
exit $rc
