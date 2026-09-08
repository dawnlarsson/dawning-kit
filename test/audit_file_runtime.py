#!/usr/bin/env python3
"""Linux CLI regressions; every mutated path is inside one owned temporary directory."""
import argparse,hashlib,json,os,pathlib,subprocess,tempfile,sys
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--shell',required=True,type=pathlib.Path)
parser.add_argument('--source',type=pathlib.Path)
parser.add_argument('--output',type=pathlib.Path)
args=parser.parse_args();binary=args.shell.resolve()
results={'binary':str(binary),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'cases':[],'safety':'All mutation operands are owned temporary paths. No rm operand or symlink target is root-equivalent. Root identity cases are exclusively in the mocked fixtures.','limits':['Find signed node magnitudes above INT64_MAX are explicitly rejected; GNU supports some larger magnitudes.','System filesystem and timestamp ranges apply to CLI cases; signed extrema and Z/W are covered synthetically.']}
if args.source:results['source_sha256']=hashlib.sha256(args.source.read_bytes()).hexdigest()
def run(tool,words,exe=None,stdin=None):
 p=subprocess.run([tool]+list(map(str,words)),executable=str(exe or binary),input=stdin,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=5,env={'PATH':'/usr/bin:/bin','LC_ALL':'C','TZ':'UTC'})
 return {'status':p.returncode,'stdout':p.stdout.decode(errors='backslashreplace'),'stderr':p.stderr.decode(errors='backslashreplace')}
def same(label,ours,ref,fields=('status','stdout')):
 good=all(ours[k]==ref[k] for k in fields)
 results['cases'].append({'case':label,'ours':ours,'reference':ref,'equal_fields':list(fields),'passed':good})
 if not good:print('FAIL',label,ours,ref,file=sys.stderr)
with tempfile.TemporaryDirectory(prefix='dawning-file-regressions-') as tmp:
 root=pathlib.Path(tmp);target=root/'target';reference=root/'reference';target.write_text('x');reference.write_text('ref')
 for flags in [[],['-a'],['-m'],['-a','-m'],['--time=atime'],['--time=mtime']]:
  for date in [None,'1 second','-1 day','now','1970-01-01','1970-01-01 00:00:03.123456789','@-1.5','2000-02-29','Sep 9 2001','9 Sep 2001','monday','']:
   answers=[]
   for exe in [binary,'/usr/bin/touch']:
    os.utime(reference,ns=(1000000001,2000000002));os.utime(target,ns=(3000000003,4000000004))
    words=flags+['-r',str(reference)]+([] if date is None else ['-d',date])+[str(target)]
    answer=run('touch',words,exe);stat=target.stat();answer['times']=[stat.st_atime_ns,stat.st_mtime_ns];answers.append(answer)
   same('touch-reference-'+str(flags)+'-'+repr(date),*answers,fields=('status','times'))
 for stamp in ['202602310000','202601010000.00junk','190002290000','210002290000','202604310000','202601012400','202601010060','202601010000.0','202601010000.000','202601010000.61','202601010000.99','200002290000','202402290000','202602280000','202604300000','197001010000','6901010000','6801010000','202601010000.60','202601010000.59']:
  answers=[]
  for exe in [binary,'/usr/bin/touch']:
   os.utime(target,ns=(3000000003,4000000004));answer=run('touch',['-t',stamp,target],exe);stat=target.stat();answer['times']=[stat.st_atime_ns,stat.st_mtime_ns];answers.append(answer)
  same('touch-compact-'+stamp,*answers,fields=('status','times'))
 link=root/'link';link.symlink_to(target)
 for flag in ['-h','-a','-m']:
  answers=[]
  for exe in [binary,'/usr/bin/touch']:
   os.utime(target,ns=(3000000003,4000000004));os.utime(link,ns=(5000000005,6000000006),follow_symlinks=False)
   answer=run('touch',[flag,'-r',reference,link],exe);s=link.lstat();t=target.stat();answer['times']=([s.st_atime_ns,s.st_mtime_ns,t.st_atime_ns,t.st_mtime_ns] if flag=='-h' else [s.st_mtime_ns,t.st_atime_ns,t.st_mtime_ns]);answers.append(answer)
  same('touch-link-'+flag,*answers,fields=('status','times'))
 for flag in ['-c','-t']:
  missing=root/'touch-missing';words=([flag] if flag=='-c' else [flag,'202602310000'])+[str(missing)]
  answer=run('touch',words);answer['exists']=missing.exists();results['cases'].append({'case':'touch-absent-'+flag,'ours':answer,'passed':not answer['exists'] and answer['status']==(flag=='-t')})
 for seconds in [-2147483649,-1,0,1,2147483648]:
  os.utime(target,ns=(seconds*10**9,seconds*10**9+999999999))
  same('stat-epochs-'+str(seconds),run('stat',['-c','%X:%Y',target]),run('stat',['-c','%X:%Y',target],'/usr/bin/stat'))
 same('stat-format',run('stat',['-c',r'%n\t%X:%Y:%%:%x',target]),run('stat',['-c',r'%n\t%X:%Y:%%:%x',target],'/usr/bin/stat'))
 os.utime(target,ns=(1000000000,2000000000));target.chmod(0o644)
 for predicate in ['-maxdepth','-mindepth','-size','-links','-inum','-mtime','-mmin']:
  for value in ['', '+', '-', 'garbage', '1garbage', '1x', '1cc', '18446744073709551616','0','1','+1','-1']:
   # GNU date-count parser accepts arbitrarily large durations; ours explicitly bounds the signed node.
   if predicate in ['-mtime','-mmin'] and value=='18446744073709551616':continue
   words=[target,predicate,value,'-print'];same('find-'+predicate+'-'+repr(value),run('find',words),run('find',words,'/usr/bin/find'))
 for suffix in ['b','c','w','k','M','G']:
  words=[target,'-size','1'+suffix,'-print'];same('find-size-suffix-'+suffix,run('find',words),run('find',words,'/usr/bin/find'))
 for mode in ['/0','/000','/a=','-0','0','/400','/111','/u=r','/u=x','-a=']:
  words=[target,'-perm',mode,'-print'];same('find-perm-'+mode,run('find',words),run('find',words,'/usr/bin/find'))
 for predicate in ['-size','-links','-inum','-mtime']:
  answer=run('find',[target,predicate,'9223372036854775808','-print']);results['cases'].append({'case':'find-signed-bound-'+predicate,'ours':answer,'passed':answer['status']==1 and not answer['stdout']})
 for label,flags,stdin in [('recursive',['-rf'],None),('missing',['-f'],None),('declined',['-ri'],b'n\n'),('unreadable',['-rf'],None)]:
  answers=[]
  for i,exe in enumerate([binary,'/usr/bin/rm']):
   directory=root/('rm-'+label+'-'+str(i));directory.mkdir();child=directory/'child';child.write_text('x')
   if label=='missing':operand=directory/'absent'
   else:operand=directory
   if label=='unreadable':directory.chmod(0)
   answer=run('rm',flags+[str(operand)],exe,stdin)
   if directory.exists():directory.chmod(0o700)
   answer['remaining']=[directory.exists(),child.exists()];answers.append(answer)
  same('rm-owned-'+label,*answers,fields=('status','remaining'))
results['passed']=all(c['passed'] for c in results['cases'])
text=json.dumps(results,indent=2)+'\n'
if args.output:args.output.write_text(text)
else:print(text,end='')
raise SystemExit(not results['passed'])
