# Stremio Plus 0.5.20 — prova locale su PS5

Title ID **PPSA74126**, content version **00.070.000**. Questa è una build
destinata alla prova del proprietario, senza pubblicazione o invio allo store.
Le verifiche locali e i loro limiti sono in
[LOCAL_0_5_20_VALIDATION.md](LOCAL_0_5_20_VALIDATION.md).
Le verifiche della precedente build restano documentate separatamente in
[LOCAL_0_5_19_VALIDATION.md](LOCAL_0_5_19_VALIDATION.md).

## Installazione della cartella applicazione

1. Chiudi Stremio Plus. Conserva una copia dell'applicazione precedente e
   dei dati che vuoi mantenere, in particolare `/data/Stremio/appdata`.
2. Estrai `PPSA74126.zip`: deve risultare una cartella `PPSA74126` con
   `eboot.bin`, `sce_sys`, `sce_module` e gli asset, non un archivio sorgenti.
3. Copia la cartella completa nella posizione applicazioni usata dal tuo
   ambiente homebrew. Per una distribuzione come cartella, mantieni il
   permesso eseguibile su `eboot.bin` e `sce_module/libc.prx`.
4. Per il formato immagine, usa invece `PPSA74126.ffpfsc` con il tuo loader
   compatibile. Cartella ZIP e immagine appartengono alla stessa compilazione.
5. Conserva i simboli forniti con questa build. Quelli delle versioni
   precedenti, inclusa 0.5.19, non corrispondono al nuovo eseguibile.

L'app conserva Title ID e percorso dati. Non è necessario cancellare le
preferenze o i video scaricati per provare questi interventi.

## 1. Home senza lampeggi e riposizionamenti ripetuti

Avvia l'app con un account già collegato. La pagina di caricamento deve
restare visibile mentre si preparano i primi scaffali, i dati iniziali
dell'account e le immagini visibili. Una volta aperta la Home, gli altri
cataloghi devono arrivare senza far scomparire e riapparire tutti i poster.

Prova anche con un add-on lento o non raggiungibile. L'attesa iniziale ha un
limite massimo di **8 secondi**; gli add-on lenti possono completarsi dopo
l'apertura. Se non è arrivato alcun contenuto, viene resa accessibile la
pagina ancora in caricamento. Il limite non implica che la rete debba aver
finito entro otto secondi.

Mentre arrivano altri scaffali, spostati su un titolo già caricato. Verifica
che il titolo selezionato e la sua posizione sullo schermo restino stabili,
anche quando compare lo scaffale Continua a guardare. Un aggiornamento
esplicito dei cataloghi resta un nuovo caricamento richiesto dall'utente.

## 2. Login indipendente dal browser del telefono

Le sessioni già salvate con il vecchio procedimento non vengono sostituite
di nascosto: **effettua una nuova associazione una volta** per verificare il
nuovo flusso.

1. Esci dall'account nelle impostazioni della PS5 e associa nuovamente l'app
   con il codice/QR, approvandolo nel browser del telefono.
2. Attendi che la Home e la libreria siano disponibili sulla PS5.
3. Esegui il normale logout dal solo browser del telefono.
4. Sulla PS5 apri la libreria, verifica una modifica ai preferiti e torna
   alla Home. Chiudi poi l'app completamente e riaprila.
5. La PS5 deve rimanere associata al proprio account. Un comando del servizio
   che revoca tutte le sessioni sarebbe una prova diversa dal logout del
   solo browser.

Il test locale verifica richiesta `loginWithToken`, salvataggio immediato
del token restituito, riavvio, errori, annullamento e scadenza con risposte
API controllate. La sequenza sopra verifica il comportamento effettivo del
servizio, che non è stato simulato come prova reale.

## 3. Microscatti a 1080p: confronto con i nuovi log

I log di `Downloads(4).zip`, registrati con la build **0.5.19**, mostrano una
sorgente H.264 a **25 fps**. VLC consegna i fotogrammi ogni **40 ms**, mentre
l'app ne presenta circa **22,3 al secondo**: l'upload/conversione richiede in
media circa **42,94 ms**, oltre i 40 ms disponibili per ciascun fotogramma.
Nelle sette finestre complete analizzate risultano **559 fotogrammi scartati
nell'uscita**. In questa prova il decoder è software, ma la consegna regolare
a 25 fps colloca il rallentamento osservato dopo la decodifica. Il firmware
non è registrato nei log: **12.7 è il firmware segnalato dall'utente**, non
un dato verificato automaticamente in questa sessione.

