/* Shared escaping versus the former writer loops. Native timings only measure
   hardware; QEMU remains useful for checking equal bytes. */
#include "../src/compiler_memory.c"
#include "bench_measure.c"

#define TRIES 9
#define MAXIMUM 65536
static p8 input[MAXIMUM + 1], output[MAXIMUM * 6 + 2], expected[MAXIMUM * 6 + 2];
static positive used;
#ifdef ESCAPE_BENCH_POLICY
static const p8 hex_policy = ESCAPE_BENCH_POLICY;
#else
static p8 hex_policy = 63;
#endif
static volatile positive sink;

__attribute__((noinline, noclone)) static fn capture(address_any data, positive length)
{
        memory_copy(output + used, data, length);
        used += length;
}

static fn former_hex(positive size, p8 policy)
{
        static const p8 categories[256] = {
            [0 ... 8] = HEX_CONTROL, [9] = HEX_TAB,
            [10 ... 31] = HEX_CONTROL, [' '] = HEX_SPACE,
            ['"'] = HEX_QUOTE, ['\\'] = HEX_SLASH, [127] = HEX_CONTROL,
            [128 ... 255] = HEX_HIGH,
        };
        positive start = 0;
        for (positive at = 0; at < size; at++)
        {
                if (!(categories[input[at]] & policy))
                        continue;
                if (at > start)
                        capture(input + start, at - start);
                p8 escaped[4] = {'\\', 'x'};
                memory_into_hex(escaped + 2, input + at, 1);
                capture(escaped, sizeof(escaped));
                start = at + 1;
        }
        if (size > start)
                capture(input + start, size - start);
}

static fn former_json(void)
{
        p8 address_to at = input;
        p8 address_to start = input;
        capture("\"", 1);
        while (*at)
        {
                p8 byte = *at;
                if (byte >= 32 && byte != '"' && byte != '\\')
                {
                        at++;
                        continue;
                }
                if (at > start)
                        capture(start, at - start);
                if (byte == '"' || byte == '\\')
                {
                        p8 escaped[2] = {'\\', byte};
                        capture(escaped, sizeof(escaped));
                }
                else
                {
                        p8 escaped[6] = {'\\', 'u', '0', '0'};
                        memory_into_hex(escaped + 4, &byte, 1);
                        capture(escaped, sizeof(escaped));
                }
                at++;
                start = at;
        }
        if (at > start)
                capture(start, at - start);
        capture("\"", 1);
}

static p64 run(bool assembly, positive size, positive rounds, bool json)
{
        p64 start = get_cpu_time();
        for (positive at = 0; at < rounds; at++)
        {
                used = 0;
                if (assembly)
                {
                        if (json) writer_json_string(capture, input);
                        else writer_hex_escaped(capture, input, size, hex_policy);
                }
                else if (json) former_json();
                else former_hex(size, hex_policy);
                sink += used;
        }
        return get_cpu_time() - start;
}

b32 main(void)
{
#ifndef ESCAPE_BENCH_POLICY
        if (program_argument_count() > 1)
        {
                positive policy = string_to_positive(program_argument(1));
                if (policy > 63) return 2;
                hex_policy = policy;
        }
#endif
        static const positive sizes[] = {1, 2, 3, 4, 6, 7, 8, 16, 20, 21,
                                         32, 128, 4096, MAXIMUM};
        static string_address shapes[] = {"plain", "sparse", "dense", "mixed", "high"};
        for (positive json = 0; json < 2; json++)
                for (positive shape = 0; shape < array_count(shapes); shape++)
                        for (positive row = 0; row < array_count(sizes); row++)
                        {
                                positive size = sizes[row], ratios[TRIES];
                                for (positive at = 0; at < size; at++)
                                {
                                        input[at] = 'a' + at % 26;
                                        if (shape == 1 && at % 127 == 7) input[at] = '\n';
                                        if (shape == 2) input[at] = 1 + at % 31;
                                        if (shape == 3 && at % 3) input[at] = at % 2 ? '\\' : 7;
                                        if (shape == 4) input[at] = 128 + at % 128;
                                }
                                input[size] = 0;
                                run(false, size, 1, json);
                                positive expected_size = used;
                                memory_copy(expected, output, used);
                                run(true, size, 1, json);
                                if (expected_size != used || memory_compare(expected, output, used))
                                        return 1;
                                positive rounds = (1u << 22) / size;
                                if (rounds > 100000) rounds = 100000;
                                for (positive trial = 0; trial < TRIES; trial++)
                                {
                                        p64 former, assembly;
                                        if (trial & 1)
                                        {
                                                assembly = run(true, size, rounds, json);
                                                former = run(false, size, rounds, json);
                                        }
                                        else
                                        {
                                                former = run(false, size, rounds, json);
                                                assembly = run(true, size, rounds, json);
                                        }
                                        ratios[trial] = assembly * 10000 / (former ? former : 1);
                                }
                                order(ratios, TRIES);
                                string_format(log, "%s policy%p %s %p bytes: paired median ASM/C %p.",
                                    json ? "JSON" : "hex", json ? 64 : hex_policy, shapes[shape], size,
                                    ratios[TRIES / 2] / 100);
                                positive_to_padded(log, ratios[TRIES / 2] % 100, 2, '0', 0);
                                log("%\n", 2);
                                log_flush();
                        }
        return 0;
}
