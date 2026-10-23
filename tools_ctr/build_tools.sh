#!/bin/bash
# build_tools.sh -- compiles makerom (v0.19.0) and ctrtool from Project_CTR inside the devkitpro/devkitarm image and leaves the Linux binaries here:
#   tools_ctr/makerom, tools_ctr/ctrtool
# From the repository root, with Docker:   bash tools_ctr/build_tools.sh
# The binaries are not in the repository (ctrtool's folder states no license, so they are not redistributed); the build scripts run them inside Docker.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMMIT="d52c2b0e788c2d085763d7c5dcc4b2edd9ffb7d8"     # Project_CTR, the commit makerom v0.19.0 / ctrtool were built from
MSYS_NO_PATHCONV=1 docker run --rm -v "$(cd "$HERE" && pwd -W 2>/dev/null || pwd):/out" devkitpro/devkitarm bash -c '
set -euo pipefail
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq git make g++ >/dev/null
git clone -q https://github.com/3DSGuy/Project_CTR /tmp/Project_CTR
cd /tmp/Project_CTR
git checkout -q '"$COMMIT"'
make -C makerom deps >/dev/null 2>&1 || true
make -C makerom -j4
make -C ctrtool deps >/dev/null 2>&1 || true
make -C ctrtool -j4
cp makerom/bin/makerom ctrtool/bin/ctrtool /out/
chmod +x /out/makerom /out/ctrtool
{ /out/makerom -help 2>&1 || true; } | head -2
'
echo "OK: $HERE/makerom and $HERE/ctrtool"
