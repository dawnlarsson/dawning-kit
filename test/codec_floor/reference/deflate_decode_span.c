/* Reference for deflate_decode_span; the library bodies started as gcc -O2
   -fno-tree-vectorize output of this file (x86_64 -mno-red-zone) and were
   then tuned by hand. The vectorizer must stay off: at -O2 gcc 15 merges the
   overlapping word stores of a match with offset 8..31 into one wide load,
   which reads bytes the first store has not written yet.
   Entries are the packed u32 cells gzip.c builds:
     literal   0x80000000 | byte << 8 | codeword bits
     length    length << 16 | (codeword + extra bits), one cell per extra value
     distance  base << 16 | codeword bits << 8 | (codeword + extra bits)
     end       0x8000 | 0x2000 | codeword bits
     subtable  start << 16 | 0x8000 | 0x4000 | subtable bits << 8 | root bits
     invalid   0x8000
   Subtable cells count only the bits past the root; masks[n] is (1 << n) - 1. */
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef struct {
        u64 bits, count;
        const u8 *next, *limit;
        u8 *out, *out_limit;
        const u8 *window;
        const u32 *litlen, *offset, *masks;
        u64 status;
} job;
struct word { u64 v; } __attribute__((packed, may_alias));
#define LOAD(p) (((const struct word *)(p))->v)
#define STORE(p, x) (((struct word *)(p))->v = (x))
#define REFILL() do { \
        bitbuf |= LOAD(in) << (u8)bitsleft; \
        in += 7 - (((u8)bitsleft >> 3) & 7); \
        bitsleft |= 56; \
} while (0)

void deflate_decode_span(job *s)
{
        u64 bitbuf = s->bits, bitsleft = s->count, saved, status = 0;
        const u8 *in = s->next;
        u8 *out = s->out;
        const u32 *lt = s->litlen, *ot = s->offset;
        u32 entry;
        if (s->limit - in < 33 || s->out_limit - out < 301)
                goto done;
        const u8 *in_end = s->limit - 32;
        u8 *out_end = s->out_limit - 300;
        REFILL();
        entry = lt[bitbuf & 2047];
        do {
                u64 length, offset;
                const u8 *src;
                u8 *dst;
                saved = bitbuf;
                bitbuf >>= (u8)entry;
                bitsleft -= entry;
                if ((int)entry < 0) {
                        *out++ = (u8)(entry >> 8);
                        entry = lt[bitbuf & 2047];
                        saved = bitbuf;
                        bitbuf >>= (u8)entry;
                        bitsleft -= entry;
                        if ((int)entry < 0) {
                                *out++ = (u8)(entry >> 8);
                                entry = lt[bitbuf & 2047];
                                saved = bitbuf;
                                bitbuf >>= (u8)entry;
                                bitsleft -= entry;
                                if ((int)entry < 0) {
                                        *out++ = (u8)(entry >> 8);
                                        entry = lt[bitbuf & 2047];
                                        REFILL();
                                        continue;
                                }
                        }
                }
                if (entry & 0x8000) {
                        if (entry & 0x2000) { status = 1; goto done; }
                        if (!(entry & 0x4000)) { status = 2; goto done; }
                        entry = lt[(entry >> 16) + (bitbuf & s->masks[(entry >> 8) & 15])];
                        saved = bitbuf;
                        bitbuf >>= (u8)entry;
                        bitsleft -= entry;
                        if ((int)entry < 0) {
                                *out++ = (u8)(entry >> 8);
                                entry = lt[bitbuf & 2047];
                                REFILL();
                                continue;
                        }
                        if (entry & 0x8000) {
                                status = (entry & 0x2000) ? 1 : 2;
                                goto done;
                        }
                }
                length = entry >> 16;
                if ((u8)bitsleft < 31)
                        REFILL();
                entry = ot[bitbuf & 255];
                if (entry & 0x8000) {
                        if (!(entry & 0x4000)) { status = 3; goto done; }
                        if ((u8)bitsleft < 38)
                                REFILL();
                        bitbuf >>= 8;
                        bitsleft -= 8;
                        entry = ot[(entry >> 16) + (bitbuf & s->masks[(entry >> 8) & 15])];
                        if (entry & 0x8000) { status = 3; goto done; }
                }
                saved = bitbuf;
                bitbuf >>= (u8)entry;
                bitsleft -= entry;
                offset = (entry >> 16) + ((saved & s->masks[(u8)entry]) >> ((entry >> 8) & 255));
                src = out - offset;
                if (src < s->window) { status = 4; goto done; }
                dst = out;
                out += length;
                entry = lt[bitbuf & 2047];
                REFILL();
                if (offset >= 8) {
                        do {
                                STORE(dst, LOAD(src));
                                STORE(dst + 8, LOAD(src + 8));
                                STORE(dst + 16, LOAD(src + 16));
                                STORE(dst + 24, LOAD(src + 24));
                                src += 32;
                                dst += 32;
                        } while (dst < out);
                } else if (offset == 1) {
                        u64 v = 0x0101010101010101ull * src[0];
                        do {
                                STORE(dst, v);
                                STORE(dst + 8, v);
                                STORE(dst + 16, v);
                                STORE(dst + 24, v);
                                dst += 32;
                        } while (dst < out);
                } else {
                        do {
                                STORE(dst, LOAD(src));
                                src += offset;
                                dst += offset;
                                STORE(dst, LOAD(src));
                                src += offset;
                                dst += offset;
                        } while (dst < out);
                }
        } while (in < in_end && out < out_end);
done:
        bitsleft = (u8)bitsleft;
        s->bits = bitbuf & ~(~0ull << bitsleft);
        s->count = bitsleft;
        s->next = in;
        s->out = out;
        s->status = status;
}
