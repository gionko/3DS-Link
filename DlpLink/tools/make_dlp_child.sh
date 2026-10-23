#!/bin/bash
# make_dlp_child.sh -- the REFERENCE child: $OUT/3dslink.cia + AzaharLink.3gx + installer (child.elf) -> $OUT/3dslink-child.cia
#   $OUT/3dslink-child.cia       the child as makerom + finalize_child.py make it (NCCH sealed with the key of zeros, the Access Descriptor of Nintendo)
#   $OUT/3dslink-child.emu.cia   the same BEFORE finalize (NoCrypto): the only one that Azahar can install (`azahar.exe -i`)
# NOT a deliverable (3DS Link is ONE CIA: the app composes the child on the console, AzaharLink/ConfigApp/dlpc, from its own CIA, the plugin and the model of the child it
# carries; DlpLink/docs/COMPOSITORE.md). It is the ORACLE: the composer's test (DlpLink/tests/dlpc_test.c) proves that the composer makes this very file byte for byte, the
# independent verifier (verify_cia.py) judges it, and the emulator pass installs the .emu.cia. It comes after the app (it carries the app's CIA in its RomFS) and after the
# model (make_child_model.sh, which does not need the app); check_chain.sh then proves that the model of THIS child is the model the app carries.
. "$(dirname "$0")/common.sh"
need "$OUT/3dslink.cia" "eseguire prima make_3dslink_cia.sh"; need "$PLUGIN_3GX" "plugin .3gx"
need "$ELF_CHILD" "compilare DlpLink/child"; need "$SMDH_CHILD" "SMDH del figlio"; need "$PKG/data/dlpchild_ad.bin" "the access descriptor of a Download Play child (1024 bytes): not in this repository, take it from your own console"
# La coppia app + plugin: stessa versione (kVersion = intestazione del .3gx = VERSION). Il plugin e' quello appena costruito (build_all.sh lo copia poi in dist/).
python3 "$TOOLS/check_plugin_pair.py" "$ROOT/AzaharLink/ConfigApp/source/settings.hpp" "$PLUGIN_3GX" "$VERSION"
STAGE="$OUT/child_romfs"; rm -rf "$STAGE"; mkdir -p "$STAGE"
cp "$OUT/3dslink.cia" "$STAGE/3dslink.cia"; cp "$PLUGIN_3GX" "$STAGE/AzaharLink.3gx"
{ echo "version=$VERSION"; echo "version16=$VER16"; echo "tid=00040000$TID_LO"
  for f in 3dslink.cia AzaharLink.3gx; do echo "$f=$(stat -c %s "$STAGE/$f") $(sha256sum "$STAGE/$f" | cut -d' ' -f1)"; done; } > "$STAGE/manifest.txt"
TID="00040001$TID_LO"
# l'app che il figlio installera' e' valida (TMD, NCCH, SVC, icona, ...), con la TID e la versione del pacchetto
python3 "$TOOLS/verify_cia.py" "$STAGE/3dslink.cia" --kind app --tid "00040000$TID_LO" --version "$VER16" --elf "$ELF_LINK" | tail -1
rm -f "$OUT/3dslink-child.cia" "$OUT/3dslink-child.emu.cia"
"$MAKEROM" -f cia -o "$OUT/3dslink-child.cia" -target t -exefslogo -desc dlpchild:1 -elf "$ELF_CHILD" -rsf "$PKG/rsf/dlpchild.rsf" \
  -icon "$SMDH_CHILD" -ver "$VER16" -DCHILD_TITLE="$CHILD_TITLE" -DCHILD_PRODUCT_CODE="$CHILD_PRODUCT_CODE" \
  -DCHILD_UNIQUE_ID="$UNIQUE_ID" -DCHILD_ROMFS="$STAGE" 2>&1 | { grep -v "could not be loaded" || true; }
need "$OUT/3dslink-child.cia" "makerom non ha prodotto il file"
cp "$OUT/3dslink-child.cia" "$OUT/3dslink-child.emu.cia"
python3 "$TOOLS/version.py" patch "$OUT/3dslink-child.emu.cia"      # la copia per Azahar ha la stessa versione
python3 "$TOOLS/verify_cia.py" "$OUT/3dslink-child.emu.cia" --kind child --emu --tid "$TID" --version "$VER16" --romfs-dir "$STAGE" \
  --expect-strings "$CHILD_NAME" "$CHILD_DESC" "$CHILD_AUTHOR" | tail -1
python3 "$TOOLS/finalize_child.py" "$OUT/3dslink-child.cia" --ad "$PKG/data/dlpchild_ad.bin" --version "$VER16"
python3 "$TOOLS/verify_cia.py" "$OUT/3dslink-child.cia" --kind child --tid "$TID" --version "$VER16" --romfs-dir "$STAGE" --elf "$ELF_CHILD" \
  --expect-strings "$CHILD_NAME" "$CHILD_DESC" "$CHILD_AUTHOR"
# (The trap of the Download Play client's cache - same version, other contents: the client starts the OLD copy - is not here any more: what a version stands for is the
# app's CIA, the plugin and the model, and version.py guard looks at those three, in check_chain.sh. This child is not handed out.)
echo "OK: $OUT/3dslink-child.cia ($(stat -c %s "$OUT/3dslink-child.cia") byte, versione $VERSION = $VER16, Title ID $TID): il riferimento dei test, non si consegna"
