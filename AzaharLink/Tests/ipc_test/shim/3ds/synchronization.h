// Host stand-in for libctru's <3ds/synchronization.h>: only the atomics uds.c uses.
#pragma once
#include <3ds/types.h>
#define AtomicIncrement(ptr) __atomic_add_fetch((u32*)(ptr), 1, __ATOMIC_SEQ_CST)
#define AtomicDecrement(ptr) __atomic_sub_fetch((u32*)(ptr), 1, __ATOMIC_SEQ_CST)
#define AtomicPostIncrement(ptr) __atomic_fetch_add((u32*)(ptr), 1, __ATOMIC_SEQ_CST)
#define AtomicPostDecrement(ptr) __atomic_fetch_sub((u32*)(ptr), 1, __ATOMIC_SEQ_CST)
#define AtomicSwap(ptr, value) __atomic_exchange_n((u32*)(ptr), (value), __ATOMIC_SEQ_CST)
