#include "../compiler_memory.c"
/*
        The fused NUL-name pass at every alignment and a protected page edge.

        Its word path may only begin after alignment: a load that crosses the
        terminator into the second page faults instead of being hidden by an
        ordinary mapped neighbour.  The byte model also proves that the hash
        excludes NUL and that the second return register is the exact length.
*/

#define PAGE 4096
#define PROT_NONE 0

static positive checks;
static positive failures;

static positive model_hash(const p8 address_to text, positive length)
{
        positive hash = 5381;

        for (positive at = 0; at < length; at++)
                hash = hash * 33 + text[at];

        return hash;
}

b32 main(void)
{
        p8 address_to room = memory(PAGE * 2);
        p8 address_to edge;

        if (!room)
                return 1;

        edge = room + PAGE;
        if (system_call_3(syscall(mprotect), (positive)edge, PAGE,
                          PROT_NONE) < 0)
                return 1;

        for (positive length = 0; length <= 257; length++)
        {
                p8 address_to text = edge - length - 1;
                positive2 got;
                positive want;

                for (positive at = 0; at < length; at++)
                        text[at] = (p8)(at % 251 + 1);
                text[length] = 0;

                want = model_hash(text, length);
                got = string_hash_33_length((string_address)text);
                checks += 2;

                if (got.x != want)
                {
                        failures++;
                        if (failures < 20)
                                string_format(log,
                                              "FAIL hash length %p: %p != %p\n",
                                              length, got.x, want);
                }

                if (got.y != length)
                {
                        failures++;
                        if (failures < 20)
                                string_format(log,
                                              "FAIL length %p: %p != %p\n",
                                              length, got.y, length);
                }
        }

        string_format(log, "hash+length page edge: %p checks, %p failures\n",
                      checks, failures);
        log_flush();
        return failures != 0;
}
