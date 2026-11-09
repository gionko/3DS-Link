#!/usr/bin/env python3
# plugin_dlp_extract.py <plugin.cpp> <out.inc>: the Download Play wiring of plugin.cpp (from "struct DlpGame {" to the end of MakeRealDlpHub,
# which comes right after MakeDlpHub), as it is. 1.1.3: also, beside <out.inc>, plugin_dlp_wiring.inc: the block of Plugin::EarlyInit that decides
# by the table which hub a game gets (from the line of the Lobby games to the end of the RealDlp branch), as it is.
import os
import sys

text = open(sys.argv[1], 'rb').read().decode().replace('\r\n', '\n')
start = text.index('struct DlpGame {')
marker = 'return std::unique_ptr<DlpHub>(new (std::nothrow) DlpHub(engine, kernel, config, resolver, real));\n}\n'
end = text.index(marker, start) + len(marker)
open(sys.argv[2], 'w').write(text[start:end])
# the decision of EarlyInit: from the line of the Lobby games (pinned) to the first line that closes the chain at its indent
wiring_start = text.index('    if (DlpModeOf(game_id_) == DlpGameMode::Lobby && active_) {\n')
wiring_end = text.index('\n    }\n', wiring_start) + len('\n    }\n')
wiring = text[wiring_start:wiring_end]
if '    } else if (DlpModeOf(game_id_) == DlpGameMode::RealDlp && active_) {\n' not in wiring:
    sys.exit('plugin_dlp_extract.py: the RealDlp branch is not part of the block of the Lobby games in EarlyInit')
open(os.path.join(os.path.dirname(os.path.abspath(sys.argv[2])), 'plugin_dlp_wiring.inc'), 'w').write(wiring)
