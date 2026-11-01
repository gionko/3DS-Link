#!/usr/bin/env python3
"""Proof of the proof for Tests/hook_sim/entry_sim.py: one defect at a time in Sources/ctr/svc_entry.s, and the emulator test must fail.

    python Tests/hook_sim/entry_mutants.py [project-folder]      (Git Bash on the host: it needs Docker for the assembler, `unicorn` for the emulator)

The logic behind the entry has its own: Tests/hook_swap_mutants.py (inside the container with g++).
"""
import os, subprocess, sys, shutil

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
SRC = ROOT + '/Sources/ctr/svc_entry.s'
raw_source = open(SRC, 'rb').read()   # (put back byte for byte at the end: the sources are CRLF)
orig = raw_source.decode('utf-8').replace(chr(13) + chr(10), chr(10))

MUTANTS = [
    ('no count in', 'add  r1, r1, #1\n    strex r3, r1, [r2]', 'add  r1, r1, #0\n    strex r3, r1, [r2]'),
    ('no count out (normal path)', '2:  ldrex r1, [r2]\n    sub  r1, r1, #1', '2:  ldrex r1, [r2]\n    sub  r1, r1, #0'),
    ('no count out (hand-back path)', '3:  ldrex r1, [r2]\n    sub  r1, r1, #1', '3:  ldrex r1, [r2]\n    sub  r1, r1, #0'),
    ('hand-back: flags not restored', '    bne  3b\n    msr  cpsr_f, r12\n', '    bne  3b\n'),
    ('hand-back: lr slot', 'str  r2, [sp, #20]', 'str  r2, [sp, #16]'),
    ('hand-back: site slot', 'str  r3, [sp, #24]', 'str  r3, [sp, #20]'),
    ('hand-back: game lr read from the wrong slot', 'ldr  r2, [sp, #24]                 @ the game\'s lr', 'ldr  r2, [sp, #20]                 @ the game\'s lr'),
    ('match: r0 not restored', 'pop  {r0, r1, r4, r12}', 'pop  {r2, r1, r4, r12}'),
    ('match: wrong application id', 'cmpeq r3, #0x300', 'cmpeq r3, #0x200'),
    ('match: no arg test', 'cmpeq r3, #0x300\n    bne  .Lcall\\@', 'cmp r3, r3\n    bne  .Lcall\\@'),
    ('match: tramp return not passed', 'ldr  r1, [r4, #20]                 @ the return address', 'ldr  r1, [r4, #16]                 @ the return address'),
    ('match: stack not aligned', 'bic  sp, sp, #7\n    push {r0, r1, r4, r12}', 'push {r0, r1, r4, r12}'),
    ('match: r12 (flags) not kept across C', 'push {r0, r1, r4, r12}             @ (r1', 'push {r0, r1, r4, r2}             @ (r1'),
    ('handback asked by CloseHandle', 'SVC_ENTRY azl_svc23_entry, azl_hook_CloseHandle, 0', 'SVC_ENTRY azl_svc23_entry, azl_hook_CloseHandle, 1'),
    ('hand-back jumps with sp not advanced', 'ldr  pc, [sp], #4', 'ldr  pc, [sp]'),
]

def run(label):
    cmd = ['docker', 'run', '--rm', '-v', ROOT_WIN + ':/src', '-w', '/src', 'devkitpro/devkitarm', 'bash', 'Tests/hook_sim/build_entry.sh', 'analysis/sim_mut']
    env = dict(os.environ, MSYS_NO_PATHCONV='1')
    b = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if b.returncode != 0:
        return 'BUILD FAILED: ' + (b.stdout + b.stderr)[-300:]
    r = subprocess.run([sys.executable, ROOT + '/Tests/hook_sim/entry_sim.py', ROOT + '/analysis/sim_mut'], capture_output=True, text=True)
    return r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ('NO OUTPUT ' + r.stderr[-300:])

try:
    ROOT_WIN = subprocess.run(['cygpath', '-w', ROOT], capture_output=True, text=True).stdout.strip() or ROOT
except OSError:
    ROOT_WIN = ROOT
bad = 0
try:
    for label, old, new in MUTANTS:
        if orig.count(old) != 1:
            print('%-45s SKIPPED (pattern found %d times)' % (label, orig.count(old)))
            bad += 1
            continue
        open(SRC, 'w', encoding='utf-8', newline='').write(orig.replace(old, new))
        out = run(label)
        seen = 'BUILD FAILED' in out or ' 0 failures' not in out
        print('%-45s %s  [%s]' % (label, 'seen' if seen else 'NOT SEEN', out[:80]))
        if not seen:
            bad += 1
finally:
    open(SRC, 'wb').write(raw_source)
    shutil.rmtree(ROOT + '/analysis/sim_mut', ignore_errors=True)
print('not seen or skipped: %d' % bad)
