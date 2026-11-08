#!/usr/bin/env python3
"""Proof of the proof for Tests/hook_sim/gate_sim.py: one defect at a time in Sources/ctr/gate.s, and the emulator test must fail.

    python Tests/hook_sim/gate_mutants.py [project-folder]      (Git Bash on the host: Docker for the assembler, `unicorn` for the emulator)

The real source is never touched: each mutant is written to analysis/gate_mut/gate.s and assembled from there (build_gate.sh,
GATE_SRC). The unchanged gate goes through the same pipeline first and must pass.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
SRC = ROOT + '/Sources/ctr/gate.s'
WORK = ROOT + '/analysis/gate_mut'
orig = open(SRC, 'rb').read().decode('utf-8').replace(chr(13) + chr(10), chr(10))

COUNT_IN = ('    adr   r2, azl_gate_in_flight\n1:  ldrex r1, [r2]\n    add   r1, r1, #1\n    strex r3, r1, [r2]\n    cmp   r3, #0\n'
            '    bne   1b\n    DMB   r3                          @ (r3 = 0) the count is visible before the flag is read\n'
            '    ldr   r1, [r2, #-4]               @ azl_gate_flag\n')
PRE_CALL = ('    mov   r1, lr                      @ stub+8: which site\n')
POST_CALL_END = '    blx   r3                          @ r0 = the result\n.Lcounted\\@:\n    COUNT_OUT\n'
EXIT = ('    add   sp, sp, #16\n    pop   {r12}\n    and   r1, r12, #4\n    add   sp, sp, r1                  @ the pad word, if any\n'
        '    msr   cpsr_f, r12\n')

MUTANTS = [
    ('M1: flag read before the count', [(COUNT_IN, COUNT_IN.replace(
        '    adr   r2, azl_gate_in_flight\n1:', '    adr   r2, azl_gate_in_flight\n    ldr   r1, [r2, #-4]\n    push  {r1}\n1:').replace(
        '    ldr   r1, [r2, #-4]               @ azl_gate_flag\n', '    pop   {r1}\n'))]),
    ('M2: count down before the call to pre', [
        (PRE_CALL, '    COUNT_OUT\n' + PRE_CALL),
        ('    str   r1, [sp, #12]               @ ctx[3] = the action\n    COUNT_OUT\n', '    str   r1, [sp, #12]               @ ctx[3] = the action\n')]),
    ('DMB out of the count going up', [('    DMB   r3                          @ (r3 = 0) the count is visible before the flag is read\n', '')]),
    ('DMB out of the count coming down', [('    DMB   r3                          @ the plugin\'s work is visible before the count comes down\n', '')]),
    ('no count down on the way "absent"', [('.Labsent\\@:\n    COUNT_OUT\n', '.Labsent\\@:\n')]),
    ('the plugin\'s post run after the count came down', [
        ('    beq   .Lskipped\\@\n    ldr   r1, [sp, #16]\n', '    beq   .Lskipped\\@\n    COUNT_OUT\n    ldr   r1, [sp, #16]\n'),
        (POST_CALL_END, '    blx   r3                          @ r0 = the result\n    b     .Lout\\@\n.Lcounted\\@:\n    COUNT_OUT\n')]),
    ('msr of the flags before the saved flags are back', [(EXIT, EXIT.replace('    pop   {r12}\n', '    msr   cpsr_f, r12\n    pop   {r12}\n', 1).replace(
        '                  @ the pad word, if any\n    msr   cpsr_f, r12\n', '                  @ the pad word, if any\n'))]),
    ('post called without reading the flag again', [
        ('    COUNT_IN                          @ the post only if the plugin is (still, or again) there\n    cmp   r1, #0\n    beq   .Lskipped\\@\n',
         '    COUNT_IN                          @ the post only if the plugin is (still, or again) there\n')]),
    ('ctx[3] (the action) not saved', [('    str   r1, [sp, #12]               @ ctx[3] = the action\n', '')]),
    ('pre called whatever the flag', [('    COUNT_IN\n    cmp   r1, #0\n    beq   .Labsent\\@\n', '    COUNT_IN\n')]),
    ('the pad word not remembered', [('    orr   r12, r12, r1                @ (remembered in bit 2 of the saved flags)\n', '')]),
    ('post told the wrong site', [('    ldr   r1, [r1, #36]               @ stub+8\n', '    ldr   r1, [r1, #32]               @ stub+8\n')]),
    ('no svc on the way "absent"', [('    str   r1, [sp, #12]               @ ctx[3] = 1: the svc, no post\n    b     .Lsvc\\@\n',
                                     '    str   r1, [sp, #12]               @ ctx[3] = 1: the svc, no post\n    b     .Lout\\@\n')]),
    ('post_skipped not counted', [('2:  ldrex r1, [r2]\n    add   r1, r1, #1\n', '2:  ldrex r1, [r2]\n    add   r1, r1, #0\n')]),
    # (step 6: last_site / last_handle are written only by a thread that goes into the plugin; as up to step 5, by every thread, a close that
    #  times out could name a thread that went straight to the kernel after the close)
    ('last_site written by every thread, before the count', [
        ('    sub   sp, sp, #16                 @ ctx[0..3]: [sp+16] = the saved flags, [sp+36+pad] = stub+8\n    COUNT_IN\n',
         '    sub   sp, sp, #16                 @ ctx[0..3]: [sp+16] = the saved flags, [sp+36+pad] = stub+8\n    adr   r2, azl_gate_last_site\n'
         '    str   lr, [r2]\n    str   r0, [r2, #4]\n    COUNT_IN\n'),
        ('    str   lr, [r2, #0x14]             @ last_site (r2 = &in_flight): only a thread that goes into the plugin, counted, writes it,\n'
         '    str   r0, [r2, #0x18]             @ last_handle   so a close that times out names a thread that is in, not one that went by\n', '')]),
    # (2026-11-08, risk O-A: the gate answers the plugin's fake sessions itself with the flag 0)
    ('F1: handle 0 taken for a free slot', [('    .if \\fake == 1\n    cmp   r0, #0                      @ (handle 0: never a fake one)\n    beq   5f\n',
                                             '    .if \\fake == 1\n    cmp   r0, #0\n    mov   r0, r0\n')]),
    ('F2: the last slot not looked at', [('    mov   r3, #7\n3:  ldr   r1, [r2], #8', '    mov   r3, #6\n3:  ldr   r1, [r2], #8')]),
    ('F3: the handle given as the result', [('    ldr   r1, [r2, #-4]               @ slot.result\n', '    ldr   r1, [r2, #-8]               @ slot.result\n')]),
    ('F4: the reply header with no normal word', [('    orr   r0, r0, #0x40               @ the reply: its id, one normal word\n',
                                                   '    orr   r0, r0, #0                  @ the reply: its id, one normal word\n')]),
    ('F5: fake_hits not counted', [('6:  ldrex r1, [r2]\n    add   r1, r1, #1\n', '6:  ldrex r1, [r2]\n    add   r1, r1, #0\n')]),
    ('F6: a fake session answered and sent to the kernel too', [
        ('    mov   r0, #0                      @ the svc itself succeeded: the game reads cmdbuf[1]\n    b     .Lout\\@\n',
         '    mov   r0, #0                      @ the svc itself succeeded: the game reads cmdbuf[1]\n    b     5f\n')]),
    ('F7: the closed slot written by the gate', [('    adr   r2, azl_gate_fake_closed\n8:', '    str   r12, [r2, r3, lsl #3]\n8:')]),
    ('F8: fake_closed not marked', [('    orr   r1, r1, r12\n', '    orr   r1, r1, #0\n')]),
    ('F9: the close of a fake session not made', [('    bne   8b                          @ (the slot itself',
                                                   '    b     .Lout\\@                          @ (the slot itself')]),
    ('F10: the wrong bit in fake_closed', [('    mov   r12, r12, lsl r3            @ the slot', '    mov   r12, r12, lsl #0            @ the slot')]),
    ('F11: the result in the wrong word of the cmdbuf', [('    str   r1, [r3, #0x84]', '    str   r1, [r3, #0x88]')]),
    ('F12: handle 0 closed as a free slot', [('    .if \\fake == 2\n    cmp   r0, #0\n    beq   5f\n', '    .if \\fake == 2\n    cmp   r0, #0\n    mov   r0, r0\n')]),
    ('F13: the close looks at 6 slots', [('    cmp   r3, #7\n    blo   3b\n', '    cmp   r3, #6\n    blo   3b\n')]),
    ('F14: past the table taken for a slot', [('    b     5f                          @ not one of ours: the close, as before\n', '    b     4f                          @ not one of ours: the close, as before\n')]),
    ('F15: the reply keeps the request\'s word counts', [('    mov   r0, r0, lsr #16\n', '    mov   r0, r0, lsr #0\n')]),
    ('F16: the wrong TLS register', [('    mrc   p15, 0, r3, c13, c0, 3      @ the thread\'s TLS\n', '    mrc   p15, 0, r3, c13, c0, 2      @ the thread\'s TLS\n')]),
    ('F17: a slot closed while shut answered anyway', [('    bne   5f                          @ closed while shut', '    nop                               @ closed while shut')]),
]


def win(path):
    try:
        return subprocess.run(['cygpath', '-w', path], capture_output=True, text=True).stdout.strip() or path
    except OSError:
        return path


def run(source_text):
    os.makedirs(WORK, exist_ok=True)
    with open(WORK + '/gate.s', 'w', encoding='utf-8', newline='\n') as f:
        f.write(source_text)
    cmd = ['docker', 'run', '--rm', '-v', win(ROOT) + ':/src', '-w', '/src', '-e', 'GATE_SRC=analysis/gate_mut/gate.s',
           'devkitpro/devkitarm', 'bash', 'Tests/hook_sim/build_gate.sh', 'analysis/gate_mut/sim']
    b = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, MSYS_NO_PATHCONV='1'))
    if b.returncode != 0:
        return 'BUILD FAILED: ' + (b.stdout + b.stderr)[-300:].replace('\n', ' ')
    r = subprocess.run([sys.executable, ROOT + '/Tests/hook_sim/gate_sim.py', WORK + '/sim', '--fail-fast'], capture_output=True, text=True)
    out = r.stdout.strip().splitlines()
    fails = [l.strip() for l in out if l.strip().startswith('FAIL:')]
    return (out[-1] if out else 'NO OUTPUT ' + r.stderr[-300:]) + ('  first: ' + fails[0][:150] if fails else '')


bad = 0
try:
    ctl = run(orig)
    print('%-50s %s' % ('(the gate as it is)', ctl[:120]))
    if ' 0 failures' not in ctl:
        print('the unchanged gate does not pass: the mutants say nothing')
        bad += 1
    only = os.environ.get('GATE_MUTANT_FILTER', '')
    for label, pairs in MUTANTS:
        if only and only not in label:
            continue
        text = orig
        skipped = False
        for old, new in pairs:
            if text.count(old) < 1 or old == new:
                print('%-50s SKIPPED (pattern found %d times)' % (label, text.count(old)))
                skipped = True
                break
            text = text.replace(old, new)
        if skipped:
            bad += 1
            continue
        out = run(text)
        seen = 'BUILD FAILED' in out or ' 0 failures' not in out
        print('%-50s %s  [%s]' % (label, 'seen' if seen else 'NOT SEEN', out[:200]))
        if not seen:
            bad += 1
finally:
    shutil.rmtree(WORK, ignore_errors=True)
print('gate_mutants: %d mutants, not seen or skipped: %d' % (len(MUTANTS), bad))
sys.exit(1 if bad else 0)
