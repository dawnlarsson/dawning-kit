#include "library.c"
/*
        Where a kernel can be sped up without touching a vector register.

        x86 hands its own architecture three symbols -- memcpy, memmove,
        memset -- and leaves the other ten to lib/string.c, which is a byte a
        turn. Those are the ones with room in them, and taking them costs
        nothing in safety: the narrow bodies are general purpose registers
        only, so there is no vector state to preserve and nothing an emulator
        behind a device mapping cannot decode.

        The flags are forced down so this is what a kernel build runs, not
        what userspace runs.

        The references are what lib/string.c is, compiled the way the kernel
        compiles it: a byte at a time, and not vectorised, because a kernel
        build has no vector registers to vectorise into.
*/
static p8 buf[1 << 20] __attribute__((aligned(64)));
static p8 two[1 << 20] __attribute__((aligned(64)));

#define PLAIN __attribute__((noinline, optimize("no-tree-vectorize")))

PLAIN static positive byte_strlen(const p8 address_to s)
{ const p8 address_to p = s; while (*p) p++; return (positive)(p - s); }

PLAIN static b32 byte_strcmp(const p8 address_to a, const p8 address_to b)
{ while (*a && *a == *b) { a++; b++; } return (b32)*a - (b32)*b; }

PLAIN static b32 byte_memcmp(const p8 address_to a, const p8 address_to b, positive n)
{ for (positive i = 0; i < n; i++) if (a[i] != b[i]) return (b32)a[i] - (b32)b[i]; return 0; }

PLAIN static const p8 address_to byte_strchr(const p8 address_to s, p8 c)
{ for (;; s++) { if (*s == c) return s; if (!*s) return 0; } }

PLAIN static const p8 address_to byte_memchr(const p8 address_to s, p8 c, positive n)
{ for (positive i = 0; i < n; i++) if (s[i] == c) return s + i; return 0; }

static volatile positive sink;

b32 main(void)
{
        moonwater_cpu_detect();
        cpu_has_avx2 = 0;
        cpu_has_avx512 = 0;

        for (positive i = 0; i < sizeof(buf); i++) { buf[i] = 'a'; two[i] = 'a'; }

        p64 s, mine, theirs;
        b32 rounds;

        string_format(log, "  routine        bytes    lib/string.c      ours       x\n");

#define ROW(name, n, rnds, theirs_call, mine_call)                            \
        buf[n] = 0; two[n] = 0;                                               \
        rounds = rnds;                                                        \
        s = get_cpu_time();                                                   \
        for (b32 r = 0; r < rounds; r++) sink += (positive)(theirs_call);     \
        theirs = get_cpu_time() - s;                                          \
        s = get_cpu_time();                                                   \
        for (b32 r = 0; r < rounds; r++) sink += (positive)(mine_call);       \
        mine = get_cpu_time() - s;                                            \
        buf[n] = 'a'; two[n] = 'a';                                           \
        string_format(log, "  %s   %p        %p       %p     %p.%px\n",       \
                      (const p8 address_to)name, (positive)n,                 \
                      theirs / 1000, mine / 1000,                             \
                      theirs / mine, (theirs * 10 / mine) % 10);

        ROW("strlen ", 16,   2000000, byte_strlen(buf), string_length(buf))
        ROW("strlen ", 256,  1000000, byte_strlen(buf), string_length(buf))
        ROW("strlen ", 4096, 200000,  byte_strlen(buf), string_length(buf))
        ROW("strcmp ", 16,   2000000, byte_strcmp(buf, two), string_compare(buf, two))
        ROW("strcmp ", 256,  1000000, byte_strcmp(buf, two), string_compare(buf, two))
        ROW("strcmp ", 4096, 200000,  byte_strcmp(buf, two), string_compare(buf, two))
        ROW("memcmp ", 256,  1000000, byte_memcmp(buf, two, 256), memory_compare(buf, two, 256))
        ROW("memcmp ", 4096, 200000,  byte_memcmp(buf, two, 4096), memory_compare(buf, two, 4096))
        ROW("strchr ", 256,  1000000, byte_strchr(buf, 'z'), string_first_of(buf, 'z'))
        ROW("strchr ", 4096, 200000,  byte_strchr(buf, 'z'), string_first_of(buf, 'z'))
        ROW("memchr ", 4096, 200000,  byte_memchr(buf, 'z', 4096), memory_first_of(buf, 'z', 4096))

        log_flush();
        return 0;
}
