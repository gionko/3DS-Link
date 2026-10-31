#!/bin/bash
# The build stamp that the log header prints must say when the CODE last changed, not when plugin.cpp was last compiled (P10-0001).
# What happened (2026-10-25): the first C-stick build had a new Sources/main.cpp over a plugin.cpp object of 10-23, so its logs said
# "AzaharLink 1.0.3 (built Oct 23 2026 18:37:31)" exactly like the build without the fix, and nobody could tell which one a log came from.
#
# The test builds a COPY of the tree twice with the shipped Makefile (Docker, pablomk7/libctrpf:0.8.0), the second time after a change to
# Sources/main.cpp alone, and requires that
#   1. plugin.o was NOT compiled again (the case of 10-25: the object that held the date is stale), and
#   2. the date/time strings that the ELF carries are different (a new stamp appeared).
# On the tree before P10-0001 the second requirement fails (plugin.cpp's __DATE__ / __TIME__ stay what they were); with it, build_id.cpp
# is recompiled after every other object and the stamp moves.
#   bash Tests/build_id_test.sh [tree]      (Git Bash; Docker running; the tree defaults to the one this script is in)
#   BUILD_ID_TEST=skip leaves it out
set -e
[ "$BUILD_ID_TEST" = skip ] && { echo "build id test: skipped"; exit 0; }
TREE=${1:-$(cd "$(dirname "$0")/.." && pwd)}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
tar -C "$TREE" --exclude=Build --exclude=analysis --exclude='*.elf' --exclude='*.3gx' --exclude=dist --exclude=Tests \
    --exclude=ConfigApp --exclude=diag -cf - . | tar -xf - -C "$WORK"
build() {
  (cd "$WORK" && MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/src" -w /src \
      pablomk7/libctrpf:0.8.0 make >"$WORK/build.log" 2>&1) || { tail -20 "$WORK/build.log"; echo "build id test FAILED: the build failed"; exit 1; }
}
# every date / time string of the program (plugin.cpp's __DATE__ and __TIME__, or build_id.cpp's one string, and whatever else carries one)
stamps() {
  grep -a -o -E '[A-Z][a-z]{2} [ 0-9][0-9] 20[0-9]{2}( [0-9]{2}:[0-9]{2}:[0-9]{2})?|[0-9]{2}:[0-9]{2}:[0-9]{2}' "$WORK/AzaharLink.elf" | sort -u
}
FAILED=0
build
stamps > "$WORK/stamps1.txt"
M1=$(stat -c %Y "$WORK/Build/plugin.o")
sleep 2
echo "// build_id_test: a change to main.cpp alone" >> "$WORK/Sources/main.cpp"
build
stamps > "$WORK/stamps2.txt"
M2=$(stat -c %Y "$WORK/Build/plugin.o")
if [ "$M1" != "$M2" ]; then
  echo "build id test: plugin.o was compiled again after a change to main.cpp alone (the test would not reproduce the case of 2026-10-25)"
  FAILED=1
fi
NEW=$(comm -13 "$WORK/stamps1.txt" "$WORK/stamps2.txt" | tr '\n' ';')
if [ -z "$NEW" ]; then
  echo "build id test FAILED: after a change to main.cpp alone the date/time strings of the program are the same as before (the log header would say the same build)"
  FAILED=1
else
  echo "build id test: the stamp moved with the change (new strings: $NEW)"
fi
# and the header must print the stamp of build_id.cpp, which holds both pieces in ONE string
if grep -q 'kBuildStamp' "$TREE/Sources/ctr/plugin.cpp"; then
  grep -a -q -E '^[A-Z][a-z]{2} [ 0-9][0-9] 20[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}$' "$WORK/stamps2.txt" || { echo "build id test FAILED: no 'Mon DD YYYY HH:MM:SS' string in the program"; FAILED=1; }
fi
[ "$FAILED" = 0 ] && echo "build id test: OK"
exit $FAILED
