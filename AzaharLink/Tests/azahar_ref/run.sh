#!/bin/bash
# Differential test against the original Azahar frame generators.
# Needs network access (fetches Azahar sources from GitHub, installs crypto++/fmt via apt).
# Run inside a Debian-based container with g++ (e.g. the devkitpro/devkitarm image):
#   docker run --rm -v <repo>:/src -w /src devkitpro/devkitarm bash Tests/azahar_ref/run.sh
set -e
cd "$(dirname "$0")/../.."
WORK=${WORK:-/tmp/azl_ref}
# The Azahar release of the room server image the integration tests use (azahar-room:2126.1.2); AZAHAR_REF=master
# compares with the newest code instead.
AZAHAR_REF=${AZAHAR_REF:-2126.1.2}
BASE=https://raw.githubusercontent.com/azahar-emu/azahar/$AZAHAR_REF/src
NWM=core/hle/service/nwm
mkdir -p "$WORK/src/$NWM" "$WORK/src/common"

if ! dpkg -s libcrypto++-dev libfmt-dev >/dev/null 2>&1; then
  apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq libcrypto++-dev libfmt-dev >/dev/null
fi

for f in uds_beacon.cpp uds_beacon.h uds_data.cpp uds_data.h uds_connection.cpp uds_connection.h \
         uds_common.h nwm_uds.h; do
  curl -fsSL -o "$WORK/src/$NWM/$f" "$BASE/$NWM/$f"
done
curl -fsSL -o "$WORK/src/common/swap.h" "$BASE/common/swap.h"

# nwm_uds.h pulls in the whole kernel; keep only the plain data definitions (from the namespace
# opening up to the NWM_UDS class).
{
  echo '#pragma once'
  echo '#include <array>'
  echo '#include <vector>'
  echo '#include "common/common_types.h"'
  echo '#include "common/swap.h"'
  echo '#include "core/hle/service/service.h"'
  echo '#include "core/hle/service/nwm/uds_common.h"'
  awk '/^namespace Service::NWM \{/ {on=1} /^class NWM_UDS final/ {exit} on {print}'       "$WORK/src/$NWM/nwm_uds.h"
  echo '} // namespace Service::NWM'
} > "$WORK/src/$NWM/nwm_uds.h.new"
mv "$WORK/src/$NWM/nwm_uds.h.new" "$WORK/src/$NWM/nwm_uds.h"

INC="-Itests_shim -I$WORK/src -IIncludes"
rm -rf "$WORK/shim" && cp -r Tests/azahar_ref/shim "$WORK/shim"
# the shim's headers must win over the (absent) real ones; swap.h comes from the real tree
INC="-I$WORK/shim -I$WORK/src -IIncludes"

g++ -std=c++20 -O1 -g -w -fsanitize=address,undefined $INC \
    Tests/azahar_ref/diff_test.cpp \
    "$WORK/src/$NWM/uds_beacon.cpp" "$WORK/src/$NWM/uds_data.cpp" "$WORK/src/$NWM/uds_connection.cpp" \
    Sources/azl/uds_frames.cpp Sources/azl/crypto.cpp \
    -lcryptopp -lfmt -o "$WORK/diff_test"
"$WORK/diff_test"
