#!/usr/bin/env python3
"""Compare actual native folder/ZIP/image output, including image inode metadata."""
from pathlib import Path
import hashlib, json, stat, sys, zipfile
from mkpfs.pfs import inspect_pfs_image, open_inner_file_view
from mkpfs.exfat import ExfatReader
from mkpfs import consts

root=Path(sys.argv[1]); extracted=Path(sys.argv[2]); destination=Path(sys.argv[3])
app=root/'dist/PPSA74126'; zip_path=root/'dist/PPSA74126.zip'; image_path=root/'dist/PPSA74126.ffpfsc'
def digest(path):
 h=hashlib.sha256()
 with path.open('rb') as stream:
  for chunk in iter(lambda:stream.read(1024*1024),b''):h.update(chunk)
 return h.hexdigest()
def files(path):return {p.relative_to(path).as_posix():p for p in path.rglob('*') if p.is_file()}
folder=files(app); unpacked=files(extracted); checks=[]
expected_directories={''}|{p.relative_to(app).as_posix() for p in app.rglob('*') if p.is_dir()}
def check(condition, description):
 checks.append({'check':description,'pass':bool(condition)})
 if not condition:raise RuntimeError(description)
check(set(folder)==set(unpacked),'PFS extracted file paths match application folder')
check(all(digest(folder[name])==digest(unpacked[name]) for name in folder),'PFS extracted bytes match every application file')
check(not any(p.is_symlink() for p in app.rglob('*')),'Application contains only regular files/directories')
with zipfile.ZipFile(zip_path) as z:
 check(z.testzip() is None,'ZIP CRC validation')
 entries={p.filename.removeprefix('PPSA74126/'):p for p in z.infolist() if not p.is_dir()}
 check(all(p.filename.startswith('PPSA74126/') for p in z.infolist()),'ZIP contains only the PPSA74126 top-level folder')
 check(set(entries)==set(folder),'ZIP file paths match application folder')
 directories={p.filename.removeprefix('PPSA74126/').rstrip('/'):p for p in z.infolist() if p.is_dir()}
 check(set(directories)==expected_directories,'ZIP directory paths match application folder')
 for name,entry in directories.items():
  check(stat.S_IMODE(entry.external_attr>>16)==stat.S_IMODE((app/name).stat().st_mode),'ZIP directory mode '+name)
 for name,path in folder.items():
  check(hashlib.sha256(z.read(entries[name])).hexdigest()==digest(path),'ZIP bytes '+name)
  check(stat.S_IMODE(entries[name].external_attr>>16)==stat.S_IMODE(path.stat().st_mode),'ZIP mode '+name)
inspection=inspect_pfs_image(image_path,verify_payloads=False)
check(not inspection.errors,'PFS structural inspection: '+repr(inspection.errors))
check(inspection.header is not None and inspection.header.version==consts.PFS_VERSION_PS5,'PFS format version is PS5')
check(set(inspection.file_inodes)=={'PPSA74126.exfat'},'PFS contains exactly the requested title exFAT image')
inode_report={}
for name,index in inspection.file_inodes.items():
 inode=inspection.inodes[index]
 check(stat.S_IMODE(inode.mode)==0o555 and bool(inode.flags & consts.INODE_FLAG_READONLY),'PFS pinned read/execute-only container inode '+name)
 inode_report[name]={'mode':oct(stat.S_IMODE(inode.mode)),'flags':hex(inode.flags),'logicalBytes':inode.logical_size}
for name,index in inspection.dir_inodes.items():
 check(stat.S_IMODE(inspection.inodes[index].mode)==0o555,'PFS pinned read/execute-only container directory '+name)
opened=open_inner_file_view(image_path)
check(opened is not None,'Inner exFAT is accessible')
view,handle,inner_name=opened
try:
 reader=ExfatReader(view);dirs=set(['']);files_inside={}
 def visit(entries):
  for entry in entries:
   if entry.is_dir:
    dirs.add(entry.rel_path);visit(entry.children)
   else: files_inside[entry.rel_path]=entry
 visit(reader.root_entries())
 check(set(files_inside)==set(folder),'Inner exFAT file paths match application folder')
 check(dirs==expected_directories,'Inner exFAT directory paths match application folder')
 for name,entry in files_inside.items():
  check(entry.length==folder[name].stat().st_size,'Inner exFAT logical size '+name)
finally: handle.close()
for name in ('eboot.bin','sce_module/libc.prx'):
 check(stat.S_IMODE(folder[name].stat().st_mode)==0o755,'Folder executable mode '+name)
metadata=json.loads((app/'sce_sys/param.json').read_text());receipt=json.loads((app/'build-receipt.json').read_text())
check(metadata['titleId']==receipt['titleId']=='PPSA74126','Title identity matches receipt')
check(metadata['contentVersion']==receipt['contentVersion'],'Content version matches receipt')
outputs={}
for path in [zip_path,image_path,app/'eboot.bin',app/'sce_module/libc.prx',root/'build/native/pie.elf',root/'build/native/eboot.elf',root/'build/native/sandbox-elevator.elf',root/'build/native/download-writer.elf']:
 outputs[path.relative_to(root).as_posix()]={'bytes':path.stat().st_size,'sha256':digest(path)}
check(outputs['build/native/pie.elf']['sha256']==receipt['linkElfSha256'],'Matching diagnostic PIE ELF in build receipt')
report={'pass':True,'checks':len(checks),'filesCompared':len(folder),'directoryCount':len(expected_directories),'applicationVersion':receipt['applicationVersion'],'titleId':metadata['titleId'],'contentVersion':metadata['contentVersion'],'sourceTreeSha256':receipt['sourceTreeSha256'],'physicalConsoleTest':False,'pfsWarnings':inspection.warnings,'pfsModePolicy':'The pinned MkPFS wraps the application folder in PPSA74126.exfat and stores that image in read-only PFS/PFSC with 0555 container inodes. exFAT has no POSIX execute permissions. ZIP preserves 0755 eboot/libc, 0644 other files. Contents, file lengths, paths and application metadata were verified after deep extraction; container inode permissions were parsed directly.','outputs':outputs,'pfsFiles':inode_report,'checksDetail':checks}
destination.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k not in ('pfsFiles','checksDetail')},indent=2))
