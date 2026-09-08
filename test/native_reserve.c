/* Exact lifted ARM64 movable-store leaves, with native allocation beneath. */
#include "reserve.h"

typedef unsigned char u8;
typedef unsigned long u64;

void *malloc(u64);
void free(void *);
int printf(const char *, ...);

static int allocation_fails;

void *memory(u64 size)
{
        return allocation_fails ? (void *)-12 : malloc(size);
}

void memory_free(void *at, u64 size)
{
        (void)size;
        if (at)
                free(at);
}

void *memory_copy(void *to, const void *from, u64 size)
{
        u8 *out = to;
        const u8 *in = from;

        for (u64 i = 0; i < size; i++)
                out[i] = in[i];

        return to;
}

int memory_reserve(void **, u64 *, u64, u64, u64, u64);
void memory_release(void **, u64 *, u64 *, u64);
u64 memory_growth(u64, u64, u64);

#define CHECK(condition) do { checks++; if (!(condition)) bad++; } while (0)

int main(void)
{
        void *held = 0;
        u64 have = 0, used = 0;
        u64 checks = 0, bad = 0;

        CHECK(memory_growth(64, 64, 16) == 64);
        CHECK(memory_growth(0, 1, 64) == 64);
        CHECK(memory_growth(64, 65, 16) == 128);
        CHECK(memory_growth(0, 1, 0) == 0);
        CHECK(memory_growth(1ul << 63, ~0ul, 1) == 0);

        CHECK(memory_reserve(&held, &have, used, 1, sizeof(u64), 64));
        CHECK(held != 0);
        CHECK(have == 64);

        u64 *words = held;
        for (used = 0; used < have; used++)
                words[used] = used ^ 0x55aa55aa55aa55aaul;

        void *first = held;
        CHECK(memory_reserve(&held, &have, used, 64, sizeof(u64), 64));
        CHECK(held == first);
        CHECK(have == 64);

        CHECK(memory_reserve(&held, &have, used, 65, sizeof(u64), 64));
        CHECK(held != 0);
        CHECK(have == 128);

        words = held;
        for (u64 i = 0; i < used; i++)
                CHECK(words[i] == (i ^ 0x55aa55aa55aa55aaul));

        memory_release(&held, &have, &used, sizeof(u64));
        CHECK(held == 0);
        CHECK(have == 0);
        CHECK(used == 0);

        CHECK(!memory_reserve(&held, &have, 0, 1, 0, 64));
        CHECK(!memory_reserve(&held, &have, 0, ~0ul, 1, 1ul << 63));
        CHECK(!memory_reserve(&held, &have, 0, 2, ~0ul, 2));
        CHECK(held == 0 && have == 0);

        allocation_fails = 1;
        CHECK(!memory_reserve(&held, &have, 0, 1, 1, 64));
        CHECK(held == 0 && have == 0);

        printf("arm64 memory reserve: %lu checks | %lu failures\n", checks, bad);
        return bad != 0;
}
