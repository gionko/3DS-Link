#!/bin/bash
# dlpc_check.sh -- the proof of the composer of the Download Play child (AzaharLink/ConfigApp/dlpc/dlpc.c) on the PC. Run_checks.sh calls it (step 6); alone:
#   MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/src" -w /src devkitpro/devkitarm bash DlpLink/tests/dlpc_check.sh
# It needs what build_all.sh leaves in DlpLink/out (3dslink.cia, 3dslink-child.cia, 3dslink-child.emu.cia, child_romfs/): the real child is the reference.
# Environment (all optional):
#   DLPC_ARCH=-m32   a 32-bit build, where size_t is 32 bits as on the console (needs a 32-bit libc: the devkitarm image has none, the azl-hle-build image has)
#   DLPC_MODE=plain  no sanitizers (the speed table, DLPC_BENCH=1, is made this way)
#   DLPC_BENCH=1     only the speed table of the stages
#   DLPC_QUICK=1     fewer places in the mutation and card tests (the default of run_checks.sh is the whole test)
#   DLPC_WORK=DIR    where the composed children and the scratch files go (default /tmp/dlpc_work); the models are made there too
#   DLPC_SRC=FILE    another dlpc.c to build in place of the real one (for a test of the test: a copy of dlpc.c with a deliberate bug must make this script fail)
. "$(dirname "$0")/../tools/common.sh"
ARCH="${DLPC_ARCH:-}"; MODE="${DLPC_MODE:-sanitize}"
W="${DLPC_WORK:-/tmp/dlpc_work}"; B="$W/build"
D="$ROOT/AzaharLink/ConfigApp"; TS="$D/titleshare/common"; SRC="${DLPC_SRC:-$D/dlpc/dlpc.c}"
STAGE="$OUT/child_romfs"
for f in "$OUT/3dslink.cia" "$OUT/3dslink-child.cia" "$OUT/3dslink-child.emu.cia" "$STAGE/AzaharLink.3gx" "$STAGE/manifest.txt"; do
  [ -f "$f" ] || { echo "MANCA: $f (eseguire build_all.sh)"; exit 2; }
done
rm -rf "$B"; mkdir -p "$B" "$W"
if [ "$MODE" = sanitize ]; then CF="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"; SANL="-fsanitize=address,undefined"; else CF="-O2 -g"; SANL=""; fi
export ASAN_OPTIONS=detect_leaks=0
# dlpc.c: the strictest warnings, as errors (it is compiled for the console with -Wall only; the strict ones find what a 32-bit size_t or a sign would break)
STRICT="-std=c99 -pedantic -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Wstrict-prototypes -Werror"
echo "== 6. the composer: build ($([ -n "$ARCH" ] && echo "$ARCH, ")$MODE, $(gcc -dumpmachine))"
gcc $ARCH $STRICT $CF -c "$SRC" -o "$B/dlpc.o" || exit 1
gcc $ARCH -O2 $SANL -c "$TS/sha256.c" -o "$B/sha256.o" || exit 1
gcc $ARCH -O2 $SANL -c "$TS/aes.c" -o "$B/aes.o" || exit 1
gcc $ARCH -O2 $SANL -I "$TS" -c "$TS/cia_front.c" -o "$B/cia_front.o" || exit 1     # (the virtual CIA of Titles Delivery: a source of the app's CIA)
gcc $ARCH -std=gnu11 -Wall -Wextra $CF -I "$D/dlpc" -I "$TS" -c "$PKG/tests/dlpc_test.c" -o "$B/dlpc_test.o" || exit 1
gcc $ARCH -std=gnu11 -w $CF -I "$PKG/host/tests" -I "$PKG/host" -c "$PKG/tests/dlpc_glue_dlph.c" -o "$B/glue_dlph.o" || exit 1
g++ $ARCH -std=c++17 -Wall -Wextra $CF -c "$PKG/tests/dlpc_glue_inst.cpp" -o "$B/glue_inst.o" || exit 1
g++ $ARCH $CF -Wl,--wrap=calloc -Wl,--wrap=fwrite -Wl,--wrap=fread -Wl,--wrap=rename -o "$B/dlpc_test" "$B/dlpc_test.o" "$B/dlpc.o" "$B/sha256.o" "$B/aes.o" "$B/cia_front.o" "$B/glue_dlph.o" "$B/glue_inst.o" || exit 1
# the models: from the finished children, with the same tool the build uses
python3 "$TOOLS/make_model.py" "$OUT/3dslink-child.cia" "$W/child_model.bin" | cut -c1-150 || exit 1
python3 "$TOOLS/make_model.py" "$OUT/3dslink-child.emu.cia" "$W/child_model.emu.bin" --emu | cut -c1-150 || exit 1
python3 "$TOOLS/make_model.py" --check "$W/child_model.bin" | cut -c1-150 || exit 1
chmod +x "$CTRTOOL" 2>/dev/null || true
ARGS=(--work "$W/run" --app "$OUT/3dslink.cia" --plugin "$STAGE/AzaharLink.3gx" --model "$W/child_model.bin" --model-emu "$W/child_model.emu.bin"
      --ref "$OUT/3dslink-child.cia" --ref-emu "$OUT/3dslink-child.emu.cia" --stage "$STAGE"
      --verify "$TOOLS/verify_cia.py" --ctrtool "$CTRTOOL" --tools "$TOOLS" --agree "$PKG/tests/dlpc_model_agree.py"
      --expect "$CHILD_NAME" "$CHILD_DESC" "$CHILD_AUTHOR" --tid "00040000$TID_LO" --version16 "$VER16" --version-text "$VERSION")
