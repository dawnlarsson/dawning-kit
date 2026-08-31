/* Former path C bodies against the shared three-architecture ASM paths. */
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline, noclone))
#define TRIES 5
#define PATH_CAPACITY 4096

enum { BENCH_JOIN, BENCH_TAIL, BENCH_HEAD };

static p8 output[PATH_CAPACITY];
static p8 long_directory[2048];
static p8 long_name[1024];
static p8 long_path[3072];
static volatile positive sink;

NOT_INLINED positive former_path_join(p8 address_to into, positive limit,
                                      string_address directory,
                                      string_address name)
{
        positive length = string_length_max(directory, limit - 1);

        memory_copy_apart(into, directory, length);

        if (length > 0 && into[length - 1] != '/' && length + 1 < limit)
                into[length++] = '/';

        positive tail = string_length_max(name, limit - 1 - length);

        return (positive)(memory_copy_apart_end(into + length, name, tail) - into);
}

NOT_INLINED positive former_path_tail(p8 address_to into, string_address path)
{
        positive length = string_length(path);

        if (!length)
        {
                into[0] = end;
                return 0;
        }

        while (length > 1 && path[length - 1] == '/')
                length--;

        if (length == 1 && path[0] == '/')
        {
                into[0] = '/';
                into[1] = end;
                return 1;
        }

        positive start = length;

        while (start > 0 && path[start - 1] != '/')
                start--;

        positive found = length - start;

        if (found > PATH_CAPACITY - 1)
                found = PATH_CAPACITY - 1;

        memory_copy_apart_end(into, path + start, found);
        return found;
}

NOT_INLINED positive former_path_head(p8 address_to into, string_address path)
{
        positive length = string_length(path);

        while (length > 1 && path[length - 1] == '/')
                length--;

        positive cut = length;

        while (cut > 0 && path[cut - 1] != '/')
                cut--;

        if (!cut)
        {
                into[0] = '.';
                into[1] = end;
                return 1;
        }

        while (cut > 1 && path[cut - 1] == '/')
                cut--;

        memory_copy_apart_end(into, path, cut);
        return cut;
}

static fn paths_make_long()
{
        for (positive i = 0; i < sizeof(long_directory) - 1; i++)
                long_directory[i] = i && !(i % 31) ? '/' : (p8)('a' + i % 23);
        long_directory[sizeof(long_directory) - 1] = end;

        for (positive i = 0; i < sizeof(long_name) - 1; i++)
                long_name[i] = i && !(i % 47) ? '/' : (p8)('A' + i % 19);
        long_name[sizeof(long_name) - 1] = end;

        positive at = 0;
        for (positive i = 0; i < sizeof(long_directory) - 1; i++)
                long_path[at++] = long_directory[i];
        long_path[at++] = '/';
        for (positive i = 0; i < sizeof(long_name) - 1; i++)
                long_path[at++] = long_name[i];
        long_path[at] = end;
}

static p64 path_run(bool assembly, positive operation,
                    string_address directory, string_address name,
                    string_address path, positive rounds)
{
        p64 start = get_cpu_time();

        for (positive i = 0; i < rounds; i++)
        {
                positive length;

                if (operation == BENCH_JOIN)
                        length = assembly
                                     ? path_join(output, PATH_CAPACITY,
                                                 directory, name)
                                     : former_path_join(output, PATH_CAPACITY,
                                                        directory, name);
                else if (operation == BENCH_TAIL)
                        length = assembly
                                     ? path_tail_copy(output, PATH_CAPACITY, path)
                                     : former_path_tail(output, path);
                else
                        length = assembly
                                     ? path_head_copy(output, PATH_CAPACITY, path)
                                     : former_path_head(output, path);

                sink += length + output[0] + output[length ? length - 1 : 0];
        }

        return get_cpu_time() - start;
}

static fn path_row(string_address label, positive operation,
                   string_address directory, string_address name,
                   string_address path, positive rounds)
{
        p64 former = positive_max;
        p64 assembly = positive_max;

        for (positive t = 0; t < TRIES; t++)
        {
                p64 one;
                p64 two;

                if (t & 1)
                {
                        two = path_run(true, operation, directory, name, path, rounds);
                        one = path_run(false, operation, directory, name, path, rounds);
                }
                else
                {
                        one = path_run(false, operation, directory, name, path, rounds);
                        two = path_run(true, operation, directory, name, path, rounds);
                }

                if (one < former)
                        former = one;
                if (two < assembly)
                        assembly = two;
        }

        string_format(log, "  %s: former-C %p  assembly %p  asm/C %p%%\n",
                      label, (positive)former, (positive)assembly,
                      (positive)(assembly * 100 / (former ? former : 1)));
}

b32 main()
{
        paths_make_long();
        moonwater_cpu_detect();

        string_format(log, "paths, best of %p\n", (positive)TRIES);

        path_row("join short", BENCH_JOIN, "usr", "bin", "usr/bin", 1u << 18);
        path_row("join nested", BENCH_JOIN, "/usr/local/share", "moonwater/bin",
                 "/usr/local/share/moonwater/bin", 1u << 16);
        path_row("join long", BENCH_JOIN, long_directory, long_name, long_path,
                 1u << 13);

        path_row("tail short", BENCH_TAIL, "usr", "bin", "usr/bin", 1u << 18);
        path_row("tail nested", BENCH_TAIL, "/usr/local/share", "moonwater/bin",
                 "/usr/local/share/moonwater/bin///", 1u << 16);
        path_row("tail long", BENCH_TAIL, long_directory, long_name, long_path,
                 1u << 13);

        path_row("head short", BENCH_HEAD, "usr", "bin", "usr/bin", 1u << 18);
        path_row("head nested", BENCH_HEAD, "/usr/local/share", "moonwater/bin",
                 "/usr/local/share/moonwater/bin///", 1u << 16);
        path_row("head long", BENCH_HEAD, long_directory, long_name, long_path,
                 1u << 13);

        log_flush();
        return 0;
}
