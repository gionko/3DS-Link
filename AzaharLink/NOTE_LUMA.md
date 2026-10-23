# Note: versioni di Luma3DS e Plugin Loader

Appunto del 2026-10-17, scritto mentre l'utente prova la build `dist/3DSLink.3dsx` 318f4f5f su quattro console. **Solo appunti: nessuna modifica al codice.**
Regola di partenza: **3DS Link non modifica né installa Luma**; con la Luma che c'è sulla console si lavora.

## In due righe

* Il **Plugin Loader** (i plugin `.3gx`, quindi anche `AzaharLink.3gx`) esiste nella Luma3DS **ufficiale solo dalla v13.0 (2023-07-20)**. Prima, nella Luma ufficiale, non c'è: nemmeno il servizio `plg:ldr`.
* Sulle due console con Luma **10.1.1** e **10.2.1** l'app scrive **`unknown`** accanto a *Plugin Loader* (confermato dall'utente): è il servizio che manca, non l'interruttore spento. Lì il plugin non può essere caricato da nessun titolo, qualunque cosa faccia l'app. L'unica cura è una Luma ≥ 13.0, e Luma non la tocchiamo.

## Le quattro console (si riconoscono dalla riga `versioni:` di `3DSLink-TitleDelivery.log` / `log.txt` di TitleShare)

Le lettere `dsa`…`dsd` di TitleShare **cambiano da una prova all'altra**: si distinguono dall'impronta (sistema + Luma + dimensione della SD + orologio).

