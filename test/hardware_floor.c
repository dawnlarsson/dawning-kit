/*
        Each CPU-bound library.c leaf against this machine's traffic floor.

        Not vs glibc. The floor is a loop that moves the same bytes and
        computes nothing: one-stream read, two-stream read, copy, or fill.
        Ratio is ours_ns / floor_ns. 1.00 is the traffic bound. Below 1
        the floor was not a floor (printed "unresolved" in BENCH_floor).
        Compute and latency leaves have no traffic bound that means the
        same thing; they still print a floor column so the CSV is one shape.

            Darwin (lifted ARM64 bodies):
                python3 test/differential.py --harness native_extract \
                    src/library.c $NAMES > /tmp/lifted.h
                cc -O2 -fno-builtin -DUSE_LIFTED -DSKIP_SHA256 -DSKIP_GHASH -DSKIP_FIELD -DSKIP_AES -DSKIP_HEX -DSKIP_LOCK \
                    -DSKIP_ITOA -I/tmp test/hardware_floor.c -o /tmp/hwfloor

            Darwin names (sha256/ghash/field/aes/hex/itoa skipped: Mach-O :lo12: tables
                or not lifted):
                memory_copy_apart memory_copy memory_fill memory_fill_32
                memory_fill_64 memory_reverse memory_frob
                memory_to_lower_ascii memory_to_upper_ascii
                memory_exchange_apart memory_translate string_length
                string_length_max memory_compare memory_common_prefix
                memory_compare_ascii_case memory_first_of memory_last_of
                memory_count memory_sum_bytes memory_hash_33 hash_xxh64
                memory_checksum_bsd16 memory_span_byte memory_utf8_span
                string_compare string_compare_max string_first_of
                string_first_of_or_end string_last_of_or_end string_copy
                string_copy_max string_search string_find memory_search
                memory_search_prepare memory_search_prepared
                memory_search_prepared_core string_to_positive
                memory_copy_end memory_copy_apart_end

            Linux (outlined library.c):
                printf '%s\n' '#include "src/library.c"' > /tmp/mw.c
                cc -O2 -ffreestanding -fno-builtin -fno-stack-protector \
                    -c /tmp/mw.c -o /tmp/libmw.o -I.
                objcopy --redefine-sym _start=moonwater_start /tmp/libmw.o
                cc -O2 -fno-builtin -DUSE_OURS test/hardware_floor.c \
                    /tmp/libmw.o -o /tmp/hwfloor

        Prints CSV: arch,name,shape,class,bytes,ours_ns,floor_ns,ratio
        qemu-user is not a hardware floor; do not quote it as one.
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
#include "lifted.h"
#endif

#if defined(__APPLE__)
#define ASM_C_SYM(name) "_" name
#else
#define ASM_C_SYM(name) name
#endif

__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl " ASM_C_SYM("floor_read") "\n"
    ASM_C_SYM("floor_read") ":\n"
#if defined(__x86_64__)
    "xor %eax, %eax\n   test %rsi, %rsi\n   jz 9f\n"
    "1:  cmp $64, %rsi\n   jb 5f\n"
    "movdqu 0(%rdi), %xmm0\n   movdqu 16(%rdi), %xmm1\n"
    "movdqu 32(%rdi), %xmm2\n   movdqu 48(%rdi), %xmm3\n"
    "add $64, %rdi\n   sub $64, %rsi\n   jmp 1b\n"
    "5:  test %rsi, %rsi\n   jz 9f\n"
    "6:  add (%rdi), %rax\n   add $8, %rdi\n   sub $8, %rsi\n"
    "cmp $8, %rsi\n   jae 6b\n"
    "9:  ret\n"
#elif defined(__aarch64__)
    "mov x2, #0\n   cbz x1, 9f\n"
    "1:  cmp x1, #64\n   b.lo 5f\n"
    "ldp q0, q1, [x0]\n   ldp q2, q3, [x0, #32]\n"
    "add x0, x0, #64\n   sub x1, x1, #64\n   b 1b\n"
    "5:  cbz x1, 9f\n"
    "6:  ldr x3, [x0]\n   add x2, x2, x3\n   add x0, x0, #8\n"
    "subs x1, x1, #8\n   b.hi 6b\n"
    "9:  mov x0, x2\n   ret\n"
#else
    "li a2, 0\n   beqz a1, 9f\n"
    "1:  ld a3, 0(a0)\n   add a2, a2, a3\n"
    "addi a0, a0, 8\n   addi a1, a1, -8\n   bnez a1, 1b\n"
    "9:  mv a0, a2\n   ret\n"
#endif

    ".p2align 4\n"
    ".globl " ASM_C_SYM("floor_read_two") "\n"
    ASM_C_SYM("floor_read_two") ":\n"
#if defined(__x86_64__)
    "xor %eax, %eax\n   test %rdx, %rdx\n   jz 9f\n"
    "1:  cmp $64, %rdx\n   jb 5f\n"
    "movdqu 0(%rdi), %xmm0\n   movdqu 16(%rdi), %xmm1\n"
    "movdqu 32(%rdi), %xmm2\n   movdqu 48(%rdi), %xmm3\n"
    "movdqu 0(%rsi), %xmm4\n   movdqu 16(%rsi), %xmm5\n"
    "movdqu 32(%rsi), %xmm6\n   movdqu 48(%rsi), %xmm7\n"
    "add $64, %rdi\n   add $64, %rsi\n   sub $64, %rdx\n   jmp 1b\n"
    "5:  test %rdx, %rdx\n   jz 9f\n"
    "6:  add (%rdi), %rax\n   add (%rsi), %rax\n"
    "add $8, %rdi\n   add $8, %rsi\n   sub $8, %rdx\n"
    "cmp $8, %rdx\n   jae 6b\n"
    "9:  ret\n"
#elif defined(__aarch64__)
    "mov x3, #0\n   cbz x2, 9f\n"
    "1:  cmp x2, #64\n   b.lo 5f\n"
    "ldp q0, q1, [x0]\n   ldp q2, q3, [x0, #32]\n"
    "ldp q4, q5, [x1]\n   ldp q6, q7, [x1, #32]\n"
    "add x0, x0, #64\n   add x1, x1, #64\n"
    "sub x2, x2, #64\n   b 1b\n"
    "5:  cbz x2, 9f\n"
    "6:  ldr x4, [x0], #8\n   ldr x5, [x1], #8\n"
    "add x3, x3, x4\n   add x3, x3, x5\n"
    "subs x2, x2, #8\n   b.hi 6b\n"
    "9:  mov x0, x3\n   ret\n"
#else
    "li a3, 0\n   beqz a2, 9f\n"
    "1:  ld a4, 0(a0)\n   ld a5, 0(a1)\n"
    "add a3, a3, a4\n   add a3, a3, a5\n"
    "addi a0, a0, 8\n   addi a1, a1, 8\n   addi a2, a2, -8\n   bnez a2, 1b\n"
    "9:  mv a0, a3\n   ret\n"
#endif

    ".p2align 4\n"
    ".globl " ASM_C_SYM("floor_copy") "\n"
    ASM_C_SYM("floor_copy") ":\n"
#if defined(__x86_64__)
    "xor %eax, %eax\n   test %rdx, %rdx\n   jz 9f\n"
    "1:  cmp $64, %rdx\n   jb 5f\n"
    "movdqu 0(%rsi), %xmm0\n   movdqu 16(%rsi), %xmm1\n"
    "movdqu 32(%rsi), %xmm2\n   movdqu 48(%rsi), %xmm3\n"
    "movdqu %xmm0, 0(%rdi)\n   movdqu %xmm1, 16(%rdi)\n"
    "movdqu %xmm2, 32(%rdi)\n   movdqu %xmm3, 48(%rdi)\n"
    "add $64, %rdi\n   add $64, %rsi\n   sub $64, %rdx\n   jmp 1b\n"
    "5:  test %rdx, %rdx\n   jz 9f\n"
    "6:  mov (%rsi), %rax\n   mov %rax, (%rdi)\n"
    "add $8, %rdi\n   add $8, %rsi\n   sub $8, %rdx\n"
    "cmp $8, %rdx\n   jae 6b\n"
    "9:  ret\n"
#elif defined(__aarch64__)
    "cbz x2, 9f\n"
    "1:  cmp x2, #64\n   b.lo 5f\n"
    "ldp q0, q1, [x1]\n   ldp q2, q3, [x1, #32]\n"
    "stp q0, q1, [x0]\n   stp q2, q3, [x0, #32]\n"
    "add x0, x0, #64\n   add x1, x1, #64\n"
    "sub x2, x2, #64\n   b 1b\n"
    "5:  cbz x2, 9f\n"
    "6:  ldr x3, [x1], #8\n   str x3, [x0], #8\n"
    "subs x2, x2, #8\n   b.hi 6b\n"
    "9:  mov x0, #0\n   ret\n"
#else
    "beqz a2, 9f\n"
    "1:  ld a3, 0(a1)\n   sd a3, 0(a0)\n"
    "addi a0, a0, 8\n   addi a1, a1, 8\n   addi a2, a2, -8\n   bnez a2, 1b\n"
    "9:  li a0, 0\n   ret\n"
#endif

    ".p2align 4\n"
    ".globl " ASM_C_SYM("floor_fill") "\n"
    ASM_C_SYM("floor_fill") ":\n"
#if defined(__x86_64__)
    "xor %eax, %eax\n   test %rsi, %rsi\n   jz 9f\n"
    "pxor %xmm0, %xmm0\n"
    "1:  cmp $64, %rsi\n   jb 5f\n"
    "movdqu %xmm0, 0(%rdi)\n   movdqu %xmm0, 16(%rdi)\n"
    "movdqu %xmm0, 32(%rdi)\n   movdqu %xmm0, 48(%rdi)\n"
    "add $64, %rdi\n   sub $64, %rsi\n   jmp 1b\n"
    "5:  test %rsi, %rsi\n   jz 9f\n"
    "6:  mov %rax, (%rdi)\n   add $8, %rdi\n   sub $8, %rsi\n"
    "cmp $8, %rsi\n   jae 6b\n"
    "9:  ret\n"
#elif defined(__aarch64__)
    "movi v0.16b, #0\n   cbz x1, 9f\n"
    "1:  cmp x1, #64\n   b.lo 5f\n"
    "stp q0, q0, [x0]\n   stp q0, q0, [x0, #32]\n"
    "add x0, x0, #64\n   sub x1, x1, #64\n   b 1b\n"
    "5:  cbz x1, 9f\n"
    "6:  str xzr, [x0], #8\n   subs x1, x1, #8\n   b.hi 6b\n"
    "9:  mov x0, #0\n   ret\n"
#else
    "beqz a1, 9f\n"
    "1:  sd zero, 0(a0)\n   addi a0, a0, 8\n"
    "addi a1, a1, -8\n   bnez a1, 1b\n"
    "9:  li a0, 0\n   ret\n"
#endif
);

unsigned long floor_read(void *, unsigned long);
unsigned long floor_read_two(void *, void *, unsigned long);
unsigned long floor_copy(void *, void *, unsigned long);
unsigned long floor_fill(void *, unsigned long);

#if defined(USE_OURS) || defined(USE_LIFTED)
void *memory_copy_apart(void *, const void *, unsigned long);
void *memory_copy(void *, const void *, unsigned long);
void *memory_fill(void *, int, unsigned long);
void *memory_fill_32(void *, unsigned int, unsigned long);
void *memory_fill_64(void *, unsigned long, unsigned long);
void *memory_reverse(void *, unsigned long);
void *memory_frob(void *, unsigned long);
void *memory_to_lower_ascii(void *, unsigned long);
void *memory_to_upper_ascii(void *, unsigned long);
void *memory_exchange_apart(void *, void *, unsigned long);
void *memory_translate(void *, unsigned long, const void *);
unsigned long string_length(const char *);
unsigned long string_length_max(const char *, unsigned long);
int memory_compare(const void *, const void *, unsigned long);
unsigned long memory_common_prefix(const void *, const void *, unsigned long);
int memory_compare_ascii_case(const void *, const void *, unsigned long);
void *memory_first_of(const void *, int, unsigned long);
void *memory_last_of(const void *, int, unsigned long);
unsigned long memory_count(const void *, unsigned long, int);
unsigned long memory_sum_bytes(const void *, unsigned long);
unsigned long memory_hash_33(const void *, unsigned long);
unsigned long long hash_xxh64(const void *, unsigned long, unsigned long long);
unsigned memory_checksum_bsd16(const void *, unsigned long, unsigned);
#ifndef SKIP_SHA256
void sha256_compress(unsigned int *, unsigned char *);
void md5_blocks(unsigned int *, const unsigned char *, unsigned long);
void sha1_blocks(unsigned int *, const unsigned char *, unsigned long);
void sha256_blocks(unsigned int *, const unsigned char *, unsigned long);
void sha512_blocks(unsigned long *, const unsigned char *, unsigned long);
void blake2b_blocks(unsigned long *, const unsigned char *, unsigned long,
                    unsigned long);
#endif
#ifndef SKIP_AES
void aes128_ctr_blocks(const unsigned char *, unsigned char *,
                       const unsigned char *, unsigned char *, unsigned long);
#endif
#ifndef SKIP_FIELD
void p256_multiply(unsigned long *, const unsigned long *, const unsigned long *);
void p256_square(unsigned long *, const unsigned long *);
void p256_add(unsigned long *, const unsigned long *, const unsigned long *);
void p256_subtract(unsigned long *, const unsigned long *, const unsigned long *);
void p384_multiply(unsigned long *, const unsigned long *, const unsigned long *);
void p384_square(unsigned long *, const unsigned long *);
void p384_add(unsigned long *, const unsigned long *, const unsigned long *);
void p384_subtract(unsigned long *, const unsigned long *, const unsigned long *);
#endif
#ifndef SKIP_GHASH
void ghash_key(unsigned char *, const unsigned char *);
void ghash_blocks(unsigned char *, const unsigned char *,
                  const unsigned char *, unsigned long);
#endif
#ifndef SKIP_HEX
unsigned long memory_into_hex(void *, const void *, unsigned long);
#endif
unsigned long memory_span_byte(const void *, int, unsigned long);
typedef struct { unsigned long x, y; } floor_positive2;
floor_positive2 memory_utf8_span(const void *, unsigned long, unsigned long);
int string_compare(const char *, const char *);
int string_compare_max(const char *, const char *, unsigned long);
char *string_first_of(const char *, int);
char *string_last_of_or_end(const char *, int);
char *string_copy(char *, const char *);
char *string_copy_max(char *, const char *, unsigned long);
char *string_search(const char *, const char *);
char *string_find(const char *, const char *);
void *memory_search(const void *, unsigned long, const void *, unsigned long);
#ifndef SKIP_ITOA
unsigned long positive_into(char *, unsigned long);
#endif
unsigned long string_to_positive(const char *);
void *memory_copy_end(void *, const void *, unsigned long);
void *memory_copy_apart_end(void *, const void *, unsigned long);
#endif

/* The lock is Linux runtime assembly (in library.c) and only the
   outlined library carries it. threads_live is forced to one around the
   company row to time the atomic path in this single-threaded harness. */
