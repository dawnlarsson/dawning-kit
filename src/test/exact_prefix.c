#include "../compiler_memory.c"
/*
        Literal common-prefix lengths against a byte model, with the spans at
        both edges of a mapped page.  The adjacent pages are absent, so even
        one byte read before or after the caller's exact bound faults.
*/

#define PAGE 4096
#define PROT_NONE 0

static p8 address_to one_room;
static p8 address_to two_room;
static positive checks;
static positive failures;

static positive model(const p8 address_to one, const p8 address_to two,
                      positive size)
{
        positive at = 0;

        while (at < size && one[at] == two[at])
                at++;
        return at;
}

static p8 address_to room(void)
{
        p8 address_to base = memory(PAGE * 3);

        if (!base)
                return null;
        if ((bipolar)system_call_3(syscall(mprotect), (positive)base, PAGE,
                                   PROT_NONE) < 0 ||
            (bipolar)system_call_3(syscall(mprotect),
                                   (positive)(base + PAGE * 2), PAGE,
                                   PROT_NONE) < 0)
        {
                memory_free(base, PAGE * 3);
                return null;
        }
        return base + PAGE;
}

static fn judge(string_address what, positive size, positive position,
                positive got, positive want)
{
        checks++;
        if (got != want)
        {
                failures++;
                if (failures < 20)
                        string_format(log,
                                      "FAIL %s size %p position %p: %p != %p\n",
                                      what, size, position, got, want);
        }
}

#define CHECK(N)                                                              \
        do                                                                    \
        {                                                                     \
                for (positive side = 0; side < 2; side++)                    \
                {                                                             \
                        p8 address_to one = side ? one_room + PAGE - (N)      \
                                                 : one_room;                  \
                        p8 address_to two = side ? two_room + PAGE - (N)      \
                                                 : two_room;                  \
                                                                              \
                        for (positive at = 0; at < (N); at++)                 \
                                one[at] = two[at] =                           \
                                    (p8)(at * 37 + (N) * 11);                 \
                                                                              \
                        for (positive position = 0; position <= (N);         \
                             position++)                                      \
                        {                                                     \
                                if (position < (N))                           \
                                        two[position] ^= 0xff;                \
                                positive want = model(one, two, (N));         \
                                judge((string_address)"known", (N),         \
                                      position,                               \
                                      common_prefix_known(one, two, (N)),     \
                                      want);                                  \
                                judge((string_address)"macro", (N),         \
                                      position,                               \
                                      memory_common_prefix(one, two, (N)),    \
                                      want);                                  \
                                judge((string_address)"routine", (N),       \
                                      position,                               \
                                      (memory_common_prefix)(one, two, (N)),  \
                                      want);                                  \
                                if (position < (N))                           \
                                        two[position] ^= 0xff;                \
                        }                                                     \
                }                                                             \
        } while (0)

b32 main(void)
{
        one_room = room();
        two_room = room();
        if (!one_room || !two_room)
                return 1;

        CHECK(0); CHECK(1); CHECK(2); CHECK(3); CHECK(4); CHECK(5); CHECK(6);
        CHECK(7); CHECK(8); CHECK(9); CHECK(10); CHECK(11); CHECK(12);
        CHECK(13); CHECK(14); CHECK(15); CHECK(16); CHECK(17); CHECK(18);
        CHECK(19); CHECK(20); CHECK(21); CHECK(22); CHECK(23); CHECK(24);
        CHECK(25); CHECK(26); CHECK(27); CHECK(28); CHECK(29); CHECK(30);
        CHECK(31); CHECK(32); CHECK(33); CHECK(40); CHECK(48); CHECK(64);
        CHECK(80); CHECK(96); CHECK(127); CHECK(128); CHECK(160);

        string_format(log, "common-prefix literals: %p checks, %p failures\n",
                      checks, failures);
        log_flush();
        return failures != 0;
}
