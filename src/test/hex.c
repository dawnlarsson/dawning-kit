#include "../compiler_memory.c"
#include "counted.inc"

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
        }
        for (positive at = 0; at < 2; at++)
                if (guarded[at] && (positive)guarded[at] < positive_max - 4095)
                        memory_free(guarded[at], quantum * 3);
        return test_report(null);
}
