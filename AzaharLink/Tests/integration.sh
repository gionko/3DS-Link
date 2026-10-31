#!/bin/bash
# Runs the integration tests against real azahar-room containers:
#   - the unit tests are only BUILT here (Tests/run.sh with BUILD_ONLY=1; run it without to run them),
#   - room_integration: engines over the real room server, then the bounded TX queue and the ENet packet
#     limit against an in-process room server that stalls or sends oversized messages; the room link and the
#     game's session: a link that is cut and opened again (through a UDP gate), a member the room still
#     lists, pauses of the network thread of 3 to 12 s, players who leave the room, the goodbye that is sent
#     before a quick stop (and its deadline), and a room that closes or kicks (in-process room server),
#   - room_integration again, linked with the plugin's own ENet platform layer (Sources/enet/enet_3ds.c):
#     the in-process part only (Tests/enet3ds/build.sh), and enet_path_test (the data path of ENet) against the same layer,
#   - room_routing_probe: how the real room routes a frame (broadcast MAC, a member's MAC, an unknown MAC),
#   - dlp_room_probe: the fake Download Play lobby over the real room (the whole lobby then the game's UDS, four players, a client that leaves and a creator that goes),
#   - ipc_test: libctru's real uds.c / ndm.c -> UdsHle -> the room (32-bit, fake kernel),
#   - ffe_replay_test: the FFE golden replay through UdsHle against an Azahar host model and two Azahar client models,
#   - trace_replay_test: the real traces of the console (FFE and TFH as host, the lid episode with a second lobby in the
#     room, a client stretch) through UdsHle against the frozen Azahar client model,
#   - result_closure_test: every command x state answers with a firmware result,
#   - command_matrix_test: every command x state answers exactly what the firmware does,
#   - ndm_wiring_test: ndm:u through the IPC front-end (a session per handle, one process, the 7th handle),
#   - dlp_fkcl_test: dlp:FKCL, the wireless check of Battle Royale (the probe), through the IPC front-end,
#   - dlp_lobby_hle_test: the fake Download Play lobby (UdsHle + DlpHub + two engines) through the IPC front-end: GetServiceHandle of
#     dlp:FKCL / dlp:SRVR, Triple Deluxe by raw words, the gate while the lobby holds the engine, closing a session, no network thread,
#     the statistics, the 8 s cap of the join's wait,
# Prereqs: Docker; image azahar-room:<tag> loaded (docker load -i azahar-room-*.dockerimage).
#   Tests/integration.sh            (from the repository root, Git Bash / Linux)
# The containers and the network carry the id of this shell, so two runs (another terminal, a reviewer) do not
# remove each other's rooms. A run that was killed outright leaves its two containers behind:
#   docker rm -f $(docker ps -aq --filter name=azl-room)
set -e
cd "$(dirname "$0")/.."
ROOM_IMAGE=${ROOM_IMAGE:-azahar-room:2126.1.2}
BUILD_IMAGE=${BUILD_IMAGE:-devkitpro/devkitarm}
NET=azl-test-net-$$
R1=azl-room1-$$
R2=azl-room2-$$
docker network create $NET >/dev/null
trap 'docker rm -f $R1 $R2 >/dev/null 2>&1 || true; docker network rm $NET >/dev/null 2>&1 || true' EXIT

docker run -d --rm --name $R1 --network $NET $ROOM_IMAGE azahar-room \
    --room-name "AzaharLink test" --port 24872 --max_members 8 \
    --preferred-app "Test Game" --preferred-app-id 0004000000164800 \
    --ban-list-file /tmp/ban1.txt --log-file /tmp/room1.log >/dev/null
docker run -d --rm --name $R2 --network $NET $ROOM_IMAGE azahar-room \
    --room-name "Locked room" --port 24872 --max_members 2 --password secret \
    --preferred-app "Test Game" --preferred-app-id 0004000000164800 \
    --ban-list-file /tmp/ban2.txt --log-file /tmp/room2.log >/dev/null
sleep 2

RC=0
# (the whole repository is mounted: the policy check, run first by Tests/run.sh, reads the neighbour DlpLink through ../DlpLink)
MSYS_NO_PATHCONV=1 docker run --rm --network $NET -v "$(cd .. && (pwd -W 2>/dev/null || pwd)):/src" -w /src/AzaharLink \
    -e BUILD_ONLY=1 -e R1=$R1 -e R2=$R2 $BUILD_IMAGE bash -c '
      bash Tests/run.sh && /tmp/azl_tests/room_integration $R1 24872 $R2 24872 &&
      bash Tests/enet3ds/build.sh && /tmp/azl_e3ds/room_integration_3ds --tx-queue && /tmp/azl_e3ds/enet_path_test_3ds &&
      /tmp/azl_tests/room_routing_probe $R1 24872 &&
      /tmp/azl_tests/dlp_room_probe $R1 24872 &&
      bash Tests/ipc_test/build.sh && /tmp/azl_tests32/ipc_test $R1 24872 &&
      /tmp/azl_tests32/ffe_replay_test && /tmp/azl_tests32/trace_replay_test && /tmp/azl_tests32/result_closure_test &&
      /tmp/azl_tests32/command_matrix_test && /tmp/azl_tests32/ndm_wiring_test && /tmp/azl_tests32/dlp_fkcl_test &&
      /tmp/azl_tests32/dlp_lobby_hle_test' || RC=$?
echo "--- room1 server log"; docker logs $R1 2>&1 | tail -15
exit $RC
