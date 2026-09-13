/*
        Floor vs the host libc, same call shapes.

            Linux (spark-free, two binaries):
                cc -O2 -fno-builtin -DUSE_LIBC test/libc_audit.c -o /tmp/audit.libc
                cc -O2 -ffreestanding -fno-builtin -fno-stack-protector \
                    -c -x c -o /tmp/libmw.o -I. src/library.c
                printf '%s\n' '#include "src/library.c"' > /tmp/mw.c
                cc -O2 -ffreestanding -fno-builtin -fno-stack-protector \
                    -c /tmp/mw.c -o /tmp/libmw.o -I.
                cc -O2 -fno-builtin -DUSE_OURS test/libc_audit.c /tmp/libmw.o \
                    -o /tmp/audit.ours

            Darwin (lifted ARM64 bodies):
                python3 test/differential.py --harness native_extract src/library.c \
                    ... > /tmp/lifted.h
                cc -O2 -fno-builtin -DUSE_LIBC test/libc_audit.c -o /tmp/audit.libc
                cc -O2 -fno-builtin -DUSE_LIFTED -I/tmp test/libc_audit.c \
                    -o /tmp/audit.ours

        Prints CSV: name,shape,bytes,ns_per_call
        Ratio is libc/ours (>1 we win) and is joined outside.
*/

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef USE_LIFTED
#define ASM_WIDER(a, b) ""
#define ASM_NARROW(a, b) ""
#define ASM_RET "ret\n"
#define AVX2_WIDTH "32"
#define AVX2_ZEROED ""
#define AVX2_ZEROS_AT(x) ""
#define AVX2_LEAVE ""
#define STRING_COPY_TAIL ""
#define WIDE_LENGTH_INTO(...) ""
#include "lifted.h"
#endif

#ifdef USE_OURS
void *memory_copy_apart(void *, const void *, unsigned long);
void *memory_copy(void *, const void *, unsigned long);
void *memory_fill(void *, int, unsigned long);
unsigned long string_length(const char *);
unsigned long string_length_max(const char *, unsigned long);
int memory_compare(const void *, const void *, unsigned long);
void *memory_first_of(const void *, int, unsigned long);
int string_compare(const char *, const char *);
int string_compare_max(const char *, const char *, unsigned long);
char *string_first_of(const char *, int);
char *string_last_of_or_end(const char *, int);
char *string_copy(char *, const char *);
char *string_search(const char *, const char *);
void *memory_search(const void *, unsigned long, const void *, unsigned long);
#elif defined(USE_LIFTED)
void *memory_copy_apart(void *, const void *, unsigned long);
void *memory_copy(void *, const void *, unsigned long);
void *memory_fill(void *, int, unsigned long);
unsigned long string_length(const char *);
unsigned long string_length_max(const char *, unsigned long);
int memory_compare(const void *, const void *, unsigned long);
void *memory_first_of(const void *, int, unsigned long);
int string_compare(const char *, const char *);
int string_compare_max(const char *, const char *, unsigned long);
char *string_first_of(const char *, int);
char *string_last_of_or_end(const char *, int);
char *string_copy(char *, const char *);
char *string_search(const char *, const char *);
void *memory_search(const void *, unsigned long, const void *, unsigned long);
#endif

#if defined(USE_OURS) || defined(USE_LIFTED)
#define COPY memory_copy_apart
#define MOVE memory_copy
#define FILL memory_fill
#define LEN string_length
#define LENMAX string_length_max
#define CMP memory_compare
#define CHR memory_first_of
#define SCMP string_compare
#define SCMPMAX string_compare_max
#define SCHR string_first_of
#define SRCHR string_last_of_or_end
#define SCOPY string_copy
#define SSTR string_search
#define MMEM memory_search
#else
#define COPY memcpy
#define MOVE memmove
#define FILL memset
#define LEN strlen
#define LENMAX strnlen
#define CMP memcmp
#define CHR memchr
#define SCMP strcmp
#define SCMPMAX strncmp
#define SCHR strchr
#define SRCHR strrchr
#define SCOPY strcpy
#define SSTR strstr
#define MMEM memmem
#endif

enum { ROOM = 1 << 20 };
static unsigned char src[ROOM] __attribute__((aligned(64)));
static unsigned char dst[ROOM] __attribute__((aligned(64)));
static unsigned char src2[ROOM] __attribute__((aligned(64)));
static volatile unsigned long sink;

static uint64_t now_ns(void)
{
#if defined(__APPLE__)
        return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t);
        return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}

static void (*work_fn)(unsigned long, unsigned long);

static uint64_t best_ns(void (*work)(unsigned long, unsigned long),
                        unsigned long size, unsigned long rounds)
{
        uint64_t best = (uint64_t)-1;
        int trial;

        work(size, rounds / 8 + 1);
        for (trial = 0; trial < 5; trial++)
        {
                uint64_t start = now_ns();
                work(size, rounds);
                uint64_t took = now_ns() - start;
                if (took < best)
                        best = took;
        }
        return best;
}

