// Host stand-in for libctru's <3ds/svc.h> (the real one is ARM inline assembly).
#pragma once
#include <3ds/types.h>
#include <3ds/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { RESET_ONESHOT = 0, RESET_STICKY = 1, RESET_PULSE = 2 } ResetType;
typedef enum { MEMPERM_READ = 1, MEMPERM_WRITE = 2, MEMPERM_EXECUTE = 4 } MemPerm;

#ifndef CUR_PROCESS_HANDLE
#define CUR_PROCESS_HANDLE 0xFFFF8001
#endif
#ifndef CUR_THREAD_HANDLE
#define CUR_THREAD_HANDLE 0xFFFF8000
#endif

u32* getThreadCommandBuffer(void);
u32* getThreadStaticBuffers(void);

Result svcCreateEvent(Handle* event, ResetType reset_type);
Result svcSignalEvent(Handle handle);
Result svcClearEvent(Handle handle);
Result svcWaitSynchronization(Handle handle, s64 nanoseconds);
Result svcCloseHandle(Handle handle);
Result svcDuplicateHandle(Handle* out, Handle original);
void svcSleepThread(s64 ns);
Result svcGetThreadPriority(s32* out, Handle handle);
Result svcGetThreadId(u32* out, Handle handle);
Result svcSendSyncRequest(Handle session);
Result svcGetProcessId(u32* out, Handle process);
Result svcCreateMemoryBlock(Handle* memblock, u32 addr, u32 size, MemPerm my_perm, MemPerm other_perm);

#ifdef __cplusplus
}
#endif
