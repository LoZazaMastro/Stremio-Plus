# Verifica e backport selettivo VLC PS5

Data della verifica: **23 ottobre 2026**. Nessuna modifica a repository remoti,
branch, commit, release o pubblicazioni. Il report completo e le identità dei
file sono in `review.json`.

## Revisioni effettivamente confrontate

- Il core già usato è **VLC 3.0.24**, ancora la versione stabile indicata
  dall'[indice ufficiale VideoLAN](https://download.videolan.org/pub/videolan/vlc/last/).
  Il SHA-256 del sorgente coincide con quello già fissato nel progetto.
- Il vecchio port proviene dall'allegato `VLC-main.zip`, identificato dal
  digest nella NOTICE. Si presenta come app 0.4, ma l'archivio non identifica
  un commit Git: non gli viene attribuito un tag per deduzione.
- Il port confrontato è la [release v0.6.0](https://github.com/ZiZc3/VLC/releases/tag/v0.6.0),
  commit `c0ffdca8351a3a7210eabae26897e36f83b26bd8`, pubblicata il 21 ottobre.
- Il main controllato separatamente è
  `c1862643f84b68d3b4f01cb484b887196093bfa2`. Non è lo stesso commit del tag:
  introduce frontend, equalizzatore e vibrazione. Il confronto Git conferma
  che `patches`, `ps5/modules` e `ps5/compat` sono identici al tag.

Nel confronto iniziale, le prime 17 patch del port già presenti in Stremio
erano identiche byte per byte alle corrispondenti patch della release. Nel
risultato finale 16 rimangono identiche; per `0014-vmem-ps5-colour-info.patch`
sono cambiati soltanto il contesto e i confini degli hunk, così che il setup
riconosca la patch già applicata anche dopo `0025`. La prova su 4.383 file
conferma un albero generato identico e un secondo passaggio invariato con
tutte le 25 patch riconosciute (`patch-reentry-result.json`). Nessun codice
eseguito è cambiato con questa correzione del setup.

## Cambiamenti importati

`vendor/vlc-ps5/patches/0025-vmem-ps5-keep-visible.patch` è la patch upstream
`0018-vmem-ps5-keep-visible.patch` senza modifiche, con numero diverso per
non sovrascrivere la sequenza locale. Il SHA-256 è
`4bcf85d308566ae899739822103cacce1a9224e7f0d785f1772ef1f222d7a87a`.

L'adattamento in `src/player_vlc.cpp` conserva le dimensioni del buffer
decodificato e distingue la parte visibile. Il renderer esistente usa già
pitch e offset controllati per caricare soltanto i pixel visibili. Origini
non nulle o metadati incoerenti conservano il precedente percorso di
normalizzazione. Rimangono i limiti dimensionali, il limite di 64 MiB per
slot, i quattro slot, il buffer di riserva e le lease tramite shared ownership.

L'altro backport riguarda l'associazione VP9 tra PTS/visibilità e output
ritardato in `src/hwdec_ps5.cpp`. La revisione e i test ASan/UBSan separati sono
registrati in `evo-review/vlc-hardware/`. Non è stato sostituito integralmente
il decoder upstream: ciò avrebbe eliminato protezioni locali di overflow,
allineamento e gestione dei fallimenti. La NOTICE attribuisce esattamente
questi due adattamenti.

## Prova A/B di vmem

La prova collega gli oggetti Player e vmem del baseline conservati con le
stesse restanti librerie. Il candidato sostituisce soltanto questi due
oggetti, compilati dai sorgenti aggiornati. Decoder FFmpeg, demux, callback,
slot e gestione delle lease sono reali. I wrapper contano le copie e osservano
i formati; non sostituiscono la decodifica.

Il percorso PS5 attualmente consegnato usa libVLC: il modulo `ps5vdec` per
H.264/HEVC e l'adattatore già esistente `src/player_vlc_vp9.cpp`, registrato
come `stremiovp9`, per VP9 a 8 bit tramite `HwDecoder::open/send/receive`.
CMake include l'adattatore nel target PS5 e la tabella dei moduli statici lo
registra. La FIFO corregge quindi un backend già raggiungibile dal player
VLC; non introduce un nuovo profilo o supporto hardware. L'effettiva selezione
e decodifica del modulo sulla console restano da verificare. Anche il
miglioramento vmem misurato riguarda il percorso VLC consegnato.

| Caso | Copie complete intermedie, prima → dopo | Filtro adattatore, prima → dopo | Pixel visibili e lease |
| --- | ---: | ---: | --- |
| H.264 1280×720, I420 | 37 → 0 | 1 → 0 | PASS, 30 frame per versione |
| H.264 1920×1080, I420 | 37 → 0 | 1 → 0 | PASS, 30 frame per versione |
| HEVC 1920×1080 Main10, I0AL | 38 → 0 | 1 → 0 | PASS, 30 frame per versione |
| H.264 con output NV12 richiesto dal test | 0 → 0 | 1 → 1 | PASS, 30 frame per versione |
| HEVC Main10 con output P010 richiesto dal test | 0 → 0 | 1 → 1 | PASS, 30 frame per versione |

In tutti questi casi `sws_scale` è chiamato **zero volte in entrambe le
versioni**. La configurazione Stremio precedente evitava già il ridimensionamento
geometrico. Il miglioramento dimostrato è l'eliminazione di una copia completa
intermedia a fotogramma nei formati planari invariati. Restano le copie necessarie
verso i buffer posseduti dall'applicazione. I conteggi comprendono anche frame
preparati da VLC e non necessariamente prelevati dai 30 campioni osservati.

Ogni frame osservato è confrontato mediante SHA-256 di **tutti** i pixel
visibili con la decodifica indipendente, escludendo solo padding e allineamento.
La lease del primo frame rimane identica durante l'avanzamento e dopo `close`.
NV12/P010 esercitano le conversioni software; non costituiscono una prova del
decoder hardware PS5. Non sono stati misurati miglioramenti di FPS o tempo CPU.

## Limite rilevato: crop SPS con origine non nulla

Un caso separato codifica H.264 1280×720 con crop `[sinistra=16, alto=8,
destra=16, basso=8]`. L'immagine richiesta è 1248×704. **La correttezza dei
pixel fallisce sia nel baseline sia nel candidato**, che producono gli stessi
30 hash visibili. Il test delle lease passa.

Il riferimento è stato ricontrollato con il **medesimo FFmpeg 8.1.2 statico**:
la decodifica conserva il quadro intero e applica esplicitamente il crop non
allineato. Un secondo riferimento CLI indipendente restituisce gli stessi 72
hash. Entrambe le versioni dell'applicazione corrispondono invece al rettangolo
in alto a sinistra, prima di quel crop; VLC comunica a vmem un'origine zero.
Questo è un limite già presente del percorso decoder. Viene registrato come
**FAIL della correttezza del crop e PASS di non regressione**, senza dichiarare
un supporto completo del crop né modificare altri criteri del decoder durante
questo backport.

Il comando normale ripete i cinque casi riusciti:

```sh
python3 tests/run_player_geometry_tests.py build/host-vlc
```

La diagnosi opzionale del limite restituisce deliberatamente un esito negativo
finché il problema separato resta presente:

```sh
python3 tests/run_player_geometry_tests.py build/host-vlc --include-known-crop
```

## Confini della conclusione

Questi risultati giustificano i due backport selezionati. Non dimostrano la
causa dei microscatti segnalati su firmware 12.7. Decodifica hardware effettiva,
scanout HDMI, cambio refresh e comportamento dei driver restano prove da fare
sulla console. Le patch upstream per salto PCR oltre dieci secondi, buffering
dei canali live e parser 1080i riguardano problemi diversi; non vengono
presentate come una correzione dimostrata della cadenza 1080p.
