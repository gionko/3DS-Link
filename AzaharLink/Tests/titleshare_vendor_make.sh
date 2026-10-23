#!/bin/bash
# Makes the three files that prove what the copy of the title delivery module in ConfigApp/titleshare is, from the originals in ../TitleShare
# and the copy itself (run it after a patch is changed, from a place that has ../TitleShare; Tests/titleshare_vendor_check.sh then checks them):
#   ORIGINALS.md5   the md5 of every ORIGINAL file (what the files were when they were taken)
#   MANIFEST.md5    the md5 of every file of the copy (what they are now)
#   vendor.patch    the unified diff original -> copy of every file that differs (paths a/<file> b/<file>, <file> as in the manifest)
# The patches themselves are in the files of the copy (edited by hand) and described in ConfigApp/titleshare/PATCHES.md.
cd "$(dirname "$0")/.."
DIR=ConfigApp/titleshare
ORIG=../TitleShare
[ -d "$ORIG/common" ] && [ -d "$ORIG/share/source" ] || { echo "no $ORIG here: nothing to make from"; exit 1; }

TMP=$(mktemp -d)
mkdir -p "$TMP/a/common" "$TMP/a/source"
cp "$ORIG"/common/* "$TMP/a/common/"
cp "$ORIG"/share/source/* "$TMP/a/source/"

# the originals' md5, in the copy's file list (a file that is only in one of them is an error the check reports)
(cd "$DIR" && find common source -type f | sort) > "$TMP/list"
(cd "$TMP/a" && xargs md5sum -t < "$TMP/list") > "$DIR/ORIGINALS.md5"
(cd "$DIR" && xargs md5sum -t < "$TMP/list") > "$DIR/MANIFEST.md5"

# the patch: only the files that differ
: > "$DIR/vendor.patch"
while read -r f; do
  if ! cmp -s "$TMP/a/$f" "$DIR/$f"; then
    diff -u --label "a/$f" --label "b/$f" "$TMP/a/$f" "$DIR/$f" >> "$DIR/vendor.patch"
  fi
done < "$TMP/list"
rm -rf "$TMP"
echo "made $DIR/ORIGINALS.md5, $DIR/MANIFEST.md5 and $DIR/vendor.patch ($(grep -c '^--- a/' "$DIR/vendor.patch") patched files)"
