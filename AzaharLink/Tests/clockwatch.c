/* clockwatch <seconds>: samples CLOCK_REALTIME and CLOCK_MONOTONIC every 10 ms and reports every moment the wall clock stepped against the monotonic
   one by more than 20 ms. Inside a container of Docker Desktop (WSL2) it shows +58 ms and +3.3 s every 30 s, which is what Tests/mono_clock.cpp keeps ENet out of.
     gcc -O1 -o /tmp/clockwatch Tests/clockwatch.c && /tmp/clockwatch 90 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static double ms(struct timespec t) {
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, char** argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 60;
    struct timespec r, m;
    clock_gettime(CLOCK_REALTIME, &r);
    clock_gettime(CLOCK_MONOTONIC, &m);
    double off0 = ms(r) - ms(m), last = off0, t0 = ms(m);
    int steps = 0;
    while (ms(m) - t0 < secs * 1000.0) {
        usleep(10000);
        clock_gettime(CLOCK_REALTIME, &r);
        clock_gettime(CLOCK_MONOTONIC, &m);
        double off = ms(r) - ms(m);
        if (off - last > 20 || last - off > 20) {
            printf("t=%8.1f s  the wall clock stepped by %+.1f ms against the monotonic one (offset %.1f -> %.1f)\n", (ms(m) - t0) / 1000, off - last, last - off0, off - off0);
            ++steps;
        }
        last = off;
    }
    printf("%d s watched: %d steps, the wall clock gained %.1f ms\n", secs, steps, last - off0);
    return 0;
}
