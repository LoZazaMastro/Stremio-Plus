# Stremio Plus 0.5.20 — verifica locale e microscatti 1080p

Data: 23 ottobre 2026. Title ID `PPSA74126`, content version `00.070.000`.
Candidato locale da provare sulla PS5 del proprietario. Nessun branch, commit,
workflow, release, richiesta allo store o pubblicazione è stato creato.

Questa versione comprende la scelta esplicita di avvio e la correzione del
percorso di caricamento video individuato dopo l'arrivo di `Downloads(4).zip`.
Mantiene le correzioni a Home, sessione account e volume della precedente
build 0.5.19.
Lo snapshot `POST_0_5_18_WORK_IN_PROGRESS.md` è storico e non descrive i test
successivamente eseguiti.

## Cosa mostrano realmente i nuovi log

La sessione corrente è `1a169d41e94-d2`, avviata il 23 ottobre alle 17:34:31 UTC,
con la build 0.5.19. I file contengono anche sessioni precedenti: i dati sotto
sono filtrati per la sola sessione corrente, senza sommare log.txt ed events.
Il firmware della console non è registrato; il riferimento a 12.7 viene dalla
segnalazione dell'utente, non da un'identificazione automatica in questi log.

| Misura | Riscontro nella sessione corrente |
| --- | --- |
| Sorgente | H.264, 1920 × 1080, 25 fps, 8 bit SDR |
| Decoder effettivo | `software`; il modulo hardware è disponibile ma rifiuta la query della memoria |
| Rifiuto hardware | `0x811d0200`, livello configurato 52, pipeline 4 e poi 1 |
| Consegna VLC | Circa 40 ms, quindi 25 fps, in sette finestre complete |
| Prelievo/presentazione app | 22,26–22,37 fps; circa 44,7–44,9 ms |
| Callback / prelievi | 5.253 / 4.694 nelle sette finestre |
| Scarti nell'uscita | 559, circa 10,64%; `vmem_drops` finale 559 |
| Scarti interni VLC vout | `vout_lost_pictures=0` |
| Upload/conversione nell'app | 42,91–42,95 ms medi per fotogramma nelle finestre stabili |
| Attesa per il ritorno dallo swap | Circa 1,46 ms |
| Audio | E-AC-3 in passthrough, `active_eac3` |
| Output richiesto | 3840 × 2160; cambio frequenza rifiutato, `mode_refused`, codice 3 |

Un film a 25 fps dispone di 40 ms per fotogramma. L'upload da solo supera quel
budget: il decoder software fornisce regolarmente i fotogrammi, ma l'app non
li consuma abbastanza rapidamente. Questo individua il collo di bottiglia
nel percorso successivo alla decodifica; non dimostra che la CPU sia troppo
lenta a decodificare né che il solo mancato uso del decoder hardware sia la
causa dei 559 scarti.

I vecchi campi `runtime.source_fps=0` e `video_processing.source_fps≈23,18`
non descrivono correttamente la frequenza nominale: il primo rimaneva al
valore iniziale e il secondo derivava dai timestamp dei frame accettati.
La versione 0.5.20 li corregge e distingue frequenza nominale, cadenza accettata
e ritorni dallo swap.
`log_dropped_events` conta messaggi diagnostici, non fotogrammi video.
`clean_shutdown:false` da solo non prova un crash e `crash-last.txt` conserva
la storia precedente. Non sono stati rimossi i sistemi di cattura dei crash.

## Correzione dell'upload video

Nel driver OpenGL effettivamente usato, con SDK fissato alla versione 1.0.0 / commit
`dce74910af7f4274c48d05e0ae001b3038462588`, Mesa assegna ai formati colore
renderabili sia il bind sampler sia il bind render-target. Il driver sceglie
quindi un'immagine 2D con layout tiled anche per i piani R8/RG8 che l'app
campiona soltanto. Durante il caricamento la CPU esegue una copia per texel
nel layout tiled.

La modifica usa **GL_TEXTURE_RECTANGLE per i soli piani in ingresso**, che
in questo driver mantengono memoria lineare. Lo shader usa `sampler2DRect`
con coordinate scalate sulla dimensione di ciascun piano. Il risultato RGBA
rimane una texture 2D alla risoluzione della sorgente, compatibile con la UI.
Non viene aggiunta una seconda presentazione VideoOut né cambiato l'SDK.

