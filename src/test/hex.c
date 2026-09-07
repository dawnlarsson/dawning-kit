#include "../compiler_memory.c"
#include "counted.inc"

static p8 escape_output[2048];
static positive escape_used, escape_calls;
static bool escape_overflow;

static fn escape_capture(address_any data, positive length)
{
        escape_calls++;
        if (length > sizeof(escape_output) - escape_used)
                escape_overflow = true;
        else
        {
                memory_copy(escape_output + escape_used, data, length);
                escape_used += length;
        }
}

static positive escape_oracle(p8 address_to into, p8 byte, positive policy)
{
        bool selected = policy == 64 ? byte < 32 || byte == '"' || byte == '\\' :
            (byte < 32 && byte != 9 && (policy & 1)) ||
            (byte == 127 && (policy & 1)) || (byte == 9 && (policy & 2)) ||
            (byte == 32 && (policy & 4)) || (byte == 34 && (policy & 8)) ||
            (byte == 92 && (policy & 16)) || (byte >= 128 && (policy & 32));
        if (!selected)
        {
                into[0] = byte;
                return 1;
        }
        into[0] = '\\';
        if (policy == 64 && byte >= 32)
        {
                into[1] = byte;
                return 2;
        }
        positive size = policy == 64 ? 6 : 4;
        into[1] = policy == 64 ? 'u' : 'x';
        into[2] = into[3] = '0';
        for (positive at = 0; at < 2; at++)
        {
                p8 nibble = (byte >> (at ? 0 : 4)) & 15;
                into[size - 2 + at] = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
        }
        return size;
}

static fn escape_full_check(p8 address_to input, positive length, positive policy)
{
        p8 output[208], expected[208];
        memory_fill(output, 0xa5, sizeof(output));
        memory_fill(expected, 0xa5, sizeof(expected));
        positive written = 0;
        for (positive at = 0; at < length; at++)
                written += escape_oracle(expected + 8 + written, input[at], policy);
        positive2 result = memory_into_escaped(output + 8, input, length, length * 6, policy);
        check("escape complete short vector with exact scratch canaries",
              result.x == length && result.y == written &&
              !memory_compare(output, expected, sizeof(output)));
}

static fn escape_span_check(p8 address_to input, positive length)
{
        for (positive policy = 0; policy <= 64; policy++)
        {
                positive first = length;
                p8 encoded[6];
                for (positive at = 0; at < length; at++)
                        if (escape_oracle(encoded, input[at], policy) != 1)
                        {
                                first = at;
                                break;
                        }
                check("escape bounded first index",
                      memory_escape_index(input, length, policy) == first);
                for (positive mode = 0; mode < 2; mode++)
                for (positive capacity = 0; capacity <= 80; capacity++)
                {
                        p8 output[96], expected[96];
                        positive read = 0, written = 0;
                        memory_fill(output, 0xa5, sizeof(output));
                        memory_fill(expected, 0xa5, sizeof(expected));
                        while (read < length)
                        {
                                positive size = escape_oracle(encoded, input[read], policy);
                                if (size > capacity - written)
                                        break;
                                for (positive at = 0; at < size; at++)
                                        expected[written + 8 + at] = encoded[at];
                                written += size;
                                read++;
                        }
                        positive2 result = memory_into_escaped(output + 8, input,
                            length, capacity, policy | (mode ? 128 : 0));
                        if (mode && result.x < read)
                        {
                                check("escape literal yield makes progress", result.x &&
                                    result.x + 1 < length &&
                                    escape_oracle(encoded, input[result.x], policy) == 1 &&
                                    escape_oracle(encoded, input[result.x + 1], policy) == 1);
                                read = result.x;
                                written = 0;
                                memory_fill(expected, 0xa5, sizeof(expected));
                                for (positive at = 0; at < read; at++)
                                        written += escape_oracle(expected + 8 + written, input[at], policy);
                        }
                        check("escape exact capacity and atomic replacements",
                              result.x == read && result.y == written &&
                              !memory_compare(output, expected, sizeof(output)));
                }
        }
}

