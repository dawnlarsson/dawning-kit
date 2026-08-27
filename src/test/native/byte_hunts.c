/*
        The byte hunts, run on this machine.

        Built by src/test/native/run, which lifts the arm64 bodies out of
        library.c first. Both shapes that broke them before the fix are here,
        plus a sweep of every alignment and length; see FINDING for what the
        two shapes are and why random data does not find them.
*/
#include "lifted.h"

typedef unsigned char u8;
typedef unsigned long u64;

char *string_first_of(const char *, int);
char *string_first_of_or_end(const char *, int);
char *string_first_of_max(const char *, u64, int);
char *string_find(const char *, const char *);

static char *r_first(const char *s, int c)
{
        for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; }
}

static char *r_or_end(const char *s, int c)
{
        while (*s && *s != (char)c) s++;
        return (char *)s;
}

static char *r_max(const char *s, u64 n, int c)
{
        for (u64 i = 0; i < n; i++) {
                if (s[i] == (char)c) return (char *)(s + i);
                if (!s[i]) return 0;
        }
        return 0;
}

static char *r_find(const char *h, const char *n)
{
        if (!*n) return 0;
        for (; *h; h++) {
                const char *a = h, *b = n;
                while (*a && *b && *a == *b) { a++; b++; }
                if (!*b) return (char *)h;
        }
        return 0;
}

int printf(const char *, ...);

static u64 seed = 0x2545F4914F6CDD1Dul;
static u64 next(void) { seed ^= seed<<13; seed ^= seed>>7; seed ^= seed<<17; return seed; }

int main(void)
{
        static u8 a[4096], nee[64];
        u64 checks = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0;

        // The two adjacencies a borrow out of the prefix can forge.
        for (u64 off = 1; off < 8; off++)
                for (int c = 1; c < 256; c++) {
                        for (u64 i = 0; i < off+40; i++) a[i] = 0x55;
                        a[off-1] = (u8)c; a[off] = (u8)(c+1);
                        a[off+20] = (u8)c; a[off+30] = 0;
                        checks += 3;
                        if (string_first_of((char*)a+off,c)        != r_first((char*)a+off,c)) b1++;
                        if (string_first_of_or_end((char*)a+off,c) != r_or_end((char*)a+off,c)) b2++;
                        if (string_first_of_max((char*)a+off,40,c) != r_max((char*)a+off,40,c)) b3++;

                        for (u64 i = 0; i < off+40; i++) a[i] = 0x55;
                        a[off-1] = 0x00; a[off] = 0x01;
                        a[off+20] = (u8)c; a[off+30] = 0;
                        checks += 3;
                        if (string_first_of((char*)a+off,c)        != r_first((char*)a+off,c)) b1++;
                        if (string_first_of_or_end((char*)a+off,c) != r_or_end((char*)a+off,c)) b2++;
                        if (string_first_of_max((char*)a+off,40,c) != r_max((char*)a+off,40,c)) b3++;
                }

        // Every alignment and length, over an alphabet small enough that
        // partial matches and adjacent byte values are common.
        for (u64 t = 0; t < 300000; t++) {
                u64 off = next()%64, len = next()%120 + 1;
                int c = (int)(next()%255) + 1;
                for (u64 i = 0; i < off+len+16; i++) a[i] = (u8)(next()%6) + 1;
                a[off+len] = 0;
                checks += 3;
                if (string_first_of((char*)a+off,c)         != r_first((char*)a+off,c)) b1++;
                if (string_first_of_or_end((char*)a+off,c)  != r_or_end((char*)a+off,c)) b2++;
                if (string_first_of_max((char*)a+off,len,c) != r_max((char*)a+off,len,c)) b3++;
        }

        // string_find, where a failed candidate must not eat the starts inside it.
        for (u64 t = 0; t < 200000; t++) {
                u64 off = next()%32, hl = next()%80 + 1, nl = next()%5 + 1;
                for (u64 i = 0; i < off+hl; i++) a[i] = (u8)(next()%4) + 'a';
                a[off+hl] = 0;
                for (u64 i = 0; i < nl; i++) nee[i] = (u8)(next()%4) + 'a';
                nee[nl] = 0;
                checks++;
                if (string_find((char*)a+off,(char*)nee) != r_find((char*)a+off,(char*)nee)) b4++;
        }

        printf("arm64 byte hunts: %lu checks | first_of %lu | or_end %lu | max %lu | find %lu\n",
               checks, b1, b2, b3, b4);
        return (b1||b2||b3||b4) != 0;
}
