#!/bin/bash
# The copy of the title delivery module that the 3DS Link app builds from (ConfigApp/titleshare/common and ConfigApp/titleshare/source) is the
# original TitleShare code PLUS EXACTLY the patches documented in ConfigApp/titleshare/PATCHES.md (paths, the texts the user sees, the
# logging layer: nothing of the protocol, the sender, the receiver, the store, the CIA or the crypto). This script proves it:
#   (1) every file of the copy is what ConfigApp/titleshare/MANIFEST.md5 says, and the copy holds no other file;
#   (2) the files that the manifest says changed (MANIFEST.md5 against ORIGINALS.md5, the md5 of every original) are exactly the files
#       of ConfigApp/titleshare/vendor.patch, and every one of them is described in PATCHES.md;
#   (3) the patch taken OUT of the copy gives back the originals: applied in reverse to a temporary copy it makes every file the md5 that
#       ORIGINALS.md5 records (so the originals need not be here: the proof is in the repository);
#   (4) when ../TitleShare exists: its files are the ones ORIGINALS.md5 records, no original is missing, and the patch applied to them
#       makes the copy, byte for byte.
# Called by Tests/policy_check.sh (so by Tests/run.sh); runs anywhere with bash, md5sum, cmp and patch (inside the container ../TitleShare
# is not there: then (1) to (3) run). Tests/titleshare_vendor_make.sh makes MANIFEST.md5, ORIGINALS.md5 and vendor.patch.
cd "$(dirname "$0")/.."
FAILED=0
fail() {
  echo "titleshare vendor check FAILED: $1"
  FAILED=1
}
DIR=ConfigApp/titleshare
ORIG=../TitleShare

for f in MANIFEST.md5 ORIGINALS.md5 vendor.patch PATCHES.md; do
  [ -f "$DIR/$f" ] || { echo "titleshare vendor check FAILED: $DIR/$f is missing"; exit 1; }
done

# (1) the manifest: one line per file, "md5  path" relative to ConfigApp/titleshare
(cd "$DIR" && md5sum -c --quiet MANIFEST.md5) || fail "a vendored file differs from MANIFEST.md5 (or is missing)"
LISTED=$(awk '{print $2}' "$DIR/MANIFEST.md5" | sort)
FOUND=$(cd "$DIR" && find common source -type f | sort)
[ "$LISTED" = "$FOUND" ] || fail "the files of $DIR are not the files of MANIFEST.md5 (an extra or a missing file)"
[ -e "$DIR/source/main.c" ] || fail "source/main.c must stay vendored whole (it is the original's file; the app does not build it, the proof needs it)"
[ "$(awk '{print $2}' "$DIR/ORIGINALS.md5" | sort)" = "$LISTED" ] || fail "ORIGINALS.md5 does not list the same files as MANIFEST.md5"

# (2) the files that changed are the files of the patch, and each is described
CHANGED=$(join -j 2 <(sort -k2 "$DIR/MANIFEST.md5") <(sort -k2 "$DIR/ORIGINALS.md5") | awk '$2 != $3 {print $1}' | sort)
PATCHED=$(grep '^--- a/' "$DIR/vendor.patch" | sed 's#^--- a/##' | sort)
[ "$CHANGED" = "$PATCHED" ] || fail "the files that differ from the originals ('$(echo $CHANGED)') are not the files of vendor.patch ('$(echo $PATCHED)')"
[ "$(grep -c '^+++ b/' "$DIR/vendor.patch")" = "$(grep -c '^--- a/' "$DIR/vendor.patch")" ] || fail "vendor.patch has a file header that is not a pair"
for f in $PATCHED; do
  grep -F -q -- "\`$f\`" "$DIR/PATCHES.md" || fail "PATCHES.md does not describe the patch of $f"
done
# the patch may touch these fourteen files and no other: the key file, the log layer, the names of the cache and the park folder, the texts of the menu,
# (the user's third hardware test, 2026-10-16) the sender's screen data and its welcome of a console that comes in (sender.c, sender.h), and
# (the same test's second list, 2026-10-17) the trailer of the roster frame that tells every receiver who the others are and what they do
# (common/proto.c, common/proto.h, source/receiver.c, source/receiver.h), and
# (the second hardware test, 2026-10-17) the beacon's whole name and the dead constants (source/net.h) and the lid/periodic-line thread of the power code (source/power.c)
# (2026-10-23) the key generator's constant, which the crypto asks of the app's built-in keys instead of carrying it (common/sdcrypt.c)
# (the scheduler, the store, the CIA and the rest of the crypto, the rest of common/ and of source/, are never patched)
ALLOWED="common/proto.c common/proto.h common/sdcrypt.c source/catalog.c source/ctx.c source/log.c source/log.h source/main.c source/net.h source/power.c source/receiver.c source/receiver.h source/sender.c source/sender.h"
for f in $PATCHED; do
  case " $ALLOWED " in *" $f "*) ;; *) fail "vendor.patch changes $f (only $ALLOWED may be patched)" ;; esac
done

# (3) the patch out of the copy gives the originals
TMP=$(mktemp -d)
cp -r "$DIR/common" "$DIR/source" "$TMP/"
if (cd "$TMP" && patch --binary -R -p1 -s --no-backup-if-mismatch < "$OLDPWD/$DIR/vendor.patch" > /dev/null); then
  if ! (cd "$TMP" && md5sum -c --quiet "$OLDPWD/$DIR/ORIGINALS.md5"); then
    fail "the copy minus vendor.patch is not the original files (ORIGINALS.md5)"
  fi
else
  fail "vendor.patch does not apply in reverse to the copy"
fi
rm -rf "$TMP"

# (4) the originals
if [ -d "$ORIG/common" ] && [ -d "$ORIG/share/source" ]; then
  TMP=$(mktemp -d)
  mkdir "$TMP/common" "$TMP/source"
  cp "$ORIG"/common/* "$TMP/common/"
  cp "$ORIG"/share/source/* "$TMP/source/"
  (cd "$TMP" && md5sum -c --quiet "$OLDPWD/$DIR/ORIGINALS.md5") || fail "a file of $ORIG is not what ORIGINALS.md5 records (the original changed: take the copy again)"
  [ "$(cd "$TMP" && find common source -type f | sort)" = "$LISTED" ] || fail "the files of $ORIG are not the files of the copy (an original is missing from it, or is new)"
  (cd "$TMP" && patch --binary -p1 -s --no-backup-if-mismatch < "$OLDPWD/$DIR/vendor.patch" > /dev/null) || fail "vendor.patch does not apply to the originals in $ORIG"
  for f in $LISTED; do
    cmp -s "$TMP/$f" "$DIR/$f" || fail "the original $f plus vendor.patch is not the copy"
  done
  rm -rf "$TMP"
  ORIG_NOTE="and the originals in $ORIG plus the patch"
else
  ORIG_NOTE="(no $ORIG here: the originals were not compared; the reverse patch gave their md5)"
fi

if [ "$FAILED" = 0 ]; then
  echo "titleshare vendor check: the copy is the originals plus the $(echo $PATCHED | wc -w) documented patches $ORIG_NOTE"
fi
exit $FAILED
