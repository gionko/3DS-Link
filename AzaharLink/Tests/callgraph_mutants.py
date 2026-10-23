#!/usr/bin/env python3
"""Mutation check of Tests/callgraph_check.py on the REAL build: every mutant is a copy of the tree with ONE printf-family
call added somewhere a game thread can reach but the text markers of Tests/policy_check.sh do not look (or do not look
closely), built with the shipped docker command and disassembled; the checker must FAIL on it, and pass on the unchanged
copy. About 25 seconds per build. Run by hand (it needs Docker and Python 3, not unicorn):

    python Tests/callgraph_mutants.py [name-fragment ...]      (from anywhere; with fragments, only the mutants whose
                                                                name contains one of them)
"""
import os
import shutil
import subprocess
import sys
import tempfile

root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
copy_dirs = ['Sources', 'Includes', 'third_party']
copy_files = ['Makefile', 'CTRPluginFramework.plgInfo', '3gx.ld']
image = 'pablomk7/libctrpf:0.8.0'

mutants = [
    ('unchanged', None, None, None),
    ('snprintf in a helper above the first marker of hook.cpp, called by PassThrough', 'Sources/ctr/hook.cpp',
     'Result PassThrough(Handle handle) {\n    u32* cmd = getThreadCommandBuffer();\n',
     '__attribute__((noinline)) static void Probe(u32 v) { char t[8]; __builtin_snprintf(t, sizeof(t), "%u", (unsigned)v); volatile char sink = t[0]; (void)sink; }\n'
     'Result PassThrough(Handle handle) {\n    u32* cmd = getThreadCommandBuffer();\n    Probe(cmd[0]);\n'),
    ('snprintf in AptSleepEventOf', 'Includes/azl/apt_sleep.hpp', '    if (signal == kAptSignalSleepEnter) {\n',
     '    char which[16];\n    __builtin_snprintf(which, sizeof(which), "%u", (unsigned)reply[2]);\n'
     '    volatile char sink = which[0];\n    (void)sink;\n    if (signal == kAptSignalSleepEnter) {\n'),
    ('snprintf in SleepWatch::Enter', 'Includes/azl/link_policy.hpp', '        enter_ms_ = now_ms;\n        ++apt_enters_;\n',
     '        enter_ms_ = now_ms;\n        char t[8];\n        __builtin_snprintf(t, sizeof(t), "%u", (unsigned)now_ms);\n'
     '        volatile char sink = t[0];\n        (void)sink;\n        ++apt_enters_;\n'),
    ('snprintf in NdmHle::CloseSession (called by ForgetFakeHandle, not marked)', 'Sources/azl/ndm_hle.cpp',
     '    SessionObj& s = sessions_[si];\n    // 0x106eb4, in the same order.\n',
     '    SessionObj& s = sessions_[si];\n    { char t[8]; __builtin_snprintf(t, sizeof(t), "%d", si); volatile char sink = t[0]; (void)sink; }\n    // 0x106eb4, in the same order.\n'),
    ('snprintf in platform::NowMs', 'Sources/azl/platform.cpp',
     '    return azl_ticks_to_ms(svcGetSystemTick());\n}\n\nu64 NowUs() {\n    return azl_ticks_to_us(svcGetSystemTick());',
     '    char tag[8];\n    __builtin_snprintf(tag, sizeof(tag), "%u", (unsigned)svcGetSystemTick());\n    volatile char sink = tag[0];\n'
     '    (void)sink;\n    return azl_ticks_to_ms(svcGetSystemTick());\n}\n\nu64 NowUs() {\n    return azl_ticks_to_us(svcGetSystemTick());'),
    ('Log with a format back in the engine role line (Shutdown -> RoleTrace)', 'Sources/azl/uds_engine.cpp',
     '    traced_role_ = role;\n    // A handful of lines per session', '    traced_role_ = role;\n    Log("uds: role %u", (unsigned)role);\n    // A handful of lines per session'),
    ('DebugEvent with a format in Teardown (Shutdown / StopService -> Teardown)', 'Sources/azl/uds_engine.cpp',
     'void UdsEngine::Teardown(NetworkStatusChangeReason reason, bool send_deauth) {\n',
     'void UdsEngine::Teardown(NetworkStatusChangeReason reason, bool send_deauth) {\n    DebugEvent("teardown %d", (int)send_deauth);\n'),
    ('DebugEvent with a format back in DebugWifi (the Deauthentication a closing session sends)', 'Sources/azl/room_client.cpp',
     '    DebugWifiLine(dir,', '    DebugEvent("wifi %s", dir);\n    DebugWifiLine(dir,'),
    ('snprintf in the engine transport (RoomClient::SendWifiPacket)', 'Sources/azl/room_client.cpp',
     'void RoomClient::SendWifiPacket(const WifiPacket& packet) {\n',
     'void RoomClient::SendWifiPacket(const WifiPacket& packet) {\n    { char t[8]; __builtin_snprintf(t, sizeof(t), "%d", (int)packet.channel); volatile char sink = t[0]; (void)sink; }\n'),
    # ---- 0.1.16: the observer of the sleep protocol (reached through a pointer), the ring it stores into, the role test of the
    #      sleep action and the throttle leg that follows every served request ----
    ('Log in Plugin::NoteSleepEvent (the APT observer, reached through a pointer)', 'Sources/ctr/plugin.cpp',
     '    self.crumbs_.Push(kind, value, static_cast<u32>(platform::NowMs()));\n',
     '    Log("crumb %u", (unsigned)kind);\n    self.crumbs_.Push(kind, value, static_cast<u32>(platform::NowMs()));\n'),
    ('snprintf in SleepCrumbs::Push (called by the observer, in a header)', 'Includes/azl/sleep_crumbs.hpp',
     '        const u32 i = head_.fetch_add(1, std::memory_order_relaxed);\n',
     '        { char t[8]; __builtin_snprintf(t, sizeof(t), "%u", (unsigned)kind); volatile char sink = t[0]; (void)sink; }\n'
     '        const u32 i = head_.fetch_add(1, std::memory_order_relaxed);\n'),
    ('snprintf in Plugin::SleepActs (called by OnSleepEnter)', 'Sources/ctr/plugin.cpp',
     '    const int mode = config_.sleep_stop;\n',
     '    const int mode = config_.sleep_stop;\n'
     '    { char t[8]; __builtin_snprintf(t, sizeof(t), "%d", mode); volatile char sink = t[0]; (void)sink; }\n'),
    ('Log in UdsHle::AccountCall (the leg after every served request)', 'Sources/ctr/uds_hle.cpp',
     '    const u64 ended = platform::NowTicks();\n    const u32 ticks = static_cast<u32>(ended) - started_ticks;\n',
     '    const u64 ended = platform::NowTicks();\n    Log("accounted %u", (unsigned)id);\n    const u32 ticks = static_cast<u32>(ended) - started_ticks;\n'),
    ('snprintf next to the 64-bit time sum of AccountCall (0.1.16, the CPU figure)', 'Sources/ctr/uds_hle.cpp',
     '        StatsAdd64(g_stats.ipc_sum_ticks, ticks);\n',
     '        { char t[8]; __builtin_snprintf(t, sizeof(t), "%u", (unsigned)ticks); volatile char sink = t[0]; (void)sink; }\n'
     '        StatsAdd64(g_stats.ipc_sum_ticks, ticks);\n'),
    ('snprintf in the APT observer call of PassThroughApt', 'Sources/ctr/hook.cpp',
     '        g_apt_observer(header == kAptReplySleepQuery ? kAptNoteReplyQuery : kAptNoteReplyComplete, reply);\n',
     '        { char t[8]; __builtin_snprintf(t, sizeof(t), "%u", (unsigned)reply); volatile char sink = t[0]; (void)sink; }\n'
     '        g_apt_observer(header == kAptReplySleepQuery ? kAptNoteReplyQuery : kAptNoteReplyComplete, reply);\n'),
    # ---- 0.1.16: the capture's producers (the ring's lanes, the lost markers, the diet, the raw command records) ----
    ('snprintf in the diet (DebugIpc -> DietLocked)', 'Sources/azl/log.cpp',
     '    if (++slot->calls <= kDietFullCalls) {\n        return false;\n    }\n',
     '    { char t[8]; __builtin_snprintf(t, sizeof(t), "%u", (unsigned)slot->calls); volatile char sink = t[0]; (void)sink; }\n'
     '    if (++slot->calls <= kDietFullCalls) {\n        return false;\n    }\n'),
    ('Log in the marker of a hole (every producer -> AdmitLocked -> EmitLostLocked)', 'Sources/azl/log.cpp',
     '    Lost& lost = g_dbg_lost[lane];\n    u32 head = g_dbg_head;\n    PutWord(head, 16u',
     '    Lost& lost = g_dbg_lost[lane];\n    Log("lost %u", (unsigned)lost.full);\n    u32 head = g_dbg_head;\n    PutWord(head, 16u'),
    ('snprintf in the raw record of a command (RunTraced -> DebugServiceCommand -> PushCommand)', 'Sources/azl/log.cpp',
     '    DebugRecord rec = MakeRecord(kDbgCommand, lane);\n',
     '    { char t[8]; __builtin_snprintf(t, sizeof(t), "%u", (unsigned)head.a); volatile char sink = t[0]; (void)sink; }\n'
     '    DebugRecord rec = MakeRecord(kDbgCommand, lane);\n'),
    ('snprintf in a kernel operation of the engine (CtrKernelOps::SignalEvent, reached through a vtable)',
     'Includes/ctr/kernel_ops.hpp', None, None),  # (placeholder: filled in below from the real text)
]


