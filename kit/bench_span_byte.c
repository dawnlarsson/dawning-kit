/* Bounded equal-byte prefix: scalar caller loop against library assembly. */
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline))
#define TRIES 9
#define MAXIMUM (1u << 20)
#define TARGET_BYTES (1u << 25)

static p8 block[MAXIMUM + 16];
static volatile positive sink;

NOT_INLINED static positive former_span(address_any address, p8 value,
                                        positive length)
{
        p8 address_to bytes = address;
        positive at = 0;

        while (at < length && bytes[at] == value)
                at++;

        return at;
}

typedef positive (*span_call)(address_any, p8, positive);
static span_call volatile span_calls[2] = {former_span, memory_span_byte};

static bool correctness(void)
{
        static const positive long_lengths[] = {255, 256, 257, 4095};

        for (positive value = 0; value < 256; value++)
                for (positive offset = 0; offset < 16; offset++)
                        for (positive length = 0; length <= 20; length++)
                                for (positive mismatch = 0; mismatch <= length; mismatch++)
                                {
                                        memory_fill(block + offset, (p8)value, length);
                                        if (mismatch < length)
                                                block[offset + mismatch] = (p8)(value + 1);
                                        if (memory_span_byte(block + offset, (p8)value,
                                                             length) != mismatch)
                                                return false;
                                }

        for (positive value = 0; value < 256; value++)
                for (positive offset = 0; offset < 16; offset++)
                        for (positive li = 0;
                             li < sizeof(long_lengths) / sizeof(*long_lengths); li++)
                        {
                                positive length = long_lengths[li];
                                positive positions[] = {0, 1, 15, 16, length / 2,
                                                        length - 1, length};

                                for (positive pi = 0;
                                     pi < sizeof(positions) / sizeof(*positions); pi++)
                                {
                                        positive mismatch = positions[pi];

                                        memory_fill(block + offset, (p8)value, length);
                                        if (mismatch < length)
                                                block[offset + mismatch] = (p8)(value + 1);
                                        if (memory_span_byte(block + offset, (p8)value,
                                                             length) != mismatch)
                                                return false;
                                }
                        }
        return true;
}

static positive rounds_for(positive n){positive r=TARGET_BYTES/(n?n:1);if(r<8)r=8;if(r>(1u<<22))r=1u<<22;return r;}
static p64 run(bool assembly,positive n,positive rounds){p64 start=get_cpu_time();while(rounds--)sink+=span_calls[assembly](block,'0',n);return get_cpu_time()-start;}
static fn order(positive address_to q){for(positive i=1;i<TRIES;i++){positive v=q[i],j=i;while(j&&q[j-1]>v){q[j]=q[j-1];j--;}q[j]=v;}}
static fn row(positive n,bool late){positive q[TRIES],r=rounds_for(n);memory_fill(block,'0',n);if(late&&n)block[n-1]='1';for(positive t=0;t<TRIES;t++){p64 a,b;if(t&1){b=run(true,n,r);a=run(false,n,r);}else{a=run(false,n,r);b=run(true,n,r);}q[t]=(positive)(b*10000/(a?a:1));}order(q);string_format(log,"  %s %p bytes  asm/C %p.%p%%\n",late?(string_address)"late":(string_address)"equal",n,q[TRIES/2]/100,q[TRIES/2]%100);}

b32 main(void)
{
        static const positive sizes[]={8,16,24,32,64,128,256,4096,MAXIMUM};
        if(!correctness()){string_format(log,"memory_span_byte correctness failed\n");log_flush();return 1;}
        string_format(log,"memory_span_byte, paired median of %p\n",(positive)TRIES);
        for(positive i=0;i<sizeof(sizes)/sizeof(*sizes);i++){row(sizes[i],false);row(sizes[i],true);}
        log_flush();return 0;
}
