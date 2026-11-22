#!/bin/bash
# The dice phase of Mario Party: Island Tour hosted by a console (2026-11-14) as a repeatable load for the model: builds the model of this tree
# (and, with BEFORE, of a tree without the remedy, under this tree's driver) and runs Tests/vsim/island_dice.py, which holds the results:
#   - before (BEFORE given): the room's stream stops for good - the console's game loses the other player, the waiting data stays at ENet's limit;
#   - this tree with the relief of the listed titles (azl/flow_relief.hpp): the game keeps its pace and the stream stays within the bound;
#   - a game that works (the sample trace): the same output from the two trees, line for line.
# Run inside the build container (gcc, objcopy, python3), from anywhere; ENet 1.3.18 UNMODIFIED is needed for the room's side, as for build.sh:
#   ENET_UP=/path/to/enet-1.3.18 [BEFORE=/path/to/the/other/AzaharLink] bash Tests/vsim/island_dice.sh [out-dir]     (out-dir defaults to /tmp/azl_vsim_dice)
# About a minute. It is a model (Tests/vsim/README.md): it says what the remedy does to the mechanism, the console decides.
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/azl_vsim_dice}
mkdir -p "$OUT"
bash Tests/vsim/build.sh "$OUT/new" > /dev/null
OLD=""
if [ -n "$BEFORE" ]; then
  # the other tree's ENet and loop constants under THIS tree's driver (the driver has the closed-loop game; the other tree's may not)
  rm -rf "$OUT/before_tree"
  mkdir -p "$OUT/before_tree"
  cp -r "$BEFORE/Sources" "$BEFORE/Includes" "$OUT/before_tree/"
  mkdir -p "$OUT/before_tree/Tests"
  cp -r Tests/vsim "$OUT/before_tree/Tests/vsim"
  (cd "$OUT/before_tree" && bash Tests/vsim/build.sh "$OUT/old" > /dev/null)
  OLD="$OUT/old/vsim"
fi
python3 Tests/vsim/island_dice.py "$OUT/new/vsim" "$OLD"
