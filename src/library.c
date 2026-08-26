/*
        Experimental C standard library
        intended for tiny C programs that run in the distro
        without any runtime requirements (no linking!)

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev

        use the accompanying build shell script to compile
        $ sh build <source_file.c> <output_file_name>
*/

#ifndef STANDARD_MODERN_C
#define STANDARD_MODERN_C

#if defined(__clang__)

#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wundef"
#pragma clang diagnostic ignored "-Wstrict-prototypes"

#elif defined(__GNUC__) || defined(__GNUG__)

#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#endif

#if defined(__linux__) || defined(__unix__)
#define LINUX 1
#define UNIX 1
#elif defined(__APPLE__)
#define APPLE 1
#include <TargetConditionals.h>
#if TARGET_OS_MAC
#define MACOS 1
#elif TARGET_OS_IPHONE
#define IOS 1
#endif
#elif defined(_WIN32)
#define WINDOWS 1
#elif defined(__IOS__)
#define IOS 1
#elif defined(__ANDROID__)
#define ANDROID 1
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define X64 1
#define BITS 64
#elif defined(__i386) || defined(_M_IX86)
#define X86 1
#define BITS 32
#elif defined(__aarch64__) || defined(_M_ARM64)
#define ARM64 1
#define BITS 64
#elif defined(__arm__) || defined(_M_ARM)
#define ARM32 1
#define BITS 32
#elif defined(__riscv)
#if __riscv_xlen == 64
#define RISCV64 1
#define BITS 64
#elif __riscv_xlen == 32
#define RISCV32 1
#define BITS 32
#endif
#elif defined(__PPC64__)
#define PPC64 1
#define BITS 64
#elif defined(__s390x__)
#define S390X 1
#define BITS 64
#endif

#if defined(__SSE__) || defined(__ARM_NEON)
#define SIMD 1
#endif

#if defined(__MODULE__) || defined(STANDARD_MODERN_C_KERNEL)
#define KERNEL_MODE 1
#endif

#define FLAT __attribute__((flatten))
#define PURE __attribute__((pure))
#define INLINE __attribute__((always_inline))
#define NO_FRAME __attribute__((noframe))
#define KEEP __attribute__((used))
#define DEAD_END __attribute__((noreturn))
#define WEAK __attribute__((weak))

#define pub extern __attribute__((visibility("default"))) KEEP

#define address_to *
#define address_of &
#define address_any void *
#define address_bad ((address_any) - 1)

#undef null
#define null ((address_any)0)
#define null_ADDRESS null
#define is_null(address) ((address) == null)

// null terminator
#define end '\0'

typedef __builtin_va_list var_args;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
// ### Initializes a variable argument list.
#define var_list(list, last_param) __builtin_va_start(list, 0)
#else
// ### Initializes a variable argument list.
// list:        variable argument list to initialize
// last_param:  last named parameter before variable arguments
#define var_list(list, last_param) __builtin_va_start(list, last_param)
#endif

// ### Cleans up a variable argument list.
// list:        variable argument list to clean up
#define var_list_end(list) __builtin_va_end(list)

// Retrieves the next argument from a variable argument list.
// list:        variable argument list
// type:        type of the argument to retrieve
#define var_list_get(list, returned_type) __builtin_va_arg(list, returned_type)

// ### Creates a copy of a variable argument list.
// from:        source variable argument list to copy
// destination: variable argument list
#define var_list_copy(from, destination) __builtin_va_copy(destination, from)

// ### Convenience macro to process all variable arguments of a specific type.
// list:        variable argument list
// count:       number of arguments to process
// type:        type of arguments
// action:      Code block with '_arg' representing each argument
#define var_list_iter(list, count, type, action)              \
        do                                                    \
        {                                                     \
                for (int _i = 0; _i < (count); _i++)          \
                {                                             \
                        type _arg = var_list_get(list, type); \
                        action;                               \
                }                                             \
        } while (0)

#define bit_test(bit, address) (address_to(address) & (1u << (bit)))
#define bit_set(bit, address) (address_to(address) |= (1u << (bit)))
#define bit_clear(bit, address) (address_to(address) &= ~(1u << (bit)))
#define bit_flip(bit, address) (address_to(address) ^= (1u << (bit)))
#define bit_mask(bit) (1u << (bit))

#define struct_from_field(field_address, struct_type, field_name) \
        container_of(field_address, struct_type, field_name)

#define memory_barrier() __asm__ __volatile__("" ::: "memory")
#define memory_read_barrier() __sync_synchronize()
#define memory_write_barrier() __sync_synchronize()
#define memory_full_barrier() __sync_synchronize()

