@ Stand-ins for the C handlers, only to give svc_entry.s something to call in the emulator.
@ The emulator intercepts them (it answers like the plugin would, and clobbers the caller-saved
@ registers and flags on purpose, as compiled C is allowed to).
    .arm
    .text
    .global azl_hook_SendSyncRequest
azl_hook_SendSyncRequest:
    bx lr
    .global azl_hook_CloseHandle
azl_hook_CloseHandle:
    bx lr
    .global azl_handback_site
azl_handback_site:
    mov r0, #0
    bx lr

@ The counter of the threads inside an entry (ctr/hook.cpp defines it in the plugin).
    .data
    .align 2
    .global azl_hooks_in_flight
azl_hooks_in_flight:
    .word 0
