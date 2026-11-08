#!/usr/bin/env python3
"""Proof of the proof for Tests/traced_sessions_test.cpp: one defect at a time in Includes/azl/traced_sessions.hpp, and the test must fail.

    python3 Tests/traced_sessions_mutants.py     (inside the container that has g++, like Tests/run.sh; from the project folder)
    MUTANT_FILTER=evict python3 ...              only the cases whose label has the word
"""
import os
import shutil
import subprocess
import sys
import tempfile

HEADER = 'Includes/azl/traced_sessions.hpp'
ORIG = open(HEADER, encoding='utf-8').read()
NL = chr(10)

MUTANTS = [
    ('opened: the index rule is gone', '    void Opened(u32 session) {' + NL + '        if (session == 0 || session == kTracedClaiming) {',
     '    void Opened(u32 session) {' + NL + '        if (true) {'),
    ('opened: the very same value is kept (counter round)', 'if (old != 0 && old != kTracedClaiming && SameHandleSlot(old, session) &&',
     'if (old != 0 && old != kTracedClaiming && old != session && SameHandleSlot(old, session) &&'),
    ('opened: any index frees an entry', 'if (old != 0 && old != kTracedClaiming && SameHandleSlot(old, session) &&', 'if (old != 0 && old != kTracedClaiming &&'),
    ('index: the mask takes a bit of the counter', 'constexpr u32 kHandleIndexMask = 0x7FFF;', 'constexpr u32 kHandleIndexMask = 0xFFFF;'),
    ('index: the mask is too short', 'constexpr u32 kHandleIndexMask = 0x7FFF;', 'constexpr u32 kHandleIndexMask = 0x3F;'),
    ('claim: the slot gets the handle before it is filled', 'if (__atomic_compare_exchange_n(&s.handle, &expected, kTracedClaiming, false,',
     'if (__atomic_compare_exchange_n(&s.handle, &expected, session, false,'),
    ('evict: no eviction', '        if (!apt) {' + NL + '            __atomic_fetch_add(&stats.dropped', '        if (true) {' + NL + '            __atomic_fetch_add(&stats.dropped'),
    ('evict: a session of another service goes', 'if (h == 0 || h == kTracedClaiming || !__atomic_load_n(&s.apt, __ATOMIC_ACQUIRE)) {', 'if (h == 0 || h == kTracedClaiming) {'),
    ('evict: a slot being filled can be chosen', 'if (h == 0 || h == kTracedClaiming || !__atomic_load_n(&s.apt, __ATOMIC_ACQUIRE)) {', 'if (h == 0 || !__atomic_load_n(&s.apt, __ATOMIC_ACQUIRE)) {'),
    ('evict: the most recently used goes', 'if (!oldest || static_cast<s32>(used - oldest_used) < 0) {', 'if (!oldest || static_cast<s32>(used - oldest_used) > 0) {'),
    ('evict: the handle is read again after the choice', '        AZL_TRACED_BEFORE_EVICT();' + NL,
     '        AZL_TRACED_BEFORE_EVICT();' + NL + '        if (oldest) {' + NL + '            oldest_handle = __atomic_load_n(&oldest->handle, __ATOMIC_ACQUIRE);' + NL + '        }' + NL),
    ('evict: not counted', '            __atomic_fetch_add(&stats.evicted, 1u, __ATOMIC_RELAXED);' + NL, ''),
    ('touch: a use is not noted', '        __atomic_store_n(&s.used, Tick(), __ATOMIC_RELEASE);' + NL, ''),
    ('closed: the entry stays', '            __atomic_compare_exchange_n(&s->handle, &expected, 0u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);' + NL, ''),
    ('fill: the APT flag is lost', '__atomic_store_n(&s.apt, apt, __ATOMIC_RELAXED);', '__atomic_store_n(&s.apt, false, __ATOMIC_RELAXED);'),
    ('fill: the handle is never stored', '        __atomic_store_n(&s.handle, session, __ATOMIC_RELEASE);' + NL, ''),
    ('add: a non-APT newcomer is not counted as dropped', '            __atomic_fetch_add(&stats.dropped, 1u, __ATOMIC_RELAXED);' + NL + '            return false;' + NL + '        }' + NL + '        // Full',
     '            return false;' + NL + '        }' + NL + '        // Full'),
    ('find: the claiming value matches a slot being filled', '        if (handle == 0 || handle == kTracedClaiming) {' + NL + '            return nullptr;' + NL + '        }' + NL + '        for (TracedSession& s : slots) {',
     '        if (handle == 0) {' + NL + '            return nullptr;' + NL + '        }' + NL + '        for (TracedSession& s : slots) {'),
]

only = os.environ.get('MUTANT_FILTER')
work = tempfile.mkdtemp()
os.makedirs(work + '/azl')
bad = 0
ran = 0
try:
    for label, old, new in MUTANTS:
        if only and only not in label:
            continue
        if ORIG.count(old) != 1:
            print('%-62s PATTERN FOUND %d TIMES (the header changed: update the mutant)' % (label, ORIG.count(old)))
            bad += 1
            continue
        open(work + '/azl/traced_sessions.hpp', 'w', encoding='utf-8', newline='').write(ORIG.replace(old, new))
        exe = work + '/t'
        build = subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-I' + work, '-IIncludes', '-pthread', '-fsanitize=address,undefined',
                                'Tests/traced_sessions_test.cpp', '-o', exe], capture_output=True, text=True)
        ran += 1
        if build.returncode != 0:
            print('%-62s BUILD FAILED (the mutant is not code): %s' % (label, build.stderr[-200:]))
            bad += 1
            continue
        try:
            run = subprocess.run([exe], capture_output=True, text=True, timeout=120)
            seen = run.returncode != 0
        except subprocess.TimeoutExpired:
            seen = True  # (a test that never ends is a failing test)
        print('%-62s %s' % (label, 'seen' if seen else 'NOT SEEN'))
        if not seen:
            bad += 1
finally:
    shutil.rmtree(work, ignore_errors=True)
print('traced_sessions mutants: %d run, %d not seen or broken' % (ran, bad))
sys.exit(1 if bad else 0)
