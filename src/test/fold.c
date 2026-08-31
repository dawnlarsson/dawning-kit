/*
        The constant-size fold that kernel/patch/fold-x86.h puts into the
        kernel's own string header, checked against the routines it stands in
        for.

        WHY THIS FILE EXISTS. The fold is the one edit we make to Linux that
        changes what a CALL SITE compiles to rather than what a name resolves
        to, and it lands in a header that every kernel translation unit
        includes. A fault in it is a fault everywhere. The first version wrote
        five bytes behind its destination for a count of three, and the kernel
        took a general protection fault in PID 1 before the first shell.

        WHY THE POISON IS DIFFERENT IN EACH BUFFER. The first version of this
        check filled every buffer with the same byte and PASSED against that
        broken fold: the out of bounds READ landed in the neighbouring array,
        which held the same poison, so it wrote the poison back over the
        poison and the comparison saw nothing. Each region has its own value
        now, and the source sits inside a third, so a read or a write outside
        the destination shows up as the wrong byte rather than as the right
        one by accident.

        The fold is header-only and compiles as ordinary C, so this runs on
        the host like any other lane rather than needing a kernel.
*/
#include "../compiler_memory.c"

typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;
typedef unsigned long __kernel_size_t;

#ifndef __always_inline
#define __always_inline INLINE
#endif

#define FOLD_ONLY_BODIES 1
#include "../../kernel/patch/fold-x86.h"

static p8 fold_mine[256];
static p8 fold_theirs[256];
static p8 fold_region[256];

static positive fold_checks;
static positive fold_failures;

static fn fold_note(string_address what, positive size, positive at,
                    positive got, positive want)
{
        fold_failures++;
        string_format(log, "  %s size %p byte %p: %p want %p\n",
                      what, size, at, got, want);
}

b32 main(void)
{
        positive size;
        positive at;
        p8 address_to source = fold_region + 64;

        for (size = 0; size <= MOONWATER_FOLD_MAX; size++)
        {
                memory_fill(fold_mine, 0xA5, sizeof(fold_mine));
                memory_fill(fold_theirs, 0x3C, sizeof(fold_theirs));
                memory_fill(fold_region, 0x77, sizeof(fold_region));

                for (at = 0; at < 64; at++)
                        source[at] = (p8)(at * 37 + 11);

                moonwater_fold_copy(fold_mine + 96, source, size);
                memory_copy_apart(fold_theirs + 96, source, size);

                fold_checks++;
                for (at = 0; at < sizeof(fold_mine); at++)
                {
                        p8 want = (at >= 96 && at < 96 + size)
                                          ? fold_theirs[at] : 0xA5;

                        if (fold_mine[at] != want)
                        {
                                fold_note((string_address) "copy", size, at,
                                          fold_mine[at], want);
                                break;
                        }
                }

                memory_fill(fold_mine, 0xA5, sizeof(fold_mine));
                moonwater_fold_fill(fold_mine + 96, 0x5C, size);

                fold_checks++;
                for (at = 0; at < sizeof(fold_mine); at++)
                {
                        p8 want = (at >= 96 && at < 96 + size) ? 0x5C : 0xA5;

                        if (fold_mine[at] != want)
                        {
                                fold_note((string_address) "fill", size, at,
                                          fold_mine[at], want);
                                break;
                        }
                }
        }

        string_format(log, "%p checks, %p failures\n", fold_checks,
                      fold_failures);
        log_flush();

        return fold_failures != 0;
}
