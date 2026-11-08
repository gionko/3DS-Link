@ The exit guard's trampoline (New 3DS only; REGISTRO-MODIFICHE-ERRORI.md, phase Q, 2026-11-08).
@
@ Rosalina frees the plugin's memory block only in its EXIT branch, and the kernel (Luma3DS k11, ExitProcess.c) raises that EXIT only when
@ the game's svcExitProcess finds Luma's plugin word (config page 0x1FF800F0) at state 1 RUNNING; Rosalina then takes it only when the
@ word's event bits are exactly EXIT (plgloader.c: a whole-word store of the state, `==` on the events). A word left at 2 (INHOME), 0 (NONE)
@ or 1 with a stale HOME bit at the exit means no EXIT: the block stays, and the next launch gets "Couldn't set memblock size" (0xD8A077F9).
@ Every ARM `svc 0x03` word of the game's text is turned (once, before the game runs) into `b azl_exit_tramp` (a copy of this section in the
@ gate's page, within reach of the game's text: ctr/hook.cpp, InstallExitGuard). svcExitProcess never returns, so nothing after the site is
@ ever needed again, and every site shares this one copy.
@
@ The first thread to come here runs the guard (azl_exit_guard, in the plugin's block, mapped for good on a New 3DS) on the plugin's own stack
@ (the exiting thread can be any thread of the game, with any stack left), then makes the svc. A second thread that comes after the first
@ never makes a svc 0x03 of its own: it sleeps 1 ms at a time until the first svc ends the process (a second exit while the plugin answers
@ Rosalina's ABOUT_TO_EXIT would end the process under the plugin's own exit callback).
@ state: 0 nobody yet, 1 the guard runs, 2 done. fn / stack: written by the plugin into the copy (0: the svc at once).
@ Position independent: adr and pc-relative loads only.

    .arm
    .syntax unified
    .section .text.azl_exit_tramp, "ax", %progbits
    .align 3
    .global azl_exit_tramp_begin, azl_exit_tramp_end, azl_exit_tramp_state, azl_exit_tramp_fn, azl_exit_tramp_stack

azl_exit_tramp_begin:
    adr   r12, azl_exit_tramp_state
1:  ldrex r1, [r12]
    cmp   r1, #0
    beq   2f
    clrex
    b     5f                          @ the guard runs on another thread, or is done
2:  mov   r1, #1
    strex r2, r1, [r12]
    cmp   r2, #0
    bne   1b
    mov   r1, #0
    mcr   p15, 0, r1, c7, c10, 5      @ DMB: the state taken before anything the guard does
    ldr   r3, azl_exit_tramp_stack    @ the top of the guard's own stack (8-byte aligned)
    ldr   r2, azl_exit_tramp_fn
    cmp   r3, #0
    cmpne r2, #0
    beq   3f                          @ (not set: no guard, the svc)
    mov   r1, sp
    mov   sp, r3
    push  {r1, lr}                    @ the game's sp (8 bytes: the stack stays 8-byte aligned)
    blx   r2
    pop   {r1, lr}
    mov   sp, r1
3:  adr   r12, azl_exit_tramp_state
    mov   r1, #0
    mcr   p15, 0, r1, c7, c10, 5      @ DMB: the guard's writes before "done"
    mov   r1, #2
    str   r1, [r12]
4:  svc   0x03                        @ svcExitProcess: never returns
    b     4b
5:  ldr   r0, .Lone_ms                @ svcSleepThread(1 ms): r0:r1 = 1000000, until the first thread's svc ends the process
    mov   r1, #0
    svc   0x0A
    b     5b

    .align 2
.Lone_ms:             .word 1000000
azl_exit_tramp_state: .word 0
azl_exit_tramp_fn:    .word 0
azl_exit_tramp_stack: .word 0
azl_exit_tramp_end:
