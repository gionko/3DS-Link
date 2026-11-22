#!/bin/bash
# The fast lane: builds and runs ONLY the tests of an area, taken from Tests/run.sh as they are there (the same g++ lines, the same flags,
# the same objects before them, the same run lines): run.sh is not edited and stays the one list of the tests. The binaries of the area are
# compiled side by side (LANE_JOBS at once, all the CPUs by default); Tests/policy_check.sh runs first, as in run.sh.
#   bash Tests/lane.sh <area|test>...        areas: see below (several may be given, and single tests by the name of their binary)
#   BUILD_ONLY=1 bash Tests/lane.sh room     builds and does not run (Tests/integration.sh builds its room probes this way)
#   bash Tests/lane.sh --list                the areas and their tests, and the tests of run.sh that no area has
# OUT is /tmp/azl_tests as in run.sh. A full run.sh is still the step of the final round: a lane is for the work in between.
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-/tmp/azl_tests}
JOBS=${LANE_JOBS:-$(nproc 2>/dev/null || echo 4)}

area() {
  case "$1" in
    net)   echo test_main ring_test dedupe_test link_policy_test beacon_cache_test uds_eight_test enet_path_test enet_cap_test flow_relief_test \
                soc_send_test soc_plan_test capture_test line_test log_files_test ;;
    enet)  echo test_main ring_test enet_path_test enet_cap_test flow_relief_test soc_send_test ;;
    lists) echo test_main flow_relief_test uds_eight_test plugin_dlp_test dlp_cap_guard_test dlp_title_block_test dlp_children_test games_test paths_test public_lobbies_test ;;
    dlp)   echo dlp_lobby_test dlp_ipc_test dlp_ipc_test_joins dlp_lobby_sim_test dlp_adv_lobby_test dlp_adv_ipc_test dlp_net_test dlp_net_adv_test dlp_net_churn_test \
                dlp_net_extra_test dlp_real_engine_test dlp_hub_test dlp_hub_test_tsan plugin_dlp_test dlp_real_wire_test dlp_title_block_test \
                dlp_real_net_test dlp_real_client_test dlp_real_client_test_default dlp_real_hub_test dlp_cap_guard_test dlp_eight_test dlp_eight_hub_test dlp_real_mk7_test \
                dlp_real_island_test dlp_real_lobby_games_test make_dlp_real_lobby_games_goldens dlp_chain_report dlp_stack_check dlp_net_stack_check dlp_text_test dlp_prepare_test dlp_children_test dlp_clnt_test dlp_child_wait_test dlp_content_notice_test dlp_guest_test dlp_guest_store_test dlp_guest_run_test dlp_guest_ids_test dlp_join_test ;;
    app)   echo settings_test whole_file_test whole_file_states_test config_file_test games_test paths_test plugin_install_test eta_test dlp_text_test dlp_prepare_test guide_test about_test update_test \
                public_lobbies_test heap_plan_test zombie_plan_test room_choices_test logrot_test log_module_test ncch_icon_test builtin_keys_test dlp_children_test dlp_guest_test dlp_guest_store_test dlp_guest_run_test dlp_guest_ids_test dlp_join_test ;;
    chat)  echo chat_core_test chat_core_test_tsan chat_draw_test osk_test chat_host_test chat_host_test_tsan ;;
    hook)  echo hook_swap_test hook_swap_test_tsan gate_plan_test kernel_books_test traced_sessions_test traced_sessions_test_tsan swap_gate_test \
                swap_gate_test_tsan gsp_wrapper_test text_search_guard_test plugin_compare_test exit_guard_test plg_guard_test zombie_probe_test \
                hid_mirror_test game_card_calls_test screen_text_test sleep_crumbs_test ;;
    room)  echo room_integration lobby_probe_test room_routing_probe chat_room_probe dlp_room_probe dlp_real_room_probe room_probe room_observe room_ack_probe ;;
    all)   all_tests ;;
    *)     echo "$1" ;;
  esac
}
AREAS="net enet lists dlp app chat hook room all"

