#!/bin/bash
# The copy of the Download Play host library that the 3DS Link app builds from (ConfigApp/dlph) is DlpLink/host, byte for byte: nobody edits it from this
# side (a change is asked of the DlpLink session and the copy is taken again). This script proves it:
#   (1) every file of the copy is what ConfigApp/dlph/MANIFEST.md5 says, and the folder holds no other file than the four sources, the manifest and README.md;
#   (2) the manifest lists exactly the four sources the Makefile builds and the app includes (dlph.c dlph.h dlph_wire.c dlph_wire.h);
#   (3) when ../DlpLink/host exists, a source that differs from the copy is REPORTED (the library moved on: take the copy again and say so in dlph/README.md).
#       It is a note, not a failure: the other side's work in progress must not break this side's check.
# Called by Tests/policy_check.sh (so by Tests/run.sh); runs anywhere with bash, md5sum and cmp.
cd "$(dirname "$0")/.."
FAILED=0
fail() {
  echo "dlph vendor check FAILED: $1"
  FAILED=1
}
DIR=ConfigApp/dlph
ORIG=../DlpLink/host
SOURCES="dlph.c dlph.h dlph_wire.c dlph_wire.h"

for f in MANIFEST.md5 README.md $SOURCES; do
  [ -f "$DIR/$f" ] || { echo "dlph vendor check FAILED: $DIR/$f is missing"; exit 1; }
done

# (1) the manifest: one line per file, "md5  name" relative to ConfigApp/dlph
(cd "$DIR" && md5sum -c --quiet MANIFEST.md5) || fail "a vendored file differs from MANIFEST.md5 (or is missing)"
FOUND=$(cd "$DIR" && find . -type f | sed 's#^\./##' | sort)
EXPECTED=$(printf '%s\n' MANIFEST.md5 README.md $SOURCES | sort)
[ "$FOUND" = "$EXPECTED" ] || fail "the files of $DIR are not the four sources, MANIFEST.md5 and README.md ('$(echo $FOUND)')"

# (2) the manifest lists the sources and nothing else
LISTED=$(awk '{print $2}' "$DIR/MANIFEST.md5" | sed 's#^\*##' | sort)
WANTED=$(printf '%s\n' $SOURCES | sort)
[ "$LISTED" = "$WANTED" ] || fail "MANIFEST.md5 does not list exactly $SOURCES ('$(echo $LISTED)')"

# (3) the original, when it is here
if [ -d "$ORIG" ]; then
  for f in $SOURCES; do
    if [ ! -f "$ORIG/$f" ]; then
      echo "dlph vendor check NOTE: $ORIG/$f is not there any more"
    elif ! cmp -s "$ORIG/$f" "$DIR/$f"; then
      echo "dlph vendor check NOTE: $ORIG/$f differs from the copy in $DIR (the library moved on: take the copy again, see $DIR/README.md)"
    fi
  done
fi

[ "$FAILED" = "0" ] || exit 1
echo "dlph vendor check: ok"
