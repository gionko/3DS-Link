# dlpc — il compositore del figlio Download Play

3DS Link è **un CIA solo**. Il figlio che le console con il solo Download Play scaricano (`child.cia`: l'installatore, con dentro il CIA dell'app e il plugin) non si
distribuisce: lo costruisce l'app sulla console, la prima volta che la pagina *3DS Link Delivery* ne ha bisogno. Questa cartella è il pezzo che lo costruisce.
Il progetto e le decisioni sono in `DlpLink/docs/COMPOSITORE.md`; il contratto è `dlpc.h`.

* `dlpc.h`, `dlpc.c` — C99 puro, **senza libctru**: stdio per il file di uscita, SHA-256 e AES-128-CTR da `../titleshare/common/sha256.c` e `aes.c` (gli stessi che usa
  Titles Delivery: non si toccano). Lo stesso codice gira sulla console (devkitARM `-O2`, 24 KB di codice e testi) e nei test sul PC (gcc con ASan/UBSan, anche a 32 bit).
  Il `Makefile` dell'app compila **tutta** la cartella (`SOURCES += ../dlpc`): qui non ci devono essere altri `.c`.
* Il **modello** (`child_model_bin`, risorsa dell'app: `ConfigApp/data/child_model.bin`) è il figlio finito senza il RomFS: lo fa `DlpLink/tools/make_model.py` da un figlio cifrato a chiave zero
  o, per i test e per Azahar, da uno senza cifratura (`--emu`, NoCrypto). **La costruzione lo fa senza l'app** (`DlpLink/tools/make_child_model.sh`: un figlio «seme» dello stesso installatore con un RomFS di tre
  file di comodo): il RomFS è l'unica cosa che porta l'app, quindi il modello non ne dipende, e l'app che porta il modello e il figlio che porta l'app non fanno un ciclo (`DlpLink/docs/COMPOSITORE.md`).
  I test usano anche i modelli che `make_model.py` taglia dai figli di riferimento di makerom (`3dslink-child*.cia`, con l'app vera dentro): così il figlio composto è quello di makerom byte per byte. Ha un'intestazione propria, una somma SHA-256 di se stesso e i campi che dipendono dal RomFS a zero.
  `make_model.py --check` e `model_parse()` di `dlpc.c` applicano **le stesse regole** (la prova lo verifica su 3472 modelli con la somma rifatta e un byte cambiato).

## Come si usa
```c
dlpc_job j = {0};
j.model = child_model_bin; j.model_size = child_model_bin_size;
j.app_tid = 0x000400000F83D100ull; j.version16 = 1024; j.version_text = "1.0.0";     // gli stessi numeri dell'app (settings.hpp kVersion)
j.read_app = my_read; j.app_user = ...; j.app_size = ...;                            // il CIA dell'app: file sulla SD o CIA virtuale di Titles Delivery
j.read_plugin = my_read; j.plugin_user = ...; j.plugin_size = ...;                   // AzaharLink.3gx della cartella dati
j.out_path = "sdmc:/3ds/3DSLink/dlp/child.cia";                                      // la cartella deve esserci
j.progress = my_progress;                                                            // facoltativo; ritorna != 0 per annullare (tasto B)
char err[256]; dlpc_result r = dlpc_compose(&j, err, sizeof(err));                   // da un thread di lavoro: i callback girano su quel thread
```
Prima di cominciare: `dlpc_output_size(&j)` (quanto pesa il file: spazio sulla SD), `dlpc_memory_needed(&j)` (la memoria: **circa 2,29 MB** per 3DS Link 1.0.0 con il CIA da 0,99 MB e il plugin da 1,01 MB (**circa 2,9 MB per la 1.0.1**: il CIA dell'app porta il plugin, 1,55 MB, e il figlio è di 2.847.680 byte), il file intero in un
blocco solo; 0 = il modello non è un modello) e `dlpc_stage_work(&j, stage)` (i byte di lavoro per tipo, per la stima del tempo: SHA-256, AES, lettura e scrittura SD).
`dlpc_model_version()` dice la versione del modello (0 = non è un modello): la pagina lo usa per dire «questa build non ha l'installatore».

## Che cosa fa (sei stadi, in quest'ordine)
| stadio | lavoro |
|---|---|
| `READ_APP` | legge il CIA dell'app direttamente al suo posto nel RomFS, lo hash per il manifesto, controlla il CIA: titolo e versione del TMD e **catena di hash dei contenuti** |
| `READ_PLUGIN` | idem per il plugin: magic `3GX$` e versione (byte 9..11 dell'intestazione, come la riga «Plugin file» dell'Installation check) |
| `TREE` | `manifest.txt` (formato di `make_dlp_child.sh`), tabelle di directory e hash dei nomi, livelli IVFC 3→2→1 e hash master |
| `SEAL` | intestazione NCCH (dimensioni, hash dell'intestazione del RomFS), AES-128-CTR a chiave zero del RomFS (contatore = partitionId big-endian · 3 · sette zeri), catena TMD sui byte cifrati, intestazione CIA |
| `WRITE` | `<out>.part` sulla SD, a pezzi da 64 KB |
| `VERIFY` | rilegge il file **dentro lo stesso blocco** e lo controlla come se arrivasse da altri: CIA/TMD, testa uguale al modello (tranne i byte composti), NCCH, condizioni di `dlph_open` (≤ 32 MiB, TID 00040001, SMDH, non NoCrypto) e dell'installatore, hash di exheader/ExeFS, RomFS decifrato, IVFC livello per livello (e il riempimento a zero), tabelle, manifesto, hash dei tre file. Poi rinomina |

Il risultato è **byte per byte** il figlio di makerom + `finalize_child.py` quando il carico è lo stesso (le due varianti: la prova lo confronta con `DlpLink/out/3dslink-child*.cia`).

## Garanzie
* Ogni errore dà una frase inglese breve in `err` e **non lascia niente**: né `<out>.part` né `out_path` (un file già presente a `out_path` viene tolto all'inizio del lavoro).
  Annullare (callback != 0) vale come un errore: `DLPC_E_CANCEL`, niente file, nessun altro callback dopo.
* Un byte cambiato: nel modello → `DLPC_E_MODEL`; nel CIA dell'app (contenuti, TMD) → `DLPC_E_APP_BAD`; nell'intestazione/versione del plugin → `DLPC_E_PLUGIN_BAD`;
  sulla strada della SD o tornando indietro → `DLPC_E_VERIFY`. **Quello che nessuno può vedere**: un byte cambiato nel *corpo* del plugin, o nei certificati/ticket/meta del CIA dell'app
  (la catena TMD non li copre): il figlio che ne esce è coerente in sé (il manifesto dice l'hash di quello che ha letto) ma non è quello originale.
* Ogni dimensione è limitata (32 MiB per ogni ingresso, 32 MiB per il figlio, il limite di `dlph_open`) **prima** di ogni somma; i conti sono a 64 bit finché non si sa che il risultato è piccolo:
  `size_t` a 32 bit (la console) e i campi a 32 bit dei formati non possono andare in overflow. `app_size` fino a 2^64-1 dà `DLPC_E_TOOBIG`, senza leggere niente.
* Il compositore non crea cartelle, non ritenta, non usa thread propri.

## Tempo (un'operazione una tantum)
Sul PC l'intero lavoro è 63 ms (SHA-256 213 MB/s, AES 310 MB/s: `DLPC_MODE=plain DLPC_BENCH=1 bash DlpLink/tests/dlpc_check.sh`). Lavoro totale per 3DS Link 1.0: **SHA-256 11,3 MB, AES 3,4 MB, lettura SD
3,6 MB, scrittura SD 1,9 MB**. Su una console vecchia (SHA 1–3 MB/s, AES 0,5–1 MB/s, SD 6/3 MB/s): da 8 a 19 secondi, più la lettura del CIA dell'app se viene dalla fonte (b) di Titles Delivery
(CIA virtuale: decifra a software lo strato SD, anche questo 0,5–1 MB/s per 0,7 MB).

## La prova
`DlpLink/tests/dlpc_check.sh` (passo 6 di `run_checks.sh`, e il passo 7, `ship_check.sh`, con `dlpc_compose.c`: il modello che l'app porta davvero compone un figlio valido e se ne ricava indietro byte per byte, con due CIA diversi dell'app; il passo 6 da solo: da solo, con `DLPC_ARCH=-m32` nell'immagine `azl-hle-build`, `DLPC_MODE=plain DLPC_BENCH=1` per la tabella delle velocità) costruisce
`dlpc_test.c` e lo esegue su file veri: identità con makerom, `verify_cia.py` (55 e 53 controlli), `ctrtool`, `dlph_open`, il codice di lettura dell'installatore, un annullamento a **ogni** callback,
errori, mutazioni (modello, app, plugin), la SD che mente (~330 posti in tutto il file), carichi sintetici, il limite di 32 MiB al byte e le regole del modello contro `make_model.py`.