#if defined(USE_OURS) && !defined(SKIP_LOCK)
typedef struct { int word; } floor_lock;
void lock_take(floor_lock *);
void lock_release(floor_lock *);
extern unsigned long threads_live;
#endif

enum { ROOM = 1 << 20 };
static unsigned char src[ROOM] __attribute__((aligned(64)));
static unsigned char dst[ROOM] __attribute__((aligned(64)));
static unsigned char src2[ROOM] __attribute__((aligned(64)));
static unsigned char table[256] __attribute__((aligned(64)));
static unsigned int sha_state[8];
static unsigned char sha_block[64];
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

static uint64_t best_ns(void (*work)(unsigned long, unsigned long),
                        unsigned long size, unsigned long rounds)
{
        uint64_t best = (uint64_t)-1;
        int trial;
        unsigned long live = rounds;

        /* Keep the iteration count in a GPR across the body: some ARM64
           leaves clobber v8-v15 (AAPCS callee-saved SIMD). Clang will
           otherwise hoist (double)rounds into d8 and the ratio becomes inf. */
        __asm__ volatile("" : "+r"(live) : : "memory");
        work(size, live / 8 + 1);
        for (trial = 0; trial < 5; trial++) {
                uint64_t start = now_ns();
                work(size, live);
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

static const char *arch_name(void)
{
#if defined(__x86_64__)
        return "x86_64";
#elif defined(__aarch64__)
        return "arm64";
#elif defined(__riscv) && __riscv_xlen == 64
        return "riscv64";
#else
        return "unknown";
#endif
}

static void row(const char *name, const char *shape, const char *klass,
                unsigned long size, void (*ours)(unsigned long, unsigned long),
                void (*floor)(unsigned long, unsigned long),
                unsigned long guess)
{
        unsigned long rounds = rounds_for(guess);
        uint64_t ours_ticks = best_ns(ours, size, rounds);
        uint64_t floor_ticks = best_ns(floor, size, rounds);
        double ours_ns, floor_ns, ratio;

        __asm__ volatile("" : "+r"(rounds) : : "memory");
        ours_ns = (double)ours_ticks / (double)(rounds ? rounds : 1);
        floor_ns = (double)floor_ticks / (double)(rounds ? rounds : 1);
        ratio = floor_ns > 0 ? ours_ns / floor_ns : 0;

        printf("%s,%s,%s,%s,%lu,%.3f,%.3f,%.3f\n", arch_name(), name, shape,
               klass, size, ours_ns, floor_ns, ratio);
}

static void floor_one_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += floor_read(src, size);
}

static void floor_two_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += floor_read_two(src, src2, size);
}

