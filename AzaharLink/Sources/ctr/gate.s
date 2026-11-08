@ The persistent gate of the svc hook ("porta persistente").
@
@ The whole section is copied ONCE, before the game's code runs, into a page of the game's process that the swap of
@ the plugin's block (Old 3DS: HOME menu, power menu, library applets) never unmaps. Every hooked call site holds ONE
@ word, `b stub_i`, and every stub is three words, written once too:
@
@       stub_i:  str  lr, [sp, #-4]!        @ the game's lr
@                bl   azl_gate_32           @ (or azl_gate_23)
@                b    site_i + 4            @ back to the game's own next instruction
@
@ After that nothing is ever written again in the game's text or in this page's code: a game thread that sleeps in a
@ `svc` (here, or anywhere) wakes up on the very instructions it went to sleep on, whatever the swap did meanwhile.
@ The swap only flips `flag`, a word of data:
@
@   * 1 = the plugin is there: the gate asks pre() and, if told so, post() in the plugin's block;
@   * 0 = the plugin is going or gone: the gate makes the call straight to the kernel and touches nothing of the plugin.
@
@ Closing (SWAP_ENTER): flag = 0, a barrier, then wait for in_flight == 0. A thread counts itself (ldrex/strex) and then,
@ after a barrier, reads the flag: either the closer sees the count, or the thread sees 0 (Dekker). The plugin's code is
@ run by a game thread only between the count going up and coming down, and no real `svc` is ever made from the plugin's
@ block: the svc of the game is made HERE, uncounted, so a thread that sleeps in it does not hold the swap up and does
@ not wake up in memory that is gone.
@
@ pre(handle, stub+8, ctx) returns r0 = the handle for the svc (or the answer) and r1 = the action:
@     0 = answered by the plugin (r0 is the result), 1 = make the svc, 2 or more = make the svc, then call post.
@ post(result, stub+8, ctx) returns the result in r0. It is called only if the flag is (still, or again) 1; when it is
@ not, post_skipped counts it. ctx is four words on the game's stack: ctx[0..2] belong to pre/post (values only, never
@ a pointer into the plugin's block); ctx[3] is the action, written by the gate. The site's index is the plugin's to
@ work out from stub+8: (stub+8 - 8 - first stub) / 12.
@
@ The game's registers (r0 aside) and condition flags come back as they were; the plugin's C code may clobber what AAPCS
@ lets it, so r1-r3 and r12 are saved, and the stack is aligned to eight bytes at every call into the plugin. The game's
@ stack below its SP: 4 bytes (the stub) + 20 + 4 (the saved flags) + 16 (ctx) = 44 bytes, 48 when one pad word is
@ needed for the alignment, at the call into pre/post.
@
@ The section is position independent (adr and pc-relative loads only). Until the plugin uses it, --gc-sections drops it
@ from the plugin's ELF. Tests: Tests/hook_sim/gate_sim.py (this file, assembled, in an ARM11 emulator against the swap at
@ every instruction boundary) and Tests/hook_sim/gate_mutants.py.

