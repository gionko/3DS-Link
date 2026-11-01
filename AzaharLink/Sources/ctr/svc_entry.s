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
@
@ Two things more, both for the swap of the plugin's memory on an Old 3DS (azl/hook_swap.hpp):
@
@  * azl_hooks_in_flight counts the threads that are inside an entry (ldrex/strex, a barrier on each side of the work): the hook
@    waits for it to reach zero before the framework lets Luma3DS free the block. It costs registers only, no stack.
@
@  * azl_svc32_entry looks at the command buffer of the calling thread before it calls C: APT:ReceiveParameter with the application's id
@    (0x000D0080, 0x300) is the call at whose end the kernel raises the swap, ON that thread, and puts it to sleep until the swap
@    is over. If that call is made from here it wakes up in memory that has been freed. So the C function azl_handback_site is asked
@    first; when it returns a call site, the entry does NOT make the call: it gives the thread back to that site with every register,
@    the flags, the stack and lr as they were when the game's code reached the site (the trampoline's `push {lr}` undone), and
@    the site holds the game's own `svc` again, so the game makes the call itself, from its own code. Every other call, and this one
@    when azl_handback_site returns 0 (not an Old 3DS, no HOME button just now, hooks out already), goes the normal way.

    .arm
    .text
    .align 2

    .macro DMB scratch
    mcr  p15, 0, \scratch, c7, c10, 5
    .endm

    .macro SVC_ENTRY name, handler, handback
    .global \name
    .type \name, %function
\name:
    push {r1-r4, r12, lr}
    mrs  r12, cpsr
    ldr  r2, .Lin_flight\@
1:  ldrex r1, [r2]
    add  r1, r1, #1
    strex r3, r1, [r2]
    cmp  r3, #0
    bne  1b
    DMB  r3
    .if \handback
    mrc  p15, 0, r2, c13, c0, 3        @ the thread's TLS: the command buffer is at +0x80
    ldr  r3, [r2, #0x80]
    ldr  r1, .Lreceive\@
    cmp  r3, r1
    ldreq r3, [r2, #0x84]
    cmpeq r3, #0x300
    bne  .Lcall\@
    mov  r4, sp                        @ APT:ReceiveParameter (application): may the call be made by the game itself?
    bic  sp, sp, #7
    push {r0, r1, r4, r12}             @ (r1: only to keep the stack at eight bytes)
    ldr  r1, [r4, #20]                 @ the return address of the trampoline's bl, saved by the first push above
    bl   azl_handback_site
    mov  r3, r0                        @ the call site, or 0
    pop  {r0, r1, r4, r12}
    mov  sp, r4
    cmp  r3, #0
    bne  .Lhandback\@
.Lcall\@:
    .endif
    mov  r4, sp
    bic  sp, sp, #7
    push {r4, r12}
    bl   \handler
    pop  {r4, r12}
    mov  sp, r4
    ldr  r2, .Lin_flight\@
    DMB  r3
2:  ldrex r1, [r2]
    sub  r1, r1, #1
    strex r3, r1, [r2]
    cmp  r3, #0
    bne  2b
    msr  cpsr_f, r12
    pop  {r1-r4, r12, lr}
    bx   lr
    .if \handback
.Lhandback\@:
    @ r0 = the handle, r3 = the call site, r12 = the flags, sp = the bottom of the frame above (r1-r4, r12, lr, then the game's lr
    @ that the trampoline pushed). The call leaves the hook: not in flight any more.
    ldr  r2, .Lin_flight\@
    DMB  r1
3:  ldrex r1, [r2]
    sub  r1, r1, #1
    strex r4, r1, [r2]
    cmp  r4, #0
    bne  3b
    msr  cpsr_f, r12
    ldr  r2, [sp, #24]                 @ the game's lr
    str  r3, [sp, #24]                 @ the call site goes where it was: `ldr pc, [sp], #4` below takes it
    str  r2, [sp, #20]                 @ and the game's lr takes the place of the entry's, which pop {..., lr} restores
    pop  {r1-r4, r12, lr}
    ldr  pc, [sp], #4
.Lreceive\@:
    .word 0x000D0080
    .endif
.Lin_flight\@:
    .word azl_hooks_in_flight
    .size \name, . - \name
    .endm

    SVC_ENTRY azl_svc32_entry, azl_hook_SendSyncRequest, 1
    SVC_ENTRY azl_svc23_entry, azl_hook_CloseHandle, 0
