/*
        memset, memcpy, memmove, memfrob and the string copies, run on this machine.

        The verify lane runs the arm64 block under qemu, which proves the
        answers but not that they came out of an arm64 core. These seven have
        an aligned NEON loop with the head and the last chunk held in q
        registers across it, and the alignment step moves the source by one of
        sixty four values -- so the sweep here is size crossed with destination
        offset crossed with source offset, and the overlap distances that put a
        memmove's read on either side of its own writes.

        memory_copy branches to memory_copy_apart by name rather than carrying a
        second copy of the forward direction. Darwin spells that symbol with a
        leading underscore and extract.py writes the underscored one, so the
        plain name is set to the same address here.
*/
__asm__(".globl memory_copy_apart\n.set memory_copy_apart, _memory_copy_apart\n");
#include "bulk.h"

typedef unsigned char u8;
typedef unsigned long u64;

void *memory_fill(void *, int, u64);
void memory_exchange_apart(void *, void *, u64);
void *memory_frob(void *, u64);
void *memory_copy_apart(void *, const void *, u64);
void *memory_copy(void *, const void *, u64);
char *string_copy(char *, const char *);
char *string_copy_max(char *, const char *, u64);
char *string_cut(char *, int);
void string_replace_all(char *, int, int);

int printf(const char *, ...);

#define ROOM 2600

static u8 got[ROOM], want[ROOM], from[ROOM];
static u64 checks, bad;

// The references: what the C in library.c did before the assembly.
static void r_fill(u8 *d, int v, u64 n)
{
        while (n--)
                *d++ = (u8)v;
}

static void r_copy(u8 *d, const u8 *s, u64 n)
{
        if (d > s && d < s + n) {
                d += n - 1;
                s += n - 1;
                while (n--)
                        *d-- = *s--;
        } else
                while (n--)
                        *d++ = *s++;
}

static void r_exchange(u8 *a, u8 *b, u64 n)
{
        if (a == b)
                return;

        while (n--) {
                u8 held = *a;
                *a++ = *b;
                *b++ = held;
        }
}

static void *r_frob(u8 *p, u64 n)
{
        u8 *answer = p;

        while (n--)
                *p++ ^= 42;

        return answer;
}

static u64 r_len(const char *s)
{
        const char *p = s;
        while (*p)
                p++;
        return (u64)(p - s);
}

static char *r_first(char *s, int c)
{
        for (; *s; s++)
                if (*s == (char)c)
                        return s;
        return c ? 0 : s;
}

static void whole(const char *what, u64 size, u64 off)
{
        checks++;

        for (u64 i = 0; i < ROOM; i++)
                if (got[i] != want[i]) {
                        if (bad++ < 8)
                                printf("  FAIL %s size %lu off %lu: byte %lu is %u want %u\n",
                                       what, size, off, i, got[i], want[i]);
                        return;
                }
}

static const long gaps[] = {-1200, -513, -257, -129, -65, -64, -33, -32, -9, -8,
                            -3, -1, 1, 3, 8, 9, 32, 33, 64, 65, 129, 257, 513, 1200};

static void memory(void)
{
        for (u64 i = 0; i < ROOM; i++)
                from[i] = (u8)(i * 13 + 5);

        for (u64 size = 0; size <= 700; size++)
                for (u64 off = 0; off < 72; off++) {
                        if (off + size + 200 > ROOM)
                                continue;

                        r_fill(got, 0xA5, ROOM);
                        r_fill(want, 0xA5, ROOM);
                        memory_fill(got + off, (int)(size + 3), size);
                        r_fill(want + off, (int)(size + 3), size);
                        whole("memory_fill", size, off);

                        for (u64 so = 0; so < 9; so++) {
                                r_fill(got, 0xA5, ROOM);
                                r_fill(want, 0xA5, ROOM);
                                memory_copy_apart(got + off, from + so, size);
                                r_copy(want + off, from + so, size);
                                whole("memory_copy_apart", size, off);
                        }

                        for (u64 g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++) {
                                long gap = gaps[g];
                                u64 base = 1200 + off;

                                if ((long)base + gap < 0)
                                        continue;
                                if (base + size + 40 > ROOM)
                                        continue;
                                if (base + gap + size + 40 > ROOM)
                                        continue;

                                r_copy(got, from, ROOM);
                                r_copy(want, from, ROOM);
                                memory_copy(got + base + gap, got + base, size);
                                r_copy(want + base + gap, want + base, size);
                                whole("memory_copy", size, off);
                        }
                }
}

static void exchange_one(u64 left, u64 right, u64 size)
{
        r_fill(got, 0xA5, ROOM);
        r_fill(want, 0xA5, ROOM);

        for (u64 i = 0; i < size; i++) {
                got[left + i] = want[left + i] = (u8)(i * 37 + left);
                got[right + i] = want[right + i] = (u8)(i * 73 + right);
        }

        memory_exchange_apart(got + left, got + right, size);
        r_exchange(want + left, want + right, size);
        whole("memory_exchange_apart", size, left);
}

