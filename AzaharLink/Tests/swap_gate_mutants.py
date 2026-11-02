#!/usr/bin/env python3
"""Proof of the proof for Tests/swap_gate_test.cpp: one defect at a time in Includes/azl/swap_gate.hpp, and the test must fail.

    python3 Tests/swap_gate_mutants.py          (inside the container that has g++, like Tests/run.sh; from the project folder)
"""
import os
import shutil
import subprocess
import sys
import tempfile

HEADER = 'Includes/azl/swap_gate.hpp'
ORIG = open(HEADER, encoding='utf-8').read()

MUTANTS = [
    ('enter: not counted', '    __atomic_fetch_add(&g.inside, who, __ATOMIC_SEQ_CST);\n    if (__atomic_load_n', '    if (__atomic_load_n'),
    ('enter: the closed gate is not looked at', 'if (__atomic_load_n(&g.closed, __ATOMIC_SEQ_CST) == 0) {', 'if (true) {'),
    ('enter: a stopped thread stays counted', '    __atomic_fetch_sub(&g.inside, who, __ATOMIC_SEQ_CST);\n    return false;', '    return false;'),
    ('enter: the flag off is ignored', '    if (g.on == 0) {\n        return;\n    }\n    while (!GateTryEnter', '    while (!GateTryEnter'),
    ('enter: no second try after the park', '    while (!GateTryEnter(g, who)) {', '    if (!GateTryEnter(g, who)) {'),
    ('enter: stops are not counted', '        __atomic_fetch_add(&g.stops, 1u, __ATOMIC_RELAXED);\n', ''),
    ('leave: not counted', '    if (g.on != 0) {\n        __atomic_fetch_sub(&g.inside, who, __ATOMIC_SEQ_CST);\n    }', '    (void)who;'),
    ('close: the flag off is ignored', '    if (g.on == 0) {\n        return 0;\n    }\n', ''),
    ('close: the gate does not close', '    __atomic_store_n(&g.closed, 1u, __ATOMIC_SEQ_CST);\n', ''),
    ('close: nobody is waited for', 'while (left != 0 && now_us() - start < timeout_us) {', 'while (false) {'),
    ('close: no limit to the wait', 'while (left != 0 && now_us() - start < timeout_us) {', 'while (left != 0 && g_clock_unused()) {'),
    ('close: the wait stops one step early', 'now_us() - start < timeout_us) {', 'now_us() - start + 1000 < timeout_us) {'),
    ('close: stale count', '        step();\n        left = __atomic_load_n(&g.inside, __ATOMIC_SEQ_CST);', '        step();'),
    ('close: stats of the start missing', '    g.stats.inside_start = left;\n', ''),
    ('close: stats of who is left missing', '    g.stats.inside_left = left;\n', ''),
    ('close: the wait is not saturated', 'waited > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(waited)', 'static_cast<u32>(waited)'),
    ('close: not counted', '    ++g.stats.closes;\n', ''),
    ('open: the gate stays closed', '__atomic_exchange_n(&g.closed, 0u, __ATOMIC_SEQ_CST) != 0', '__atomic_load_n(&g.closed, __ATOMIC_SEQ_CST) != 0'),
    ('open: every open counts', '    if (__atomic_exchange_n(&g.closed, 0u, __ATOMIC_SEQ_CST) != 0) {\n        ++g.stats.opens;\n    }', '    __atomic_store_n(&g.closed, 0u, __ATOMIC_SEQ_CST);\n    ++g.stats.opens;'),
    ('scope: no leave', '~SwapGateScope() { GateLeave(g_swap_gate, who_); }', '~SwapGateScope() {}'),
    ('scope: no enter', 'explicit SwapGateScope(u32 who) : who_(who) { GateEnter(g_swap_gate, who_); }', 'explicit SwapGateScope(u32 who) : who_(who) {}'),
    ('weights: two kinds share a bit', 'constexpr u32 kGateWriter = 1u << 8;', 'constexpr u32 kGateWriter = 1u << 0;'),
]


def main():
    out = tempfile.mkdtemp(prefix='swap_gate_mut')
    survivors = []
    try:
        inc = os.path.join(out, 'inc', 'azl')
        os.makedirs(inc)
        for name in ('common.hpp',):
            shutil.copy(os.path.join('Includes/azl', name), inc)
        for label, old, new in MUTANTS:
            if ORIG.count(old) != 1:
                print('BAD PATTERN', label, ORIG.count(old))
                survivors.append(label)
                continue
            text = ORIG.replace(old, new)
            if 'g_clock_unused' in text:
                text = text.replace('namespace azl {\n', 'namespace azl {\ninline bool g_clock_unused() { return true; }\n', 1)
            open(os.path.join(inc, 'swap_gate.hpp'), 'w', encoding='utf-8').write(text)
            exe = os.path.join(out, 't')
            built = subprocess.run(['g++', '-std=c++17', '-O1', '-w', '-I' + os.path.join(out, 'inc'), '-IIncludes', '-pthread', 'Tests/swap_gate_test.cpp', '-o', exe],
                                   capture_output=True, text=True)
            if built.returncode != 0:
                print('killed (does not build):', label)
                continue
            try:
                run = subprocess.run([exe], capture_output=True, text=True, timeout=30)
                died = run.returncode != 0
            except subprocess.TimeoutExpired:
                died = True
            print(('killed:  ' if died else 'SURVIVED:'), label)
            if not died:
                survivors.append(label)
    finally:
        shutil.rmtree(out, ignore_errors=True)
    print('%d mutants, %d survived' % (len(MUTANTS), len(survivors)))
    sys.exit(1 if survivors else 0)


if __name__ == '__main__':
    main()