# every binary run.sh builds, and the run lines that are not a binary of $OUT (a python or bash check: named after its script)
all_tests() {
  { sed -n 's/^g++ .* -o "\$OUT\/\([A-Za-z0-9_]*\)"$/\1/p' Tests/run.sh
    sed -n 's/^  \(python3\|bash\) Tests\/\([A-Za-z0-9_]*\)\.\(py\|sh\).*/\2/p' Tests/run.sh; } | awk '!seen[$0]++' | tr '\n' ' '
}

if [ "$1" = "--list" ]; then
  for a in $AREAS; do echo "$a: $(area $a)"; done
  inzone=" $(for a in $AREAS; do [ $a = all ] || area $a; done | tr '\n' ' ') "
  for t in $(all_tests); do [[ "$inzone" == *" $t "* ]] || echo "in no area (only in 'all'): $t"; done
  exit 0
fi
[ $# -gt 0 ] || { echo "usage: bash Tests/lane.sh <area|test>...   (areas: $AREAS; --list)"; exit 2; }

WANT=" $(for a in "$@"; do area "$a"; done | tr '\n' ' ') "
known=" $(all_tests) "
for t in $WANT; do
  [[ "$known" == *" $t "* ]] || { echo "lane: '$t' is neither an area nor a test of Tests/run.sh"; exit 2; }
done

mkdir -p "$OUT"
SCRIPT="$OUT/lane_script.sh"
BUILDS="$OUT/lane_builds.txt"
# run.sh cut in three: what comes before the run lines without the g++ lines of the binaries (objects, variables, generated files: all of it,
# it is cheap), the g++ lines of the binaries asked for (side by side), and the run lines of the tests asked for
awk -v want="$WANT" -v builds="$BUILDS" -v jobs="$JOBS" '
  BEGIN { printf "" > builds }
  /^cd "\$\(dirname "\$0"\)\/\.\."$/ { next }   # (the script lives in $OUT and runs from AzaharLink already)
  /^if \[ -z "\$BUILD_ONLY" \]; then$/ {
    inrun = 1
    print "export CXXFLAGS CFLAGS CORE HUB_WRAP OUT"
    # the binaries asked for, built by xargs JOBS at once: each line is a shell line of run.sh, with the variables exported above
    print "xargs -d \"\\n\" -r -P " jobs " -n 1 bash -c < \"" builds "\""
    print
    next
  }
  inrun {
    name = ""
    if (match($0, /"\$OUT\/[A-Za-z0-9_]+"/)) { name = substr($0, RSTART + 6, RLENGTH - 7) }
    if (match($0, /^  (python3|bash) Tests\/[A-Za-z0-9_]+\.(py|sh)/)) { s = $0; sub(/^  (python3|bash) Tests\//, "", s); sub(/\..*/, "", s); name = s }
    if ($0 ~ /^  mkdir -p / || $0 ~ /^fi$/ || name == "" || index(want, " " name " ")) print
    next
  }
  /^g\+\+ / && match($0, /-o "\$OUT\/[A-Za-z0-9_]+"$/) {
    name = substr($0, RSTART + 9, RLENGTH - 10)
    if (index(want, " " name " ")) print > builds
    next
  }
  { print }
' Tests/run.sh > "$SCRIPT"
grep -q '^xargs -d ' "$SCRIPT" || { echo "lane: the run lines of Tests/run.sh were not found (its 'if [ -z \"\$BUILD_ONLY\" ]; then' line changed?)"; exit 2; }
[ -z "$LANE_SHOW" ] || { cat "$SCRIPT"; echo "--- builds:"; cat "$BUILDS"; exit 0; }   # (LANE_SHOW=1: what would run, nothing runs)
echo "lane: $(wc -l < "$BUILDS") binaries to build ($JOBS at once), tests:$WANT"
OUT="$OUT" bash "$SCRIPT"
echo "lane: done (${*})"
