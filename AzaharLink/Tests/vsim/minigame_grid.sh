#!/bin/bash
# (plugin-causa, 2026-11-14) The minigames of Mario Party: Island Tour hosted by a console, in the model: a grid of seeds x costs of a send x loss
# of the WiFi, one line per run with what decides (did the room drop the console, the longest the game went without a frame of the room, the
# refusals at the limit, the room's sends of the command the console waited for). Usage, inside the build container:
#   bash Tests/vsim/minigame_grid.sh <vsim binary> [extra key=value ...]
set -e
cd "$(dirname "$0")/../.."
VS=$1; shift
SECS=${SECS:-120}
# TRACE_ARGS: room_fps big_share con_fps for minigame_trace.py (its defaults: the windows of the session)
printf "%-5s %-6s %-6s | %-8s %-9s %-9s %-8s %-8s %-8s %-8s %-8s %-8s %-8s %-8s %-8s\n" seed send loss lost_at room_drop gap_max_s refused admitted dropped hole_ms hole_att cmd_att ring_dr never e2e_p95
for send in ${SENDS:-4400 5500}; do
 for loss in ${LOSSES:-0.02 0.033}; do
  for seed in ${SEEDS:-1 2 3 4 5 6}; do
   python3 Tests/vsim/minigame_trace.py /tmp/mg_$seed.trace $SECS $seed $TRACE_ARGS > /dev/null
   "$VS" Tests/vsim/sample/minigame.cfg /tmp/mg_$seed.trace seconds=$SECS seed=$seed send_us=$send loss=$loss "$@" > /tmp/mg_out.txt || true
   g() { v=$(grep -m1 "^$1=" /tmp/mg_out.txt | cut -d= -f2); echo "${v:--}"; }
   printf "%-5s %-6s %-6s | %-8s %-9s %-9s %-8s %-8s %-8s %-8s %-8s %-8s %-8s %-8s %-8s\n" $seed $send $loss "$(g lost_at_s)" "$(g room_disconnects)" \
     "$(awk -v x="$(g deliver_gap_max_ms)" 'BEGIN{printf "%.1f", x/1000}')" "$(g enet_cap_refused)" "$(g enet_cap_admitted)" "$(g enet_cap_dropped)" "$(g enet_hol_max_ms)" \
     "$(g room_hole_attempts_max)" "$(g room_cmd_attempts_max)" "$(g ring_drops)" "$(g frames_never_delivered)" "$(g e2e_ms_p95)"
  done
 done
done
