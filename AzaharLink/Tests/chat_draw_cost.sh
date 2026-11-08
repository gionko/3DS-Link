#!/bin/bash
# The cost of the chat's lines and of the keyboard's copy on the GAME's thread, in instructions: Tests/chat_draw_cost_probe.cpp
# (azl/chat_draw.hpp, azl/chat_host.hpp's CopyImage and azl/gfx_surface.hpp, nothing of the 3DS) built with the plugin's compiler
# (pablomk7/libctrpf:0.8.0, the image the plugin is built in) and the Makefile's flags, then run in an ARM emulator by Tests/chat_draw_cost.py,
# which counts the instructions of one frame's drawing and holds them to budgets (the worst frame: 5 blocks of three full lines with the 3D on,
# in the five formats; the keyboard's copy of the whole bottom screen, checked byte by byte). The probe is built twice: as the plugin is,
# and with AZL_GFX_NO_BURST (azl/gfx_surface.hpp: a group of pixels one store a word, as on the PC), the reference whose framebuffers the
# plugin's build must equal byte by byte; the bus writes of a frame (a block store one) are counted on the plugin's build. The x86-64 test (Tests/chat_draw_test.cpp)
# counts the stores and checks every pixel, but it cannot see a call or a division a pixel on the ARM11: this does. The keyboard's picture
# drawn again on the plugin's thread (azl/osk_draw.hpp: the whole screen, and over what it showed after one change) is counted the same way.
#   bash Tests/chat_draw_cost.sh        (Git Bash, from anywhere; Docker running; Python 3 with the unicorn package; run by Tests/stack_usage.sh)
set -e
cd "$(dirname "$0")/.."
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/Includes" "$WORK/Tests"
cp -r Includes/azl "$WORK/Includes/"
cp Tests/chat_draw_cost_probe.cpp "$WORK/Tests/"
# (the Makefile's ARCH and CFLAGS/CXXFLAGS, then a bare program: no C library, no start-up files)
FLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -Os -mword-relocations -fomit-frame-pointer -ffunction-sections -fno-strict-aliasing -D__3DS__ -DAZL_USE_CTRPF -Wall -Wno-psabi -fno-rtti -fno-exceptions -std=gnu++17 -IIncludes"
(cd "$WORK" && MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/src" -w /src pablomk7/libctrpf:0.8.0 \
    bash -c "set -e; for v in probe:-UAZL_GFX_NO_BURST probe_ref:-DAZL_GFX_NO_BURST; do \$DEVKITARM/bin/arm-none-eabi-g++ $FLAGS \${v#*:} -fno-threadsafe-statics -nostdlib -nostartfiles -static -Wl,-e,_start -Wl,-Ttext-segment=0x10000 -Wl,--gc-sections -Wl,-u,azl_cost_setup -Wl,-u,azl_cost_draw -Wl,-u,azl_copy_setup -Wl,-u,azl_copy_run -Wl,-u,azl_copy_check -Wl,-u,azl_osk_setup -Wl,-u,azl_osk_draw -Wl,-u,azl_osk_check Tests/chat_draw_cost_probe.cpp -lgcc -o \${v%%:*}.elf; done") \
    || { echo "chat draw cost FAILED: the probe does not build"; exit 1; }
PY=""
for candidate in python3 python py; do
  if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -c "import sys, unicorn; sys.exit(sys.version_info[0] < 3)" >/dev/null 2>&1; then
    PY="$candidate"
    break
  fi
done
[ -n "$PY" ] || { echo "chat draw cost FAILED: no Python 3 with the unicorn package (pip install unicorn)"; exit 1; }
"$PY" Tests/chat_draw_cost.py "$WORK/probe.elf" "$WORK/probe_ref.elf"
