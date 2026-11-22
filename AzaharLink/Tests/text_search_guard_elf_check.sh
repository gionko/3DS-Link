#!/bin/bash
# The ARM side of the guard against CTRPluginFramework's startup search (Sources/ctr/text_search_guard.cpp, tested on a PC by text_search_guard_test): in the built .elf
# the framework's call to Process::GetTextSize() inside KeepThreadMain must go to the wrapper (the Makefile's --wrap), and it must be the call that is followed by
#     mov r7, r0 ; add r6, r6, #4
# which is what the guard looks for at the return address (a libctrpf that is not 0.8.0 may lay the code out differently: then this check says so, and the guard would do nothing).
# It also checks the second call the guard answers (the test of Services::GSP::Initialize, for the Download Play application): see below.
#   bash Tests/text_search_guard_elf_check.sh [AzaharLink.elf]       (inside the build container, or wherever arm-none-eabi-objdump is on the PATH)
ELF=${1:-AzaharLink.elf}
OBJDUMP=${OBJDUMP:-$(command -v arm-none-eabi-objdump || echo /opt/devkitpro/devkitARM/bin/arm-none-eabi-objdump)}
WRAP=__wrap__ZN18CTRPluginFramework7Process11GetTextSizeEv
[ -f "$ELF" ] || { echo "text search guard: $ELF is not there"; exit 2; }
"$OBJDUMP" -d --no-show-raw-insn "$ELF" > /tmp/tsg_dis.$$ 2>/dev/null || { echo "text search guard: objdump failed"; exit 2; }
trap 'rm -f /tmp/tsg_dis.$$ /tmp/tsg_raw.$$' EXIT
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
# The second job of the guard (the Download Play application, 2026-11-13): Services::GSP::Initialize tests "size of the code - 9" against an ADDRESS before it looks for the Nintendo SDK
# wrapper, and the guard answers 0x100009 to THAT call for that title. The guard knows the call by the five words at its return address: the built .elf must have exactly one call to
# the wrapper followed by those words (taken here from the guard's own source, so the two cannot drift apart), and they must read
#     ldrd r2, [sp, #12] ; sub r3, r3, r2 ; sub r0, r0, r3, asr #2 ; cmp r0, r4 ; bcc (blo)
# in a function that builds a pattern of kSdkPatternWords words (sub r1, r2, #36), and the size of the search that follows must come from ANOTHER call to the wrapper, one that is not
# followed by those words (it gets the real size: the search does not read past the title's code). A libctrpf that lays this out differently fails here, instead of leaving a guard that
# does nothing (the title would abort at start as before) or one that answers the wrong call.
# Two more things the answer 0x100009 leans on are read here: r4, the address the size is tested against, is 0x100000 when the loop is entered (mov r4, #1048576 earlier in the function), and
# the second call goes into Utils::Search(0x100000, size, pattern) (mov r0, #1048576 ; bl Utils::Search within the three instructions after it).
GUARD=${GUARD:-$(dirname "$0")/../Sources/ctr/text_search_guard.cpp}
[ -f "$GUARD" ] || { echo "text search guard: $GUARD is not there"; exit 2; }
word() { tr -d '\r' < "$GUARD" | sed -n "s/^constexpr u32 $1 = 0x\([0-9a-f]*\);.*/\1/p"; }
W0=$(word kLdrdR2R3Sp12); W1=$(word kSubR3R3R2); W2=$(word kSubR0R0R3Asr2); W3=$(word kCmpR0R4); W4=$(word kBcc | cut -c1-2)
PWORDS=$(tr -d '\r' < "$GUARD" | sed -n 's/^constexpr u32 kSdkPatternWords = \([0-9]*\);.*/\1/p')
if [ -z "$W0" ] || [ -z "$W1" ] || [ -z "$W2" ] || [ -z "$W3" ] || [ -z "$W4" ] || [ -z "$PWORDS" ]; then
  echo "text search guard FAILED: the words of the GSP test are not in $GUARD"
  exit 1
fi
"$OBJDUMP" -d "$ELF" > /tmp/tsg_raw.$$ 2>/dev/null || { echo "text search guard: objdump failed"; exit 2; }
gsp=$(awk -v w0="$W0" -v w1="$W1" -v w2="$W2" -v w3="$W3" -v w4="$W4" -v call="bl[ \t]+[0-9a-f]+ <$WRAP>" -v pat="sub[ \t]+r1, r2, #$((PWORDS * 4))([ \t]|\$)" -v search="bl[ \t]+[0-9a-f]+ <_ZN18CTRPluginFramework5Utils6SearchImEE" '
  function again() { step = ($0 ~ call) ? 1 : 0 }
  /^[0-9a-f]+ <.*>:$/ { pattern = 0; step = 0; base = 0; tail = 0; next }
  $0 ~ pat { pattern = 1 }
  $0 ~ /mov[ \t]+r4, #1048576([ \t]|$)/ { base = 1 }
  tail == -1 { if (base && $0 ~ search) searched++; tail = 0 }
  tail > 0 { tail = ($0 ~ /mov[ \t]+r0, #1048576([ \t]|$)/) ? -1 : tail - 1 }
  step == 1 { if ($2 == w0 && $0 ~ /ldrd[ \t]+r2, \[sp, #12\]/) step = 2; else again(); next }
  step == 2 { if ($2 == w1 && $0 ~ /sub[ \t]+r3, r3, r2/) step = 3; else again(); next }
  step == 3 { if ($2 == w2 && $0 ~ /sub[ \t]+r0, r0, r3, asr #2/) step = 4; else again(); next }
  step == 4 { if ($2 == w3 && $0 ~ /cmp[ \t]+r0, r4/) step = 5; else again(); next }
  step == 5 { if (substr($2, 1, 2) == w4 && $0 ~ /(bcc|blo)[ \t]/) { words++; step = 6 } else again(); next }
  step == 6 { step = ($0 ~ call) ? 7 : 0; next }
  step == 7 { if ($2 != w0) { second++; if (pattern) inpattern++; tail = 3 } again(); next }
  $0 ~ call { step = 1 }
  END { print words + 0, second + 0, inpattern + 0, searched + 0 }' /tmp/tsg_raw.$$)
set -- $gsp; gsp_words=$1; gsp_second=$2; gsp_pattern=$3; gsp_search=$4
echo "text search guard: $gsp_words call(s) to the wrapper followed by the five words of the GSP test ($W0 $W1 $W2 $W3 ${W4}......: cmp r0, r4 ; bcc), $gsp_second with the search's size from another call, $gsp_pattern in a function with a pattern of $PWORDS words"
if [ "$gsp_words" != 1 ] || [ "$gsp_second" != 1 ] || [ "$gsp_pattern" != 1 ]; then
  echo "text search guard FAILED: the test of GSP::Initialize before its search of the SDK wrapper is not (only) one call followed by ldrd r2, [sp, #12] ; sub r3, r3, r2 ; sub r0, r0, r3, asr #2 ; cmp r0, r4 ; bcc, then a second call for the size of the search, in a function with a pattern of $PWORDS words (expected 1 1 1)"
  exit 1
fi
echo "text search guard: $gsp_search search(es) of the SDK wrapper start at 0x100000 with the size of that second call, in a loop entered with r4 = 0x100000"
if [ "$gsp_search" != 1 ]; then
  echo "text search guard FAILED: the second call is not followed by mov r0, #0x100000 ; bl Utils::Search in a function that sets r4 = 0x100000 before its loop (expected 1): the answer 0x100009 of the guard is right only for a test against 0x100000 and a search that takes its size from the other call"
  exit 1
fi
echo "text search guard: OK"
