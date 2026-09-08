/* ARM64 memory_reverse lifted verbatim from library.c and run on the host. */
#include "reverse.h"

typedef unsigned char u8;
typedef unsigned long u64;

void *memory_reverse(void *, u64);
int printf(const char *, ...);

#define MEDIUM (1u << 15)
#define LARGE ((1u << 20) + 128)

static u8 got[MEDIUM], want[MEDIUM];
static u8 large_got[LARGE], large_want[LARGE];
static u64 checks, bad;
static u64 state = 0x2545f4914f6cdd1dull;

static u64 next(void)
{
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
}

static void fill(u8 *at, u8 value, u64 size)
{
        while (size--)
                *at++ = value;
}

static void reference(u8 *at, u64 size)
{
        u8 *end = at + size;

        while (at < end) {
                u8 value;

                end--;
                if (at >= end)
                        break;

                value = *at;
                *at++ = *end;
                *end = value;
        }
}

static void prepare(u64 extent, u64 offset, u64 size)
{
        fill(got, 0xa5, extent);
        fill(want, 0xa5, extent);

        for (u64 i = 0; i < size; i++)
                got[offset + i] = want[offset + i] = (u8)next();
}

static void same(const char *name, const u8 *left, const u8 *right, u64 size)
{
        checks++;

        for (u64 i = 0; i < size; i++)
                if (left[i] != right[i]) {
                        if (bad++ < 8)
                                printf("  FAIL %s: byte %lu is %u want %u\n",
                                       name, i, left[i], right[i]);
                        return;
                }
}

static void short_matrix(void)
{
        checks++;
        if (memory_reverse(0, 0) != 0)
                bad++;

        for (u64 size = 0; size <= 1024; size++)
                for (u64 residue = 0; residue < 64; residue++) {
                        u64 offset = 64 + residue;
                        u64 extent = offset + size + 64;

                        prepare(extent, offset, size);

                        checks++;
                        if (memory_reverse(got + offset, size) != got + offset)
                                bad++;

                        reference(want + offset, size);
                        same("small sizes/residues", got, want, extent);

                        memory_reverse(got + offset, size);
                        reference(want + offset, size);
                        same("double reverse", got, want, extent);
                }
}

static void random_matrix(void)
{
        for (u64 turn = 0; turn < 1024; turn++) {
                u64 size = next() % 16385;
                u64 offset = 33 + (next() & 63);
                u64 extent = offset + size + 71;

                prepare(extent, offset, size);
                memory_reverse(got + offset, size);
                reference(want + offset, size);
                same("random length/content", got, want, extent);
        }
}

static void page_edges(void)
{
        static const u64 sizes[] = {
            0, 1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 33,
            63, 64, 65, 255, 256, 257, 4095, 4096, 4097,
        };
        u64 base = (u64)(void *)got;
        u64 edge = ((base + 4095) & ~(u64)4095) - base + 8192;

        for (u64 c = 0; c < sizeof(sizes) / sizeof(sizes[0]); c++) {
                u64 size = sizes[c];
                u64 offset = edge - size;
                u64 extent = edge + 64;

                prepare(extent, offset, size);
                memory_reverse(got + offset, size);
                reference(want + offset, size);
                same("page-edge end", got, want, extent);
        }
}

static void megabyte(void)
{
        for (u64 i = 0; i < LARGE; i++) {
                u8 value = (u8)next();

                large_got[i] = large_want[i] = value;
        }

        memory_reverse(large_got + 37, 1u << 20);
        reference(large_want + 37, 1u << 20);
        same("one megabyte", large_got, large_want, LARGE);

        memory_reverse(large_got + 37, 1u << 20);
        reference(large_want + 37, 1u << 20);
        same("one megabyte double", large_got, large_want, LARGE);
}

int main(void)
{
        short_matrix();
        random_matrix();
        page_edges();
        megabyte();

        printf("arm64 memory_reverse: %lu checks | %lu failures\n", checks, bad);
        return bad ? 1 : 0;
}
