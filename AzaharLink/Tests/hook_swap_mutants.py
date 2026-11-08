#!/usr/bin/env python3
"""Proof of the proof for Tests/hook_swap_test.cpp, Tests/gate_plan_test.cpp and Tests/kernel_books_test.cpp: one defect at a time in
Includes/azl/hook_swap.hpp (the plugin's side of the persistent gate: GateClose, GateOpen, GateClosing),
Includes/azl/gate_plan.hpp (the branches, the stubs, where the gate's page goes, WriteGate) or Includes/azl/books_math.hpp (the kernel's
books), and the test of that header must fail (a test that never ends counts as failing: the mutant is seen).

    python3 Tests/hook_swap_mutants.py          (inside the container that has g++, like Tests/run.sh; from the project folder)
    MUTANT_FILTER=close python3 ...              only the cases whose label has the word

The asm side has its own: Tests/hook_sim/gate_mutants.py (the assembler and the emulator are the host's).
"""
import os
import shutil
import subprocess
import sys
import tempfile

# which header a mutant changes, and the test (with its sources) that must see it
TARGETS = {
    'hook_swap': ('Includes/azl/hook_swap.hpp', ['Tests/hook_swap_test.cpp']),
    'gate_plan': ('Includes/azl/gate_plan.hpp', ['Tests/gate_plan_test.cpp']),
    'books': ('Includes/azl/books_math.hpp', ['Tests/kernel_books_test.cpp']),
}
ORIG = {k: open(v[0], encoding='utf-8').read() for k, v in TARGETS.items()}  # (text mode: the sources may be CRLF, the patterns below are LF)

