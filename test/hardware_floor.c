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
                cc -O2 -fno-builtin -DUSE_LIFTED -DSKIP_SHA256 -DSKIP_HEX \
                    -DSKIP_ITOA -I/tmp test/hardware_floor.c -o /tmp/hwfloor

            Darwin names (sha256/hex/itoa skipped: Mach-O :lo12: tables):
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
static void sha_w(unsigned long size, unsigned long rounds)
{
        unsigned long i;
        (void)size;
        for (i = 0; i < rounds; i++) {
                sha256_compress(sha_state, sha_block);
                sink += sha_state[0];
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
#endif
        return 0;
}
