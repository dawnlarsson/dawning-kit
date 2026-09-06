#include "../compiler_memory.c"

static p8 table_output[16384];
static positive table_used, table_calls;
static bool table_overflow;

static fn table_capture(address_any bytes, positive length)
{
        table_calls++;
        if (!length)
                length = string_length(bytes);
        if (length > sizeof(table_output) - table_used)
        {
                table_overflow = true;
                return;
        }
        memory_copy_apart(table_output + table_used, bytes, length);
        table_used += length;
}

#define log table_capture
#include "../spark.c"
#include "../sh/shell.c"
#undef log
#include "counted.inc"

static fn table_reset(void)
{
        table_used = table_calls = 0;
        table_overflow = false;
}

static string_address table_field(address_any row, p8 column,
                                   p8 address_to scratch)
{
        (void)column; (void)scratch;
        return *(string_address address_to)row;
}

static const ul_table_column table_columns[] = {
    {"value", "VALUE", 0, false, UL_TABLE_STRING, true}
};
static p8 table_selected[] = {0};

static fn table_render(string_address value, positive mode)
{
        table_reset();
        if (mode == 2)
                ul_table_json("test", address_of value, sizeof(value), 1,
                              table_columns, table_selected, 1, table_field);
        else
                ul_table_out(address_of value, sizeof(value), 1, table_columns,
                             1, table_selected, 1, true, mode == 1, table_field);
}

static p8 table_hex_digit(p8 nibble)
{
        return nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
}

static fn table_checks(void)
{
        p8 bytes[1030], expected[8192];
        for (positive size = 0; size <= 1024; size++)
        {
                positive width = 0, longest = 0, lines = 1, single_width = 0;
                for (positive at = 0; at < size; at++)
                {
                        p8 byte = (p8)(1 + ((at * 197 + size) % 255));
                        bytes[at] = byte;
                        single_width += byte < ' ' || byte == 127 ? 4 : 1;
                        if (byte == '\n')
                        {
                                longest = max(longest, width);
                                width = 0;
                                lines++;
                        }
                        else
                                width += byte < ' ' || byte == 127 ? 4 : 1;
                }
                bytes[size] = 0;
                longest = max(longest, width);
                check("table multiline width", ul_table_safe_width(bytes, true) == longest);
                check("table single-line width", ul_table_safe_width(bytes, false) == single_width);
#ifdef TABLE_LEGACY_LINE_COUNT
                // For the paired harness built against the pre-fold snapshot.
                check("table newline count", ul_table_line_count(bytes, true) == lines);
#else
                check("table newline count", ul_table_line_count(bytes) == lines);
#endif
                positive start = 0;
                for (positive line = 0; line <= lines; line++)
                {
                        positive stop = start, got_length;
                        while (stop < size && bytes[stop] != '\n')
                                stop++;
                        string_address got = ul_table_line(bytes, line, address_of got_length);
                        positive wanted = line < lines ? stop - start : 0;
                        check("table indexed line", got_length == wanted &&
                              !memory_compare(got, bytes + start, wanted));
                        start = stop < size ? stop + 1 : size;
                }
        }

        // Bounded safe fields include embedded NUL; raw/JSON fields end at it.
        positive used = 0;
        for (positive byte = 0; byte < 256; byte++)
        {
                bytes[byte] = (p8)byte;
                if (byte < ' ' || byte == 127)
                {
                        expected[used++] = '\\'; expected[used++] = 'x';
                        expected[used++] = table_hex_digit(byte >> 4);
                        expected[used++] = table_hex_digit(byte & 15);
                }
                else
                        expected[used++] = byte;
        }
        table_reset();
        ul_lsns_safe_span(bytes, 256);
        check("safe byte policy", !table_overflow && table_used == used &&
              !memory_compare(table_output, expected, used));
        check("safe width agrees with output", ul_lsns_safe_span_length(bytes, 256) == used);

        for (positive byte = 1; byte < 256; byte++)
                bytes[byte - 1] = byte;
        bytes[255] = 0;
        for (positive mode = 1; mode <= 2; mode++)
        {
                string_address prefix = mode == 1 ? "VALUE\n" :
                    "{\n   \"test\": [\n      {\n         \"value\": \"";
                used = string_length(prefix);
                memory_copy(expected, prefix, used);
                for (positive byte = 1; byte < 256; byte++)
                {
                        if (mode == 1 && (byte <= ' ' || byte >= 127 || byte == '\\'))
                        {
                                expected[used++] = '\\'; expected[used++] = 'x';
                        }
                        else if (mode == 2 && byte < ' ')
                        {
                                expected[used++] = '\\'; expected[used++] = 'u';
                                expected[used++] = '0'; expected[used++] = '0';
                        }
                        else
                        {
                                if (mode == 2 && (byte == '"' || byte == '\\'))
                                        expected[used++] = '\\';
                                expected[used++] = byte;
                                continue;
                        }
                        expected[used++] = table_hex_digit(byte >> 4);
                        expected[used++] = table_hex_digit(byte & 15);
                }
                string_address suffix = mode == 1 ? "\n" : "\"\n      }\n   ]\n}\n";
                positive tail = string_length(suffix);
                memory_copy(expected + used, suffix, tail);
                used += tail;
                table_render(bytes, mode);
                if (table_used != used || memory_compare(table_output, expected, used))
                        string_format(log, "table mode=%p got=%p wanted=%p prefix=%p\n",
                                      mode, table_used, used,
                                      memory_common_prefix(table_output, expected,
                                                           min(table_used, used)));
                check("raw/JSON exact byte policy", !table_overflow && table_used == used &&
                      !memory_compare(table_output, expected, used));
        }

        table_render("ab\tcd\nx\n", 0);
        check("normal multiline layout", table_used == 18 &&
              !memory_compare(table_output, "VALUE\nab\\x09cd\nx\n\n", 18));

        table_render("", 1);
        check("empty raw field", table_used == 7 &&
              !memory_compare(table_output, "VALUE\n\n", 7));
        string_address empty_json =
            "{\n   \"test\": [\n      {\n         \"value\": \"\"\n      }\n   ]\n}\n";
        table_render("", 2);
        check("empty JSON field", table_used == string_length(empty_json) &&
              !memory_compare(table_output, empty_json, table_used));
}

b32 main(void)
{
        table_checks();
#ifdef TABLE_BENCHMARK
        if (!failures)
        {
                static p8 text[2049];
                for (positive shape = 0; shape < 3; shape++)
                {
                        for (positive at = 0; at < sizeof(text) - 1; at++)
                                text[at] = shape == 1 && at % 64 == 63 ? '\n' :
                                           shape == 2 && at % 64 == 63 ? '\t' : 'a';
                        for (positive mode = 0; mode < 3; mode++)
                        {
                                p64 start = get_cpu_time();
                                for (positive round = 0; round < 4096; round++)
                                        table_render(text, mode);
                                p64 elapsed = get_cpu_time() - start;
                                string_format(log, "table shape=%p mode=%p ticks=%p writes=%p\n",
                                              shape, mode, (positive)elapsed, table_calls);
                        }
                }
        }
#endif
        return test_report(null);
}
