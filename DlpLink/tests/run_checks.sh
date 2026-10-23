#!/bin/bash
# run_checks.sh -- test del pacchetto (dentro Docker devkitarm, dalla radice del repo):
#   MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/src" -w /src devkitpro/devkitarm bash DlpLink/tests/run_checks.sh
# 1) il verificatore deve dichiarare VALIDO il figlio che e' PARTITO su hardware (fixture, 144 KB)
# 2) deve dichiarare VALIDI i due file appena costruiti (se ci sono in $OUT)
# 3) deve SEGNALARE i guasti: 7 mutazioni su una copia del figlio costruito (selftest_verify.py)
#    e i controlli della catena (check_chain.py) devono dire NO ai guasti che li riguardano (selftest_chain.py)
# 4) ctrtool -y non deve dare FAIL oltre alle firme attese (ticket, TMD, intestazione NCCH; per l'app: ticket, TMD, AccessDescriptor)
# (il figlio di questi controlli, 3dslink-child.cia, e' il RIFERIMENTO: non si consegna, lo compone l'app)
# 6) il compositore contro quel riferimento (dlpc_check.sh)   7) il modello che l'app porta davvero, fatto senza l'app (ship_check.sh)
. "$(dirname "$0")/../tools/common.sh"
FIX="$PKG/tests/fixtures/DlpTestChild_hw_ok.cia"      # = DlpTest/out/DlpTestChild.cia del 2026-10-18 (versione 1027), Title ID 0004000100FD1100
rc=0
echo "== 0. le decisioni dell'installatore (child/source/installer_logic.hpp) contro CIA veri: lettura del TMD, politica, manifesto, testi degli errori"
mkdir -p /tmp/dlplink_child_logic
g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined "$PKG/tests/child_logic_test.cpp" -o /tmp/dlplink_child_logic/child_logic_test
CL_ARGS=()
[ -f "$OUT/3dslink.cia" ] && CL_ARGS+=("$OUT/3dslink.cia" "00040000$TID_LO" "$VER16")
[ -f "$OUT/3dslink-child.cia" ] && CL_ARGS+=("$OUT/3dslink-child.cia" "00040001$TID_LO" "$VER16")
if [ "${#CL_ARGS[@]}" -ge 3 ]; then /tmp/dlplink_child_logic/child_logic_test "${CL_ARGS[@]}" | tail -3 || rc=1; else echo "(no CIA built yet: nothing to read)"; fi
echo "== 1. (the child of the hardware tests is not in this repository)"
if [ -f "$OUT/3dslink-child.cia" ]; then
  echo "== 2. figlio e app costruiti"
  python3 "$TOOLS/verify_cia.py" "$OUT/3dslink-child.cia" --kind child --tid "00040001$TID_LO" --version "$VER16" --romfs-dir "$OUT/child_romfs" --elf "${ELF_CHILD}" | tail -1 || rc=1
  python3 "$TOOLS/verify_cia.py" "$OUT/3dslink.cia" --kind app --tid "00040000$TID_LO" --version "$VER16" --elf "$ELF_LINK" | tail -1 || rc=1
  echo "== 3. self-test negativo"
  python3 "$TOOLS/selftest_verify.py" "$OUT/3dslink-child.cia" "$OUT/child_romfs" || rc=1
  # il test negativo dei controlli della catena (check_chain.py): sa dire NO a un modello diverso, di un'altra versione, a un'immagine che manca, e SI' ai campi che makerom tira a sorte
  python3 "$PKG/tests/selftest_chain.py" "$OUT/child_model.bin" || rc=1
  echo "== 4. ctrtool -y: FAIL ammessi"
  for f in 3dslink.cia 3dslink-child.cia; do
    echo "-- $f"; "$CTRTOOL" -v -y "$OUT/$f" 2>&1 | grep -E "FAIL|ERROR" | sed 's/  */ /g' | cut -c1-90
  done
  # 6) the composer of the child (AzaharLink/ConfigApp/dlpc): from the model and the payload it makes the very file of step 2 (byte for byte, both variants);
  #    the proof is DlpLink/tests/dlpc_test.c (own script: it can also run alone, with -m32 or for the speed table)
  bash "$PKG/tests/dlpc_check.sh" || rc=1
  # 7) the model that SHIPS (the one in the app, made without the app by make_child_model.sh) composes a good child, and is the child without its RomFS whatever app is in it
  #    (DlpLink/tests/ship_check.sh; it also can run alone)
  bash "$PKG/tests/ship_check.sh" || rc=1
fi
exit $rc
