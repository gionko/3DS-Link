#!/bin/bash
# build_all.sh -- tutto il giro, da Git Bash (usa Docker), dalla cartella che si vuole.
#   bash DlpLink/tools/build_all.sh [--bump micro|minor|major] [--register] [--dist] [--no-app] [--no-plugin]
# 3DS Link e' UN CIA solo (3DSLink.cia): il figlio per il Download Play (child.cia) non si consegna, lo fa l'app sulla console (AzaharLink/ConfigApp/dlpc, DlpLink/docs/COMPOSITORE.md)
# dal proprio CIA, dal plugin e dal MODELLO del figlio, che porta dentro come risorse (il plugin e' una risorsa dell'app: la mette lei sulla SD, in /3ds/3DSLink, e il figlio si compone
# da quella). L'ORDINE e' quello che non ha cicli (il figlio di makerom porta l'app e l'app porta il modello e il plugin):
#   0. il plugin (AzaharLink.3gx), che si copia in AzaharLink/ConfigApp/data/AzaharLink.3gx (la risorsa) subito prima dell'app
#   1. l'installatore (DlpLink/child: child.elf, con la guida "Come ottenere boot9.bin" e le sue immagini)
#   2. il MODELLO (make_child_model.sh): il figlio SENZA il suo RomFS, fatto da un figlio "seme" con un RomFS di comodo (non serve l'app: il modello non ne dipende) -> DlpLink/out/child_model.bin
#      (e .emu.bin per i test), che si copia in AzaharLink/ConfigApp/data/child_model.bin
#   3. l'app (ConfigApp: 3DSLink.elf + .smdh + .3dsx), con il modello, il plugin e la guida dentro
#   4. i CIA e i controlli (build_cias.sh): 3dslink.cia -> il figlio di RIFERIMENTO di makerom (3dslink-child.cia, + .emu.cia per Azahar: l'oracolo dei test, NON si consegna) -> la catena
#      (check_chain.sh: il modello, il modello nell'app, l'app e l'installatore portano la guida, l'app porta ESATTAMENTE il plugin di questo giro (dimensione e SHA-256) con la versione
#      di VERSION nell'intestazione, il ciclo: il modello del figlio di riferimento e' quello senza app, il registro) -> run_checks.sh
#   5. con --dist: AzaharLink/dist
#     --bump X     alza la versione di 3DS Link PRIMA di costruire e la scrive in TUTTI i punti (AzaharLink/Tests/set_version.py: VERSION, plugin, app, plgInfo,
#                  documenti). Serve prima di OGNI build che si consegna dopo che una versione e' gia' arrivata a una console: makerom non e' riproducibile
#                  (id casuali), quindi ogni build ha un contenuto diverso e un client che ha gia' quella versione rilancia la copia VECCHIA.
#     --register   registra la versione in DlpLink/releases.tsv, con lo SHA-256 dei tre file che la definiscono (il CIA dell'app, il modello, il plugin: quello che un client puo'
#                  aver tenuto con quel Title ID e quella versione; la trappola scatta se la stessa versione ricompare con un contenuto diverso). Si fa SOLO per la build che si
#                  consegna davvero, alla prima consegna a una console: prima releases.tsv non esiste.
#     --dist       copia in AzaharLink/dist: 3DSLink.3dsx e 3DSLink.cia (nient'altro: PACCHETTO UNICO, il plugin e' DENTRO tutti e due e lo scrive l'app, quindi dist/AzaharLink.3gx non
#                  c'e' piu'; il figlio non e' un file da consegnare, e dist/dlp/child.cia non c'e' piu'), riscrive i md5 e le dimensioni nei documenti (Tests/update_dist_numbers.py)
#                  e lancia Tests/policy_check.sh.
#     --no-app     non ricostruisce l'app (usa il 3DSLink.elf che c'e'): per quando sono cambiati solo gli strumenti. Il modello nuovo NON va nell'app (che resta con quello
#                  con cui e' stata costruita): la catena controlla che sia lo stesso modello fuori dai campi a caso, cioe' che l'installatore non sia cambiato.
#                  Se l'installatore (o la guida) e' cambiato, l'app va ricostruita: il controllo lo dice. Lo stesso per il plugin: l'app resta con quello con cui e' stata costruita, e
#                  se il plugin e' cambiato la catena rifiuta (l'app porta un altro plugin).
#     --no-plugin  non ricostruisce il plugin (usa l'AzaharLink.3gx che c'e'): solo se la versione e il plugin non sono cambiati. Il plugin che c'e' va comunque nell'app.
# 3DS Link e' un'app sola con UNA versione sola (il file AzaharLink/VERSION): il plugin, l'app, il CIA, il modello e i documenti dicono tutti lo stesso numero.
# Dopo l'app e prima dei CIA l'icona deve essere quella giusta (AzaharLink/ConfigApp/tools/make_icon.py, Python di Windows): lo SMDH dell'app e del figlio
# nascono da icon.png, e verify_cia.py rifiuta un SMDH con gli angoli neri.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUMP=""; REGISTER=0; DIST=0; APP=1; PLUGIN=1
while [ $# -gt 0 ]; do
  case "$1" in
    --bump) BUMP="$2"; shift ;;
    --register) REGISTER=1 ;;
    --dist) DIST=1 ;;
    --no-app) APP=0 ;;
    --no-plugin) PLUGIN=0 ;;
    *) echo "uso: build_all.sh [--bump micro|minor|major] [--register] [--dist] [--no-app] [--no-plugin]" >&2; exit 2 ;;
  esac
  shift