static void exchange(void)
{
        static const u64 large[] = {129, 255, 256, 257, 511, 512, 513, 700, 1000};

        memory_exchange_apart(0, 0, 0);
        memory_exchange_apart(0, 0, 37);

        r_fill(got, 0xA5, ROOM);
        r_fill(want, 0xA5, ROOM);
        memory_exchange_apart(got + 31, got + 31, 1000);
        whole("memory_exchange_apart equal", 1000, 31);

        for (u64 size = 0; size <= 128; size++)
                for (u64 left = 0; left < 16; left++)
                        for (u64 right = 0; right < 16; right++)
                                exchange_one(32 + left, 1400 + right, size);

        for (u64 width = 0; width < sizeof(large) / sizeof(large[0]); width++)
                for (u64 left = 0; left < 16; left++)
                        for (u64 right = 0; right < 16; right++)
                                exchange_one(32 + left, 1400 + right,
                                             large[width]);
}

static void frob(void)
{
        static const u64 widths[] = {
                0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33,
                63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512,
                700, 1000, 2048,
        };

        checks++;
        if (memory_frob(0, 0) != 0)
                bad++;

        for (u64 width = 0; width < sizeof(widths) / sizeof(widths[0]); width++)
                for (u64 off = 0; off < 32; off++) {
                        u64 size = widths[width];
                        void *answer;

                        for (u64 i = 0; i < ROOM; i++)
                                got[i] = want[i] = (u8)(i * 37 + size + off);

                        answer = memory_frob(got + 64 + off, size);
                        r_frob(want + 64 + off, size);
                        checks++;
                        if (answer != got + 64 + off)
                                bad++;
                        whole("memory_frob", size, off);

                        memory_frob(got + 64 + off, size);
                        r_frob(want + 64 + off, size);
                        whole("memory_frob twice", size, off);
                }
}

static u8 subject[ROOM], spare[ROOM], mirror[ROOM];

static void strings(void)
{
        for (u64 size = 0; size <= 700; size++)
                for (u64 off = 0; off < 72; off++) {
                        if (off + size + 200 > ROOM)
                                continue;

                        char *text = (char *)subject + off;

                        for (u64 i = 0; i < size; i++)
                                text[i] = (char)('a' + (i * 7 + off) % 4);

                        text[size] = 0;

                        // string_copy, into a poisoned buffer so a byte written
                        // past the terminator it copied is caught
                        for (u64 d = 0; d < 3; d++) {
                                r_fill(spare, 0xA5, ROOM);
                                string_copy((char *)spare + d, text);
                                checks++;
                                if (r_len((char *)spare + d) != size)
                                        bad++;
                                checks++;
                                for (u64 i = 0; i < size; i++)
                                        if (spare[d + i] != (u8)text[i]) { bad++; break; }
                                checks++;
                                for (u64 i = d + size + 1; i < ROOM; i++)
                                        if (spare[i] != 0xA5) { bad++; break; }
                        }

                        // string_copy_max at every bound around the length
                        for (u64 b = 0; b < 6; b++) {
                                u64 limit = size + 2 > b ? size + 2 - b : 0;

                                if (limit + 8 > ROOM)
                                        continue;

                                r_fill(spare, 0xA5, ROOM);
                                string_copy_max((char *)spare, text, limit);

                                u64 kept = size < limit ? size : limit;

                                checks++;
                                for (u64 i = 0; i < kept; i++)
                                        if (spare[i] != (u8)text[i]) { bad++; break; }

                                checks++;
                                if (size < limit && spare[kept] != 0)
                                        bad++;

                                checks++;
                                for (u64 i = size < limit ? kept + 1 : limit; i < ROOM; i++)
                                        if (spare[i] != 0xA5) { bad++; break; }
                        }

                        // string_cut and string_replace_all write into the
                        // string, so each gets its own copy -- at the same
                        // offset, because both walk to a block boundary before
                        // they widen and an always-aligned string never enters
                        // that walk
                        for (u64 c = 0; c < 2; c++) {
                                int symbol = c ? 'b' : 'q';
                                char *mine = (char *)spare + off;

                                r_fill(spare, 0xA5, ROOM);
                                for (u64 i = 0; i <= size; i++)
                                        mine[i] = text[i];

                                char *where = r_first(mine, symbol);
                                u64 at = where ? (u64)(where - mine) : 0;
                                char follow = where ? mine[at + 1] : 0;

                                char *answer = string_cut(mine, symbol);

                                checks++;
                                if (answer != ((!where || follow == 0) ? 0 : mine + at + 1))
                                        bad++;

                                checks++;
                                if (where && mine[at] != 0)
                                        bad++;

                                checks++;
                                for (u64 i = 0; i < ROOM; i++) {
                                        if (i >= off && i <= off + size)
                                                continue;
                                        if (spare[i] != 0xA5) { bad++; break; }
                                }

                                r_fill(spare, 0xA5, ROOM);
                                r_fill(mirror, 0xA5, ROOM);
                                for (u64 i = 0; i <= size; i++)
                                        mine[i] = mirror[off + i] = (u8)text[i];
                                for (u64 i = 0; i < size; i++)
                                        if (mirror[off + i] == (u8)symbol)
                                                mirror[off + i] = 'z';

                                string_replace_all(mine, symbol, 'z');

                                checks++;
                                for (u64 i = 0; i < ROOM; i++)
                                        if (spare[i] != mirror[i]) { bad++; break; }
                        }
                }
}

int main(void)
{
        memory();
        exchange();
        frob();
        strings();

        printf("arm64 bulk: %lu checks | failures %lu\n", checks, bad);
        return bad != 0;
}