Riferimenti riproducibili nei sorgenti inclusi nell'archivio SDK della cache:

- Mesa 26.2.0 `src/mesa/state_tracker/st_cb_texture.c`: `default_bindings` e
  `gl_target_to_pipe`.
- Driver PS5 `src/gallium/ps5/ps5_screen.c`: `ps5_linear_sampled_layout`,
  `ps5_transfer_map`, `ps5_transfer_unmap`, `ps5_texture_subdata`.
- Applicazione: `src/video_gl.cpp` e `src/video_gl.h`.

Restano validi i limiti su dimensioni, pitch e piani, il controllo degli errori,
l'allocazione dell'output più grande prima dei piani, il distacco dell'FBO prima
di cancellare le texture, il rilascio delle allocazioni parziali e il mantenimento
dell'ultima immagine valida su descrittori non validi. Il modello dei test
conta anche lo staging che il driver può riservare per il bind render-target;
non assume erroneamente che RECT elimini ogni riserva di memoria.

È una correzione del percorso individuato nel codice e dai tempi applicativi.
**Non è ancora una misura del nuovo tempo di upload sulla PS5.** Per verificarla
la build registra ogni 30 s `allocation`, `plane_transfer` e `conversion_submit`
oltre a loop/upload/swap. Sono tempi CPU attorno alle chiamate GL, comprensivi
di eventuali attese nel driver: non misurano il completamento GPU o lo scanout HDMI.
Non aggiungono fence, query GPU o `glFinish` al ciclo di riproduzione.

## Compatibilità del decoder hardware

Il riferimento EVO allegato documenta un test upstream AVC 1080p con profilo 100,
livello massimo 51 e pipeline 1. È stato aggiunto un solo tentativo 52 → 51 dopo
il rifiuto della query, limitato a flussi con profilo 66/77/100, livello noto
maggiore di zero e ≤ 51, e dimensioni codificate note ≤ 1920 × 1088. Il tentativo
iniziale con livello 52 e pipeline 4 e il precedente retry con pipeline 1
restano presenti.

Non si riducono livelli ignoti o superiori, non si confonde l'altezza 1090 del
buffer VLC con il bitstream e il nuovo retry non scatta su errori di allocazione
o creazione. ABI, memoria, coda preparata prima del cambio delle credenziali, pool,
ownership, cleanup e comportamento HEVC precedente sono conservati.
Il significato specifico del codice `0x811d0200` non è stato inventato.
Il successo della nuova configurazione sulla console è ancora da verificare.

Il modulo `vendor/vlc-ps5/modules/ps5vdec.c` è compilato nell'app. Gli archivi
statici VLC e le ricevute delle dipendenze restano validi e sono stati verificati,
senza riscriverne arbitrariamente i checksum.

## Scelta prima di ogni riproduzione

Ogni richiesta di riproduzione mostra una scelta prima di aprire il player.
Se esiste un segnalibro valido per quel film/episodio, le azioni sono
**Riproduci dall'inizio** e **Riproduci da m:ss**, con la seconda selezionata.
Senza segnalibro valido vengono proposti inizio e annullamento. Una diretta
esplicita propone **Avvia diretta**, senza promettere un riavvio impossibile.
Le etichette seguono la lingua italiana o inglese dell'interfaccia.

La scelta copre sorgenti online, download completi o progressivi, sorgenti
riconosciute come copie già scaricate e passaggio all'episodio successivo,
sia online sia locale. Il passaggio automatico al prossimo episodio arriva
alla scelta e attende l'utente prima di aprire il nuovo player.

Cerchio annulla senza modificare la posizione salvata. Le callback annullate,
sostituite, già consumate o appartenenti a un altro account/titolo/episodio
non possono avviare una riproduzione. La sorgente viene catturata per valore,
quindi un aggiornamento dei risultati degli add-on non cambia la scelta.

Il punto di ripresa viene passato esplicitamente al launcher. Un avvio
dall'inizio usa zero e non rilegge di nascosto il segnalibro. Rimangono le
precedenti regole di validità e completamento del progresso. Ogni episodio
usa la propria identità e non eredita il tempo di un altro episodio.

Nei download incompleti rimangono obbligatori il prefisso verificato e la
provenienza della stessa sorgente. Se la prova del prefisso viene meno fra
la domanda e la conferma, l'app segnala l'indisponibilità e non apre il
player; una nuova scelta è necessaria. Non si ricavano secondi di ripresa
dalla percentuale scaricata.

