#!/usr/bin/env python3
from pathlib import Path
import collections,hashlib,json,subprocess,sys
root=Path(sys.argv[1]).absolute();v=Path(sys.argv[2]).absolute();elf=root/'build/native/pie.elf'
def digest(path):
 h=hashlib.sha256()
 with path.open('rb') as stream:
  for chunk in iter(lambda:stream.read(1024*1024),b''):h.update(chunk)
 return h.hexdigest()
sections=subprocess.check_output(['llvm-readelf-18','--sections',str(elf)],text=True)
(v/'native-pie-sections.log').write_text(sections)
nm=subprocess.check_output(['llvm-nm-18','--defined-only','--demangle',str(elf)],text=True)
symbols=[s for s in nm.splitlines() if ' T ' in s or ' t ' in s]
chosen=[]
for key in ('App::update_home_startup_at','App::update()','HwDecoder::Impl::decode_one','HwDecoder::Impl::harvest','HwDecoder::send','HwDecoder::receive','HwDecoder::flush','Player::take_frame'):
 matching=[s for s in symbols if key in s]
 if matching:chosen.append(matching[0])
lookups=[]
for entry in chosen:
 answer=subprocess.check_output(['llvm-addr2line-18','-e',str(elf),'-f','-C',entry.split()[0]],text=True).strip()
 lookups.append({'symbol':entry,'lookup':answer,'sourceLineResolved':':0' not in answer and '??' not in answer})
with (v/'native-dwarf-verify.log').open('w') as out:
 dwarf=subprocess.run(['llvm-dwarfdump-18','--verify',str(elf)],stdout=out,stderr=subprocess.STDOUT).returncode
object_results={}
for name in ('app.cpp.o','hwdec_ps5.cpp.o'):
 path=root/'build/native/cmake/CMakeFiles/stremio.dir/src'/name
 logfile=v/('native-'+name.replace('.','-')+'-dwarf-verify.log')
 with logfile.open('w') as out:
  code=subprocess.run(['llvm-dwarfdump-18','--verify',str(path)],stdout=out,stderr=subprocess.STDOUT).returncode
 object_results[name]={'exit':code,'log':logfile.name}
errors=collections.Counter()
with (v/'native-dwarf-verify.log').open() as source:
 for line in source:
  if line.startswith('error:'):errors[line.split(':',2)[1].strip()]+=1
report={'elfSha256':digest(elf),'hasSymtab':'.symtab' in sections,'hasDebugInfo':'.debug_info' in sections,'hasDebugLine':'.debug_line' in sections,'addressLookups':lookups,'globalDwarfVerifyExit':dwarf,'globalDwarfErrorCount':sum(errors.values()),'globalDwarfErrorCategories':dict(errors),'objectDwarfVerify':object_results,'globalDwarfVerifyLog':'native-dwarf-verify.log','limitation':'The complete linked ELF did not pass global DWARF verification. Real function address-to-line results and individual object verification are reported separately. Full variable/type debugging is not promised, and no ELF sections were removed to suppress diagnostics.' if dwarf else 'The application uses optimized line-table debug data; full variable/type inspection is not promised.'}
(v/'native-debug-info.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2));sys.exit(dwarf)