| Console | Modello | Sistema | Luma | SD | Orologio | Plugin Loader |
| --- | --- | --- | --- | --- | --- | --- |
| **A** (quella dell'utente, «Console-A», MAC `DC:68:EB:xx:xx:xx`) | New 2DS XL | 11.17.0-50E | **13.3.3** | 119,07 GiB | giusto | c'è, acceso (il plugin ci ha girato) |
| **B** | New 2DS XL | 11.17.0-50E | **13.3.3** | 59,46 GiB | giusto | c'è (Luma ≥ 13); stato mai registrato |
| **C** (vecchia di proposito) | New 2DS XL | 11.13.0-45E | **10.1.1** | 59,61 GiB | **2011** di proposito | **non c'è** (`unknown`) |
| **D** (vecchia di proposito) | New 2DS XL | 11.15.0-47E | **10.2.1** | 28,80 GiB | **2011** di proposito | **non c'è** (`unknown`) |
| Friend-A (amico, da internet) | New 3DS XL | non registrato | non registrata (≥ 13.0, il plugin ci ha girato) | 128 GB | — | c'era, all'inizio spento (acceso da Rosalina) |

Non esiste nessuna Old 3DS / Old 2DS tra le console provate. L'orologio al 2011 su C e D è voluto: non correggerlo.
Dove si legge la versione: la riga `versioni: sistema … | Luma 0x0d030300 | kernel … | firm …` (Luma = `svcGetSystemInfo(0x10000, 0)`, si decodifica `major<<24 | minor<<16 | build<<8`: `0x0d030300` = 13.3.3, `0x0a020100` = 10.2.1, `0x0a010100` = 10.1.1). I log del **plugin** (`AzaharLink.log`) non contengono né la versione di Luma né il modello.

## Quando è nato il Plugin Loader (fonti primarie: tag e sorgenti di LumaTeam/Luma3DS, controllate due volte)

| Luma | Data | Cosa cambia per il Plugin Loader |
| --- | --- | --- |
| 8.0 | 2017-06-05 | Nasce Rosalina (menu `L+Giù+Select`): è l'involucro in cui poi vive `plg:ldr`. Nessun plugin. |
| 10.1.1 / 10.2.1 | 2020-04-27 / 2020-11-17 | **Nessun Plugin Loader, nessun `plg:ldr`.** (Le due console C e D.) |
| 11.0 | 2022-06-04 | La configurazione passa a `/luma/config.ini` (dove poi starà `plugin_loader_enabled`). |
| 12.0.1 | 2023-03-28 | Ultima Luma ufficiale **senza** Plugin Loader. |
| **13.0** | **2023-07-20** | **Prima Luma ufficiale con il Plugin Loader** (unione del fork di PabloMK7 e Nanquitas; le note citano CTGP-7). Menu Rosalina: 4ª voce `Plugin Loader: [Disabled]`. **Spento di default**, e lo stato resta in `config.ini`. |
| 13.1 | 2024-05-03 | Nuove bandiere nell'intestazione 3GX; un plugin marcato «solo emulatore» viene rifiutato (da controllare che il nostro `.3gx` non lo sia: finora è girato solo su Luma 13.3.3). |
| 13.2 | 2024-09-27 | Il plugin può usare memoria PRIVATA invece che condivisa (utile per la rete). |
| 13.3.3 | 2025-07-15 | Quella dell'utente sulle console A e B (provata). |
| **13.4** | **2026-04-02** | Ultima versione ufficiale. Sul ramo principale c'è già, non rilasciata, una correzione (`plgldr: Fix plugin bugs related to process termination`, 2026-10-14) che riguarda proprio i casi in cui l'evento di uscita del plugin non arriva: da guardare con il crash di uscita di Tri Force Heroes. |

Dettagli che contano per noi:

* I comandi che usiamo (`2` = «è acceso?», `3` = «accendi/spegni») sono **identici in tutte le versioni** che hanno il Plugin Loader (anche nel fork non ufficiale `Luma3DS_3GX` dal 2018): niente da adattare per versione.
* **Salire di versione di Luma da < 13 a ≥ 13 lascia il Plugin Loader spento** (il formato della configurazione cambia e viene riportata ai valori di base): al primo avvio dopo l'aggiornamento l'app dirà `Disabled` e proporrà di accenderlo (come già fa).
* Esiste un fork non ufficiale (`PabloMK7/Luma3DS_3GX`) con numeri di versione 10.x–12.x che **ha** il servizio: su una Luma di quei numeri l'app mostrerebbe `Enabled`/`Disabled`, non `unknown`. Quindi il numero di versione da solo non basta: vale la parola che mostra l'app.
* `unknown` può comparire anche con una Luma ≥ 13 se la sola sessione di `plg:ldr` è occupata (il nostro plugin, via CTRPluginFramework, la tiene mentre un titolo col plugin gira). Rischio basso: l'app gira da sola, a titoli chiusi. Non verificato su hardware.
* Il nostro `AzaharLink.3gx` è provato **solo su Luma 13.3.3** (e su quella di Friend-A, ≥ 13). Su 13.0/13.1 (sola memoria condivisa, intestazione senza le bandiere nuove) non è mai stato provato.
* L'aggiornamento di Luma, oggi, è a mano: si copia `boot.firm` (e `boot.3dsx`) dalla release sulla radice della SD. La wiki di Luma dice che gli **aggiornatori automatici non sono supportati**.

## Decisioni dell'utente (2026-10-17, sera)

* **La versione di Luma va nel controllo dell'installazione** (punto 1 qui sotto): riga «Luma3DS» accanto a *Plugin Loader*, con la versione e il verdetto a tre colori: **rosso «not work»** sotto la 13.0 (Plugin Loader inesistente; *Titles Delivery* funziona), **arancione «maybe»** 13.0-13.2 (mai provata), **verde «tested»** dalla 13.3 (provata la 13.3.3). Il nostro plugin usa memoria **condivisa** (`UsePrivateMemory: false`), l'unica che c'è in 13.0 e 13.1: nessun motivo noto per cui non vada. Piano in `NOTE_PROSSIMO_GIRO.md`, punto 13. Una tabella «che cosa funziona con quale Luma» va anche in `README.md` e `dist/LEGGIMI.txt`.
* **Aggiornare Luma da 3DS Link (punto 2): messo da parte.**
* **Controllo sul PC della compatibilità con 13.0-13.2: NON si fa** (l'utente ha fermato l'analisi: l'informazione si ricava dal registro delle modifiche, tabella qui sopra; chi aggiorna Luma ha comunque la versione nuova). La riga dell'app dice «dovrebbe funzionare, mai provato».
* **Provare 13.0 / 13.1 / 13.2 (punto 3): non si fa** (non c'è il tempo per la release); la Old 3DS / Old 2DS resta non provata e non in programma.

## Da fare, più avanti (il punto 1 è deciso per la prossima build; gli altri no)

1. **Dire chiaro, nell'app, quando la Luma è troppo vecchia**: con `unknown` oggi l'*Installation check* non ha nessuna riga e l'utente non sa perché. Leggere e mostrare la versione di Luma (`svcGetSystemInfo(0x10000, 0)`), e scrivere «la Luma di questa console (10.2.1) è anteriore alla 13.0: il Plugin Loader non esiste». *Titles Delivery* non dipende dal Plugin Loader e funziona anche su C e D.
2. **Aggiornare Luma da 3DS Link** (idea dell'utente, lontana). Il mod di Mario Kart 7 che l'utente ricorda come «CTR7» è quasi certamente **CTGP-7** (Custom Track Grand Prix 7, di PabloMK7, lo stesso autore del Plugin Loader di Luma e di Azahar): **da confermare con l'utente**. Cosa si è trovato:
   * Il mod e il suo lanciatore attuale sono **chiusi**; è aperto solo un'istantanea archiviata del 2021 (`PabloMK7/CTGP-7_Launcher`, MIT; installa una Luma con Plugin Loader copiando `boot.firm` sulla radice della SD, con copia di sicurezza `.bak`, **senza nessun controllo di integrità**, e poi riavvia). Il registro delle modifiche del mod conferma che il lanciatore aggiorna Luma (ha seguito ogni versione, fino alla 13.4); il codice vero di oggi non si può leggere.
   * Gli aggiornatori di Luma davvero aperti: `KunoichiZ/lumaupdate` (WTFPL: versione installata, ultima release da GitHub, controllo di dimensione, copia `.bak`, estrazione di `boot.firm` dallo zip, riavvio) e `PabloMK7/EzB9SUpdater` (MIT: scarica in una cartella temporanea, prepara, riavvia; rifiuta una Luma molto vecchia). Anche **Universal-Updater** installa già Luma (voce `luma3ds` nel suo catalogo).
   * Idee da copiare quando sarà il momento: leggere la versione con `svcGetSystemInfo(0x10000, 0)`; trovare il file da sostituire con la richiesta «percorso del firm avviato» (`0x400 + 8·i`, Luma ≥ 13.0) invece di supporre `sdmc:/boot.firm`; l'ultima release da `api.github.com/repos/LumaTeam/Luma3DS/releases/latest` (le risorse hanno dimensione e SHA-256); scaricare in un file temporaneo, controllare dimensione, SHA-256 e il segno `FIRM`, poi spostare il vecchio in `.bak` e il nuovo al suo posto; riavviare solo dopo un tasto; **tenere acceso il controllo dei certificati TLS** (CTGP-7 ed EzB9S lo spengono: non copiarlo); non toccare la NAND.
   * Avvertenze: **licenze** (il progetto è «GPL v2 o successiva»: il codice MIT/WTFPL si può riusare citando; quello GPL-3.0 di Luma no, se non si passa alla GPL-3; i repository senza licenza, come `CTGP-7updates`, non si copiano); **rischio** (un `boot.firm` sbagliato lascia la console senza CFW finché non si sistema la SD dal PC); nessun aiuto dai manutentori di Luma; il nostro plugin andrebbe dichiarato compatibile per ogni versione di Luma (CTGP-7 ha dovuto farlo a ogni uscita).
3. **Provare 13.0 / 13.1 / 13.2** (le prime con Plugin Loader) con il nostro `.3gx`, se si vuole dichiarare quali Luma sono supportate; e una **Old 3DS / Old 2DS** (già in lista).

Le fonti, con i link (tag, commit, release), sono i tag, i commit e le release del repository LumaTeam/Luma3DS.
