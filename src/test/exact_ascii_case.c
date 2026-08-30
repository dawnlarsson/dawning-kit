#include "../compiler_memory.c"
/*
        Every literal size admitted by the folded ASCII comparison.

        Blocks are placed against the end of mapped pages so even one byte of
        over-read faults.  Each size exercises equality under case changes,
        every differing position, high bytes that must not be folded, and the
        public macro beside the out-of-line routine and a byte model.
*/

#define PAGE 4096
#define PROT_NONE 0

static p8 address_to one_edge;
static p8 address_to two_edge;
#include "counted.inc"

static p8 model_upper(p8 value)
{
        return value >= 'a' && value <= 'z' ? (p8)(value - 32) : value;
}

static b32 model(const p8 address_to one, const p8 address_to two,
                 positive size)
{
        for (positive at = 0; at < size; at++)
        {
                p8 left = model_upper(one[at]);
                p8 right = model_upper(two[at]);

                if (left != right)
                        return (b32)left - (b32)right;
        }

        return 0;
}

static p8 address_to room(void)
{
        p8 address_to base = memory(PAGE * 2);

        system_call_3(syscall(mprotect), (positive)(base + PAGE), PAGE,
                      PROT_NONE);
        return base + PAGE;
}

static fn judge(string_address what, positive size, positive position,
                b32 got, b32 want)
{
        checks++;
        if (got != want)
        {
                failures++;
                if (failures < 20)
                        string_format(log, "FAIL %s size %p position %p: %b != %b\n",
                                      what, size, position, got, want);
        }
}

#define CHECK(N)                                                              \
        do                                                                    \
        {                                                                     \
                p8 address_to one = one_edge - (N);                           \
                p8 address_to two = two_edge - (N);                           \
                                                                              \
                for (positive at = 0; at < (N); at++)                         \
                {                                                             \
                        p8 value = (p8)(at * 37 + (N) * 11);                  \
                        one[at] = value;                                       \
                        two[at] = value >= 'A' && value <= 'Z'                \
                                      ? (p8)(value + 32)                      \
                                      : value;                                \
                }                                                             \
                                                                              \
                judge((string_address)"known equal", (N), (N),              \
                      compare_ascii_case_known(one, two, (N)),                \
                      model(one, two, (N)));                                  \
                judge((string_address)"macro equal", (N), (N),              \
                      memory_compare_ascii_case(one, two, (N)),               \
                      model(one, two, (N)));                                  \
                                                                              \
                for (positive position = 0; position < (N); position++)       \
                {                                                             \
                        p8 saved = two[position];                              \
                        two[position] = saved == 0xff ? 0x80 : (p8)(saved + 1); \
                        judge((string_address)"known difference", (N),       \
                              position,                                       \
                              compare_ascii_case_known(one, two, (N)),        \
                              model(one, two, (N)));                          \
                        judge((string_address)"macro difference", (N),       \
                              position,                                       \
                              memory_compare_ascii_case(one, two, (N)),       \
                              model(one, two, (N)));                          \
                        two[position] = saved;                                \
                }                                                             \
        } while (0)

b32 main(void)
{
        one_edge = room();
        two_edge = room();

        if (!one_edge || !two_edge)
                return 1;

        for (positive left = 0; left < 256; left++)
                for (positive right = 0; right < 256; right++)
                {
                        p8 one = (p8)left;
                        p8 two = (p8)right;

                        judge((string_address)"all byte pairs", 1, 0,
                              compare_ascii_case_known(address_of one,
                                                       address_of two, 1),
                              model(address_of one, address_of two, 1));
                }

        CHECK(0); CHECK(1); CHECK(2); CHECK(3); CHECK(4); CHECK(5); CHECK(6);
        CHECK(7); CHECK(8); CHECK(9); CHECK(10); CHECK(11); CHECK(12);

        return test_report((string_address) "ASCII-case literal sizes: ");
}
