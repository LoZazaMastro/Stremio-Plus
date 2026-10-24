# Dipendenze locali per Stremio Plus 0.5.19 — PPSA74126

Questa cartella conserva gli input verificati per ricompilare il progetto localmente. I due archivi delle dipendenze si estraggono nella **stessa cartella `Stremio-Plus-0.5.19-dependencies/`**. Servono entrambi:

| Archivio previsto | Contenuto | Dimensione dei file prima del contenitore ZIP |
| --- | --- | ---: |
| `Stremio-Plus-0.5.19-dependencies-core.zip` | SDK v0.42, OpenGL 1.0.0 con sorgenti, 16 archivi media, zlib per gli strumenti, snapshot Git e pacchetti Python | 355,596 MB |
| `Stremio-Plus-0.5.19-dependencies-pacbrew.zip` | Pacchetti binari PacBrew v0.40.2 | 346,234 MB |

`inventory.json` elenca dimensioni, SHA-256, tipo di contenuto, identità delle revisioni e percorsi originali. I file vengono conservati una sola volta: NASM 2.16.03 e FreeType 2.13.3 sono già nella cartella `source-archives/` dei sorgenti dell'applicazione.

## Struttura

- `downloads/`: archivi originali SDK, OpenGL, PacBrew e zlib 1.3.2.
- `media/`: i 16 archivi media che completano i due già inclusi nei sorgenti.
- `git/`: boilerplate nativo e MkPFS alle revisioni esatte, con la metadata originale dei loro snapshot Git superficiali.
- `packaging-wheels/`: MkPFS e nove dipendenze Python, con versioni e checksum.
- `restore-cache.py`: ripristino verificato della cache dentro una nuova copia dei sorgenti.
- `SHA256SUMS`: controlli di integrità dei file consegnati.

## Ripristino e ricompilazione

Usare Linux x86_64 con **Python 3.12**, `python3-venv`, Git e gli altri prerequisiti host di `BUILDING.md` già installati. I pacchetti del sistema operativo e il compilatore host non sono contenuti in questi ZIP. I wheel nativi sono quelli verificati nell'ambiente Linux x86_64 con Python 3.12.

1. Estrarre i sorgenti dell'applicazione in una nuova cartella. Estrarre entrambi gli ZIP delle dipendenze accanto ad essa, nella cartella comune indicata sopra.
2. Dalla cartella delle dipendenze, verificare tutto:

   ```sh
   sha256sum --check SHA256SUMS
   ```

3. Dalla radice dei sorgenti, dove si trovano `build.sh` e `BUILDING.md`, avviare il ripristino passando la posizione delle dipendenze:

   ```sh
   python3.12 ../Stremio-Plus-0.5.19-dependencies/restore-cache.py "$PWD"
   ```

   Se le due cartelle non sono adiacenti, indicare il percorso assoluto di `restore-cache.py`. La cartella dei sorgenti è l'argomento finale.

4. Ricompilare seguendo `BUILDING.md`. Per il target nativo:

   ```sh
   BUILD_JOBS=4 ./build.sh ps5
   ```

Il ripristino controlla tutti gli input prima di scrivere, copia gli archivi nelle posizioni attese, ripristina le due dipendenze Git in stato detached e prepara MkPFS con `pip --no-index`. Scrive una ricevuta in `.deps/offline-cache-restore.json`. Una seconda esecuzione riutilizza i file verificati. Una cache esistente con contenuto diverso viene segnalata e preservata.

Il ripristino richiede cartelle reali all'interno della copia dei sorgenti. Non segue collegamenti della cache verso altre cartelle: questo evita che una copia dei sorgenti estratta da un archivio continui a dipendere dal percorso originale di compilazione.

Gli archivi delle dipendenze e gli snapshot Git sono sufficienti per ripetere i passaggi di download del progetto senza rete, su un host già predisposto. La compilazione VLC e dell'applicazione resta un passaggio da eseguire; il ripristino della cache non la avvia.

## Identità Git conservate

| Dipendenza | Commit | Albero sorgenti |
| --- | --- | --- |
| Boilerplate nativo | `f98de734b68b0b980d38ff03a330066afd6ae16d` | `723111f8b760d912f975edce4a821085c1167c2a` |
| MkPFS | `6cb8313dfe0c988ac52617794553f343243d3a56` | `034378333170989698f7428c322e58b582c68604` |

I bundle derivano da snapshot Git superficiali. Il file `.bundle.shallow` associato è indispensabile: contiene il limite della cronologia originale. `restore-cache.py` lo ripristina prima del fetch locale e controlla commit, albero, assenza di modifiche e connettività degli oggetti. Non crea branch o commit e non comunica con repository remoti.

## Sorgenti e pacchetti binari

OpenGL 1.0.0 contiene sia il SDK binario sia gli snapshot sorgenti corrispondenti, inclusi Mesa, OpenGNM, patch, licenze e provenienza. Il suo SHA-256 è quello previsto dal progetto: `f93643c04c843d56143b00951df1f8042ea7706ae9f19e4e9abf158e1ead77c5`.

I 18 input media sono archivi sorgenti. Il boilerplate e MkPFS sono snapshot sorgenti con identità Git verificabile. SDK v0.42 e PacBrew v0.40.2 sono pacchetti binari con header; i loro sorgenti upstream aggiuntivi non sono presenti in questa cache. L'inventario distingue i relativi URL e checksum documentati dagli input effettivamente disponibili. Questo pacchetto consente di ricompilare l'applicazione usando quei SDK verificati; non rappresenta una ricompilazione integrale offline di ogni componente dei SDK o del compilatore host.

Le librerie VLC già compilate, se consegnate separatamente, corrispondono alla loro ricevuta. Le directory di build originali e i file pkg-config/link contengono percorsi assoluti: per una nuova posizione conviene rigenerarli dai sorgenti della cache, preservando la ricevuta originale come evidenza del build consegnato.

## Verifiche eseguite sul ripristino

Il helper è stato eseguito in cartelle temporanee isolate: ripristino completo, installazione MkPFS offline, `pip check`, `mkpfs --help`, seconda esecuzione idempotente, rifiuto di input corrotto prima delle scritture, preservazione di una cache modificata e rifiuto di un collegamento esterno. Entrambi gli snapshot Git sono stati ripristinati e verificati con `git fsck`, alle revisioni e agli alberi riportati sopra. Non è stata eseguita una seconda compilazione completa dell'applicazione durante questa prova del helper.

Queste sono verifiche locali degli strumenti di compilazione; la prova dell'applicazione sulla PS5 resta distinta.
