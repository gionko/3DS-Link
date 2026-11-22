#!/bin/bash
# (2026-11-15, N4 of the plan) Parties of 2 to 8 players in the model: what the room's stream does to a console's network thread as the players grow, and
# what the send budget (azl/title_rules.hpp send_budget_bytes, RoomClient::SetSendBudget) and the ring read first after an absence (flow::kAwayDrainMaxMs)
# change. Four loads, each with its players and three variants of the loop (off: the queue whole every round; budget: 4096 bytes a round; budget+ring:
# and the ring's backlog read first, 50 ms at the most):
#   dice    Mario Party: Island Tour, the dice (the closed-loop game of vsim_client.c, sample/island_dice.cfg), 2 3 4 players, with the row's relief
#   mini    Island Tour's minigames (Tests/vsim/minigame_trace.py: the room's stream of one other player times the others), 2 3 4 players, with the relief
#   mk7     Mario Kart 7 (the same generator with the figures of the 63's log of 2026-11-14, a room of 3-4: about 25 frames a second of about 800 bytes from
#           each other player, the console 42 a second), 2 4 8 players, no row (as the plugin has it today: the budget is only asked "what if")
#   tetris  Tetris, 8 players (about 1 KB a second from each: 8 frames a second of 120-260 bytes), no row
# It is a model (Tests/vsim/README.md): read the DIFFERENCES between the variants on the same seeds, not the absolute values; the console decides.
# EXTRA="key=value ..." is added to every run (e.g. the ENet knobs ack_hold_events / ack_hold_ms of the README).
#   ENET_UP=/path/to/enet-1.3.18 bash Tests/vsim/party_grid.sh [out-dir]          (inside the build container; about 10 minutes; out-dir /tmp/azl_party)
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/azl_party}
mkdir -p "$OUT"
bash Tests/vsim/build.sh "$OUT/m" > /dev/null
M="$OUT/m/vsim"
SEEDS=${SEEDS:-"1 2 3 4"}
SECS=${SECS:-60}
RELIEF="same_ms=30 burst=16 cap_drop=1 cap_drop_max=2048"
R="$OUT/runs.txt"
: > "$R"
run() { # load players variant seed cfg trace extra...
  local load=$1 n=$2 var=$3 seed=$4 cfg=$5 trace=$6; shift 6
  local v=""
  case $var in
    off) v="" ;;
    budget) v="send_budget=4096" ;;
    ring) v="send_budget=4096 ring_first_ms=50" ;;
  esac
  "$M" "$cfg" "$trace" seed=$seed "$@" $v $EXTRA > "$OUT/one.txt" 2>/dev/null || true
  echo "load=$load players=$n variant=$var seed=$seed $(grep -E '^(room_disconnects|ring_drops|room_retransmits|enet_seq_skipped_away|enet_cap_refused|enet_cap_dropped|busy_ms_s|txwait_p95|txwait_p99|added_ms_p95|added_ms_p99|round_max_ms|game_con_steps_s|game_azh_steps_s|game_con_dead_at_s|game_azh_dead_at_s|budget_rounds|drain_rounds|enet_away_dgrams_max|enet_away_episodes|lost_at_s)=' "$OUT/one.txt" | tr '\n' ' ')" >> "$R"
}
for seed in $SEEDS; do
  for n in 2 3 4; do
    python3 Tests/vsim/minigame_trace.py "$OUT/mini_${n}_$seed.trace" $SECS $seed $((160 * (n - 1))) 0.08 80 > /dev/null
  done
  for n in 2 4 8; do
    python3 Tests/vsim/minigame_trace.py "$OUT/mk7_${n}_$seed.trace" $SECS $seed $((25 * (n - 1))) 0.25 42 > /dev/null
  done
  python3 Tests/vsim/minigame_trace.py "$OUT/tetris_8_$seed.trace" $SECS $seed 56 0 8 > /dev/null
done
: > "$OUT/empty.trace"
for seed in $SEEDS; do
  for var in off budget ring; do
    for n in 2 3 4; do
      run dice $n $var $seed Tests/vsim/sample/island_dice.cfg "$OUT/empty.trace" game_players=$n $RELIEF
      run mini $n $var $seed Tests/vsim/sample/minigame.cfg "$OUT/mini_${n}_$seed.trace" seconds=$SECS $RELIEF
    done
    for n in 2 4 8; do
      run mk7 $n $var $seed Tests/vsim/sample/minigame.cfg "$OUT/mk7_${n}_$seed.trace" seconds=$SECS burst=1
    done
    run tetris 8 $var $seed Tests/vsim/sample/minigame.cfg "$OUT/tetris_8_$seed.trace" seconds=$SECS burst=1
  done
done
python3 Tests/vsim/party_grid.py "$R"
