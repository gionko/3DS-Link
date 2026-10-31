#!/bin/bash
# Builds the virtual-time model of the plugin's network thread (Tests/vsim/README.md). Run inside the build container (gcc, objcopy), from
# anywhere; it needs ENet 1.3.18 UNMODIFIED for the room's side (the sources of lsalzman/enet v1.3.18: the *.c files and include/):
#   ENET_UP=/path/to/enet-1.3.18 bash Tests/vsim/build.sh [out-dir]       (out-dir defaults to /tmp/azl_vsim)
# The plugin's side is THIS tree's Sources/enet with its own platform layer (Sources/enet/enet_3ds.c), so the same script built from the
# original tree and from the patched one gives the two models to compare.
set -e
cd "$(dirname "$0")/../.."
UP=${ENET_UP:?set ENET_UP to the unmodified ENet 1.3.18 source tree}
OUT=${1:-/tmp/azl_vsim}
V=Tests/vsim
mkdir -p "$OUT/obj"
CF="-O2 -g -Wall -Wno-unused-function -Wno-unused-variable"
# the ENet fields and counters the patched tree has (the model's knobs); the original tree does not have them
KN=""; grep -q azlAckHoldEvents Includes/enet/enet.h && KN="-DVSIM_KNOBS=1"
for f in callbacks compress host list packet peer protocol; do gcc $CF -IIncludes -c Sources/enet/$f.c -o "$OUT/obj/c_$f.o"; done
gcc $CF -IIncludes -I$V/shim -c Sources/enet/enet_3ds.c -o "$OUT/obj/c_plat.o"
gcc $CF -IIncludes -I$V $KN -c $V/vsim_client.c -o "$OUT/obj/c_driver.o"
gcc $CF -I$V -c $V/vnet.c -o "$OUT/obj/c_vnet.o"
# the room's ENet: upstream, every enet_* symbol renamed r_enet_* so that both live in one program
for f in callbacks compress host list packet peer protocol; do gcc $CF -I$UP/include -DENET_BUILDING_LIB=1 -c $UP/$f.c -o "$OUT/obj/r_$f.o"; done
gcc $CF -I$UP/include -I$V -c $V/room_plat.c -o "$OUT/obj/r_plat.o"
gcc $CF -I$UP/include -I$V -c $V/room_side.c -o "$OUT/obj/r_side.o"
( cd "$OUT/obj"
  nm -P r_*.o | awk '$1 ~ /^enet_/ {print $1}' | sort -u | awk '{print $1" r_"$1}' > r_map.txt
  for f in r_*.o; do objcopy --redefine-syms=r_map.txt $f; done
  gcc -o ../vsim c_*.o r_*.o -lm )
echo "built $OUT/vsim ${KN:+(with the knobs of the patched tree)}"
echo "run:  $OUT/vsim Tests/vsim/sample/TFH_friend_b1.cfg Tests/vsim/sample/TFH_friend_b1.trace   [key=value lines of the cfg can be added to the file]"
