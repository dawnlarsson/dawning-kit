from pathlib import Path
import subprocess
root=Path('artifacts/codec-floor-native');root.mkdir(parents=True,exist_ok=True)
head=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
typedef struct {u16 next;u8 extra,nbits;u32 base;} cell;
typedef struct {u8 log,pad[7];cell cells[512];} table;
typedef struct {u8 *window;u64 pos,window_size;u8 *lits;u64 lit_len;u8 *seq;u64 seq_len;table *ll,*of,*ml;u32 *rep;u64 nseq;u8 *output_end;} job;
extern long zstd_sequences_run(job*);
void *floor_copy(void *d,void *s,u64 n) __asm__("_memory_copy_apart");
void *floor_copy(void *d,void *s,u64 n) { return memcpy(d,s,n); }
void *floor_fill(void *d,u8 v,u64 n) __asm__("_memory_fill");
void *floor_fill(void *d,u8 v,u64 n) { return memset(d,v,n); }
static void put(u8 *p,u64 *at,u32 v,u32 n) {for(u32 i=0;i<n;i++,(*at)++)p[*at/8]|=((v>>i)&1)<<(*at%8);}
static u32 value(u32 i,u32 factor,u32 n){return (i*factor+3)&((1u<<n)-1);}
int main(void){
 u64 page=getpagesize();u8 *mapping=mmap(0,3*page,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);if(mapping==MAP_FAILED)return 2;
 mprotect(mapping+page,page,PROT_READ|PROT_WRITE);
 static u8 packed[8192],lits[16384],expect[65536],output[65536];
 u32 ofbits[]={0,1,2,5,10};unsigned checks=0;
 for(u32 c=0;c<5;c++)for(u32 lb=0;lb<=4;lb++)for(u32 mb=0;mb<=4;mb++)for(u32 count=1;count<=512;count*=8){
  u32 ob=ofbits[c],rep[3]={2000,1999,1998},oracle_rep[3]={2000,1999,1998};
  table ll={0},of={0},ml={0};ll.cells[0].extra=lb;ml.cells[0].extra=mb;ml.cells[0].base=3;of.cells[0].extra=ob;of.cells[0].base=ob>=2?(1u<<ob)-3:0;
  memset(packed,0,sizeof(packed));u64 bit=0;
  for(u32 i=count;i--;){put(packed,&bit,value(i,7,lb),lb);put(packed,&bit,value(i,11,mb),mb);put(packed,&bit,value(i,13,ob),ob);}
  put(packed,&bit,1,1);u64 size=(bit+7)/8;u8 *seq=mapping+2*page-size;memcpy(seq,packed,size);
  for(u32 i=0;i<4096;i++)expect[i]=(u8)(i*97+13);
  for(u32 i=0;i<sizeof(lits);i++)lits[i]=(u8)(i*19+7);
  u64 pos=4096,lit=0;
  for(u32 i=0;i<count;i++){
   u32 ln=value(i,7,lb),mn=3+value(i,11,mb),ov=value(i,13,ob),dist;
   if(ob>=2){dist=(1u<<ob)-3+ov;oracle_rep[2]=oracle_rep[1];oracle_rep[1]=oracle_rep[0];oracle_rep[0]=dist;}
   else if(!ob){dist=oracle_rep[ln==0];oracle_rep[1]=oracle_rep[ln!=0];oracle_rep[0]=dist;}
   else {u32 which=1+(ln==0)+ov;dist=which==3?oracle_rep[0]-1:oracle_rep[which];if(which!=1)oracle_rep[2]=oracle_rep[1];oracle_rep[1]=oracle_rep[0];oracle_rep[0]=dist;}
   if(!dist||dist>pos+ln)return 3;
   memcpy(expect+pos,lits+lit,ln);pos+=ln;lit+=ln;
   for(u32 k=0;k<mn;k++){expect[pos]=expect[pos-dist];pos++;}
  }
  memset(output,0x5a,sizeof(output));memcpy(output,expect,4096);
  job j={output,4096,4096,lits,lit,seq,size,&ll,&of,&ml,rep,count,output+pos};
  if(zstd_sequences_run(&j)||j.pos!=pos||memcmp(expect,output,pos)||memcmp(rep,oracle_rep,sizeof(rep))||output[pos]!=0x5a){fprintf(stderr,"FAIL %u %u %u %u\n",ob,lb,mb,count);return 1;}
  checks++;
 }
 printf("%u/%u zstd sequence fixtures passed on native arm64\n",checks,checks);return 0;
}
'''
asm=subprocess.check_output(['python3','test/differential.py','--harness','native_extract','src/library.c','zstd_bits_open','zstd_bits_reload','zstd_sequences_run','memory_copy_match'],text=True)
macro=next(line for line in Path('src/library.c').read_text().splitlines() if line.startswith('#define ZSTD_SEQ_GET_ARM64'))
(root/'native-zstd.c').write_text(macro+'\n'+head+asm)
subprocess.run(['clang','-O2',str(root/'native-zstd.c'),'-o',str(root/'native-zstd')],check=True)
subprocess.run([str(root/'native-zstd')],check=True)
