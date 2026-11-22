#!/usr/bin/env python3
# config_file_extract.py <plugin.cpp> <plugin.hpp> <out.inc> (F7-rev, 2026-11-21): the plugin's own code of AzaharLink.ini, as it is, for
# Tests/config_file_test.cpp - plugin.cpp cannot be built on the PC (libctrpf), so the test compiles these pieces of it, unchanged, in front of
# a stand-in of CTRPF's File. Beside <out.inc>, config_file_head.inc: the paths (kDataDir .. kConfigBakPath, kDefaultNickname) and struct
# Config of plugin.hpp (before the test's stand-in of the Plugin class); <out.inc>: the card of azl/whole_file.hpp (struct CtrWhole) and
# Plugin::ReadDataFile, Plugin::LoadConfig, Plugin::SaveConfig. A piece that is not found ends the build: the test never runs on an older text.
import os
import sys

NL = chr(10)


def text_of(path):
    return open(path, 'rb').read().decode().replace(chr(13) + NL, NL)


def block(text, start, end, what):
    a = text.find(start)
    if a < 0:
        sys.exit('config_file_extract.py: not found: ' + what)
    b = text.find(end, a)
    if b < 0:
        sys.exit('config_file_extract.py: no end for: ' + what)
    return text[a:b + len(end)]


cpp = text_of(sys.argv[1])
hpp = text_of(sys.argv[2])
head = [
    block(cpp, 'constexpr const char* kDataDir = ', 'constexpr const char* kConfigBakPath = "/3ds/3DSLink/AzaharLink.ini.bak";' + NL, 'the paths'),
    block(cpp, 'constexpr const char* kDefaultNickname = ', ';' + NL, 'kDefaultNickname'),
    block(hpp, 'struct Config {' + NL, NL + '};' + NL, 'struct Config'),
]
body = ['namespace {' + NL + block(cpp, 'struct CtrWhole {' + NL, NL + '};' + NL, 'struct CtrWhole') + '} // namespace' + NL]
for name in ('bool Plugin::ReadDataFile(', 'bool Plugin::LoadConfig() {', 'bool Plugin::SaveConfig() {'):
    body.append(block(cpp, name, NL + '}' + NL, name))
banner = '// cut out of Sources/ctr/plugin.cpp and Includes/ctr/plugin.hpp by Tests/config_file_extract.py: do not edit' + NL
open(os.path.join(os.path.dirname(os.path.abspath(sys.argv[3])), 'config_file_head.inc'), 'w', newline=NL).write(banner + NL.join(head))
open(sys.argv[3], 'w', newline=NL).write(banner + NL.join(body))
