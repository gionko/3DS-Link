#!/bin/bash
# room_ack.sh: what the ENet client's data path (acknowledgement order, the receive that only says "nothing", the read passes) does
# against the REAL azahar-room, with the room's own ENet timers and a wire with the delay of a LAN. Two members join one room; one
# sends a broadcast frame every 1000/fps ms (UNSEQUENCED, like radio_like=1), the other takes it, works on it for handler_ms (the
# engine, the debug capture) and answers with a frame of its own. Per run it prints the delay of the frame, the datagrams the receiver
# got beyond the frames (the room's retransmissions) and the datagrams it sent (ack only / ack + data / data only).
# The wire: a user-space proxy between the members and the room (Tests/udp_gate.cpp: DELAY_MS each way, JITTER_MS, LOSS) - the Docker
# Desktop kernel has no netem. The two members talk to the proxy on two ports (30001, 30002).
#   Tests/room_ack.sh                       (from the repository root, Git Bash / Linux; Docker; image azahar-room:<tag>)
#   SECONDS_PER_RUN=15 DELAY_MS=4 JITTER_MS=1 LOSS=0 Tests/room_ack.sh
# To compare two trees run it from each (the original one and the patched one) and put the lines side by side.
set -e
cd "$(dirname "$0")/.."
ROOM_IMAGE=${ROOM_IMAGE:-azahar-room:2126.1.2}
BUILD_IMAGE=${BUILD_IMAGE:-devkitpro/devkitarm}
SECONDS_PER_RUN=${SECONDS_PER_RUN:-15}
DELAY_MS=${DELAY_MS:-4}
JITTER_MS=${JITTER_MS:-1}
LOSS=${LOSS:-0}
RUNS=${RUNS:-"60:0:1 60:3:1 60:6:1 30:3:1 60:3:0"} # fps:handler_ms:answer
NET=azl-ack-net-$$
R=azl-ack-room-$$
P=azl-ack-probe-$$
docker network create $NET >/dev/null
trap 'docker rm -f $R $P >/dev/null 2>&1 || true; docker network rm $NET >/dev/null 2>&1 || true' EXIT
SRC="$(pwd -W 2>/dev/null || pwd)"

docker run -d --rm --name $R --network $NET $ROOM_IMAGE azahar-room \
    --room-name "AzaharLink test" --port 24872 --max_members 8 \
    --preferred-app "Test Game" --preferred-app-id 0004000000164800 \
    --ban-list-file /tmp/ban1.txt --log-file /tmp/room1.log >/dev/null
MSYS_NO_PATHCONV=1 docker run -d --rm --name $P --network $NET -v "$SRC:/src" -w /src $BUILD_IMAGE sleep 3600 >/dev/null
# the probe and the proxy, built in the container (the unit tests are not needed): ENet + the core + the probe, with the send wrapped to classify the datagrams
MSYS_NO_PATHCONV=1 docker exec $P bash -c '
  set -e
  O=/tmp/ack; mkdir -p $O
  CFLAGS="-O1 -g -IIncludes -DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_SOCKLEN_T=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1 -w"
  for f in callbacks compress host list packet peer protocol; do gcc $CFLAGS -c Sources/enet/$f.c -o $O/enet_$f.o; done
  gcc $CFLAGS -c third_party/enet/unix.c -o $O/enet_unix.o
  CORE="Sources/azl/crypto.cpp Sources/azl/log.cpp Sources/azl/platform.cpp Sources/azl/room_proto.cpp Sources/azl/uds_frames.cpp Sources/azl/uds_engine.cpp Sources/azl/room_client.cpp Sources/azl/ndm_hle.cpp Sources/azl/arm_reloc.cpp Sources/azl/svc_patch.cpp Sources/azl/log_files.cpp"
  CNT=; grep -q ENetAzlCounters Includes/enet/enet.h && CNT=-DP5A_COUNTERS=1
  g++ -std=c++17 -O1 -g -w -IIncludes -pthread $CNT Tests/room_ack_probe.cpp Tests/mono_clock.cpp $CORE $O/enet_*.o -Wl,--wrap=enet_socket_send -Wl,--wrap=enet_socket_receive -Wl,--wrap=enet_time_get -o $O/room_ack_probe
  g++ -std=c++17 -O2 -w -pthread Tests/udp_gate.cpp -o $O/udp_gate'
sleep 2
MSYS_NO_PATHCONV=1 docker exec -d $P /tmp/ack/udp_gate --room $R:24872 --client 30001 --client 30002 --seed 7 --both-delay $DELAY_MS --both-jit $JITTER_MS --both-loss $LOSS
sleep 1
echo "wire: ${DELAY_MS} ms each way, jitter ${JITTER_MS} ms, loss ${LOSS}; ${SECONDS_PER_RUN} s per run; tree $(git -C . rev-parse --short HEAD 2>/dev/null || echo '?')"
for r in $RUNS; do
  IFS=: read fps handler answer <<<"$r"
  MSYS_NO_PATHCONV=1 docker exec $P /tmp/ack/room_ack_probe 127.0.0.1 30001 $SECONDS_PER_RUN $fps $handler $answer "fps$fps-h${handler}-a$answer" 30002
done
echo "--- room log"; docker logs $R 2>&1 | tail -5
