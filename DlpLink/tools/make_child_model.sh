#!/bin/bash
# make_child_model.sh -- the MODEL of the Download Play child: what the 3DS Link app carries (AzaharLink/ConfigApp/data/child_model.bin, linked in as child_model_bin)
# and the composer (AzaharLink/ConfigApp/dlpc) builds the child from, on the console.
#   $OUT/child_model.bin       the model of the real child (NCCH sealed with the key of zeros, the Access Descriptor of Nintendo): the resource of the app
#   $OUT/child_model.emu.bin   the same for the emulator variant (NoCrypto): only the tests and Azahar use it
# The model is the child WITHOUT its RomFS (make_model.py), and the RomFS is what carries the app's CIA: so the model cannot depend on the app, and this script does not
# need it. It makes a "seed" child with makerom from the SAME installer (child.elf) and the same plugin as the reference child (make_dlp_child.sh), with a RomFS of three
# small stand-in files, finalizes it as make_dlp_child.sh does, and cuts the RomFS away. That is what breaks the cycle of the build (the app embeds the model, the reference
# child embeds the app): the order is plugin -> installer -> THIS -> app -> 3dslink.cia -> reference child, and check_chain.sh proves afterwards that the model of the
# reference child (made with the real app inside) is the same model outside the few fields that makerom draws at random in every run.
# The seed child is not a deliverable and is not kept as one: it stays in $OUT/model_seed/ to be looked at, and every run makes it again.
. "$(dirname "$0")/common.sh"
need "$ELF_CHILD" "compilare DlpLink/child"; need "$SMDH_CHILD" "SMDH del figlio"; AD_ARG=wild; if [ "${DLP_AD:-wild}" = file ]; then AD_ARG="$PKG/data/dlpchild_ad.bin"; need "$PKG/data/dlpchild_ad.bin" "DLP_AD=file takes the 1024 bytes of an access descriptor of a Download Play child from your own console (the default, DLP_AD=wild, needs no file)"; fi; need "$PLUGIN_3GX" "plugin .3gx"
need "$PKG/rsf/dlpchild.rsf" "template"
# the same pair check as the reference child: the plugin that the child will carry is the one of the app's version
python3 "$TOOLS/check_plugin_pair.py" "$ROOT/AzaharLink/ConfigApp/source/settings.hpp" "$PLUGIN_3GX" "$VERSION"
SEED="$OUT/model_seed"; STAGE="$SEED/romfs"
rm -rf "$SEED"; mkdir -p "$STAGE"
# three stand-in files with the names of the real ones (any content: the model does not keep the RomFS); the app's CIA is a short text, because it does not exist yet
printf 'stand-in: the app that this child installs is not built yet when the model is made (the model does not depend on it)\n' > "$STAGE/3dslink.cia"
cp "$PLUGIN_3GX" "$STAGE/AzaharLink.3gx"
{ echo "version=$VERSION"; echo "version16=$VER16"; echo "tid=00040000$TID_LO"; echo "note=seed of the model, not a package"; } > "$STAGE/manifest.txt"
TID="00040001$TID_LO"
rm -f "$OUT/child_model.bin" "$OUT/child_model.emu.bin"
"$MAKEROM" -f cia -o "$SEED/seed.cia" -target t -exefslogo -desc dlpchild:1 -elf "$ELF_CHILD" -rsf "$PKG/rsf/dlpchild.rsf" \
  -icon "$SMDH_CHILD" -ver "$VER16" -DCHILD_TITLE="$CHILD_TITLE" -DCHILD_PRODUCT_CODE="$CHILD_PRODUCT_CODE" \
  -DCHILD_UNIQUE_ID="$UNIQUE_ID" -DCHILD_ROMFS="$STAGE" 2>&1 | { grep -v "could not be loaded" || true; }
need "$SEED/seed.cia" "makerom non ha prodotto il file"
cp "$SEED/seed.cia" "$SEED/seed.emu.cia"
python3 "$TOOLS/version.py" patch "$SEED/seed.emu.cia"
python3 "$TOOLS/verify_cia.py" "$SEED/seed.emu.cia" --kind child --emu --tid "$TID" --version "$VER16" --romfs-dir "$STAGE" \
  --expect-strings "$CHILD_NAME" "$CHILD_DESC" "$CHILD_AUTHOR" | tail -1
python3 "$TOOLS/finalize_child.py" "$SEED/seed.cia" --ad "$AD_ARG" --version "$VER16"
python3 "$TOOLS/verify_cia.py" "$SEED/seed.cia" --kind child --tid "$TID" --version "$VER16" --romfs-dir "$STAGE" --elf "$ELF_CHILD" \
  --expect-strings "$CHILD_NAME" "$CHILD_DESC" "$CHILD_AUTHOR" | tail -1
python3 "$TOOLS/make_model.py" "$SEED/seed.cia" "$OUT/child_model.bin"
python3 "$TOOLS/make_model.py" "$SEED/seed.emu.cia" "$OUT/child_model.emu.bin" --emu
python3 "$TOOLS/check_chain.py" model "$OUT/child_model.bin" --version16 "$VER16"
python3 "$TOOLS/check_chain.py" model "$OUT/child_model.emu.bin" --version16 "$VER16" --emu
echo "OK: $OUT/child_model.bin ($(stat -c %s "$OUT/child_model.bin") byte, sha256 $(sha256sum "$OUT/child_model.bin" | cut -c1-16)) e child_model.emu.bin, versione $VERSION = $VER16"
