#!/bin/bash
# Assembles the REAL Sources/ctr/gate.s at the fake address of the gate's page that Tests/hook_sim/gate_sim.py uses
# (0x01E7E000; the default linker script puts .text.azl_gate in .text), and the stand-in plugin (gate_plugin_stubs.s) at the address of the plugin's block (0x07000000).
# The gate goes through the C preprocessor like in the plugin's Makefile (devkitARM's rule for .s files).
#     bash Tests/hook_sim/build_gate.sh [out-dir]      (inside a devkitARM container, from the project folder)
set -e
cd "$(dirname "$0")/../.."
export DEVKITARM=${DEVKITARM:-/opt/devkitpro/devkitARM}
P=$DEVKITARM/bin/arm-none-eabi
OUT=${1:-analysis/gate_sim}
SRC=${GATE_SRC:-Sources/ctr/gate.s}
mkdir -p "$OUT"
ARCH="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"
$P-gcc $ARCH -x assembler-with-cpp -c -o "$OUT/gate.o" "$SRC"
$P-gcc $ARCH -nostdlib -nostartfiles -Wl,-Ttext=0x01E7E000 -Wl,--entry=azl_gate_begin \
    -o "$OUT/gate.elf" "$OUT/gate.o"
$P-objcopy -O binary -j .text "$OUT/gate.elf" "$OUT/gate.bin"
$P-nm "$OUT/gate.elf" | awk '$3 ~ /^azl_gate_/ {print $3, $1}' > "$OUT/gate.sym"
$P-objdump -d "$OUT/gate.elf" > "$OUT/gate.dis"
$P-gcc $ARCH -nostdlib -nostartfiles -Wl,-Ttext=0x07000000 -Wl,--entry=plugin_pre \
    -o "$OUT/plugin.elf" Tests/hook_sim/gate_plugin_stubs.s
$P-objcopy -O binary -j .text "$OUT/plugin.elf" "$OUT/plugin.bin"
$P-nm "$OUT/plugin.elf" | awk '$3 ~ /^plugin_/ {print $3, $1}' > "$OUT/plugin.sym"
wc -c "$OUT/gate.bin" "$OUT/plugin.bin"
# The exit guard's trampoline (Sources/ctr/exit_tramp.s, REGISTRO phase Q) at 0x01E7F000, for Tests/hook_sim/exit_tramp_sim.py.
TRAMP_SRC=${TRAMP_SRC:-Sources/ctr/exit_tramp.s}
$P-gcc $ARCH -x assembler-with-cpp -c -o "$OUT/tramp.o" "$TRAMP_SRC"
$P-gcc $ARCH -nostdlib -nostartfiles -Wl,-Ttext=0x01E7F000 -Wl,--entry=azl_exit_tramp_begin -o "$OUT/tramp.elf" "$OUT/tramp.o"
$P-objcopy -O binary -j .text "$OUT/tramp.elf" "$OUT/tramp.bin"
$P-nm "$OUT/tramp.elf" | awk '$3 ~ /^azl_exit_tramp_/ {print $3, $1}' > "$OUT/tramp.sym"
