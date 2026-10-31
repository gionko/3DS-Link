#!/usr/bin/env python3
# plugin_dlp_extract.py <plugin.cpp> <out.inc>: the Download Play wiring of plugin.cpp (from "struct DlpGame {" to the end of MakeDlpHub), as it is.
import sys

text = open(sys.argv[1], 'rb').read().decode().replace('\r\n', '\n')
start = text.index('struct DlpGame {')
marker = 'return std::unique_ptr<DlpHub>(new (std::nothrow) DlpHub(engine, kernel, config, resolver));\n}\n'
end = text.index(marker, start) + len(marker)
open(sys.argv[2], 'w').write(text[start:end])
