#!/bin/bash
# Assembles the REAL Sources/ctr/svc_entry.s (plus handler stubs) at the fake address the sim uses.
set -e
cd "$(dirname "$0")/../.."
export DEVKITARM=${DEVKITARM:-/opt/devkitpro/devkitARM}
P=$DEVKITARM/bin/arm-none-eabi
OUT=${1:-analysis/sim}
mkdir -p "$OUT"
$P-gcc -march=armv6k -nostdlib -nostartfiles -Wl,-Ttext=0x07000100 -Wl,--entry=azl_svc32_entry \
    -o "$OUT/entry.elf" Sources/ctr/svc_entry.s Tests/hook_sim/handler_stubs.s
$P-objcopy -O binary "$OUT/entry.elf" "$OUT/entry.bin"
$P-nm "$OUT/entry.elf" | grep -E "azl_" | awk '{print $3, $1}' > "$OUT/symbols.txt"
cat "$OUT/symbols.txt"
