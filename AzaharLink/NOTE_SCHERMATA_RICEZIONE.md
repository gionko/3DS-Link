# Note: azioni per titolo sulla schermata di ricezione (Receive Titles, avanzamento)

Appunto del 2026-10-17. **Solo analisi di fattibilità, nessun codice** (l'utente prova ancora la build `dist/3DSLink.3dsx` 318f4f5f). Fonti: lettura del codice del modulo (`receiver.c`, `sender.c`, `store.c`, `sched.c`, `proto.c`) e delle schermate (`delivery_run.cpp`), con un secondo revisore scettico (alcune correzioni sono già incluse qui sotto).

## Decisione dell'utente (2026-10-17, dopo aver letto l'analisi)

* **Togliere un titolo in waiting: NON si fa** (difficoltà media, non gli piace). **Aggiungere un titolo dimenticato: NON si fa.** Le righe 2 e 3 qui sotto restano solo come analisi.
* **«Cursore»**: non era il puntatore del mouse, voleva dire la **riga evidenziata** che si sposta con Su/Giù (come il ricevente evidenziato in *Send Titles*); non c'è nessun puntatore. Con le due azioni scartate la riga evidenziata serviva solo per il **popup informativo** su un titolo installato (ID, titoli collegati): **scartato anche quello** (2026-10-17), quindi nessuna riga evidenziata. Il **progresso dell'installing** (quanto manca) non ha bisogno della riga evidenziata e può comparire direttamente sulla riga.
* Che una **DLC sia inutile senza il suo gioco** è certo per l'utente: non serve altro.

## Cosa vuole l'utente

Sulla schermata di avanzamento della ricezione (lo schermo **basso** elenca i titoli: installed / installing / waiting, poi «and N more: M installed»; i tasti sono solo B *Cancel* e SELECT *Screens off*):

1. un **cursore** sui titoli;
2. su un titolo **in waiting**, A = **toglierlo** dalla lista (ha cambiato idea; «il server non dovrebbe cambiare niente»); un *installed* non si annulla, un *installing* non si tocca;
3. un pulsante nuovo tra *Cancel* e *Screens off* per **aggiungere un titolo dimenticato**, richiesto e gestito da solo (il ricevente ha ancora l'elenco offerto?);
4. A su un **installed** = **popup** con l'ID e i **titoli collegati** (gioco + DLC/aggiornamento: «DLC installata ma il gioco è ancora in waiting, non toglierlo»);
5. per l'**installing**, se si può, **quanto manca**.
Lo schermo **alto** non va toccato (è già pieno): tutto va sul basso.

## Risposte alle domande

* **Il ricevente ha ancora la lista che ha mandato il mittente?** Sì: l'elenco intero (fino a 16 titoli, con id, dimensione, tipo, nome) resta in memoria per tutta la ricezione e arriva a ogni aggiornamento delle schermate; non cambia durante la stanza.
* **Se tolgo un titolo, il mittente deve cambiare qualcosa?** **No.** Il mittente sa già che un titolo che sparisce dal riepilogo del ricevente (ogni 120 ms) è «finito» e smette di mandarglielo: togliere un titolo, per lui, è identico a un titolo finito. Funziona con il mittente di oggi e con ricevitori di oggi. Effetto collaterale solo estetico: per quel ricevente il titolo tolto conta sul mittente come «scaricato al 100% ma da installare», quindi la sua percentuale e i suoi minuti sullo schermo del mittente (e di chi legge l'elenco dei riceventi) restano un po' falsati finché non finisce.
* **Quanto manca per installare un titolo?** Si può sapere: un CIA si scrive in ordine, quindi byte scritti / dimensione. Versione solo-app dai totali già disponibili (precisa se nessun titolo è finito in errore); versione esatta con circa 10 righe nel modulo. Limiti veri: un titolo grande **resta a 0% fino a ~22 s** mentre AM prepara lo spazio, ~0,5 s a 100% alla fine, e può fermarsi se il mittente sta servendo altri. «Quanto manca» di un titolo solo è una stima, non una misura.
* **Titoli collegati**: nel manifesto un gioco, il suo aggiornamento e la sua DLC hanno lo **stesso nome** e lo stesso identificatore di base (`(tid & 0xFFFFFFFF) >> 8`); quindi si può dire «DLC installata, il suo gioco è in waiting» e anche «il gioco è già installato su questa console» (l'app conosce i titoli installati). Che una DLC sia inutile senza il suo gioco è conoscenza generale del 3DS, **non** provata qui: il codice della console (AM) non impone nessun ordine né dipendenza (l'ordine reale di installazione lo decide il mittente e ciò che arriva).

## Difficoltà, desiderio per desiderio

| Desiderio | Difficoltà | Dove | Note |
| --- | --- | --- | --- |
| 1. Cursore sui titoli (7 righe visibili, finestra fino a 16) | **piccola** | solo app | circa 70 righe; le schermate si aggiornano ~6 volte al secondo, quindi cursore senza animazioni e al più ~6 mosse al secondo; i controlli di testo delle pin su «and N more» vanno rifatti |
| 4. Popup su un *installed*: ID, tipo, dimensione, quando installato, titoli collegati con il loro stato | **piccola** | solo app | tutti i dati ci sono già; da fare in un piccolo file di logica con test su PC (come `delivery_eta`) |
| 5. Progresso dell'*installing* | **piccola** (app) / **piccola** (esatto: ~10 righe in `receiver.c/h` + ponte) | app, poi modulo | approssimato ma onesto; «waiting for data» se il mittente serve altri |
| 2. Togliere un titolo in *waiting* | **media** | app + ponte + `receiver.c/h` (già patchati) | nessun cambio di protocollo né di mittente, nessun file nuovo da patchare; circa 40–60 righe nel modulo: una richiesta dalla schermata, eseguita **dal thread installatore** (non dalla schermata, c'è una gara tra «scelgo il titolo» e «lo installo»: un errore può cancellare una copia già installata), da controllare a **ogni** giro del thread e non solo quando è libero; il titolo che sta già installando si rifiuta («too late»); va corretto il conteggio dei blocchi da scaricare; test su PC solo in parte |
| 3. Aggiungere un titolo dimenticato | **grande, sconsigliato ora** | modulo + mittente | il mittente **ignora** un cambio di titoli voluti dopo il primo riscontro: servirebbero patch a `sched.c` e `store.c` (mai toccati, oggi vietati dal controllo del modulo), un segnale «il mittente lo sa fare» (con un mittente vecchio fallirebbe **in silenzio**), un lavoro sulla fine del thread installatore, un controllo dello spazio e una pagina nuova. Alternativa che c'è già: **B e rientrare nella stanza**, i titoli installati sono già deselezionati. Costo: B annulla l'installazione in corso |
| Fermare l'*installing* | piccola in codice, **sconsigliata** | — | l'installazione **cancella prima la copia vecchia** del titolo (AM): annullare a metà lascia né la vecchia né la nuova; l'annullamento a metà non è mai stato visto in un log |

Ordine consigliato: **1 + 4 + 5 insieme** (solo informazioni, nessun rischio per il trasferimento), poi **2**, e **3 nella lista delle cose future**. Un'ottima variante «gratis» di 3: poter **rimettere** un titolo tolto per errore (il mittente tiene ancora il bit): non costa niente sul filo.

## Attenzioni di interfaccia (schermo basso)

* Lo spazio: 7 righe + una riga di servizio; il suggerimento «A details» va sulla stessa riga di «and N more: M installed».
* Il footer ha già due bottoni; un terzo si restringe (il bottone con SELECT diventa più piccolo ma leggibile); un quarto no.
* **Doppio A**: A apre il popup su un titolo in waiting e un secondo A lo conferma: ignorare i tasti per ~400 ms dopo l'apertura, il bottone si chiama *Remove*, B = *Keep*.
* Se il titolo passa da waiting a installing con il popup aperto, il popup diventa informativo.
* I messaggi dell'esito («too late») vanno su una riga dello schermo basso (i messaggi in alto non esistono su questa schermata e non si toccano).
* Test: le schermate non si compilano sul PC; si coprono con le pin del controllo, i mutanti, la build demo nell'emulatore e una prova su console. Si vogliono estratti in un file piano i calcoli (progresso, collegati, spazio, finestra del cursore) per provarli sul PC.
