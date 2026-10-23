#!/bin/bash
# The listing Tests/callgraph_check.py reads, made inside the plugin's build image (the host has no ARM binutils).
#   callgraph_dis.sh <elf> <output prefix>      writes <prefix>.dis (objdump -d), <prefix>.nm and <prefix>.nmc (nm -n, nm -n -C)
set -e
TOOLS=/opt/devkitpro/devkitARM/bin/arm-none-eabi
"$TOOLS-objdump" -d --no-show-raw-insn "$1" > "$2.dis"
"$TOOLS-nm" -n "$1" > "$2.nm"
"$TOOLS-nm" -n -C "$1" > "$2.nmc"
