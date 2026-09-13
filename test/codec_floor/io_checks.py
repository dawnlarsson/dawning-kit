import pathlib,tempfile,subprocess,shlex,sys,os
binary=pathlib.Path(sys.argv[1]).resolve();runner=shlex.split(sys.argv[2]) if len(sys.argv)>2 else [];checks=0
with tempfile.TemporaryDirectory() as temporary:
 root=pathlib.Path(temporary);data=bytes(range(256))*256
 for codec,ext,level in [('gzip','.gz','6'),('xz','.xz','1'),('zstd','.zst','3')]:
  exe=root/codec;exe.symlink_to(binary);command=runner+[str(exe)]
  packed=subprocess.run([codec,'-c','-'+level],input=data,capture_output=True,check=True).stdout
  for args,source in [(['-c','-'+level],data),(['-dc'],packed)]:
   with open('/dev/full','wb') as sink:p=subprocess.run(command+args,input=source,stdout=sink,stderr=subprocess.PIPE)
   assert p.returncode!=0,(codec,args,'write error ignored');checks+=1
  plain=root/(codec+'-input');out=pathlib.Path(str(plain)+ext);plain.write_bytes(data);out.write_bytes(b'keep me')
  p=subprocess.run(command+[str(plain)],capture_output=True);assert p.returncode and out.read_bytes()==b'keep me' and plain.exists(),(codec,'existing output');checks+=1
  out.unlink();victim=root/(codec+'-target');victim.write_bytes(b'keep me');out.symlink_to(victim)
  p=subprocess.run(command+['-f',str(plain)],capture_output=True);assert p.returncode and victim.read_bytes()==b'keep me' and plain.exists(),(codec,'force symlink');checks+=1
print(f'{checks}/{checks} codec output-error checks passed')
