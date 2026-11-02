#!/usr/bin/env python3
"""Proof of the proof for Tests/hook_swap_test.cpp: one defect at a time in Includes/azl/hook_swap.hpp, and the test must fail.

    python3 Tests/hook_swap_mutants.py          (inside the container that has g++, like Tests/run.sh; from the project folder)
    MUTANT_FILTER=ledger python3 ...             only the cases whose label has the word

The asm side has its own: Tests/hook_sim/entry_mutants.py (the assembler and the emulator are the host's).
"""
import os
import shutil
import subprocess
import sys
import tempfile

HEADER = 'Includes/azl/hook_swap.hpp'
ORIG = open(HEADER, encoding='utf-8').read()  # (text mode: the sources are CRLF, the patterns below are LF)

MUTANTS = [
    ('suspend: the state is not set', '    __atomic_store_n(state, kHooksSuspended, __ATOMIC_RELEASE);\n    u32 restored = 0;', '    u32 restored = 0;'),
    ('note: a shared wrapper is kept out', '    if (index == kNoSite || sites[index].original[1] == kBxLrWord) {', '    if (index == kNoSite) {'),
    ('note: nothing is noted', '        if (entry == 0 && __atomic_compare_exchange_n(&ledger.receive[i], &expected, index + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {', '        if (false) {'),
    ('note: the same site twice', '        if (entry == index + 1) {\n            return;\n        }\n        u32 expected = 0;', '        u32 expected = 0;'),
    ('resume: a noted site takes the hook at once', '            if (IsReceiveSite(ledger, i) && LedgerAdd(ledger, i)) {', '            if (false) {'),
    ('resume: a noted site never comes back', '            if (IsReceiveSite(ledger, i) && LedgerAdd(ledger, i)) {', '            if (IsReceiveSite(ledger, i)) {'),
    ('resume: the clock of the delay is not set', '    __atomic_store_n(&ledger.last_ms, now_ms, __ATOMIC_RELEASE);\n    m.Sync();\n    if (stats) {\n        ++stats->resumes;', '    m.Sync();\n    if (stats) {\n        ++stats->resumes;'),
    ('resume: the ledger is cleared after the sites', '    LedgerClear(ledger);\n    u32 again = 0;\n    u32 kept = 0;', '    u32 again = 0;\n    u32 kept = 0;'),
    ('resume: kept sites are not counted', '        stats->kept_out = kept;\n', ''),
    ('suspend: a site that is not ours is overwritten', '        if (SiteIsOurs(m, sites[i])) {\n            m.StoreOriginal(sites[i].address, sites[i].original);\n            ++restored;',
     '        if (true) {\n            m.StoreOriginal(sites[i].address, sites[i].original);\n            ++restored;'),
    ('suspend: the ledger is not cleared', '    m.Sync();\n    LedgerClear(ledger);\n    if (stats) {\n        ++stats->suspends;', '    m.Sync();\n    if (stats) {\n        ++stats->suspends;'),
    ('suspend: no cache sync', '    m.Sync();\n    LedgerClear(ledger);\n    if (stats) {\n        ++stats->suspends;', '    LedgerClear(ledger);\n    if (stats) {\n        ++stats->suspends;'),
    ('resume: the state is active before the sites are hooked', '    u32 again = 0;\n    u32 kept = 0;\n    for (u32 i = 0; i < count; ++i) {',
     '    __atomic_store_n(state, kHooksActive, __ATOMIC_RELEASE);\n    u32 again = 0;\n    u32 kept = 0;\n    for (u32 i = 0; i < count; ++i) {'),
    ('resume: a site that is not the game\'s is hooked', '        if (SiteIsOriginal(m, sites[i])) {\n            if (IsReceiveSite(ledger, i) && LedgerAdd(ledger, i)) {',
     '        if (true) {\n            if (IsReceiveSite(ledger, i) && LedgerAdd(ledger, i)) {'),
    ('resume: the state stays suspended', '    __atomic_store_n(state, kHooksActive, __ATOMIC_RELEASE); // (last', '    (void)state; // (last'),
    ('handback: done with the hooks out', '    if (__atomic_load_n(state, __ATOMIC_ACQUIRE) != kHooksActive) {\n        return 0;\n    }\n    const u32 index = FindSiteIndex', '    const u32 index = FindSiteIndex'),
    ('handback: a site somebody else changed is taken', '    if (!ours && !SiteIsOriginal(m, site)) {', '    if (false) {'),
    ('handback: the ledger is not asked', '    if (!LedgerAdd(ledger, index)) {', '    if (false) {'),
    ('handback: the site is restored before the ledger has it', '    if (!LedgerAdd(ledger, index)) { // (in the ledger BEFORE the site changes: a re-hook can never miss it)\n        return 0;\n    }\n    __atomic_store_n(&ledger.last_ms, now_ms, __ATOMIC_RELEASE);\n    if (ours) {\n        m.StoreOriginal(site.address, site.original);\n        m.Sync();\n    }',
     '    if (ours) {\n        m.StoreOriginal(site.address, site.original);\n        m.Sync();\n    }\n    if (!LedgerAdd(ledger, index)) {\n        return 0;\n    }\n    __atomic_store_n(&ledger.last_ms, now_ms, __ATOMIC_RELEASE);'),
    ('handback: the site is not restored', '    if (ours) {\n        m.StoreOriginal(site.address, site.original);\n        m.Sync();\n    }\n    return site.address;', '    return site.address;'),
    ('handback: the clock is not noted', '    __atomic_store_n(&ledger.last_ms, now_ms, __ATOMIC_RELEASE);\n    if (ours) {', '    if (ours) {'),
    ('find: the trampoline itself, not its bl return', 'sites[i].tramp + 8 == tramp_return', 'sites[i].tramp == tramp_return'),
    ('find: a site with no trampoline matches', 'if (sites[i].tramp != 0 && sites[i].tramp + 8 == tramp_return) {', 'if (sites[i].tramp + 8 == tramp_return) {'),
    ('ledger: full is not refused', '        if (__atomic_compare_exchange_n(&ledger.slot[i], &expected, index + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {\n            return true;\n        }\n    }\n    return false;',
     '        if (__atomic_compare_exchange_n(&ledger.slot[i], &expected, index + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {\n            return true;\n        }\n    }\n    return true;'),
    ('ledger: a site is added twice', '    if (LedgerHas(ledger, index)) {\n        return true;\n    }\n    for (u32 i = 0; i < kMaxHandedBack; ++i) {\n        u32 expected = 0;', '    for (u32 i = 0; i < kMaxHandedBack; ++i) {\n        u32 expected = 0;'),
    ('ledger: a slot taken by a plain store', '        u32 expected = 0;\n        if (__atomic_compare_exchange_n(&ledger.slot[i], &expected, index + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {\n            return true;\n        }',
     '        if (ledger.slot[i] == 0) {\n            ledger.slot[i] = index + 1;\n            return true;\n        }'),
    ('rearm: before the delay', 'static_cast<u32>(now_ms - __atomic_load_n(&ledger.last_ms, __ATOMIC_ACQUIRE)) < kRearmDelayMs', 'false'),
    ('rearm: the delay counts from the first hand-back', '    __atomic_store_n(&ledger.last_ms, now_ms, __ATOMIC_RELEASE);\n    if (ours) {', '    if (ledger.last_ms == 0) {\n        __atomic_store_n(&ledger.last_ms, now_ms, __ATOMIC_RELEASE);\n    }\n    if (ours) {'),
    ('rearm: with the hooks out', '    if (!LedgerAny(ledger) || __atomic_load_n(state, __ATOMIC_ACQUIRE) != kHooksActive) {\n        return 0;\n    }\n    if (static_cast<u32>(now_ms', '    if (!LedgerAny(ledger)) {\n        return 0;\n    }\n    if (static_cast<u32>(now_ms'),
    ('rearm: a site somebody else changed is hooked', '        if (entry != 0 && entry - 1 < count && SiteIsOriginal(m, sites[entry - 1])) {', '        if (entry != 0 && entry - 1 < count) {'),
    ('rearm: the ledger is kept', '    m.Sync();\n    LedgerClear(ledger);\n    if (stats) {\n        stats->rearmed += again;', '    m.Sync();\n    if (stats) {\n        stats->rearmed += again;'),
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
        open(work + '/azl/hook_swap.hpp', 'w', encoding='utf-8', newline='').write(ORIG.replace(old, new))
        exe = work + '/t'
        build = subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-I' + work, '-IIncludes', '-pthread', '-fsanitize=address,undefined',
                                'Tests/hook_swap_test.cpp', 'Sources/azl/svc_patch.cpp', 'Sources/azl/arm_reloc.cpp', '-o', exe],
                               capture_output=True, text=True)
        ran += 1
        if build.returncode != 0:
            print('%-62s BUILD FAILED (the mutant is not code): %s' % (label, build.stderr[-200:]))
            bad += 1
            continue
        run = subprocess.run([exe], capture_output=True, text=True)
        seen = run.returncode != 0
        print('%-62s %s' % (label, 'seen' if seen else 'NOT SEEN'))
        if not seen:
            bad += 1
finally:
    shutil.rmtree(work, ignore_errors=True)
print('hook_swap mutants: %d run, %d not seen or broken' % (ran, bad))
sys.exit(1 if bad else 0)
