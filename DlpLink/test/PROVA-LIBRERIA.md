# Prova su console della libreria host (`dlph`) — la prova definitiva della stanza da 2 (decisione 2026-10-20 notte)

## ESITO (2026-10-20, 11 console in fila; il log `dlph_demo_log.txt` e' stato dato in chat, l'estratto dello scarico vero e' in fondo): modo A CONFERMATO, e' la 1.0
L'utente: la versione A non ha dato problemi, scarica, butta fuori, ricrea la stanza; e' la soluzione finale per 3DS Link 1.0.
* **11 console servite, 0 perse, 11 ricreazioni**; la stanza resta su e ferma fra una console e l'altra (anche 220 s a vuoto) e si ricrea solo dopo l'uscita di una console.
* Ricreazione: **1,10 s ogni volta** (`re-making the room` -> `ROOM_OPEN`). La console successiva entra al posto 2 senza mai restare mezza collegata (nei log vecchi della rete fissa succedeva spesso: `0xC90113FA`).
* **Il nome della console c'e' sempre**, 26-70 ms dopo l'entrata (`player-k`, `Friend-B`, `Player-H`): `udsGetNodeInformation` funziona sul nodo Download Play dopo la risposta all'autenticazione.
* Una console che ha gia' il figlio (stessa versione) viene riconosciuta (`already has this version`) e esce da sola in ~2,4 s: **10 delle 11 console erano cosi'** (il figlio non era stato disinstallato), quindi **c'e' stato UN SOLO scarico vero** (Friend-B).
* **Banda piena: peggio, non meglio.** Quell'unico scarico a `gap_us = 0`: 17,2 s, 6193 KB sull'etere per 2590 KB di figlio (2,4 volte), 0,46-0,51 MiB/s sull'etere. Con `gap_us = 3000` (log vecchio `DlpTest/DlpTestRawHost-test22`, stessa libreria sul filo): ~10 s, 1-3 frammenti su 182 rimandati. La radio impiega comunque ~2,9 ms a frammento: senza pausa la console perde quasi tutto cio' che arriva piu' in fretta di come lo legge. **Decisione: default `gap_us = 3000`** (demo e documenti aggiornati; il flag "banda piena" non serve). Un solo campione: se in futuro si vuole misurare 1500, basta un secondo scarico vero con `refused`/`again` nel log (la libreria ora scrive una riga per console: `console #n ... fragments sent (.. again, .. refused)`).
* Non verificato dal log (non loggato): lo schermo basso. L'utente dice di vedere la lista; nessuna segnalazione di campi mancanti.

Estratto del log (l'unico scarico vero, console `Friend-B`, posto 2; tempi in ms dell'orologio del programma):
```
[7475349] a console entered seat 2   [7475445] event CLIENT_JOINED 2   [7475471] seat 2 is 'Friend-B'
[7481653] stats: 29%, 2482 KB sent, 0.51 MiB/s    (6,3 s dopo l'entrata: 751 KB utili, 2482 KB sull'etere)
[7491664] stats: 88%, 6134 KB sent, 0.46 MiB/s    (16,3 s: 2279 KB utili, 6134 KB sull'etere)
[7492481] event PROGRESS 100%   [7492507] content delivered   [7492815] the client left with the app
[7493972] event ROOM_REMADE 0 (1,1 s dopo "re-making the room")   [7502592] la console seguente entra
```

Decisione dell'utente: in 3DS Link 1.0 il Download Play e' una **voce di menu a parte, "3DS Link Delivery"**; stanza da 2 (host + un posto), una console alla volta, la stanza si ricrea quando la console ha scaricato ed e' uscita. Questa prova e' il meccanismo vero (poi risultato: vedi ESITO; la "banda piena" si e' rivelata piu' lenta e il default e' `gap_us = 3000`). Dettagli e schermata server: `docs/3DSLINK-DELIVERY.md`.

Programma: `DlpLinkHostDemo.3dsx` (md5 `be8b3bba55c129ca09cf692d7c29d423`; la prova del 2026-10-20 era con `22178c6a…`, uguale tranne `gap_us` 3000 e una riga di log per console). Figlio: `child.cia` (la sonda, md5 `aa4fa116854cc91586621f79bcc590e6`).
Copiare su `sdmc:/3ds/DlpTest/` dell'host: `DlpLinkHostDemo.3dsx` e `child.cia` (la cartella la prepari tu). Togliere i vecchi `DlpRawHost*.3dsx`. **Nessun cambio di SD durante la sessione**: la modalita' si sceglie con il menu all'avvio (START per uscire e rilanciare dall'Homebrew Launcher).
Servono: l'host + almeno 2 console client (meglio 3, di cui una vecchia: C o D). Prima di ogni giro il client deve avere il figlio disinstallato (titolo `0004000100FD5100`; stessa versione installata = "nessun contenuto").

