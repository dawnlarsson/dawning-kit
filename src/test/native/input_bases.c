/* Exact lifted bounded input-base leaves on native Apple ARM silicon. */
#include "input_bases.h"

typedef unsigned char u8;
typedef unsigned long u64;

u64 string_digits_max(const u8 *, u64, u64 *);
u64 string_digits_octal_max(const u8 *, u64, u64 *);
u64 string_digits_octal_escape_max(const u8 *, u64, u64 *);
u64 string_digits_hexadecimal_max(const u8 *, u64, u64 *);
u64 string_digits_hexadecimal_escape_max(const u8 *, u64, u64 *);
u64 string_digits_base_max(const u8 *, u64, u64, u64 *);
int printf(const char *, ...);

#define SUBJECTS 64
#define ROUNDS (1ul << 20)
#define TRIES 11

typedef u64 (*fixed_parser)(const u8 *, u64, u64 *);
typedef u64 (*base_parser)(const u8 *, u64, u64, u64 *);

static u8 subjects[SUBJECTS][96];
static u64 limits[SUBJECTS];
static volatile u64 sink;

static __attribute__((noinline, noclone)) u64 scalar_octal(const u8 *source, u64 bound,
                                                   u64 *used)
{
        u64 value = 0, at = 0;
        while (at < bound && source[at] >= '0' && source[at] <= '7') {
                value = (value << 3) + source[at] - '0';
                at++;
        }
        if (used) *used = at;
        return value;
}

static __attribute__((noinline, noclone)) u64 scalar_hex(const u8 *source, u64 bound,
                                                 u64 *used)
{
        u64 value = 0, at = 0;
        while (at < bound) {
                u8 c = source[at];
                u64 digit;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                else break;
                value = (value << 4) + digit;
                at++;
        }
        if (used) *used = at;
        return value;
}

static __attribute__((noinline, noclone)) u64 scalar_base(const u8 *source, u64 bound,
                                                  u64 base, u64 *used)
{
        u64 value = 0, at = 0;
        while (at < bound) {
                u8 c = source[at];
                u64 digit;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
                else break;
                if (digit >= base) break;
                value = value * base + digit;
                at++;
        }
        if (used) *used = at;
        return value;
}

static void make_subjects(u64 base, u64 maximum)
{
        for (u64 which = 0; which < SUBJECTS; which++) {
                u64 length = 1 + which % maximum;
                for (u64 at = 0; at < length; at++) {
                        u64 digit = (which * 13 + at * 17) % base;
                        subjects[which][at] = (u8)(digit < 10 ? '0' + digit
                            : ((at & 1) ? 'A' : 'a') + digit - 10);
                }
                subjects[which][length] = '@';
                limits[which] = length + 1;
        }
}

static u64 ticks(void)
{
        u64 value;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
        return value;
}

static __attribute__((noinline, noclone)) u64 run_fixed(fixed_parser parser)
{
        u64 start = ticks();
        for (u64 round = 0; round < ROUNDS; round++) {
                u64 which = round & (SUBJECTS - 1), used;
                u64 value = parser(subjects[which], limits[which], &used);
                sink += value + used;
        }
        return ticks() - start;
}

static __attribute__((noinline, noclone)) u64 run_base(base_parser parser, u64 base)
{
        u64 start = ticks();
        for (u64 round = 0; round < ROUNDS; round++) {
                u64 which = round & (SUBJECTS - 1), used;
                u64 value = parser(subjects[which], limits[which], base, &used);
                sink += value + used;
        }
        return ticks() - start;
}

static u64 median(u64 values[TRIES])
{
        for (u64 at = 1; at < TRIES; at++) {
                u64 value = values[at], before = at;
                while (before && values[before - 1] > value) {
                        values[before] = values[before - 1];
                        before--;
                }
                values[before] = value;
        }
        return values[TRIES / 2];
}

static void fixed_row(const char *name, fixed_parser scalar,
                      fixed_parser assembly, u64 base, u64 maximum)
{
        u64 old[TRIES], floor[TRIES];
        make_subjects(base, maximum);
        for (u64 trial = 0; trial < TRIES; trial++) {
                if (trial & 1) {
                        floor[trial] = run_fixed(assembly);
                        old[trial] = run_fixed(scalar);
                } else {
                        old[trial] = run_fixed(scalar);
                        floor[trial] = run_fixed(assembly);
                }
        }
        u64 scalar_ticks = median(old), assembly_ticks = median(floor);
        printf("  %-18s scalar %lu  assembly %lu  assembly/scalar %lu%%\n",
               name, scalar_ticks, assembly_ticks,
               assembly_ticks * 100 / scalar_ticks);
}

static void base_row(void)
{
        u64 old[TRIES], floor[TRIES];
        make_subjects(36, 13);
        for (u64 trial = 0; trial < TRIES; trial++) {
                if (trial & 1) {
                        floor[trial] = run_base(string_digits_base_max, 36);
                        old[trial] = run_base(scalar_base, 36);
                } else {
                        old[trial] = run_base(scalar_base, 36);
                        floor[trial] = run_base(string_digits_base_max, 36);
                }
        }
        u64 scalar_ticks = median(old), assembly_ticks = median(floor);
        printf("  %-18s scalar %lu  assembly %lu  assembly/scalar %lu%%\n",
               "base 36, 1-13", scalar_ticks, assembly_ticks,
               assembly_ticks * 100 / scalar_ticks);
}

int main(void)
{
        u64 used = 99;
        int bad = 0;
        bad += string_digits_octal_max((const u8 *)"777@", 4, &used) != 511 || used != 3;
        bad += string_digits_octal_escape_max((const u8 *)"777@", 4,
                                               &used) != 511 || used != 3;
        bad += string_digits_hexadecimal_max((const u8 *)"aF@", 3, &used) != 175 || used != 2;
        bad += string_digits_hexadecimal_escape_max((const u8 *)"aF@", 3,
                                                     &used) != 175 || used != 2;
        bad += string_digits_base_max((const u8 *)"Z@", 2, 36, &used) != 35 || used != 1;
        bad += string_digits_octal_max(0, 0, 0) != 0;

        printf("arm64 native bounded input bases: %s\n", bad ? "FAILED" : "correct");
        fixed_row("octal escape, 1-3", scalar_octal,
                  string_digits_octal_escape_max, 8, 3);
        fixed_row("octal word, 1-22", scalar_octal,
                  string_digits_octal_max, 8, 22);
        fixed_row("hex escape, 1-2", scalar_hex,
                  string_digits_hexadecimal_escape_max, 16, 2);
        fixed_row("hex word, 1-16", scalar_hex,
                  string_digits_hexadecimal_max, 16, 16);
        base_row();
        return bad;
}
