#!/bin/bash
# build.sh -- fetches bannertool and compiles it into $OUT/bannertool. Runs INSIDE the devkitarm container (it has git, gcc and the network):
#   MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/src" -w /src devkitpro/devkitarm bash DlpLink/tools/bannertool/build.sh
# bannertool (MIT, by Steveice10) makes the banner of the HOME menu from a picture and a tune. Its own repository is gone: this takes the fork of Mtgxyz at a fixed commit.
# Nothing of it is kept in this repository. Native (Linux) build, not a 3DS one: the tool runs on the build machine.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-$HERE/../../out}"
COMMIT="543e288d706fa35aafac22507a5afc5b2e526c8c"
mkdir -p "$OUT"
SRC="$OUT/bannertool_src"
rm -rf "$SRC" "$OUT/bannertool_build"
git clone -q https://github.com/Mtgxyz/bannertool "$SRC"
git -C "$SRC" checkout -q "$COMMIT"
mkdir -p "$OUT/bannertool_build"
cd "$SRC"
for f in source/pc/stb_image.c source/pc/stb_vorbis.c; do
  gcc -O2 -w -c "$f" -o "$OUT/bannertool_build/$(basename "$f" .c).o"
done
g++ -O2 -std=gnu++11 -w -Isource source/main.cpp source/cmd.cpp source/3ds/cbmd.cpp source/3ds/cwav.cpp source/3ds/lz11.cpp source/3ds/util.cpp \
  source/pc/wav.cpp "$OUT/bannertool_build/stb_image.o" "$OUT/bannertool_build/stb_vorbis.o" -o "$OUT/bannertool"
rm -rf "$OUT/bannertool_build" "$SRC"
echo "OK: $OUT/bannertool ($(stat -c %s "$OUT/bannertool") bytes)"
