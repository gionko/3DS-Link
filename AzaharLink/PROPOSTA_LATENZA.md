# Proposta per 3DS Link: la latenza degli altri giocatori (ramo `3DSlatency`)

Nota per Gionko, scritta il 2026-11-11. Questo ramo **non è un altro 3DS Link**: è 3DS Link 1.1.3 più una funzione, la latenza verso gli altri giocatori di
una partita. Qui c'è, pezzo per pezzo, che cosa il ramo chiede a 3DS Link, che cosa cambia di tuo e che cosa no, quanto costa e con che prove, così puoi
decidere che cosa unire, che cosa cambiare e che cosa lasciare fuori. Il dettaglio tecnico è in `NOTE_LATENCY.md`; le build di prova escono dal fork
`gigamaster97-stack/3DS-Link` come pre-release.

## In una riga

Il plugin legge i pacchetti che il gioco stesso manda e riceve su UDS (Pia e PRUDP v1), ne ricava per ogni altro giocatore il tempo di andata e ritorno,
lo scrive in `latency.log` e lo mostra sullo schermo alto mentre la tastiera della chat è aperta. **Non manda niente, non serve il plugin sulle altre console,
non aggiunge thread.**

## I pezzi, in ordine di quanto toccano il tuo codice

Ogni gruppo si può unire senza i successivi.

### A. Il modulo di misura e il suo log (solo aggiunte)

| Dove | Che cosa |
|---|---|
| `Sources/azl/latency.cpp`, `Includes/azl/latency.hpp` | file nuovi: l'anello, lo stimatore, `latency.log` |
| `Sources/ctr/uds_hle.cpp` | 2 chiamate: `latency::OnRecv` dopo un PullPacket riuscito con dati, `latency::OnSend` dopo un SendTo riuscito (7 righe con l'include) |
| `Sources/ctr/plugin.cpp` | nel callback del tick della stanza: `latency::Tick(now_ms, in_session)`; in EarlyInit il file (`LatencyFile`), scritto dal thread del log dentro il cancello dello swap come il log principale; nella finestra delle statistiche **una riga**, `latency::NoteLink(link.rtt_ms, link.rtt_var_ms, link.loss_permille)` |
| `Tests/latency_test.cpp`, `Tests/run.sh`, `Tests/ipc_test/build.sh`, `Tests/policy_check.sh` | la prova del modulo, la sua riga nei due script, i pin |

**Giochi che non parlano né Pia né PRUDP v1 (novità del 2026-11-12).** I log di una console vera (build 11g) mostravano "protocol unknown" per *Tekken 3D Prime Edition*
e *Tetris*: nessuna latenza. Con catture di Azahar (plugin di cattura, stesso PC o contro la console) ho decifrato due strati piccoli e li ho aggiunti dentro
`latency.cpp` (nessun file nuovo, nessun'altra modifica al tuo codice): **Tetris**, uno strato con le lettere `ack` nell'intestazione (dati affidabili e ACK; la
latenza è "il mio dato è partito" -> "è arrivato l'ACK", meno l'attesa dell'altro gioco, stimata con la MIA attesa), e **Tekken**, una richiesta corta ogni 2 s con
risposta immediata, e **Street Fighter IV** (2026-11-13), un keepalive al secondo con un numero e l'ACK da 28 byte che lo riporta (meno l'attesa dell'altro gioco,
stimata con la mia, come Tetris). Il campo di `protocol` nel log ora è `<nome>,pia,prudp,altri,lock=..,ack=N,tk=N,sf=N`. I tre strati hanno la stessa forma (un
numero che torna indietro, o una richiesta e la sua risposta): lo stimatore, l'attesa e la finestra sono lo stesso codice, cambiano solo i byte che dicono "questo è il
numero". Misurato: Street Fighter su due istanze (ritardo vero 33,4 ms) mostra 32 ms; su 120 s di partita vera contro una console attraverso una stanza pubblica 66 ms.
Le prove: `Tests/latency_test.cpp` modi `ack`, `tekken`, `ssf4`. Non provato su due console vere.

**Picchi (2026-11-13).** La mediana della finestra e' stabile per costruzione, quindi un peggioramento di pochi secondi non la muoveva: ora il modulo guarda
anche gli ultimi 2 s da soli e, mentre sono chiaramente peggiori della finestra (almeno 1,5 volte e 30 ms sopra), sullo schermo va quel valore (da 50 a 500
per due secondi: si legge 500 subito); passato il picco torna la finestra; sullo schermo della chat il valore ha un `!` accanto finche' dura (`Gionko:480ms!`). `latency.log` lo dice
(`lat: n2 spike: ...` / `spike over`). Prova: `Tests/latency_test.cpp` modo `spike`. La finestra e' di 30 s per tutti i giochi (quelli con pochi campioni
avevano 120 s: troppo lunga da seguire).

