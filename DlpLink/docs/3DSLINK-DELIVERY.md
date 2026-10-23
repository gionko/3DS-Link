# 3DS Link Delivery — consegna alla sessione di 3DS Link (2026-10-20 notte)

Per chi costruisce la UI e l'integrazione nell'app (`AzaharLink/ConfigApp`). Qui c'e' tutto quello che la parte "Download Play" mette a disposizione e cosa serve in cambio. Regole del protocollo e storia: `DLP-HOST.md`. Prova su console della libreria: `../test/PROVA-LIBRERIA.md`.

## 1. Cosa e' stato deciso (utente, 2026-10-20 notte)
* 3DS Link 1.0 ha una **voce di menu nuova, "3DS Link Delivery"**. Non e' contemporanea a Send Titles: quando la apri, la console diventa un host Download Play e basta.
* **Stanza da 2** (questa console + un posto), **una console alla volta**. Quando la console ha scaricato ed e' uscita (va a installare), la stanza si ricrea e ne puo' entrare un'altra. Ricreare la stanza qui non disturba nessuno (non c'e' Send Titles acceso).
* **Stanza da 2 provata su hardware (2026-10-20): 11 console in fila, 0 perse, ricreazione 1,1 s, nome sempre letto.** Utente: modo A = versione finale della 1.0.
* **Banda**: la radio e' il limite (~2,9 ms a frammento). `gap_us = 0` e' stato misurato PEGGIO (17 s e 2,4x i byte sull'etere); **default `gap_us = 3000`** (~10 s, 1% ripetuti). Nessun carosello con cui dividere la radio, ma la pausa serve comunque.
* Chi riceve: un'altra console **con Luma3DS** e il solo Download Play di sistema. Scarica un "figlio" che si installa da solo, mostra una schermata in stile 3DS Link e rimanda alla HOME (pacchetto regalo). Un pacchetto solo: `3DSLink.cia` contiene tutto (app, plugin, capacita' di fare da host).
* Il server deve mostrare le **informazioni base**: quale console e' collegata (col nome), sta scaricando, a che punto, e avere il pulsante **"chiudi stanza"**.
* Prova su console della libreria: una sessione sola (`../test/PROVA-LIBRERIA.md`); finche' non e' fatta, la UI si costruisce sulla demo e sul test su PC.