static void floor_copy_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += floor_copy(dst, src, size);
}

static void floor_fill_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += floor_fill(dst, size);
}

static void memcpy_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_copy_apart(dst, src, size);
}

static void memmove_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_copy(dst, src, size);
}

static void memset_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_fill(dst, 0xa5, size);
}

static void fill32_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        unsigned long words = size / 4;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_fill_32(dst, 0xa5a5a5a5u, words);
}

static void fill64_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        unsigned long words = size / 8;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_fill_64(dst, 0xa5a5a5a5a5a5a5a5ull,
                                                     words);
}

static void reverse_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_reverse(dst, size);
}

static void frob_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_frob(dst, size);
}

static void lower_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_to_lower_ascii(dst, size);
}

static void upper_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_to_upper_ascii(dst, size);
}

static void exchange_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_exchange_apart(dst, src, size);
}

static void translate_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_translate(dst, size, table);
}

#ifndef SKIP_HEX
static void hex_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        unsigned long n = size > ROOM / 2 ? ROOM / 2 : size;
        for (i = 0; i < rounds; i++)
                sink += memory_into_hex(dst, src, n);
}
#endif

static void strlen_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        (void)size;
        for (i = 0; i < rounds; i++)
                sink += string_length((char *)src);
}

static void strnlen_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += string_length_max((char *)src, size);
}

static void memcmp_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned)memory_compare(src, src2, size);
}

static void prefix_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += memory_common_prefix(src, src2, size);
}

static void memcmp_case_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned)memory_compare_ascii_case(src, src2, size);
}

static void memchr_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_first_of(src, 'Z', size);
}

static void memrchr_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_last_of(src, 'Z', size);
}

static void count_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += memory_count(src, size, 'a');
}

static void sum_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += memory_sum_bytes(src, size);
}

static void hash33_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += memory_hash_33(src, size);
}

static void xxh_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)hash_xxh64(src, size, 0);
}

static void bsd16_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += memory_checksum_bsd16(src, size, 0);
}

#ifndef SKIP_SHA256
#if defined(USE_OURS) && !defined(SKIP_LOCK)
static floor_lock lock_word;

static void lock_pair_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        (void)size;
        for (i = 0; i < rounds; i++) {
                lock_take(&lock_word);
                sink += (unsigned long)lock_word.word;
                lock_release(&lock_word);
        }
}

/* The traffic of an uncontended pair: the word goes to 1 and back to 0. */
static void lock_store_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        (void)size;
        for (i = 0; i < rounds; i++) {
                lock_word.word = 1;
                sink += (unsigned long)lock_word.word;
                lock_word.word = 0;
        }
}

/* What standard.c's C lock was: an inline compare-and-swap and exchange. */
static void lock_atomic_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        (void)size;
        for (i = 0; i < rounds; i++) {
                __sync_bool_compare_and_swap(&lock_word.word, 0, 1);
                sink += (unsigned long)lock_word.word;
                __atomic_exchange_n(&lock_word.word, 0, __ATOMIC_SEQ_CST);
        }
}
#endif

static void sha_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        (void)size;
        for (i = 0; i < rounds; i++) {
                sha256_compress(sha_state, sha_block);
                sink += sha_state[0];
        }
}

/*
        The hash block cores over 64 KiB against the textbook rounds compiled
        from C here: no renamed registers, no carried terms, no extension
        instructions -- what every body has to beat, not a traffic bound.
        The cores ask for their extension bodies themselves on the first
        call, so these rows time what the machine has: SHA-NI for sha1 and
        sha256 on a processor with it, the floor for the rest.
*/
#define HASH_BYTES 65536
static unsigned char hash_data[HASH_BYTES] __attribute__((aligned(64)));
static unsigned int hash_narrow[8];
static unsigned long hash_wide[11];

