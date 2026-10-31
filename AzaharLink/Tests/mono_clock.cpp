// A monotonic clock for ENet in the programs that talk to a REAL room (room_integration, room_ack_probe): link with -Wl,--wrap=enet_time_get.
//
// ENet's PC platform layer (third_party/enet/unix.c) reads gettimeofday, the wall clock, and a virtual machine steps that clock when it
// synchronises with its host: Docker Desktop on Windows did it every 30 s, +58 ms and then +3.3 s against CLOCK_MONOTONIC (Tests/clockwatch.c shows it;
// measured 2026-10-27). ENet believes that 3.3 s went by: every packet in flight times out, a link that was silent for 3.7 s is "declared lost"
// (one run of the WiFi-glitch test in three), and the pauses and round trips the statistics read are wrong. The console's layer
// (Sources/enet/enet_3ds.c) reads the system tick, which is monotonic, and so does the build of Tests/enet3ds/build.sh (its shim of the tick).
#include <cstdint>
#include <ctime>

extern "C" std::uint32_t __wrap_enet_time_get(void) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint32_t>(ts.tv_sec) * 1000u + static_cast<std::uint32_t>(ts.tv_nsec / 1000000);
}
