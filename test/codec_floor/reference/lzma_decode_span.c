#include "span.h"
typedef struct {u32 range,code;const u8 *in;} rc;
#define INLINE static __attribute__((always_inline)) inline
INLINE void norm(rc *r) {if(r->range<0x1000000u) {r->range<<=8;r->code=(r->code<<8)|*r->in++;}}
INLINE u32 literal_bit(rc *r,u16 *cell) {
    norm(r);u32 p=*cell,bound=(r->range>>11)*p,one=r->code>=bound,mask=0-one;
    r->range=bound^((bound^(r->range-bound))&mask);
    r->code-=bound&mask;
    u32 p0=p+((2048-p)>>5),p1=p-(p>>5);
    *cell=p0^((p0^p1)&mask);
    return one;
}
INLINE u32 bit(rc *r,u16 *cell) {return literal_bit(r,cell);}
/* The tree helpers retain the initial depth; reverse trees are indexed by
   already-decoded low bits, matching the existing decoder's model layout. */
INLINE u32 forward(rc *r,u16 *p,u32 n) {u32 sym=1,stop=1u<<n;do {sym=(sym<<1)|bit(r,p+sym);}while(sym<stop);return sym-stop;}
INLINE u32 reverse(rc *r,u16 *p,u32 n) {u32 v=0;for(u32 i=0;i<n;i++)v|=bit(r,p+(v|(1u<<i)))<<i;return v;}
INLINE u32 length(rc *r,u16 *choice,u16 *choice2,u16 low[16][8],u16 mid[16][8],u16 *high,u32 ps) {
 if(!bit(r,choice))return 2+forward(r,low[ps],3);
 if(!bit(r,choice2))return 10+forward(r,mid[ps],3);
 return 18+forward(r,high,8);
}
INLINE u8 previous(u8 *dict,u64 size,u64 pos,u64 dist) {return dict[pos>=dist?pos-dist:pos+size-dist];}
void lzma_decode_span(job *j) {
 rc r={j->range,j->code,j->next};models *m=j->model;
 u64 pos=j->pos,full=j->full,unpacked=j->unpacked,room=j->room;
 u64 rep0=j->rep[0],rep1=j->rep[1],rep2=j->rep[2],rep3=j->rep[3];
 u32 state=j->state,psmask=(1u<<j->pb)-1,lpmask=(1u<<j->lp)-1;
 u8 *dict=j->dict;u64 size=j->size;
 while(j->limit-r.in>=64 && room>=273 && unpacked<j->stop) {
   u32 ps=(u32)unpacked&psmask,len;
   if(!bit(&r,&m->is_match[state][ps])) {
     u32 prev=full?previous(dict,size,pos,1):0;
     u16 *probs=m->lit+((((unpacked&lpmask)<<j->lc)+(prev>>(8-j->lc)))*768);
     u32 sym=1;
     if(state>=7) {
       u32 match=rep0&&rep0<=full?previous(dict,size,pos,rep0):0,offset=256;
       do {
          match<<=1;u32 mb=match&offset;
          u32 b=literal_bit(&r,probs+offset+mb+sym);
          sym=(sym<<1)|b;offset&=~(mb^(0-b));
       }while(sym<256);
     }else {
       do {sym=(sym<<1)|literal_bit(&r,probs+sym);}while(sym<256);
     }
     dict[pos++]=(u8)sym;len=1;
     state=state<4?0:state<10?state-3:state-6;
   }else {
     if(!bit(&r,&m->is_rep[state])) {
       rep3=rep2;rep2=rep1;rep1=rep0;
       len=length(&r,&m->match_choice,&m->match_choice2,m->match_low,m->match_mid,m->match_high,ps);
       u32 slot=forward(&r,m->dist_slot[len<6?len-2:3],6);
       u64 dist=slot;
       if(slot>=4) {
         u32 bits=(slot>>1)-1;
         dist=(u64)(2|(slot&1))<<bits;
         if(slot<14)dist+=reverse(&r,m->dist_special+dist-slot-1,bits);
         else {
           u32 v=0;
           for(u32 i=0;i<bits-4;i++) {norm(&r);r.range>>=1;u32 b=r.code>=r.range;r.code-=r.range&(0-b);v=(v<<1)|b;}
           dist+=((u64)v<<4)+reverse(&r,m->dist_align,4);
         }
       }
       rep0=dist+1;state=state<7?7:10;
     }else {
       if(!bit(&r,&m->is_rep0[state])) {
         if(!bit(&r,&m->is_rep0_long[state][ps])) {state=state<7?9:11;len=1;goto emit;}
       }else {
         u64 dist;
         if(!bit(&r,&m->is_rep1[state]))dist=rep1;
         else {
           if(!bit(&r,&m->is_rep2[state]))dist=rep2;
           else {dist=rep3;rep3=rep2;}
           rep2=rep1;
         }
         rep1=rep0;rep0=dist;
       }
       len=length(&r,&m->rep_choice,&m->rep_choice2,m->rep_low,m->rep_mid,m->rep_high,ps);
       state=state<7?8:11;
     }
emit:
     if(!rep0 || rep0>full || len>j->stop-unpacked) {j->error=1;break;}
     u64 from=pos>=rep0?pos-rep0:pos+size-rep0;
     u32 left=len;
     /* At a circular source boundary copy the short seed exactly. Once the
        source is contiguous, eight-byte copies preserve forward LZ overlap. */
     if(rep0<8 && left>=8) {
       for(u32 k=0;k<8;k++) {dict[pos++]=dict[from++];if(from==size)from=0;}
       left-=8;
       u64 stride=((8+rep0-1)/rep0)*rep0;
       from=pos>=stride?pos-stride:pos+size-stride;
     }
     while(left && from>size-8) {
       dict[pos++]=dict[from++];if(from==size)from=0;left--;
     }
#if defined(__riscv)
     if(!((pos|from)&7))
#endif
     while(left>=8 && from<=size-8) {
#if defined(__riscv)
       u64 word=*(u64*)(dict+from);*(u64*)(dict+pos)=word;
#else
       u64 word;__builtin_memcpy(&word,dict+from,8);
       __builtin_memcpy(dict+pos,&word,8);
#endif
       pos+=8;from+=8;left-=8;
       if(from==size)from=0;
     }
     while(left) {dict[pos++]=dict[from++];if(from==size)from=0;left--;}

   }
   full+=len;if(full>size)full=size;
   room-=len;unpacked+=len;
 }
 j->range=r.range;j->code=r.code;j->next=(u8*)r.in;
 j->pos=pos;j->full=full;j->unpacked=unpacked;j->room=room;j->state=state;
 j->rep[0]=rep0;j->rep[1]=rep1;j->rep[2]=rep2;j->rep[3]=rep3;
}