static unsigned int hash_rol(unsigned int x, int s) { return (x << s) | (x >> ((32 - s) & 31)); }
static unsigned long hash_ror(unsigned long x, int s) { return (x >> s) | (x << ((64 - s) & 63)); }
static unsigned int hash_le(const unsigned char *p)
{
        return (unsigned int)p[0] | (unsigned int)p[1] << 8 | (unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
}
static unsigned int hash_be(const unsigned char *p)
{
        return (unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 | (unsigned int)p[2] << 8 | (unsigned int)p[3];
}

static const unsigned int hash_c_md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static void hash_c_md5(unsigned int *st, const unsigned char *d, unsigned long n)
{
        static const unsigned char s[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
        for (; n; n--, d += 64) {
                unsigned int x[16], a = st[0], b = st[1], c = st[2], e = st[3];
                int i;
                for (i = 0; i < 16; i++)
                        x[i] = hash_le(d + 4 * i);
                for (i = 0; i < 64; i++) {
                        unsigned int f, t;
                        int g;
                        if (i < 16) { f = (b & c) | (~b & e); g = i; }
                        else if (i < 32) { f = (e & b) | (~e & c); g = (5 * i + 1) & 15; }
                        else if (i < 48) { f = b ^ c ^ e; g = (3 * i + 5) & 15; }
                        else { f = c ^ (b | ~e); g = (7 * i) & 15; }
                        t = e; e = c; c = b;
                        b = b + hash_rol(a + f + hash_c_md5_k[i] + x[g], s[i / 16][i % 4]);
                        a = t;
                }
                st[0] += a; st[1] += b; st[2] += c; st[3] += e;
        }
}

static void hash_c_sha1(unsigned int *st, const unsigned char *d, unsigned long n)
{
        for (; n; n--, d += 64) {
                unsigned int w[80], a = st[0], b = st[1], c = st[2], e = st[3], h = st[4];
                int i;
                for (i = 0; i < 16; i++)
                        w[i] = hash_be(d + 4 * i);
                for (i = 16; i < 80; i++)
                        w[i] = hash_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
                for (i = 0; i < 80; i++) {
                        unsigned int f, k, t;
                        if (i < 20) { f = (b & c) | (~b & e); k = 0x5a827999; }
                        else if (i < 40) { f = b ^ c ^ e; k = 0x6ed9eba1; }
                        else if (i < 60) { f = (b & c) | (b & e) | (c & e); k = 0x8f1bbcdc; }
                        else { f = b ^ c ^ e; k = 0xca62c1d6; }
                        t = hash_rol(a, 5) + f + h + k + w[i];
                        h = e; e = c; c = hash_rol(b, 30); b = a; a = t;
                }
                st[0] += a; st[1] += b; st[2] += c; st[3] += e; st[4] += h;
        }
}

static void hash_c_sha256(unsigned int *st, const unsigned char *d, unsigned long n)
{
        static const unsigned int k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };
        for (; n; n--, d += 64) {
                unsigned int w[64], v[8];
                int i;
                for (i = 0; i < 16; i++)
                        w[i] = hash_be(d + 4 * i);
                for (i = 16; i < 64; i++)
                        w[i] = w[i - 16] + w[i - 7] +
                               (hash_rol(w[i - 15], 25) ^ hash_rol(w[i - 15], 14) ^ (w[i - 15] >> 3)) +
                               (hash_rol(w[i - 2], 15) ^ hash_rol(w[i - 2], 13) ^ (w[i - 2] >> 10));
                for (i = 0; i < 8; i++)
                        v[i] = st[i];
                for (i = 0; i < 64; i++) {
                        unsigned int t1 = v[7] + (hash_rol(v[4], 26) ^ hash_rol(v[4], 21) ^ hash_rol(v[4], 7)) +
                                          ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
                        unsigned int t2 = (hash_rol(v[0], 30) ^ hash_rol(v[0], 19) ^ hash_rol(v[0], 10)) +
                                          ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
                        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
                        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
                }
                for (i = 0; i < 8; i++)
                        st[i] += v[i];
        }
}

static const unsigned long hash_c_iv[8] = {
    0x6a09e667f3bcc908ul, 0xbb67ae8584caa73bul, 0x3c6ef372fe94f82bul, 0xa54ff53a5f1d36f1ul,
    0x510e527fade682d1ul, 0x9b05688c2b3e6c1ful, 0x1f83d9abfb41bd6bul, 0x5be0cd19137e2179ul,
};

static void hash_c_sha512(unsigned long *st, const unsigned char *d, unsigned long n)
{
        static const unsigned long k[80] = {
            0x428a2f98d728ae22ul, 0x7137449123ef65cdul, 0xb5c0fbcfec4d3b2ful, 0xe9b5dba58189dbbcul,
            0x3956c25bf348b538ul, 0x59f111f1b605d019ul, 0x923f82a4af194f9bul, 0xab1c5ed5da6d8118ul,
            0xd807aa98a3030242ul, 0x12835b0145706fbeul, 0x243185be4ee4b28cul, 0x550c7dc3d5ffb4e2ul,
            0x72be5d74f27b896ful, 0x80deb1fe3b1696b1ul, 0x9bdc06a725c71235ul, 0xc19bf174cf692694ul,
            0xe49b69c19ef14ad2ul, 0xefbe4786384f25e3ul, 0x0fc19dc68b8cd5b5ul, 0x240ca1cc77ac9c65ul,
            0x2de92c6f592b0275ul, 0x4a7484aa6ea6e483ul, 0x5cb0a9dcbd41fbd4ul, 0x76f988da831153b5ul,
            0x983e5152ee66dfabul, 0xa831c66d2db43210ul, 0xb00327c898fb213ful, 0xbf597fc7beef0ee4ul,
            0xc6e00bf33da88fc2ul, 0xd5a79147930aa725ul, 0x06ca6351e003826ful, 0x142929670a0e6e70ul,
            0x27b70a8546d22ffcul, 0x2e1b21385c26c926ul, 0x4d2c6dfc5ac42aedul, 0x53380d139d95b3dful,
            0x650a73548baf63deul, 0x766a0abb3c77b2a8ul, 0x81c2c92e47edaee6ul, 0x92722c851482353bul,
            0xa2bfe8a14cf10364ul, 0xa81a664bbc423001ul, 0xc24b8b70d0f89791ul, 0xc76c51a30654be30ul,
            0xd192e819d6ef5218ul, 0xd69906245565a910ul, 0xf40e35855771202aul, 0x106aa07032bbd1b8ul,
            0x19a4c116b8d2d0c8ul, 0x1e376c085141ab53ul, 0x2748774cdf8eeb99ul, 0x34b0bcb5e19b48a8ul,
            0x391c0cb3c5c95a63ul, 0x4ed8aa4ae3418acbul, 0x5b9cca4f7763e373ul, 0x682e6ff3d6b2b8a3ul,
            0x748f82ee5defb2fcul, 0x78a5636f43172f60ul, 0x84c87814a1f0ab72ul, 0x8cc702081a6439ecul,
            0x90befffa23631e28ul, 0xa4506cebde82bde9ul, 0xbef9a3f7b2c67915ul, 0xc67178f2e372532bul,
            0xca273eceea26619cul, 0xd186b8c721c0c207ul, 0xeada7dd6cde0eb1eul, 0xf57d4f7fee6ed178ul,
            0x06f067aa72176fbaul, 0x0a637dc5a2c898a6ul, 0x113f9804bef90daeul, 0x1b710b35131c471bul,
            0x28db77f523047d84ul, 0x32caab7b40c72493ul, 0x3c9ebe0a15c9bebcul, 0x431d67c49c100d4cul,
            0x4cc5d4becb3e42b6ul, 0x597f299cfc657e2aul, 0x5fcb6fab3ad6faecul, 0x6c44198c4a475817ul,
        };
        for (; n; n--, d += 128) {
                unsigned long w[80], v[8];
                int i;
                for (i = 0; i < 16; i++)
                        w[i] = (unsigned long)hash_be(d + 8 * i) << 32 | hash_be(d + 8 * i + 4);
                for (i = 16; i < 80; i++)
                        w[i] = w[i - 16] + w[i - 7] +
                               (hash_ror(w[i - 15], 1) ^ hash_ror(w[i - 15], 8) ^ (w[i - 15] >> 7)) +
                               (hash_ror(w[i - 2], 19) ^ hash_ror(w[i - 2], 61) ^ (w[i - 2] >> 6));
                for (i = 0; i < 8; i++)
                        v[i] = st[i];
                for (i = 0; i < 80; i++) {
                        unsigned long t1 = v[7] + (hash_ror(v[4], 14) ^ hash_ror(v[4], 18) ^ hash_ror(v[4], 41)) +
                                           ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
                        unsigned long t2 = (hash_ror(v[0], 28) ^ hash_ror(v[0], 34) ^ hash_ror(v[0], 39)) +
                                           ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
                        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
                        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
                }
                for (i = 0; i < 8; i++)
                        st[i] += v[i];
        }
}

static void hash_c_blake2b(unsigned long *st, const unsigned char *d, unsigned long n, unsigned long tail)
{
        static const unsigned char sigma[12][16] = {
            {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
            {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
            {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
            {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
            {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
            {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
        };
        static const unsigned char lane[8][4] = {{0, 4, 8, 12}, {1, 5, 9, 13}, {2, 6, 10, 14}, {3, 7, 11, 15},
                                                 {0, 5, 10, 15}, {1, 6, 11, 12}, {2, 7, 8, 13}, {3, 4, 9, 14}};
        for (; n; n--, d += 128) {
                unsigned long add = n == 1 ? tail : 128, m[16], v[16];
                int i, r, g;
                st[8] += add;
                if (st[8] < add)
                        st[9]++;
                for (i = 0; i < 16; i++)
                        m[i] = (unsigned long)hash_le(d + 8 * i) | (unsigned long)hash_le(d + 8 * i + 4) << 32;
                for (i = 0; i < 8; i++) {
                        v[i] = st[i];
                        v[i + 8] = hash_c_iv[i];
                }
                v[12] ^= st[8];
                v[13] ^= st[9];
                v[14] ^= n == 1 ? st[10] : 0;
                for (r = 0; r < 12; r++)
                        for (g = 0; g < 8; g++) {
                                const unsigned char *l = lane[g];
                                v[l[0]] += v[l[1]] + m[sigma[r][2 * g]];
                                v[l[3]] = hash_ror(v[l[3]] ^ v[l[0]], 32);
                                v[l[2]] += v[l[3]];
                                v[l[1]] = hash_ror(v[l[1]] ^ v[l[2]], 24);
                                v[l[0]] += v[l[1]] + m[sigma[r][2 * g + 1]];
                                v[l[3]] = hash_ror(v[l[3]] ^ v[l[0]], 16);
                                v[l[2]] += v[l[3]];
                                v[l[1]] = hash_ror(v[l[1]] ^ v[l[2]], 63);
                        }
                for (i = 0; i < 8; i++)
                        st[i] ^= v[i] ^ v[i + 8];
        }
}

#define HASH_WORKERS(core, call_ours, call_c, sum)                            \
        static void core##_w(unsigned long size, unsigned long rounds)        \
        {                                                                     \
                unsigned long i;                                              \
                for (i = 0; i < rounds; i++) {                                \
                        call_ours;                                            \
                        sink += sum;                                          \
                }                                                             \
                (void)size;                                                   \
        }                                                                     \
        static void core##_c_w(unsigned long size, unsigned long rounds)      \
        {                                                                     \
                unsigned long i;                                              \
                for (i = 0; i < rounds; i++) {                                \
                        call_c;                                               \
                        sink += sum;                                          \
                }                                                             \
                (void)size;                                                   \
        }

HASH_WORKERS(md5, md5_blocks(hash_narrow, hash_data, HASH_BYTES / 64),
             hash_c_md5(hash_narrow, hash_data, HASH_BYTES / 64), hash_narrow[0])
HASH_WORKERS(sha1, sha1_blocks(hash_narrow, hash_data, HASH_BYTES / 64),
             hash_c_sha1(hash_narrow, hash_data, HASH_BYTES / 64), hash_narrow[0])
HASH_WORKERS(sha256, sha256_blocks(hash_narrow, hash_data, HASH_BYTES / 64),
             hash_c_sha256(hash_narrow, hash_data, HASH_BYTES / 64), hash_narrow[0])
HASH_WORKERS(sha512, sha512_blocks(hash_wide, hash_data, HASH_BYTES / 128),
             hash_c_sha512(hash_wide, hash_data, HASH_BYTES / 128), hash_wide[0])
HASH_WORKERS(blake2b, blake2b_blocks(hash_wide, hash_data, HASH_BYTES / 128, 128),
             hash_c_blake2b(hash_wide, hash_data, HASH_BYTES / 128, 128), hash_wide[0])
#endif

#ifndef SKIP_FIELD
/*
        The P-256/P-384 field routines against the Montgomery arithmetic
        crypto.c keeps for the group orders and RSA, compiled from C: the
        schoolbook product, word-by-word reduction and a masked final
        subtraction. Operands are fixed values below p.
*/
typedef unsigned __int128 field_wide;

static const unsigned long field_p256[4] = {
    0xffffffffffffffffUL, 0x00000000ffffffffUL, 0, 0xffffffff00000001UL};
static const unsigned long field_p384[6] = {
    0x00000000ffffffffUL, 0xffffffff00000000UL, 0xfffffffffffffffeUL,
    0xffffffffffffffffUL, 0xffffffffffffffffUL, 0xffffffffffffffffUL};
static unsigned long field_a[6] = {
    0x243f6a8885a308d3UL, 0x13198a2e03707344UL, 0xa4093822299f31d0UL,
    0x082efa98ec4e6c89UL, 0x452821e638d01377UL, 0x0be5466cf34e90c6UL};
static unsigned long field_b[6] = {
    0xc0ac29b7c97c50ddUL, 0x3f84d5b5b5470917UL, 0x9216d5d98979fb1bUL,
    0x0801f2e2858efc16UL, 0x636920d871574e69UL, 0x0a458fea3f4933d7UL};
static unsigned long field_r[6];

static unsigned long field_c_sub(unsigned long *d, const unsigned long *a,
                                 const unsigned long *b, int n)
{
        field_wide borrow = 0;
        for (int i = 0; i < n; i++)
        {
                field_wide v = (field_wide)a[i] - b[i] - borrow;
                d[i] = (unsigned long)v;
                borrow = (v >> 64) & 1;
        }
        return (unsigned long)borrow;
}

static void field_c_pick(unsigned long *d, const unsigned long *a,
                         const unsigned long *b, int n, unsigned long take_b)
{
        unsigned long mask = 0 - take_b;
        for (int i = 0; i < n; i++)
                d[i] = (a[i] & ~mask) | (b[i] & mask);
}

__attribute__((noinline))
static void field_c_multiply(unsigned long *d, const unsigned long *a,
                             const unsigned long *b, const unsigned long *m,
                             unsigned long inverse, int n)
{
        unsigned long t[12] = {0}, reduced[6], top = 0;
        for (int i = 0; i < n; i++)
        {
                field_wide carry = 0;
                for (int j = 0; j < n; j++)
                {
                        carry += (field_wide)t[i + j] + (field_wide)a[i] * b[j];
                        t[i + j] = (unsigned long)carry;
                        carry >>= 64;
                }
                t[i + n] = (unsigned long)carry;
        }
        for (int i = 0; i < n; i++)
        {
                unsigned long q = t[i] * inverse;
                field_wide carry = 0;
                for (int j = 0; j < n; j++)
                {
                        carry += (field_wide)t[i + j] + (field_wide)q * m[j];
                        t[i + j] = (unsigned long)carry;
                        carry >>= 64;
                }
                carry += (field_wide)t[i + n] + top;
                t[i + n] = (unsigned long)carry;
                top = (unsigned long)(carry >> 64);
        }
        unsigned long borrow = field_c_sub(reduced, t + n, m, n);
        field_c_pick(d, t + n, reduced, n, top | (borrow ^ 1));
}

__attribute__((noinline))
static void field_c_add(unsigned long *d, const unsigned long *a,
                        const unsigned long *b, const unsigned long *m, int n)
{
        unsigned long sum[6], reduced[6];
        field_wide carry = 0;
        for (int i = 0; i < n; i++)
        {
                carry += (field_wide)a[i] + b[i];
                sum[i] = (unsigned long)carry;
                carry >>= 64;
        }
        unsigned long borrow = field_c_sub(reduced, sum, m, n);
        field_c_pick(d, sum, reduced, n, (unsigned long)carry | (borrow ^ 1));
}

__attribute__((noinline))
static void field_c_subtract(unsigned long *d, const unsigned long *a,
                             const unsigned long *b, const unsigned long *m,
                             int n)
{
        unsigned long difference[6], restored[6];
        unsigned long borrow = field_c_sub(difference, a, b, n);
        field_wide carry = 0;
        for (int i = 0; i < n; i++)
        {
                carry += (field_wide)difference[i] + m[i];
                restored[i] = (unsigned long)carry;
                carry >>= 64;
        }
        field_c_pick(d, difference, restored, n, borrow);
}

#define FIELD_ROW(name, call)                                                  \
        static void name(unsigned long size, unsigned long rounds)             \
        {                                                                      \
                (void)size;                                                    \
                for (unsigned long i = 0; i < rounds; i++)                     \
                {                                                              \
                        call;                                                  \
                        sink += field_r[0];                                    \
                }                                                              \
        }

FIELD_ROW(p256_mul_w, p256_multiply(field_r, field_a, field_b))
FIELD_ROW(p256_mul_c_w, field_c_multiply(field_r, field_a, field_b, field_p256, 1, 4))
FIELD_ROW(p256_sqr_w, p256_square(field_r, field_a))
FIELD_ROW(p256_sqr_c_w, field_c_multiply(field_r, field_a, field_a, field_p256, 1, 4))
FIELD_ROW(p256_add_w, p256_add(field_r, field_a, field_b))
FIELD_ROW(p256_add_c_w, field_c_add(field_r, field_a, field_b, field_p256, 4))
FIELD_ROW(p256_sub_w, p256_subtract(field_r, field_a, field_b))
FIELD_ROW(p256_sub_c_w, field_c_subtract(field_r, field_a, field_b, field_p256, 4))
FIELD_ROW(p384_mul_w, p384_multiply(field_r, field_a, field_b))
FIELD_ROW(p384_mul_c_w, field_c_multiply(field_r, field_a, field_b, field_p384, 0x100000001UL, 6))
FIELD_ROW(p384_sqr_w, p384_square(field_r, field_a))
FIELD_ROW(p384_sqr_c_w, field_c_multiply(field_r, field_a, field_a, field_p384, 0x100000001UL, 6))
FIELD_ROW(p384_add_w, p384_add(field_r, field_a, field_b))
FIELD_ROW(p384_add_c_w, field_c_add(field_r, field_a, field_b, field_p384, 6))
FIELD_ROW(p384_sub_w, p384_subtract(field_r, field_a, field_b))
FIELD_ROW(p384_sub_c_w, field_c_subtract(field_r, field_a, field_b, field_p384, 6))
#endif

#ifndef SKIP_GHASH
/*
        GHASH against the integer carry-less multiply written in C: the
        floor column is the compiler's arrangement of the baseline algorithm,
        the thing every body has to beat, not a traffic bound. ghash_blocks
        takes its widest body here (VPCLMULQDQ zmm on a 9950X, PCLMULQDQ,
        PMULL or Zbc where present). The ISA references for a 16 KiB record:
        OpenSSL GMAC 37.0 GB/s on the 9950X and 7.6 GB/s on an M2 Pro.
*/
static const unsigned char ghash_key_bytes[16] = {
    0x66, 0xe9, 0x4b, 0xd4, 0xef, 0x8a, 0x2c, 0x3b,
    0x88, 0x4c, 0xfa, 0x59, 0xca, 0x34, 0x2b, 0x2e};
static unsigned char ghash_state[16];
static unsigned char ghash_table[1552] __attribute__((aligned(64)));

static uint64_t ghash_c_load(const unsigned char *at)
{
        uint64_t value = 0;
        for (int i = 0; i < 8; i++)
                value = value << 8 | at[i];
        return value;
}

static void ghash_c_store(unsigned char *at, uint64_t value)
{
        for (int i = 7; i >= 0; i--)
        {
                at[i] = (unsigned char)value;
                value >>= 8;
        }
}

static uint64_t ghash_c_reverse(uint64_t x)
{
        x = ((x >> 1) & 0x5555555555555555ull) | ((x & 0x5555555555555555ull) << 1);
        x = ((x >> 2) & 0x3333333333333333ull) | ((x & 0x3333333333333333ull) << 2);
        x = ((x >> 4) & 0x0f0f0f0f0f0f0f0full) | ((x & 0x0f0f0f0f0f0f0f0full) << 4);
        return __builtin_bswap64(x);
}

static uint64_t ghash_c_low(uint64_t x, uint64_t y)
{
        uint64_t x0 = x & 0x1111111111111111ull, x1 = x & 0x2222222222222222ull;
        uint64_t x2 = x & 0x4444444444444444ull, x3 = x & 0x8888888888888888ull;
        uint64_t y0 = y & 0x1111111111111111ull, y1 = y & 0x2222222222222222ull;
        uint64_t y2 = y & 0x4444444444444444ull, y3 = y & 0x8888888888888888ull;
        uint64_t z0 = (x0 * y0) ^ (x1 * y3) ^ (x2 * y2) ^ (x3 * y1);
        uint64_t z1 = (x0 * y1) ^ (x1 * y0) ^ (x2 * y3) ^ (x3 * y2);
        uint64_t z2 = (x0 * y2) ^ (x1 * y1) ^ (x2 * y0) ^ (x3 * y3);
        uint64_t z3 = (x0 * y3) ^ (x1 * y2) ^ (x2 * y1) ^ (x3 * y0);

        return (z0 & 0x1111111111111111ull) | (z1 & 0x2222222222222222ull) |
               (z2 & 0x4444444444444444ull) | (z3 & 0x8888888888888888ull);
}

static void ghash_c_blocks(unsigned char *state, const unsigned char *key,
                           const unsigned char *data, unsigned long blocks)
{
        uint64_t h1 = ghash_c_load(key), h0 = ghash_c_load(key + 8);
        uint64_t h1r = ghash_c_reverse(h1), h0r = ghash_c_reverse(h0);
        uint64_t hm = h0 ^ h1, hmr = h0r ^ h1r;
        uint64_t s1 = ghash_c_load(state), s0 = ghash_c_load(state + 8);

        while (blocks--)
        {
                uint64_t x1 = s1 ^ ghash_c_load(data);
                uint64_t x0 = s0 ^ ghash_c_load(data + 8);
                uint64_t lo0 = ghash_c_low(x0, h0), lo1 = ghash_c_low(x1, h1);
                uint64_t lom = ghash_c_low(x0 ^ x1, hm);
                uint64_t hi0 = ghash_c_reverse(ghash_c_low(ghash_c_reverse(x0), h0r));
                uint64_t hi1 = ghash_c_reverse(ghash_c_low(ghash_c_reverse(x1), h1r));
                uint64_t him = ghash_c_reverse(
                    ghash_c_low(ghash_c_reverse(x0 ^ x1), hmr));
                uint64_t w0 = lo0 << 1;
                uint64_t w1 = hi0 ^ ((lo0 ^ lo1 ^ lom) << 1);
                uint64_t w2 = (hi0 ^ hi1 ^ him) ^ (lo1 << 1);
                uint64_t w3 = hi1;

                w2 ^= w0 ^ (w0 >> 1) ^ (w0 >> 2) ^ (w0 >> 7);
                w1 ^= (w0 << 63) ^ (w0 << 62) ^ (w0 << 57);
                w3 ^= w1 ^ (w1 >> 1) ^ (w1 >> 2) ^ (w1 >> 7);
                w2 ^= (w1 << 63) ^ (w1 << 62) ^ (w1 << 57);
                s1 = w3;
                s0 = w2;
                data += 16;
        }

        ghash_c_store(state, s1);
        ghash_c_store(state + 8, s0);
}

static void ghash_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        if (!ghash_table[1536] && !ghash_table[1537])
                ghash_key(ghash_table, ghash_key_bytes);
        for (i = 0; i < rounds; i++) {
                ghash_blocks(ghash_state, ghash_table, src, size / 16);
                sink += ghash_state[0];
        }
}

static void ghash_c_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++) {
                ghash_c_blocks(ghash_state, ghash_key_bytes, src, size / 16);
                sink += ghash_state[0];
        }
}
#endif

#ifndef SKIP_AES
/*
        AES-128 counter mode against FIPS-197 written with an S-box table in
        C: the floor column is a table AES, which indexes on the key and the
        data and is faster than any constant-time software AES, so a ratio
        under one is the hardware body (VAES, AES-NI, the AES extension or
        Zvkned) and over one is the bitsliced floor paying for constant time.
        ISA references for a 16 KiB record on the 9950X: OpenSSL AES-128-CTR
        15.5 GB/s (AES-NI) and AES-128-GCM 27.2 GB/s.
*/
static unsigned char aes_round[176];
static unsigned char aes_counter[16];
static unsigned char aes_sbox[256];

static unsigned char aes_c_xtime(unsigned char v)
{
        return (unsigned char)((v << 1) ^ (v & 0x80 ? 0x1b : 0));
}

static void aes_c_setup(void)
{
        static const unsigned char rcon[10] = {1, 2, 4, 8, 16, 32, 64, 128, 0x1b, 0x36};
        unsigned char p = 1, q = 1;
        if (aes_sbox[0])
                return;
        do {
                p = p ^ (unsigned char)(p << 1) ^ (p & 0x80 ? 0x1b : 0);
                q ^= q << 1; q ^= q << 2; q ^= q << 4;
                if (q & 0x80) q ^= 0x09;
                aes_sbox[p] = q ^ (unsigned char)(q << 1 | q >> 7) ^ (unsigned char)(q << 2 | q >> 6) ^
                              (unsigned char)(q << 3 | q >> 5) ^ (unsigned char)(q << 4 | q >> 4) ^ 0x63;
        } while (p != 1);
        aes_sbox[0] = 0x63;
        for (int i = 0; i < 16; i++)
                aes_round[i] = (unsigned char)(i * 29 + 7);
        for (int i = 16; i < 176; i += 4) {
                unsigned char t[4] = {aes_round[i - 4], aes_round[i - 3], aes_round[i - 2], aes_round[i - 1]};
                if (i % 16 == 0) {
                        unsigned char k = t[0];
                        t[0] = aes_sbox[t[1]] ^ rcon[i / 16 - 1];
                        t[1] = aes_sbox[t[2]]; t[2] = aes_sbox[t[3]]; t[3] = aes_sbox[k];
                }
                for (int j = 0; j < 4; j++)
                        aes_round[i + j] = aes_round[i - 16 + j] ^ t[j];
        }
}

static void aes_c_blocks(const unsigned char *round, unsigned char *counter,
                         const unsigned char *in, unsigned char *out, unsigned long blocks)
{
        static const unsigned char shift[16] = {0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11};
        for (; blocks; blocks--, in += 16, out += 16) {
                unsigned char s[16], n[16];
                for (int i = 0; i < 16; i++) s[i] = counter[i] ^ round[i];
                for (int r = 1; r <= 10; r++) {
                        for (int i = 0; i < 16; i++) n[i] = aes_sbox[s[shift[i]]];
                        if (r < 10)
                                for (int i = 0; i < 16; i += 4) {
                                        unsigned char a = n[i], b = n[i + 1], c = n[i + 2], d = n[i + 3];
                                        n[i] = aes_c_xtime(a) ^ aes_c_xtime(b) ^ b ^ c ^ d;
                                        n[i + 1] = a ^ aes_c_xtime(b) ^ aes_c_xtime(c) ^ c ^ d;
                                        n[i + 2] = a ^ b ^ aes_c_xtime(c) ^ aes_c_xtime(d) ^ d;
                                        n[i + 3] = aes_c_xtime(a) ^ a ^ b ^ c ^ aes_c_xtime(d);
                                }
                        for (int i = 0; i < 16; i++) s[i] = n[i] ^ round[16 * r + i];
                }
                for (int i = 0; i < 16; i++) out[i] = in[i] ^ s[i];
                for (int i = 15; i >= 12; i--)
                        if (++counter[i]) break;
        }
}

static void aes_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        aes_c_setup();
        for (i = 0; i < rounds; i++) {
                aes128_ctr_blocks(aes_round, aes_counter, src, dst, size / 16);
                sink += dst[0];
        }
}

static void aes_c_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        aes_c_setup();
        for (i = 0; i < rounds; i++) {
                aes_c_blocks(aes_round, aes_counter, src, dst, size / 16);
                sink += dst[0];
        }
}
#endif

static void span_byte_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += memory_span_byte(src, 'a', size);
}

static void utf8_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += memory_utf8_span(src, size, size).x;
}

static void strcmp_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        (void)size;
        for (i = 0; i < rounds; i++)
                sink += (unsigned)string_compare((char *)src, (char *)src2);
}

