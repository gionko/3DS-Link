#!/bin/bash
# Builds Tests/ipc_test against libctru's REAL uds.c / ndm.c (fetched from GitHub).
# Everything is compiled as 32-bit (-m32): the IPC words carry 32-bit pointers, like on the 3DS.
# Run inside a Debian container with g++; installs the multilib toolchain on first use.
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/azl_tests32}
LIBCTRU=${LIBCTRU:-/tmp/libctru}
mkdir -p "$OUT"
if ! dpkg -s g++-multilib >/dev/null 2>&1; then
  apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq g++-multilib >/dev/null
fi
# The libctru revision the tests were last run against (LIBCTRU_REV=master takes the newest).
LIBCTRU_REV=${LIBCTRU_REV:-9b55eda44cf80b971503991e0c78f9bc8fe50425}
if [ ! -d "$LIBCTRU" ]; then
  git init -q "$LIBCTRU"
  git -C "$LIBCTRU" fetch -q --depth 1 https://github.com/devkitPro/libctru.git "$LIBCTRU_REV"
  git -C "$LIBCTRU" checkout -q FETCH_HEAD
fi
INC="-ITests/ipc_test/shim -I$LIBCTRU/libctru/include -IIncludes"
CFLAGS32="-m32 -w -O1 -g -fno-strict-aliasing"
ENETFLAGS="-DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_SOCKLEN_T=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1"

for f in callbacks compress host list packet peer protocol; do
  gcc $CFLAGS32 $ENETFLAGS -IIncludes -c Sources/enet/$f.c -o "$OUT/enet_$f.o"
done
gcc $CFLAGS32 $ENETFLAGS -IIncludes -c third_party/enet/unix.c -o "$OUT/enet_unix.o"
for f in services/uds services/ndm; do
  gcc $CFLAGS32 $INC -c "$LIBCTRU/libctru/source/$f.c" -o "$OUT/ctru_$(basename $f).o"
done
for f in "$LIBCTRU"/libctru/source/util/utf/*.c; do
  gcc $CFLAGS32 -include stddef.h $INC -c "$f" -o "$OUT/ctru_utf_$(basename $f .c).o"
done
# (dlp_hub.cpp and dlp_lobby_net.cpp: ctr/uds_hle.cpp serves the Download Play of the games in Lobby mode through the hub)
CORE="Sources/azl/crypto.cpp Sources/azl/log.cpp Sources/azl/platform.cpp Sources/azl/room_proto.cpp
      Sources/azl/uds_frames.cpp Sources/azl/uds_engine.cpp Sources/azl/room_client.cpp Sources/azl/ndm_hle.cpp
      Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp"
g++ -m32 -std=c++17 -O1 -g -w -pthread $INC \
    Tests/ipc_test/ipc_test.cpp Tests/ipc_test/shim_svc.cpp Sources/ctr/uds_hle.cpp $CORE \
    "$OUT"/ctru_*.o "$OUT"/enet_*.o -o "$OUT/ipc_test"
# The tests that drive UdsHle without libctru's client code (no room server needed): the FFE golden
# replay, the replays of the real host traces (FFE, TFH, the lid episode), the result-set closure, the
# command x state matrix, the ndm:u wiring and dlp:FKCL (the probe of Battle Royale). Same shim, same 32-bit build.
for t in ffe_replay_test trace_replay_test result_closure_test command_matrix_test ndm_wiring_test dlp_fkcl_test; do
  g++ -m32 -std=c++17 -O1 -g -w -pthread $INC \
      "Tests/$t.cpp" Tests/ipc_test/shim_svc.cpp Sources/ctr/uds_hle.cpp $CORE \
      "$OUT"/enet_*.o -o "$OUT/$t"
done
# The fake Download Play lobby through the IPC front-end: UdsHle + DlpHub + two engines on the in-memory room (no room server needed),
# raw IPC words, the GetServiceHandle / close hooks, the gate, the statistics, the 8 s cap of the join's wait.
g++ -m32 -std=c++17 -O1 -g -w -pthread $INC \
    Tests/ipc_test/dlp_lobby_hle_test.cpp Tests/ipc_test/shim_svc.cpp Sources/ctr/uds_hle.cpp $CORE \
    "$OUT"/enet_*.o -o "$OUT/dlp_lobby_hle_test"
