#!/usr/bin/env python3
"""check_plugin_pair.py SETTINGS.hpp PLUGIN.3gx [VERSION] -- l'app e il plugin che il figlio porta devono essere la COPPIA GIUSTA.

3DS Link controlla la versione del plugin sulla SD (parola a offset 8 dell'intestazione .3gx: byte 9 = revisione, 10 = minor, 11 = major) contro la sua
`kVersion` (AzaharLink/ConfigApp/source/settings.hpp): se differiscono la riga "Plugin file" dell'Installation check e' ROSSA e nessun gioco puo' essere
abilitato. Il figlio scrive il plugin accanto all'installazione dell'app: se la coppia non e' giusta si accorge solo l'utente, sulla console di un altro.
Esce con 1 se la versione del plugin e' diversa da kVersion, o se il file non e' un .3gx.
"""
import re
import sys

settings, plugin = sys.argv[1], sys.argv[2]
m = re.search(r'kVersion\s*=\s*"(\d+)\.(\d+)\.(\d+)"', open(settings, encoding="utf-8").read())
if not m:
    sys.exit("ERRORE: kVersion non trovata in " + settings)
want = tuple(int(x) for x in m.groups())
head = open(plugin, "rb").read(16)
if head[:4] != b"3GX$":
    sys.exit("ERRORE: %s non e' un .3gx (magic %r)" % (plugin, head[:4]))
have = (head[11], head[10], head[9])
if have != want:
    sys.exit("ERRORE: il plugin e' %d.%d.%d ma l'app dice kVersion %d.%d.%d: il figlio porterebbe una coppia sbagliata (riga rossa all'avvio)" % (have + want))
if len(sys.argv) > 3 and sys.argv[3] != "%d.%d.%d" % want:
    sys.exit("ERRORE: app e plugin dicono %d.%d.%d ma la versione di 3DS Link (AzaharLink/VERSION) e' %s: Tests/set_version.py riscrive tutti i punti, poi si ricostruisce" % (want + (sys.argv[3],)))
print("coppia giusta: app e plugin sono %d.%d.%d (= VERSION)" % want)
