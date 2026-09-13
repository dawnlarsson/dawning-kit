typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef struct {u32 range,code; const u8 *next,*limit;} state;
#define NORM() do { if(range < 0x1000000u) { if(in==end) goto done; range<<=8; code=(code<<8)|*in++; } } while(0)
#if 1
#define SELECT(a,b) ((a)^(((a)^(b))&mask))
#else
#define SELECT(a,b) (bit ? (b) : (a))
#endif
#define BIT(index) \
    NORM(); \
    u16 *cell=probs+(index); \
    u32 p=*cell, bound=(range>>11)*p,bit=code>=bound,mask=0-bit; \
    range=SELECT(bound,range-bound); \
    code-=bound&mask; \
    *cell=SELECT(p+((2048-p)>>5),p-(p>>5))
long long lzma_range_decode(state *s, u16 *probs, u64 mode) {
    u32 range=s->range, code=s->code;
    const u8 *in=s->next,*end=s->limit;
    u32 count=mode&255, sym=count?1:0;
    long long result=-1;
    if(mode&256) {
        u32 rev=0,position=1,left=count?count:1;
        do { BIT(rev|position);rev|=position&mask;position<<=1; }while(--left);
        result=rev;
    }else if(mode&512) {
        u32 match=(u32)(mode>>16),matched=256,left=count?count:1;
        do {
            u32 mb=(match>>(left-1))&1;
            BIT(sym+((1+mb)*matched));
            sym=(sym<<1)|bit;
            matched&=0-(u32)(mb==bit);
        }while(--left);
        result=sym-(count?1u<<count:0);
    }else {
        u32 left=count?count:1;
        do { BIT(sym);sym=(sym<<1)|bit; }while(--left);
        result=sym-(count?1u<<count:0);
    }
done:
    s->range=range;s->code=code;s->next=in;return result;
}