Che cosa fa sui thread del gioco: una copia di al massimo 96 byte in un anello di 128 record (`__sync_fetch_and_add`: nessun lock, nessun log, nessuna allocazione,
niente sullo stack); finché la console non è in una lobby l'anello non esiste e il costo è un caricamento e un salto. Tutto il resto gira sul thread di rete, nel suo tick.

Che cosa **non** tocca: la struttura `Stats`, la pagina di stato, `LinkStats` (viene solo letta, dalla riga sopra), i protocolli, la stanza. Il modulo non ha un
contatore di CPU suo: il suo costo è già dentro i numeri della tua riga `stats:` ("queue+tick" per il thread di rete, "uds" per i thread del gioco).

`latency.log` (solo con `debug=1`, file a parte, ruota a 8 MB): ogni riga comincia con l'ora dell'orologio della console; mai un indirizzo MAC, solo i nomi.

```
12:20:32.490 lat: n2 "Bob" 37 ms (30 s: min 31, p90 52; 290 samples, M)
12:20:35.000 lat: room link 6+-4 ms, loss 5/1000
```

La seconda riga sono i **tuoi** numeri del collegamento alla stanza (ENet), messi accanto alla latenza dei giocatori: chi legge il file di un tester distingue
"la mia linea" (sale `room link`, salgono tutti) da "la linea dell'altro" (sale un giocatore solo) e da "il plugin" (`lat: ring dropped`, pochi campioni).
È il motivo per cui il file esiste: togliere il fattore umano dai resoconti dei tester.

### B. Sapere chi è un nodo (aggiunte al motore e al plugin)

| Dove | Che cosa |
|---|---|
| `Includes/azl/uds_engine.hpp`, `Sources/azl/uds_engine.cpp` | due funzioni di sola lettura: `NodeMac(node, &mac)` e `MyNodeId()` (15 righe) |
| `Includes/ctr/plugin.hpp`, `Sources/ctr/plugin.cpp` | `RoomNickOfMac` (il nickname che la stanza dà a chi ha quel MAC) e `NodeOfNick` (il nodo di chi scrive con quel nickname) |

Servono a dare a un nodo **lo stesso nome che la chat scrive** (nodo -> MAC -> membro della stanza -> nickname). Il MAC resta in memoria e non va in nessun file.

### C. La latenza sullo schermo (aggiunte al disegno della chat)

| Dove | Che cosa |
|---|---|
| `Includes/azl/chat_latency.hpp` | file nuovo: le righe (`BuildLatencyRows`) |
| `Includes/azl/chat_core.hpp` | `ChatRing::Replace` (sostituisce tutte le righe sotto un solo numero di sequenza), il campo `Line::sep` nel riempimento della riga |
| `Includes/azl/chat_draw.hpp` | `DrawLatencyLines`, `DrawTiles`, le scale dei colori |
| `Includes/azl/chat_host.hpp` | `LatencyBoard` nell'`Overlay`, `DrawLatency`, l'ordine in `DrawOverlayTop`: latenza, notifiche, chat |
| `Sources/ctr/chat_overlay.cpp`, `Includes/ctr/chat_overlay.hpp` | `ChatLatencyDue` / `ChatLatencyPublish` (il thread di rete costruisce le righe 4 volte al secondo, solo se il disegno le chiede) |

Regole: **solo con la tastiera della chat aperta**; nell'angolo `chat_pos ^ 2` (lo stesso lato della chat, l'altro capo dello schermo), cioè sulla stessa
riga delle tue notifiche; fino a 3 altri giocatori una riga a testa (`[H] Ana:28ms`), da 4 in su piastrelle colorate senza nomi; mai il proprio giocatore.
Il framebuffer è **solo scritto**, come nel tuo disegno. Costo per un thread del gioco: un store atomico a fotogramma mentre la tastiera è aperta, zero altrimenti.

### D. Cose che cambiano l'aspetto della TUA chat (decisioni tue)

Queste non servono alla misura: sono scelte di aspetto fatte da Mirko provando la build. Sono separate e si tolgono una per una.

1. **Colore dei messaggi dal nodo, non dall'hash del nick.** Chi è in partita ha il colore del suo nodo (1 azzurro = host, 2 verde, 3 giallo, 4 arancio,
   5 rosso, 6 viola, 7 bianco, 8 fucsia) nei messaggi, nella riga della latenza e sotto la piastrella: lo stesso colore è la stessa persona ovunque.
   **Fuori da una partita non cambia niente** (restano i colori dell'hash). In partita, chi scrive senza essere un giocatore di quella partita è **grigio**:
   i cinque colori dell'hash sono gli stessi dei nodi 1, 2, 3, 4 e 6, e sembrerebbe un giocatore. Si toglie non chiamando `ChatSink::SetNodeSource`.
2. **Nick tagliato a 8 caratteri più un punto** (`Carlo Ma.: ciao`) nella chat e nella latenza, anche per l'host: il nome letto in chat è il nome letto nella latenza.
   È `CutNick` in `chat_core.hpp`; il nodo si cerca dal nick intero, prima del taglio.
3. **Niente striscia nera dietro le righe della chat: lettere con ombra**, e messaggi senza spazio tra loro. Solo scrittura, meno byte scritti della striscia
   opaca (la prova ne conta il 43 %). Le notifiche del plugin tengono la striscia nera. Limite: su una scena molto chiara si legge peggio. **Il costo su console
   non è ancora stato letto** (riga `chat: cost per frame` del prossimo log).
4. `kLines` è diventato una macro (`AZL_LINES`, sempre 5): serve solo allo strumento che disegna le scene di prova con più righe.

## Quanto costa (misurato)

| | Valore | Come è misurato |
|---|---|---|
| Memoria statica del modulo | **7.760 byte** di bss (erano 46.608: il 48 % dei 96.972 del plugin) | `arm-none-eabi-size` dell'oggetto, con i flag del Makefile |
| Anello | **15.360 byte** sull'heap, creati all'ingresso in una lobby (erano 30.720) | 128 record da 120 byte |
| Totale del modulo | **23,1 KB** (erano 77,3 KB: meno di un terzo) | bss + anello |
| Memoria statica di tutto il plugin | 58.124 byte di bss (erano 96.972) | `arm-none-eabi-size` di `AzaharLink.elf` |
| Solo in Final Fantasy Explorers | +10.240 byte sull'heap, alla prima volta che il gioco manda il suo orologio | |
| Codice | 10,3 KB | idem |
| CPU su New 3DS (MH XX, build dell'11/11 mattina) | aggancio 0,04 ms/s, tick 0,39 ms/s: **0,04 % di un core** | le tue righe `stats:` di un log vero |
| Thread di rete nello stesso log | ~327 giri/s, pausa più lunga 9 ms | idem |

### Perché l'anello è di 128 record

L'anello tiene i pacchetti del gioco tra un giro e l'altro del thread di rete. Se il thread di rete resta fermo più a lungo di quanto l'anello contiene, i
pacchetti più vecchi si perdono: non danno un valore sbagliato, danno un campione in meno, e il log lo scrive (`lat: ring dropped N`).

| Giocatori nella lobby | Pacchetti al secondo (stima) | 64 record tengono | **128 record tengono** | 256 record (com'era) |
|---|---|---|---|---|
| 4 | ~120 | 0,5 s | **1 s** | 2 s |
| 8 | ~240 | 0,27 s | **0,53 s** | 1,07 s |

La stima viene dalle catture a tre istanze: ogni console manda fino a ~32 pacchetti al secondo e ne riceve fino a ~30 da ogni altro giocatore. Nei log delle
console vere la pausa più lunga del thread di rete è stata 481 ms (una volta; nel log della New 3DS con Monster Hunter XX la più lunga è 9 ms). Con 64 record una lobby da 8 avrebbe perso
campioni in quella pausa; con 128 no. **Mirko ha scelto 128** perché il log di una lobby piena (per esempio un Tetris a 8) deve essere intero: costa 7,7 KB in
più di 64 e 15 KB in meno di prima.

## Quanto è giusto il numero (misurato)

Tre Azahar sullo stesso PC hanno lo stesso orologio: il ritardo vero di ogni pacchetto si conosce (gli stessi byte visti partire da un'istanza e arrivare
all'altra). Le catture sono state fatte passare **dentro `latency.cpp` così com'è**, e ogni secondo si è preso quello che `Snapshot()` mostrerebbe.

| | errore tipico | 90 % dei secondi entro | variazione da un secondo all'altro (95° percentile) |
|---|---|---|---|
| Pia (MH XX, MH4U, Kirby, Fantasy Life, Tri Force Heroes, Mario Party SR: 36 coppie) | +0,1 ms | -3,8 .. +5,7 ms | 1,9 ms |
| lo stesso, con lo stimatore della pre-release 1.1.3 LV2 | +0,9 ms | -4,8 .. +8,8 ms | 4,6 ms |
| PRUDP v1 (Final Fantasy Explorers, Pokémon X: 20 coppie) | +6,6 ms | +3,0 .. +12,9 ms | 1,5 ms |

Su console vera (New 3DS contro Azahar in LAN, MH XX): 15-21 ms verso l'altro giocatore, 6-7 ms verso la stanza, 5 pacchetti persi su 1000.

Scostamenti noti che restano: Kirby Battle Royale **tra due client** legge circa 25 ms in più (i loro pacchetti passano per il fotogramma dell'host); il ping
PRUDP verso l'host legge 15-27 ms in più (l'host risponde al fotogramma dopo); i valori PRUDP marcati `A` poggiano su un'ipotesi sul tempo di attesa dell'altro
gioco (per questo hanno una lettera diversa). Un protocollo cifrato o sconosciuto (Mario Kart 7) non dà nessun valore: i pacchetti vengono contati e i primi byte
scritti una volta, per poterlo guardare.

## Ingombri sullo schermo (misurato)

101.920 scene disegnate con il codice vero del plugin e confrontate pixel per pixel (4 angoli, 1-7 altri giocatori, ogni combinazione di 0-5 messaggi da 1-3
righe, cinque tipi di notifica, notifiche del framework sì e no):

* la chat **non copre mai** la latenza (0 scene);
* la latenza toglie un messaggio alla chat solo in 445 scene: servono insieme due notifiche del framework e una chat da 144 righe di pixel in su;
* **una tua notifica larga (da circa 40 caratteri) passa sopra una parte della latenza** per i suoi 5 secondi (fino a un terzo dei pixel). La notifica vince,
  perché è lei a dire che è successo qualcosa. Le notifiche corte stanno di fianco.

## Che cosa non è stato provato

* **Questa revisione su una console** (la finestra di 30 s, la riga `room link`, le tabelle ridotte). Il log vero che abbiamo è della build precedente.
* **Old 3DS**: mai.
* Il costo del testo con l'ombra su console.
* **Una lobby con più di tre giocatori veri.** Le catture sono a tre istanze: le piastrelle per 5-8 giocatori sono provate solo nel disegno, e i ~240 pacchetti
  al secondo di una lobby da 8 sono una stima, non una misura. Tetris parla uno strato suo (né Pia né PRUDP, regola `ack` nel modulo) ed è stato decifrato su tre istanze di Azahar; non è mai stato provato a 8 giocatori né su console.
* **In Azahar con risoluzione interna sopra 1x lo schermo alto torna a 400x240 mentre il plugin ci disegna** (chat, notifiche o latenza: lo stesso). È
  l'emulatore: una scrittura della CPU nel framebuffer gli fa buttare la copia ingrandita che teneva sulla scheda video (`RasterizerCache::InvalidateRegion`,
  ramo `size <= 8`). Su console non esiste. Con la latenza si nota di più solo perché resta disegnata finché la tastiera è aperta.

## Da fare prima di una pull request

1. Provare questa revisione su console (New 3DS e, se possibile, Old 3DS) e leggere `latency.log` e le righe `stats:` / `chat: cost per frame`.
2. **I mutanti**: i pin della latenza sono in `policy_check.sh`, ma `policy_mutants.py` non ha ancora i mutanti corrispondenti (lo chiede `CONTRIBUTING.md`).
3. Dividere il ramo nei gruppi A-D qui sopra, una pull request per gruppo, la A per prima.
4. Decidere insieme i punti qui sotto.

## Domande per te

1. Ti va bene `latency.log` come **file a parte**, o preferisci le righe `lat:` dentro `AzaharLink.log`? (A parte non ha MAC e si può mandare in giro così com'è.)
2. I numeri del collegamento alla stanza dentro `latency.log` (`NoteLink`): va bene leggerli da `LinkStats` in quel punto?
3. Colori per nodo, taglio del nick a 8, ombra al posto della striscia nera (gruppo D): quali vuoi, quali no?
4. Una notifica larga copre la latenza: va bene così, oppure preferisci che la latenza si sposti sotto la notifica, o che le notifiche vadano a capo prima?
5. La latenza si vede solo con la tastiera aperta. La vuoi anche in una pagina del menu del plugin (lì c'è posto per nomi interi, minimo e p90)?
6. L'anello è di 128 record (15,4 KB), per avere intero il log di una lobby da 8: i conti sono in "Perché l'anello è di 128 record". Se per te la memoria
   conta più del log di una lobby piena, 64 record bastano fino a 4 giocatori e fanno risparmiare 7,7 KB. Ti va bene 128?

## Come rifare le prove

```
docker run --rm -v <repo>:/src -w /src/AzaharLink azl-hle-build bash Tests/run.sh         # tutte le prove sul PC, policy compresa
docker run --rm -v <repo>:/src -w /src/AzaharLink azl-hle-build bash Tests/hle_tests.sh   # le prove a 32 bit del front-end IPC
bash DlpLink/tools/build_all.sh --dist                                                    # plugin, app, CIA
```

Gli strumenti dei confronti con il ritardo vero e degli ingombri (`replay_real.cpp`, `compare_real.py`, `overlap_proof.cpp`) e i loro risultati non sono nel
repository: sono nella cartella dell'audit di Mirko (`audit_2026-11-11`), con le catture.
