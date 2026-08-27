/*
        The four routines arm64 did not have, run on this machine.

        string_last_of_or_end, string_compare_max, string_length_max and
        memory_first_of were written for this block rather than lifted from
        anywhere, so every one of them is new code and none of it has ever
        run before. The references below are what libc means by strrchr,
        strncmp, strnlen and memchr.
*/
#include "lifted.h"

typedef unsigned char u8;
typedef unsigned long u64;

char *string_last_of_or_end(const char *, int);
int   string_compare_max(const char *, const char *, u64);
u64   string_length_max(const char *, u64);
void *memory_first_of(const void *, int, u64);

static char *r_last(const char *s, int c)
{
        const char *best = 0;
        for (;; s++) {
                if (*s == (char)c) best = s;
                if (!*s) return (char *)best;
        }
}

static int r_ncmp(const char *a, const char *b, u64 n)
{
        for (u64 i = 0; i < n; i++) {
                u8 x = (u8)a[i], y = (u8)b[i];
                if (x != y) return (int)x - (int)y;
                if (!x) return 0;
        }
        return 0;
}

static u64 r_nlen(const char *s, u64 n)
{
        u64 i = 0;
        while (i < n && s[i]) i++;
        return i;
}

static void *r_mchr(const void *s, int c, u64 n)
{
        const u8 *p = s;
        for (u64 i = 0; i < n; i++) if (p[i] == (u8)c) return (void *)(p + i);
        return 0;
}

int printf(const char *, ...);

static u64 seed = 0x9E3779B97F4A7C15ul;
static u64 next(void) { seed ^= seed<<13; seed ^= seed>>7; seed ^= seed<<17; return seed; }

static int sign(int v) { return v < 0 ? -1 : v > 0 ? 1 : 0; }

int main(void)
{
        static u8 a[4096], b[4096];
        u64 checks = 0, e1 = 0, e2 = 0, e3 = 0, e4 = 0;

        // Every alignment, every length across the word boundaries, and an
        // alphabet small enough that repeats and adjacent values are common.
        for (u64 t = 0; t < 300000; t++) {
                u64 off = next()%64, len = next()%140;
                int c = (int)(next()%255) + 1;

                for (u64 i = 0; i < off+len+16; i++) a[i] = (u8)(next()%5) + 1;
                a[off+len] = 0;

                checks += 3;
                if (string_last_of_or_end((char*)a+off,c) != r_last((char*)a+off,c)) e1++;
                if (string_length_max((char*)a+off,len)   != r_nlen((char*)a+off,len)) e3++;
                if (memory_first_of(a+off,c,len)          != r_mchr(a+off,c,len)) e4++;

        }

        // string_compare_max wants a pair, built so ties and differences are
        // both common: copy, then poke one byte.
        for (u64 t = 0; t < 300000; t++) {
                u64 ao = next()%32, bo = next()%32, len = next()%80 + 1;
                for (u64 i = 0; i < len+8; i++) a[ao+i] = (u8)(next()%4) + 'a';
                a[ao+len] = 0;
                for (u64 i = 0; i < len+8; i++) b[bo+i] = a[ao+i];
                if (next() & 3) b[bo + next()%(len+1)] = (u8)(next()%4) + 'a';
                u64 n = next()%(len+4);
                checks++;
                if (sign(string_compare_max((char*)a+ao,(char*)b+bo,n))
                    != sign(r_ncmp((char*)a+ao,(char*)b+bo,n))) e2++;
        }

        // the ends: nothing to look at, and a hit in the very last byte
        for (u64 len = 0; len < 40; len++)
                for (u64 off = 0; off < 16; off++) {
                        for (u64 i = 0; i < len+8; i++) a[off+i] = 0xAA;
                        if (len) a[off+len-1] = 0x5A;
                        a[off+len] = 0x5A;              // just past: must not be seen
                        checks += 2;
                        if (memory_first_of(a+off,0x5A,len) != r_mchr(a+off,0x5A,len)) e4++;
                        a[off+len] = 0;
                        if (string_length_max((char*)a+off,len) != r_nlen((char*)a+off,len)) e3++;
                }

        printf("arm64 bounded: %lu checks | last_of_or_end %lu | compare_max %lu | length_max %lu | first_of %lu\n",
               checks, e1, e2, e3, e4);
        return (e1||e2||e3||e4) != 0;
}