@ With the flag 0 (an Old 3DS's swap: SWAP_ENTER, the HOME menu, the return while the block is read back) a svc 0x32 on one of the
@ plugin's FAKE sessions (a KEvent standing for nwm::UDS, ndm:u or dlp: Sources/ctr/uds_hle.cpp) would reach the kernel, which answers
@ 0xD8E007F7 (not a session), and the game's SDK stops on an ErrDisp. So the gate answers it itself, from the table above: the reply
@ header (the command's id, one normal word) and the slot's result in the thread's command buffer, r0 = 0, no svc, no wait (a wait could
@ hold a lock the game's return from the HOME menu needs). A svc 0x23 on one of them is made for real (the kernel closes the event) and
@ the slot's bit is set in fake_closed; the gate never writes a slot (the plugin alone fills and frees them, with the flag 1), so the bit
@ only says "look at this one": once back the plugin asks the kernel (svcClearEvent) and does what azl_pre23 would have done for a
@ handle that is gone (Plugin::Tick). A bit for a handle still alive (the close refused, or the slot filled again) is dropped.
@ (2026-11-08, risk O-A of analysis/piano-2026-11-08.)

    .arm
    .syntax unified
    .section .text.azl_gate, "ax", %progbits
    .align 3

    .global azl_gate_begin, azl_gate_end, azl_gate_32, azl_gate_23
    .global azl_gate_flag, azl_gate_in_flight, azl_gate_pre32, azl_gate_post32, azl_gate_pre23, azl_gate_post23
    .global azl_gate_last_site, azl_gate_last_handle, azl_gate_post_skipped, azl_gate_fake_handles, azl_gate_fake_hits, azl_gate_fake_closed

azl_gate_begin:
azl_gate_flag:          .word 0     @ +0x00  1 = the plugin is there (written by the plugin only: install, swap)
azl_gate_in_flight:     .word 0     @ +0x04  threads between the count going up and coming down (flag must stay right above)
azl_gate_pre32:         .word 0     @ +0x08  u64 pre(u32 handle, u32 stub_ret, u32 *ctx): r0 = handle or answer, r1 = action
azl_gate_post32:        .word 0     @ +0x0C  u32 post(u32 result, u32 stub_ret, u32 *ctx): r0 = result
azl_gate_pre23:         .word 0     @ +0x10
azl_gate_post23:        .word 0     @ +0x14
azl_gate_last_site:     .word 0     @ +0x18  stub+8 of the last thread that went into the plugin (for the line of a drain that times out)
azl_gate_last_handle:   .word 0     @ +0x1C  ... and its handle
azl_gate_post_skipped:  .word 0     @ +0x20  posts not called because the flag was 0 after the svc
azl_gate_fake_handles:  .space 56   @ +0x24  7 x {handle, result}: the plugin's fake sessions (written by the plugin only, under UdsHle's lock)
azl_gate_fake_hits:     .word 0     @ +0x5C  svc 0x32 on a fake handle answered here with the flag 0
azl_gate_fake_closed:   .word 0     @ +0x60  bit i: slot i's handle closed with the flag 0 (the plugin forgets it once back)

    .macro DMB scratch
    mcr   p15, 0, \scratch, c7, c10, 5
    .endm

    @ The thread counts itself, then reads the flag. out: r1 = the flag. clobbers r2, r3, the condition flags.
    .macro COUNT_IN
    adr   r2, azl_gate_in_flight
1:  ldrex r1, [r2]
    add   r1, r1, #1
    strex r3, r1, [r2]
    cmp   r3, #0
    bne   1b
    DMB   r3                          @ (r3 = 0) the count is visible before the flag is read
    ldr   r1, [r2, #-4]               @ azl_gate_flag
    .endm

    @ The thread is no longer counted. clobbers r1-r3, the condition flags; r0 kept.
    .macro COUNT_OUT
    adr   r2, azl_gate_in_flight
    mov   r3, #0
    DMB   r3                          @ the plugin's work is visible before the count comes down
1:  ldrex r1, [r2]
    sub   r1, r1, #1
    strex r3, r1, [r2]
    cmp   r3, #0
    bne   1b
    .endm

    .macro GATE name, svcno, pre, post, fake
    .type \name, %function
\name:
    @ r0 = the handle, lr = stub+8, [sp] = the game's lr (pushed by the stub), everything else the game's.
    push  {r1-r3, r12, lr}
    mrs   r12, cpsr
    and   r12, r12, #0xFF000000       @ the condition flags (only cpsr_f goes back)
    and   r1, sp, #4
    eor   r1, r1, #4                  @ one pad word when the 20 bytes below would not end on eight
    orr   r12, r12, r1                @ (remembered in bit 2 of the saved flags)
    sub   sp, sp, r1
    push  {r12}
    sub   sp, sp, #16                 @ ctx[0..3]: [sp+16] = the saved flags, [sp+36+pad] = stub+8
    COUNT_IN
    cmp   r1, #0
    beq   .Labsent\@
    str   lr, [r2, #0x14]             @ last_site (r2 = &in_flight): only a thread that goes into the plugin, counted, writes it,
    str   r0, [r2, #0x18]             @ last_handle   so a close that times out names a thread that is in, not one that went by
    mov   r1, lr                      @ stub+8: which site
    mov   r2, sp                      @ ctx
    ldr   r3, \pre
    blx   r3                          @ the plugin: r0 = the handle (or the answer), r1 = the action
    str   r1, [sp, #12]               @ ctx[3] = the action
    COUNT_OUT
    ldr   r1, [sp, #12]
    cmp   r1, #0
    beq   .Lout\@                     @ answered by the plugin
.Lsvc\@:
    svc   \svcno                      @ r0 = the handle. A thread may sleep here for as long as it likes: this page and the
                                      @ instruction below never change, the plugin's block may go and come back meanwhile.
    ldr   r1, [sp, #12]
    cmp   r1, #2
    blo   .Lout\@                     @ no post asked
    COUNT_IN                          @ the post only if the plugin is (still, or again) there
    cmp   r1, #0
    beq   .Lskipped\@
    ldr   r1, [sp, #16]
    and   r1, r1, #4
    add   r1, sp, r1
    ldr   r1, [r1, #36]               @ stub+8
    mov   r2, sp
    ldr   r3, \post
    blx   r3                          @ r0 = the result
.Lcounted\@:
    COUNT_OUT
.Lout\@:
    add   sp, sp, #16
    pop   {r12}
    and   r1, r12, #4
    add   sp, sp, r1                  @ the pad word, if any
    msr   cpsr_f, r12
    pop   {r1-r3, r12}
    ldr   lr, [sp, #4]                @ the game's lr
    ldr   pc, [sp], #8                @ to the stub's `b site+4`, sp as the game left it
.Lskipped\@:
    adr   r2, azl_gate_post_skipped
2:  ldrex r1, [r2]
    add   r1, r1, #1
    strex r3, r1, [r2]
    cmp   r3, #0
    bne   2b
    b     .Lcounted\@
.Labsent\@:
    COUNT_OUT
    .if \fake == 1
    cmp   r0, #0                      @ (handle 0: never a fake one)
    beq   5f
    adr   r2, azl_gate_fake_handles
    mov   r3, #7
3:  ldr   r1, [r2], #8                @ slot.handle (r2 then points at the next slot)
    cmp   r1, r0
    beq   4f
    subs  r3, r3, #1
    bne   3b
    b     5f                          @ not one of ours: the svc, as before
4:  adr   r1, azl_gate_fake_closed
    ldr   r1, [r1]
    rsb   r12, r3, #7                 @ the slot's index (r12 is saved on the stack: free here)
    mov   r1, r1, lsr r12
    tst   r1, #1
    bne   5f                          @ closed while shut: the value may name something else now, the kernel answers
    mov   r1, #0
    DMB   r1                          @ the handle read before its result (the plugin writes the result, then the handle)
    ldr   r1, [r2, #-4]               @ slot.result
    mrc   p15, 0, r3, c13, c0, 3      @ the thread's TLS
    ldr   r0, [r3, #0x80]             @ the command header the game wrote
    mov   r0, r0, lsr #16
    mov   r0, r0, lsl #16
    orr   r0, r0, #0x40               @ the reply: its id, one normal word
    str   r0, [r3, #0x80]
    str   r1, [r3, #0x84]             @ cmdbuf[1] = the result the game's SDK handles (the service is not there)
    adr   r2, azl_gate_fake_hits
6:  ldrex r1, [r2]
    add   r1, r1, #1
    strex r3, r1, [r2]
    cmp   r3, #0
    bne   6b
    mov   r0, #0                      @ the svc itself succeeded: the game reads cmdbuf[1]
    b     .Lout\@
5:
    .endif
    .if \fake == 2
    cmp   r0, #0
    beq   5f
    adr   r2, azl_gate_fake_handles
    mov   r3, #0
3:  ldr   r1, [r2, r3, lsl #3]
    cmp   r1, r0
    beq   4f
    add   r3, r3, #1
    cmp   r3, #7
    blo   3b
    b     5f                          @ not one of ours: the close, as before
4:  mov   r12, #1
    mov   r12, r12, lsl r3            @ the slot's bit (r12 is saved on the stack: free here)
    adr   r2, azl_gate_fake_closed
8:  ldrex r1, [r2]
    orr   r1, r1, r12
    strex r3, r1, [r2]
    cmp   r3, #0
    bne   8b                          @ (the slot itself is the plugin's alone: it stays as it is)
5:
    .endif
    mov   r1, #1
    str   r1, [sp, #12]               @ ctx[3] = 1: the svc, no post
    b     .Lsvc\@
    .size \name, . - \name
    .endm

    GATE azl_gate_32, 0x32, azl_gate_pre32, azl_gate_post32, 1
    GATE azl_gate_23, 0x23, azl_gate_pre23, azl_gate_post23, 2

    .align 2
azl_gate_end:
