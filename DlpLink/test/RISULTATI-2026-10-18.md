# Risultati del test unico, 2026-10-18 (host = New 2DS XL, Luma 13.3.3, sistema 11.17; log in `rawhost_log_2026-10-18_giri-0-1-3-4-5-6.txt`)

| Preset | Cosa | Esito | Contenuto | Rinvii |
|---|---|---|---|---|
| 0 | baseline (Zelda, 3 nodi) | OK, il figlio-sonda installa e parte; pacchetto regalo in HOME | 30,3 s | 9 |
| 1 | rete unica (8 nodi, appdata 77, beacon pulito) | **OK**; il client mostra la stanza come «1 di 8» | 24,4 s | 7 |
| 3 | rete unica + 200 frame/s sul canale 3 (199 fps effettivi, 0 rifiutati) | OK | 25,8 s | 5 |
| 4 | rete unica + carico «1500» (effettivi **490 fps**, ~0,7 MB/s, 0 rifiutati) | OK ma la ricerca e' piu' lenta; download **3x piu' lungo** | **78,3 s** | 14 |
| 5, 1o client (recente) | piu' client, beacon sempre acceso | OK | 26,9 s | 7 |
| 5, 2o client (**console vecchia**) | idem | **si collega alla rete (nodi 2/8) ma non risponde mai ad `auth`; resta in «attendi»; nessun pacchetto ricevuto dall'host in 85 s** | - | - |
| 6 | PEER | **non provato**: l'utente l'ha lanciato sulla stessa console dell'host (scan: 0 reti, giusto) | - | - |
| 7 / 4c | PEER canale 1 | non provato | - | - |

Altro dal log: canale Wi-Fi scelto dal sistema 1 o 6; `keepopen` e `foreign_ms` funzionano su hardware; tempo `udsDestroyNetwork`->`udsCreateNetwork` di fila: 1,1 s - 3,7 s (4 misure).

## Preset 14 e 15 (2026-10-18 notte / 2026-10-19), log: `rawhost_log_2026-10-18_preset14.txt`, `rawhost_log_2026-10-19_preset15.txt`

* **Preset 14 (REBIND del canale dati): NON basta.** Il secondo client entra nella lista dei nodi, `udsGetNodeInformation` fallisce (0xC90113FA, per il primo andava), le `auth` partono (rc 0), zero pacchetti di ritorno in 140 s.
* **Preset 15 (REARM, rete ricreata dopo ogni client): FUNZIONA.** Quattro consegne di fila a due console (24,7 / 23,9 / 27,0 / 25,9 s, 5-7 rinvii), ricreazione della rete ~1,1 s. E' anche cio' che fa il Download Play vero: la sessione finisce col BeginGame.
* **Due client insieme (ingresso a pochi secondi, o il secondo a meta' del primo): NON funziona.** Entrambi arrivano a «content», poi per ~45-90 s non mandano piu' un pacchetto finche' l'utente annulla; **il secondo che entra a meta' blocca anche il primo.** Concorrenza non supportata dall'host (consegna unicast a un client per volta, blocchi da 0,55 s).
* Un client che prova a entrare mentre il primo sta installando il figlio ha avuto «errore di comunicazione»; l'altro e' entrato dopo che il primo era tornato alla HOME (poteva essere la lista di Download Play rimasta con la stanza vecchia: la rete ricreata ha un altro id; da verificare ricaricando la lista).
* Nodi visti con nome: `player-k`, `Friend-B`.
* Rimedio aggiunto (preset 16): `block=1`, da StartDistribution in poi la rete rifiuta nuovi client (`udsSetNewConnectionsBlocked`), cosi' un ritardatario non rovina il download in corso.

## 2026-10-19 sera: "nessun software trovato" nei preset 17, 18 e 15 (MENU-20) = child.cia ROVINATO sulla SD dell'host; ritrasmissione selettiva VERIFICATA su hardware

* Causa del «nessun software»: il `child.cia` sulla SD dell'host aveva stessa dimensione ma md5 diverso (`7bdcf143…` invece di `aa4fa116…`): 3.680 byte diversi nella zona certificati/ticket/TMD (0x2041-0x333d, zeri e byte a caso). L'host leggeva un TMD senza senso e annunciava nel beacon il Title ID `B3B4358FCCEA8504` versione 16688: i client lo scartavano. Con il file giusto (MENU-21, preset 15) tutto torna a funzionare. Non c'entrano ne' il codice ne' la radio. MENU-22 rifiuta un `child.cia` con Title ID che non comincia per 00040001 e lo scrive a schermo.
* **Ritrasmissione selettiva su console (MENU-21, preset 15, 2 consegne a Friend-B e player-k): FUNZIONA al primo colpo con l'interpretazione 0** (parole little-endian, bit meno significativo per primo = byte per byte; bit acceso = frammento mancante). Contenuto in **9,7 s e 10,6 s** (prima 24-27 s), **0 rinvii di blocchi interi**, 7 e 21 frammenti rimandati in tutto.
* Nel primo giro un secondo client (`player-k`) e' entrato a 8 s dall'inizio del primo (preset 15 non ha la coda): e' rimasto in lobby senza partire e se n'e' andato dopo ~16 s; consegna riuscita poco dopo dopo la ricreazione della rete. La coda (preset 17/18) e' ancora da provare con il file giusto.

## Preset 18 (QUEUE, MENU-22), 2026-10-19: la coda funziona; il guasto e' il RIUSO DI UN NUMERO DI NODO LIBERATO

* **Primo gruppo**: tre client (`Friend-B`, `player-k`, `Player-H`) entrati a pochi secondi uno dall'altro durante il download del primo: coda `queue: node 3 starts now`, `node 4 starts now`, contenuti **9,8 / 9,6 / 10,5 s**, nessun blocco intero rimandato, rete ricreata a coda vuota. Tre installazioni riuscite.
* **Secondo gruppo**: primo e secondo ok (10,0 / 10,0 s). Poi un client entra **mentre il secondo scarica, quando il nodo 2 e' libero**: prende il nodo 2 di nuovo, `udsGetNodeInformation` fallisce (`0xC90113FA`, stato di connessione 6) e il client resta ~47 s senza rispondere ad `auth` (risponde da solo alle 8921695); lo stesso con il nodo 3 liberato. Da li' si incrocia tutto e l'utente annulla.
* **Regola che emerge, valida in tutti i log**: un client che entra mentre altri sono presenti prende un numero di nodo NUOVO e va; un client che entra in un numero di nodo gia' usato e liberato in QUESTA rete resta mezzo collegato. Una rete appena ricreata ha tutti i numeri puliti: per questo la rete ricreata per client (preset 15) e la coda (18) funzionano finche' nessuno riusa un numero.
* Rimedio (MENU-23, preset 19 LOCKQ): quando se ne va il primo client e ce ne sono altri in coda si chiude la porta (`udsSetNewConnectionsBlocked`) e si spegne il beacon fino a fine gruppo, poi la rete viene ricreata; un nodo che non completa l'ingresso in 6 s viene espulso (`udsEjectClient`). Alternativa ancora da provare: preset 17, con l'espulsione di ogni nodo uscito (se il riuso poi funziona, il blocco non serve).

## Preset 17 e 19 (MENU-23), 2026-10-20

* **L'espulsione PULISCE il posto**: nel preset 17, dopo `udsEjectClient(2) after it left -> 0x00000000`, un nuovo client entrato nel posto 2 (riusato) ha avuto `udsGetNodeInformation` ok subito e ha risposto ad `auth` dopo 0,8 s. Senza espulsione (preset 18) lo stesso caso restava 47 s mezzo collegato. Un solo campione: da confermare.
* **Preset 19 (LOCKQ) funziona**: gruppi da 1, 2 e 3 client serviti in fila (~10 s ciascuno), rete ricreata a fine gruppo, blocco `lock: node N left while others are in the queue...` visto nel log. Entrare durante la ricreazione da errore di connessione (1-3 s): atteso.
* **Resta un guasto intermittente, in TUTTI i preset**: un client in download diventa muto per decine di secondi (nel preset 19 un gruppo da 3 intero; nel 17 due client, uno dei quali si e' sbloccato dopo 14 s quando e' entrato un altro nodo). Il numero di sequenza dei suoi pacchetti avanza di 1 in 14 s: il client non sta mandando nulla, non e' l'host che non legge. Ultima azione dell'host prima del silenzio: un invio selettivo (19, 17-nodo 3) o un blocco intero (17-nodo 2). Frequenza: ~4 su ~25 consegne. MENU-24: watchdog (3 s muto = rimanda il blocco, 15 s = espelle il client e la coda va avanti) e registro degli invii falliti (`udsSendTo to node N failed`), preset 20 = 17 + watchdog.

## Preset 20 (MENU-24), 2026-10-20: fallito per un ERRORE MIO nel watchdog; correzione in MENU-25

Il log mostra `watchdog: node 2 silent for 18446744073709551600 ms`: sottrazione senza segno. In `tick()` il tempo `t` e' preso all'inizio, prima di `pump_rx()`, che aggiorna `lastRx` a un istante piu' tardi: `t - lastRx` va in negativo e diventa enorme, quindi il watchdog ha espulso OGNI client appena entrava in download (poi i nodi espulsi rientravano su posti sporchi: `0xC90113FA`, espulsione a 6 s). Il test non dice niente sulla stanza fissa. Il test su PC non lo prendeva perche' il suo orologio non avanzava durante la lettura dei pacchetti: ora avanza (+3 ms per pacchetto) e il watchdog e' acceso in tutti i casi di perdita con il controllo «non deve mai scattare su un download sano»; scenario D: client che tace dopo 6 ack → ri-invio del blocco a 3, 6, 9, 12 s ed espulsione a 15 s (verificato). Correzione: il silenzio si calcola con l'orologio di adesso e mai in negativo (`silent_ms`).

## Preset 20 bis (MENU-25), 2026-10-20: tutti i download muti; ma il confronto fra tutti i log dice una cosa precisa

Conto delle consegne (tutti i log da test15 in poi, esclusi i due giri con client avviati in contemporanea e il test 20 con il bug) per **numero massimo di client collegati durante il download**:

| client collegati | riusciti | falliti |
|---|---|---|
| 1 | 11 | 1 |
| 2 | 7 | 0 (**7 su 7**) |
| 3 | 2 | 6 |

(Il test 20 bis, in cui falliscono tutti, e' una sessione cattiva: esclusa dal conto sopra; con 1 o 2 client, `watchdog` a 3 s non riesce a risvegliare nessuno.) **I preset 17 e 20, che sembravano «la stanza fissa non regge», avevano entrambi tre client collegati: il confronto con 18 e 19 era sbagliato.** Con host + 2 client (3 nodi, come il beacon vero di Zelda, `max nodes = 3`) non e' mai fallito.
Conclusione adottata: stanza da 3 nodi (host + 2 client). MENU-26: preset **21** (3 nodi, espulsione, coda, watchdog: la stanza fissa), **22** (3 nodi, blocco+rete ricreata, coda, watchdog), **15** (1 solo client, rete ricreata: base sicura).

## 2026-10-20 notte: leggendo i log per ISTANZA DI RETE (fra due `udsCreateNetwork`) esce una regola netta

Sequenza delle consegne in ogni istanza (S = riuscita, F = muta): test15 `S|S|FF|FF|S|S`, test15bis `S|S`, test17bis `FF`, test18 `SSS|SSF`, test19 `SS|S|F|SSS|SS|S`, test20bis `FFFFF`.
**Dopo la prima consegna muta, in quella stessa istanza di rete non ne riesce piu' nessuna** (mai una F seguita da S); la rete ricreata guarisce subito (test15, test19). Per questo i preset con rete ricreata (15, 18, 19) sembravano «migliori» di quelli con rete fissa (17, 20): i secondi, dopo il primo guasto, restavano rotti per sempre (20 bis: cinque F di fila, anche con un client da solo). Il primo guasto compare solo con tre client collegati (3 gruppi su 5), o con client avviati in contemporanea; con uno o due client collegati mai (18 su 19).
* MENU-27: il watchdog, dopo `stall_ms` (3 s) rimanda il blocco; dopo `drop_ms` (9 s) **ricrea la rete** (prima espelleva soltanto il client). Preset 21 (8 nodi) e 22 (3 nodi), identici in tutto il resto: coda, beacon sempre acceso, espulsione di chi esce, rete ricreata al primo guasto e a coda vuota. L'unica differenza e' `max_nodes`.
