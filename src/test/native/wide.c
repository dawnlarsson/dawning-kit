/*
        The hunts at the lengths a vector body reaches, run on this machine.

        byte_hunts.c and bounded.c between them cover six routines at lengths
        up to a hundred and forty, and neither one names string_length or
        string_last_of at all: the arm64 lane has never executed either. Both
        of those grew a wide body, and a wide body that nothing long enough
        ever reaches passes every check while being wrong in every lane.

        So this is the same eight routines the differential suite carries,
        every alignment into a sixty four byte block, lengths across 32, 64,
        128, 256 and 320, and the hunted byte walked through every position
        including the last one inside a bound and the first one past it.
*/
#include "wide.h"

typedef unsigned char u8;
typedef unsigned long u64;

u64   string_length(const char *);
u64   string_length_max(const char *, u64);
char *string_first_of(const char *, int);
char *string_first_of_or_end(const char *, int);
char *string_first_of_max(const char *, u64, int);
char *string_last_of(const char *, int);
char *string_last_of_or_end(const char *, int);
void *memory_first_of(const void *, int, u64);

static u64 r_len(const char *s)
{
        const char *p = s;
        while (*p) p++;
        return (u64)(p - s);
}

static u64 r_nlen(const char *s, u64 n)
{
        u64 i = 0;
        while (i < n && s[i]) i++;
        return i;
}

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

// Nothing for the terminator, which is the whole difference between this and
// strrchr; the library says so where it defines the two.
static char *r_last(const char *s, int c)
{
        const char *best = 0;
        if (!(char)c) return 0;
        for (; *s; s++) if (*s == (char)c) best = s;
        return (char *)best;
}

static char *r_last_or_end(const char *s, int c)
{
        const char *best = 0;
        for (;; s++) {
                if (*s == (char)c) best = s;
                if (!*s) return (char *)best;
        }
}

static void *r_mchr(const void *s, int c, u64 n)
{
        const u8 *p = s;
        for (u64 i = 0; i < n; i++) if (p[i] == (u8)c) return (void *)(p + i);
        return 0;
}

int printf(const char *, ...);

static u64 checks;
static u64 e_len, e_nlen, e_first, e_or_end, e_max, e_last, e_last_end, e_mchr;

static void one(const char *text, u64 size, int c)
{
        u64 bounds[6];

        checks += 5;
        if (string_length(text)            != r_len(text))          e_len++;
        if (string_first_of(text,c)        != r_first(text,c))      e_first++;
        if (string_first_of_or_end(text,c) != r_or_end(text,c))     e_or_end++;
        if (string_last_of(text,c)         != r_last(text,c))       e_last++;
        if (string_last_of_or_end(text,c)  != r_last_or_end(text,c)) e_last_end++;

        bounds[0] = 0; bounds[1] = 1; bounds[2] = size/2;
        bounds[3] = size; bounds[4] = size+1; bounds[5] = size+40;

        for (u64 i = 0; i < 6; i++) {
                u64 n = bounds[i];
                checks += 3;
                if (string_first_of_max(text,n,c) != r_max(text,n,c))   e_max++;
                if (memory_first_of(text,c,n)     != r_mchr(text,c,n))  e_mchr++;
                if (string_length_max(text,n)     != r_nlen(text,n))    e_nlen++;
        }
}

static const u64 sizes[] = {
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,23,24,25,
        30,31,32,33,34,35,39,40,47,48,55,56,62,63,64,65,66,67,70,79,80,
        95,96,97,111,112,120,126,127,128,129,130,131,143,144,159,160,
        175,176,191,192,193,200,223,224,240,252,254,255,256,257,258,260,
        280,288,300,310,318,319,320};

#define SIZE_COUNT (sizeof(sizes)/sizeof(sizes[0]))

static const u64 offsets[] = {0,1,7,8,15,16,17,31,32,33,63};

#define OFFSET_COUNT (sizeof(offsets)/sizeof(offsets[0]))

static u8 field[1024] __attribute__((aligned(64)));

int main(void)
{
        char *base = (char *)field;

        // Every alignment a sixty four byte loop can begin on, against every
        // length in the ladder, with the byte where an edge that is off by
        // one gets it wrong.
        for (u64 off = 0; off < 72; off++)
                for (u64 s = 0; s < SIZE_COUNT; s++) {
                        u64 size = sizes[s];
                        char *text = base + off;

                        if (off + size + 48 >= sizeof(field)) continue;

                        for (u64 i = 0; i < sizeof(field); i++) field[i] = 'a';
                        text[size] = 0;

                        one(text, size, 'z');           // nowhere
                        one(text, size, 'a');           // everywhere
                        one(text, size, 0);             // the terminator

                        if (size) {
                                text[size-1] = 'z';
                                one(text, size, 'z');   // the last byte in
                                text[size-1] = 'a';
                        }

                        text[size+1] = 'z';
                        one(text, size, 'z');           // past the terminator
                        text[size+1] = 'a';

                        text[size/2] = (char)0xff;
                        one(text, size, 0xff);          // a high byte
                        text[size/2] = 'a';
                }

        // The byte at every position in turn, which is what a body that finds
        // the right vector and the wrong lane fails and nothing else does.
        for (u64 o = 0; o < OFFSET_COUNT; o++)
                for (u64 s = 0; s < SIZE_COUNT; s++) {
                        u64 off = offsets[o], size = sizes[s];
                        char *text = base + off;

                        if (off + size + 48 >= sizeof(field)) continue;

                        for (u64 i = 0; i < sizeof(field); i++) field[i] = 'a';
                        text[size] = 0;

                        for (u64 where = 0; where <= size + 2; where++) {
                                if (where == size) continue;
                                text[where] = 'z';
                                one(text, size, 'z');
                                text[where] = 'a';
                        }
                }

        printf("arm64 wide: %lu checks | length %lu | length_max %lu | first_of %lu"
               " | or_end %lu | first_max %lu | last_of %lu | last_or_end %lu | memchr %lu\n",
               checks, e_len, e_nlen, e_first, e_or_end, e_max, e_last, e_last_end, e_mchr);

        return (e_len||e_nlen||e_first||e_or_end||e_max||e_last||e_last_end||e_mchr) != 0;
}
