/* sscanf literal matching against its control and semantic traffic floors. */
#include "../src/compiler_memory.c"
#include "bench_measure.c"

#define SCAN_LITERAL_ROUNDS (1u << 20)
#define SCAN_LITERAL_TRIES 5

static const char address_to scan_literal_format =
        "header.alpha-0123456789/body.beta-abcdefghijklmnopqrstuvwxyz/"
        "payload.gamma-ABCDEFGHIJKLMNOPQRSTUVWXYZ/trailer.delta-9876543210";
static p8 scan_literal_input[256];
static volatile positive scan_literal_sink;

static b32 (*volatile scan_literal_call)(string_address, string_address, ...) =
        sscanf;

static fn scan_literal_control()
{
        for (positive i = 0; i < SCAN_LITERAL_ROUNDS; i++)
                scan_literal_sink += scan_literal_call(
                        (string_address)scan_literal_input,
                        (string_address)"");
}

/* The unavoidable semantic work for one ordinary literal run: find the run,
   bound it by the input terminator, and answer its exact common prefix. */
static fn scan_literal_floor()
{
        for (positive i = 0; i < SCAN_LITERAL_ROUNDS; i++)
        {
                positive run = string_span_without_set(
                        (string_address)scan_literal_format, "% \t\n\r\v\f");
                positive available = string_length_max(
                        (string_address)scan_literal_input, run);
                positive matched = memory_common_prefix(
                        (address_any)scan_literal_format,
                        (address_any)scan_literal_input, available);

                scan_literal_sink += run + available + matched;
        }
}

static fn scan_literal_subject()
{
        for (positive i = 0; i < SCAN_LITERAL_ROUNDS; i++)
                scan_literal_sink += scan_literal_call(
                        (string_address)scan_literal_input,
                        (string_address)scan_literal_format);
}

static fn scan_literal_report(string_address name, bench_work work,
                              positive units, string_address unit)
{
        bench_report(name, work, SCAN_LITERAL_TRIES,
                     SCAN_LITERAL_ROUNDS * units, unit);
}

static bench_work scan_literal_named(string_address name)
{
        if (string_compare(name, (string_address)"control") == 0)
                return scan_literal_control;
        if (string_compare(name, (string_address)"floor") == 0)
                return scan_literal_floor;
        if (string_compare(name, (string_address)"subject") == 0)
                return scan_literal_subject;

        return null;
}

b32 main(void)
{
        positive bytes = string_length((string_address)scan_literal_format);

        memory_copy_apart(scan_literal_input,
                          (address_any)scan_literal_format, bytes + 1);

        if (program_argument_count() > 1)
        {
                bench_work work =
                        scan_literal_named(program_argument(1));

                if (is_null(work))
                        return 2;

                work();
                return 0;
        }

        string_format(log, "sscanf literal, best of %p (%p rounds, %p bytes)\n",
                      (positive)SCAN_LITERAL_TRIES,
                      (positive)SCAN_LITERAL_ROUNDS, bytes);
        scan_literal_report((string_address)"empty call control",
                            scan_literal_control, 1,
                            (string_address)"call");
        scan_literal_report((string_address)"semantic traffic floor",
                            scan_literal_floor, bytes,
                            (string_address)"byte");
        scan_literal_report((string_address)"sscanf literal",
                            scan_literal_subject, bytes,
                            (string_address)"byte");
        log_flush();

        return 0;
}