static fn escape_stream_check(p8 address_to input, positive length)
{
        for (positive policy = 0; policy <= 64; policy++)
        {
                p8 expected[2048], output[2048];
                positive written = 0;
                for (positive at = 0; at < length; at++)
                        written += escape_oracle(expected + written, input[at], policy);
                for (positive mode = 0; mode < 2; mode++)
                for (positive split = 0; split <= length; split++)
                {
                        positive read = 0, used = 0;
                        while (read < length)
                        {
                                positive size = read < split ? split - read : length - read;
                                positive2 chunk = memory_into_escaped(output + used, input + read,
                                    size, 6 + (read % 59), policy | (mode ? 128 : 0));
                                if (!chunk.x)
                                        break;
                                read += chunk.x;
                                used += chunk.y;
                        }
                        check("escape split stream",
                              read == length && used == written &&
                              !memory_compare(output, expected, written));
                }
        }
        // JSON strings exclude NUL, but include high bytes and DEL unchanged.
        p8 expected[2048], value[257];
        positive used = 1;
        expected[0] = '"';
        for (positive at = 0; at < 255; at++)
        {
                value[at] = at + 1;
                used += escape_oracle(expected + used, value[at], 64);
        }
        value[255] = 0;
        expected[used++] = '"';
        escape_used = escape_calls = 0;
        escape_overflow = false;
        writer_json_string(escape_capture, value);
        check("JSON shared spelling and batching", !escape_overflow &&
              escape_used == used && escape_calls < 16 &&
              !memory_compare(expected, escape_output, used));
        for (positive size = 0; size <= 21; size++)
                for (positive byte = 1; byte < 256; byte++)
                {
                        used = 1;
                        for (positive at = 0; at < size; at++)
                        {
                                value[at] = byte;
                                used += escape_oracle(expected + used, byte, 64);
                        }
                        value[size] = 0;
                        expected[used++] = '"';
                        escape_used = escape_calls = 0;
                        escape_overflow = false;
                        writer_json_string(escape_capture, value);
                        check("JSON short cells and batching threshold", !escape_overflow &&
                            escape_used == used && (size > 20 || escape_calls == 1) &&
                            !memory_compare(expected, escape_output, used));
                }
}

static fn escape_scan_check(void)
{
        static const positive sizes[] = {1, 7, 8, 9, 15, 16, 17, 31, 32, 33,
                                         63, 64, 65, 127, 128, 129, 255, 256, 257};
        p8 storage[272], encoded[6];
        memory_fill(storage, 'A', sizeof(storage));
        for (positive byte = 0; byte < 256; byte++)
                for (positive row = 0; row < array_count(sizes); row++)
                {
                        positive size = sizes[row];
                        p8 address_to input = storage + byte % 16;
                        positive places[] = {0, size / 2, size - 1, 7, 8, 15, 16};
                        for (positive which = 0; which < array_count(places); which++)
                        {
                                positive at = places[which];
                                if (at >= size) continue;
                                input[at] = byte;
                                for (positive policy = 0; policy <= 64; policy++)
                                {
                                        check("escape scan every byte across vector lanes",
                                            memory_escape_index(input, size, policy) ==
                                            (escape_oracle(encoded, byte, policy) == 1 ? size : at));
                                        if (size <= 32)
                                                escape_full_check(input, size, policy);
                                }
                                input[at] = 'A';
                        }
                }
}

static fn escape_check(p8 address_to input, positive length)
{
        for (positive policy = 0; policy < 64; policy++)
        {
                p8 expected[1024];
                positive used = 0;
                for (positive at = 0; at < length; at++)
                {
                        p8 byte = input[at];
                        bool escaped = (byte < 32 && byte != '\t' && (policy & HEX_CONTROL)) ||
                            (byte == 127 && (policy & HEX_CONTROL)) ||
                            (byte == '\t' && (policy & HEX_TAB)) ||
                            (byte == ' ' && (policy & HEX_SPACE)) ||
                            (byte == '"' && (policy & HEX_QUOTE)) ||
                            (byte == '\\' && (policy & HEX_SLASH)) ||
                            (byte > 127 && (policy & HEX_HIGH));
                        if (escaped)
                        {
                                expected[used++] = '\\';
                                expected[used++] = 'x';
                                for (b32 shift = 4; shift >= 0; shift -= 4)
                                {
                                        p8 nibble = (byte >> shift) & 15;
                                        expected[used++] = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
                                }
                        }
                        else
                                expected[used++] = byte;
                }
                escape_used = escape_calls = 0;
                escape_overflow = false;
                writer_hex_escaped(escape_capture, input, length, (p8)policy);
                check("hex escape policy and bounded bytes", !escape_overflow &&
                      escape_used == used && !memory_compare(escape_output, expected, used));
                check("hex literal batching", policy || escape_calls == (length != 0));
        }
}

static fn hex_check(p8 address_to out, p8 address_to input, positive size)
{
        check("hex returned length", memory_into_hex(out, input, size) == size * 2);
        bool correct = true;
        for (positive i = 0; i < size * 2; i++)
        {
                // Arithmetic oracle, independent of the assembly lookup table.
                p8 nibble = (input[i / 2] >> ((i & 1) ? 0 : 4)) & 15;
                p8 expected = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
                if (out[i] != expected)
                        correct = false;
        }
        check("hex bytes", correct);
}

