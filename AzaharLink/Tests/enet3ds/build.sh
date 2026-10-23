#!/bin/bash
# Builds room_integration against the plugin's own ENet platform layer (Sources/enet/enet_3ds.c) instead of ENet's
# unix.c, with a shim for the one libctru call it makes. That file carries the CPU optimisations (the recvfrom
# that is skipped after a poll timeout), the SO_RCVBUF negotiation and the soc counters, and nothing else runs it
# off the console. Run inside the build container (Linux, g++), from anywhere:
#   bash Tests/enet3ds/build.sh && /tmp/azl_e3ds/room_integration_3ds --tx-queue
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/azl_e3ds}
mkdir -p "$OUT"
CFLAGS="-O1 -g -IIncludes -ITests/enet3ds/shim -DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_SOCKLEN_T=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1 -w"
for f in callbacks compress host list packet peer protocol; do
  gcc $CFLAGS -c Sources/enet/$f.c -o "$OUT/enet_$f.o"
done
gcc $CFLAGS -c Sources/enet/enet_3ds.c -o "$OUT/enet_3ds.o"
CORE="Sources/azl/crypto.cpp Sources/azl/log.cpp Sources/azl/platform.cpp Sources/azl/room_proto.cpp
      Sources/azl/uds_frames.cpp Sources/azl/uds_engine.cpp Sources/azl/room_client.cpp
      Sources/azl/ndm_hle.cpp Sources/azl/arm_reloc.cpp Sources/azl/svc_patch.cpp"
g++ -std=c++17 -O1 -g -w -DAZL_E3DS=1 -IIncludes -pthread Tests/room_integration.cpp $CORE "$OUT"/enet_*.o -o "$OUT/room_integration_3ds"
