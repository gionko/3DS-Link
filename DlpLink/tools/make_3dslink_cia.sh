#!/bin/bash
# make_3dslink_cia.sh -- 3DSLink.elf + icon.png + banner -> $OUT/3dslink.cia  (app su SD, Title ID 0004 0000 <UniqueId> 00)
. "$(dirname "$0")/common.sh"
need "$ELF_LINK" "compilare AzaharLink/ConfigApp con devkitARM"; need "$ICON_LINK" "l'icona di ConfigApp (tools/make_icon.py)"
need "$BANNER_DIR/banner.png" "il banner (ConfigApp/banner/make_banner.py)"; need "$BANNER_DIR/jingle.wav" "la musica del banner (ConfigApp/banner/make_banner.py)"
need "$PKG/rsf/3dslink.rsf" "template"
rm -f "$OUT/3dslink.cia" "$OUT/3dslink-cia.smdh" "$OUT/banner.bnr"
# makerom is not reproducible (random ticket id, title key, content id): the child that embeds the PREVIOUS app CIA is stale the moment this one is made.
# It goes away here, so that it has to be built again (make_dlp_child.sh) and a child with another app inside can never be delivered by mistake.
rm -rf "$OUT/3dslink-child.cia" "$OUT/3dslink-child.emu.cia" "$OUT/child_romfs"
# -target t -exefslogo + "Logo: Homebrew" nell'RSF sono OBBLIGATORI (senza logo: ErrDisp "SD removed", README DlpTest 2026-10-14).
# Lo SMDH del CIA e' suo (non quello del .3dsx): la HOME mostra di un pacchetto non ancora aperto la descrizione LUNGA e l'editore, e li' deve esserci il NOME dell'app, non lo slogan.
smdhtool --create "$APP_CIA_NAME" "$APP_CIA_DESC" "$APP_CIA_AUTHOR" "$ICON_LINK" "$OUT/3dslink-cia.smdh" >/dev/null
need "$OUT/3dslink-cia.smdh" "smdhtool non ha prodotto lo SMDH"
# Il banner (la schermata alta della HOME: il piccolo modello 3D con la musica) lo fa bannertool (MIT, di Steveice10, nel fork di Mtgxyz: lo scarica DlpLink/tools/bannertool/build.sh);
# non e' nell'immagine di devkitARM: si compila la prima volta (una ventina di secondi, DlpLink/out/bannertool) e poi resta. L'uscita e' deterministica (il CIA no: makerom non lo e').
if [ ! -x "$BANNERTOOL" ]; then
  OUT="$(dirname "$BANNERTOOL")" bash "$TOOLS/bannertool/build.sh" >/dev/null
fi
need "$BANNERTOOL" "bannertool non e' stato compilato (DlpLink/tools/bannertool/build.sh)"
"$BANNERTOOL" makebanner -i "$BANNER_DIR/banner.png" -a "$BANNER_DIR/jingle.wav" -o "$OUT/banner.bnr" >/dev/null
need "$OUT/banner.bnr" "bannertool non ha prodotto il banner"
[ "$(head -c 4 "$OUT/banner.bnr")" = "CBMD" ] || { echo "il banner non comincia con CBMD" >&2; exit 1; }
# I due avvisi "could not be loaded" di makerom (chiave NCCH, common key) sono ATTESI con -target t.
"$MAKEROM" -f cia -o "$OUT/3dslink.cia" -target t -exefslogo -elf "$ELF_LINK" -rsf "$PKG/rsf/3dslink.rsf" -icon "$OUT/3dslink-cia.smdh" -banner "$OUT/banner.bnr" \
  -ver "$VER16" -DAPP_TITLE="$APP_TITLE" -DAPP_PRODUCT_CODE="$APP_PRODUCT_CODE" -DAPP_UNIQUE_ID="$UNIQUE_ID" \
  -DAPP_SYSTEM_MODE="$APP_SYSTEM_MODE" -DAPP_SYSTEM_MODE_EXT="$APP_SYSTEM_MODE_EXT" 2>&1 | { grep -v "could not be loaded" || true; }
need "$OUT/3dslink.cia" "makerom non ha prodotto il file"
# makerom (v0.19.0 in tools_ctr) PERDE il campo "minor" di -ver (1090 -> 1026): si riscrive la versione esatta in TMD e ticket.
python3 "$TOOLS/version.py" patch "$OUT/3dslink.cia"
python3 "$TOOLS/verify_cia.py" "$OUT/3dslink.cia" --kind app --version "$VER16" --tid "00040000$TID_LO" --elf "$ELF_LINK" --expect-banner --expect-strings "$APP_CIA_NAME" "$APP_CIA_DESC" "$APP_CIA_AUTHOR"
echo "OK: $OUT/3dslink.cia  (versione $VERSION = $VER16, $(stat -c %s "$OUT/3dslink.cia") byte)"
