#!/usr/bin/env python3
"""Package only the exact native ELF files associated with the current receipt."""
from pathlib import Path
import hashlib,json,shutil,subprocess,sys,zipfile
root=Path(sys.argv[1]).absolute();validation=Path(sys.argv[2]).absolute()
app=root/'dist/PPSA74126';receipt=json.loads((app/'build-receipt.json').read_text())
subprocess.run([sys.executable,str(root/'tools/build-receipt.py'),'--verify',str(root),str(app)],check=True)
def digest(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
 return h.hexdigest()
assert digest(root/'build/native/pie.elf')==receipt['linkElfSha256']
report=json.loads((validation/'native-artifact-validation.json').read_text())
assert report['sourceTreeSha256']==receipt['sourceTreeSha256']
for key,value in report['outputs'].items():assert digest(root/key)==value['sha256'],key
version=receipt['applicationVersion'];stage=root/'build'/f'symbols-{version}'
if stage.exists():shutil.rmtree(stage)
stage.mkdir(parents=True)
for name in ('pie.elf','eboot.elf','sandbox-elevator.elf','download-writer.elf'):shutil.copy2(root/'build/native'/name,stage/name)
shutil.copy2(app/'build-receipt.json',stage/'build-receipt.json')
for name in ('native-artifact-validation.json','native-debug-info.json','native-pie-sections.log','native-startup-audit.json','native-process.json'):
 source=(root/'build/native'/name) if name in ('native-startup-audit.json','native-process.json') else validation/name
 if source.exists():shutil.copy2(source,stage/name)
debug=json.loads((stage/'native-debug-info.json').read_text());assert debug['elfSha256']==receipt['linkElfSha256'];example=debug['addressLookups'][0]['symbol'].split()[0] if debug.get('addressLookups') else '120'
readme=f"""# Simboli diagnostici — Stremio Plus {version}

Title ID: `{receipt['titleId']}`. Content version: `{receipt['contentVersion']}`.

Questi ELF provengono dalla stessa compilazione locale di `PPSA74126.zip` e
`PPSA74126.ffpfsc` registrata in `native-artifact-validation.json`.
Nessuna prova di esecuzione su PS5 è compresa in questa verifica.

## Corrispondenza

- Source tree SHA-256: `{receipt['sourceTreeSha256']}`.
- PIE ELF SHA-256: `{receipt['linkElfSha256']}`.
- `pie.elf`: ELF del link nativo, non stripped, con simboli e line tables.
- `eboot.elf`: ELF convertito, dopo configurazione dell'heap, usato per produrre
  l'`eboot.bin` firmato nella cartella applicazione.
- `sandbox-elevator.elf` e `download-writer.elf`: gli stessi helper della cartella
  applicazione, con i rispettivi dati diagnostici.
- `build-receipt.json`: vincolo tra sorgenti, dipendenze VLC e file applicazione.

`SHA256SUMS` verifica tutti i file di questo bundle, escluso il file checksum
stesso. Il report di validazione contiene anche gli hash di ZIP, FFpfsc e
`eboot.bin`; non riutilizzare questi simboli con binari rigenerati.

## Risalire da un indirizzo alla riga sorgente

Da questa directory, con LLVM 18 installato:

```sh
sha256sum --check SHA256SUMS
llvm-addr2line-18 -e pie.elf -f -C 0x{example}
llvm-nm-18 --defined-only --demangle pie.elf
```

Per un PC assoluto registrato dalla console, usare l'indirizzo relativo al
modulo: indirizzo ELF = PC meno base di caricamento del titolo. Se il crash log
fornisce già tale offset, usarlo direttamente. L'entry point degli ELF di questa
build è relativo al modulo; non usare la base del loader o quella di un altro
modulo. La base effettiva dipende dalla sessione sulla PS5.

I percorsi sorgente nei dati diagnostici includono la directory della
compilazione locale. Il file e la riga vanno cercati nel repository allegato
con lo stesso source tree hash.

## Verifiche e limiti

Sono presenti `.symtab`, `.debug_info` e `.debug_line`. Gli indirizzi campione
di funzioni reali dell'applicazione hanno risolto file e riga con
`llvm-addr2line-18`; risultati esatti in `native-debug-info.json`.

L'applicazione è ottimizzata e usa `-gline-tables-only`: questo bundle non
promette ispezione completa di variabili o tipi, né una corrispondenza uno-a-uno
fra tutte le istruzioni ottimizzate e il sorgente. Alcune dipendenze precompilate
includono ulteriori dati debug secondo le proprie opzioni di compilazione.

La verifica DWARF globale dell'ELF collegato non è passata: LLVM 18 segnala
intervalli sovrapposti/non contenuti e alcuni tag di tipo. La verifica dei
singoli oggetti `app.cpp.o` e `hwdec_ps5.cpp.o` è passata, e tutte le otto
risoluzioni di indirizzi campionate sono riuscite, incluse `decode_one` e
`harvest` della logica VP9 aggiornata. La causa completa dei messaggi globali non è
stata dimostrata. Nessuna sezione è stata rimossa e nessun messaggio è stato
nascosto per trasformare questa verifica in un successo. La ricevuta e gli
hash attestano la corrispondenza al binario, non l'assenza di ogni problema
nei metadati DWARF.

Il contenitore FFpfsc usa il comportamento originale di MkPFS: cartella app
nel volume exFAT `PPSA74126.exfat`, compresso in PFS/PFSC PS5. exFAT non
conserva permessi POSIX; lo ZIP cartella conserva `0755` per eboot/libc e `0644`
per gli altri file. Tutti i contenuti estratti sono stati confrontati byte per
byte mediante SHA-256 con cartella e ZIP.
"""
(stage/'README.md').write_text(readme)
checks=''.join(digest(p)+'  '+p.name+'\n' for p in sorted(stage.iterdir()) if p.is_file())
(stage/'SHA256SUMS').write_text(checks)
out=root/'dist'/f'Stremio-Plus-{version}-symbols.zip';temporary=out.with_suffix('.zip.part')
with zipfile.ZipFile(temporary,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
 for p in sorted(stage.iterdir()):archive.write(p,p.name)
with zipfile.ZipFile(temporary) as archive:assert archive.testzip() is None
temporary.replace(out)
record={'file':str(out),'bytes':out.stat().st_size,'sha256':digest(out),'sourceTreeSha256':receipt['sourceTreeSha256'],'pieElfSha256':receipt['linkElfSha256']}
(validation/'native-symbols-bundle.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2))
