#include "../compiler_memory.c"
#include "counted.inc"

static p8 escape_output[1024];
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

b32 main(void)
{
        p8 source[288], output[560];
        for (positive at = 0; at < 256; at++) source[at] = (p8)at;
        escape_check(source, 256);
        escape_check(address_bad, 0);
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
        }
        for (positive at = 0; at < 2; at++)
                if (guarded[at] && (positive)guarded[at] < positive_max - 4095)
                        memory_free(guarded[at], quantum * 3);
        return test_report(null);
}
