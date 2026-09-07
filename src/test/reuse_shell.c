#include "../compiler_memory.c"
#include "../spark.c"
#include "../sh/shell.c"
#include "counted.inc"

static p8 reuse_output[16384];
static positive reuse_used;

static fn reuse_capture(address_any data, positive length)
{
        if (!length) length = string_length(data);
        if (length <= sizeof(reuse_output) - reuse_used)
                memory_copy(reuse_output + reuse_used, data, length);
        reuse_used += length;
}

static p8 reuse_hex(p8 value)
{
        return value < 10 ? '0' + value : 'a' + value - 10;
}

static fn reuse_masks(void)
{
        positive set[UL_CPU_WORDS] = {0};
        p8 wanted[256], bounded[258];
        positive seed = 0x534841524544;
        for (positive shape = 0; shape < 64; shape++)
        {
                for (positive i = 0; i < 8; i++)
                {
                        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                        set[i] = shape == 0 ? 0 : shape == 1 ? positive_max : seed;
                }
                for (positive bytes = 0; bytes <= 64; bytes++)
                        for (positive grouped = 0; grouped < 2; grouped++)
                        {
                                positive nibbles = bytes * 2, used = 0;
                                while (nibbles > 1 &&
                                       !((set[(nibbles - 1) / 16] >>
                                          (((nibbles - 1) % 16) * 4)) & 15))
                                        nibbles--;
                                for (positive left = nibbles; left; left--)
                                {
                                        if (grouped && left != nibbles && !(left % 8))
                                                wanted[used++] = ',';
                                        positive at = left - 1;
                                        wanted[used++] = reuse_hex(
                                            set[at / 16] >> ((at % 16) * 4) & 15);
                                }
                                reuse_used = 0;
                                ul_cpu_mask_write(reuse_capture, set, bytes, grouped);
                                check("mask formatter byte widths and grouping",
                                      reuse_used == used &&
                                      !memory_compare(reuse_output, wanted, used));
                        }
                reuse_used = 0;
                ul_cpu_mask_write(reuse_capture, set, sizeof(set), true);
                for (positive room = 0; room < sizeof(bounded) - 2; room++)
                {
                        memory_fill(bounded, 0xa5, sizeof(bounded));
                        ul_lscpu_set_text(bounded + 1, room, set, true);
                        positive take = room ? min(room - 1, reuse_used) : 0;
                        check("lscpu bounded shared mask sink",
                              bounded[0] == 0xa5 && bounded[room + 1] == 0xa5 &&
                              !memory_compare(bounded + 1, reuse_output, take) &&
                              (!room || !bounded[take + 1]));
                }
        }
        memory_zero(set, sizeof(set));
        ul_bit_range_set(set, 1, 2, 1);
        ul_bit_range_set(set, 5, 13, 4);
        reuse_used = 0;
        ul_cpu_list_write(reuse_capture, set, sizeof(set), true);
        check("stride list keeps singleton pairs",
              reuse_used == 10 && !memory_compare(reuse_output, "1,2,5-13:4", 10));
        ul_lscpu_set_text(bounded, sizeof(bounded), set, false);
        check("lscpu keeps contiguous-pair policy", string_equals(bounded, "1-2,5,9,13"));
}

static fn reuse_uuid(void)
{
        p8 input[16], made[39], wanted[37];
        for (positive seed = 0; seed < 256; seed++)
        {
                positive used = 0;
                for (positive i = 0; i < 16; i++)
                {
                        input[i] = (p8)(seed + i * 31);
                        if (i == 4 || i == 6 || i == 8 || i == 10)
                                wanted[used++] = '-';
                        wanted[used++] = reuse_hex(input[i] >> 4);
                        wanted[used++] = reuse_hex(input[i] & 15);
                }
                wanted[used] = 0;
                memory_fill(made, 0xa5, sizeof(made));
                storage_uuid_bytes(made + 1, input);
                check("shared UUID hex groups and exact bounds",
                      made[0] == 0xa5 && made[38] == 0xa5 &&
                      !memory_compare(made + 1, wanted, sizeof(wanted)));
        }
}

static fn reuse_lists(void)
{
        static const positive offsets[] = {0, 25, 57, 121};
        positive set[UL_CPU_WORDS], parsed[UL_CPU_WORDS];
        p8 wanted[256], actual[256];
        for (positive pattern = 0; pattern < 4096; pattern++)
                for (positive shift = 0; shift < array_count(offsets); shift++)
                {
                        positive base = offsets[shift], used = 0;
                        memory_zero(set, sizeof(set));
                        for (positive bit = 0; bit < 12; bit++)
                                if (pattern & ((positive)1 << bit))
                                        set[(base + bit) / 64] |= (positive)1 << ((base + bit) % 64);
                        for (positive bit = 0; bit < 12; bit++)
                        {
                                if (!(pattern & ((positive)1 << bit))) continue;
                                positive last = bit;
                                while (last + 1 < 12 && (pattern & ((positive)1 << (last + 1)))) last++;
                                if (used) wanted[used++] = ',';
                                used += positive_into(wanted + used, base + bit);
                                if (last != bit)
                                {
                                        wanted[used++] = '-';
                                        used += positive_into(wanted + used, base + last);
                                }
                                bit = last;
                        }
                        wanted[used] = 0;
                        ul_lscpu_set_text(actual, sizeof(actual), set, false);
                        check("contiguous list all 12-bit masks across word boundaries",
                              string_equals(actual, wanted));
                        reuse_used = 0;
                        ul_cpu_list_write(reuse_capture, set, sizeof(set), true);
                        reuse_output[reuse_used] = 0;
                        memory_zero(parsed, sizeof(parsed));
                        bool okay = !reuse_used || ul_bit_list_read(reuse_output, parsed, UL_CPU_BITS, false);
                        check("stride list round trip all 12-bit masks across word boundaries",
                              okay && !memory_compare(parsed, set, sizeof(set)));
                }
}

