/*
        Compile-time targeting probe for compiler_memory.c.

        A translation unit sets LIBRARY_SIZE_MAX (and the family INLINE
        knobs) before the include. This file is two literal copies: 16 and
        64 bytes. With LIBRARY_SIZE_MAX 16 the 64-byte site must remain a
        call; with 128 it must be the inlined window. objdump of the two
        probe_* symbols is the evidence.

            cc -O2 -ffreestanding -fno-builtin -fno-stack-protector -c \
                -DLIBRARY_SIZE_MAX=16 -o /tmp/probe16.o \
                test/library_inline_probe.c -I.
            cc -O2 -ffreestanding -fno-builtin -fno-stack-protector -c \
                -DLIBRARY_SIZE_MAX=128 -o /tmp/probe128.o \
                test/library_inline_probe.c -I.
*/

#include "src/compiler_memory.c"

__attribute__((noinline, noclone))
void probe_copy_16(void *d, void *s)
{
        memory_copy(d, s, 16);
}

__attribute__((noinline, noclone))
void probe_copy_64(void *d, void *s)
{
        memory_copy(d, s, 64);
}

__attribute__((noinline, noclone))
void probe_fill_32(void *d)
{
        memory_fill(d, 0, 32);
}
