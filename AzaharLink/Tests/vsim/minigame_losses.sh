#!/bin/bash
# (plugin-causa, 2026-11-14) Where the holes come from, in the model: the same 10 minutes of minigames with the WiFi's loss and the console's receive ring
# (64 KB) on and off, before (BEFORE's ENet) and with the drop at the limit. One line per variant: the runs where the room dropped the console, the
# refusals at the limit, the longest hole, the datagrams the ring lost.
#   ENET_UP=<ENet 1.3.18> BEFORE=<a tree without azlCapDrop>/AzaharLink bash Tests/vsim/minigame_losses.sh
set -e
cd "$(dirname "$0")/../.."
bash Tests/vsim/build.sh /tmp/mgl_new > /dev/null
rm -rf /tmp/mgl_bt; mkdir -p /tmp/mgl_bt/Tests
cp -r "$BEFORE/Sources" "$BEFORE/Includes" /tmp/mgl_bt/
cp -r Tests/vsim /tmp/mgl_bt/Tests/
(cd /tmp/mgl_bt && bash Tests/vsim/build.sh /tmp/mgl_old > /dev/null)
SECS=${SECS:-600}
for seed in 1 2 3 4 5 6; do python3 Tests/vsim/minigame_trace.py /tmp/mgl_$seed.trace $SECS $seed > /dev/null; done
for v in "loss=0.02 rcvbuf_bytes=65536" "loss=0 rcvbuf_bytes=65536" "loss=0.02 rcvbuf_bytes=4194304" "loss=0 rcvbuf_bytes=4194304"; do
  for tree in old new; do
    extra=""; [ $tree = new ] && extra="cap_drop=1"
    for send in 4400 5500; do
      for seed in 1 2 3 4 5 6; do
        /tmp/mgl_$tree/vsim Tests/vsim/sample/minigame.cfg /tmp/mgl_$seed.trace seconds=$SECS seed=$seed send_us=$send $v $extra > /tmp/mgl_r.txt || true
        g() { v2=$(grep -m1 "^$1=" /tmp/mgl_r.txt | cut -d= -f2); echo "${v2:-0}"; }
        echo "$(g room_disconnects) $(g enet_cap_refused) $(g enet_hol_max_ms) $(g ring_drops)"
      done
    done | awk -v name="$v ${tree} ${extra}" '{d+=$1; r+=$2; if ($3>h) h=$3; rd+=$4; n++} END {printf "%-40s runs %d: room drops %d, refused %d, longest hole %d ms, ring drops %d\n", name, n, d, r, h, rd}'
  done
done