La **0.5.20** modifica il caricamento dei piani video usando texture
`GL_TEXTURE_RECTANGLE`, per evitare la riorganizzazione per pixel nella CPU
individuata nel driver. Aggiunge anche un tentativo hardware AVC con livello
massimo 51 dopo il rifiuto della configurazione precedente, soltanto per
stream con profilo, livello e dimensioni compatibili e noti. Questo tentativo
non garantisce l'attivazione del decoder hardware. I dettagli e i limiti
sono in [LOCAL_0_5_20_VALIDATION.md](LOCAL_0_5_20_VALIDATION.md).

**Il miglioramento deve ancora essere misurato sulla PS5.** Per confrontare
la nuova build con quella dei log:

1. Riproduci **lo stesso video 1080p per almeno cinque minuti**, mantenendo
   la stessa sorgente, la stessa uscita video e lo stesso percorso audio.
   Annota firmware effettivo, risoluzione e frequenza di uscita impostate.
2. Segna il tempo del film in cui percepisci eventuali scatti. Dopo il tratto
   continuo, prova pausa/ripresa e uno spostamento avanti e indietro.
3. Ferma il video, torna alla Home e riaprilo, verificando sia **Riproduci
   dall'inizio** sia **Riproduci da m:ss**. Chiudi completamente l'app e
   ripeti l'apertura per controllare anche il riavvio.
4. Passa da un video 1080p a uno 4K e poi di nuovo al 1080p, chiudendo ogni
   riproduzione prima della successiva. Controlla che la nuova immagine sia
   corretta e che il ritorno alla Home resti utilizzabile. Questi passaggi
   verificano anche riallocazione e rilascio delle superfici video.
5. Se possibile, confronta anche una copia locale completa dello stesso
   contenuto: aiuta a distinguere le attese di rete dalla presentazione.
   Cambia una sola condizione alla volta.

I log distinguono:

- `decoder=ps5-hardware`, `software` o `pending`: selezione effettiva
  rilevata; `hardware_available` è soltanto disponibilità.
- `software_fallback`: riapertura forzata in software dopo il tentativo
  hardware; non identifica qualsiasi selezione iniziale software.
- `source_fps`, `decoded`, `vout_lost_pictures` e `vmem_drops`:
  frequenza della sorgente e contatori di stadi diversi.
- `player/vlc cadence`: intervalli e ritardi dei callback e dell'acquisizione
  dei fotogrammi da parte dell'app.
- `presentation timing`: intervalli del ciclo principale, caricamento video,
  attesa dello swap e intervalli tra swap che hanno accettato un nuovo frame.
- `video upload timing`, con `path=rectangle-planes`: separa allocazione,
  trasferimento dei piani (`plane_transfer`) e invio della conversione
  (`conversion_submit`). Questi dati sono registrati anche nell'oggetto
  `presentation_timing` di `runtime.json`, con conteggi e istogrammi.
- `source_fps` in `runtime.json`: frequenza nominale dello stream;
  `accepted_frame_fps`: cadenza filtrata ricavata dai timestamp dei frame
  accettati, distinta dalla frequenza del display.

Confronta soprattutto **tempo di upload, cadenza di acquisizione e scarti**:
il risultato atteso sullo stesso 1080p a 25 fps è un upload inferiore al budget
di 40 ms, un'acquisizione vicina ai 25 fps e la scomparsa della perdita
sistematica osservata. Leggi sempre il decoder effettivo: il tentativo AVC
può riuscire oppure mantenere il ripiego software. Entrambi i casi devono
restare diagnosticabili, senza attribuire il risultato alla sola presenza
del modulo hardware.

Nei passaggi 1080p/4K e nelle riaperture conserva anche le righe di memoria:
controlla eventuali errori di allocazione o rilascio, rifiuti di fotogrammi e
una crescita che prosegue a ogni ripetizione. Non è richiesto che ogni valore
torni subito a zero: alcune riserve vengono mantenute per il riuso. Le
protezioni precedenti contro i crash restano attive e la loro tenuta deve
essere verificata anche in una sessione prolungata.