static fn hex_suite(void)
{
        p8 source[288], output[560];
        for (positive at = 0; at < 256; at++) source[at] = (p8)at;
        escape_check(source, 256);
        escape_stream_check(source, 256);
        escape_scan_check();
        for (positive offset = 0; offset < 16; offset++)
                for (positive size = 0; size < 66; size++)
                {
                        escape_span_check(source + offset, size);
                        escape_check(source + offset, size);
                }
        escape_check(address_bad, 0);
        escape_span_check(address_bad, 0);
        positive2 empty = memory_into_escaped(address_bad, address_bad, 17, 0, 64);
        check("escape zero capacity does not access input", !empty.x && !empty.y);
        check("hex empty null spans", memory_into_hex(null, null, 0) == 0);
        for (positive source_at = 0; source_at < 16; source_at++)
                for (positive output_at = 1; output_at <= 16; output_at++)
                        for (positive size = 0; size <= 257; size++)
                        {
                                for (positive at = 0; at < sizeof(source); at++)
                                        source[at] = (p8)(at * 197 + size);
                                memory_fill(output, 0xa5, sizeof(output));
                                hex_check(output + output_at, source + source_at, size);
                                bool untouched = true;
                                for (positive at = 0; at < sizeof(output); at++)
                                        if ((at < output_at || at >= output_at + size * 2) &&
                                            output[at] != 0xa5)
                                                untouched = false;
                                for (positive at = 0; at < sizeof(source); at++)
                                        if (source[at] != (p8)(at * 197 + size))
                                                untouched = false;
                                check("hex source and output canaries", untouched);
                        }

        const positive quantum = 65536;
        p8 address_to guarded[2] = {memory(quantum * 3), memory(quantum * 3)};
        bool mapped = guarded[0] && guarded[1] &&
                      (positive)guarded[0] < positive_max - 4095 &&
                      (positive)guarded[1] < positive_max - 4095;
        check("hex guard mappings", mapped);
        if (mapped)
        {
                for (positive at = 0; at < 2; at++)
                {
                        check("hex left guard", system_call_3(syscall(mprotect),
                              (positive)guarded[at], quantum, 0) == 0);
                        check("hex right guard", system_call_3(syscall(mprotect),
                              (positive)(guarded[at] + quantum * 2), quantum, 0) == 0);
                }
                for (positive size = 0; size <= 257; size++)
                        for (positive source_edge = 0; source_edge < 2; source_edge++)
                                for (positive output_edge = 0; output_edge < 2; output_edge++)
                                {
                                        p8 address_to input = guarded[0] +
                                            (source_edge ? quantum * 2 - size : quantum);
                                        p8 address_to out = guarded[1] +
                                            (output_edge ? quantum * 2 - size * 2 : quantum);
                                        for (positive at = 0; at < size; at++)
                                                input[at] = (p8)(at * 37 + size);
                                        hex_check(out, input, size);
                                }
                check("hex inaccessible empty spans",
                      memory_into_hex(guarded[1], guarded[0], 0) == 0);
                for (positive at = 0; at < 256; at++)
                        guarded[0][quantum * 2 - 256 + at] = (p8)at;
                escape_check(guarded[0] + quantum * 2 - 256, 256);
                for (positive size = 0; size < 66; size++)
                {
                        p8 address_to input = guarded[0] + quantum * 2 - size;
                        escape_span_check(input, size);
                        if (size <= 32)
                                for (positive policy = 0; policy <= 64; policy++)
                                {
                                        p8 expected[192];
                                        positive written = 0;
                                        for (positive at = 0; at < size; at++)
                                                written += escape_oracle(expected + written,
                                                    input[at], policy);
                                        p8 address_to into = guarded[1] + quantum * 2 - size * 6;
                                        positive2 chunk = memory_into_escaped(into, input,
                                            size, size * 6, policy);
                                        check("escape short vectors at both exact page ends",
                                            chunk.x == size && chunk.y == written &&
                                            !memory_compare(into, expected, written));
                                }
                        for (positive policy = 0; policy <= 64; policy++)
                                for (positive mode = 0; mode < 2; mode++)
                                for (positive capacity = 0; capacity < 66; capacity++)
                                {
                                        p8 address_to into = guarded[1] + quantum * 2 - capacity;
                                        positive2 chunk = memory_into_escaped(into, input,
                                            size, capacity, policy | (mode ? 128 : 0));
                                        check("escape input/output exact page ends",
                                              chunk.x <= size && chunk.y <= capacity);
                                }
                }
        }
        for (positive at = 0; at < 2; at++)
                if (guarded[at] && (positive)guarded[at] < positive_max - 4095)
                        memory_free(guarded[at], quantum * 3);
}

b32 main(void)
{
#if X64
        p8 vector = cpu_has_avx2;
        cpu_has_avx2 = 0;
        hex_suite();
        cpu_has_avx2 = vector;
        if (vector) hex_suite();
#else
        hex_suite();
#endif
        return test_report(null);
}