static fn reuse_json(void)
{
        static const positive lengths[] = {0, 1, 7, 8, 15, 16, 17, 31, 32, 33,
                                            127, 255, 256, 257, 511, 513,
                                            1023, 1024, 4097, 8193};
        static p8 input[8193], wanted[65536];
        for (positive seed = 0; seed < 256; seed++)
                for (positive n = 0; n < array_count(lengths); n++)
                        for (positive lower = 0; lower < 2; lower++)
                        {
                                positive length = lengths[n], used = 0;
                                wanted[used++] = '"';
                                for (positive i = 0; i < length; i++)
                                {
                                        p8 byte = input[i] = (p8)(seed + i * 13);
                                        if (lower && byte >= 'A' && byte <= 'Z') byte += 32;
                                        p8 short_name = byte == '\b' ? 'b' : byte == '\f' ? 'f' :
                                            byte == '\n' ? 'n' : byte == '\r' ? 'r' : byte == '\t' ? 't' : 0;
                                        if (short_name || byte == '"' || byte == '\\')
                                        {
                                                wanted[used++] = '\\';
                                                wanted[used++] = short_name ? short_name : byte;
                                        }
                                        else if (byte < 32)
                                        {
                                                wanted[used++] = '\\'; wanted[used++] = 'u';
                                                wanted[used++] = '0'; wanted[used++] = '0';
                                                wanted[used++] = reuse_hex(byte >> 4);
                                                wanted[used++] = reuse_hex(byte & 15);
                                        }
                                        else wanted[used++] = byte;
                                }
                                wanted[used++] = '"';
                                text_out_used = 0;
                                column_json_string((column_cell){input, length}, lower);
                                check("column JSON exact short controls and lowercase chunks",
                                      text_out_used == used &&
                                      !memory_compare(text_out_buffer, wanted, used));
                        }
        static p8 long_name[32768];
        memory_fill(long_name, 'Q', sizeof(long_name));
        text_out_used = 0;
        column_json_string((column_cell){long_name, sizeof(long_name)}, true);
        check("long literal JSON key bounded lowercase chunks",
              text_out_used == sizeof(long_name) + 2 && text_out_buffer[0] == '"' &&
              text_out_buffer[text_out_used - 1] == '"' &&
              memory_count(text_out_buffer + 1, sizeof(long_name), 'q') == sizeof(long_name));
        text_out_used = 0;
}

static fn reuse_braces(void)
{
        static const bipolar values[] = {0, 1, -1, 9, -9, 10, -10, 99, -99,
                                         bipolar_min, bipolar_max};
        for (positive i = 0; i < array_count(values); i++)
                for (positive width = 0; width <= 40; width++)
                        for (positive padded = 0; padded < 2; padded++)
                        {
                                p8 plain[24], wanted[64], actual[66];
                                positive length = bipolar_into(plain, values[i]);
                                positive sign = values[i] < 0, used = 0;
                                if (sign) wanted[used++] = '-';
                                if (padded)
                                        for (positive n = length; n < width; n++) wanted[used++] = '0';
                                for (positive n = sign; n < length; n++) wanted[used++] = plain[n];
                                memory_fill(actual, 0xa5, sizeof(actual));
                                positive made = expand_brace_number_text(actual + 1, values[i], width, padded);
                                check("brace signed minimum widths use shared decimal floor",
                                      made == used && !memory_compare(actual + 1, wanted, used) &&
                                      actual[0] == 0xa5 && actual[used + 1] == 0xa5);
                        }
}

static fn reuse_percent_b(void)
{
        static const positive lengths[] = {0, 1, 7, 8, 15, 16, 255, 256, 4096, 8193};
        static const struct { string_address source, wanted; } endings[] = {
            {"", ""}, {"\\n", "\n"}, {"\\012", "\n"}, {"\\q", "\\q"},
            {"\\\\", "\\"}, {"\\", "\\"}, {"\\cignored", ""},
        };
        static p8 source[8224];
        for (positive n = 0; n < array_count(lengths); n++)
                for (positive e = 0; e < array_count(endings); e++)
                {
                        positive length = lengths[n];
                        memory_fill(source, 'Q', length);
                        string_copy(source + length, endings[e].source);
                        printf_in_b = true;
                        printf_cut = false;
                        reuse_used = 0;
                        printf_escaped(reuse_capture, source);
                        positive tail = string_length(endings[e].wanted);
                        check("printf percent-b shared slash scanner",
                              reuse_used == length + tail &&
                              memory_count(reuse_output, length, 'Q') == length &&
                              !memory_compare(reuse_output + length, endings[e].wanted, tail));
                }
        printf_cut = printf_in_b = false;
}