## Cosa fa il programma
Schermo alto: log (ogni evento della libreria). **Schermo basso: la schermata del server in testo semplice** (quella che l'app 3DS Link disegnera' in grafica): stato, titolo offerto, nome della console, posto, percentuale, MiB/s, tempo che manca, frammenti ripetuti, ultime console servite. **SELECT** = manda via la console che sta scaricando. **START** = chiude la stanza ed esce. Il log completo e' in `sdmc:/3ds/DlpTest/dlph_demo_log.txt` (scrive anche `nodes in the room: mask 0x....` a ogni cambio di nodi).

## Sequenza (premere A all'avvio)
1. Avvio: in basso `Room open: waiting for a console`.
2. **Console X**: Download Play → titolo → scarica. Guardare lo schermo basso dell'host: il **nome** di X compare entro 1-2 s? percentuale, MiB/s, tempo, frammenti ripetuti. Annotare dopo quanti secondi X ha finito e che il figlio si installa.
3. **Console Y subito dopo X**: cosa mostra Download Play mentre X scarica (stanza piena? titolo sparito?) e quanti secondi dopo l'uscita di X, Y vede il titolo e scarica, senza toccare niente sull'host.
4. **Console Z** (se c'e') come Y. Se una e' vecchia (C/D): nessuna richiesta di aggiornare il sistema.
5. **SELECT** mentre una console e' tra il 30% e il 70%: sparisce dalla stanza, `sent away` nella lista, la stanza si ricrea, la stessa console deve poter riprovare e finire. Annotare cosa mostra il client.
6. Se avanza tempo: allontanare/spegnere il Wi-Fi di un client a meta' scarico → dopo 9 s `re-making the room (the client was stuck)` e la console successiva viene servita.
7. START sull'host.

## Cosa mi serve
`sdmc:/3ds/DlpTest/dlph_demo_log.txt` + le annotazioni dei punti 2, 3, 5 (una riga ciascuna) + una foto dello schermo basso a meta' scarico.

## Esiti → decisioni (scritti prima)
| Esito | Decisione |
|---|---|
| Ogni console servita, tempo per 2,65 MB ≤ ~12 s, frammenti ripetuti < 15%, Y entra 1-4 s dopo l'uscita di X | **Meccanismo confermato**; la sessione di 3DS Link costruisce la UI e l'integrazione senza altre prove. (Esito reale: meccanismo confermato; a `gap_us = 0` 17 s e 2,4x ripetuti -> default 3000.) |
| Servite ma frammenti ripetuti > 25% o scarichi piu' lenti con `gap_us = 0` | **E' successo**: default `gap_us = 3000` (provato: ~10 s, 1% ripetuti). 1500 solo se serve ed e' misurato. |
| Schermo basso: `This console already has this version` (nessun avanzamento, in lista `(had it)`) | **Non e' un successo**: il figlio e' gia' installato su quella console (stessa versione = "nessun contenuto"). Disinstallare il figlio `0004000100FD5100` o alzare la versione e ripetere. Il programma lo dice apposta, per non confondere un falso verde con una consegna. |
| `refused by radio` alto (> 5% dei frammenti) | La coda radio si riempie: alzare `gap_us` e rimisurare; il rinvio selettivo copre le perdite ma costa tempo. |
| Il nome della console non compare mai (`name not known yet`) | La schermata mostra "Console" + numero del posto; il nome resta opzionale (`udsGetNodeInformation` non lo da' per i nodi Download Play). |
| Y entra ma resta "mezza collegata" fino a 10 s e la stanza viene ricreata da sola (`HANDSHAKE`, log `a console entered seat 2` senza `seat 2 is a Download Play console`) | Il posto non e' pulito dopo la ricreazione: aggiungere in `remake()` (dlph.c) una pausa fra `udsDestroyNetwork` e `udsCreateNetwork` e rimisurare. |
| SELECT lascia la console bloccata o la stanza non si ricrea | Il "manda via" della UI diventa "chiudi e riapri la stanza" (`dlph_stop` + `dlph_start`) finche' non e' risolto. |
| Console vecchia (C/D) chiede di aggiornare il sistema | Il beacon pulito non basta su quella versione: riaprire il caso (rapporto 07). |

## Facoltativo, solo se vuoi i dati per il futuro: giro B (stanza persistente, non in 1.0)
Premere B all'avvio: la stanza resta su (8 nodi) e un posto usato resta chiuso 45 s (`seat 2 was used 0 s ago: closed ...`). Servirebbe solo per una futura rete unica Download Play + Send Titles (`docs/DLP-HOST.md` punto 2bis): non fa parte della 1.0.

## Non ancora provato su hardware
* la stanza da **2 nodi** (finora 3 o 8; nel beacon `max nodes = 2`) e la libreria in se' (identica a `DlpTest/rawhost`, riorganizzata);
* (fatto il 2026-10-20) la banda piena: peggiore, vedi ESITO;
* il nome della console letto con `udsGetNodeInformation` sul nodo Download Play;
* il test su PC passa: consegna esatta, perdita 1% e 3%, client muto, file danneggiato rifiutato, nome/percentuale/velocita'/tempo/ripetuti/cronologia sulla schermata, console mandata via, e per B porta chiusa + quarantena.
