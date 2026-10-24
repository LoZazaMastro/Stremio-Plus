# Decoder e presentazione: riscontri e modifica locale 0.5.20

## Prova PS5 realmente fornita dall'utente

Archivio `Downloads(4).zip`, sessione `1a169d41e94-d2`, build 0.5.19 del
23 ottobre 2026. Gli orari seguenti sono UTC. I dati sono stati estratti da
`log-review-20261023-1080p-long/events*.jsonl`, limitati alla sessione corrente
e deduplicati per contenuto completo, senza sommare la copia in `log.txt`.

- Il decoder PS5 rifiuta la memory-query a livello configurato 52 sia con
  pipeline 4 sia con pipeline 1 (`0x811d0200`, 17:34:37.875–37.895).
- Il player registra esplicitamente `decoder=software` e
  `source_fps=25.000000` durante la riproduzione.
- Sette finestre complete del player, da 17:35:12.092 a 17:38:12.204,
  contengono 5.253 callback a circa 40 ms e 4.694 prelievi dall'app a circa
  44,7–44,9 ms. La differenza di 559 corrisponde al contatore `vmem_drops`
  finale: circa il 10,64% dei frame di quelle finestre.
- `vout_lost_pictures` rimane zero; il software fornisce regolarmente 25 fps.
  La perdita osservata avviene dopo il decoder, nel consumo dei frame da
  parte dell'app. Non è corretto attribuirla automaticamente al mancato
  avvio del decoder hardware.
- Le finestre di presentazione rilevano circa 42,91–42,95 ms dentro
  `VideoGl::upload`, mentre `swap_wait` resta intorno a 1,46 ms. Il dato
  comprende upload/conversione e possibili attese driver; non è una misura
  isolata del tempo di decodifica o dello scanout HDMI.

Il dettaglio numerico e gli eventi del decoder sono conservati in
`1080p-long-analysis.json`.

## Retry AVC circoscritto

Il riferimento locale EVO è il file allegato
`EVO-PLAYER-PS5-main.zip`, SHA-256
`27f2b819c8dc6a0ceb3f267a7063640a5bc4ff0d88a65b204a232c50df5953ea`.
È identico al precedente `project_sources/16-EVO-PLAYER-PS5-main.zip`.
`docs/evo-pro/videodec2-abi.md` documenta un test upstream di una immagine
H.264 1080p con profilo 100, livello massimo configurato 51 e pipeline 1.
Quella è evidenza upstream, non una nuova prova della nostra app sulla PS5.

Modificato `vendor/vlc-ps5/modules/ps5vdec.c`:

1. Il primo tentativo rimane livello 52 / pipeline 4.
2. Il retry preesistente pipeline 1 rimane invariato.
3. Se anche la query di configurazione viene rifiutata, è ammesso un solo
   tentativo aggiuntivo livello 51 / pipeline 1, esclusivamente per AVC con
   profilo dichiarato 66, 77 o 100, livello noto maggiore di zero e non
   superiore a 51, dimensioni codificate note entro 1920×1088.
4. Livelli ignoti o superiori a 51, profili ignoti e dimensioni fuori limite
   non vengono riclassificati. I 1090 pixel del buffer I420 di uscita VLC
   non sono usati come dimensioni del bitstream del decoder.
5. Il retry aggiunto non scatta dopo errori di allocazione o creazione del
   decoder. Esauriti i tentativi, rimane il normale rifiuto del modulo e la
   selezione software da parte di VLC.
6. Sono registrati separatamente profilo/livello/dimensioni/frequenza
   dell'input e la configurazione realmente tentata.

ABI delle strutture native, protezioni di memoria, mapping, proprietà dei
buffer, limiti dei pool, preparazione anticipata della libreria/coda e
cleanup rimangono invariati. Il comportamento HEVC precedente è conservato.
Il significato specifico di `0x811d0200` non è stato dedotto senza una fonte.
L'accettazione della configurazione a livello 51 su questa PS5 resta da
verificare con la nuova build.

### Test eseguiti

- `python3 tests/run_ps5vdec_startup_tests.py`: **78 casi / 10.039 controlli
  PASS**, con AddressSanitizer, UndefinedBehaviorSanitizer e registro
  indipendente di proprietà delle allocazioni native.
- Il test compila le funzioni di startup di produzione; sostituisce soltanto
  tipi VLC/import native e i callback di decode/flush non esercitati.
- Casi aggiunti: tre profili supportati; limite livello 51 e altezza 1088;
  primo tentativo riuscito; entrambi i livelli rifiutati; livello 52/ignoto/0;
  profilo ignoto; larghezza >1920, altezza1090, dimensioni ignote; errori di
  allocazione/creazione senza retry di livello.
- Compilazione isolata dell'oggetto nativo `ps5vdec-native.o` con il compiler
  PS5 e gli stessi flag CMake della build0.5.20: **PASS**. Rimane un warning
  preesistente di confronto signed/unsigned nella misura del tempo decoder.
- Verifica ricevute VLC PS5 (106 plugin/132 archivi) e host
  (106 plugin/129 archivi): **PASS** dopo la modifica.

Il modulo è compilato nell'app da `CMakeLists.txt`, non negli archivi VLC.
Non entra nel fingerprint delle dipendenze di `tools/setup-vlc.sh` e non
modifica patch, selezione plugin o registrazione statica. Non occorre
rigenerare arbitrariamente le ricevute `.deps/vlc*`: le verifiche originali
passano. Occorre ricompilare e ripacchettizzare l'app, con nuova ricevuta
app e simboli corrispondenti.

## Verifica indipendente del percorso texture Mesa/PS5

Fonte: archivio OpenGL pin `ps5-opengl-sdk-1.0.0.tar.gz`, con il nested
`sources/mesa-26.2.0.tar.xz`. Il file rilevante per la selezione dei bind è
`src/mesa/state_tracker/st_cb_texture.c`, non `st_texture.c`.

- `mesa-reference/st_cb_texture.c:883–903`: `default_bindings` sceglie per i
  formati colore `PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET` quando
  il formato è supportato come texture2D; altrimenti ripiega su sampler.
- `st_cb_texture.c:1136–1158`: quel bind è passato a `st_texture_create`.
- `st_cb_texture.c:92–106`: `GL_TEXTURE_2D` diventa `PIPE_TEXTURE_2D`,
  mentre `GL_TEXTURE_RECTANGLE_NV` diventa `PIPE_TEXTURE_RECT`.
- `opengl-source/ps5-opengl/src/gallium/ps5/ps5_screen.c:1600–1633`:
  `ps5_linear_sampled_layout` restituisce false per il percorso 2D a singolo
  livello, formati RGBA8/R8/RG8/float supportati, con entrambi i bind.
  Questo ramo richiede esplicitamente `PIPE_TEXTURE_2D`; una texture RECT
  campionabile resta nel percorso lineare.

Quindi la modifica del target delle sole texture video di ingresso può
selezionare un diverso percorso del driver senza sostituire l'SDK.
Questa è una verifica del codice, non una misurazione delle prestazioni
effettive della nuova build sulla PS5.