MUTANTS = [
    # the persistent gate, the plugin's side (hook_swap.hpp)
    ('gate close: the flag after the count is read', '    Ops::StoreFlag(&g.flag, 0u);\n    Ops::Barrier(); // (Dekker: the flag is out before the count is read)\n    const u64 start = now_us();\n    u32 left = Ops::LoadCount(&g.in_flight);',
     '    const u64 start = now_us();\n    u32 left = Ops::LoadCount(&g.in_flight);\n    Ops::StoreFlag(&g.flag, 0u);\n    Ops::Barrier();'),
    ('gate close: no barrier between the flag and the count', '    Ops::Barrier(); // (Dekker: the flag is out before the count is read)\n', ''),
    ('gate close: the flag is not closed', '    Ops::StoreFlag(&g.flag, 0u);\n    Ops::Barrier(); // (Dekker', '    Ops::Barrier(); // (Dekker'),
    ('gate close: the wait does not wait', '    while (left != 0 && now_us() - start < timeout_us) {', '    while (false) {'),
    ('gate close: the wait never gives up', '    while (left != 0 && now_us() - start < timeout_us) {', '    while (left != 0) {'),
    ('gate close: who was left is not said', '        stats->in_flight_left = left;\n', ''),
    ('gate open: the flag stays closed', '    Ops::StoreFlag(&g.flag, 1u);', '    Ops::StoreFlag(&g.flag, 0u);'),
    ('gate closing: the other way round', '    return __atomic_load_n(&g.flag, __ATOMIC_ACQUIRE) == 0;', '    return __atomic_load_n(&g.flag, __ATOMIC_ACQUIRE) != 0;'),
    # the plan of the gate's page (gate_plan.hpp)
    ('choose: the page above 0x01E80000', '    u32 top = kGateCeiling; // the highest end the page may have', '    u32 top = 0x04000000u;', 'gate_plan'),
    ('choose: the page in the framework\'s', 'constexpr u32 kGateCeiling = 0x01E80000;', 'constexpr u32 kGateCeiling = 0x01E82000;', 'gate_plan'),
    ('choose: the reach of the sites ignored', '        if (!GateInReach(min_site, max_site, base, size)) {', '        if (false) {', 'gate_plan'),
    ('choose: a mapped span taken', '        if (span.state != kGateMemFree) {', '        if (false) {', 'gate_plan'),
    ('choose: a free span too short taken', '        if (static_cast<u64>(span.base) + span.size < top) {', '        if (false) {', 'gate_plan'),
    ('choose: a query that lies is believed', '        if (!query(base, &span) || span.base > base || static_cast<u64>(span.base) + span.size <= base) {', '        if (!query(base, &span)) {', 'gate_plan'),
    ('branch: one word too far forward', 'offset > 0x1FFFFFC', 'offset > 0x2000000', 'gate_plan'),
    ('branch: one word too far back', 'offset < -0x2000000', 'offset < -0x2000004', 'gate_plan'),
    ('stub: back to the site, not after it', 'EncodeBranch(stub + 8, site + 4, &words[2])', 'EncodeBranch(stub + 8, site, &words[2])', 'gate_plan'),
    ('page count: no room for the gate', '(gate_bytes + sites * kGateStubBytes + kGatePageBytes - 1)', '(sites * kGateStubBytes + kGatePageBytes - 1)', 'gate_plan'),
    ('site index: past the last stub', ' || offset / kGateStubBytes >= count', '', 'gate_plan'),
    ('write: the flag is never set', '    m.StorePage(page + kGateFlag, 1u);\n', '', 'gate_plan'),
    ('write: the header keeps the blob\'s words', 'i < kGateCode / 4 ? 0u : blob.words[i]', 'i == kGateFlag / 4 ? 0u : blob.words[i]', 'gate_plan'),
    ('write: a site somebody changed is hooked', ' || m.Load(site.address) != site.original', '', 'gate_plan'),
    ('write: two words per site', '        if (!m.PatchText(sites[i].address, jump)) {', '        m.PatchText(sites[i].address + 4, jump);\n        if (!m.PatchText(sites[i].address, jump)) {', 'gate_plan'),
    ('write: no sync after the sites', '    m.Sync();\n    r.hooked = kept;', '    r.hooked = kept;', 'gate_plan'),
    # the kernel's books (books_math.hpp)
    ('books: lift leaves the limit', '    limits[kCommit] += bytes;', '    limits[kCommit] += 0;', 'books'),
    ('books: the reservation freed the charged way', '    const s32 freed = kernel.FreeUncharged(address, bytes);', '    const s32 freed = kernel.FreeCharged(address, bytes);', 'books'),
    ('books: the raise keeps the lifted limit when refused', '    if (rc != 0) {\n        kernel.Lower(bytes);\n    }\n', '', 'books'),
    ('books: the charge lifts after the reservation', '    kernel.Lift(lift);\n    const s32 rc = ReserveThroughKernel(kernel, bytes);\n    kernel.Lower(lift);',
     '    const s32 rc = ReserveThroughKernel(kernel, bytes);\n    kernel.Lift(lift);\n    kernel.Lower(lift);', 'books'),
    ('books: the charge keeps the lifted limit', '    const s32 rc = ReserveThroughKernel(kernel, bytes);\n    kernel.Lower(lift);\n    return rc;', '    const s32 rc = ReserveThroughKernel(kernel, bytes);\n    return rc;', 'books'),
    # the lift of a Charge covers what current stands above the limit (2026-11-06, AVV-1): the reservation meets the pages, never the limit alone
    ('books: the charge lifts the block alone', '    const u32 lift = ChargeLift(limit, current, bytes);', '    const u32 lift = bytes;', 'books'),
    ('books: the lift ignores current above the limit', '    return over > 0 ? bytes + static_cast<u32>(over) : bytes;', '    return bytes;', 'books'),
    ('books: the lift shrinks under the limit', '    return over > 0 ? bytes + static_cast<u32>(over) : bytes;', '    return static_cast<u32>(static_cast<s64>(bytes) + over);', 'books'),
    ('books: the charge comes down by the block alone', '    kernel.Lower(lift);', '    kernel.Lower(bytes);', 'books'),
    ('books: the fallback on a refusal above the limit', '    const s32 rc = ChargeThroughKernel(kernel, bytes);\n    if (rc != kNoPages) {',
     '    const s32 rc = ChargeThroughKernel(kernel, bytes);\n    if (rc != kNoPages && rc != kAboveLimit) {', 'books'),
    ('books: the uncharge freed with svc 0xA3', '        const s32 freed = kernel.FreeCharged(address, piece);', '        const s32 freed = kernel.FreeUncharged(address, piece);', 'books'),
    ('books: the uncharge never a page at a time', '            if (piece > kPageBytes) {\n                piece = kPageBytes;\n                continue;\n            }\n', '', 'books'),
    ('books: the uncharge counts a failed piece', '            kernel.FreeUncharged(address, piece); // (the pages back; nothing came off current)\n            return freed;',
     '            kernel.FreeUncharged(address, piece); // (the pages back; nothing came off current)\n            *released += piece;\n            return freed;', 'books'),
    ('books: a failed svc 0xA3 free left as it is', '        kernel.FreeCharged(address, bytes);\n        return freed;', '        return freed;', 'books'),
    ('books: settle the wrong way', '    limits[kCommit] -= bytes;', '    limits[kCommit] += bytes;', 'books'),
    ('books: the check skips the limits', '        ok = ok && call_limits[i] == read_limits[i];\n', '', 'books'),
    # the fallback for Monster Hunter XX on an Old 3DS (2026-11-06): current[] written only after the kernel's try, and only when it had no pages
    ('books: the fallback always', '    const s32 rc = ChargeThroughKernel(kernel, bytes);\n    if (rc != kNoPages) {',
     '    const s32 rc = ChargeThroughKernel(kernel, bytes);\n    if (rc != kNoPages && rc != 0) {', 'books'),
    ('books: the fallback never', '    const s32 rc = ChargeThroughKernel(kernel, bytes);\n    if (rc != kNoPages) {',
     '    const s32 rc = ChargeThroughKernel(kernel, bytes);\n    if (true) {', 'books'),
    ('books: the fallback before the kernel', '    *way = Way::Kernel;\n    const s32 rc = ChargeThroughKernel(kernel, bytes);\n    if (rc != kNoPages) {\n        return rc;\n    }\n    kernel.WriteCurrent(static_cast<s32>(bytes));\n',
     '    *way = Way::Kernel;\n    kernel.WriteCurrent(static_cast<s32>(bytes));\n    const s32 rc = ChargeThroughKernel(kernel, bytes);\n    if (rc != kNoPages) {\n        kernel.WriteCurrent(-static_cast<s32>(bytes));\n        return rc;\n    }\n', 'books'),
    ('books: the fallback for any refusal', '    const s32 rc = ChargeThroughKernel(kernel, bytes);\n    if (rc != kNoPages) {',
     '    const s32 rc = ChargeThroughKernel(kernel, bytes);\n    if (rc == 0) {', 'books'),
    ('books: the uncharge without its fallback', '    const s32 rc = ReleaseThroughKernel(kernel, bytes, released);\n    if (rc != kNoPages) {',
     '    const s32 rc = ReleaseThroughKernel(kernel, bytes, released);\n    if (true) {', 'books'),
    ('books: the uncharge fallback for any answer', '    const s32 rc = ReleaseThroughKernel(kernel, bytes, released);\n    if (rc != kNoPages) {',
     '    const s32 rc = ReleaseThroughKernel(kernel, bytes, released);\n    if (rc == 0) {', 'books'),
    ('books: the uncharge writes off what was released', '    kernel.WriteCurrent(-static_cast<s32>(bytes - *released));', '    kernel.WriteCurrent(-static_cast<s32>(bytes));', 'books'),
    ('books: the release never leaves SYSTEM', '            if (rc == kNoPages && region + 1 < kReleaseRegionCount) {\n                ++region;\n                continue;\n            }\n', '', 'books'),
    ('books: the release regions in another order', '{Region::System, Region::Application, Region::Base};', '{Region::Base, Region::Application, Region::System};', 'books'),
    ('books: the write takes off', '    current[kCommit] += bytes;', '    current[kCommit] -= bytes;', 'books'),
]

