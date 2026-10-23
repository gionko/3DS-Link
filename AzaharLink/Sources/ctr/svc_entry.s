@ Entry points of the svc hooks.
@
@ Every `svc 0x32` (and `svc 0x23`) found in the game is replaced by a jump to a small per-site
@ trampoline (see hook.cpp):
@
@       push {lr}
@       bl   azl_svc32_entry        @ <- these
@       pop  {lr}
@       <the instruction that followed the svc, relocated>
@       ldr  pc, [pc, #-4] ; .word <address after the original instruction>
@
@ On entry r0 holds the svc argument (a handle) and lr the return address inside the trampoline.
@ The C handler returns the result in r0. Every other register and the condition flags are
@ preserved, because the game's code was compiled expecting the kernel to preserve them.
@
@ The game's stack pointer is only guaranteed to be word aligned; AAPCS (and ldrd/strd) want eight
@ bytes, so the stack is aligned here before calling into C.

    .arm
    .text
    .align 2

    .macro SVC_ENTRY name, handler
    .global \name
    .type \name, %function
\name:
    push {r1-r4, r12, lr}
    mrs  r12, cpsr
    mov  r4, sp
    bic  sp, sp, #7
    push {r4, r12}
    bl   \handler
    pop  {r4, r12}
    mov  sp, r4
    msr  cpsr_f, r12
    pop  {r1-r4, r12, lr}
    bx   lr
    .size \name, . - \name
    .endm

    SVC_ENTRY azl_svc32_entry, azl_hook_SendSyncRequest
    SVC_ENTRY azl_svc23_entry, azl_hook_CloseHandle