static unsigned long rounds_for(unsigned long guess_ns)
{
        unsigned long rounds = 80000000ull / (guess_ns ? guess_ns : 1);
        if (rounds < 80)
                rounds = 80;
        if (rounds > 2000000)
                rounds = 2000000;
        return rounds;
}

static void memcpy_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)COPY(dst, src, size);
}

static void memmove_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)MOVE(dst, src, size);
}

static void memset_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)FILL(dst, 0xa5, size);
}

static void strlen_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += LEN((char *)src);
}

static void strnlen_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += LENMAX((char *)src, size);
}

static void memcmp_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned)CMP(src, src2, size);
}

static void memchr_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)CHR(src, 'Z', size);
}

static void strcmp_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned)SCMP((char *)src, (char *)src2);
}

static void strncmp_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned)SCMPMAX((char *)src, (char *)src2, size);
}

static void strchr_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)SCHR((char *)src, 'Z');
}

static void strrchr_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)SRCHR((char *)src, 'Z');
}

static void strcpy_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)SCOPY((char *)dst, (char *)src);
}

static void strstr_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)SSTR((char *)src, (char *)src2);
}

static void memmem_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)MMEM(src, size, src2, 3);
}

static void row(const char *name, const char *shape, unsigned long size,
                void (*work)(unsigned long, unsigned long), unsigned long guess)
{
        unsigned long rounds = rounds_for(guess);
        double ns = (double)best_ns(work, size, rounds) / (double)rounds;
        printf("%s,%s,%lu,%.3f\n", name, shape, size, ns);
}

static void fill_az(unsigned long n)
{
        unsigned long i;
        for (i = 0; i < n; i++)
                src[i] = (unsigned char)('a' + (i % 26));
        src[n] = 0;
        memcpy(src2, src, n + 1);
}

int main(void)
{
        static const unsigned long sizes[] = {8, 16, 32, 64, 256, 4096, 65536};
        unsigned long s, n, guess;

        for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
        {
                n = sizes[s];
                guess = n < 64 ? 8 : n < 4096 ? 20 : n / 8;

                memset(src, 0x5a, n);
                row("memcpy", "copy", n, memcpy_w, guess);
                row("memmove", "apart", n, memmove_w, guess);
                row("memset", "fill", n, memset_w, guess);

                fill_az(n);
                row("strlen", "len", n, strlen_w, guess);
                row("strnlen", "len", n, strnlen_w, guess);
                row("strcmp", "equal", n, strcmp_w, guess);
                row("strncmp", "equal", n, strncmp_w, guess);
                row("strcpy", "copy", n, strcpy_w, guess);

                memcpy(src2, src, n + 1);
                row("memcmp", "equal", n, memcmp_w, guess);
                src2[0] ^= 0xff;
                row("memcmp", "miss@0", n, memcmp_w, 8);
                memcpy(src2, src, n + 1);
                src2[n > 8 ? 7 : 0] ^= 0xff;
                row("memcmp", "miss@7", n, memcmp_w, 8);
                memcpy(src2, src, n + 1);
                src2[n - 1] ^= 0xff;
                row("memcmp", "miss@end", n, memcmp_w, guess);

                memcpy(src2, src, n + 1);
                src2[0] ^= 1;
                row("strcmp", "miss@0", n, strcmp_w, 8);
                memcpy(src2, src, n + 1);
                src2[n > 8 ? 7 : 0] ^= 1;
                row("strcmp", "miss@7", n, strcmp_w, 8);
                memcpy(src2, src, n + 1);
                src2[n - 1] ^= 1;
                row("strcmp", "miss@end", n, strcmp_w, guess);

                memcpy(src2, src, n + 1);
                src2[n > 8 ? 7 : 0] ^= 1;
                row("strncmp", "miss@7", n, strncmp_w, 8);

                memset(src, 'a', n);
                src[0] = 'Z';
                src[n] = 0;
                row("strchr", "at-0", n, strchr_w, 8);
                row("memchr", "at-0", n, memchr_w, 8);
                src[0] = 'a';
                src[n - 1] = 'Z';
                row("strchr", "at-end", n, strchr_w, guess);
                row("strrchr", "at-end", n, strrchr_w, guess);
                row("memchr", "at-end", n, memchr_w, guess);

                memset(src, 'a', n);
                src[n] = 0;
                src2[0] = 'x';
                src2[1] = 'y';
                src2[2] = 'z';
                src2[3] = 0;
                row("strstr", "miss", n, strstr_w, n < 256 ? 20 : n / 20);
                row("memmem", "miss", n, memmem_w, n < 256 ? 20 : n / 20);
                memcpy(src + (n > 3 ? n - 3 : 0), "xyz", 3);
                src[n] = 0;
                row("strstr", "hit-end", n, strstr_w, n < 256 ? 20 : n / 20);
                row("memmem", "hit-end", n, memmem_w, n < 256 ? 20 : n / 20);
        }

        return sink ? 0 : 0;
}
