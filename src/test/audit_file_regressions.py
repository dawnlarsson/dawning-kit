#!/usr/bin/env python3
"""Rebuild source-extracted, filesystem-mocked regressions for the file fixes."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[2]
TEMPLATES=ROOT/'src/test/audit_file'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,help='retain generated fixtures and JSON in this directory')
args=parser.parse_args()
workspace=None if args.output else tempfile.TemporaryDirectory(prefix='dawning-file-regressions-')
HERE=args.output.resolve() if args.output else Path(workspace.name)
HERE.mkdir(parents=True,exist_ok=True)
FILES={'file':(ROOT/'src/sh/file.c').read_text(),'common':(ROOT/'src/library.common.c').read_text(),'clock':(ROOT/'src/standard/clock.c').read_text()}
spans=[]
def function(name):
 for source,text in FILES.items():
  m=re.search(r'^((?:static |inline |INLINE |PURE |CONST |RETURNS_NONNULL )*(?:\w+[ \t]+)+)'+re.escape(name)+r'\([^;]*?\)\n\{',text,re.M)
  if not m: continue
  begin=m.start();at=m.end()-1;depth=0;state='code'
  while at<len(text):
   c=text[at];n=text[at:at+2]
   if state=='line':
    if c=='\n':state='code'
   elif state=='comment':
    if n=='*/':state='code';at+=1
   elif state in ('"',"'"):
    if c=='\\':at+=1
    elif c==state:state='code'
   elif n=='//':state='line';at+=1
   elif n=='/*':state='comment';at+=1
   elif c in ('"',"'"):state=c
   elif c=='{':depth+=1
   elif c=='}':
    depth-=1
    if depth==0:
     body=text[begin:at+1];spans.append({'function':name,'source':source,'sha256':hashlib.sha256(body.encode()).hexdigest()});return body
   at+=1
 raise ValueError('function not found: '+name)
def region(start,stop):
 text=FILES['file'];at=text.index(start);return text[at:text.index(stop,at)+len(stop)]
facts=region('typedef struct\n{\n        b64 seconds;','} file_facts;')
def expand(text):
 text=text.replace('@facts@',facts)
 text=text.replace('@find_types@', region('typedef struct\n{\n        p8 kind;\n        b32 unit;', '} find_batch;'))
 text=text.replace('@find_predicates@', region('#define FIND_SETS_DEEPEST 1', '    {"-newermt", \'w\', FIND_TAKES_VALUE},\n};'))
 text=text.replace('@dates@',function('clock_floor_divide')+'\n'+function('clock_days_from_civil')+'\n'+function('clock_civil_from_days')+'\n'+function('file_split_moment')+'\n'+region('static const string_address file_month_names[12]', '"july",    "august",   "september", "october", "november", "december"};')+'\n'+region('typedef struct\n{\n        string_address name;\n        b64 seconds;','bool file_moment_read(string_address text, b64 now, b64 address_to out)') .rsplit('\nbool file_moment_read(',1)[0])
 return re.sub(r'@([a-zA-Z_][a-zA-Z_0-9]*)@',lambda m:function(m[1]),text)
results={'sources':{k:hashlib.sha256(v.encode()).hexdigest() for k,v in FILES.items()},'safety':'All filesystem, traversal and process APIs in the fixtures are mocked; no real root-equivalent filesystem operation occurs. Parser and emitter dependencies explicitly adapted to libc are noted in the templates.','checks':[]}
for name in ['rm','touch','stat','find']:
 path=HERE/(name+'-fixture.c');path.write_text(expand((TEMPLATES/'common-fixture.h').read_text()+(TEMPLATES/(name+'-fixture.in.c')).read_text()))
 exe=HERE/(name+'-fixture')
 cmd=['clang','-std=gnu11','-O2','-fsanitize=address,undefined','-fno-sanitize-recover=all','-Wno-incompatible-pointer-types','-Wno-pointer-sign',str(path),'-o',str(exe)]
 build=subprocess.run(cmd,capture_output=True,text=True)
 item={'name':name,'build_command':cmd,'build_status':build.returncode,'build_stdout':build.stdout,'build_stderr':build.stderr}
 if build.returncode==0:
  run=subprocess.run([str(exe)],capture_output=True,text=True);item.update(status=run.returncode,stdout=run.stdout,stderr=run.stderr)
  symbols=subprocess.run(['nm','-u',str(exe)],capture_output=True,text=True);item['undefined_symbols']=symbols.stdout
 results['checks'].append(item)
 print(name, 'PASS' if item.get('status')==0 else 'FAIL')
 if build.returncode:print(build.stderr)
 elif run.returncode:print(run.stdout,run.stderr)
results['extracted_functions']=spans
(HERE/'regressions-results.json').write_text(json.dumps(results,indent=2)+'\n')
failed=any(c.get('status')!=0 for c in results['checks'])
if workspace:workspace.cleanup()
sys.exit(failed)