def find_kernel_op_mutant():
    path = os.path.join(root, 'Includes/ctr/kernel_ops.hpp')
    text = open(path, 'rb').read().decode('utf-8').replace('\r\n', '\n')
    needle = 'void SignalEvent(u32 handle) override'
    i = text.find(needle)
    if i < 0:
        return None
    j = text.index('{', i) + 1
    return text[i:j]


def build(tmp):
    env = dict(os.environ, MSYS_NO_PATHCONV='1')
    r = subprocess.run(['docker', 'run', '--rm', '-v', tmp + ':/src', '-w', '/src', image, 'make'], env=env,
                       capture_output=True, text=True)
    return r.returncode == 0 and os.path.exists(os.path.join(tmp, 'AzaharLink.elf')), (r.stdout + r.stderr)[-600:]


def disassemble(tmp):
    shutil.copy(os.path.join(root, 'Tests/callgraph_dis.sh'), os.path.join(tmp, 'callgraph_dis.sh'))
    env = dict(os.environ, MSYS_NO_PATHCONV='1')
    r = subprocess.run(['docker', 'run', '--rm', '-v', tmp + ':/src', '-w', '/src', image, 'bash', '/src/callgraph_dis.sh',
                        '/src/AzaharLink.elf', '/src/cg'], env=env, capture_output=True, text=True)
    return r.returncode == 0