#ifdef REUSE_BENCHMARK
static fn reuse_json_scalar(column_cell value, bool lower)
{
        text_put_character('"');
        for (positive i = 0; i < value.length; i++)
        {
                p8 byte = value.bytes[i];
                if (lower && byte >= 'A' && byte <= 'Z') byte += 32;
                if (byte == '"' || byte == '\\')
                {
                        text_put_character('\\');
                        text_put_character(byte);
                }
                else if (byte == '\b') text_put_string("\\b");
                else if (byte == '\f') text_put_string("\\f");
                else if (byte == '\n') text_put_string("\\n");
                else if (byte == '\r') text_put_string("\\r");
                else if (byte == '\t') text_put_string("\\t");
                else if (byte < 32)
                {
                        p8 escaped[] = {'\\', 'u', '0', '0', reuse_hex(byte >> 4), reuse_hex(byte & 15)};
                        text_put(escaped, sizeof(escaped));
                }
                else text_put_character(byte);
        }
        text_put_character('"');
}

static fn reuse_benchmark(void)
{
        static const positive lengths[] = {16, 256, 4096, 32768};
        static p8 bytes[32768];
        for (positive shape = 0; shape < 2; shape++)
        {
                for (positive i = 0; i < sizeof(bytes); i++)
                        bytes[i] = shape && i % 32 == 31 ? '\n' : 'Q';
                for (positive n = 0; n < array_count(lengths); n++)
                        for (positive lower = 0; lower < 2; lower++)
                        {
                                positive elapsed[2];
                                for (positive engine = 0; engine < 2; engine++)
                                {
                                        positive start = get_cpu_time();
                                        for (positive i = 0; i < 8388608 / lengths[n]; i++)
                                        {
                                                text_out_used = 0;
                                                if (engine) column_json_string((column_cell){bytes, lengths[n]}, lower);
                                                else reuse_json_scalar((column_cell){bytes, lengths[n]}, lower);
                                        }
                                        elapsed[engine] = get_cpu_time() - start;
                                }
                                string_format(log, "json shape=%p bytes=%p lower=%p scalar=%p shared=%p percent=%p\n",
                                    shape, lengths[n], lower, elapsed[0], elapsed[1], elapsed[1] * 100 / elapsed[0]);
                        }
        }
        text_out_used = 0;
}
#endif

b32 main(void)
{
        bipolar number;
        positive mode;
        check("pr accepts signed line-number boundaries",
              pr_signed(" +2147483647", &number) && number == b32_max &&
              pr_signed("-2147483648", &number) && number == b32_min);
        check("pr rejects overflowing line numbers",
              !pr_signed("2147483648", &number) &&
              !pr_signed("-2147483649", &number) &&
              !pr_signed("18446744073709551616", &number));
        check("nice saturates the complete overflowing digit run",
              nice_adjustment("18446744073709551616", &number) && number == 39 &&
              nice_adjustment("-18446744073709551616", &number) && number == -39 &&
              !nice_adjustment("18446744073709551616x", &number));
        check("chmod rejects octal overflow",
              !file_mode_adjust("2000000000000000000000", 0, false, 0, false, &mode));
        check("chmod keeps explicit leading-zero policy",
              file_mode_adjust("0000755", 06000, true, 0, false, &mode) && mode == 0755);
        shell_store store = {0};
        check("arena starts with a small block", shell_store_take(&store, 1) != null);
        shell_block address_to head = store.head;
        store.here = null;
        check("arena grows before its head without losing the chain",
              head && shell_store_take(&store, head->size + 1) &&
              store.head != head && store.head->next == head);
        while (store.head)
        {
                shell_block address_to next = store.head->next;
                memory_free(store.head, sizeof(shell_block) + store.head->size);
                store.head = next;
        }
        store.here = null;
        shell_memory_failed = false;
        check("arena span rejects length overflow",
              !shell_store_copy(&store, null, positive_max) && shell_memory_failed);
        shell_memory_failed = false;
        reuse_masks();
        reuse_lists();
        reuse_uuid();
        reuse_json();
        reuse_braces();
        reuse_percent_b();
        check("uniq field skip terminates at EOL", uniq_skipped(" a b", 4, positive_max, 0) == 4);
        check("uniq character skip saturates", uniq_skipped(" a b", 4, 1, positive_max) == 4);
        check("uniq empty field skip is bounded", !uniq_skipped("", 0, positive_max, positive_max));
#ifdef REUSE_BENCHMARK
        if (!failures) reuse_benchmark();
#endif
        return test_report(null);
}