static void strncmp_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned)string_compare_max((char *)src, (char *)src2,
                                                    size);
}

static void strchr_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        (void)size;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)string_first_of((char *)src, 'Z');
}

static void strrchr_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        (void)size;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)string_last_of_or_end((char *)src, 'Z');
}

static void strcpy_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        (void)size;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)string_copy((char *)dst, (char *)src);
}

static void strncpy_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)string_copy_max((char *)dst, (char *)src,
                                                      size);
}

static void strstr_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        (void)size;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)string_search((char *)src, (char *)src2);
}

static void find_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        (void)size;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)string_find((char *)src, (char *)src2);
}

static void memmem_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_search(src, size, src2, 3);
}

static void copy_end_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_copy_end(dst, src, size);
}

static void copy_apart_end_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        for (i = 0; i < rounds; i++)
                sink += (unsigned long)memory_copy_apart_end(dst, src, size);
}

#ifndef SKIP_ITOA
static void into_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        (void)size;
        for (i = 0; i < rounds; i++)
                sink += positive_into((char *)dst, 1234567890123ull);
}
#endif

static void parse_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        (void)size;
        for (i = 0; i < rounds; i++)
                sink += string_to_positive((char *)src);
}

static void fill_az(unsigned long n)
{
        unsigned long i;
        for (i = 0; i < n; i++)
                src[i] = (unsigned char)('a' + (i % 26));
        src[n] = 0;
        memcpy(src2, src, n + 1);
        memcpy(dst, src, n + 1);
}

