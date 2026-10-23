# common.sh -- variabili e controlli comuni. Si "sourcea" dagli script; si esegue DENTRO Docker, dalla radice del repo:
#   MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/src" -w /src devkitpro/devkitarm bash DlpLink/tools/<script>.sh
# (Windows/Git Bash: fuori da Docker usare `python`, non `python3`; qui dentro python3 e openssl ci sono.)
set -euo pipefail
PKG="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"          # .../DlpLink
ROOT="${ROOT:-$(cd "$PKG/.." && pwd)}"                          # radice del repo
OUT="${OUT:-$PKG/out}"                                          # uscita (cartella ignorata da git)
MAKEROM="$ROOT/tools_ctr/makerom"; CTRTOOL="$ROOT/tools_ctr/ctrtool"
TOOLS="$PKG/tools"
ELF_LINK="${ELF_LINK:-$ROOT/AzaharLink/ConfigApp/3DSLink.elf}"
SMDH_LINK="${SMDH_LINK:-$ROOT/AzaharLink/ConfigApp/3DSLink.smdh}"                # (quello del .3dsx: il CIA ha un SMDH suo, vedi make_3dslink_cia.sh)
ICON_LINK="${ICON_LINK:-$ROOT/AzaharLink/ConfigApp/icon.png}"                    # l'icona 48x48 (ConfigApp/tools/make_icon.py): il CIA ne fa il suo SMDH
BANNER_DIR="${BANNER_DIR:-$ROOT/AzaharLink/ConfigApp/banner}"                    # banner.png (256x128) e jingle.wav: la faccia del CIA sulla schermata alta della HOME (make_banner.py)
BANNERTOOL="${BANNERTOOL:-$PKG/out/bannertool}"                                  # il programma che li fa .bnr: DlpLink/tools/bannertool (sorgente MIT scaricato), lo compila make_3dslink_cia.sh se manca
PLUGIN_3GX="${PLUGIN_3GX:-$ROOT/AzaharLink/AzaharLink.3gx}"
ELF_CHILD="${ELF_CHILD:-$PKG/child/child.elf}"                  # l'INSTALLER (figlio): lo costruisce DlpLink/child/Makefile
SMDH_CHILD="${SMDH_CHILD:-$PKG/child/child.smdh}"
# Identita' (Title ID = 0004 000x <UniqueId 24 bit> 00). L'unico posto dove si cambiano.
UNIQUE_ID="${UNIQUE_ID:-0x0F83D1}"                               # PIANO.md: app 000400000F83D100, figlio 000400010F83D100 (0xFD3D era un residuo delle prove)
APP_PRODUCT_CODE="${APP_PRODUCT_CODE:-CTR-P-3DLK}"; CHILD_PRODUCT_CODE="${CHILD_PRODUCT_CODE:-CTR-P-3DLC}"
APP_TITLE="${APP_TITLE:-3DS Link}"; CHILD_TITLE="${CHILD_TITLE:-3DS Link}"   # (il nome nell'exheader e' di 8 byte: makerom taglia; non e' quello che il Download Play mostra)
# Quello che il Download Play mostra: nome = descrizione CORTA dello SMDH del figlio, sotto = descrizione LUNGA (63 e 127 caratteri al massimo).
# Si impostano nel Makefile di DlpLink/child (APP_TITLE, APP_DESCRIPTION, APP_AUTHOR) e make_dlp_child.sh li RICONTROLLA nello SMDH costruito.
CHILD_NAME="${CHILD_NAME:-3DS Link Installer}"; CHILD_AUTHOR="${CHILD_AUTHOR:-Gionko}"   # (CHILD_DESC, "Powered by Gionko · vX.Y.Z", si completa sotto con la versione)
APP_SYSTEM_MODE="${APP_SYSTEM_MODE:-64MB}"; APP_SYSTEM_MODE_EXT="${APP_SYSTEM_MODE_EXT:-124MB}"   # Legacy = Luma limita l'app a 64 MiB anche su New 3DS (heap ~29 MiB invece di ~89); mai 178MB (heap > 96 MiB: crash)
need() { [ -e "$1" ] || { echo "MANCA: $1 ($2)" >&2; exit 2; }; }
[ -x "$MAKEROM" ] || chmod +x "$MAKEROM" 2>/dev/null || true
command -v openssl >/dev/null || { echo "serve openssl (c'e' nell'immagine devkitpro/devkitarm)" >&2; exit 2; }
VERSION="$(python3 "$TOOLS/version.py" show)"                   # "1.0.0": l'UNICO numero di 3DS Link, il file AzaharLink/VERSION
VER16="$(python3 "$TOOLS/version.py" u16)"                      # (M<<10)|(m<<4)|u  -> 1024
CHILD_DESC="${CHILD_DESC:-Powered by Gionko · v$VERSION}"        # la descrizione LUNGA dello SMDH del figlio: la riga sotto il nome nel Download Play (127 caratteri al massimo)
# Lo SMDH del CIA (quello che la HOME mostra di un pacchetto non ancora aperto: il nome e l'editore, NON lo slogan): nome, descrizione LUNGA e editore. Lo slogan («Connect, share, play together») sta
# nello SMDH del .3dsx (ConfigApp/Makefile, APP_DESCRIPTION) e nel banner. Il nome dell'app e' «3DS Link» e basta (l'utente, 2026-10-22: «dovrebbe apparire il nome del programma e non il claim»).
APP_CIA_NAME="${APP_CIA_NAME:-3DS Link}"; APP_CIA_DESC="${APP_CIA_DESC:-3DS Link}"; APP_CIA_AUTHOR="${APP_CIA_AUTHOR:-Powered by Gionko}"
TID_LO="$(printf '%06X' $((UNIQUE_ID)))00"                      # 24 bit di unique id + variation 00 (8 cifre hex)
mkdir -p "$OUT"