only = os.environ.get('MUTANT_FILTER')
work = tempfile.mkdtemp()
os.makedirs(work + '/azl')
bad = 0
ran = 0
try:
    for entry in MUTANTS:
        label, old, new = entry[:3]
        target = entry[3] if len(entry) > 3 else 'hook_swap'
        header, sources = TARGETS[target]
        if only and only not in label:
            continue
        orig = ORIG[target]
        if orig.count(old) != 1:
            print('%-62s PATTERN FOUND %d TIMES (the header changed: update the mutant)' % (label, orig.count(old)))
            bad += 1
            continue
        shutil.rmtree(work + '/azl', ignore_errors=True)
        os.makedirs(work + '/azl')
        open(work + '/' + header[len('Includes/'):], 'w', encoding='utf-8', newline='').write(orig.replace(old, new))
        exe = work + '/t'
        build = subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-I' + work, '-IIncludes', '-pthread', '-fsanitize=address,undefined']
                               + sources + ['-o', exe], capture_output=True, text=True)
        ran += 1
        if build.returncode != 0:
            print('%-62s BUILD FAILED (the mutant is not code): %s' % (label, build.stderr[-200:]))
            bad += 1
            continue
        try:
            run = subprocess.run([exe], capture_output=True, text=True, timeout=120)
            seen = run.returncode != 0
        except subprocess.TimeoutExpired:
            seen = True  # (a close that waits for ever: the test never ends)
        print('%-62s %s' % (label, 'seen' if seen else 'NOT SEEN'))
        if not seen:
            bad += 1
finally:
    shutil.rmtree(work, ignore_errors=True)
print('hook_swap mutants (hook_swap, gate_plan, books): %d run, %d not seen or broken' % (ran, bad))
sys.exit(1 if bad else 0)