#define inline_if_small(max_size) __attribute__((always_inline)) \
__attribute__((optimize("inline-max-size=" #max_size)))

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define if_common(cond) if (likely(cond))
#define if_rare(cond) if (unlikely(cond))

#define prefetch_read(addr) __builtin_prefetch((addr), 0, 3)
#define prefetch_write(addr) __builtin_prefetch((addr), 1, 3)

#define local_var(type, name) \
        __attribute__((section(".percpu"))) __typeof__(type) name

#define atomic_add(address, val) __sync_fetch_and_add(address, val)
#define atomic_sub(address, val) __sync_fetch_and_sub(address, val)
#define atomic_inc(address) atomic_add(address, 1)
#define atomic_dec(address) atomic_sub(address, 1)
#define atomic_exchange(address, val) __sync_lock_test_and_set(address, val)
#define atomic_compare_exchange(address, expected, desired) \
        __sync_bool_compare_and_swap(address, expected, desired)

#ifdef LIBRARY_API

#define api_function(name, returned_type, default, args...) \
        WEAK pub returned_type name(args) { return default; }

#define api_type(name, type, default) \
        WEAK pub type name = default;

#else

#define api_function(name, returned_type, default, args...) \
        pub returned_type name(args)

#define api_type(name, type, default) \
        pub type name = default;

#endif // LIBRARY_API

#define ANSI "\x1b["

#define TERM_CLEAR_SCREEN ANSI "2J" ANSI "H"
#define TERM_HIDE_CURSOR ANSI "?25l"
#define TERM_SHOW_CURSOR ANSI "?25h"

#define TERM_RESET ANSI "0m"
#define TERM_BOLD ANSI "1m"
#define TERM_DIM ANSI "2m"
#define TERM_UNDERLINED ANSI "4m"
#define TERM_BLINK ANSI "5m"
#define TERM_REVERSE ANSI "7m"
#define TERM_HIDDEN ANSI "8m"
#define TERM_INVERT ANSI "7m"

#define TERM_BLACK_DARK ANSI "30m"
#define TERM_RED_DARK ANSI "31m"
#define TERM_GREEN_DARK ANSI "32m"
#define TERM_YELLOW_DARK ANSI "33m"
#define TERM_BLUE_DARK ANSI "34m"
#define TERM_MAGENTA_DARK ANSI "35m"
#define TERM_CYAN_DARK ANSI "36m"
#define TERM_WHITE_DARK ANSI "37m"

#define TERM_GREY ANSI "90m"
#define TERM_RED ANSI "91m"
#define TERM_GREEN ANSI "92m"
#define TERM_YELLOW ANSI "93m"
#define TERM_BLUE ANSI "94m"
#define TERM_MAGENTA ANSI "95m"
#define TERM_CYAN ANSI "96m"
#define TERM_WHITE ANSI "97m"

#define TERM_BG_BLACK ANSI "40m"
#define TERM_BG_RED ANSI "41m"
#define TERM_BG_GREEN ANSI "42m"
#define TERM_BG_YELLOW ANSI "43m"
#define TERM_BG_BLUE ANSI "44m"
#define TERM_BG_MAGENTA ANSI "45m"
#define TERM_BG_CYAN ANSI "46m"
#define TERM_BG_WHITE ANSI "47m"

#define TERM_MOUSE_ON ANSI "?1003h"
#define TERM_MOUSE_OFF ANSI "?1003l"

#define TERM_MOUSE_BUTTON_ON ANSI "?1002h"
#define TERM_MOUSE_BUTTON_OFF ANSI "?1002l"

#define TERM_MOUSE_EXTENDED_ON ANSI "?1006h"
#define TERM_MOUSE_EXTENDED_OFF ANSI "?1006l"

#define TERM_UP ANSI "A"
#define TERM_DOWN ANSI "B"

#define TERM_ALT_BUFFER ANSI "?1049h"
#define TERM_MAIN_BUFFER ANSI "?1049l"

// ### Values that never can be negative
#define positive_range unsigned

// ### Values that can be negative
#define bipolar_range signed

/// A non value returning function
typedef void fn;

// ### Positive range 8 bit integer
// range:       0 to +255
// memory:      [ 00000000 ]
// hex:         [ 0x00 ]
// linguistic:  (zero) to (plus) two hundred fifty-five
// traditional: unsigned char
// alt:         array of 8 bits
typedef positive_range char p8;
#define p8_max 255
#define p8_min 0
#define p8_char_max 3
#define p8_bytes 1
#define p8_bits 8

// ### Bipolar range 8 bit integer
// range:       -128 to +127
// memory:      [ 00000000 ]
// hex:         [ 0x00 ]
// linguistic:  (minus) one hundred twenty-eight to (plus) one hundred twenty-seven
// traditional: char
// alt:         array of 8 bits
typedef bipolar_range char b8;
#define b8_max 127
#define b8_min -128
#define b8_char_max 4
#define b8_bytes 1
#define b8_bits 8

// ### Positive range 16 bit integer
// range:       0 to +65535
// memory:      [ 00000000 | 00000000 ]
// hex:         [ 0x00 | 0x00 ]
// linguistic:  (zero) to (plus) sixty-five thousand...
// traditional: unsigned short
// alt:         array of 16 bits
typedef positive_range short int p16;
#define p16_max 65535
#define p16_min 0
#define p16_char_max 6
#define p16_bytes 2
#define p16_bits 16

// ### Bipolar range 16 bit integer
// range:       -32768 to +32767
// memory:      [ 00000000 | 00000000 ]
// hex:         [ 0x00 | 0x00 ]
// linguistic:  (minus) thirty-two thousand... to (plus) thirty-two thousand...
// traditional: short
// alt:         array of 16 bits
typedef bipolar_range short int b16;
#define b16_max 32767
#define b16_min -32768
#define b16_char_max 6
#define b16_bytes 2
#define b16_bits 16

// ### Positive range 32 bit integer
// range:       0 to +4294967295
// memory:      [ 00000000 | 00000000 | 00000000 | 00000000 ]
// hex:         [ 0x00 | 0x00 | 0x00 | 0x00 ]
// linguistic:  (zero) to (plus) four billion...
// traditional: unsigned int
// alt:         array of 32 bits
typedef positive_range int p32;
#define p32_max 4294967295
#define p32_min 0
#define p32_char_max 10
#define p32_bytes 4
#define p32_bits 32

// ### Bipolar range 32 bit integer
// range:       -2147483648 to +2147483647
// memory:      [ 00000000 | 00000000 | 00000000 | 00000000 ]
// hex:         [ 0x00 | 0x00 | 0x00 | 0x00 ]
// linguistic:  (minus) two billion... to (plus) two billion...
// traditional: int
// alt:         array of 32 bits
typedef bipolar_range int b32;
#define b32_max 2147483647
#define b32_min -2147483648
#define b32_char_max 11
#define b32_bytes 4
#define b32_bits 32

// ### Positive range 64 bit integer
// range:       0 to +18446744073709551615
// memory:      [ 00000000 | 00000000 | 00000000 | 00000000 | 00000000 | 00000000 | 00000000 | 00000000 ]
// hex:         [ 0x00 | 0x00 | 0x00 | 0x00 | 0x00 | 0x00 | 0x00 | 0x00 ]
// linguistic:  (zero) to (plus) eighteen quintillion...
// traditional: unsigned long int
// alt:         array of 64 bits
typedef positive_range long long int p64;
#define p64_max 18446744073709551615
#define p64_min 0
#define p64_char_max 20
#define p64_bytes 8
#define p64_bits 64

// ### Bipolar range 64 bit integer
// range:       -9223372036854775808 to +9223372036854775807
// memory:      [ 00000000 | 00000000 | 00000000 | 00000000 | 00000000 | 00000000 | 00000000 | 00000000 ]
// hex:         [ 0x00 | 0x00 | 0x00 | 0x00 | 0x00 | 0x00 | 0x00 | 0x00 ]
// linguistic:  (minus) nine quintillion... to (plus) nine quintillion...
// traditional: long int
// alt:         array of 64 bits
typedef bipolar_range long long int b64;
#define b64_max 9223372036854775807
#define b64_min -9223372036854775808
#define b64_char_max 21
#define b64_bytes 8
#define b64_bits 64

#if BITS != 64
__extension__ typedef bipolar_range long long int i64;
__extension__ typedef positive_range long long int u64;
#endif

// 128 bit integers exist as a compiler extension on 64 bit targets only, and
// C has no literal syntax that can express their range, so the bounds are
// built as expressions rather than written out.
#if defined(__SIZEOF_INT128__)
#define HAS_128 1

// ### Positive range 128 bit integer
// range:       0 to +340282366920938463463374607431768211455
// memory:      [ 00000000 x16 ]
// hex:         [ 0x00 x16 ]
// linguistic:  (zero) to (plus) three hundred forty undecillion...
// traditional: unsigned __int128
// alt:         array of 128 bits
__extension__ typedef positive_range __int128 p128;
#define p128_max ((p128) ~ (p128)0)
#define p128_min ((p128)0)
#define p128_char_max 39
#define p128_bytes 16
#define p128_bits 128

// ### Bipolar range 128 bit integer
// range:       -170141183460469231731687303715884105728
//         to   +170141183460469231731687303715884105727
// memory:      [ 00000000 x16 ]
// hex:         [ 0x00 x16 ]
// traditional: __int128
// alt:         array of 128 bits
__extension__ typedef bipolar_range __int128 b128;
#define b128_max ((b128)(((p128)1 << 127) - 1))
#define b128_min ((b128)((p128)1 << 127))
#define b128_char_max 40
#define b128_bytes 16
#define b128_bits 128
#endif

typedef float f32;
#define f32_max 3.40282346638528859812e+38F
#define f32_min 1.17549435082228750797e-38F
#define f32_epsilon 1.1920928955078125e-07F
#define f32_char_max 10
#define f32_bytes 4
#define f32_bits 32

typedef double f64;
#define f64_max 1.79769313486231570815e+308
#define f64_min 2.22507385850720138309e-308
#define f64_epsilon 2.22044604925031308085e-16
#define f64_char_max 20
#define f64_bytes 8
#define f64_bits 64

// ### Extended range decimal
// long double is the widest decimal the compiler offers, but its width is
// decided by the target, not by us: 80 bit x87 stored in 16 bytes on x86_64,
// plain f64 on arm64 macOS, 128 bit quad on arm64 Linux. The bounds therefore
// come from the compiler rather than being written out, so they stay true on
// every target instead of only on the one they were typed for.
typedef long double f128;
#define f128_max __LDBL_MAX__
#define f128_min __LDBL_MIN__
#define f128_epsilon __LDBL_EPSILON__
#define f128_char_max 45
#define f128_bytes __SIZEOF_LONG_DOUBLE__
#define f128_bits (__SIZEOF_LONG_DOUBLE__ * 8)

#if BITS == 64
typedef f64 decimal;
#define decimal_max f64_max
#define decimal_min f64_min
#define decimal_char_max f64_char_max
#define decimal_bytes f64_bytes
#define decimal_bits f64_bits
#else
typedef f32 decimal;
#define decimal_max f32_max
#define decimal_min f32_min
#define decimal_char_max f32_char_max
#define decimal_bytes f32_bytes
#define decimal_bits f32_bits
#endif

#if BITS == 64

// ### Positive range [native] bit integer
// recommended type for most cases (especially for memory addresses, or loops)
// where native is the default integer size of the system architecture
// that's compiled for, for example,
//
// 64 bit systems: positive == p64
// range:       0 to +18446744073709551615
//
// 32 bit systems: positive == p32
// range:       0 to +4294967295
//
typedef p64 positive;
#define positive_max p64_max
#define positive_min p64_min
#define positive_char_max p64_char_max
#define positive_bytes p64_bytes
#define positive_bits p64_bits

// ### Bipolar range [native] bit integer
// recommended type for most cases (especially for memory addresses, or loops)
// where native is the default integer size of the system architecture
// that's compiled for, for example,
//
// 64 bit systems: bipolar == b64
// range:       -9223372036854775808 to +9223372036854775807
//
// 32 bit systems: bipolar == b32
// range:       -2147483648 to +2147483647
typedef b64 bipolar;
#define bipolar_max b64_max
#define bipolar_min b64_min
#define bipolar_char_max b64_char_max
#define bipolar_bytes b64_bytes
#define bipolar_bits b64_bits

// ### Native Decimal range floating point
// range:       1.7E-308 to 1.7E+308
typedef f64 decimal;
#define decimal_max f64_max
#define decimal_min f64_min
#define decimal_char_max f64_char_max
#define decimal_bytes f64_bytes
#define decimal_bits f64_bits

#else

// ### Positive range [native] bit integer
// recommended type for most cases (especially for memory addresses, or loops)
// where native is the default integer size of the system architecture
// that's compiled for, for example,
//
// 64 bit systems: positive == p64
// range:       0 to +18446744073709551615
//
// 32 bit systems: positive == p32
// range:       0 to +4294967295
//
typedef p32 positive;
#define positive_max p32_max
#define positive_min p32_min
#define positive_char_max p32_char_max
#define positive_bytes p32_bytes
#define positive_bits p32_bits

// ### Bipolar range [native] bit integer
// recommended type for most cases (especially for memory addresses, or loops)
// where native is the default integer size of the system architecture
// that's compiled for, for example,
//
// 64 bit systems: bipolar == b64
// range:       -9223372036854775808 to +9223372036854775807
//
// 32 bit systems: bipolar == b32
// range:       -2147483648 to +2147483647
typedef b32 bipolar;
#define bipolar_max b32_max
#define bipolar_min b32_min
#define bipolar_char_max b32_char_max
#define bipolar_bytes b32_bytes
#define bipolar_bits b32_bits

// ### Native Decimal range floating point

typedef f32 decimal;
#define decimal_max f32_max
#define decimal_min f32_min
#define decimal_char_max f32_char_max
#define decimal_bytes f32_bytes
#define decimal_bits f32_bits
#endif

typedef typeof(sizeof(0)) sized;

#define false 0
#define true 1

#undef bool
#define bool p8

#define ir(asm_args...) \
        asm volatile(asm_args)

#define b8_data(...) asm volatile(".byte " #__VA_ARGS__ "\n")
#define b16_data(...) asm volatile(".word " #__VA_ARGS__ "\n")
#define b32_data(...) asm volatile(".long " #__VA_ARGS__ "\n")
#define b64_data(...) asm volatile(".quad " #__VA_ARGS__ "\n")

#if X64
#define ASM(name) asm_x64_##name
#endif

#if ARM64
#define ASM(name) asm_arm64_##name
#endif

#if RISCV64
#define ASM(name) asm_riscv64_##name
#endif

#define asm_x64_add "add"
#define asm_x64_sub "sub"
#define asm_x64_copy "mov"
#define asm_x64_copy_64 "movq"
#define asm_x64_copy_32 "movl"
#define asm_x64_store "mov"
#define asm_x64_jump "jmp"
#define asm_x64_branch "je"
#define asm_x64_ret "ret"
#define asm_x64_reg_0 "%rax"
#define asm_x64_reg_1 "%rdi"
#define asm_x64_reg_2 "%rsi"
#define asm_x64_reg_3 "%rdx"
#define asm_x64_reg_4 "%rcx"
#define asm_x64_reg_5 "%r8"
#define asm_x64_reg_6 "%r9"
#define asm_x64_temp_0 "%r10"
#define asm_x64_temp_1 "%r11"
#define asm_x64_stack_pointer "%rsp"
#define asm_x64_frame_pointer "%rbp"
#define asm_x64_syscall "syscall"
#define asm_x64_syscall_slot asm_x64_reg_0

#define asm_arm64_add "add"
#define asm_arm64_sub "sub"
#define asm_arm64_copy "mov"
#define asm_arm64_jump "b"
#define asm_arm64_branch "beq"
#define asm_arm64_store "str"
#define asm_arm64_ret "ret"
#define asm_arm64_reg_0 "x0"
#define asm_arm64_reg_1 "x1"
#define asm_arm64_reg_2 "x2"
#define asm_arm64_reg_3 "x3"
#define asm_arm64_reg_4 "x4"
#define asm_arm64_reg_5 "x5"
#define asm_arm64_reg_6 "x6"
#define asm_arm64_temp_0 "x9"
#define asm_arm64_temp_1 "x10"
#define asm_arm64_stack_pointer "sp"
#define asm_arm64_frame_pointer "x29"

#if defined(MACOS) && defined(ARM64)
#define asm_arm64_syscall "svc 0x80"
#define asm_arm64_syscall_slot "x16"
#else
#define asm_arm64_syscall "svc 0"
#define asm_arm64_syscall_slot "x8"
#endif

#define asm_riscv64_add "add"
#define asm_riscv64_sub "sub"
#define asm_riscv64_copy "ld"
#define asm_riscv64_store "sd"
#define asm_riscv64_jump "jalr"
#define asm_riscv64_branch "beq"
#define asm_riscv64_syscall "ecall"
#define asm_riscv64_ret "ret"
#define asm_riscv64_reg_0 "a0"
#define asm_riscv64_reg_1 "a1"
#define asm_riscv64_reg_2 "a2"
#define asm_riscv64_reg_3 "a3"
#define asm_riscv64_reg_4 "a4"
#define asm_riscv64_reg_5 "a5"
#define asm_riscv64_reg_6 "a6"
#define asm_riscv64_temp_0 "t0"
#define asm_riscv64_temp_1 "t1"
#define asm_riscv64_stack_pointer "sp"
#define asm_riscv64_frame_pointer "s0"
#define asm_riscv64_syscall_slot asm_riscv64_reg_0

#define jump(where) ir(ASM(jump) " " ASM(where) ";")
#define branch(where) ir(ASM(branch) " " ASM(where) ";")
#define add(what, with) ir(ASM(add) " " ASM(what) "," ASM(with) ";")
#define sub(what, with) ir(ASM(sub) " " ASM(what) "," ASM(with) ";")
#define call(what) ir("call " ASM(what) ";")

#define register_get(reg, dest) ir(ASM(copy) " %0, " ASM(reg) : "=r"(dest))
#define register_set(reg, src) ir(ASM(copy) " " ASM(reg) ", %0" : : "r"(src))

// ### String address
// a pointer to a string in memory, usually the first p8 character of the string
typedef p8 address_to string_address;
typedef p8 string[];
typedef const p8 address_to const_string;

// Helper function for writing static strings to a writer with data + length
// example with:
//      write(str("Hello, world!\n"));
// example without:
//      write("Hello, world!\n", 14); // error prone!
#define str(string) (string), (sizeof(string))

#define string_index(source, index) (address_to((source) + (index)))
#define string_get(source) (address_to(source))
#define string_set(source, value) (address_to(source) = value)
#define string_is(source, value) (address_to(source) == (value))
#define string_not(source, value) (address_to(source) != (value))
#define string_equals(source, input) (strcmp(source, input) == 0)

#define string_set_if(source, check, value) \
        ((source) == (check) ? ((source) = (value), true) : false)

#undef min
#define min(value, input) ((value) > (input) ? (input) : (value))

#undef max
#define max(value, input) ((value) < (input) ? (input) : (value))

#define square(value) ((value) * (value))
#define cube(value) ((value) * (value) * (value))
#define mod(value, input) ((value) % (input))
#define floor(a) ((decimal)((bipolar)(a)))

#undef clamp
#define clamp(value, min, max) ((value) < (min) ? (min) : (value) > (max) ? (max) \
                                                                          : (value))

// Writer functions are intended as flexible outout functions passed to functions as arguments
// and should be easy for compiler to optimize into a zero cost abstraction
// if length is zero, the function should write until a null terminator is reached (string_length)
// writers redused file size, faster, and more flexible
typedef fn(address_to writer)(address_any data, positive length);
typedef fn(address_to writer_string)(string_address string);
typedef fn(address_to writer_string_len)(string_address string, positive length);

// a thread-local storage variable, unique to each thread
#define local __thread

typedef struct
{
        b64 counter;
} atomic64;

#ifndef STANDARD_MODERN_C_NO_MATH

#define PI 3.14159265359f
#define PI2 6.28318530718f
#define PI_05x (PI * 0.5f)

#define CONVERSION_CONSTANTS                         \
        constexpr decimal RadToDeg = 180.0f / PI;    \
        constexpr decimal RadToTurn = 0.5f / PI;     \
        constexpr decimal DegToRad = PI / 180.0f;    \
        constexpr decimal DegToTurn = 0.5f / 180.0f; \
        constexpr decimal TurnToRad = PI / 0.5f;     \
        constexpr decimal TurnToDeg = 180.0f / 0.5f

#define AngleRad(value) (value)
#define AngleDeg(value) ((value) * DegToRad)
#define AngleTurn(value) ((value) * TurnToRad)

#ifndef KERNEL_MODE // for now :3
// simpler polynomial error < 0.01
decimal fast_sin(decimal x)
{
        x = x - PI2 * (bipolar)(x / PI2);

        if (x < 0)
                x += PI2;

        decimal sign = 1.0f;

        if (x > PI)
        {
                x -= PI;
                sign = -1.0f;
        }
        if (x > PI / 2)
        {
                x = PI - x;
        }

        return sign * 4.0f * x * (PI - x) / (PI * PI);
}
#endif // KERNEL_MODE

typedef union vector2
{
        struct
        {
                decimal x, y;
        };

        struct
        {
                decimal width, height;
        };

        decimal axis[2];

} vector2;

typedef union bipolar2
{
        struct
        {
                bipolar x, y;
        };

        struct
        {
                bipolar width, height;
        };

        bipolar axis[2];

} bipolar2;

typedef union positive2
{
        struct
        {
                positive x, y;
        };

        struct
        {
                positive width, height;
        };

        positive axis[2];

} positive2;

typedef union vector3
{
        struct
        {
                decimal x, y, z;
        };

        struct
        {
                decimal width, height, depth;
        };

        decimal axis[3];

} vector3;

typedef union bipolar3
{
        struct
        {
                bipolar x, y, z;
        };

        struct
        {
                bipolar width, height, depth;
        };

        bipolar axis[3];

} bipolar3;

typedef union positive3
{
        struct
        {
                positive x, y, z;
        };

        struct
        {
                positive width, height, depth;
        };

        positive axis[3];

} positive3;

typedef union vector4
{
        struct
        {
                decimal x, y, z, w;
        };

        struct
        {
                decimal width, height, depth, time;
        };

        decimal axis[4];

} vector4;

typedef vector4 quaternion;

typedef union matrix2
{
        decimal axis[2][2];
        vector2 colum[2];
} matrix2;

typedef union matrix3
{
        decimal axis[3][3];
        vector3 colum[3];
} matrix3;

typedef union matrix4
{
        decimal axis[4][4];
        vector4 colum[4];
} matrix4;

#endif // STANDARD_MODERN_C_NO_MATH


/*
        The assembly, and the shape the machine it lands on wants it in.

        Every routine below is one implementation serving both sides: this file
        is compiled into the kernel through core.c and into every program
        through its own include, so what the kernel runs and what a program
        runs are the same instructions rather than the same intent written
        twice. The C loops that used to sit here as the userspace half are
        gone; what is left under the last #else is the fallback for an
        architecture that has no block yet, which is the same answer the kernel
        gets there.

        The macros exist because a symbol is not just a label. The kernel
        decorates every function it emits with things that are invisible until
        the configuration that needs them is turned on, and assembly written by
        hand is exactly where those get forgotten.
*/
#if X64 || ARM64 || RISCV64
#define MOONWATER_ASSEMBLY 1
#endif

#ifdef KERNEL_MODE
#define ASM_EXPORT(name) EXPORT_SYMBOL(name)
#else
#define ASM_EXPORT(name)
#endif

// aarch64 spells it with a percent, everything else with an at. Getting this
// wrong makes the assembler read the rest of the line as a comment.
#if ARM64
#define ASM_TYPE "%function"
#else
#define ASM_TYPE "@function"
#endif

// Indirect branch tracking needs a landing pad at every symbol something can
// call through a pointer. Without one the call faults on a machine that has it.
#if defined(KERNEL_MODE) && X64 && defined(CONFIG_X86_KERNEL_IBT)
#define ASM_ENDBR "endbr64\n"
#else
#define ASM_ENDBR ""
#endif

/*
        A return, or whatever the mitigations have made of one.

        A bare ret is the gadget the return thunk exists to take away, so a
        kernel built with that mitigation rewrites every one into a jump to it.
        Writing ret here regardless would leave these the only functions in the
        image still returning the old way -- a hole in exactly the routines
        every other function calls. Straight line speculation wants the trap
        after it instead. Neither is on in the build this was written against,
        which is the reason to decide it here rather than notice later.
*/
#if defined(KERNEL_MODE) && X64
#if defined(CONFIG_MITIGATION_RETHUNK) || defined(CONFIG_RETHUNK)
#define ASM_RET "jmp __x86_return_thunk\n"
#elif defined(CONFIG_MITIGATION_SLS) || defined(CONFIG_SLS)
#define ASM_RET "ret\nint3\n"
#else
#define ASM_RET "ret\n"
#endif
#else
#define ASM_RET "ret\n"
#endif

#define ASM_FUNC(name)                  \
    ".balign 16\n"                      \
    ".globl " #name "\n"                \
    ".type " #name ", " ASM_TYPE "\n"   \
    #name ":\n" ASM_ENDBR

#define ASM_END(name) ".size " #name ", .-" #name "\n"

#if X64
__asm__(
    //
    //       strlen -- a word at a time.
    //
    //       lib/string.c's strlen is a byte at a time, and on x86_64 nothing
    //       overrides it: arch/x86/include/asm/string_64.h claims memcpy, memmove
    //       and memset and leaves the rest. arm64 and riscv both define
    //       __HAVE_ARCH_STRLEN and ship their own, so this is x86 catching up
    //       rather than x86 being special.
    //
    //       Measured against the generic loop on a 9950X, 4096 calls each:
    //
    //           4 bytes    23048 ticks byte     36120 word
    //           8 bytes    36292 ticks byte     23047 word
    //          16 bytes    62350 ticks byte     26358 word
    //          32 bytes   194317 ticks byte     33067 word
    //          64 bytes   219300 ticks byte     46010 word
    //
    //       The crossover is at eight. Below it the alignment setup costs more
    //       than it saves, above it the win runs to nearly six times.
    //
    //       Reading a whole word that straddles the start of the string is safe:
    //       aligning down stays inside the same page, so the load cannot fault on
    //       memory the caller did not give us. The bytes before the string are
    //       forced to 0xff afterwards so they cannot be mistaken for the
    //       terminator.
    //
    "        .text\n"
    ASM_FUNC(strlen)
    "        mov     %rdi, %r8               # keep the start\n"
    "        mov     %edi, %ecx\n"
    "        and     $7, %ecx                # how far into the word it begins\n"
    "        and     $-8, %rdi               # align down\n"
    "        mov     (%rdi), %rdx            # safe: same page as the start\n"
    "        shl     $3, %ecx                # bytes -> bits\n"
    "        mov     $1, %rax\n"
    "        shl     %cl, %rax\n"
    "        dec     %rax                    # ones below the string, zero if aligned\n"
    "        or      %rax, %rdx              # so they cannot look like a terminator\n"
    "        movabs  $0x0101010101010101, %r10\n"
    "        movabs  $0x8080808080808080, %r11\n"
    //
    //       (v - 0x01..) & ~v & 0x80.. is non-zero exactly when some byte
    //       of v is zero: subtracting one borrows into the high bit of a
    //       zero byte and of no other.
    //
    "1:      mov     %rdx, %rax\n"
    "        sub     %r10, %rax\n"
    "        mov     %rdx, %rsi\n"
    "        not     %rsi\n"
    "        and     %rsi, %rax\n"
    "        and     %r11, %rax\n"
    "        jnz     2f\n"
    "        add     $8, %rdi\n"
    "        mov     (%rdi), %rdx\n"
    "        jmp     1b\n"
    "2:      bsf     %rax, %rax              # first set high bit\n"
    "        shr     $3, %rax                # its byte within the word\n"
    "        add     %rdi, %rax              # address of the terminator\n"
    "        sub     %r8, %rax               # minus where we started\n"
    ASM_RET
    ASM_END(strlen)
    //
    //       strcmp -- a word at a time, with two pointers and no length.
    //
    //       The hard one, and the reason is worth stating. strlen could align its
    //       pointer down, because an eight byte read aligned to eight never leaves
    //       the page it started in. strncmp and memcmp could read unaligned,
    //       because a length told them how far they were allowed to go. strcmp has
    //       neither: two pointers at whatever alignments the caller chose, and
    //       nothing but a terminator to say where they end. Aligning one down
    //       misaligns the other, and reading unaligned past the terminator can
    //       walk into a page nobody mapped.
    //
    //       What makes it safe is the same fact stated differently: a read of
    //       eight bytes cannot fault if all eight are in a page that already holds
    //       a byte we are allowed to read. The byte at the pointer is such a byte
    //       -- the string has not ended yet, or we would have stopped -- so the
    //       read is safe whenever it does not cross the page boundary, and the
    //       offset within the page says whether it does.
    //
    //       Two compares buy eight bytes of progress. Near a page boundary the
    //       loop steps a byte at a time until it is past, which happens for at
    //       most seven bytes out of every four thousand and ninety six.
    //
    "        .text\n"
    ASM_FUNC(strcmp)
    "        movabs  $0x0101010101010101, %r10\n"
    "        movabs  $0x8080808080808080, %r11\n"
    "1:      #\n"
    //      Would either read cross a page? 0xff8 is the last offset at
    //      which eight bytes still fit.
    //
    "        mov     %edi, %ecx\n"
    "        and     $0xfff, %ecx\n"
    "        cmp     $0xff8, %ecx\n"
    "        ja      2f\n"
    "        mov     %esi, %ecx\n"
    "        and     $0xfff, %ecx\n"
    "        cmp     $0xff8, %ecx\n"
    "        ja      2f\n"
    "        mov     (%rdi), %r8\n"
    "        mov     (%rsi), %r9\n"
    "        cmp     %r8, %r9\n"
    "        jne     2f                      # differ: let the byte step find where\n"
    //
    //      Eight equal bytes. If a terminator is among them the strings
    //      ended together and are equal.
    //
    "        mov     %r8, %rax\n"
    "        sub     %r10, %rax\n"
    "        mov     %r8, %rcx\n"
    "        not     %rcx\n"
    "        and     %rcx, %rax\n"
    "        and     %r11, %rax\n"
    "        jnz     3f\n"
    "        add     $8, %rdi\n"
    "        add     $8, %rsi\n"
    "        jmp     1b\n"
    //
    //      One byte, then back to the word loop. Reached when a read would
    //      cross a page and when a word differs -- in the second case it
    //      walks the few bytes to the difference, which happens once.
    //
    "2:      movzbl  (%rdi), %eax\n"
    "        movzbl  (%rsi), %ecx\n"
    "        sub     %ecx, %eax\n"
    "        jnz     4f\n"
    "        test    %ecx, %ecx\n"
    "        jz      3f\n"
    "        inc     %rdi\n"
    "        inc     %rsi\n"
    "        jmp     1b\n"
    "3:      xor     %eax, %eax\n"
    "4:      RET\n"
    ASM_END(strcmp)
    //
    //       strchr and memchr -- a word at a time.
    //
    //       The last two byte loops in lib/string.c worth taking. Both hunt for a
    //       byte, so both broadcast it across a word and reuse the trick that finds
    //       a zero byte: after exclusive-or with the broadcast, the byte that
    //       matched is the byte that is now zero.
    //
    //       An eight byte load aligned to eight never crosses a page, so reading
    //       the word that contains the string's first byte cannot fault on memory
    //       the caller does not own. The matches in the bytes before the string are
    //       thrown away afterwards by masking the result rather than the input,
    //       which keeps the byte being searched for out of it -- forcing those
    //       bytes to 0xff would false-match a search for 0xff.
    //
    "        .text\n"
    //
    //       char *strchr(const char *s, int c)
    //
    //       Two hunts at once: the byte, and the terminator that ends the search.
    //       Whichever comes first in the word is the answer, and it is a hit only
    //       if that byte is the one asked for -- which is also how strchr(s, 0)
    //       returns the terminator rather than nothing.
    //
    ASM_FUNC(strchr)
    "        movzbl  %sil, %ecx\n"
    "        movabs  $0x0101010101010101, %r10\n"
    "        mov     %rcx, %rsi\n"
    "        imul    %r10, %rsi              # c in every byte; %sil is still c\n"
    "        movabs  $0x8080808080808080, %r11\n"
    "        mov     %edi, %ecx\n"
    "        and     $7, %ecx\n"
    "        and     $-8, %rdi\n"
    "        mov     (%rdi), %rdx\n"
    "        shl     $3, %ecx\n"
    "        mov     $-1, %r9\n"
    "        shl     %cl, %r9                # which bytes of this word are ours\n"
    "1:      mov     %rdx, %rax\n"
    "        xor     %rsi, %rax              # zero where the byte matched\n"
    "        mov     %rax, %rcx\n"
    "        not     %rcx\n"
    "        sub     %r10, %rax\n"
    "        and     %rcx, %rax\n"
    "        and     %r11, %rax              # found the byte\n"
    "        mov     %rdx, %r8\n"
    "        sub     %r10, %r8\n"
    "        mov     %rdx, %rcx\n"
    "        not     %rcx\n"
    "        and     %rcx, %r8\n"
    "        and     %r11, %r8               # found the terminator\n"
    "        or      %r8, %rax\n"
    "        and     %r9, %rax\n"
    "        jnz     2f\n"
    "        mov     $-1, %r9                # past the first word, all of it is ours\n"
    "        add     $8, %rdi\n"
    "        mov     (%rdi), %rdx\n"
    "        jmp     1b\n"
    "2:      bsf     %rax, %rax\n"
    "        shr     $3, %rax\n"
    "        add     %rdi, %rax              # the byte or the terminator, whichever\n"
    "        movzbl  (%rax), %ecx\n"
    "        cmp     %sil, %cl\n"
    "        je      3f\n"
    "        xor     %eax, %eax              # it was the terminator: not found\n"
    "3:      RET\n"
    ASM_END(strchr)
    //
    //       void *memchr(const void *s, int c, size_t n)
    //
    //       The same hunt with a fence instead of a terminator. A match found in
    //       the word that reaches past the bound is discarded by comparing its
    //       address, which is cheaper than stopping the scan short.
    //
    ASM_FUNC(memchr)
    "        xor     %eax, %eax\n"
    "        test    %rdx, %rdx\n"
    "        jz      9f\n"
    "        movzbl  %sil, %ecx\n"
    "        movabs  $0x0101010101010101, %r10\n"
    "        mov     %rcx, %rsi\n"
    "        imul    %r10, %rsi\n"
    "        movabs  $0x8080808080808080, %r11\n"
    "        lea     (%rdi,%rdx), %r9        # one past the last byte we may report\n"
    "        mov     %edi, %ecx\n"
    "        and     $7, %ecx\n"
    "        and     $-8, %rdi\n"
    "        mov     (%rdi), %rdx\n"
    "        xor     %rsi, %rdx\n"
    "        shl     $3, %ecx\n"
    "        mov     $-1, %r8\n"
    "        shl     %cl, %r8\n"
    "1:      mov     %rdx, %rax\n"
    "        sub     %r10, %rax\n"
    "        mov     %rdx, %rcx\n"
    "        not     %rcx\n"
    "        and     %rcx, %rax\n"
    "        and     %r11, %rax\n"
    "        and     %r8, %rax\n"
    "        jnz     2f\n"
    "        mov     $-1, %r8\n"
    "        add     $8, %rdi\n"
    "        cmp     %r9, %rdi\n"
    "        jae     8f\n"
    "        mov     (%rdi), %rdx\n"
    "        xor     %rsi, %rdx\n"
    "        jmp     1b\n"
    "2:      bsf     %rax, %rax\n"
    "        shr     $3, %rax\n"
    "        add     %rdi, %rax\n"
    "        cmp     %r9, %rax\n"
    "        jb      9f                      # inside the bound: that is the answer\n"
    "8:      xor     %eax, %eax\n"
    "9:      RET\n"
    ASM_END(memchr)
    //
    //       strchrnul, strnchr and strrchr -- the rest of the byte hunts.
    //
    //       All three are strchr with one thing changed: what to return when the
    //       byte is absent, where to stop, and which match to keep. The machinery
    //       underneath is the one strchr already uses -- broadcast the byte across
    //       a word, exclusive-or, and the byte that matched is the byte that is
    //       now zero -- so what follows is mostly the differences.
    //
    "        .text\n"
    //
    //       char *strchrnul(const char *s, int c)
    //
    //       strchr that answers with the terminator instead of nothing. Since the
    //       scan already stops at whichever of the two comes first, that is the
    //       same code without the last test.
    //
    ASM_FUNC(strchrnul)
    "        movzbl  %sil, %ecx\n"
    "        movabs  $0x0101010101010101, %r10\n"
    "        mov     %rcx, %rsi\n"
    "        imul    %r10, %rsi\n"
    "        movabs  $0x8080808080808080, %r11\n"
    "        mov     %edi, %ecx\n"
    "        and     $7, %ecx\n"
    "        and     $-8, %rdi\n"
    "        mov     (%rdi), %rdx\n"
    "        shl     $3, %ecx\n"
    "        mov     $-1, %r9\n"
    "        shl     %cl, %r9\n"
    "1:      mov     %rdx, %rax\n"
    "        xor     %rsi, %rax\n"
    "        mov     %rax, %rcx\n"
    "        not     %rcx\n"
    "        sub     %r10, %rax\n"
    "        and     %rcx, %rax\n"
    "        and     %r11, %rax\n"
    "        mov     %rdx, %r8\n"
    "        sub     %r10, %r8\n"
    "        mov     %rdx, %rcx\n"
    "        not     %rcx\n"
    "        and     %rcx, %r8\n"
    "        and     %r11, %r8\n"
    "        or      %r8, %rax\n"
    "        and     %r9, %rax\n"
    "        jnz     2f\n"
    "        mov     $-1, %r9\n"
    "        add     $8, %rdi\n"
    "        mov     (%rdi), %rdx\n"
    "        jmp     1b\n"
    "2:      bsf     %rax, %rax\n"
    "        shr     $3, %rax\n"
    "        add     %rdi, %rax\n"
    ASM_RET
    ASM_END(strchrnul)
    //
    //       char *strnchr(const char *s, size_t count, int c)
    //
    //       strchr with a fence. Note the argument order: the count comes second
    //       and the byte third, which is the opposite way round from memchr and
    //       is the kind of thing that is only wrong once.
    //
    ASM_FUNC(strnchr)
    "        xor     %eax, %eax\n"
    "        test    %rsi, %rsi\n"
    "        jz      9f\n"
    "        movzbl  %dl, %ecx\n"
    "        movabs  $0x0101010101010101, %r10\n"
    "        lea     (%rdi,%rsi), %r9        # one past the last byte we may report\n"
    "        mov     %rcx, %rsi\n"
    "        imul    %r10, %rsi\n"
    "        movabs  $0x8080808080808080, %r11\n"
    "        mov     %edi, %ecx\n"
    "        and     $7, %ecx\n"
    "        and     $-8, %rdi\n"
    "        mov     (%rdi), %rdx\n"
    "        shl     $3, %ecx\n"
    //
    //      Into %r8, not %rcx: the shift count is in %cl, which is part of
    //      %rcx, so building the mask there destroys the count before the
    //      shift reads it. That mistake passed the build, booted, and was
    //      wrong in 293398 of 1401280 cases.
    //
    "        mov     $-1, %r8\n"
    "        shl     %cl, %r8\n"
    "        push    %r8                     # the valid-byte mask, out of registers\n"
    "1:      mov     %rdx, %rax\n"
    "        xor     %rsi, %rax\n"
    "        mov     %rax, %rcx\n"
    "        not     %rcx\n"
    "        sub     %r10, %rax\n"
    "        and     %rcx, %rax\n"
    "        and     %r11, %rax\n"
    "        mov     %rdx, %r8\n"
    "        sub     %r10, %r8\n"
    "        mov     %rdx, %rcx\n"
    "        not     %rcx\n"
    "        and     %rcx, %r8\n"
    "        and     %r11, %r8\n"
    "        or      %r8, %rax\n"
    "        and     (%rsp), %rax\n"
    "        jnz     2f\n"
    "        movq    $-1, (%rsp)\n"
    "        add     $8, %rdi\n"
    "        cmp     %r9, %rdi\n"
    "        jae     8f\n"
    "        mov     (%rdi), %rdx\n"
    "        jmp     1b\n"
    "2:      bsf     %rax, %rax\n"
    "        shr     $3, %rax\n"
    "        add     %rdi, %rax\n"
    "        cmp     %r9, %rax\n"
    "        jae     8f                      # beyond the count\n"
    "        movzbl  (%rax), %ecx\n"
    "        cmp     %sil, %cl\n"
    "        je      3f\n"
    "8:      xor     %eax, %eax\n"
    "3:      add     $8, %rsp\n"
    "9:      RET\n"
    ASM_END(strnchr)
    //
    //       char *strrchr(const char *s, int c)
    //
    //       The last match rather than the first, which the forward scan does not
    //       answer directly: within a word the highest set bit is wanted, not the
    //       lowest, so bsr where the others use bsf. A word without a terminator
    //       may hold a later match than anything before it, so the best so far is
    //       carried along; the word that holds the terminator only counts matches
    //       below it.
    //
    //       Searching for the terminator itself is a byte walk. It is the one case
    //       where the answer is the end of the string rather than a match inside
    //       it, and it is rare enough not to be worth its own scan.
    //
    ASM_FUNC(strrchr)
    "        movzbl  %sil, %ecx\n"
    "        test    %cl, %cl\n"
    "        jnz     4f\n"
    // strrchr(s, 0) is the terminator.
    "1:      cmpb    $0, (%rdi)\n"
    "        je      2f\n"
    "        inc     %rdi\n"
    "        jmp     1b\n"
    "2:      mov     %rdi, %rax\n"
    ASM_RET
    "4:      push    %rbx\n"
    "        xor     %ebx, %ebx              # best so far: none\n"
    "        movabs  $0x0101010101010101, %r10\n"
    "        mov     %rcx, %rsi\n"
    "        imul    %r10, %rsi\n"
    "        movabs  $0x8080808080808080, %r11\n"
    "        mov     %edi, %ecx\n"
    "        and     $7, %ecx\n"
    "        and     $-8, %rdi\n"
    "        mov     (%rdi), %rdx\n"
    "        shl     $3, %ecx\n"
    "        mov     $-1, %r9\n"
    "        shl     %cl, %r9\n"
    "5:      mov     %rdx, %rax\n"
    "        xor     %rsi, %rax\n"
    "        mov     %rax, %rcx\n"
    "        not     %rcx\n"
    "        sub     %r10, %rax\n"
    "        and     %rcx, %rax\n"
    "        and     %r11, %rax\n"
    "        and     %r9, %rax               # matches in this word\n"
    "        mov     %rdx, %r8\n"
    "        sub     %r10, %r8\n"
    "        mov     %rdx, %rcx\n"
    "        not     %rcx\n"
    "        and     %rcx, %r8\n"
    "        and     %r11, %r8\n"
    "        and     %r9, %r8                # terminator in this word\n"
    "        test    %r8, %r8\n"
    "        jnz     7f\n"
    "        test    %rax, %rax\n"
    "        jz      6f\n"
    "        bsr     %rax, %rcx\n"
    "        shr     $3, %rcx\n"
    "        lea     (%rdi,%rcx), %rbx       # a later match than any before\n"
    "6:      mov     $-1, %r9\n"
    "        add     $8, %rdi\n"
    "        mov     (%rdi), %rdx\n"
    "        jmp     5b\n"
    //
    //      The string ends in this word. Only matches below the
    //      terminator count: isolate its lowest bit and keep what is
    //      under it.
    //
    "7:      mov     %r8, %rcx\n"
    "        neg     %rcx\n"
    "        and     %r8, %rcx\n"
    "        dec     %rcx\n"
    "        and     %rcx, %rax\n"
    "        jz      8f\n"
    "        bsr     %rax, %rcx\n"
    "        shr     $3, %rcx\n"
    "        lea     (%rdi,%rcx), %rbx\n"
    "8:      mov     %rbx, %rax\n"
    "        pop     %rbx\n"
    ASM_RET
    ASM_END(strrchr)
    //
    //       strncmp and strnlen -- a word at a time.
    //
    //       Both are byte loops in lib/string.c and neither is overridden on
    //       x86_64. Two functions in one file, which the dialect allows now: a
    //       #> shared closes the run of blocks before it and the next #> arch
    //       opens a new one.
    //
    //       An eight byte load aligned to eight never crosses a page, so reading
    //       the word that contains a pointer can never fault on memory the caller
    //       did not give us. That is what makes the unbounded scan safe; where a
    //       length is given the reads are bounded anyway.
    //
    "        .text\n"
    //
    //       int strncmp(const char *a, const char *b, size_t n)
    //
    //       Eight bytes from each, unaligned, which is legal here because n bounds
    //       the read. Equal and no terminator in them means advance; anything else
    //       hands those eight to the byte loop, which already knows how to stop on
    //       a difference or a terminator and gets the sign right. The difference is
    //       found once per call, so simple beats clever there.
    //
    ASM_FUNC(strncmp)
    "        xor     %eax, %eax\n"
    "        test    %rdx, %rdx\n"
    "        jz      9f\n"
    "        movabs  $0x0101010101010101, %r10\n"
    "        movabs  $0x8080808080808080, %r11\n"
    "1:      cmp     $8, %rdx\n"
    "        jb      2f\n"
    "        mov     (%rdi), %r8\n"
    "        mov     (%rsi), %r9\n"
    "        cmp     %r8, %r9\n"
    "        jne     2f                      # let the byte loop settle it\n"
    // Equal so far. If either holds a terminator the strings end here.
    "        mov     %r8, %rax\n"
    "        sub     %r10, %rax\n"
    "        mov     %r8, %rcx\n"
    "        not     %rcx\n"
    "        and     %rcx, %rax\n"
    "        and     %r11, %rax\n"
    "        jnz     8f\n"
    "        add     $8, %rdi\n"
    "        add     $8, %rsi\n"
    "        sub     $8, %rdx\n"
    "        jmp     1b\n"
    "2:      test    %rdx, %rdx\n"
    "        jz      8f\n"
    "3:      movzbl  (%rdi), %eax\n"
    "        movzbl  (%rsi), %ecx\n"
    "        sub     %ecx, %eax\n"
    "        jnz     9f\n"
    "        test    %ecx, %ecx\n"
    "        jz      8f\n"
    "        inc     %rdi\n"
    "        inc     %rsi\n"
    "        dec     %rdx\n"
    "        jnz     3b\n"
    "8:      xor     %eax, %eax\n"
    "9:      RET\n"
    ASM_END(strncmp)
    //
    //       size_t strnlen(const char *s, size_t n)
    //
    //       strlen with a fence. The scan is the same -- align down, force the
    //       bytes before the string non-zero, then look for a zero byte eight at a
    //       time -- and a terminator found beyond n is clamped back to n, which is
    //       what strnlen returns when there is none inside the bound.
    //
    ASM_FUNC(strnlen)
    "        xor     %eax, %eax\n"
    "        test    %rsi, %rsi\n"
    "        jz      9f\n"
    "        mov     %rdi, %r8               # start\n"
    "        lea     (%rdi,%rsi), %r9        # one past the last byte we may report\n"
    "        mov     %edi, %ecx\n"
    "        and     $7, %ecx\n"
    "        and     $-8, %rdi\n"
    "        mov     (%rdi), %rdx\n"
    "        shl     $3, %ecx\n"
    "        mov     $1, %rax\n"
    "        shl     %cl, %rax\n"
    "        dec     %rax\n"
    "        or      %rax, %rdx\n"
    "        movabs  $0x0101010101010101, %r10\n"
    "        movabs  $0x8080808080808080, %r11\n"
    "1:      mov     %rdx, %rax\n"
    "        sub     %r10, %rax\n"
    "        mov     %rdx, %rcx\n"
    "        not     %rcx\n"
    "        and     %rcx, %rax\n"
    "        and     %r11, %rax\n"
    "        jnz     2f\n"
    "        add     $8, %rdi\n"
    "        cmp     %r9, %rdi\n"
    "        jae     3f                      # nothing within the bound\n"
    "        mov     (%rdi), %rdx\n"
    "        jmp     1b\n"
    "2:      bsf     %rax, %rax\n"
    "        shr     $3, %rax\n"
    "        add     %rdi, %rax              # where the terminator is\n"
    "        sub     %r8, %rax               # how far in that is\n"
    "        cmp     %rsi, %rax\n"
    "        cmova   %rsi, %rax              # never more than n\n"
    ASM_RET
    "3:      mov     %rsi, %rax\n"
    "9:      RET\n"
    ASM_END(strnlen)
    //
    //       moonwater_ticks -- the machine's own free running counter.
    //
    //       One instruction on every architecture that has it, and no architecture
    //       spells it the same way, which is the smallest honest example of what
    //       .asm files here are for. There is no portable instruction to reach for
    //       and no C that compiles to this, so the choice is a block per machine
    //       or nothing.
    //
    //       What it returns is a hardware tick, not a nanosecond and not a cycle:
    //       a monotonic count at a rate the platform picks, useful for measuring
    //       one span against another and nothing else. The kernel's own
    //       ktime_get() is the right answer for anything that needs a unit; this
    //       is for the places already too hot to call it.
    //
    //       Declared in core.c, beside the code that calls it.
    //
    "        .text\n"
    ASM_FUNC(moonwater_ticks)
    //
    //       rdtsc splits its answer across two 32 bit halves, which is
    //       older than the 64 bit registers it lands in. Writing eax and
    //       edx has already cleared the top of rax and rdx, so putting
    //       them together is a shift and an or.
    //
    //       Unserialized on purpose: lfence first would be the correct
    //       reading of "now" and costs more than the things this is meant
    //       to measure.
    //
    "        rdtsc\n"
    "        shl     $32, %rdx\n"
    "        or      %rdx, %rax\n"
    //
    //       RET, not ret. Under a return thunk mitigation the kernel's
    //       macro is a jump to the thunk instead, and a bare ret in kernel
    //       assembly is the bug that leaves. It comes from asm/linkage.h,
    //       which linux/linkage.h above already pulled in.
    //
    ASM_RET
    ASM_END(moonwater_ticks)
    // memcpy and memset, which were the last byte loops in here.
    //
    // rep movsb and rep stosb are a single instruction the memory controller
    // runs at its own width on anything since Ivy Bridge, and they need no
    // alignment work to get there. Below the point where starting one pays,
    // eight bytes at a time out of a plain loop is cheaper.
    ASM_FUNC(moonwater_fill)
    "        mov     %rdi, %r10\n"
    "        movzbl  %sil, %eax\n"
    "        movabs  $0x0101010101010101, %r9\n"
    "        imul    %r9, %rax\n"
    "        cmp     $256, %rdx\n"
    "        jae     5f\n"
    "        sub     $8, %rdx\n"
    "        jb      2f\n"
    "1:      mov     %rax, (%rdi)\n"
    "        add     $8, %rdi\n"
    "        sub     $8, %rdx\n"
    "        jae     1b\n"
    "2:      add     $8, %rdx\n"
    "        jz      4f\n"
    "3:      mov     %al, (%rdi)\n"
    "        inc     %rdi\n"
    "        dec     %rdx\n"
    "        jnz     3b\n"
    "4:      mov     %r10, %rax\n"
    ASM_RET
    "5:      mov     %rdx, %rcx\n"
    "        shr     $3, %rcx\n"
    "        rep stosq\n"
    "        mov     %edx, %ecx\n"
    "        and     $7, %ecx\n"
    "        rep stosb\n"
    "        mov     %r10, %rax\n"
    ASM_RET
    ASM_END(moonwater_fill)

    ASM_FUNC(moonwater_copy)
    "        mov     %rdi, %rax\n"
    "        cmp     $16, %rdx\n"
    "        jae     6f\n"
    // Under sixteen, two loads that overlap in the middle cover the whole of
    // it with no loop and no tail: the bytes written twice are written the
    // same both times.
    "        cmp     $8, %rdx\n"
    "        jb      7f\n"
    "        mov     (%rsi), %r9\n"
    "        mov     -8(%rsi,%rdx), %r11\n"
    "        mov     %r9, (%rdi)\n"
    "        mov     %r11, -8(%rdi,%rdx)\n"
    ASM_RET
    "7:      cmp     $4, %rdx\n"
    "        jb      8f\n"
    "        mov     (%rsi), %r9d\n"
    "        mov     -4(%rsi,%rdx), %r11d\n"
    "        mov     %r9d, (%rdi)\n"
    "        mov     %r11d, -4(%rdi,%rdx)\n"
    ASM_RET
    "8:      test    %rdx, %rdx\n"
    "        jz      4f\n"
    "3:      mov     (%rsi), %r9b\n"
    "        mov     %r9b, (%rdi)\n"
    "        inc     %rsi\n"
    "        inc     %rdi\n"
    "        dec     %rdx\n"
    "        jnz     3b\n"
    "4:      \n"
    ASM_RET
    "6:      cmp     $256, %rdx\n"
    "        jae     5f\n"
    "1:      mov     (%rsi), %r9\n"
    "        mov     %r9, (%rdi)\n"
    "        add     $8, %rsi\n"
    "        add     $8, %rdi\n"
    "        sub     $8, %rdx\n"
    "        cmp     $8, %rdx\n"
    "        jae     1b\n"
    "        test    %rdx, %rdx\n"
    "        jz      4b\n"
    "        mov     -8(%rsi,%rdx), %r9\n"
    "        mov     %r9, -8(%rdi,%rdx)\n"
    ASM_RET
    "5:      mov     %rdx, %rcx\n"
    "        rep movsb\n"
    ASM_RET
    ASM_END(moonwater_copy)

    // Backwards only where the regions overlap the wrong way. A plain loop
    // rather than the direction flag: the kernel requires it clear on every
    // path out, and one that faults mid-copy would not have cleared it.
    ASM_FUNC(moonwater_move)
    "        mov     %rdi, %rax\n"
    "        cmp     %rsi, %rdi\n"
    "        jbe     1f\n"
    "        mov     %rsi, %r8\n"
    "        add     %rdx, %r8\n"
    "        cmp     %r8, %rdi\n"
    "        jb      2f\n"
    "1:      mov     %rdx, %rcx\n"
    "        rep movsb\n"
    ASM_RET
    "2:      test    %rdx, %rdx\n"
    "        jz      4f\n"
    "        lea     -1(%rdi,%rdx), %rdi\n"
    "        lea     -1(%rsi,%rdx), %rsi\n"
    "3:      mov     (%rsi), %r9b\n"
    "        mov     %r9b, (%rdi)\n"
    "        dec     %rsi\n"
    "        dec     %rdi\n"
    "        dec     %rdx\n"
    "        jnz     3b\n"
    "4:      \n"
    ASM_RET
    ASM_END(moonwater_move)
);
#ifdef KERNEL_MODE
ASM_EXPORT(memchr);
ASM_EXPORT(strchr);
ASM_EXPORT(strchrnul);
ASM_EXPORT(strcmp);
ASM_EXPORT(strlen);
ASM_EXPORT(strnchr);
ASM_EXPORT(strncmp);
ASM_EXPORT(strnlen);
ASM_EXPORT(strrchr);
#endif
#elif ARM64
__asm__(
    //
    //       strlen -- a word at a time.
    //
    //       lib/string.c's strlen is a byte at a time, and on x86_64 nothing
    //       overrides it: arch/x86/include/asm/string_64.h claims memcpy, memmove
    //       and memset and leaves the rest. arm64 and riscv both define
    //       __HAVE_ARCH_STRLEN and ship their own, so this is x86 catching up
    //       rather than x86 being special.
    //
    //       Measured against the generic loop on a 9950X, 4096 calls each:
    //
    //           4 bytes    23048 ticks byte     36120 word
    //           8 bytes    36292 ticks byte     23047 word
    //          16 bytes    62350 ticks byte     26358 word
    //          32 bytes   194317 ticks byte     33067 word
    //          64 bytes   219300 ticks byte     46010 word
    //
    //       The crossover is at eight. Below it the alignment setup costs more
    //       than it saves, above it the win runs to nearly six times.
    //
    //       Reading a whole word that straddles the start of the string is safe:
    //       aligning down stays inside the same page, so the load cannot fault on
    //       memory the caller did not give us. The bytes before the string are
    //       forced to 0xff afterwards so they cannot be mistaken for the
    //       terminator.
    //
    "        .text\n"
    //
    //       arm64 and riscv already define __HAVE_ARCH_STRLEN and ship
    //       their own, so there is nothing here for them to catch up to.
    //
    //
    //      Nothing, deliberately, and this is what "#> arch other" with an
    //      empty block is for.
    //
    //      This file is in src/, so src/Makefile builds it for whichever
    //      architecture the kernel is being configured for. An #error here
    //      -- which is what stood in this place -- does not mean "not
    //      implemented", it means the arm64 and riscv builds stop on the
    //      first of these files they reach.
    //
    //      They do not need it. arm64 and riscv both ship their own, and
    //      where they do not the generic C in lib/string.c is what runs,
    //      exactly as it did before any of this. Emitting nothing leaves
    //      them where they were; the header claim that hands the symbol
    //      over is in build.sh and only ever touches x86's header.
    //
    //
    //       strcmp -- a word at a time, with two pointers and no length.
    //
    //       The hard one, and the reason is worth stating. strlen could align its
    //       pointer down, because an eight byte read aligned to eight never leaves
    //       the page it started in. strncmp and memcmp could read unaligned,
    //       because a length told them how far they were allowed to go. strcmp has
    //       neither: two pointers at whatever alignments the caller chose, and
    //       nothing but a terminator to say where they end. Aligning one down
    //       misaligns the other, and reading unaligned past the terminator can
    //       walk into a page nobody mapped.
    //
    //       What makes it safe is the same fact stated differently: a read of
    //       eight bytes cannot fault if all eight are in a page that already holds
    //       a byte we are allowed to read. The byte at the pointer is such a byte
    //       -- the string has not ended yet, or we would have stopped -- so the
    //       read is safe whenever it does not cross the page boundary, and the
    //       offset within the page says whether it does.
    //
    //       Two compares buy eight bytes of progress. Near a page boundary the
    //       loop steps a byte at a time until it is past, which happens for at
    //       most seven bytes out of every four thousand and ninety six.
    //
    "        .text\n"
    //
    //      Nothing, deliberately, and this is what "#> arch other" with an
    //      empty block is for.
    //
    //      This file is in src/, so src/Makefile builds it for whichever
    //      architecture the kernel is being configured for. An #error here
    //      -- which is what stood in this place -- does not mean "not
    //      implemented", it means the arm64 and riscv builds stop on the
    //      first of these files they reach.
    //
    //      They do not need it. arm64 and riscv both ship their own, and
    //      where they do not the generic C in lib/string.c is what runs,
    //      exactly as it did before any of this. Emitting nothing leaves
    //      them where they were; the header claim that hands the symbol
    //      over is in build.sh and only ever touches x86's header.
    //
    //
    //       strchr and memchr -- a word at a time.
    //
    //       The last two byte loops in lib/string.c worth taking. Both hunt for a
    //       byte, so both broadcast it across a word and reuse the trick that finds
    //       a zero byte: after exclusive-or with the broadcast, the byte that
    //       matched is the byte that is now zero.
    //
    //       An eight byte load aligned to eight never crosses a page, so reading
    //       the word that contains the string's first byte cannot fault on memory
    //       the caller does not own. The matches in the bytes before the string are
    //       thrown away afterwards by masking the result rather than the input,
    //       which keeps the byte being searched for out of it -- forcing those
    //       bytes to 0xff would false-match a search for 0xff.
    //
    "        .text\n"
    //
    //       char *strchr(const char *s, int c)
    //
    //       Two hunts at once: the byte, and the terminator that ends the search.
    //       Whichever comes first in the word is the answer, and it is a hit only
    //       if that byte is the one asked for -- which is also how strchr(s, 0)
    //       returns the terminator rather than nothing.
    //
    //
    //      Nothing, deliberately, and this is what "#> arch other" with an
    //      empty block is for.
    //
    //      This file is in src/, so src/Makefile builds it for whichever
    //      architecture the kernel is being configured for. An #error here
    //      -- which is what stood in this place -- does not mean "not
    //      implemented", it means the arm64 and riscv builds stop on the
    //      first of these files they reach.
    //
    //      They do not need it. arm64 and riscv both ship their own, and
    //      where they do not the generic C in lib/string.c is what runs,
    //      exactly as it did before any of this. Emitting nothing leaves
    //      them where they were; the header claim that hands the symbol
    //      over is in build.sh and only ever touches x86's header.
    //
    //
    //       void *memchr(const void *s, int c, size_t n)
    //
    //       The same hunt with a fence instead of a terminator. A match found in
    //       the word that reaches past the bound is discarded by comparing its
    //       address, which is cheaper than stopping the scan short.
    //
    // As above: nothing here on purpose.
    //
    //       strchrnul, strnchr and strrchr -- the rest of the byte hunts.
    //
    //       All three are strchr with one thing changed: what to return when the
    //       byte is absent, where to stop, and which match to keep. The machinery
    //       underneath is the one strchr already uses -- broadcast the byte across
    //       a word, exclusive-or, and the byte that matched is the byte that is
    //       now zero -- so what follows is mostly the differences.
    //
    "        .text\n"
    //
    //       char *strchrnul(const char *s, int c)
    //
    //       strchr that answers with the terminator instead of nothing. Since the
    //       scan already stops at whichever of the two comes first, that is the
    //       same code without the last test.
    //
    ASM_FUNC(strchrnul)
    "        and     w1, w1, #0xff\n"
    "        mov     x10, #0x0101\n"
    "        movk    x10, #0x0101, lsl #16\n"
    "        movk    x10, #0x0101, lsl #32\n"
    "        movk    x10, #0x0101, lsl #48   // 0x0101010101010101\n"
    "        lsl     x11, x10, #7            // 0x8080808080808080\n"
    "        mul     x3, x1, x10             // the byte, in all eight positions\n"
    "        and     x4, x0, #7              // how far into the word it begins\n"
    "        bic     x5, x0, #7              // align down: same page, cannot fault\n"
    "        ldr     x6, [x5]\n"
    "        lsl     x4, x4, #3              // bytes -> bits\n"
    "        mov     x7, #-1\n"
    "        lsl     x7, x7, x4              // which bytes of the first word count\n"
    "1:      eor     x8, x6, x3              // the byte that matched is now zero\n"
    "        sub     x9, x8, x10\n"
    "        bic     x9, x9, x8\n"
    "        and     x9, x9, x11\n"
    "        sub     x12, x6, x10            // and the terminator, the same way\n"
    "        bic     x12, x12, x6\n"
    "        and     x12, x12, x11\n"
    "        orr     x9, x9, x12\n"
    "        and     x9, x9, x7\n"
    "        cbnz    x9, 2f\n"
    "        mov     x7, #-1                 // every byte of every later word counts\n"
    "        add     x5, x5, #8\n"
    "        ldr     x6, [x5]\n"
    "        b       1b\n"
    "2:      rbit    x9, x9\n"
    "        clz     x9, x9                  // first set high bit\n"
    "        lsr     x9, x9, #3              // its byte within the word\n"
    "        add     x0, x5, x9\n"
    "        ret\n"
    ASM_END(strchrnul)
    //
    //       char *strnchr(const char *s, size_t count, int c)
    //
    //       strchr with a fence. Note the argument order: the count comes second
    //       and the byte third, which is the opposite way round from memchr and
    //       is the kind of thing that is only wrong once.
    //
    ASM_FUNC(strnchr)
    "        mov     x9, #0\n"
    "        cbz     x1, 9f\n"
    "        and     w2, w2, #0xff\n"
    "        mov     x10, #0x0101\n"
    "        movk    x10, #0x0101, lsl #16\n"
    "        movk    x10, #0x0101, lsl #32\n"
    "        movk    x10, #0x0101, lsl #48\n"
    "        lsl     x11, x10, #7\n"
    "        mul     x3, x2, x10\n"
    "        add     x13, x0, x1             // one past the last byte we may report\n"
    "        and     x4, x0, #7\n"
    "        bic     x5, x0, #7\n"
    "        ldr     x6, [x5]\n"
    "        lsl     x4, x4, #3\n"
    "        mov     x7, #-1\n"
    "        lsl     x7, x7, x4\n"
    "1:      eor     x8, x6, x3\n"
    "        sub     x9, x8, x10\n"
    "        bic     x9, x9, x8\n"
    "        and     x9, x9, x11\n"
    "        sub     x12, x6, x10\n"
    "        bic     x12, x12, x6\n"
    "        and     x12, x12, x11\n"
    "        orr     x9, x9, x12\n"
    "        and     x9, x9, x7\n"
    "        cbnz    x9, 2f\n"
    "        mov     x7, #-1\n"
    "        add     x5, x5, #8\n"
    "        cmp     x5, x13\n"
    "        b.hs    8f\n"
    "        ldr     x6, [x5]\n"
    "        b       1b\n"
    "2:      rbit    x9, x9\n"
    "        clz     x9, x9\n"
    "        lsr     x9, x9, #3\n"
    "        add     x9, x5, x9\n"
    "        cmp     x9, x13\n"
    "        b.hs    8f                      // beyond the count\n"
    "        ldrb    w4, [x9]\n"
    "        cmp     w4, w2\n"
    "        b.eq    9f                      // it was the byte, not the terminator\n"
    "8:      mov     x9, #0\n"
    "9:      mov     x0, x9\n"
    "        ret\n"
    ASM_END(strnchr)
    //
    //       char *strrchr(const char *s, int c)
    //
    //       The last match rather than the first, which the forward scan does not
    //       answer directly: within a word the highest set bit is wanted, not the
    //       lowest, so bsr where the others use bsf. A word without a terminator
    //       may hold a later match than anything before it, so the best so far is
    //       carried along; the word that holds the terminator only counts matches
    //       below it.
    //
    //       Searching for the terminator itself is a byte walk. It is the one case
    //       where the answer is the end of the string rather than a match inside
    //       it, and it is rare enough not to be worth its own scan.
    //
    // As above: nothing here on purpose.
    //
    //       strncmp and strnlen -- a word at a time.
    //
    //       Both are byte loops in lib/string.c and neither is overridden on
    //       x86_64. Two functions in one file, which the dialect allows now: a
    //       #> shared closes the run of blocks before it and the next #> arch
    //       opens a new one.
    //
    //       An eight byte load aligned to eight never crosses a page, so reading
    //       the word that contains a pointer can never fault on memory the caller
    //       did not give us. That is what makes the unbounded scan safe; where a
    //       length is given the reads are bounded anyway.
    //
    "        .text\n"
    //
    //       int strncmp(const char *a, const char *b, size_t n)
    //
    //       Eight bytes from each, unaligned, which is legal here because n bounds
    //       the read. Equal and no terminator in them means advance; anything else
    //       hands those eight to the byte loop, which already knows how to stop on
    //       a difference or a terminator and gets the sign right. The difference is
    //       found once per call, so simple beats clever there.
    //
    //
    //      Nothing, deliberately, and this is what "#> arch other" with an
    //      empty block is for.
    //
    //      This file is in src/, so src/Makefile builds it for whichever
    //      architecture the kernel is being configured for. An #error here
    //      -- which is what stood in this place -- does not mean "not
    //      implemented", it means the arm64 and riscv builds stop on the
    //      first of these files they reach.
    //
    //      They do not need it. arm64 and riscv both ship their own, and
    //      where they do not the generic C in lib/string.c is what runs,
    //      exactly as it did before any of this. Emitting nothing leaves
    //      them where they were; the header claim that hands the symbol
    //      over is in build.sh and only ever touches x86's header.
    //
    //
    //       size_t strnlen(const char *s, size_t n)
    //
    //       strlen with a fence. The scan is the same -- align down, force the
    //       bytes before the string non-zero, then look for a zero byte eight at a
    //       time -- and a terminator found beyond n is clamped back to n, which is
    //       what strnlen returns when there is none inside the bound.
    //
    // As above: nothing here on purpose.
    //
    //       moonwater_ticks -- the machine's own free running counter.
    //
    //       One instruction on every architecture that has it, and no architecture
    //       spells it the same way, which is the smallest honest example of what
    //       .asm files here are for. There is no portable instruction to reach for
    //       and no C that compiles to this, so the choice is a block per machine
    //       or nothing.
    //
    //       What it returns is a hardware tick, not a nanosecond and not a cycle:
    //       a monotonic count at a rate the platform picks, useful for measuring
    //       one span against another and nothing else. The kernel's own
    //       ktime_get() is the right answer for anything that needs a unit; this
    //       is for the places already too hot to call it.
    //
    //       Declared in core.c, beside the code that calls it.
    //
    "        .text\n"
    ASM_FUNC(moonwater_ticks)
    //
    //       The virtual counter: fixed rate, readable at EL0 and EL1, and
    //       what the kernel's own arch_timer reads.
    //
    "        mrs     x0, cntvct_el0\n"
    "        ret\n"
    ASM_END(moonwater_ticks)
    // memcpy and memset, which were the last byte loops in here.
    ASM_FUNC(moonwater_fill)
    "        mov     x3, x0\n"
    "        and     w1, w1, #0xff\n"
    "        orr     w1, w1, w1, lsl #8\n"
    "        orr     w1, w1, w1, lsl #16\n"
    "        orr     x1, x1, x1, lsl #32\n"
    "1:      cmp     x2, #16\n"
    "        b.lo    2f\n"
    "        stp     x1, x1, [x0], #16\n"
    "        sub     x2, x2, #16\n"
    "        b       1b\n"
    "2:      cmp     x2, #8\n"
    "        b.lo    3f\n"
    "        str     x1, [x0], #8\n"
    "        sub     x2, x2, #8\n"
    "3:      cbz     x2, 4f\n"
    "        strb    w1, [x0], #1\n"
    "        sub     x2, x2, #1\n"
    "        b       3b\n"
    "4:      mov     x0, x3\n"
    ASM_RET
    ASM_END(moonwater_fill)

    ASM_FUNC(moonwater_copy)
    "        mov     x3, x0\n"
    "1:      cmp     x2, #16\n"
    "        b.lo    2f\n"
    "        ldp     x4, x5, [x1], #16\n"
    "        stp     x4, x5, [x0], #16\n"
    "        sub     x2, x2, #16\n"
    "        b       1b\n"
    "2:      cmp     x2, #8\n"
    "        b.lo    3f\n"
    "        ldr     x4, [x1], #8\n"
    "        str     x4, [x0], #8\n"
    "        sub     x2, x2, #8\n"
    "3:      cbz     x2, 4f\n"
    "        ldrb    w4, [x1], #1\n"
    "        strb    w4, [x0], #1\n"
    "        sub     x2, x2, #1\n"
    "        b       3b\n"
    "4:      mov     x0, x3\n"
    ASM_RET
    ASM_END(moonwater_copy)

    ASM_FUNC(moonwater_move)
    "        mov     x3, x0\n"
    "        cmp     x0, x1\n"
    "        b.ls    5f\n"
    "        add     x4, x1, x2\n"
    "        cmp     x0, x4\n"
    "        b.hs    5f\n"
    "        add     x0, x0, x2\n"
    "        add     x1, x1, x2\n"
    "6:      cbz     x2, 7f\n"
    "        ldrb    w4, [x1, #-1]!\n"
    "        strb    w4, [x0, #-1]!\n"
    "        sub     x2, x2, #1\n"
    "        b       6b\n"
    "7:      mov     x0, x3\n"
    ASM_RET
    "5:      cbz     x2, 8f\n"
    "        ldrb    w4, [x1], #1\n"
    "        strb    w4, [x0], #1\n"
    "        sub     x2, x2, #1\n"
    "        b       5b\n"
    "8:      mov     x0, x3\n"
    ASM_RET
    ASM_END(moonwater_move)
);
#ifdef KERNEL_MODE
ASM_EXPORT(strchrnul);
ASM_EXPORT(strnchr);
#endif
#elif RISCV64
__asm__(
    //
    //       strlen -- a word at a time.
    //
    //       lib/string.c's strlen is a byte at a time, and on x86_64 nothing
    //       overrides it: arch/x86/include/asm/string_64.h claims memcpy, memmove
    //       and memset and leaves the rest. arm64 and riscv both define
    //       __HAVE_ARCH_STRLEN and ship their own, so this is x86 catching up
    //       rather than x86 being special.
    //
    //       Measured against the generic loop on a 9950X, 4096 calls each:
    //
    //           4 bytes    23048 ticks byte     36120 word
    //           8 bytes    36292 ticks byte     23047 word
    //          16 bytes    62350 ticks byte     26358 word
    //          32 bytes   194317 ticks byte     33067 word
    //          64 bytes   219300 ticks byte     46010 word
    //
    //       The crossover is at eight. Below it the alignment setup costs more
    //       than it saves, above it the win runs to nearly six times.
    //
    //       Reading a whole word that straddles the start of the string is safe:
    //       aligning down stays inside the same page, so the load cannot fault on
    //       memory the caller did not give us. The bytes before the string are
    //       forced to 0xff afterwards so they cannot be mistaken for the
    //       terminator.
    //
    "        .text\n"
    //
    //       arm64 and riscv already define __HAVE_ARCH_STRLEN and ship
    //       their own, so there is nothing here for them to catch up to.
    //
    //
    //      Nothing, deliberately, and this is what "#> arch other" with an
    //      empty block is for.
    //
    //      This file is in src/, so src/Makefile builds it for whichever
    //      architecture the kernel is being configured for. An #error here
    //      -- which is what stood in this place -- does not mean "not
    //      implemented", it means the arm64 and riscv builds stop on the
    //      first of these files they reach.
    //
    //      They do not need it. arm64 and riscv both ship their own, and
    //      where they do not the generic C in lib/string.c is what runs,
    //      exactly as it did before any of this. Emitting nothing leaves
    //      them where they were; the header claim that hands the symbol
    //      over is in build.sh and only ever touches x86's header.
    //
    //
    //       strcmp -- a word at a time, with two pointers and no length.
    //
    //       The hard one, and the reason is worth stating. strlen could align its
    //       pointer down, because an eight byte read aligned to eight never leaves
    //       the page it started in. strncmp and memcmp could read unaligned,
    //       because a length told them how far they were allowed to go. strcmp has
    //       neither: two pointers at whatever alignments the caller chose, and
    //       nothing but a terminator to say where they end. Aligning one down
    //       misaligns the other, and reading unaligned past the terminator can
    //       walk into a page nobody mapped.
    //
    //       What makes it safe is the same fact stated differently: a read of
    //       eight bytes cannot fault if all eight are in a page that already holds
    //       a byte we are allowed to read. The byte at the pointer is such a byte
    //       -- the string has not ended yet, or we would have stopped -- so the
    //       read is safe whenever it does not cross the page boundary, and the
    //       offset within the page says whether it does.
    //
    //       Two compares buy eight bytes of progress. Near a page boundary the
    //       loop steps a byte at a time until it is past, which happens for at
    //       most seven bytes out of every four thousand and ninety six.
    //
    "        .text\n"
    //
    //      Nothing, deliberately, and this is what "#> arch other" with an
    //      empty block is for.
    //
    //      This file is in src/, so src/Makefile builds it for whichever
    //      architecture the kernel is being configured for. An #error here
    //      -- which is what stood in this place -- does not mean "not
    //      implemented", it means the arm64 and riscv builds stop on the
    //      first of these files they reach.
    //
    //      They do not need it. arm64 and riscv both ship their own, and
    //      where they do not the generic C in lib/string.c is what runs,
    //      exactly as it did before any of this. Emitting nothing leaves
    //      them where they were; the header claim that hands the symbol
    //      over is in build.sh and only ever touches x86's header.
    //
    //
    //       strchr and memchr -- a word at a time.
    //
    //       The last two byte loops in lib/string.c worth taking. Both hunt for a
    //       byte, so both broadcast it across a word and reuse the trick that finds
    //       a zero byte: after exclusive-or with the broadcast, the byte that
    //       matched is the byte that is now zero.
    //
    //       An eight byte load aligned to eight never crosses a page, so reading
    //       the word that contains the string's first byte cannot fault on memory
    //       the caller does not own. The matches in the bytes before the string are
    //       thrown away afterwards by masking the result rather than the input,
    //       which keeps the byte being searched for out of it -- forcing those
    //       bytes to 0xff would false-match a search for 0xff.
    //
    "        .text\n"
    //
    //       char *strchr(const char *s, int c)
    //
    //       Two hunts at once: the byte, and the terminator that ends the search.
    //       Whichever comes first in the word is the answer, and it is a hit only
    //       if that byte is the one asked for -- which is also how strchr(s, 0)
    //       returns the terminator rather than nothing.
    //
    //
    //      Nothing, deliberately, and this is what "#> arch other" with an
    //      empty block is for.
    //
    //      This file is in src/, so src/Makefile builds it for whichever
    //      architecture the kernel is being configured for. An #error here
    //      -- which is what stood in this place -- does not mean "not
    //      implemented", it means the arm64 and riscv builds stop on the
    //      first of these files they reach.
    //
    //      They do not need it. arm64 and riscv both ship their own, and
    //      where they do not the generic C in lib/string.c is what runs,
    //      exactly as it did before any of this. Emitting nothing leaves
    //      them where they were; the header claim that hands the symbol
    //      over is in build.sh and only ever touches x86's header.
    //
    //
    //       void *memchr(const void *s, int c, size_t n)
    //
    //       The same hunt with a fence instead of a terminator. A match found in
    //       the word that reaches past the bound is discarded by comparing its
    //       address, which is cheaper than stopping the scan short.
    //
    //
    //      Nothing, and not for lack of headroom: riscv does run the byte
    //      loop here. arch/riscv/kernel/pi compiles its own copy of
    //      lib/string.c for the code that runs before the MMU, objcopies
    //      it to __pi_, and libfdt in there calls memchr -- so taking
    //      memchr away from lib/string.c takes __pi_memchr away with it
    //      and vmlinux does not link. See the riscv arm of the claim in
    //      build.sh.
    //
    //
    //       strchrnul, strnchr and strrchr -- the rest of the byte hunts.
    //
    //       All three are strchr with one thing changed: what to return when the
    //       byte is absent, where to stop, and which match to keep. The machinery
    //       underneath is the one strchr already uses -- broadcast the byte across
    //       a word, exclusive-or, and the byte that matched is the byte that is
    //       now zero -- so what follows is mostly the differences.
    //
    "        .text\n"
    //
    //       char *strchrnul(const char *s, int c)
    //
    //       strchr that answers with the terminator instead of nothing. Since the
    //       scan already stops at whichever of the two comes first, that is the
    //       same code without the last test.
    //
    //
    //      Where x86 has bsf and arm64 has rbit+clz, base rv64 has
    //      neither: ctz is Zbb, and QEMU's virt machine does not have it
    //      ("riscv: base ISA extensions acdfhim"), so requiring it would
    //      mean an illegal instruction on the machine this is developed
    //      on. So the byte index comes out of the multiply the M
    //      extension already guarantees:
    //
    //          x & -x                  keep only the lowest set bit
    //          - 1                     ones below it
    //          & 0x0101..01            one per byte below it
    //          * 0x0101..01 >> 56      count them, since each contributes
    //                                  1 to the top byte and there are at
    //                                  most eight
    //          - 1                     that count is the index plus one
    //
    //      Seven instructions where Zbb would take two, and only on the
    //      way out. The alternative was a floor that cannot be tested.
    //
    ASM_FUNC(strchrnul)
    "        andi    a1, a1, 0xff\n"
    "        li      t0, 0x0101010101010101\n"
    "        slli    t1, t0, 7               # 0x8080808080808080\n"
    "        mul     a3, a1, t0              # the byte, in all eight positions\n"
    "        andi    a4, a0, 7               # how far into the word it begins\n"
    "        andi    a5, a0, -8              # align down: same page, cannot fault\n"
    "        ld      a6, 0(a5)\n"
    "        slli    a4, a4, 3               # bytes -> bits\n"
    "        li      a7, -1\n"
    "        sll     a7, a7, a4              # which bytes of the first word count\n"
    "1:      xor     t2, a6, a3              # the byte that matched is now zero\n"
    "        sub     t3, t2, t0\n"
    "        not     t4, t2\n"
    "        and     t3, t3, t4\n"
    "        and     t3, t3, t1\n"
    "        sub     t5, a6, t0              # and the terminator, the same way\n"
    "        not     t6, a6\n"
    "        and     t5, t5, t6\n"
    "        and     t5, t5, t1\n"
    "        or      t3, t3, t5\n"
    "        and     t3, t3, a7\n"
    "        bnez    t3, 2f\n"
    "        li      a7, -1                  # every byte of every later word counts\n"
    "        addi    a5, a5, 8\n"
    "        ld      a6, 0(a5)\n"
    "        j       1b\n"
    "2:      sub     t5, zero, t3\n"
    "        and     t3, t3, t5              # lowest set high bit\n"
    "        addi    t3, t3, -1\n"
    "        and     t3, t3, t0\n"
    "        mul     t3, t3, t0\n"
    "        srli    t3, t3, 56\n"
    "        addi    t3, t3, -1              # its byte within the word\n"
    "        add     a0, a5, t3\n"
    "        ret\n"
    ASM_END(strchrnul)
    //
    //       char *strnchr(const char *s, size_t count, int c)
    //
    //       strchr with a fence. Note the argument order: the count comes second
    //       and the byte third, which is the opposite way round from memchr and
    //       is the kind of thing that is only wrong once.
    //
    //      The same count-the-bytes-below sequence as strchrnul above.
    ASM_FUNC(strnchr)
    "        beqz    a1, 8f\n"
    "        andi    a2, a2, 0xff\n"
    "        li      t0, 0x0101010101010101\n"
    "        slli    t1, t0, 7\n"
    "        mul     a3, a2, t0\n"
    "        add     a4, a0, a1              # one past the last byte we may report\n"
    "        andi    t2, a0, 7\n"
    "        andi    a5, a0, -8\n"
    "        ld      a6, 0(a5)\n"
    "        slli    t2, t2, 3\n"
    "        li      a7, -1\n"
    "        sll     a7, a7, t2\n"
    "1:      xor     t3, a6, a3\n"
    "        sub     t4, t3, t0\n"
    "        not     t5, t3\n"
    "        and     t4, t4, t5\n"
    "        and     t4, t4, t1\n"
    "        sub     t6, a6, t0\n"
    "        not     t2, a6\n"
    "        and     t6, t6, t2\n"
    "        and     t6, t6, t1\n"
    "        or      t4, t4, t6\n"
    "        and     t4, t4, a7\n"
    "        bnez    t4, 2f\n"
    "        li      a7, -1\n"
    "        addi    a5, a5, 8\n"
    "        bgeu    a5, a4, 8f\n"
    "        ld      a6, 0(a5)\n"
    "        j       1b\n"
    "2:      sub     t5, zero, t4\n"
    "        and     t4, t4, t5              # lowest set high bit\n"
    "        addi    t4, t4, -1\n"
    "        and     t4, t4, t0\n"
    "        mul     t4, t4, t0\n"
    "        srli    t4, t4, 56\n"
    "        addi    t4, t4, -1              # its byte within the word\n"
    "        add     t4, a5, t4\n"
    "        bgeu    t4, a4, 8f              # beyond the count\n"
    "        lbu     t5, 0(t4)\n"
    "        bne     t5, a2, 8f              # the terminator, not the byte\n"
    "        mv      a0, t4\n"
    "        ret\n"
    "8:      li      a0, 0\n"
    "        ret\n"
    ASM_END(strnchr)
    //
    //       char *strrchr(const char *s, int c)
    //
    //       The last match rather than the first, which the forward scan does not
    //       answer directly: within a word the highest set bit is wanted, not the
    //       lowest, so bsr where the others use bsf. A word without a terminator
    //       may hold a later match than anything before it, so the best so far is
    //       carried along; the word that holds the terminator only counts matches
    //       below it.
    //
    //       Searching for the terminator itself is a byte walk. It is the one case
    //       where the answer is the end of the string rather than a match inside
    //       it, and it is rare enough not to be worth its own scan.
    //
    // As above: nothing here on purpose.
    //
    //       strncmp and strnlen -- a word at a time.
    //
    //       Both are byte loops in lib/string.c and neither is overridden on
    //       x86_64. Two functions in one file, which the dialect allows now: a
    //       #> shared closes the run of blocks before it and the next #> arch
    //       opens a new one.
    //
    //       An eight byte load aligned to eight never crosses a page, so reading
    //       the word that contains a pointer can never fault on memory the caller
    //       did not give us. That is what makes the unbounded scan safe; where a
    //       length is given the reads are bounded anyway.
    //
    "        .text\n"
    //
    //       int strncmp(const char *a, const char *b, size_t n)
    //
    //       Eight bytes from each, unaligned, which is legal here because n bounds
    //       the read. Equal and no terminator in them means advance; anything else
    //       hands those eight to the byte loop, which already knows how to stop on
    //       a difference or a terminator and gets the sign right. The difference is
    //       found once per call, so simple beats clever there.
    //
    //
    //      Nothing, deliberately, and this is what "#> arch other" with an
    //      empty block is for.
    //
    //      This file is in src/, so src/Makefile builds it for whichever
    //      architecture the kernel is being configured for. An #error here
    //      -- which is what stood in this place -- does not mean "not
    //      implemented", it means the arm64 and riscv builds stop on the
    //      first of these files they reach.
    //
    //      They do not need it. arm64 and riscv both ship their own, and
    //      where they do not the generic C in lib/string.c is what runs,
    //      exactly as it did before any of this. Emitting nothing leaves
    //      them where they were; the header claim that hands the symbol
    //      over is in build.sh and only ever touches x86's header.
    //
    //
    //       size_t strnlen(const char *s, size_t n)
    //
    //       strlen with a fence. The scan is the same -- align down, force the
    //       bytes before the string non-zero, then look for a zero byte eight at a
    //       time -- and a terminator found beyond n is clamped back to n, which is
    //       what strnlen returns when there is none inside the bound.
    //
    // As above: nothing here on purpose.
    //
    //       moonwater_ticks -- the machine's own free running counter.
    //
    //       One instruction on every architecture that has it, and no architecture
    //       spells it the same way, which is the smallest honest example of what
    //       .asm files here are for. There is no portable instruction to reach for
    //       and no C that compiles to this, so the choice is a block per machine
    //       or nothing.
    //
    //       What it returns is a hardware tick, not a nanosecond and not a cycle:
    //       a monotonic count at a rate the platform picks, useful for measuring
    //       one span against another and nothing else. The kernel's own
    //       ktime_get() is the right answer for anything that needs a unit; this
    //       is for the places already too hot to call it.
    //
    //       Declared in core.c, beside the code that calls it.
    //
    "        .text\n"
    ASM_FUNC(moonwater_ticks)
    //
    //       The time CSR is the fixed rate one and matches what the other
    //       two return; cycle would count clock ticks and change rate with
    //       frequency scaling.
    //
    "        csrr    a0, time\n"
    "        ret\n"
    ASM_END(moonwater_ticks)
    // memcpy and memset, which were the last byte loops in here.
    ASM_FUNC(moonwater_fill)
    "        mv      a3, a0\n"
    "        andi    a1, a1, 0xff\n"
    "        slli    t0, a1, 8\n"
    "        or      a1, a1, t0\n"
    "        slli    t0, a1, 16\n"
    "        or      a1, a1, t0\n"
    "        slli    t0, a1, 32\n"
    "        or      a1, a1, t0\n"
    "        li      t1, 8\n"
    "1:      blt     a2, t1, 2f\n"
    "        sd      a1, 0(a0)\n"
    "        addi    a0, a0, 8\n"
    "        addi    a2, a2, -8\n"
    "        j       1b\n"
    "2:      beqz    a2, 3f\n"
    "        sb      a1, 0(a0)\n"
    "        addi    a0, a0, 1\n"
    "        addi    a2, a2, -1\n"
    "        j       2b\n"
    "3:      mv      a0, a3\n"
    ASM_RET
    ASM_END(moonwater_fill)

    ASM_FUNC(moonwater_copy)
    "        mv      a3, a0\n"
    "        li      t1, 8\n"
    "1:      blt     a2, t1, 2f\n"
    "        ld      t0, 0(a1)\n"
    "        sd      t0, 0(a0)\n"
    "        addi    a1, a1, 8\n"
    "        addi    a0, a0, 8\n"
    "        addi    a2, a2, -8\n"
    "        j       1b\n"
    "2:      beqz    a2, 3f\n"
    "        lbu     t0, 0(a1)\n"
    "        sb      t0, 0(a0)\n"
    "        addi    a1, a1, 1\n"
    "        addi    a0, a0, 1\n"
    "        addi    a2, a2, -1\n"
    "        j       2b\n"
    "3:      mv      a0, a3\n"
    ASM_RET
    ASM_END(moonwater_copy)

    ASM_FUNC(moonwater_move)
    "        mv      a3, a0\n"
    "        bleu    a0, a1, 5f\n"
    "        add     t0, a1, a2\n"
    "        bgeu    a0, t0, 5f\n"
    "        add     a0, a0, a2\n"
    "        add     a1, a1, a2\n"
    "6:      beqz    a2, 7f\n"
    "        addi    a1, a1, -1\n"
    "        addi    a0, a0, -1\n"
    "        lbu     t0, 0(a1)\n"
    "        sb      t0, 0(a0)\n"
    "        addi    a2, a2, -1\n"
    "        j       6b\n"
    "7:      mv      a0, a3\n"
    ASM_RET
    "5:      beqz    a2, 8f\n"
    "        lbu     t0, 0(a1)\n"
    "        sb      t0, 0(a0)\n"
    "        addi    a1, a1, 1\n"
    "        addi    a0, a0, 1\n"
    "        addi    a2, a2, -1\n"
    "        j       5b\n"
    "8:      mv      a0, a3\n"
    ASM_RET
    ASM_END(moonwater_move)
);
#ifdef KERNEL_MODE
ASM_EXPORT(strchrnul);
ASM_EXPORT(strnchr);
#endif
#endif

// What the block above actually defines here. Each architecture only
// carries what it was missing, so this is not the same set everywhere.
#if X64
#define MOONWATER_HAVE_MEMCHR 1
#define MOONWATER_HAVE_MOONWATER_TICKS 1
#define MOONWATER_HAVE_STRCHR 1
#define MOONWATER_HAVE_STRCHRNUL 1
#define MOONWATER_HAVE_STRCMP 1
#define MOONWATER_HAVE_STRLEN 1
#define MOONWATER_HAVE_STRNCHR 1
#define MOONWATER_HAVE_STRNCMP 1
#define MOONWATER_HAVE_STRNLEN 1
#define MOONWATER_HAVE_STRRCHR 1
#endif
#if ARM64
#define MOONWATER_HAVE_MOONWATER_TICKS 1
#define MOONWATER_HAVE_STRCHRNUL 1
#define MOONWATER_HAVE_STRNCHR 1
#endif
#if RISCV64
#define MOONWATER_HAVE_MOONWATER_TICKS 1
#define MOONWATER_HAVE_STRCHRNUL 1
#define MOONWATER_HAVE_STRNCHR 1
#endif

address_any moonwater_fill(address_any destination, b8 value, positive size);
address_any moonwater_copy(address_any destination, address_any source, positive size);
address_any moonwater_move(address_any destination, address_any source, positive size);

// ### Fill a memory block with the same value
// fills a memory block with the same value
// returns: destination address
// destination: the memory block to fill
// traditional: memset
address_any memory_fill(address_any destination, b8 value, positive size)
{
        return moonwater_fill(destination, value, size);
}

// ### Fill source memory block with destination memory block
// copies a memory block from source to destination
// returns: destination address
// destination: the memory block to copy to
// source: the memory block to copy from
// traditional: memcpy
address_any memory_copy(address_any destination, address_any source, positive size)
{
        return moonwater_move(destination, source, size);
}

// ### Fast memory copy
// copies a memory block from source to destination but dosn't handle overlapping regions
address_any memory_copy_fast(address_any destination, address_any source, positive size)
{
        return moonwater_copy(destination, source, size);
}

/*
        Declared here only where nothing else has.

        In the kernel these are the machine's own string functions and
        linux/string.h has already said what they look like; saying it again
        with this file's own typedefs is one name with two shapes, and the
        compiler is right to refuse it.
*/
#ifndef KERNEL_MODE
unsigned long strlen(const char address_to source);
int strcmp(const char address_to source, const char address_to input);
char address_to strchr(const char address_to source, int character);
#endif

#ifdef MOONWATER_HAVE_STRLEN

positive string_length(string_address source)
{
        return (positive)strlen((const char address_to)source);
}

#else

// ### Length of string segment in linear memory
// returns the length of a string terminated by a null character
// NOT a entire array length
// a string array can hold more than one string, null terminators
// are used to separate strings, so where you run strlen is important
// traditional: strlen
positive string_length(string_address source)
{
        string_address step = source;

        while (string_get(step))
                step++;

        return step - source;
}

#endif


#ifdef MOONWATER_HAVE_STRCMP

b32 string_compare(string_address source, string_address input)
{
        return (b32)strcmp((const char address_to)source, (const char address_to)input);
}

#else

// ### Compare two string segments
// returns: 0 - if strings are equal
// returns: positive number - if first string is greater
// returns: negative number - if second string is greater
// traditional: strcmp
b32 string_compare(string_address source, string_address input)
{
        while (string_get(source) && string_get(input))
        {
                if string_not (source, address_to input)
                        break;

                source++;
                input++;
        }

        return string_get(source) - string_get(input);
}

#endif

// ### Copy string segment
// copies a string segment from source to destination
// returns: destination address
// destination: the memory block to copy to
// source: the memory block to copy from
// traditional: strcpy
string_address string_copy(string_address destination, string_address source)
{
        string_address start = destination;

        while (string_get(source))
                string_set(destination++, string_get(source++));

        string_set(destination, end);

        return start;
}

// ### Copy string segment with a maximum length
// traditional: strncpy
string_address string_copy_max(string_address destination, string_address source, positive length)
{
        string_address start = destination;

        // length-- in the condition also decrements on the failing test, which
        // underflows a positive when the loop never runs.
        while (length && string_get(source))
        {
                string_set(destination++, string_get(source++));
                length--;
        }

        // Terminate only when the source ended inside the limit. Writing
        // unconditionally puts the terminator at destination[length], one byte
        // past the bound the caller asked us to stay within.
        if (length)
                string_set(destination, end);

        return start;
}

// ### Find first character in string segment
// returns: address of the first occurrence of the character
// returns: null if the character is not found
// source: the memory block to search
// character: the character to search for
// traditional: strchr
string_address string_first_of(string_address source, p8 character)
{
#ifdef MOONWATER_HAVE_STRCHR
        // Same answer at the terminator: strchr(s, 0) returns it rather than
        // nothing, which is what the line under the #else does too.
        return (string_address)strchr((const char address_to)source, character);
#else
        while (string_get(source))
        {
                if string_is (source, character)
                        return source;

                source++;
        }

        return (string_get(source) == character) ? source : null;
#endif
}

// ### Find last character in string segment
// returns: address of the last occurrence of the character
// returns: null if the character is not found
// source: the memory block to search
// character: the character to search for
// traditional: strrchr
//
// Not the strrchr in src/chrn_word.asm, which is a different function at one
// input: strrchr(source, 0) is the terminator there and null here.
string_address string_last_of(string_address source, p8 character)
{
        string_address last = null;

        while (string_get(source))
        {
                if string_is (source, character)
                        last = source;

                source++;
        }

        return last;
}

// Performs a single cut forward in a string by inserting a null terminator where the FIRST cut symbol is found.
// Returns the address AFTER the cut, effectively splitting the string into two parts.
// searching starts from the beginning of the string,
// linearly steps forward until a cut symbol is found or a string end is reached.
//
// # example:
//      string_address input = "Hello World";
//      string_address second_part = string_cut(input, ' ');
//      // input = "Hello"
//      // second_part = "World"
string_address string_cut(string_address string, b8 cut_symbol)
{
        string_address step = string;

        while (string_get(step))
        {
                if (string_is(step, cut_symbol))
                {
                        string_set(step, end);
                        step++;
                        return string_get(step) ? step : null;
                }
                step++;
        }

        return null;
}

// returns the the start address of the first occurrence input, null if not found
string_address string_find(string_address string, string_address input)
{
        string_address step = string;
        string_address step_input = input;

        while (string_get(step))
        {
                if (string_not(step, string_get(step_input)))
                {
                        step++;
                        continue;
                }

                string_address find = step;

                while
                        string_get(step_input)
                        {
                                if string_not (step, string_get(step_input))
                                        break;

                                step++;
                                step_input++;
                        }

                if string_is (step_input, end)
                        return find;

                step_input = input;
        }

        return null;
}

fn string_replace_all(string_address string, b8 cut_symbol, b8 replace_symbol)
{
        string_address step = string;

        while (string_get(step))
        {
                if string_is (step, cut_symbol)
                        string_set(step, replace_symbol);

                step++;
        }
}

fn string_get_environment(const b8 address_to name)
{
}

// performs several cuts depending on number of arguments, each argument
// will be written to at the start of the cut string
/* TBD
string_address string_split(string_address string, b8 cut_symbol, ...)
{
        var_args args;
        var_list(args, string);

        string_address step = string;

        while (1)
        {
                step = string_cut(step, cut_symbol);

                if (step == null)
                        break;

                string_address split_step = var_list_get(args, string_address);

                if (split_step == null)
                        break;

                address_to split_step = (string_address)split_step;
        }

        var_list_end(args);

        return step;
}
*/

// ### Takes a positive number and writes out the string representation
fn positive_to_string(writer write, positive number)
{
        if (number == 0)
                return write("0", 1);

        // No thread safety for you >:) (wip) TODO: fix
        static p8 digits[32] = {0};
        digits[0] = end;

        p8 address_to step = digits + 31;
        address_to step-- = end;

        while (number > 0 && step > digits)
        {
                address_to step-- = '0' + (number % 10);
                number /= 10;
        }

        write(step + 1, digits + 31 - step - 1);
}

fn bipolar_to_string(writer write, bipolar number)
{
        if (number >= 0)
                return positive_to_string(write, (positive)number);

        write("-", 1);

        bipolar abs_number = number * -1;
        positive_to_string(write, (positive)abs_number);
}

positive string_to_positive(string_address input)
{
        positive result = 0;
        positive multiplier = 1;
        string_address step = input + string_length(input) - 1;

        while (step >= input && string_get(step) >= '0' && string_get(step) <= '9')
        {
                result += (string_get(step) - '0') * multiplier;
                multiplier *= 10;
                step--;
        }

        return result;
}

bipolar string_to_bipolar(string_address input)
{
        if (string_get(input) == '-')
        {
                input++;
                return -string_to_positive(input);
        }

        return string_to_positive(input);
}

//
//      The guard has to be outside the signature, not just around the body.
//      decimal is a floating point type, and arm64 builds the kernel with
//      -mgeneral-regs-only, which refuses one in a prototype whether or not
//      anything reaches the code inside:
//
//          error: '-mgeneral-regs-only' is incompatible with the use of
//                 floating-point types
//
//      x86 let it through because nothing in the body needed an SSE register.
//
#ifndef KERNEL_MODE // Temporary
fn decimal_to_string(writer write, decimal value)
{

        if (value < 0)
        {
                write("-", 1);
                value = -value;
        }

        bipolar integer_part = (bipolar)value;
        decimal fraction_part = value - integer_part;

        bipolar_to_string(write, integer_part);

        write(".", 1);

        fraction_part *= 1000000;
        integer_part = (bipolar)fraction_part;

        if (integer_part < 100000)
                write("0", 1);
        if (integer_part < 10000)
                write("0", 1);
        if (integer_part < 1000)
                write("0", 1);
        if (integer_part < 100)
                write("0", 1);
        if (integer_part < 10)
                write("0", 1);

        bipolar_to_string(write, integer_part);
}
#endif // KERNEL_MODE

fn string_format(writer write, string_address format, ...)
{
        var_args args;
        var_list(args, format);

        string_address segment_start = format;
        positive index = 0;

        while (string_get(format))
        {
                if string_not (format, '%')
                {
                        format++;
                        index++;
                        continue;
                }

                if (index > 0)
                        write(segment_start, format - segment_start);

                format++;
                index = 0;

                switch (string_get(format))
                {
                case 'b':
                {
                        // todo: fix, long int breaks here...
                        int raw_value = var_list_get(args, int);
                        bipolar value = (bipolar)raw_value;
                        bipolar_to_string(write, value);
                        break;
                }
                case 'p':
                {
                        positive value = var_list_get(args, positive);
                        positive_to_string(write, value);
                        break;
                }
#ifndef KERNEL_MODE // Temporary
                case 'f':
                {
                        decimal value = var_list_get(args, decimal);
                        decimal_to_string(write, value);
                        break;
                }
#endif
                case 's':
                {
                        string_address value = var_list_get(args, string_address);
                        write(value, 0);
                        break;
                }
                case '%':
                        write("%", 1);
                        break;

// Optional user extensions 0 - 9
// if up to 9 is needed open a pr!
#ifdef string_format_extension_0
                case '0':
                {
                        string_format_extension_0(write, args);
                        break;
                }
#endif
#ifdef string_format_extension_1
                case '1':
                {
                        string_format_extension_1(write, args);
                        break;
                }
#endif
#ifdef string_format_extension_2
                case '2':
                {
                        string_format_extension_2(write, args);
                        break;
                }
#endif
#ifdef string_format_extension_3
                case '3':
                {
                        string_format_extension_3(write, args);
                        break;
                }
#endif

                case end:
                        return;
                }

                format++;
                segment_start = format;
        }

        write(segment_start, format - segment_start);

        var_list_end(args);
}

// ### Takes a path and writes out the last directory name
fn path_basename(writer write, string_address input)
{
        positive length = string_length(input);

        while (length > 1 && input[length - 1] == '/')
                length--;

        if (length == 1 && input[0] == '/')
                return write("/", 1);

        positive step = length;

        while (step > 0 && input[step - 1] != '/')
                step--;

        write(input + step, length - step);
}

fn shell_set_cursor(writer write, positive2 pos)
{
        string_format(write, ANSI "%p;%pH", pos.y, pos.x);
}

// ### Get CPU time (Time Stamp Counter)
// returns: the current CPU time
p64 get_cpu_time()
{
#if defined(X64)
        p32 high, low;
        ir("rdtsc" : "=a"(low), "=d"(high));
        return ((p64)high << 32) | low;
#elif defined(ARM64)
        p64 result;
        ir("mrs %0, cntvct_el0" : "=r"(result));
        return result;
#elif defined(RISCV64)
        p64 result;
        ir("rdtime %0" : "=r"(result));
        return result;
#endif
}

// Userspace land
#ifndef KERNEL_MODE

#if ARM64

// The copy(reg_N, reg_M) form below relies on the naked calling convention,
// which clang does not provide for C on ARM64: fn_asm falls back to a plain
// function there, so the compiler spills the arguments to the stack, the asm
// reads registers that were never set, and the inline ret skips the epilogue
// that would release the frame. Verified by disassembly -- it produced a
// garbage svc and left sp 48 bytes low.
//
// Register constrained operands express the same thing without needing a
// naked function, so the compiler places each argument where the kernel
// expects it and the function returns normally.
#if defined(MACOS) || defined(IOS)
// Darwin passes the syscall number in x16 and traps with svc #0x80.
#define SYSCALL_NUMBER_REGISTER "x16"
#define SYSCALL_INSTRUCTION "svc #0x80"
#else
// Linux passes the syscall number in x8 and traps with svc #0.
#define SYSCALL_NUMBER_REGISTER "x8"
#define SYSCALL_INSTRUCTION "svc #0"
#endif

static bipolar system_call(positive syscall)
{
        register positive n asm(SYSCALL_NUMBER_REGISTER) = syscall;
        register positive a0 asm("x0");
        asm volatile(SYSCALL_INSTRUCTION : "=r"(a0) : "r"(n) : "memory", "cc");
        return (bipolar)a0;
}

static bipolar system_call_1(positive syscall, positive argument_1)
{
        register positive n asm(SYSCALL_NUMBER_REGISTER) = syscall;
        register positive a0 asm("x0") = argument_1;
        asm volatile(SYSCALL_INSTRUCTION : "+r"(a0) : "r"(n) : "memory", "cc");
        return (bipolar)a0;
}

static bipolar system_call_2(positive syscall, positive argument_1, positive argument_2)
{
        register positive n asm(SYSCALL_NUMBER_REGISTER) = syscall;
        register positive a0 asm("x0") = argument_1;
        register positive a1 asm("x1") = argument_2;
        asm volatile(SYSCALL_INSTRUCTION : "+r"(a0) : "r"(n), "r"(a1) : "memory", "cc");
        return (bipolar)a0;
}

static bipolar system_call_3(positive syscall, positive argument_1, positive argument_2, positive argument_3)
{
        register positive n asm(SYSCALL_NUMBER_REGISTER) = syscall;
        register positive a0 asm("x0") = argument_1;
        register positive a1 asm("x1") = argument_2;
        register positive a2 asm("x2") = argument_3;
        asm volatile(SYSCALL_INSTRUCTION : "+r"(a0) : "r"(n), "r"(a1), "r"(a2) : "memory", "cc");
        return (bipolar)a0;
}

static bipolar system_call_4(positive syscall, positive argument_1, positive argument_2, positive argument_3, positive argument_4)
{
        register positive n asm(SYSCALL_NUMBER_REGISTER) = syscall;
        register positive a0 asm("x0") = argument_1;
        register positive a1 asm("x1") = argument_2;
        register positive a2 asm("x2") = argument_3;
        register positive a3 asm("x3") = argument_4;
        asm volatile(SYSCALL_INSTRUCTION : "+r"(a0) : "r"(n), "r"(a1), "r"(a2), "r"(a3) : "memory", "cc");
        return (bipolar)a0;
}

static bipolar system_call_5(positive syscall, positive argument_1, positive argument_2, positive argument_3, positive argument_4, positive argument_5)
{
        register positive n asm(SYSCALL_NUMBER_REGISTER) = syscall;
        register positive a0 asm("x0") = argument_1;
        register positive a1 asm("x1") = argument_2;
        register positive a2 asm("x2") = argument_3;
        register positive a3 asm("x3") = argument_4;
        register positive a4 asm("x4") = argument_5;
        asm volatile(SYSCALL_INSTRUCTION : "+r"(a0) : "r"(n), "r"(a1), "r"(a2), "r"(a3), "r"(a4) : "memory", "cc");
        return (bipolar)a0;
}

static bipolar system_call_6(positive syscall, positive argument_1, positive argument_2, positive argument_3, positive argument_4, positive argument_5, positive argument_6)
{
        register positive n asm(SYSCALL_NUMBER_REGISTER) = syscall;
        register positive a0 asm("x0") = argument_1;
        register positive a1 asm("x1") = argument_2;
        register positive a2 asm("x2") = argument_3;
        register positive a3 asm("x3") = argument_4;
        register positive a4 asm("x4") = argument_5;
        register positive a5 asm("x5") = argument_6;
        asm volatile(SYSCALL_INSTRUCTION : "+r"(a0) : "r"(n), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory", "cc");
        return (bipolar)a0;
}

#elif X64

// The register shuffle in the naked implementation put syscall argument 4 in
// rcx. Linux takes it in r10, and the syscall instruction itself overwrites
// rcx with the return address, so every call with four or more arguments got a
// garbage fourth argument -- openat created its file with a random mode.
//
// Register constrained operands put each value where the kernel actually wants
// it and let the compiler manage the rest.
#define SYSCALL_CLOBBERS "rcx", "r11", "memory"

static bipolar system_call(positive syscall)
{
        bipolar result;
        asm volatile("syscall" : "=a"(result) : "a"(syscall) : SYSCALL_CLOBBERS);
        return result;
}

static bipolar system_call_1(positive syscall, positive argument_1)
{
        bipolar result;
        asm volatile("syscall"
                     : "=a"(result)
                     : "a"(syscall), "D"(argument_1)
                     : SYSCALL_CLOBBERS);
        return result;
}

static bipolar system_call_2(positive syscall, positive argument_1, positive argument_2)
{
        bipolar result;
        asm volatile("syscall"
                     : "=a"(result)
                     : "a"(syscall), "D"(argument_1), "S"(argument_2)
                     : SYSCALL_CLOBBERS);
        return result;
}

static bipolar system_call_3(positive syscall, positive argument_1, positive argument_2, positive argument_3)
{
        bipolar result;
        asm volatile("syscall"
                     : "=a"(result)
                     : "a"(syscall), "D"(argument_1), "S"(argument_2), "d"(argument_3)
                     : SYSCALL_CLOBBERS);
        return result;
}

static bipolar system_call_4(positive syscall, positive argument_1, positive argument_2, positive argument_3, positive argument_4)
{
        bipolar result;
        register positive r10 asm("r10") = argument_4;
        asm volatile("syscall"
                     : "=a"(result)
                     : "a"(syscall), "D"(argument_1), "S"(argument_2), "d"(argument_3), "r"(r10)
                     : SYSCALL_CLOBBERS);
        return result;
}

static bipolar system_call_5(positive syscall, positive argument_1, positive argument_2, positive argument_3, positive argument_4, positive argument_5)
{
        bipolar result;
        register positive r10 asm("r10") = argument_4;
        register positive r8 asm("r8") = argument_5;
        asm volatile("syscall"
                     : "=a"(result)
                     : "a"(syscall), "D"(argument_1), "S"(argument_2), "d"(argument_3), "r"(r10), "r"(r8)
                     : SYSCALL_CLOBBERS);
        return result;
}

static bipolar system_call_6(positive syscall, positive argument_1, positive argument_2, positive argument_3, positive argument_4, positive argument_5, positive argument_6)
{
        bipolar result;
        register positive r10 asm("r10") = argument_4;
        register positive r8 asm("r8") = argument_5;
        register positive r9 asm("r9") = argument_6;
        asm volatile("syscall"
                     : "=a"(result)
                     : "a"(syscall), "D"(argument_1), "S"(argument_2), "d"(argument_3), "r"(r10), "r"(r8), "r"(r9)
                     : SYSCALL_CLOBBERS);
        return result;
}

#elif RISCV64

// a7 carries the syscall number, a0..a5 the arguments, and a0 comes back with
// the result. Same reason as the other two: no naked functions needed.
#define SYSCALL_CLOBBERS "memory"

static bipolar system_call(positive syscall)
{
        register positive n asm("a7") = syscall;
        register positive a0 asm("a0");
        asm volatile("ecall" : "=r"(a0) : "r"(n) : SYSCALL_CLOBBERS);
        return (bipolar)a0;
}

static bipolar system_call_1(positive syscall, positive argument_1)
{
        register positive n asm("a7") = syscall;
        register positive a0 asm("a0") = argument_1;
        asm volatile("ecall" : "+r"(a0) : "r"(n) : SYSCALL_CLOBBERS);
        return (bipolar)a0;
}

static bipolar system_call_2(positive syscall, positive argument_1, positive argument_2)
{
        register positive n asm("a7") = syscall;
        register positive a0 asm("a0") = argument_1;
        register positive a1 asm("a1") = argument_2;
        asm volatile("ecall" : "+r"(a0) : "r"(n), "r"(a1) : SYSCALL_CLOBBERS);
        return (bipolar)a0;
}

static bipolar system_call_3(positive syscall, positive argument_1, positive argument_2, positive argument_3)
{
        register positive n asm("a7") = syscall;
        register positive a0 asm("a0") = argument_1;
        register positive a1 asm("a1") = argument_2;
        register positive a2 asm("a2") = argument_3;
        asm volatile("ecall" : "+r"(a0) : "r"(n), "r"(a1), "r"(a2) : SYSCALL_CLOBBERS);
        return (bipolar)a0;
}

static bipolar system_call_4(positive syscall, positive argument_1, positive argument_2, positive argument_3, positive argument_4)
{
        register positive n asm("a7") = syscall;
        register positive a0 asm("a0") = argument_1;
        register positive a1 asm("a1") = argument_2;
        register positive a2 asm("a2") = argument_3;
        register positive a3 asm("a3") = argument_4;
        asm volatile("ecall" : "+r"(a0) : "r"(n), "r"(a1), "r"(a2), "r"(a3) : SYSCALL_CLOBBERS);
        return (bipolar)a0;
}

static bipolar system_call_5(positive syscall, positive argument_1, positive argument_2, positive argument_3, positive argument_4, positive argument_5)
{
        register positive n asm("a7") = syscall;
        register positive a0 asm("a0") = argument_1;
        register positive a1 asm("a1") = argument_2;
        register positive a2 asm("a2") = argument_3;
        register positive a3 asm("a3") = argument_4;
        register positive a4 asm("a4") = argument_5;
        asm volatile("ecall" : "+r"(a0) : "r"(n), "r"(a1), "r"(a2), "r"(a3), "r"(a4) : SYSCALL_CLOBBERS);
        return (bipolar)a0;
}

static bipolar system_call_6(positive syscall, positive argument_1, positive argument_2, positive argument_3, positive argument_4, positive argument_5, positive argument_6)
{
        register positive n asm("a7") = syscall;
        register positive a0 asm("a0") = argument_1;
        register positive a1 asm("a1") = argument_2;
        register positive a2 asm("a2") = argument_3;
        register positive a3 asm("a3") = argument_4;
        register positive a4 asm("a4") = argument_5;
        register positive a5 asm("a5") = argument_6;
        asm volatile("ecall" : "+r"(a0) : "r"(n), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : SYSCALL_CLOBBERS);
        return (bipolar)a0;
}

#else
#error "no system_call implementation for this architecture"
#endif // architecture specific system_call

p8 address_to program_stack_base = 0;

typedef struct timespec
{
        p64 tv_sec;
        p64 tv_nsec;
} timespec;

// User required implementations
b32 main();

// Platform required implementations
fn exit(b32 code);
fn sleep(timespec address_to time);

#undef memset
// for compatibility, makes the linker happy
address_any memset(address_any destination, int value, long unsigned int size)
{
        return memory_fill(destination, value, size);
}

#ifdef STANDARD_MODERN_C_COMPATIBILITY
// tbd: https://pubs.opengroup.org/onlinepubs/9799919799/

typedef b8 int8_t;
typedef b16 int16_t;
typedef b32 int32_t;
typedef b64 int64_t;

typedef p8 uint8_t;
typedef p16 uint16_t;
typedef p32 uint32_t;
typedef p64 uint64_t;

typedef b8 int_least8_t;
typedef b16 int_least16_t;
typedef b32 int_least32_t;
typedef b64 int_least64_t;

typedef p8 uint_least8_t;
typedef p16 uint_least16_t;
typedef p32 uint_least32_t;
typedef p64 uint_least64_t;

typedef b8 int_fast8_t;
typedef b64 int_fast16_t;
typedef b64 int_fast32_t;
typedef b64 int_fast64_t;

typedef p8 uint_fast8_t;
typedef p64 uint_fast16_t;
typedef p64 uint_fast32_t;
typedef p64 uint_fast64_t;

typedef b64 isize;
typedef p64 usize;

typedef b64 intptr_t;
typedef p64 uintptr_t;

typedef atomic64 atomic64_t;
typedef atomic64_t atomic_long_t;

typedef sized size_t;

typedef b64 intmax_t;
typedef p64 uintmax_t;

typedef b64 ptrdiff_t;

#undef memcpy
// use memory_copy instead, this is for compatibility
address_any memcpy(address_any destination, address_any source, long unsigned int size)
{
        return memory_copy(destination, source, size);
}

/*
        Where the assembly above defines one of these, it is that symbol
        themselves rather than wrappers around the C, and string_length,
        string_compare and string_first_of are what call them. Defining them
        here as well is the same name twice and the link fails.
*/
#ifndef MOONWATER_HAVE_STRLEN

#undef strlen
// use string_length instead, this is for compatibility
positive strlen(string_address source)
{
        return string_length(source);
}

#endif

#ifndef MOONWATER_HAVE_STRCMP
#undef strcmp
// use string_compare instead, this is for compatibility
b32 strcmp(string_address source, string_address input)
{
        return string_compare(source, input);
}

#endif

#undef strcpy
// use string_copy instead, this is for compatibility
string_address strcpy(string_address destination, string_address source)
{
        return string_copy(destination, source);
}

#undef strncpy
// use string_copy_max instead, this is for compatibility
string_address strncpy(string_address destination, string_address source, positive length)
{
        return string_copy_max(destination, source, length);
}

#ifndef MOONWATER_HAVE_STRCHR
#undef strchr
// use string_first_of instead, this is for compatibility
char address_to strchr(char address_to source, int character)
{
        return string_first_of(source, character);
}
#endif

#ifndef MOONWATER_HAVE_STRRCHR
#undef strrchr
// use string_last_of instead, this is for compatibility
char address_to strrchr(char address_to source, int character)
{
        return string_last_of(source, character);
}
#endif

#endif // STANDARD_MODERN_C_COMPATIBILITY

#ifndef STANDARD_NO_PLATFORM

#define stdin 0
#define stdout 1
#define stderr 2

#define SIGTRAP 5
#define SIGKILL 9
#define SIGSTOP 20
#define SIGCHLD 17

#define O_NOCTTY 0400
#define O_NONBLOCK 0
#define O_DIRECTORY 0200000
#define AT_FDCWD -100
#define O_TRUNC 01000
#define O_CLOEXEC 02000000

#define FILE_READ 00
#define FILE_WRITE (01 | 0100 | 01000)
#define FILE_READ_WRITE 02
#define FILE_EXECUTE 010
#define FILE_APPEND (01 | 0100 | 02000)
#define FILE_CREATE 0100
#define FILE_TRUNCATE 0200

#define FILE_PROTECT_READ 0400
#define FILE_PROTECT_WRITE 0200

#define FILE_MAP_PRIVATE 01000
#define FILE_MAP_SHARED 02000
#define FILE_MAP_ANONYMOUS 04000

#define FILE_SEEK_SET 0
#define FILE_SEEK_CUR 1
#define FILE_SEEK_END 2

#include "platform/syscall.c"
#include "platform/any.c"

typedef struct
{
        p32 device;
        p64 inode;
        p32 protection;
        p64 hard_links;
        p32 owner;
        p32 group;
        p32 special_device_id;
        b64 size;
        b64 blocksize;
        b64 blocks;
        b64 last_access;
        b64 last_edit;
        b64 last_update;
} file_status;

typedef struct
{
        positive handle;
        string_address path;
        positive flags;
        address_any data;
        bool loaded;
        file_status status;
} file;

#define file_address file address_to

#define log_file(write, source)       \
        string_format(write,          \
                      "File: %s\n"    \
                      "Handle: %b\n"  \
                      "Flags: %p\n"   \
                      "Data: %p\n"    \
                      "Loaded: %b\n", \
                      source.path,    \
                      source.handle,  \
                      source.flags,   \
                      source.data,    \
                      source.loaded)

#define log_file_status(write, source)                 \
        string_format(write,                           \
                      "Device: %p\n"                   \
                      "Inode: %b\n"                    \
                      "Protection: %p\n"               \
                      "Hard Links: %p\n"               \
                      "Owner: %p\n"                    \
                      "Group: %p\n"                    \
                      "Special Device ID: %p\n"        \
                      "Size: %b\n"                     \
                      "Blocksize: %b\n"                \
                      "Blocks: %b\n"                   \
                      "Last Access: %b\n"              \
                      "Last Edit: %b\n"                \
                      "Last Update: %b\n",             \
                      source.status.device,            \
                      source.status.inode,             \
                      source.status.protection,        \
                      source.status.hard_links,        \
                      source.status.owner,             \
                      source.status.group,             \
                      source.status.special_device_id, \
                      source.status.size,              \
                      source.status.blocksize,         \
                      source.status.blocks,            \
                      source.status.last_access,       \
                      source.status.last_edit,         \
                      source.status.last_update)

#ifdef WINDOWS
__declspec(dllimport) HMODULE __stdcall LoadLibraryA(LPCSTR);
__declspec(dllimport) FARPROC __stdcall GetProcAddress(HMODULE, LPCSTR);
__declspec(dllimport) int __stdcall FreeLibrary(HMODULE);
#endif

positive limit_max_name_length = 256;
string library_fallback_system_paths = "/lib:/usr/local/lib:/usr/lib";

bool file_valid(file_address source)
{
        return source->handle != -1;
}

// flags: FILE_WRITE, FILE_READ, FILE_READ_WRITE, FILE_EXECUTE, FILE_TRUNCATE
fn file_new_lazy(file_address result, string_address path, positive flags)
{
        result->path = path;
        result->flags = flags;

#if defined(WINDOWS)
        HANDLE h = CreateFileA(path, ((flags & O_RDONLY) ? GENERIC_READ : 0) | ((flags & O_WRONLY) ? GENERIC_WRITE : 0) | ((flags & O_RDWR) ? (GENERIC_READ | GENERIC_WRITE) : 0),
                               FILE_SHARE_READ, NULL,
                               ((flags & O_CREAT) ? ((flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS) : OPEN_EXISTING),
                               FILE_ATTRIBUTE_NORMAL, NULL);

        result->handle = (h == INVALID_HANDLE_VALUE) ? -1 : (positive)h;
#else
        result->handle = system_call_3(syscall(openat), AT_FDCWD, (positive)path, flags);
#endif
}

// Get file status information, like size, last access time, etc.
//
// example usage:
//      file example = {0};
//      file_new_lazy(address_of example, "README.md", FILE_READ);
//      file_get_status(address_of example);
fn file_get_status(file_address source)
{
#if defined(WINDOWS)
        BY_HANDLE_FILE_INFORMATION info = {0};
        if (GetFileInformationByHandle((HANDLE)source->handle, address_of info))
        {
                result.size = info.nFileSizeLow;
                result.last_access = info.ftLastAccessTime.dwLowDateTime;
                result.last_edit = info.ftLastWriteTime.dwLowDateTime;
                result.last_update = info.ftCreationTime.dwLowDateTime;
        }
#else
        system_call_2(syscall(fstat), source->handle, (positive)address_of source->status);
#endif
}

// file handle, path relative to the current working directory
// use file_new_lazy if you want to open a file without a status syscall
//
// flags: FILE_WRITE, FILE_READ, FILE_READ_WRITE, FILE_EXECUTE, FILE_TRUNCATE
//
// Examples:
//      file example = {0};
//
//      // open or create a file
//      file_new(address_to example, "vulkan.log", FILE_WRITE | FILE_CREATE | FILE_TRUNCATE);
//
//      // open a read only file *if* it exists
//      file_new(address_to example, "vulkan.log", FILE_READ);
//
//      // open a file for reading and writing, create it if it does not exist
//      file_new(address_to example, "vulkan.log", FILE_READ_WRITE | FILE_CREATE);
//
fn file_new(file_address result, string_address path, positive flags)
{
        file_new_lazy(result, path, flags);

        file_get_status(result);
}

// Load entire file into memory
address_any file_load(file_address source)
{
        if (!file_valid(source))
                return null;

        if (source->loaded && source->data)
                return source->data;

        positive size = source->status.size;

        if (size == 0)
                return null;

        positive page_size = 4096;
        positive pages = (size + page_size - 1) / page_size;

#if defined(WINDOWS)
        source->data = VirtualAlloc(NULL, pages * page_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!source->data)
                return null;

        DWORD bytes_read;
        SetFilePointer((HANDLE)source->handle, 0, NULL, FILE_BEGIN);
        if (!ReadFile((HANDLE)source->handle, source->data, (DWORD)size, address_of bytes_read, NULL) ||
            bytes_read != size)
        {
                VirtualFree(source->data, 0, MEM_RELEASE);
                source->data = null;
                return null;
        }
#else

        source->data = (address_any)system_call_6(syscall(mmap),
                                                  0, pages * page_size,
                                                  FILE_PROTECT_READ | FILE_PROTECT_WRITE,
                                                  FILE_MAP_PRIVATE | FILE_MAP_ANONYMOUS, -1, 0);

        if (source->data == address_bad)
        {
                source->data = null;
                return null;
        }

        system_call_3(syscall(lseek), source->handle, 0, FILE_SEEK_SET);

        positive bytes_read = system_call_3(syscall(read), source->handle, (positive)source->data, size);

        if (bytes_read != size)
        {
                system_call_2(syscall(munmap), (positive)source->data, pages * page_size);
                source->data = null;
                return null;
        }
#endif

        source->loaded = true;
        return source->data;
}

positive file_read(file_address source, address_any buffer, positive size, positive offset)
{
        if (!file_valid(source))
                return -1;

        if (source->loaded && source->data)
        {

                if (offset >= source->status.size)
                        return 0;

                positive available = source->status.size - offset;
                positive to_read = size < available ? size : available;
                memory_copy(buffer, (p8 address_to)source->data + offset, to_read);
                return to_read;
        }

#if defined(WINDOWS)
        LARGE_INTEGER li_offset;
        li_offset.QuadPart = offset;
        SetFilePointerEx((HANDLE)source->handle, li_offset, NULL, FILE_BEGIN);

        DWORD bytes_read;
        if (!ReadFile((HANDLE)source->handle, buffer, (DWORD)size, address_of bytes_read, NULL))
                return -1;
        return bytes_read;
#else
        system_call_3(syscall(lseek), source->handle, offset, FILE_SEEK_SET);
        return system_call_3(syscall(read), source->handle, (positive)buffer, size);
#endif
}

// Unload file data from memory
fn file_unload(file_address source)
{
        if (!source->loaded && !source->data)
                return;

        positive page_size = 4096;
        positive pages = (source->status.size + page_size - 1) / page_size;

#if defined(WINDOWS)
        VirtualFree(source->data, 0, MEM_RELEASE);
#else
        system_call_2(syscall(munmap), (positive)source->data, pages * page_size);
#endif

        source->data = null;
        source->loaded = false;
}

// Write to file from provided buffer
positive file_write(file_address source, address_any buffer, positive size, positive offset)
{
        if (!file_valid(source))
                return -1;

        bool update_memory = source->loaded && source->data && offset < source->status.size;

#if defined(WINDOWS)
        LARGE_INTEGER li_offset;
        li_offset.QuadPart = offset;
        SetFilePointerEx((HANDLE)source->handle, li_offset, NULL, FILE_BEGIN);

        DWORD bytes_written;
        if (!WriteFile((HANDLE)source->handle, buffer, (DWORD)size, address_of bytes_written, NULL))
                return -1;

        if (update_memory && bytes_written > 0)
        {
                positive end_offset = offset + bytes_written;
                if (end_offset > source->status.size)
                {
                        file_get_status(source);
                        file_unload(source);
                }
                else
                {
                        memory_copy((p8 address_to)source->data + offset, buffer, bytes_written);
                }
        }

        return bytes_written;
#else
        system_call_3(syscall(lseek), source->handle, offset, FILE_SEEK_SET);
        positive bytes_written = system_call_3(syscall(write), source->handle, (positive)buffer, size);

        if (update_memory && bytes_written > 0)
        {

                positive end_offset = offset + bytes_written;

                if (end_offset > source->status.size)
                {
                        file_get_status(source);
                        file_unload(source);
                }
                else
                {
                        memory_copy((p8 address_to)source->data + offset, buffer, bytes_written);
                }
        }

        return bytes_written;
#endif
}

// Close file and clean up resources
fn file_close(file_address source)
{
        if (!file_valid(source))
                return;

        file_unload(source);

#if defined(WINDOWS)
        CloseHandle((HANDLE)source->handle);
#else
        system_call_1(syscall(close), source->handle);
#endif
        source->handle = -1;
        source->path = null;
}

// ### Load library the system
// Traditional: dlopen
fn library_open(file_address storage_location, string_address library_path)
{
#ifdef WINDOWS
        return LoadLibraryA(library_path);
#endif

        string_address is_relative_path = string_first_of(library_path, '/');

        if (!is_relative_path)
        {
                file_new(storage_location, library_path, FILE_READ | FILE_EXECUTE);
        }
}

// ### Get address of a function/symbol in the library
// Traditional: dlsym
address_any library_get(address_any library, string_address name)
{
#ifdef WINDOWS
        return GetProcAddress(library, name);
#else
        // Not implemented outside Windows yet. Returning null is a result the
        // caller can test; falling off the end is undefined behaviour and
        // hands back whatever happens to be in the return register.
        (void)library;
        (void)name;
        return null;
#endif
}

// ### Free the library
// Traditional: dlclose
fn library_close(address_any library)
{
#ifdef WINDOWS
        FreeLibrary(library);
#endif
}

bool raw_windows_paths = false;

p8 working_directory[1024] = {0};

string_address working_directory_get()
{
#if defined(WINDOWS)
        GetCurrentDirectoryA(sizeof(working_directory), working_directory);

        if (!raw_windows_paths)
                string_replace_all(working_directory, '\\', '/');
#else
        system_call_2(syscall(getcwd), (positive)working_directory, sizeof(working_directory));
#endif
        return working_directory;
}

fn working_directory_set(string_address path)
{
#if defined(WINDOWS)
        if (!raw_windows_paths)
                string_replace_all(path, '/', '\\');

        SetCurrentDirectoryA(path);

#else
        system_call_1(syscall(chdir), (positive)path);
#endif

        working_directory_get();
}

#if defined(LINUX)
#include "platform/linux.c"
#endif

#if defined(MACOS)
#include "platform/macos.c"
#endif

// ### Memory allocation
// Allocates a linear chunk of memory of the specified size.
address_any memory(positive size)
{
        if (size < 4096)
        {
                // tbd: bump allocator
        }

#if defined(WINDOWS)
        return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
        return (address_any)system_call_3(syscall(mmap), 0, size, FILE_PROTECT_READ | FILE_PROTECT_WRITE | FILE_MAP_PRIVATE | FILE_MAP_ANONYMOUS - 1);
#endif
}

fn memory_free(address_any address, positive size)
{
        if (!address || size == 0)
                return;

#if defined(WINDOWS)
        VirtualFree(address, 0, MEM_RELEASE);
#else
        system_call_2(syscall(munmap), (positive)address, size);
#endif
}

#endif // STANDARD_NO_PLATFORM

#endif // KERNEL_MODE

#endif // STANDARD_MODERN_C