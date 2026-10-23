#!/bin/bash
# The 32-bit tests that drive UdsHle without a room server: the FFE golden replay (spec C9), the replays of the real host
# traces (FFE, TFH, the lid episode), the result-set closure (C10), the command x state matrix (T4) and the ndm:u wiring (F19).
# Run inside a Debian container with
# g++ (the build installs g++-multilib and fetches libctru on first use), from anywhere:
#   docker run --rm -v <repo>:/src -w /src devkitpro/devkitarm bash Tests/hle_tests.sh
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-/tmp/azl_tests32}
bash Tests/ipc_test/build.sh
"$OUT/ffe_replay_test"
"$OUT/trace_replay_test"
"$OUT/result_closure_test"
"$OUT/command_matrix_test"
"$OUT/ndm_wiring_test"
