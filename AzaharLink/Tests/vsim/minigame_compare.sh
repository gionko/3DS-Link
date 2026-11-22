#!/bin/bash
# (plugin-causa, 2026-11-14) The minigames in the model, before and after the drop at the limit (ENetHost::azlCapDrop), and the test that decides:
#   ENET_UP=<ENet 1.3.18> BEFORE=<a tree without azlCapDrop>/AzaharLink [OUT=dir] [TRACE_ARGS=...] [SENDS=...] bash Tests/vsim/minigame_compare.sh [key=value ...]
# 1. this tree with the drop OFF gives the same lines as the tree before it, run for run (every other game: nothing changes);
# 2. the tree before it: the room drops the console in some of the runs (the session of 2026-11-14 22:50);
# 3. this tree with the drop ON (the listed titles): how many runs end with the room dropping the console or a gap of 10 s for the game, and how many
#    still have refusals at the limit. Shown, not held: the model says the size of the difference, the console decides (Tests/vsim/README.md).
# Exit 0 when 1 holds. The test that fails before the remedy and passes after it is Tests/enet_cap_test.cpp (TestTheDropAtTheLimit).
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/azl_vsim_mg}
mkdir -p "$OUT"
bash Tests/vsim/build.sh "$OUT/new" > /dev/null
rm -rf "$OUT/before_tree"; mkdir -p "$OUT/before_tree/Tests"
cp -r "$BEFORE/Sources" "$BEFORE/Includes" "$OUT/before_tree/"
cp -r Tests/vsim "$OUT/before_tree/Tests/vsim"
(cd "$OUT/before_tree" && bash Tests/vsim/build.sh "$OUT/old" > /dev/null)
bash Tests/vsim/minigame_grid.sh "$OUT/old/vsim" "$@" > "$OUT/before.txt"
bash Tests/vsim/minigame_grid.sh "$OUT/new/vsim" "$@" cap_drop=0 > "$OUT/off.txt"
bash Tests/vsim/minigame_grid.sh "$OUT/new/vsim" "$@" cap_drop=1 > "$OUT/on.txt"
echo "== before (the tree without the drop)"; cat "$OUT/before.txt"
echo "== this tree, drop off (every other game)"; cat "$OUT/off.txt"
echo "== this tree, drop on (the listed titles)"; cat "$OUT/on.txt"
fail=0
if ! diff -q <(cut -d'|' -f2 "$OUT/before.txt" | awk '{$6=""; print}') <(cut -d'|' -f2 "$OUT/off.txt" | awk '{$6=""; print}') > /dev/null; then
  echo "FAIL: with the drop off this tree is not the tree before it"; fail=1
fi
before_dead=$(awk 'NR>1 && $6 != "0" {n++} END {print n+0}' "$OUT/before.txt")
before_gap=$(awk 'NR>1 && ($6 != "0" || $7 >= 10.0) {n++} END {print n+0}' "$OUT/before.txt")
on_dead=$(awk 'NR>1 && ($6 != "0" || $7 >= 10.0) {n++} END {print n+0}' "$OUT/on.txt")
on_refused=$(awk 'NR>1 && $8 != "0" {n++} END {print n+0}' "$OUT/on.txt")
runs=$(awk 'NR>1' "$OUT/on.txt" | wc -l)
echo "before: the room dropped the console in $before_dead runs of $runs (or the game went 10 s without a frame: $before_gap)"
echo "drop on: the room dropped the console or the game went 10 s without a frame in $on_dead runs of $runs; runs with a refusal at the limit: $on_refused"
exit $fail
