# dlph — l'host Download Play di 3DS Link Delivery

Copia **senza modifiche** di `DlpLink/host/` (DlpLink 1.4.2, copiata il 2026-10-21; `dlph.c` ripreso lo stesso giorno dopo la correzione della parte DlpLink: offsets delle sezioni del CIA in 64 bit e controllati contro la dimensione del file, e rifiuto della variante emulatore del figlio, NCCH NoCrypto): `dlph.c`, `dlph.h`, `dlph_wire.c`, `dlph_wire.h`. La libreria fa da host
Download Play su nwm::UDS grezzo (libctru): una console con il solo Download Play di sistema (e Luma3DS) elenca «3DS Link», lo scarica da qui, lo installa e lo
avvia da sola. Le regole del protocollo, la storia e le prove su hardware sono nella parte DlpLink (`DlpLink/docs/3DSLINK-DELIVERY.md`,
`DlpLink/docs/DLP-HOST.md`, `DlpLink/test/`); qui c'è solo ciò che serve a costruirla e a usarla.

**Regola:** questi quattro file non si toccano da questa parte. Se serve una modifica si scrive alla parte DlpLink (che cosa e perché) e si riprende la
copia: `cp DlpLink/host/dlph*.[ch] ConfigApp/dlph/` e poi `cd ConfigApp/dlph && md5sum dlph.c dlph.h dlph_wire.c dlph_wire.h > MANIFEST.md5`.
`Tests/dlph_vendor_check.sh` (chiamato da `Tests/policy_check.sh`) controlla che i file siano quelli di `MANIFEST.md5`, che la cartella non contenga altro
(oltre a questo file) e, quando `../DlpLink/host` esiste, avverte se la libreria originale è andata avanti.

**Come si usa nell'app** (`ConfigApp/source/pages_dlp.cpp`): la pagina «3DS Link Delivery» tiene un thread radio (`dlph_open` → `dlph_start` → `dlph_poll`
a 100 Hz → `dlph_stop` → `dlph_close`, tutto dallo stesso thread) e legge dalla pagina `dlph_get_stats` e `dlph_history`; `dlph_request_kick` è il tasto «Send
away». La rete locale è `delivery::Wireless` (il chiamante possiede `udsInit`/`udsExit`), il log passa dal log del modulo di consegna (`dl_log_*`), sonno e
HOME dai suoi aiuti di energia (`dl_power_*`). Il pacchetto che si consegna è `<cartella dati>/dlp/child.cia` (`Paths::dlp_child`).

Impostazioni usate: stanza da 2 (`max_nodes = 2`, `persistent = false`: la stanza si rifà dopo ogni console), `gap_us = 3000` (misurato: ~10 s per 2,65 MB; 0 è
più lento), `lang = 0xFF` (lingua della console).