I tempi di ritorno dallo swap non misurano direttamente la scansione HDMI
del monitor. Anche `plane_transfer` e `conversion_submit` sono tempi CPU
attorno alle chiamate grafiche, comprensivi di eventuali attese del driver;
non sono misure del completamento GPU. Nei log precedenti il cambio di
frequenza risultava rifiutato: una cadenza non uniforme dovuta al rapporto
fra 25 fps e frequenza del display può rimanere distinta dagli scarti causati
dall'upload. I contatori aiutano a separare questi fenomeni, ma non
costituiscono da soli una diagnosi del firmware o una prova di risoluzione.

## 4. Volume con la levetta destra

Durante un video con uscita PCM, riporta inizialmente la levetta destra al
centro. Spostala **su** per aumentare e **giù** per diminuire il volume.

- Ogni scatto vale **5%**, con intervallo **0–100%**.
- La zona morta e l'isteresi evitano modifiche dovute a piccole oscillazioni.
- Tenendola inclinata, la ripetizione inizia dopo 350 ms e prosegue ogni
  120 ms, senza recuperi a raffica dopo un rallentamento dell'interfaccia.
- L'indicatore scompare dopo circa **1,8 secondi**.
- Il livello viene conservato alla riapertura dell'app; quello iniziale è
  100%. I suoni dell'interfaccia hanno ancora il proprio volume separato.
- Dopo disconnessione del pad o apertura dell'interfaccia di sistema serve
  riportare fisicamente la levetta al centro prima di una nuova variazione.

Verifica anche **L2/R2**: devono continuare a regolare il ritardo dei
sottotitoli durante la riproduzione e l'ordinamento nella pagina Download.

### Passthrough HDMI

Quando l'uscita bitstream è realmente attiva, il flusso codificato non passa
per il gain software. La levetta mostra l'indicazione di usare il volume di
TV/ricevitore, senza una percentuale fittizia e senza modificare il livello
PCM salvato. Per il volume nell'app, disattiva il passthrough e usa PCM.

Se il ricevitore o la traccia causano un fallback a PCM, il controllo
software deve tornare disponibile. La sola preferenza “passthrough” non
blocca il volume quando l'uscita effettiva è PCM.

## 5. Scelta prima di ogni riproduzione

Con un film o episodio interrotto, seleziona una sorgente: prima di aprire
il player deve comparire la scelta **Riproduci dall'inizio** / **Riproduci
da m:ss**, con la posizione salvata già selezionata. Conferma l'inizio e
controlla che il video parta da zero; ripeti scegliendo la posizione salvata.

La stessa domanda deve apparire dalla pagina Download, usando una sorgente
già scaricata e al passaggio all'episodio successivo online o scaricato.
Per un episodio mai visto, le azioni sono inizio e annullamento: non viene
riutilizzata la posizione dell'episodio precedente. Le dirette offrono
**Avvia diretta** e annullamento, perché non hanno un inizio riavvolgibile
garantito.

Premi **Cerchio** mentre la scelta è aperta: il player non deve avviarsi e
la posizione salvata deve rimanere disponibile al tentativo successivo.
Verifica anche che due pressioni rapide di conferma non aprano due player.

Per un download incompleto, la ripresa resta disponibile soltanto quando
il punto già visto appartiene alla parte scaricata e verificata della stessa
sorgente. Se questa condizione cambia mentre il dialogo è aperto, l'app
deve avvisare e richiedere una nuova scelta, senza ripartire da zero da sola.

Questa build conserva le precedenti regole di validità del segnalibro:
stesso episodio, tempo finito oltre cinque secondi e prima della fine.
Il progresso vicino alla conclusione può già essere segnato come terminato.

## Log utili per l'esito della prova

Conserva i file aggiornati di `/data/Stremio/appdata/logs`, in particolare
`log.txt`, `runtime.json`, `session.json` ed eventuali file di avvio/crash.
Accompagnali con versione app, firmware effettivo, file/codec/fps,
risoluzione/frequenza di uscita, percorso audio e istante del difetto.
Un breve video della Home è utile per i lampeggi.

La compilazione e i test host non equivalgono a un collaudo sulla console.
La pubblicazione resta sospesa fino alla prova richiesta dall'utente.
