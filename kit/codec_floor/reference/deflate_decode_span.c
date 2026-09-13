typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef struct {u64 bits, count; const u8 *next,*limit; u8 *out,*out_limit; u64 history; const u16 *lit,*dist; const u32 *lengths,*distances;} job;
struct word {u64 v;} __attribute__((packed,may_alias));
void deflate_decode_span(job *s) {
    u64 bits=s->bits, count=s->count;
    const u8 *in=s->next;
    u8 *out=s->out, *start=out;
    while(s->limit-in>=8 && s->out_limit-out>=258) {
        if(count<48) {
            unsigned n=(63-count)>>3;
            bits |= ((const struct word *)in)->v << count;
            in+=n;count+=n*8;
        }
        u32 cell=s->lit[bits&2047], used=cell>>9, symbol=cell&511;
        if(!used || symbol==256 || symbol>285)break;
        if(symbol<256) {
            *out++=symbol;bits>>=used;count-=used;
            /* A second independent literal amortizes loop and refill checks.
               The first refill left >=33 bits and >=257 output bytes. */
            cell=s->lit[bits&2047];used=cell>>9;symbol=cell&511;
            if(used && symbol<256) {*out++=symbol;bits>>=used;count-=used;}
            continue;
        }
        u64 saved=bits, before=count;
        bits>>=used;count-=used;
        u32 info=s->lengths[symbol-257], extra=info>>16;
        u64 length=(info&65535)+(bits&((1u<<extra)-1));
        bits>>=extra;count-=extra;
        cell=s->dist[bits&255];used=cell>>9;symbol=cell&511;
        if(!used || symbol>=30){bits=saved;count=before;break;}
        bits>>=used;count-=used;
        info=s->distances[symbol];extra=info>>16;
        u64 dist=(info&65535)+(bits&((1u<<extra)-1));
        if(dist>s->history+(u64)(out-start)){bits=saved;count=before;break;}
        bits>>=extra;count-=extra;
        while(dist<8 && length) {
            u64 n=dist<length?dist:length;
            for(u64 i=0;i<n;i++)out[i]=out[i-dist];
            out+=n;length-=n;dist*=2;
        }
        while(length>=8) {
            ((struct word *)out)->v=((const struct word *)(out-dist))->v;
            out+=8;length-=8;
        }
        while(length){*out=out[-dist];out++;length--;}
    }
    s->bits=bits&(((u64)1<<count)-1);s->count=count;s->next=in;s->out=out;
}
