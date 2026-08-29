/* malloc/free class fast path against its call and free-list traffic floors. */
#include "../src/compiler_memory.c"

#define ALLOCATOR_BENCH_ROUNDS (1u << 22)
#define ALLOCATOR_BENCH_TRIES 7
#define ALLOCATOR_BENCH_SIZE 64

typedef fn (*allocator_bench_work)();
typedef address_any (*allocator_bench_take_call)(positive);
typedef fn (*allocator_bench_give_call)(address_any);

static volatile positive allocator_bench_sink;

/* One header word and a payload, aligned as a real class block is. */
static positive allocator_bench_floor_storage[12]
        __attribute__((aligned(ALLOCATOR_ALIGNMENT)));
static address_any allocator_bench_floor_head;

static __attribute__((noinline, noclone)) address_any
allocator_bench_empty_take(positive bytes)
{
        (void)bytes;
        return (address_any)(allocator_bench_floor_storage + 2);
}

static __attribute__((noinline, noclone)) fn
allocator_bench_empty_give(address_any block)
{
        (void)block;
}

static __attribute__((noinline, noclone)) address_any
allocator_bench_floor_take(positive bytes)
{
        (void)bytes;
        address_any block = allocator_bench_floor_head;

        allocator_bench_floor_head = address_to allocator_link(block);
        address_to allocator_tag(block) = 4;
        return block;
}

static __attribute__((noinline, noclone)) fn
allocator_bench_floor_give(address_any block)
{
        address_to allocator_tag(block) = ALLOCATOR_FREED + 4;
        address_to allocator_link(block) = allocator_bench_floor_head;
        allocator_bench_floor_head = block;
}

static allocator_bench_take_call volatile allocator_bench_empty_take_call =
        allocator_bench_empty_take;
static allocator_bench_give_call volatile allocator_bench_empty_give_call =
        allocator_bench_empty_give;
static allocator_bench_take_call volatile allocator_bench_floor_take_call =
        allocator_bench_floor_take;
static allocator_bench_give_call volatile allocator_bench_floor_give_call =
        allocator_bench_floor_give;
static allocator_bench_take_call volatile allocator_bench_subject_take_call =
        malloc;
static allocator_bench_give_call volatile allocator_bench_subject_give_call =
        free;

static fn allocator_bench_control()
{
        for (positive i = 0; i < ALLOCATOR_BENCH_ROUNDS; i++)
        {
                address_any block =
                        allocator_bench_empty_take_call(ALLOCATOR_BENCH_SIZE);

                allocator_bench_sink += (positive)block;
                allocator_bench_empty_give_call(block);
        }
}

static fn allocator_bench_floor()
{
        for (positive i = 0; i < ALLOCATOR_BENCH_ROUNDS; i++)
        {
                address_any block =
                        allocator_bench_floor_take_call(ALLOCATOR_BENCH_SIZE);

                allocator_bench_sink += (positive)block;
                allocator_bench_floor_give_call(block);
        }
}

static fn allocator_bench_subject()
{
        for (positive i = 0; i < ALLOCATOR_BENCH_ROUNDS; i++)
        {
                address_any block =
                        allocator_bench_subject_take_call(ALLOCATOR_BENCH_SIZE);

                allocator_bench_sink += (positive)block;
                allocator_bench_subject_give_call(block);
        }
}

static p64 allocator_bench_best(allocator_bench_work work)
{
        p64 best = 0;

        for (positive which = 0; which < ALLOCATOR_BENCH_TRIES; which++)
        {
                p64 started = get_cpu_time();
                p64 elapsed;

                work();
                elapsed = get_cpu_time() - started;

                if (!best || elapsed < best)
                        best = elapsed;
        }

        return best;
}

static fn allocator_bench_report(string_address name, allocator_bench_work work)
{
        p64 ticks = allocator_bench_best(work);
        positive scaled =
                (positive)(ticks * 100 / ALLOCATOR_BENCH_ROUNDS);
        p8 fraction[3];

        positive_into_padded(fraction, scaled % 100, 2, '0');
        fraction[2] = end;
        string_format(log, "  %s  %p.%s ticks/pair\n", name, scaled / 100,
                      fraction);
}

static allocator_bench_work allocator_bench_named(string_address name)
{
        if (string_compare(name, (string_address)"control") == 0)
                return allocator_bench_control;
        if (string_compare(name, (string_address)"floor") == 0)
                return allocator_bench_floor;
        if (string_compare(name, (string_address)"subject") == 0)
                return allocator_bench_subject;

        return null;
}

b32 main(void)
{
        address_any floor_block =
                (address_any)(allocator_bench_floor_storage + 2);

        allocator_bench_floor_head = floor_block;
        address_to allocator_link(floor_block) = null;
        address_to allocator_tag(floor_block) = ALLOCATOR_FREED + 4;

        /* Warm the real class so the measured subject never reaches mmap. */
        address_any warm = malloc(ALLOCATOR_BENCH_SIZE);
        free(warm);

        if (program_argument_count() > 1)
        {
                allocator_bench_work work =
                        allocator_bench_named(program_argument(1));

                if (is_null(work))
                        return 2;

                work();
                return 0;
        }

        string_format(log, "malloc/free class fast path, best of %p (%p pairs)\n",
                      (positive)ALLOCATOR_BENCH_TRIES,
                      (positive)ALLOCATOR_BENCH_ROUNDS);
        allocator_bench_report((string_address)"empty ABI control",
                               allocator_bench_control);
        allocator_bench_report((string_address)"free-list traffic floor",
                               allocator_bench_floor);
        allocator_bench_report((string_address)"malloc + free",
                               allocator_bench_subject);
        log_flush();

        return 0;
}
