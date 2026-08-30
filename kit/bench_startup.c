/* Runtime startup components: CPU selection, identity, and stack publication. */
#include "../src/compiler_memory.c"
#include "bench_measure.c"

#define STARTUP_ROUNDS (1u << 16)
#define STARTUP_TRIES 7

typedef fn (*startup_void_call)(void);
typedef positive (*startup_identity_call)(void);
typedef string_address address_to (*startup_environment_call)(void);

static volatile positive startup_sink;
static startup_void_call volatile startup_cpu_call = moonwater_cpu_detect;
static startup_void_call volatile startup_begin_call = stdlib_program_starting;
static startup_identity_call volatile startup_identity =
        stdlib_process_identity;
static startup_identity_call volatile startup_initial_identity =
        program_initial_identity;
static startup_environment_call volatile startup_environment =
        program_environment_list;

/* Required x86 serialization/instruction traffic, without policy branches. */
static __attribute__((noinline, noclone)) fn startup_cpu_floor_call(void)
{
#if X64
        __asm__ volatile(
                "push %%rbx\n\t"
                "mov $1, %%eax\n\t"
                "xor %%ecx, %%ecx\n\t"
                "cpuid\n\t"
                "bt $27, %%ecx\n\t"
                "jnc 1f\n\t"
                "xor %%ecx, %%ecx\n\t"
                "xgetbv\n\t"
                "mov $7, %%eax\n\t"
                "xor %%ecx, %%ecx\n\t"
                "cpuid\n\t"
                "1:\n\t"
                "pop %%rbx"
                :
                :
                : "rax", "rcx", "rdx", "cc", "memory");
#endif
}

static startup_void_call volatile startup_cpu_floor = startup_cpu_floor_call;

static fn startup_cpu_floor_work()
{
        for (positive i = 0; i < STARTUP_ROUNDS; i++)
                startup_cpu_floor();
}

static fn startup_cpu_work()
{
        for (positive i = 0; i < STARTUP_ROUNDS; i++)
        {
                startup_cpu_call();
                startup_sink += cpu_has_avx2 + cpu_has_avx512;
        }
}

static fn startup_identity_work()
{
        for (positive i = 0; i < STARTUP_ROUNDS; i++)
                startup_sink += startup_identity();
}

static fn startup_initial_identity_work()
{
        for (positive i = 0; i < STARTUP_ROUNDS; i++)
                startup_sink += startup_initial_identity();
}

/* Exact environment half of stdlib_program_starting, without the PID trap. */
static fn startup_environment_work()
{
        for (positive i = 0; i < STARTUP_ROUNDS; i++)
        {
                environ = startup_environment();
                startup_sink += (positive)environ;
        }
}

static fn startup_begin_work()
{
        for (positive i = 0; i < STARTUP_ROUNDS; i++)
        {
                startup_begin_call();
                startup_sink += (positive)environ;
        }
}

static fn startup_report(string_address name, bench_work work)
{
        bench_report(name, work, STARTUP_TRIES,
                     STARTUP_ROUNDS, (string_address)"call");
}

static bench_work startup_named(string_address name)
{
        if (string_compare(name, (string_address)"cpu") == 0)
                return startup_cpu_work;
        if (string_compare(name, (string_address)"cpu-floor") == 0)
                return startup_cpu_floor_work;
        if (string_compare(name, (string_address)"identity") == 0)
                return startup_identity_work;
        if (string_compare(name, (string_address)"initial-identity") == 0)
                return startup_initial_identity_work;
        if (string_compare(name, (string_address)"environment") == 0)
                return startup_environment_work;
        if (string_compare(name, (string_address)"begin") == 0)
                return startup_begin_work;
        return null;
}

b32 main(void)
{
        if (program_argument_count() > 1)
        {
                bench_work work = startup_named(program_argument(1));

                if (is_null(work))
                        return 2;

                work();
                return 0;
        }

        string_format(log, "runtime startup components, best of %p (%p calls)\n",
                      (positive)STARTUP_TRIES, (positive)STARTUP_ROUNDS);
        startup_report((string_address)"CPU instruction/serialization floor",
                       startup_cpu_floor_work);
        startup_report((string_address)"CPU feature selection",
                       startup_cpu_work);
        startup_report((string_address)"process identity syscall",
                       startup_identity_work);
        startup_report((string_address)"loader identity handoff",
                       startup_initial_identity_work);
        startup_report((string_address)"environment publication floor",
                       startup_environment_work);
        startup_report((string_address)"stdlib startup", startup_begin_work);
        log_flush();
        return 0;
}
