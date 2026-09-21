/* Semantic reference for deflate_tokens_count and deflate_tokens_encode:
   a deflate block's literals and (length, distance) pairs, counted into
   the two alphabets or written with their codes. Positions in mpos are
   offsets into src, ascending, each pair covering mlen bytes. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

typedef struct
{
        const u8 *src;          /* 0 */
        const u32 *mpos;        /* 8 */
        const u16 *mlen;        /* 16 */
        const u16 *mdist;       /* 24 */
        u64 length;             /* 32 */
        u64 pairs;              /* 40 */
        u32 *lit;               /* 48: counts, or code | length << 16 */
        u32 *dist;              /* 56 */
        u8 *out;                /* 64 */
        u64 bits;               /* 72: pending bits, or the extra-bit total */
        u64 bitn;               /* 80 */
} deflate_tokens;

static inline u32 top(u32 v) { return 31 - __builtin_clz(v); }

/* length - 3 in 0..255: symbol - 257, extra-bit count, extra value */
static inline void length_code(u32 v, u32 *code, u32 *nb, u32 *extra)
{
        if (v < 8) { *code = v; *nb = 0; *extra = 0; return; }
        if (v == 255) { *code = 28; *nb = 0; *extra = 0; return; }
        u32 h = top(v);
        *nb = h - 2;
        *code = 4 * h - 4 + ((v >> *nb) & 3);
        *extra = v & ((1u << *nb) - 1);
}

/* distance - 1 in 0..32767 */
static inline void distance_code(u32 v, u32 *code, u32 *nb, u32 *extra)
{
        if (v < 4) { *code = v; *nb = 0; *extra = 0; return; }
        u32 h = top(v);
        *nb = h - 1;
        *code = 2 * h + ((v >> *nb) & 1);
        *extra = v & ((1u << *nb) - 1);
}

void deflate_tokens_count(deflate_tokens *j)
{
        u64 at = 0, pair = 0, extra_bits = j->bits;
        for (;;)
        {
                u64 stop = pair < j->pairs ? j->mpos[pair] : j->length;
                if (stop > j->length) stop = j->length;
                while (at < stop) j->lit[j->src[at++]]++;
                if (pair >= j->pairs || at >= j->length) break;
                u32 lc, ln, le, dc, dn, de;
                length_code(j->mlen[pair] - 3u, &lc, &ln, &le);
                distance_code(j->mdist[pair] - 1u, &dc, &dn, &de);
                j->lit[257 + lc]++;
                j->dist[dc]++;
                extra_bits += ln + dn;
                at += j->mlen[pair];
                pair++;
        }
        j->bits = extra_bits;
}

#define PUT(value, count) (bits |= (u64)(value) << n, n += (count))
#define FLUSH() (*(u64 *)out = bits, out += n >> 3, bits = (n & 56) == 64 ? 0 : bits >> (n & 56), n &= 7)

void deflate_tokens_encode(deflate_tokens *j)
{
        const u8 *src = j->src;
        const u32 *lit = j->lit, *dist = j->dist;
        u8 *out = j->out;
        u64 bits = j->bits, n = j->bitn, at = 0, pair = 0;

        FLUSH();
        for (;;)
        {
                u64 stop = pair < j->pairs ? j->mpos[pair] : j->length;
                if (stop > j->length) stop = j->length;
                while (at + 3 <= stop)
                {
                        u32 a = lit[src[at]], b = lit[src[at + 1]], c = lit[src[at + 2]];
                        PUT(a & 0xffff, a >> 16);
                        PUT(b & 0xffff, b >> 16);
                        PUT(c & 0xffff, c >> 16);
                        FLUSH();
                        at += 3;
                }
                while (at < stop)
                {
                        u32 a = lit[src[at++]];
                        PUT(a & 0xffff, a >> 16);
                }
                FLUSH();
                if (pair >= j->pairs || at >= j->length) break;
                u32 lc, ln, le, dc, dn, de;
                length_code(j->mlen[pair] - 3u, &lc, &ln, &le);
                distance_code(j->mdist[pair] - 1u, &dc, &dn, &de);
                u32 l = lit[257 + lc], d = dist[dc];
                PUT(l & 0xffff, l >> 16);
                PUT(le, ln);
                PUT(d & 0xffff, d >> 16);
                PUT(de, dn);
                FLUSH();
                at += j->mlen[pair];
                pair++;
        }
        {
                u32 e = lit[256];
                PUT(e & 0xffff, e >> 16);
                FLUSH();
        }
        j->out = out;
        j->bits = bits;
        j->bitn = n;
}