[ -f "$ELF_CHILD" ] && ARGS+=(--elf "$ELF_CHILD")
[ "${DLPC_QUICK:-0}" = 1 ] && ARGS+=(--quick)
[ "${DLPC_BENCH:-0}" = 1 ] && ARGS+=(--bench)
rc=0
"$B/dlpc_test" "${ARGS[@]}" || rc=1
if [ "${DLPC_BENCH:-0}" != 1 ]; then
  # the composed files are on disk now: the independent verifier once more, from the shell, the way build_all.sh runs it on the real child
  echo "-- verify_cia.py on the composed files, as make_dlp_child.sh runs it"
  python3 "$TOOLS/verify_cia.py" "$W/run/composed.cia" --kind child --tid "00040001$TID_LO" --version "$VER16" --romfs-dir "$STAGE" --elf "$ELF_CHILD" \
    --expect-strings "$CHILD_NAME" "$CHILD_DESC" "$CHILD_AUTHOR" | tail -1 || rc=1
  python3 "$TOOLS/verify_cia.py" "$W/run/composed.emu.cia" --kind child --emu --tid "00040001$TID_LO" --version "$VER16" --romfs-dir "$STAGE" \
    --expect-strings "$CHILD_NAME" "$CHILD_DESC" "$CHILD_AUTHOR" | tail -1 || rc=1
  cmp "$W/run/composed.cia" "$OUT/3dslink-child.cia" && echo "composed.cia == 3dslink-child.cia (byte for byte)" || rc=1
  cmp "$W/run/composed.emu.cia" "$OUT/3dslink-child.emu.cia" && echo "composed.emu.cia == 3dslink-child.emu.cia (byte for byte)" || rc=1
  # the host library's own test of a whole Download Play (a simulated stock console downloads the child, with losses, silences, re-made rooms): with the composed child
  # (it runs in its own folder: its fuzz files must not go to a slow mount)
  echo "-- DlpLink/host/tests/dlph_test.c with the composed child"
  mkdir -p "$W/dlph" && ( cd "$W/dlph" && gcc $ARCH -Wall -I "$PKG/host/tests" -I "$PKG/host" -o dlph_test "$PKG/host/tests/dlph_test.c" && cp "$W/run/composed.cia" child.cia       && ./dlph_test child.cia > dlph_test.log 2>&1; tail -1 dlph_test.log ) | grep -q "RESULT: ALL OK" && echo "dlph_test: RESULT: ALL OK (the composed child is served to a simulated console)"     || { echo "dlph_test FAILED with the composed child (see $W/dlph/dlph_test.log)"; rc=1; }
  # the file as the console's compiler sees it (only when the devkitARM compiler is here)
  ARMGCC="${DEVKITARM:-/opt/devkitpro/devkitARM}/bin/arm-none-eabi-gcc"
  if [ -x "$ARMGCC" ]; then
    "$ARMGCC" -c -g -Wall -Wextra -Werror -O2 -mword-relocations -ffunction-sections -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -Wno-psabi \
      "$D/dlpc/dlpc.c" -o "$B/dlpc_arm.o" && echo "dlpc.c builds for the console (arm-none-eabi-gcc -O2 -Wall -Wextra -Werror): $("${ARMGCC%gcc}size" "$B/dlpc_arm.o" | tail -1 | awk '{print $1" bytes of code and strings, "$2+$3" of data"}')" || rc=1
  else
    echo "(no arm-none-eabi-gcc: the build for the console is skipped)"
  fi
fi
exit $rc
