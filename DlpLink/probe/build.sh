#!/bin/bash
# Probe build, inside Docker: repo mounted read-only at /src (for tools_ctr), scratch probe at /work (a copy of this folder with DlpLink/tools and
# DlpLink/rsf next to it; the child needs the access descriptor of your own console, DlpLink/data/dlpchild_ad.bin).
set -e
MAKEROM=/src/tools_ctr/makerom
cd /work
rm -rf app/build out/*
make -C app 2>&1 | tail -15
ls -la app/probe.elf app/probe.smdh
ver=$(( $(cat child.ver 2>/dev/null || echo 2000) + 1 )); echo $ver > child.ver
$MAKEROM -f cia -o out/ChildProbe.cia -target t -exefslogo -desc dlpchild:1 -elf app/probe.elf -rsf rsf/child.rsf -icon app/probe.smdh -ver $ver 2>&1 | tail -5
cp out/ChildProbe.cia out/ChildProbe_plain.cia
python3 tools/finalize_plain.py out/ChildProbe_plain.cia
python3 tools/finalize_child.py out/ChildProbe.cia
ls -la out
