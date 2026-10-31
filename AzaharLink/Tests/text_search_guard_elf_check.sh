#!/bin/bash
# The ARM side of the guard against CTRPluginFramework's startup search (Sources/ctr/text_search_guard.cpp, tested on a PC by text_search_guard_test): in the built .elf
# the framework's call to Process::GetTextSize() inside KeepThreadMain must go to the wrapper (the Makefile's --wrap), and it must be the call that is followed by
#     mov r7, r0 ; add r6, r6, #4
# which is what the guard looks for at the return address (a libctrpf that is not 0.8.0 may lay the code out differently: then this check says so, and the guard would do nothing).
#   bash Tests/text_search_guard_elf_check.sh [AzaharLink.elf]       (inside the build container, or wherever arm-none-eabi-objdump is on the PATH)
ELF=${1:-AzaharLink.elf}
OBJDUMP=${OBJDUMP:-$(command -v arm-none-eabi-objdump || echo /opt/devkitpro/devkitARM/bin/arm-none-eabi-objdump)}
WRAP=__wrap__ZN18CTRPluginFramework7Process11GetTextSizeEv
[ -f "$ELF" ] || { echo "text search guard: $ELF is not there"; exit 2; }
"$OBJDUMP" -d --no-show-raw-insn "$ELF" > /tmp/tsg_dis.$$ 2>/dev/null || { echo "text search guard: objdump failed"; exit 2; }
trap 'rm -f /tmp/tsg_dis.$$' EXIT
grep -q "<$WRAP>:" /tmp/tsg_dis.$$ || { echo "text search guard FAILED: the wrapper $WRAP is not in $ELF"; exit 1; }
calls=$(grep -c "bl.*<$WRAP>" /tmp/tsg_dis.$$)
sites=$(awk -v w="<$WRAP>" '
  /bl.*<__wrap__ZN18CTRPluginFramework7Process11GetTextSizeEv>/ { want = 2; next }
  want == 2 { if ($0 ~ /mov[ \t]+r7, r0/) { want = 1 } else { want = 0 }; next }
  want == 1 { if ($0 ~ /add[ \t]+r6, r6, #4/) { n++ }; want = 0 }
  END { print n + 0 }' /tmp/tsg_dis.$$)
echo "text search guard: $calls call(s) to the wrapper, $sites of them followed by mov r7, r0 ; add r6, r6, #4"
if [ "$sites" != 1 ]; then
  echo "text search guard FAILED: the call in KeepThreadMain is not (only) one followed by mov r7, r0 ; add r6, r6, #4 (expected exactly 1 such site)"
  exit 1
fi
echo "text search guard: OK"
