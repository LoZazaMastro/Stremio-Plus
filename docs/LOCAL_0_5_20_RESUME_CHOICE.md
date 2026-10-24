> **Historical intermediate snapshot.** This record predates the final
> 0.5.20 video-upload correction and the completed final build. See
> [LOCAL_0_5_20_VALIDATION.md](LOCAL_0_5_20_VALIDATION.md) for the final build
> verification record. The original intermediate results below are retained.

# Stremio Plus 0.5.20 — scelta di avvio, build locale

Data: 23 ottobre 2026. Title ID `PPSA74126`, content version `00.070.000`.
La build è destinata alla prova del proprietario. Non è una pubblicazione,
un tag Git o una richiesta allo store.

## Comportamento

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

## Ambito tecnico

Le modifiche applicative sono in `app.h`, `app_browse.cpp`, `app_detail.cpp`,
`app_downloads.cpp` e `app_watch.cpp`. I controlli di volume, L2/R2, decoder,
conversione video, memoria nativa e durata dei buffer audio restano quelli
della build 0.5.19. Le dipendenze e le librerie statiche VLC restano identiche.

Questo intervento non costituisce una correzione del rifiuto `ps5vdec`
`0x811d0200` o del costo di upload/conversione di circa 43 ms osservati nei
log PS5 della precedente prova breve. Quel problema richiede dati di una
riproduzione continua e un'indagine separata. I circa 60 FPS dell'interfaccia
non sono una misura del framerate effettivo del film.

## Verifiche

Il preflight ha compilato ed eseguito un programma C++20 con GCC e Clang 18
e ha compilato un oggetto per il target `x86_64-sie-ps5`. Le ricevute delle
dipendenze sono state verificate: 132 archivi statici PS5 e 129 host,
ciascuno con 106 moduli VLC; manifest OpenGL e checksum libc corrispondono.
I compilatori versionati funzionano anche in assenza dell'alias `clang++`.

I risultati di compilazione, integrazione e confezionamento vengono raccolti
in `validation/resume-choice/` prima della consegna. I test applicativi
usano gli oggetti di produzione e controllano il confine esterno del decoder
e delle richieste HTTP; non simulano una prova fisica della PS5.

Comandi dei controlli mirati, dopo la build host:

```sh
python3 tests/run_playback_choice_tests.py build/desktop
python3 tests/run_download_app_tests.py build/desktop
python3 tests/run_next_episode_tests.py build/desktop
python3 tests/run_preferences_tests.py build/desktop
python3 tests/run_interaction_tests.py build/desktop
python3 tests/run_playback_choice_tests.py build/host-vlc
```

La prova su PS5 della nuova scelta è ancora necessaria. Le istruzioni
complete sono in [PROVA_PS5_IT.md](PROVA_PS5_IT.md), sezione 5.

## Ricompilazione e dipendenze

Usare `Stremio-Plus-0.5.20-repository.zip` e seguire [BUILDING.md](../BUILDING.md).
Le due cache `Stremio-Plus-0.5.19-dependencies-core.zip` e
`Stremio-Plus-0.5.19-dependencies-pacbrew.zip` già consegnate rimangono valide:
la versione nel loro nome identifica gli stessi input, non il nuovo binario.

```sh
python3.12 ../Stremio-Plus-0.5.19-dependencies/restore-cache.py "$PWD"
BUILD_JOBS=4 ./build.sh ps5
```

Gli output sono `dist/PPSA74126.zip`, `dist/PPSA74126.ffpfsc` e
`build/native/pie.elf`. Usare esclusivamente i simboli della stessa build.
La ricevuta inclusa nell'applicazione lega eseguibile, sorgenti, asset e
dipendenze tramite SHA-256. Il pacchetto sorgenti conserva anche le verifiche
storiche, chiaramente separate da quelle di questo aggiornamento.