## 2. Menu principale (pages_main.cpp)
Schermo basso, in ordine: **[3DS Link on/off, Azahar Emulator Room, Enable Titles for Azahar Emulator Net]** · spazio · **Titles Delivery** · **3DS Link Delivery** (nuova) · spazio · **Settings**. **Fra Titles Delivery e 3DS Link Delivery NON c'è nessuno spazio** (decisione dell'utente, 2026-10-21: è un servizio solo, in due parti; all'inizio aveva chiesto lo spazio e ha cambiato idea): `dlp.gap` non si imposta e `Tests/policy_check.sh` e un mutante lo tengono fermo.
Dove (`AzaharLink/ConfigApp/source/pages_main.cpp`; i numeri di riga non si danno: cambiano a ogni modifica, si cerca per nome):
* `BuildRows`: inserire la riga fra Titles Delivery e Settings; copiare il blocco di Titles Delivery: `id = kDlpDelivery`, `kind = Kind::Link`, `label = "3DS Link Delivery"` (**senza** `gap`: niente spazio sopra, vedi sopra), `note_title`/`note`. **Non** copiare `dim = g.luma_too_old`: come Titles Delivery e Settings, questa voce non dipende dal Plugin Loader.
* `enum Id`: aggiungere l'id. `Activate`: copiare il caso `kGameDelivery`: `if (step == 0) PushPage(pages::NewDlpHostPage());`.
* Fabbrica della pagina: dichiararla in `delivery.hpp` (`namespace pages`, accanto a `NewDeliveryPage`; `pages_main.cpp` lo include gia') e definirla come `NewDeliveryPage` (in `pages_delivery.cpp`).
* Le note sono in inglese e stanno in `GateNote` (`dlp_text.hpp`, provata da `dlp_text_test`): la scheda della nota nella pagina principale e' alta 92 px, cioe' **quattro righe** (una riga vuota fra due paragrafi ne e' una), e nell'emulatore il carattere di ripiego e' piu' largo di quello della console (una riga tiene 69-70 caratteri): perche' la frase «Press Y to see...» si veda, il primo paragrafo sta in due righe, poi la riga vuota, poi una riga sola. `dlp_text_test` conta le righe con un margine (60 caratteri per riga).
* Sei righe piu' tre spazi stanno nell'area della lista (`kListTop` - `kListBottom` di `ui.hpp`) e scorre comunque.

## 3. Schermata server "3DS Link Delivery"
Non serve la grafica da parte mia: serve che ci sia l'informazione. Modello consigliato: `SendRunPage` (`delivery_run.cpp:105-364`): schede (`DrawCard`), barra di avanzamento (`DrawProgressBar`), elenco in basso, footer (`Footer`/`FooterKey`), popup di conferma (`LayoutPopup`, `DrawPopupBox`, `DrawButton`).

**Dati a disposizione** (tutti in `dlph_stats` e `dlph_history`, sezione 4):

| Cosa mostrare | Campo | Note |
|---|---|---|
| Stato in una riga (inglese) | `dlph_state_text(stats)` | "Room open: waiting for a console", "A console is joining", "Sending 3DS Link", "Delivered: the console is leaving to install", "Room closed". In errore o a stanza chiusa usare `stats.state == DLPH_ROOM_DOWN`. |
| Cosa si offre | `child_name`, `child_version`, `child_bytes` | es. "3DS Link v1.0 - 1.9 MiB". Versione = quella del TMD del figlio (vedi 7). |
| Console collegata | `client_name`, `seat` | `client_name` e' "" per ~0,1 s dopo l'entrata (misurato: 26-70 ms) (evento `CLIENT_NAME` quando c'e'): in quel caso scrivere "Console" + posto. Il nome viene da `udsGetNodeInformation`, **non ancora visto sull'hardware**. |
| Avanzamento | `percent` | 0..100 del download. Il client ha poi qualche secondo di installazione che l'host NON vede: scrivere "Sent", mai "installed". |
| Velocita' | `mibps` | MiB/s sull'etere nell'ultimo secondo (ritrasmissioni incluse). |
| Tempo che manca | `eta_s` | -1 finche' non e' noto (primi ~1,5 s). |
| Pacchetti ripetuti | `frags_repeated` di `frags_total` (e `frags_refused`: rifiutati dalla radio, gia' contati fra gli inviati) | come "repeated packets" della schermata di Send Titles. |
| La console ha gia' questa versione | `already_has` (e `dlph_visit.already_had`) | **Non mostrare "consegnato"**: la console non ha ricevuto nulla (il figlio e' gia' installato con la stessa versione). `dlph_state_text` dice "This console already has this version"; `percent` resta 0. |
| Silenzio | `silent_ms` | tempo dall'ultimo pacchetto della console; oltre 3 s l'host rimanda il blocco, oltre 9 s ricrea la stanza. |
| Da quanto e' dentro | `client_ms` | |
| Contatori | `served`, `lost`, `remade` | servite, perse (uscite o mute prima della fine, o mandate via), stanze ricreate. |
| Ultime console | `dlph_history(out, 8)` | piu' recente prima: `name`, `result` (`DLPH_V_SERVED/LOST/KICKED`), `percent`, `duration_ms`, `number`. Colori come `ReceiverStateColor`: verde servita, rosso persa, giallo mandata via. |
| Stanza ricreata ora | `remaking` | per 1-4 s (e finche' la rete non torna: la libreria riprova ogni 2 s se il sistema la rifiuta) chi prova a entrare riceve un errore di connessione: non e' un guasto. `dlph_state_text` dice "Re-making the room: one moment"; con `remaking` vero il `state` puo' essere `ROOM_DOWN`, non e' una chiusura voluta. |

**Pulsanti (come Send Titles):**
* **B** = "Close room" con conferma (`kCloseText` come `delivery_run.cpp:375`: "A console that has not finished will stop receiving."). Se nessuna console e' dentro si puo' chiudere senza chiedere (a vostra scelta). A = si', B = no. Chiudere = fermare il thread radio, `dlph_stop()`, uscire.
* Un pulsante per **"Send away"** (es. X, visibile solo con una console dentro): `dlph_request_kick()`. La console in corso esce, la stanza si ricrea in 1-4 s. Niente conferma necessaria.
* **SELECT** = "Screens off" (`dl_toggle_screens`, vedi `delivery_run.cpp:480-488, 517-525`).

**Quando nessuno e' dentro**, sullo schermo alto un blocco di istruzioni per chi sta all'altra console (testo inglese, da `research/07` par. 7 e scout dei documenti):
1. "On the other console, open Download Play and choose Nintendo 3DS."
2. "Pick '3DS Link' from <nome di questa console>. About 12 s to download, a few more to install."
3. "The other console needs Luma3DS." (il pacchetto e' firmato in modo fittizio: servono le patch della firma.)
4. "One console at a time. If it fails, try again in a few seconds."
5. "About 16 MiB free on its SD card. Wi-Fi switch ON. Keep the console open and this screen on."
Il nome mostrato al client e' quello utente della console host (`udsInit(size, NULL)` usa il nome di sistema): per mostrare il Nickname di 3DS Link passarlo a `udsInit` (via `Wireless::Open`, vedi 5).

## 4. API (`DlpLink/host/dlph.h`, C con guardie `extern "C"`)
```c
dlph_cfg cfg = {0};
cfg.child_path = "sdmc:/3ds/3DSLink/dlp/child.cia";   // g.paths.datadir + "dlp/child.cia", vedi 6
cfg.lang = 0xFF;            // lingua di sistema per nome/descrizione che vede il client (per 3DS Link meglio inglese)
cfg.gap_us = 3000;          // misurato: 10 s a scarico; 0 era piu' lento (17 s)
cfg.persistent = false;     // stanza da 2 ricreata (1.0)
cfg.max_nodes = 2;
cfg.mac = {..};             // vedi 5
cfg.log = ...; cfg.event = ...;   // callback chiamate dal THREAD RADIO
dlph_open(&cfg); dlph_start();
// thread radio:  while (!stop) { dlph_poll(); /* >= 10 Hz */ }
// thread UI:     dlph_get_stats(), dlph_history(), dlph_request_kick(), dlph_set_gap_us()
dlph_stop(); dlph_close();
```
* `dlph_poll` blocca fino a ~0,6 s mentre manda un blocco e 1-4 s mentre ricrea la stanza: **non va chiamato dal thread dello schermo**. Tutto il resto (`open/start/poll/stop/close`) dallo stesso thread radio.
* `get_stats`/`history`/`request_kick`/`set_gap_us` si chiamano da qualunque thread: la UI legge un'istantanea pubblicata (LightLock) dopo ogni poll e ogni 16 frammenti.
* Le callback `log` e `event` arrivano dal thread radio: **non toccare `g.ShowToast` ne' stringhe della UI da li'**; mettere un flag e leggere le statistiche dal thread UI. Eventi: `ROOM_OPEN`, `CLIENT_JOINED`(posto), `CLIENT_NAME`, `PROGRESS`(percento), `CLIENT_SERVED`, `CLIENT_LOST`, `ROOM_REMADE`(1 = per un client fermo).
* Il chiamante possiede `udsInit`/`udsExit` e non deve tenere un'altra rete UDS (una console ha una sola rete). `dlph_open` fa `psInit` da se'; `dlph_stop` (`udsDestroyNetwork`) va **prima** di `udsExit`.
* Memoria: ~31 KB di variabili statiche piu' il figlio in RAM (`malloc`, 2-3 MB; limite 32 MiB). Nessun problema di memoria sul heap dell'app (88 MiB liberi a riposo sulla New 2DS XL; vecchia 3DS stimata 25-30 MiB).

## 5. Come agganciarlo (punti del codice dell'app)
* **Pagina e rete**: `delivery::Wireless` (`delivery.cpp:161-184`): `Open()` chiama `session::SuspendNetwork()` (chiude i socket e `ac`) e `udsInit(TS_SHARED_MEM = 0x40000)`; `Close()` fa `udsExit`, ripristina energia e `ResumeNetwork()`. Le chiamate dlph stanno **fra** `Open` e `Close`. Mai due `Wireless` insieme (il pattern di `ReceiveTitlesPage` tiene un `Wireless` per tutta la vita della pagina).
* **MAC (prima di `Wireless::Open`)**: `SOCU_GetNetworkOpt(SOL_CONFIG, NETOPT_MAC_ADDRESS, mac, &len)` mentre `g.soc_ok()` e' vero (e' la fonte provata sull'hardware; la demo fa cosi'). Ripiego: `setup::ReadConsoleMac()` (`system.cpp:315`), non verificato che dia gli stessi byte: alla prima prova **loggare entrambi**. Senza il MAC giusto la somma di controllo e' sbagliata e nessun client vede nulla.
* **Thread**: due strade, scegliete voi.
  (a) come Send Titles: `RunOnBigStack` (`delivery.cpp:129`, 256 KiB, priorita' del thread principale) con un tick ogni ~250 ms (`SendTick`, `delivery_run.cpp:533`); semplice ma lo schermo si ferma mentre `dlph_poll` e' dentro (fino a 0,6 s, e 1-4 s alla ricreazione);
  (b) **consigliata**: pagina normale (come `ReceiveTitlesPage`, `pages_delivery.cpp:690-859`) + un thread radio dedicato (64 KiB bastano, priorita' = principale + 1, core -2 come `CopyWorker`, `copyworker.cpp:130`). L'unico costo: il ciclo principale (`main.cpp:105-165`) disegna a ogni frame e non conosce `dl_skip_drawing` (schermi spenti).
* **Energia**: `Wireless` NON blocca sleep e HOME. `g.HoldHome()` (`ui.cpp:393`) per HOME; per lo sleep `aptSetSleepAllowed(false)` all'apertura e `true` alla chiusura (oggi lo fa solo `ts_power_begin_transfer`, chiamata da `sender_run`, che qui non c'e'; `delivery.h` non espone gli helper `ts_power_*`: aggiungere piccoli wrapper in `titleshare_host/delivery.c` o chiamare `apt` direttamente, poi `g.ApplyHome()` alla fine). Se la console va in sleep la rete UDS cade. Scrivere sullo schermo "Keep the console open".
* **Cartelle e file**: `Paths` (`paths.hpp:59-76`, `paths.cpp:168-187`): aggiungere `dlp_dir = datadir/dlp` e `dlp_child = dlp_dir/child.cia`; creare con `MakeDirs` (`paths.cpp:207`). Usare `g.paths.datadir`, non il percorso fisso `sdmc:/3ds/3DSLink`.
* **Log**: callback `cfg.log` -> `ts_log("%s", line)` con `ts_log_open("dlp")` prima di `dlph_open` e `ts_log_close()` dopo `dlph_close` (`ts_log` e' thread-safe e scrive solo con il Debug log acceso, `module_config.cpp:15`).
* **Build**: il Makefile (`ConfigApp/Makefile:36`) compila a caratteri jolly `source ../Sources/azl ../Sources/enet`; TitleShare con liste e regole a parte (50-53, 235-237). Il Docker documentato monta **solo** `AzaharLink/` (`README.md:1495`): `DlpLink/` non si vede. Strada piu' semplice: copiare `dlph.c`, `dlph_wire.c`, `dlph.h`, `dlph_wire.h` in `ConfigApp/dlph/` (aggiungere la cartella a `SOURCES`/`INCLUDES`) con un controllo di manifesto come `Tests/titleshare_vendor_check.sh`. Le intestazioni hanno gia' `extern "C"`, quindi si includono da C++.
* **Cosa serve e cosa NO** (2026-10-21): la voce **serve `boot9.bin`** (condividere lo vuole, come Send Titles di Titles Delivery: senza, la voce e' spenta con «needs boot9.bin», una nota e il tasto Y per la guida), e si spegne con l'interruttore *Settings > Download Play delivery*. **Non** serve il Plugin Loader ne' una Luma recente. La scansione dei titoli (e `AttachModule`, `movable.sed`) serve **solo** se il pacchetto va costruito dal 3DS Link installato (fonte (b) del compositore, `COMPOSITORE.md`); con `dlp/3dslink.cia` al suo posto no.
* **Conflitti**: nessuno con TitleShare (usa `TSH`/id8 0, la stanza Download Play e' `0x2810`/`0x55`; non sono mai accese insieme). Il canale dati 1 e' del broadcast Download Play e anche di TS: irrilevante finche' non stanno insieme.
* Stringhe: UI in inglese; toast finale come `delivery_run.cpp:1190-1196`.

## 6. Il file `child.cia` (cosa c'e' e cosa NO)
* `dlph_open` vuole un CIA di figlio Download Play: titolo `00040001...`, firma valida nel formato, SMDH nel meta. Rifiuta un file rovinato e **lo scrive nel log** (`the child CIA is DAMAGED...`): la schermata dice «child.cia is damaged» con quel testo.
* **3DS Link e' UN CIA solo (decisione dell'utente, 2026-10-21): `child.cia` non e' un file che si consegna.** Lo costruisce l'app, sulla console, la prima volta che la pagina *3DS Link Delivery* ne ha bisogno (e di nuovo quando cambia la versione o il file manca): `AzaharLink/ConfigApp/dlpc/` (il compositore, C99) lo compone dal **modello** che l'app porta come risorsa (il figlio senza il suo RomFS), dal CIA dell'app e dal plugin; progetto, fonti del CIA e prove in `COMPOSITORE.md`. La pagina guarda il file prima di aprire la stanza: se e' li' ed e' della versione dell'app, apre la stanza; altrimenti mostra una schermata «Preparing the package» (che cosa fa, quanto manca, B con una domanda) e **la stanza non e' mai aperta mentre il pacchetto si costruisce**. Il percorso e' `sdmc:/3ds/3DSLink/dlp/child.cia` (`Paths::dlp_child`).
* Il figlio vero (`DlpLink/child/`, «3DS Link Installer», «Powered by Gionko · vX.Y.Z»: installa `3dslink.cia` e `AzaharLink.3gx` con la regola «non installato → installa, piu' vecchio → aggiorna, uguale → solo il plugin, piu' nuovo → niente») lascia dopo l'installazione una copia del CIA dell'app in `sdmc:/3ds/3DSLink/dlp/3dslink.cia` (la prima fonte del compositore: una console con solo Luma non ha `boot9.bin`) e, se `boot9.bin` manca, mostra l'avviso non bloccante con la guida (`GUIDA-BOOT9.md`). Provato su PC e nell'emulatore, **mai su una console**: `test/PROVA-INSTALLATORE.md`.
* Il figlio di makerom (`out/3dslink-child.cia`, e `.emu.cia` per Azahar) esiste ancora, ma come **riferimento dei test**: il compositore e' provato contro di esso byte per byte, `verify_cia.py` lo giudica, la prova nell'emulatore installa la sua variante in chiaro. **Non si consegna** (e `dist/dlp/child.cia` non c'e' piu').
* **La versione si registra alla consegna, e cio' che la versione significa e' cambiato**: `python tools/version.py guard APP.cia MODEL.bin PLUGIN.3gx releases.tsv` (con `build_all.sh --register`) prende lo SHA-256 dei **tre file da cui l'app compone il figlio** (il CIA dell'app, il modello, il plugin), li unisce in un digest e rifiuta una versione gia' consegnata con un digest diverso (un client Download Play che ha tenuto il figlio con quel Title ID e quella versione risponde «ho gia' il contenuto» e rilancia la copia vecchia). makerom non e' riproducibile, quindi ogni ricostruzione cambia il digest: prima di una build da consegnare dopo che una versione e' arrivata a una console, `python tools/version.py bump micro`. `releases.tsv` lo crea la prima registrazione (alla prima consegna a una console): oggi non esiste.
* Prima del compositore esisteva solo la sonda (`DlpLink/probe/out/ChildProbe.cia`, TID `0004000100FD5100`): consegna, installa, avvia, scrive sulla SD, usa `am:net`, RomFS, UI citro2d, pacchetto regalo. Con essa si provava la voce di menu; non serve piu' (resta come strumento).

## 7. Regole da far rispettare all'app (da `PIANO.md` e `research/02,04,05,07`)
* **Title ID**: app `000400000F83D100`, figlio `000400010F83D100`. Attenzione: `tools/common.sh`, `verify_cia.py` e `research/04` hanno ancora `FD3D`: vince il PIANO, gli strumenti vanno allineati.
* **Versione**: un solo contatore crescente, uguale nel TMD dell'app e del figlio. **Mai** riusare una versione con contenuto diverso e mai abbassarla: se il client ha gia' quella versione risponde "nessun contenuto" e rilancia la copia vecchia (stato `NOCONTENT` nei log: `the client already has this version`). La versione si scrive nel TMD dopo `makerom` (che perde il campo minor). 1.0.0 = `1024` (u16 = `M<<10 | m<<4 | u`).
* **Beacon pulito**: versione di sistema host 0 e niente frame 4; fatto dalla libreria (provato su console vecchie e nuove).
* Il client mostra nome (max 63 caratteri), descrizione (127), icona 48x48 dall'SMDH del figlio, nella lingua di `cfg.lang`.
* Dimensione consigliata del figlio <= 8 MiB (limite duro 32 MiB). Con il carico di 1.0.0 (2,28 MB: app 0,99 + plugin 1,01 + installatore e modello) sono 9 blocchi, ~9-10 s con `gap_us = 3000` (2,65 MB misurati: ~10 s). **Dalla 1.0.1 il CIA dell'app porta il plugin (1,55 MB) e il figlio e' di 2.847.680 byte: 11 blocchi, circa 12 s per proporzione (non misurato)**; il figlio porta il plugin anche a parte, per l'installatore che non e' cambiato (e' circa 1 MB che l'app, al suo primo avvio, riscriverebbe comunque: se l'aria diventasse un problema, e' li' che si risparmia).

## 8. Rischi e incognite che toccano la schermata
* Console con **Luma3DS vecchio** (< 13): niente Plugin Loader, ma il Download Play e il figlio funzionano; il figlio stesso non deve chiamare `plg:ldr` se non c'e'.
* Console vecchie (Old 3DS) e regioni diverse da EUR: mai provate.
* Se la console host entra in sleep o viene chiusa, la rete cade: avvisare sullo schermo.
* Wi-Fi spento (interruttore): la rete UDS non parte; `Wireless::Open` ritorna un errore (testo in `WirelessErrorText`, `delivery.cpp:186`).
* Un client che si pianta tiene la stanza occupata fino a 9 s; una seconda console in quel tempo trova la stanza piena o un errore di connessione.
* Confermato su hardware: il nome della console (`udsGetNodeInformation`, 26-70 ms dopo l'entrata) e la stanza da 2. Da guardare ancora: la durata con `gap_us = 3000` in questa libreria (un solo scarico vero finora, a `gap_us = 0`).

## 9. Ordine dei prossimi passi
Fatti (2026-10-21, senza console): (1) la prova unica della libreria (utente, 2026-10-20); (2) il figlio installatore vero, il compositore, la catena di build senza cicli e il registro delle versioni; (3) l'integrazione del compositore nell'app (la schermata «Preparing the package», l'interruttore, la voce spenta senza `boot9.bin`, la guida in tre posti). Da fare: (4) una prova su strada: A host con 3DS Link Delivery (installato con FBI e installato dal Download Play, con e senza `boot9.bin`: `AzaharLink/CHECKLIST_DA_VERIFICARE.md` O15-O20), B senza 3DS Link, C vecchia; il passaggio nell'emulatore con i figli composti (`DlpLink/tests/ship_check.sh`, `SHIP_KEEP`).

## 10. Decisioni ancora aperte (non bloccanti per la UI)
Avviso di consenso sul client o "A = Install" esplicito; nome del figlio e dell'host mostrati nella lista del Download Play; politica "gia' installato" del figlio (stessa versione o piu' nuova: non toccare) e opzione di reinstallare; HOME riabilitata alla fine; la regola "mai installare il proprio TID".