done
cd "$ROOT"
docker_run() {   # docker_run <cartella di lavoro dentro /src> <comando...>
  local wd="$1"; shift
  MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/src" -w "/src/$wd" devkitpro/devkitarm "$@"
}

# make_in <cartella> <file da cancellare prima...>: un make che FALLISCE ferma tutto. (Prima l'uscita di make andava in un grep con `|| true` e il test -s
# dopo trovava il VECCHIO ELF: una build fallita veniva impacchettata come se fosse buona.)
make_in() {
  local wd="$1" tmp rc=0
  shift
  rm -f "$@"
  tmp="$(mktemp)"
  docker_run "$wd" make >"$tmp" 2>&1 || rc=$?
  grep -E "warning|error|Error|linking|built" "$tmp" || true
  if [ "$rc" -ne 0 ]; then
    echo "ERRORE: make in $wd e' fallito (exit $rc). Fine del suo output:" >&2
    tail -20 "$tmp" >&2
    rm -f "$tmp"
    exit 1
  fi
  rm -f "$tmp"
}
# stale_deps <cartella build>: un file .d di una build fatta con un'ALTRA cartella montata (/w, /work, ...) nomina file che qui non ci sono e make si ferma con
# "No rule to make target": in quel caso si parte da zero.
stale_deps() {
  grep -hoE '(^| )/[A-Za-z0-9_.-]+/' "$1"/*.d 2>/dev/null | sed 's/^ //' | sort -u | grep -vE '^/(src|opt|usr|lib|tmp)/$' | grep -q .
}

if [ -n "$BUMP" ]; then
  python DlpLink/tools/version.py bump "$BUMP"
fi
export PYTHONIOENCODING=utf-8
python AzaharLink/Tests/set_version.py --check
echo "== versione di 3DS Link per questo giro: $(python AzaharLink/Tests/set_version.py --show)"

if [ "$PLUGIN" = 1 ]; then
  echo "== 0. il plugin (da zero: il Makefile del plugin non sempre ricollega l'ELF)"
  rm -rf AzaharLink/AzaharLink.elf AzaharLink/AzaharLink.3gx AzaharLink/Build
  MSYS_NO_PATHCONV=1 docker run --rm -v "$(cd AzaharLink && pwd -W 2>/dev/null || pwd):/src" -w /src pablomk7/libctrpf:0.8.0 make 2>&1 | grep -E "warning|error|Error|built|3gx" | tail -8 || true
  test -s AzaharLink/AzaharLink.3gx
fi

echo "== 1. l'installatore (DlpLink/child, con la guida)"
if stale_deps DlpLink/child/build; then
  echo "   (dipendenze di una build con un altro percorso: make clean)"
  docker_run DlpLink/child make clean >/dev/null
fi
make_in DlpLink/child DlpLink/child/child.elf DlpLink/child/child.smdh DlpLink/child/child.3dsx
test -s DlpLink/child/child.elf

echo "== 2. il modello del figlio (senza l'app: non ne dipende)"
model_log="$(mktemp)"; model_rc=0
docker_run . env DLP_AD="${DLP_AD:-wild}" bash DlpLink/tools/make_child_model.sh >"$model_log" 2>&1 || model_rc=$?
grep -E "RIEPILOGO|^OK|VALIDO|^FAIL|ERRORE|MANCA" "$model_log" | grep -v 'DigitalSignature\|Signature' || true
if [ "$model_rc" -ne 0 ]; then
  echo "ERRORE: il modello del figlio non e' riuscito (exit $model_rc). Fine del suo output:" >&2
  tail -20 "$model_log" >&2
  rm -f "$model_log"
  exit 1
fi
rm -f "$model_log"
test -s DlpLink/out/child_model.bin && test -s DlpLink/out/child_model.emu.bin
if [ "$APP" = 1 ]; then
  mkdir -p AzaharLink/ConfigApp/data
  cp DlpLink/out/child_model.bin AzaharLink/ConfigApp/data/child_model.bin     # la risorsa che l'app porta (data/ -> child_model_bin)
  echo "   AzaharLink/ConfigApp/data/child_model.bin = DlpLink/out/child_model.bin ($(stat -c %s DlpLink/out/child_model.bin) byte)"
  cp AzaharLink/AzaharLink.3gx AzaharLink/ConfigApp/data/AzaharLink.3gx          # l'altra risorsa: il plugin di questo giro (passo 0), che l'app mette sulla SD (data/ -> AzaharLink_3gx)
  echo "   AzaharLink/ConfigApp/data/AzaharLink.3gx = AzaharLink/AzaharLink.3gx ($(stat -c %s AzaharLink/AzaharLink.3gx) byte, sha256 $(sha256sum AzaharLink/AzaharLink.3gx | cut -c1-16))"
else
  echo "   (--no-app: l'app resta con il modello che ha; DlpLink/out/child_model.bin e' quello di questo installatore)"
fi

if [ "$APP" = 1 ]; then
  echo "== 3. l'app (ConfigApp, con il modello, il plugin e la guida)"
  if stale_deps AzaharLink/ConfigApp/build; then
    echo "   (dipendenze di una build con un altro percorso: make clean)"
    docker_run AzaharLink/ConfigApp make clean >/dev/null
  fi
  make_in AzaharLink/ConfigApp AzaharLink/ConfigApp/3DSLink.elf AzaharLink/ConfigApp/3DSLink.smdh AzaharLink/ConfigApp/3DSLink.3dsx
  test -s AzaharLink/ConfigApp/3DSLink.elf
fi

# l'app e il plugin che si impacchettano portano DAVVERO la versione di questo giro (--no-app / --no-plugin dopo un --bump li lascerebbero vecchi)
python DlpLink/tools/check_version_in_binary.py "$(python AzaharLink/Tests/set_version.py --show | cut -d' ' -f1)" AzaharLink/ConfigApp/3DSLink.elf AzaharLink/AzaharLink.3gx

echo "== 4. i CIA e i controlli"
GUARD=check; [ "$REGISTER" = 1 ] && GUARD=register
docker_run . env GUARD="$GUARD" NO_APP="$((1 - APP))" DLP_AD="${DLP_AD:-wild}" bash DlpLink/tools/build_cias.sh

if [ "$DIST" = 1 ]; then
  echo "== 5. AzaharLink/dist"
  rm -f AzaharLink/dist/dlp/child.cia && rmdir AzaharLink/dist/dlp 2>/dev/null || true     # il figlio non e' piu' un file da consegnare
  # PACCHETTO UNICO: il plugin e' DENTRO l'app e non e' un file di dist/. Un dist/AzaharLink.3gx lasciato da una build vecchia non si cancella (e' il file che una console puo' avere):
  # va in dist/old col nome della sua versione (se ce n'e' gia' uno con quel nome e' lo stesso file, e basta toglierlo; se e' un altro, il suo md5 entra nel nome).
  if [ -f AzaharLink/dist/AzaharLink.3gx ]; then
    read -r old_rev old_minor old_major < <(od -An -tu1 -j9 -N3 AzaharLink/dist/AzaharLink.3gx)
    old_dest="AzaharLink/dist/old/AzaharLink-$old_major.$old_minor.$old_rev.3gx"
    if [ ! -e "$old_dest" ]; then
      mv AzaharLink/dist/AzaharLink.3gx "$old_dest"
    elif cmp -s AzaharLink/dist/AzaharLink.3gx "$old_dest"; then
      rm -f AzaharLink/dist/AzaharLink.3gx
    else
      mv AzaharLink/dist/AzaharLink.3gx "${old_dest%.3gx}-$(md5sum "AzaharLink/dist/AzaharLink.3gx" | cut -c1-8).3gx"
    fi
    echo "   (l'AzaharLink.3gx di una build vecchia e' in dist/old: il plugin ora e' dentro l'app)"
  fi
  cp AzaharLink/ConfigApp/3DSLink.3dsx AzaharLink/dist/3DSLink.3dsx
  cp DlpLink/out/3dslink.cia AzaharLink/dist/3DSLink.cia
  python AzaharLink/Tests/update_dist_numbers.py
  for f in 3DSLink.3dsx 3DSLink.cia; do
    printf '   %-18s %9s byte  md5 %s\n' "$f" "$(stat -c %s "AzaharLink/dist/$f")" "$(md5sum "AzaharLink/dist/$f" | cut -d' ' -f1)"
  done
  printf '   %-18s %9s byte  md5 %s  (DENTRO i due file sopra: la risorsa che l'"'"'app scrive in sdmc:/3ds/3DSLink/)\n' "AzaharLink.3gx" "$(stat -c %s AzaharLink/ConfigApp/data/AzaharLink.3gx)" "$(md5sum AzaharLink/ConfigApp/data/AzaharLink.3gx | cut -d' ' -f1)"
  echo "== 6. Tests/policy_check.sh"
  (cd AzaharLink && bash Tests/policy_check.sh)
fi
echo "FATTO. Si consegna 3DSLink.cia (o 3DSLink.3dsx): il plugin e' dentro e lo scrive l'app in sdmc:/3ds/3DSLink, il figlio Download Play lo fa l'app. DlpLink/out/3dslink-child*.cia sono il riferimento dei test e la prova per Azahar (.emu.cia), non si consegnano."