int main(void)
{
        static const unsigned long sizes[] = {8, 16, 32, 64, 256, 4096, 65536};
        unsigned long s, n, guess;
        unsigned i;

        for (i = 0; i < 256; i++)
                table[i] = (unsigned char)i;
        for (i = 0; i < 8; i++)
                sha_state[i] = 0x6a09e667u + i * 0x11111111u;
        memset(sha_block, 0x5a, sizeof sha_block);

        printf("arch,name,shape,class,bytes,ours_ns,floor_ns,ratio\n");

        for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
                n = sizes[s];
                guess = n < 64 ? 8 : n < 4096 ? 20 : n / 8;

                memset(src, 0x5a, n);
                memcpy(src2, src, n);
                memcpy(dst, src, n);

                row("memory_copy_apart", "copy", "traffic_copy", n, memcpy_w,
                    floor_copy_w, guess);
                row("memory_copy", "apart", "traffic_copy", n, memmove_w,
                    floor_copy_w, guess);
                row("memory_fill", "fill", "traffic_fill", n, memset_w,
                    floor_fill_w, guess);
                if (n >= 4)
                        row("memory_fill_32", "fill", "traffic_fill", n, fill32_w,
                            floor_fill_w, guess);
                if (n >= 8)
                        row("memory_fill_64", "fill", "traffic_fill", n, fill64_w,
                            floor_fill_w, guess);
                memcpy(dst, src, n);
                row("memory_reverse", "inplace", "traffic_copy", n, reverse_w,
                    floor_copy_w, guess);
                memcpy(dst, src, n);
                row("memory_frob", "inplace", "traffic_copy", n, frob_w,
                    floor_copy_w, guess);
                memcpy(dst, src, n);
                row("memory_to_lower_ascii", "inplace", "traffic_copy", n,
                    lower_w, floor_copy_w, guess);
                row("memory_to_upper_ascii", "inplace", "traffic_copy", n,
                    upper_w, floor_copy_w, guess);
                row("memory_exchange_apart", "swap", "traffic_copy", n,
                    exchange_w, floor_copy_w, guess);
                memcpy(dst, src, n);
                row("memory_translate", "table", "traffic_copy", n, translate_w,
                    floor_copy_w, guess);
#ifndef SKIP_HEX
                row("memory_into_hex", "encode", "compute", n, hex_w,
                    floor_copy_w, guess);
#endif
                row("memory_copy_end", "copy", "traffic_copy", n, copy_end_w,
                    floor_copy_w, guess);
                row("memory_copy_apart_end", "copy", "traffic_copy", n,
                    copy_apart_end_w, floor_copy_w, guess);

                fill_az(n);
                row("string_length", "len", "traffic_one", n, strlen_w,
                    floor_one_w, guess);
                row("string_length_max", "len", "traffic_one", n, strnlen_w,
                    floor_one_w, guess);
                row("string_compare", "equal", "traffic_two", n, strcmp_w,
                    floor_two_w, guess);
                row("string_compare_max", "equal", "traffic_two", n, strncmp_w,
                    floor_two_w, guess);
                row("string_copy", "copy", "traffic_copy", n, strcpy_w,
                    floor_copy_w, guess);
                row("string_copy_max", "copy", "traffic_copy", n, strncpy_w,
                    floor_copy_w, guess);

                memcpy(src2, src, n + 1);
                row("memory_compare", "equal", "traffic_two", n, memcmp_w,
                    floor_two_w, guess);
                src2[0] ^= 0xff;
                row("memory_compare", "miss@0", "traffic_two", n, memcmp_w,
                    floor_two_w, 8);
                memcpy(src2, src, n + 1);
                src2[n - 1] ^= 0xff;
                row("memory_compare", "miss@end", "traffic_two", n, memcmp_w,
                    floor_two_w, guess);
                memcpy(src2, src, n + 1);
                row("memory_common_prefix", "equal", "traffic_two", n, prefix_w,
                    floor_two_w, guess);
                row("memory_compare_ascii_case", "equal", "traffic_two", n,
                    memcmp_case_w, floor_two_w, guess);

                memcpy(src2, src, n + 1);
                src2[0] ^= 1;
                row("string_compare", "miss@0", "traffic_two", n, strcmp_w,
                    floor_two_w, 8);
                memcpy(src2, src, n + 1);
                src2[n - 1] ^= 1;
                row("string_compare", "miss@end", "traffic_two", n, strcmp_w,
                    floor_two_w, guess);

                memset(src, 'a', n);
                src[0] = 'Z';
                src[n] = 0;
                row("memory_first_of", "hit@0", "traffic_one", n, memchr_w,
                    floor_one_w, 8);
                src[0] = 'a';
                src[n - 1] = 'Z';
                row("memory_first_of", "hit@end", "traffic_one", n, memchr_w,
                    floor_one_w, guess);
                row("memory_last_of", "hit@end", "traffic_one", n, memrchr_w,
                    floor_one_w, guess);
                src[n - 1] = 'a';
                row("memory_first_of", "miss", "traffic_one", n, memchr_w,
                    floor_one_w, guess);
                row("memory_count", "count", "traffic_one", n, count_w,
                    floor_one_w, guess);
                row("memory_sum_bytes", "sum", "compute", n, sum_w, floor_one_w,
                    guess);
                row("memory_hash_33", "hash", "compute", n, hash33_w,
                    floor_one_w, guess);
                row("hash_xxh64", "hash", "compute", n, xxh_w, floor_one_w,
                    guess);
                row("memory_checksum_bsd16", "sum", "compute", n, bsd16_w,
                    floor_one_w, guess);
                row("memory_span_byte", "span", "traffic_one", n, span_byte_w,
                    floor_one_w, guess);
                row("memory_utf8_span", "span", "traffic_one", n, utf8_w,
                    floor_one_w, guess);

                fill_az(n);
                src[0] = 'Z';
                row("string_first_of", "hit@0", "traffic_one", n, strchr_w,
                    floor_one_w, 8);
                src[0] = 'a';
                src[n - 1] = 'Z';
                src[n] = 0;
                row("string_last_of_or_end", "hit@end", "traffic_one", n,
                    strrchr_w, floor_one_w, guess);

                fill_az(n);
                src2[0] = 'z';
                src2[1] = 'z';
                src2[2] = 0;
                row("string_search", "miss", "traffic_one", n, strstr_w,
                    floor_one_w, guess);
                row("string_find", "miss", "traffic_one", n, find_w, floor_one_w,
                    guess);
                memcpy(src2, src + (n > 8 ? 7 : 0), 3);
                src2[3] = 0;
                row("memory_search", "hit", "traffic_one", n, memmem_w,
                    floor_one_w, guess);

#ifndef SKIP_ITOA
                row("positive_into", "itoa", "compute", n, into_w, floor_one_w,
                    8);
#endif
                memcpy(src, "1234567890123", 14);
                row("string_to_positive", "atoi", "compute", n, parse_w,
                    floor_one_w, 8);
        }