## EVO come motore alternativo

È tecnicamente possibile eliminare libVLC e integrare i componenti di EVO,
conservando l'interfaccia Stremio. L'archivio allegato è identico al riferimento
EVO v0.12.5 già esaminato. Il suo percorso operativo è AGC diretto; alcuni
commenti negli header descrivono ancora una precedente implementazione GL.

EVO non espone una libreria completa e autonoma equivalente a libVLC: il
controller, il demux, l'audio e il clock condividono stato con l'app. L'interfaccia
del decoder `evo_vdec` e il batch AGC per `hui::gfx::DrawList` sono parti riutilizzabili.
Una migrazione completa dovrebbe preservare HTTP e relativi header, torrent,
download progressivi con prefissi verificati, seek/resume, tracce, sottotitoli, PCM,
passthrough e gestione della durata delle risorse GPU e audio, usando un solo
componente responsabile di VideoOut. Cambiare solo VLC lasciando VideoGl
non rimuoverebbe il vecchio costo di upload.

Questa build mantiene il motore VLC e applica la correzione circoscritta verificata
localmente. Non dichiara completata una migrazione EVO. La valutazione tecnica
con i file e i punti di chiamata effettivi è in `validation/resume-choice/evo-architecture-review.md`.

## Verifiche locali realmente eseguite

Il preflight ha compilato ed eseguito C++20 con GCC e Clang 18 e compilato un oggetto
per `x86_64-sie-ps5`. I compilatori versionati funzionano anche senza l'alias
`clang++`. Dipendenze verificate: 106 plugin e 132 archivi per PS5, 106 plugin
e 129 archivi per host; i manifest OpenGL e i checksum di libc corrispondono.

| Verifica | Risultato | Limite |
| --- | --- | --- |
| Shader, readback, coordinate e formati VideoGl | 319 controlli PASS | OpenGL host reale, non driver PS5 in esecuzione |
| Allocazioni, FBO, rilascio e frammentazione | 99 controlli PASS | GL reale con fault injection e modello dei layout PS5 |
| Avvio del decoder | 78 casi / 10.039 controlli PASS | ASan/UBSan, funzioni Sony importate simulate e ownership indipendente |
| Scelta di avvio online | 121 controlli PASS | App di produzione, interfacce HTTP/decoder controllate |
| Episodio successivo | 46 controlli PASS | App di produzione, file locali controllati |
| Download/progresso | 263 controlli PASS | Prefissi, identità, soglia 5% e scelta di avvio |
| Preferenze | 247 controlli PASS | Multiselect conservato, nessuna richiesta HTTP reale |
| Interazioni | 102 controlli PASS | Callback, navigazione e controlli dell'app |
| Diagnostica e identità della release | PASS | Istogrammi persistiti, oscuramento dei dati sensibili e controlli di versione e ricevuta |

Build finali **desktop, host-VLC e PS5 PASS**, codice di uscita 0. La build
nativa (`BUILD_JOBS=4 ./build.sh ps5`) ha richiesto 164,15 secondi e ha completato
compilazione, collegamento, conversione, firma e confezione dei pacchetti.
Dopo la ricompilazione finale sono stati rieseguiti i cinque gate App desktop
riportati sopra, con hash degli oggetti collegati invariati prima/dopo i test.
La scelta di avvio è stata verificata anche sul backend host-VLC: 121 PASS.

Il test di cadenza usa il vero Player con VLC/FFmpeg statici, H.264 1080p con
B/P frame e audio PCM. Dopo un ritardo artificiale di 350 ms nell'uscita video,
il comportamento corretto riduce la raffica di recupero nei primi 50 ms da
10 a 3 frame e riprende con ritardo finale circa 0,18 ms. I riferimenti immagine
indipendenti e le lease dei buffer rimangono validi; la frequenza nominale
resta 24 fps per il fixture. È un test host della presentazione e della nuova
API diagnostica, non la misura del fix RECT sulla PS5.

Confezionamento: **722 controlli PASS su 227 file**, con verifica della ricevuta,
CRC ZIP, verifica MkPFS, estrazione completa e confronto dei nomi/SHA-256 tra
cartella applicazione, ZIP e FFPFSC estratto; controllati anche metadati,
struttura delle directory e permessi. Il binario contiene 0.5.20 / PPSA74126 /
00.070.000. Le ricevute non sono state modificate per forzare un esito positivo.

