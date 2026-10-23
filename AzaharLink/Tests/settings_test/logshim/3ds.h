/* Stand-in for libctru's <3ds.h> that gives the title delivery module's log layer (ConfigApp/titleshare/source/log.c, the very file) what it
   uses of the console - a light lock, threads, a sleep and the system tick - on a PC with POSIX threads, so that
   Tests/settings_test/log_module_test.cpp can run it. Only what log.c, net.h and the headers they include touch is here. */
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;

#define SYSCLOCK_ARM11 268111856ull
#define U64_MAX UINT64_MAX
#define CUR_THREAD_HANDLE 0xFFFF8000u

typedef pthread_mutex_t LightLock;

static inline void LightLock_Init(LightLock* lock) {
    pthread_mutex_init(lock, NULL);
}

static inline void LightLock_Lock(LightLock* lock) {
    pthread_mutex_lock(lock);
}

static inline void LightLock_Unlock(LightLock* lock) {
    pthread_mutex_unlock(lock);
}

/* libctru: 0 when the lock was taken */
static inline int LightLock_TryLock(LightLock* lock) {
    return pthread_mutex_trylock(lock) == 0 ? 0 : 1;
}

typedef struct ThreadStub {
    pthread_t id;
    void (*fn)(void*);
    void* arg;
} * Thread;

static inline void* thread_entry_stub(void* p) {
    struct ThreadStub* t = (struct ThreadStub*) p;
    t->fn(t->arg);
    return NULL;
}

static inline Thread threadCreate(void (*fn)(void*), void* arg, size_t stack, s32 prio, s32 core, bool detached) {
    (void) stack;
    (void) prio;
    (void) core;
    (void) detached;
    struct ThreadStub* t = (struct ThreadStub*) malloc(sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    if(pthread_create(&t->id, NULL, thread_entry_stub, t) != 0) {
        free(t);
        return NULL;
    }

    return t;
}

static inline int threadJoin(Thread t, u64 timeout) {
    (void) timeout;
    pthread_join(t->id, NULL);
    return 0;
}

static inline void threadFree(Thread t) {
    free(t);
}

static inline void svcSleepThread(s64 ns) {
    struct timespec ts;
    ts.tv_sec = (time_t) (ns / 1000000000LL);
    ts.tv_nsec = (long) (ns % 1000000000LL);
    nanosleep(&ts, NULL);
}

static inline u64 svcGetSystemTick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((u64) ts.tv_sec * 1000000000ull + (u64) ts.tv_nsec) / 1000000000ull * SYSCLOCK_ARM11 +
           ((u64) ts.tv_nsec % 1000000000ull) * SYSCLOCK_ARM11 / 1000000000ull;
}

static inline int svcGetThreadPriority(s32* out, u32 handle) {
    (void) handle;
    *out = 0x30;
    return 0;
}