wanted = sys.argv[1:]
failed = 0
ran = 0
for name, rel, old, new in mutants:
    if rel == 'Includes/ctr/kernel_ops.hpp' and old is None:
        head = find_kernel_op_mutant()
        if head is None:
            print('SKIP %s (no such function in plugin.hpp)' % name)
            continue
        old, new = head, head + '\n        { char t[8]; __builtin_snprintf(t, sizeof(t), "%u", (unsigned)handle); volatile char sink = t[0]; (void)sink; }'
    if wanted and name != 'unchanged' and not any(w in name for w in wanted):
        continue
    tmp = tempfile.mkdtemp(prefix='azl_cg_')
    try:
        for d in copy_dirs:
            shutil.copytree(os.path.join(root, d), os.path.join(tmp, d))
        for f in copy_files:
            shutil.copy(os.path.join(root, f), os.path.join(tmp, f))
        if rel:
            p = os.path.join(tmp, rel)
            raw = open(p, 'rb').read().decode('utf-8')
            crlf = '\r\n' in raw
            s = raw.replace('\r\n', '\n')
            assert s.count(old) == 1, (name, s.count(old))
            s = s.replace(old, new)
            open(p, 'wb').write((s.replace('\n', '\r\n') if crlf else s).encode('utf-8'))
        ok, tail = build(tmp)
        if not ok:
            print('BAD  %s -> the mutant did not build:\n%s' % (name, tail))
            failed += 1
            continue
        if not disassemble(tmp):
            print('BAD  %s -> objdump failed' % name)
            failed += 1
            continue
        r = subprocess.run([sys.executable, os.path.join(root, 'Tests/callgraph_check.py'), os.path.join(tmp, 'cg')],
                           capture_output=True, text=True)
        want_fail = name != 'unchanged'
        good = (r.returncode != 0) == want_fail
        first = next((l for l in r.stdout.splitlines() if 'FAILED' in l), (r.stdout.strip().splitlines() or [''])[-1])
        print(('OK   ' if good else 'BAD  ') + name + ' -> exit %d | %s' % (r.returncode, first[:140]))
        failed += 0 if good else 1
        ran += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
print('%d of %d mutants not caught (the unchanged copy included)' % (failed, ran))
sys.exit(1 if failed else 0)