Simboli: PIE e ricevuta corrispondono; **13/13 risoluzioni indirizzo→riga PASS**
e **sei verifiche DWARF su singoli oggetti PASS**. La verifica **globale DWARF
resta FAIL**, exit 1 con **37.831 errori**. L'esito e il log completo sono inclusi:
non viene dichiarata la validità globale di ogni metadato di debug. Non sono
state eliminate sezioni per nascondere il problema.

Identità di produzione:

- Source tree SHA-256: `70fc195d4b6582e49edca77bb29c1224b72c8d11df3affd00d84cf1e703929d0`.
- PIE ELF SHA-256: `6f37caf3bd87ac30505031a4259623a8f7d7b15cc0c0c68cb0bdd8d0f725eefe`.

Il precedente caso diagnostico H.264 con crop SPS a origine non nulla, descritto
nelle verifiche 0.5.19, rimane una limitazione preesistente e non è stato
riclassificato come PASS. Questa versione non modifica quella geometria.


I risultati originali sono raccolti in `validation/resume-choice/`, separati
dalle verifiche storiche 0.5.19. Il confronto del driver e la revisione di EVO
sono letture di codice, non test sulla console. Gli eventuali test nativi con
mock non dimostrano accettazione hardware, audio HDMI o assenza di crash su PS5.

Comandi dei controlli mirati dopo le build host:

```sh
python3 tests/run_video_gl_tests.py
python3 tests/run_video_gl_allocation_tests.py
python3 tests/run_ps5vdec_startup_tests.py
python3 tests/run-diagnostics-tests.py
python3 tests/run_playback_choice_tests.py build/desktop
python3 tests/run_download_app_tests.py build/desktop
python3 tests/run_next_episode_tests.py build/desktop
python3 tests/run_preferences_tests.py build/desktop
python3 tests/run_interaction_tests.py build/desktop
python3 tests/run_player_cadence_tests.py build/host-vlc
```

## Ricompilazione e dipendenze

Estrarre `Stremio-Plus-0.5.20-repository.zip` e seguire `BUILDING.md`.
Le cache `Stremio-Plus-0.5.19-dependencies-core.zip` e
`Stremio-Plus-0.5.19-dependencies-pacbrew.zip` rimangono valide: le versioni
e i sorgenti delle dipendenze non sono cambiati. La versione nei loro nomi
identifica gli input conservati, non il nuovo eseguibile.

```sh
python3.12 ../Stremio-Plus-0.5.19-dependencies/restore-cache.py "$PWD"
BUILD_JOBS=4 ./build.sh ps5
```

Sono inclusi anche i 132 archivi statici effettivamente usati nel bundle
`Stremio-Plus-0.5.19-native-libraries.zip`. È una copia verificabile, non una
cache di build relocabile: per un percorso nuovo rigenerare il motore con
le ricette e gli archivi verificati, senza modificare le ricevute.

Output: `dist/PPSA74126.zip`, `dist/PPSA74126.ffpfsc`, `build/native/pie.elf`.
I simboli 0.5.20 devono appartenere a questa compilazione. La ricevuta nell'app
lega eseguibile, sorgenti di produzione, asset e dipendenze tramite SHA-256.
Documenti, test ed evidenze sono conservati nello ZIP del repository, coperto
dal proprio checksum.

## Prova PS5 ancora necessaria

Le istruzioni sono in `PROVA_PS5_IT.md`. Riprodurre prima lo stesso video 1080p per
almeno 5 minuti, poi verificare pausa/seek/ripresa, cambio video e ritorno alla Home.
Controllare la riduzione di `plane_transfer`/`upload`, la cadenza di prelievo,
gli scarti e il decoder effettivamente selezionato. Il cambio di refresh era rifiutato:
anche con 25 fotogrammi al secondo consegnati correttamente, un output a frequenza
non multipla può conservare un judder distinto dagli scarti dovuti all'upload.

Restano necessari i test del controller PS5, del volume PCM e del passthrough
sul dispositivo reale, della persistenza del login dopo il logout del browser,
della tenuta prolungata e delle precedenti protezioni. Nessun test locale
equivale a queste prove.