#ifndef SKIP_SHA256
        row("sha256_compress", "block", "compute", 64, sha_w, floor_one_w, 20);
        for (i = 0; i < HASH_BYTES; i++)
                hash_data[i] = (unsigned char)(i * 131 ^ i >> 7);
        row("md5_blocks", "64KiB", "compute", HASH_BYTES, md5_w, md5_c_w, 60000000);
        row("sha1_blocks", "64KiB", "compute", HASH_BYTES, sha1_w, sha1_c_w, 60000000);
        row("sha256_blocks", "64KiB", "compute", HASH_BYTES, sha256_w, sha256_c_w, 60000000);
        row("sha512_blocks", "64KiB", "compute", HASH_BYTES, sha512_w, sha512_c_w, 60000000);
        row("blake2b_blocks", "64KiB", "compute", HASH_BYTES, blake2b_w, blake2b_c_w, 60000000);
#endif
#if defined(USE_OURS) && !defined(SKIP_LOCK)
        /* Alone: the threads_live elision against the two stores it makes.
           Company: the atomic path against the inline C pair it replaced. */
        row("lock_take", "pair_alone", "latency", 0, lock_pair_w, lock_store_w, 2);
        threads_live = 1;
        row("lock_take", "pair_company", "latency", 0, lock_pair_w, lock_atomic_w, 8);
        threads_live = 0;
#endif
#ifndef SKIP_FIELD
        row("p256_multiply", "field", "compute", 32, p256_mul_w, p256_mul_c_w, 8);
        row("p256_square", "field", "compute", 32, p256_sqr_w, p256_sqr_c_w, 6);
        row("p256_add", "field", "compute", 32, p256_add_w, p256_add_c_w, 2);
        row("p256_subtract", "field", "compute", 32, p256_sub_w, p256_sub_c_w, 2);
        row("p384_multiply", "field", "compute", 48, p384_mul_w, p384_mul_c_w, 20);
        row("p384_square", "field", "compute", 48, p384_sqr_w, p384_sqr_c_w, 15);
        row("p384_add", "field", "compute", 48, p384_add_w, p384_add_c_w, 2);
        row("p384_subtract", "field", "compute", 48, p384_sub_w, p384_sub_c_w, 2);
#endif
#ifndef SKIP_GHASH
        row("ghash_blocks", "record", "compute", 16384, ghash_w, ghash_c_w,
            60000);
#endif
#ifndef SKIP_AES
        row("aes128_ctr_blocks", "record", "compute", 16384, aes_w, aes_c_w,
            20000);
#endif
        return 0;
}
