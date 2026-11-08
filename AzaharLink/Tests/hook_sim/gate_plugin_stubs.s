@ A stand-in for the plugin's pre/post (Sources/ctr/hook.cpp), only to give Sources/ctr/gate.s something to call in the
@ emulator (Tests/hook_sim/gate_sim.py). Assembled at the address of the plugin's block, which the sim unmaps for real
@ during the swap. They clobber on purpose what compiled C may clobber (r1-r3, r12, the condition flags); the sim checks
@ their arguments, the stack and the context from outside (code hooks), so they stay leaves.
    .arm
    .syntax unified
    .text
    .global plugin_pre, plugin_post, plugin_action, plugin_answer

plugin_pre:                           @ (handle, stub+8, ctx) -> r0 = handle or answer, r1 = action
    ldr   r12, plugin_action
    str   r12, [r2, #0]               @ ctx[0..2]: values only
    str   r0, [r2, #4]
    str   r1, [r2, #8]
    cmp   r12, #0
    ldreq r0, plugin_answer           @ action 0: a fake session answered here
    mov   r1, r12
    mov   r2, #0xAA
    mov   r3, #0xBB
    mov   r12, #0xF0000000
    msr   cpsr_f, r12                 @ trash the flags
    mov   r12, #0xCC
    bx    lr

plugin_post:                          @ (result, stub+8, ctx) -> r0 = result
    add   r0, r0, #0x100              @ a post that changes the result: the sim sees whether it ran
    mov   r1, #0x11
    mov   r2, #0x22
    mov   r3, #0x0
    msr   cpsr_f, r3                  @ trash the flags
    mov   r12, #0xDD
    bx    lr

plugin_action: .word 1
plugin_answer: .word 0xC0DE0000
