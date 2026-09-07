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

typedef unsigned long __kernel_size_t;
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

static fn fold_check(string_address what, positive size, p8 address_to source)
{
        fold_checks++;
        for (positive at = 0; at < sizeof(fold_mine); at++)
        {
                p8 want = at >= 96 && at < 96 + size
                    ? (source ? source[at - 96] : 0x5C) : 0xA5;
                if (fold_mine[at] != want)
                {
                        fold_note(what, size, at, fold_mine[at], want);
                        break;
                }
        }
}

/* Literal tokens reach the builtin arms; changed arguments must run once. */
#define FOLD_LITERAL(size)                                                   \
        do {                                                                 \
                positive to = 96, from = 64;                                 \
                memory_fill(fold_mine, 0xA5, sizeof(fold_mine));                \
                if (memcpy(fold_mine + to++, fold_region + from++, size) !=   \
                        fold_mine + 96 || to != 97 || from != 65)             \
                        fold_note("literal copy arguments", size, 0, 1, 0); \
                fold_check("literal copy", size, fold_region + 64);          \
                memory_fill(fold_mine, 0xA5, sizeof(fold_mine));                \
                if (memset(fold_mine + --to, 0x5C, size) != fold_mine + 96 || \
                    to != 96)                                                \
                        fold_note("literal fill arguments", size, 0, 1, 0); \
                fold_check("literal fill", size, null);                      \
        } while (0)

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

                memcpy(fold_mine + 96, source, size);
                memory_copy_apart(fold_theirs + 96, source, size);

                fold_check("copy", size, fold_theirs + 96);

                memory_fill(fold_mine, 0xA5, sizeof(fold_mine));
                memset(fold_mine + 96, 0x5C, size);

                fold_check("fill", size, null);
        }

        FOLD_LITERAL(0);
        FOLD_LITERAL(1);
        FOLD_LITERAL(7);
        FOLD_LITERAL(8);
        FOLD_LITERAL(15);
        FOLD_LITERAL(16);
        FOLD_LITERAL(31);
        FOLD_LITERAL(32);
        FOLD_LITERAL(33);

        positive to = 96, from = 64, count = 8, byte = 0x15C;
        memory_fill(fold_mine, 0xA5, sizeof(fold_mine));
        if (memcpy(fold_mine + to++, fold_region + from++, count++) !=
                fold_mine + 96 || to != 97 || from != 65 || count != 9)
                fold_note("dynamic copy arguments", 8, 0, 1, 0);
        fold_check("dynamic copy", 8, source);
        memory_fill(fold_mine, 0xA5, sizeof(fold_mine));
        if (memset(fold_mine + --to, byte++, count++) != fold_mine + 96 ||
            to != 96 || byte != 0x15D || count != 10)
                fold_note("dynamic fill arguments", 9, 0, 1, 0);
        fold_check("dynamic fill", 9, null);

        string_format(log, "%p checks, %p failures\n", fold_checks,
                      fold_failures);
        log_flush();

        return fold_failures != 0;
}
