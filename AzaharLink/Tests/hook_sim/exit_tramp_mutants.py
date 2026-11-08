#!/usr/bin/env python3
"""Proof of the proof for Tests/hook_sim/exit_tramp_sim.py: one defect at a time in Sources/ctr/exit_tramp.s, and the emulator test must fail.

    python Tests/hook_sim/exit_tramp_mutants.py [project-folder]      (Git Bash on the host: Docker for the assembler, `unicorn` for the emulator)

The real source is never touched: each mutant is written to analysis/exit_tramp_mut/exit_tramp.s and assembled from there (build_gate.sh,
TRAMP_SRC). The unchanged trampoline goes through the same pipeline first and must pass. Every mutant keeps the size (a size change would be
seen by the size check alone).
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
SRC = ROOT + '/Sources/ctr/exit_tramp.s'
WORK = ROOT + '/analysis/exit_tramp_mut'
orig = open(SRC, 'rb').read().decode('utf-8').replace(chr(13) + chr(10), chr(10))

MUTANTS = [
    ('the game\'s sp not given back', '    mov   sp, r1\n3:', '    mov   r1, r1\n3:'),
    ('the guard on the game\'s stack', '    mov   sp, r3\n', '    mov   r3, r3\n'),
    ('the guard not called', '    blx   r2\n', '    mov   r2, r2\n'),
    ('the state not set to done', '    mov   r1, #2\n    str   r1, [r12]\n', '    mov   r1, #2\n    mov   r1, r1\n'),
    ('the state not taken (two guards)', '2:  mov   r1, #1\n', '2:  mov   r1, #0\n'),
    ('a waiting thread makes its own exit', '    svc   0x0A\n    b     5b\n', '    svc   0x0A\n    b     4b\n'),
    ('the wait is not 1 ms', '    mov   r1, #0\n    svc   0x0A\n', '    mov   r1, #1\n    svc   0x0A\n'),
    ('no check of a missing guard', '    cmpne r2, #0\n', '    cmpne r2, r2\n'),
    ('position dependent: the state read by absolute address', '    adr   r12, azl_exit_tramp_state\n1:',
     '    ldr   r12, .Labs_state\n1:'),
]


def win(path):
    try:
        return subprocess.run(['cygpath', '-w', path], capture_output=True, text=True).stdout.strip() or path
    except OSError:
        return path


def run(source_text):
    os.makedirs(WORK, exist_ok=True)
    with open(WORK + '/exit_tramp.s', 'w', encoding='utf-8', newline='\n') as f:
        f.write(source_text)
    cmd = ['docker', 'run', '--rm', '-v', win(ROOT) + ':/src', '-w', '/src', '-e', 'TRAMP_SRC=analysis/exit_tramp_mut/exit_tramp.s',
           'devkitpro/devkitarm', 'bash', 'Tests/hook_sim/build_gate.sh', 'analysis/exit_tramp_mut/sim']
    b = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, MSYS_NO_PATHCONV='1'))
    if b.returncode != 0:
        return 'BUILD FAILED: ' + (b.stdout + b.stderr)[-300:].replace('\n', ' ')
    r = subprocess.run([sys.executable, ROOT + '/Tests/hook_sim/exit_tramp_sim.py', WORK + '/sim'], capture_output=True, text=True)
    out = r.stdout.strip().splitlines()
    fails = [l.strip() for l in out if l.strip().startswith('FAIL:')]
    return (out[-1] if out else 'NO OUTPUT ' + r.stderr[-300:]) + ('  first: ' + fails[0][:150] if fails else '')


bad = 0
try:
    ctl = run(orig)
    print('%-58s %s' % ('(the trampoline as it is)', ctl[:120]))
    if ' 0 failures' not in ctl:
        print('the unchanged trampoline does not pass: the mutants say nothing')
        bad += 1
    for label, old, new in MUTANTS:
        if orig.count(old) != 1:
            print('%-58s SKIPPED (pattern found %d times)' % (label, orig.count(old)))
            bad += 1
            continue
        text = orig.replace(old, new)
        if 'Labs_state' in new:
            text = text.replace('.Lone_ms:             .word 1000000\n', '.Lone_ms:             .word 1000000\n.Labs_state: .word azl_exit_tramp_state\n')
            text = text.replace('    b     5b\n\n    .align 2\n', '    b     5b\n    .align 2\n')
        out = run(text)
        seen = 'BUILD FAILED' in out or ' 0 failures' not in out
        print('%-58s %s  [%s]' % (label, 'seen' if seen else 'NOT SEEN', out[:200]))
        if not seen:
            bad += 1
finally:
    shutil.rmtree(WORK, ignore_errors=True)
print('exit_tramp_mutants: %d mutants, not seen or skipped: %d' % (len(MUTANTS), bad))
sys.exit(1 if bad else 0)
