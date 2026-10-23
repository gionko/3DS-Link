#!/usr/bin/env python3
"""check_version_in_binary.py X.Y.Z FILE... -- every FILE (the app's ELF, the plugin's .3gx) must contain the version as a string of its own.

3DS Link has ONE version (AzaharLink/VERSION). The headers and the TMD are written from it, but a stale binary (a make that failed, --no-app after a bump)
would still be packed into a CIA whose TMD says the new number. kVersion is compiled into both binaries as a NUL-delimited string, so a binary built
from another version does not have this one. Exit 1 when one of the files does not.
"""
import sys

if len(sys.argv) < 3:
    sys.exit(__doc__)
version = sys.argv[1].encode()
bad = 0
for path in sys.argv[2:]:
    data = open(path, "rb").read()
    if b"\0" + version + b"\0" in data:
        print("%s: carries %s" % (path, sys.argv[1]))
    else:
        print("ERRORE: %s does not carry the version %s (a stale build?)" % (path, sys.argv[1]))
        bad = 1
sys.exit(bad)
