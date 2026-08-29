#include "../compiler_memory.c"

// Reduces compiler noise for tests
#if defined(__clang__)
#pragma clang diagnostic ignored "-Woverflow"
#endif

#if defined(__GNUC__) || defined(__GNUG__)
#pragma GCC diagnostic ignored "-Woverflow"
#endif

#define test(test_name) bool test_##test_name()
#define case(test_name) {#test_name, test_##test_name}
#define fail(condition) if(!(condition)) return false

#define fail_equals(a, b) if((a) == (b)) return false

#define fail_not_equals(a, b) \
    if((a) != (b)) { \
        string_format(log_direct, "\n [FAIL] expected %p,  got %p\n", \
                (positive)(b), (positive)(a)); \
        return false; \
    }

typedef bool(address_to test_function)();

typedef struct
{
        string_address name;
        test_function function;
        bool result;
} test_case;

p32 address_to p32_nulled = ((address_any)0);

p8 test_write_buffer[1000];
positive test_write_pos = 0;

fn test_writer(address_any data, positive length) {
        if(length == 0) length = string_length(data);
        memory_copy(test_write_buffer + test_write_pos, data, length);
        test_write_pos += length;
}

#define test_type_basics(type_name, max, min) \
        test(type_name##_sizeof) { fail_not_equals(sizeof(type_name), type_name##_bytes); return true; } \
        test(type_name##_bytes_constant) { fail_not_equals(type_name##_bytes, sizeof(type_name)); return true; } \
        test(type_name##_max) { fail_not_equals(type_name##_max, max); return true; } \
        test(type_name##_min) { fail_not_equals(type_name##_min, min); return true; } \
        test(type_name##_overflow) { fail_not_equals((type_name)(type_name##_max + 1), type_name##_min); return true; } \
        test(type_name##_underflow) { fail_not_equals((type_name)(type_name##_min - 1), type_name##_max); return true; }

// Integers wrap; decimals saturate toward infinity, so f-types get the size
// and bounds checks without the wrap-around ones.
#define test_type_decimal(type_name, max, min) \
        test(type_name##_sizeof) { fail_not_equals(sizeof(type_name), type_name##_bytes); return true; } \
        test(type_name##_bytes_constant) { fail_not_equals(type_name##_bytes, sizeof(type_name)); return true; } \
        test(type_name##_max) { fail_not_equals(type_name##_max, max); return true; } \
        test(type_name##_min) { fail_not_equals(type_name##_min, min); return true; }

#define case_type_basics(type_name) \
        case(type_name##_sizeof), \
        case(type_name##_bytes_constant), \
        case(type_name##_max), \
        case(type_name##_min), \
        case(type_name##_overflow), \
        case(type_name##_underflow)

#define case_type_decimal(type_name) \
        case(type_name##_sizeof), \
        case(type_name##_bytes_constant), \
        case(type_name##_max), \
        case(type_name##_min)

test_type_basics(p8, 255, 0);
test_type_basics(b8, 127, -128);

test_type_basics(p16, 65535, 0);
test_type_basics(b16, 32767, -32768);

test_type_basics(p32, 4294967295U, 0);
test_type_basics(b32, 2147483647, -2147483648);

test_type_basics(p64, 18446744073709551615U, 0);
test_type_basics(b64, 9223372036854775807, -9223372036854775808);

// C has no 128 bit literal syntax, so the expected bounds are built the same
// way the library builds them rather than written out as constants.
#ifdef HAS_128
test_type_basics(p128, (p128) ~ (p128)0, (p128)0);
test_type_basics(b128, (b128)(((p128)1 << 127) - 1), (b128)((p128)1 << 127));
#endif

// Decimals have no wrapping overflow, so they get the bounds tests only.
test_type_decimal(f32, 3.402823466e+38f, 1.175494351e-38f);
test_type_decimal(f64, 1.7976931348623157e+308, 2.2250738585072014e-308);
test_type_decimal(f128, __LDBL_MAX__, __LDBL_MIN__);

test(bit_flip_zero_to_one) { 
    p32 value = 0;
    bit_flip(0, &value);
    fail_not_equals(value, 1);
    return true; 
}

test(bit_flip_one_to_zero) { 
    p32 value = 1;
    bit_flip(0, &value);
    fail_not_equals(value, 0);
    return true; 
}

test(bit_set_basic) {
    p32 value = 0;
    bit_set(0, &value);
    fail_not_equals(value, 1);
    return true;
}

test(bit_clear_basic) {
    p32 value = 1;
    bit_clear(0, &value);
    fail_not_equals(value, 0);
    return true;
}

test(bit_test_set_bit) {
    p32 value = 1;
    fail(bit_test(0, &value));
    return true;
}

test(bit_test_clear_bit) {
    p32 value = 0;
    fail(!bit_test(0, &value));
    return true;
}

test(bit_set_high_bit) {
    p32 value = 0;
    bit_set(31, &value);
    fail_not_equals(value, 0x80000000);
    return true;
}

test(addresses) {
        positive some_positive = 123456;
        positive address_to ptr = &some_positive;

        fail_not_equals(address_to ptr, some_positive);
        fail_not_equals(ptr, &some_positive);
        
        positive address_to null_ptr = (positive address_to)0;
        fail(is_null(null_ptr));
        
        positive arr[2] = {1,2};
        positive address_to arr_ptr = &arr[0];
        fail_not_equals(*(arr_ptr+1), 2);

        return true;
}

test(is_null) {
        fail(is_null(p32_nulled));
        fail(is_null(null));
        fail(is_null(0));
        fail(!is_null(&p32_nulled));
        
        positive value = 0;
        fail(!is_null(&value));
        
        int stack_var = 1;
        fail(!is_null(&stack_var));

        static int static_var = 2;
        fail(!is_null(&static_var));

        return true;
}

// Atomic operations test (basic)
test(atomic_operations) {
        positive value = 0;
        
        atomic_add(&value, 5);
        fail_not_equals(value, 5);
        
        atomic_sub(&value, 2);
        fail_not_equals(value, 3);
        
        atomic_inc(&value);
        fail_not_equals(value, 4);
        
        atomic_dec(&value);
        fail_not_equals(value, 3);
        
        positive old = atomic_exchange(&value, 10);
        fail_not_equals(old, 3);
        fail_not_equals(value, 10);
        
        bool success = atomic_compare_exchange(&value, 10, 20);
        fail(success);
        fail_not_equals(value, 20);
        
        success = atomic_compare_exchange(&value, 10, 30);
        fail(!success);
        fail_not_equals(value, 20);
        
        return true;
}

test(memory_fill) {
        p8 buffer[100];
        
        memory_fill(buffer, 0x42, 100);
        
        for(positive i = 0; i < 100; i++) {
                fail_not_equals(buffer[i], 0x42);
        }
        
        memory_fill(buffer, 0, 50);
        
        for(positive i = 0; i < 50; i++) {
                fail_not_equals(buffer[i], 0);
        }
        
        for(positive i = 50; i < 100; i++) {
                fail_not_equals(buffer[i], 0x42);
        }

        memory_fill(buffer, 0x99, 0);
        
        for(positive i = 0; i < 100; i++) {
                fail(buffer[i] == 0 || buffer[i] == 0x42);
        }
        
        memory_fill(buffer, 0xFF, 100);
        
        for(positive i = 0; i < 100; i++) {
                fail_not_equals(buffer[i], 0xFF);
        }
        
        return true;
}

test(memory_copy) {
        
        p8 buffer[] = "1234567890";
        p8 source[] = "Hello, World!";
        p8 destination[50] = {0};
        
        memory_copy(buffer + 2, buffer, 5);
        fail_not_equals(buffer[2], '1');
        fail_not_equals(buffer[6], '5');
        
        /*
        // non-overlapping regions
        memory_copy(destination, source, 16);
        fail_not_equals(string_compare(destination, source), 0);
        */

        return true;
}

test(string_length) {
        
        fail_not_equals(string_length(""), 0);
        fail_not_equals(string_length("a"), 1);
        fail_not_equals(string_length("Hello"), 5);
        fail_not_equals(string_length("Visible\0Hidden"), 7);
        
        p8 buffer[100];
        memory_fill(buffer, 'A', 99);
        
        buffer[99] = end;
        fail_not_equals(string_length(buffer), 99);
        
        p8 only_null[1] = {end};
        fail_not_equals(string_length(only_null), 0);

        return true;
}

test(string_compare) {
        fail_not_equals(string_compare("", ""), 0);
        fail_not_equals(string_compare("abc", "abc"), 0);
        fail(string_compare("abc", "abd") < 0);
        fail(string_compare("abd", "abc") > 0);
        fail(string_compare("abc", "abcd") < 0);
        fail(string_compare("abcd", "abc") > 0);
        fail(string_compare("", "a") < 0);
        fail(string_compare("a", "") > 0);

        return true;
}

test(string_copy) {
        p8 dest[100];
        
        string_copy(dest, "Hello");
        fail_not_equals(string_compare(dest, "Hello"), 0);
        
        string_copy(dest, "");
        fail_not_equals(string_length(dest), 0);

        string_copy(dest, "A very long string that tests the copy function");
        fail_not_equals(string_length(dest), 47);
        
        string_copy(dest, dest);
        fail_not_equals(string_compare(dest, "A very long string that tests the copy function"), 0);
        
        return true;
}

test(string_copy_max) {

        p8 dest[100];

        memory_fill(dest, 'X', 100);
        string_copy_max(dest, "Hello, World!", 5);
        
        fail_not_equals(dest[5], 'X');
        fail_not_equals(dest[0], 'H');
        fail_not_equals(dest[4], 'o');

        // Source shorter than the limit: terminate inside the bound.
        string_copy_max(dest, "Hi", 10);
        fail_not_equals(dest[2], end);

        // A zero limit must write nothing at all, not even a terminator.
        memory_fill(dest, 'H', 4);
        string_copy_max(dest, "Zebra", 0);
        fail_not_equals(dest[0], 'H');
        
        return true;
}

test(string_first_of) {
        string_address result;

        result = string_first_of("Hello, World!", 'o');
        fail(result != null);
        fail_not_equals(*result, 'o');
        
        result = string_first_of("Hello, World!", 'z');
        fail_not_equals(result, null);
        
        result = string_first_of("Hello, World!", end);
        fail(result != null);
        fail_not_equals(*result, end);
        
        result = string_first_of("abc", 'a');
        fail(result != null);
        fail_not_equals(*result, 'a');
        
        result = string_first_of("abc", 'c');
        fail(result != null);
        fail_not_equals(*result, 'c');
        
        return true;
}

test(string_last_of) {
        string_address result;

        result = string_last_of("Hello, World!", 'o');
        fail(result != null);
        fail_not_equals(*result, 'o');
        
        result = string_last_of("Hello, World!", 'H');
        fail(result != null);
        
        result = string_last_of("Hello, World!", 'z');
        fail_not_equals(result, null);
        
        result = string_last_of("abc", 'c');
        fail(result != null);
        fail_not_equals(*result, 'c');
        
        result = string_last_of("abc", 'a');
        fail(result != null);
        fail_not_equals(*result, 'a');
        
        return true;
}

test(string_cut) {
        p8 buffer[] = "Hello, World!";
        
        string_address result = string_cut(buffer, ' ');
        fail(result != null);
        fail_not_equals(string_compare(buffer, "Hello,"), 0);
        fail_not_equals(string_compare(result, "World!"), 0);
        
        p8 buffer2[] = "NoSpaces";
        result = string_cut(buffer2, ' ');
        fail_not_equals(result, null);
        
        p8 buffer3[] = "End ";
        result = string_cut(buffer3, ' ');
        fail_not_equals(result, null);

        /*
        p8 buffer4[] = " Xcut";
        result = string_cut(buffer4, ' ');
        fail(result != null);
        fail_not_equals(string_compare(buffer4, ""), 0);
        fail_not_equals(string_compare(result, "Xcut"), 0);
        */

        return true;
}

test(string_replace_all) {
        p8 buffer2[] = "aaaa";
        string_replace_all(buffer2, 'a', 'b');
        fail_not_equals(string_compare(buffer2, "bbbb"), 0);
        
        p8 buffer3[] = "cccc";
        string_replace_all(buffer3, 'c', 'c');
        fail_not_equals(string_compare(buffer3, "cccc"), 0);

        p8 buffer4[] = "dddd";
        string_replace_all(buffer4, 'z', 'y');
        fail_not_equals(string_compare(buffer4, "dddd"), 0);
        
        return true;
}

test(string_format_basic) {
        test_write_pos = 0;
        memory_fill(test_write_buffer, 0, 1000);
        
        string_format(test_writer, "Hello %s!", "World");
        test_write_buffer[test_write_pos] = end;
        fail_not_equals(string_compare(test_write_buffer, "Hello World!"), 0);
        
        test_write_pos = 0;
        string_format(test_writer, "%% test");
        test_write_buffer[test_write_pos] = end;
        fail_not_equals(string_compare(test_write_buffer, "% test"), 0);
        
        return true;
}

test(string_format_numbers) {
        test_write_pos = 0;
        memory_fill(test_write_buffer, 0, 1000);
        
        string_format(test_writer, "Positive: %p", 12345);
        test_write_buffer[test_write_pos] = end;
        fail_not_equals(string_compare(test_write_buffer, "Positive: 12345"), 0);
        
        test_write_pos = 0;
        string_format(test_writer, "Bipolar: %b", -42);
        test_write_buffer[test_write_pos] = end;
        fail_not_equals(string_compare(test_write_buffer, "Bipolar: -42"), 0);
        
        test_write_pos = 0;
        string_format(test_writer, "Zero: %p", 0);
        test_write_buffer[test_write_pos] = end;
        fail_not_equals(string_compare(test_write_buffer, "Zero: 0"), 0);
        
        return true;
}

test(string_format_mixed) {
        test_write_pos = 0;
        memory_fill(test_write_buffer, 0, 1000);
        
        string_format(test_writer, "%s has %p items worth %b each", "Ada", 5, -10);

        test_write_buffer[test_write_pos] = end;

        fail_not_equals(string_compare(test_write_buffer, "Ada has 5 items worth -10 each"), 0);
        
        return true;
}

// Path operations
test(path_basename) {
        test_write_pos = 0;
        memory_fill(test_write_buffer, 0, 1000);
        
        path_basename(test_writer, "/usr/bin/test");
        test_write_buffer[test_write_pos] = end;
        fail_not_equals(string_compare(test_write_buffer, "test"), 0);
        
        test_write_pos = 0;
        path_basename(test_writer, "/usr/bin/");
        test_write_buffer[test_write_pos] = end;
        fail_not_equals(string_compare(test_write_buffer, "bin"), 0);
        
        test_write_pos = 0;
        path_basename(test_writer, "/");
        test_write_buffer[test_write_pos] = end;
        fail_not_equals(string_compare(test_write_buffer, "/"), 0);
        
        test_write_pos = 0;
        path_basename(test_writer, "test.txt");
        test_write_buffer[test_write_pos] = end;
        fail_not_equals(string_compare(test_write_buffer, "test.txt"), 0);
        
        return true;
}

test(str_macro) {
        string_address data;
        positive length;
        
        data = str("Hello");
        fail_not_equals(data[0], 'H');
        fail_not_equals(data[1], 'e');
        
        return true;
}

test(string_end) {
        fail_not_equals(end, '\0');
        fail_not_equals(end, 0);
        
        p8 buffer[] = "Test";
        fail_not_equals(buffer[4], end);
        
        return true;
}

test(writer_pattern) {
        test_write_pos = 0;
        memory_fill(test_write_buffer, 0, 1000);
        
        test_writer("Hello", 5);
        fail_not_equals(test_write_pos, 5);
        fail_not_equals(string_compare(test_write_buffer, "Hello"), 0);
        
        test_writer(", ", 2);
        test_writer("World!", 0);
        fail_not_equals(string_compare(test_write_buffer, "Hello, World!"), 0);
        
        return true;
}


/*
        A literal is as long as its letters.

        str() once expanded to the string and sizeof(string), which counts the
        terminator, so every literal written through it carried a stray NUL
        after it. A terminal draws nothing for that and nothing here measured
        the bytes, so it shipped for years.
*/
positive measured_length;
string_address measured_data;

fn measure(address_any data, positive length)
{
        measured_data = data;
        measured_length = length;
}

test(str_length) {
        measure(str(""));
        fail_not_equals(measured_length, 0);

        measure(str("a"));
        fail_not_equals(measured_length, 1);

        measure(str("seventy"));
        fail_not_equals(measured_length, 7);

        measure(str("eightyyy"));
        fail_not_equals(measured_length, 8);

        measure(str("Hello, World!"));
        fail_not_equals(measured_length, 13);
        fail_not_equals(measured_data[12], '!');

        return true;
}

test(str_writes_no_terminator) {
        memory_fill(test_write_buffer, 0xAA, 1000);
        test_write_pos = 0;

        test_writer(str("Hello"));

        fail_not_equals(test_write_pos, 5);
        fail_not_equals(test_write_buffer[4], 'o');
        fail_not_equals(test_write_buffer[5], 0xAA);

        return true;
}

/*
        Strings that are not literals.

        Every needle in this file used to be a string literal, and literals sit
        end to end in .rodata: a routine that reads eight bytes at a time picks
        up the next literal past the terminator and still compares equal. A
        name of exactly seven characters was broken for a fortnight because of
        it. These build their strings in a buffer whose bytes after the
        terminator are 0xff, which is a byte no answer can contain.
*/
p8 padded_a[64];
p8 padded_b[64];

// Writes count letters and a terminator into buffer, with 0xff everywhere
// else, at an offset so the start is not word aligned either.
static string_address padded(p8 address_to buffer, positive offset, positive count)
{
        p8 address_to at = buffer + offset;

        memory_fill(buffer, 0xff, 64);

        for (positive i = 0; i < count; i++)
                at[i] = 'a' + (p8)(i % 16);

        at[count] = end;

        return at;
}

test(string_length_padded) {
        for (positive offset = 0; offset < 8; offset++)
                for (positive count = 0; count < 40; count++)
                        fail_not_equals(string_length(padded(padded_a, offset, count)),
                                        count);

        return true;
}

test(string_compare_padded) {
        for (positive count = 0; count < 40; count++)
        {
                string_address left = padded(padded_a, 1, count);
                string_address right = padded(padded_b, 3, count);

                fail_not_equals(string_compare(left, right), 0);

                if (!count)
                        continue;

                // One byte apart at the very end, which is where a routine
                // that stops a word early stops noticing.
                padded_b[3 + count - 1] = 'Z';
                fail_equals(string_compare(left, right), 0);
        }

        return true;
}

test(string_first_of_padded) {
        for (positive count = 1; count < 24; count++)
        {
                string_address source = padded(padded_a, 5, count);

                fail_not_equals(string_first_of(source, source[count - 1]),
                                source + ((count - 1) % 16 < count - 1
                                              ? (count - 1) % 16
                                              : count - 1));

                fail_not_equals(string_first_of(source, 0xfe), (string_address)0);
        }

        return true;
}

/*
        A table of names looked up by name.

        Each entry is built in its own buffer rather than written as a
        literal, and the name of length seven is a prefix of the one of length
        eight, which is the pair a word at a time gets wrong.
*/
p8 table_storage[16][32];
string_address table_names[17];

test(string_table_find_lengths) {
        p8 needle[32];

        for (positive n = 0; n < 16; n++)
        {
                memory_fill(table_storage[n], 0xff, 32);

                for (positive i = 0; i <= n; i++)
                        table_storage[n][i] = 'a' + (p8)i;

                table_storage[n][n + 1] = end;
                table_names[n] = table_storage[n];
        }

        table_names[16] = (string_address)0;

        for (positive n = 0; n < 16; n++)
        {
                memory_fill(needle, 0xff, 32);
                memory_copy(needle, table_storage[n], n + 2);

                fail_not_equals(string_table_find(needle, table_names,
                                                  sizeof(string_address), 16),
                                n);
        }

        // One letter longer than the longest, and one that is a prefix of
        // nothing: both have to come back as not here.
        memory_fill(needle, 0xff, 32);
        for (positive i = 0; i < 20; i++)
                needle[i] = 'a' + (p8)i;
        needle[20] = end;
        fail_not_equals(string_table_find(needle, table_names,
                                          sizeof(string_address), 16), 16);

        memory_fill(needle, 0xff, 32);
        needle[0] = 'z';
        needle[1] = end;
        fail_not_equals(string_table_find(needle, table_names,
                                          sizeof(string_address), 16), 16);

        return true;
}

/*
        A stack frame the compiler's own stores can reach.

        The kernel enters _start without pushing a return address, so a plain
        C entry point leaves the stack eight bytes out of phase and the
        aligned SSE stores emitted for a fill like this one fault. This was a
        heredoc in the CI workflow; it is a test.
*/
test(stack_is_aligned) {
        p8 buffer[4096];

        memory_fill(buffer, 0x5a, sizeof(buffer));

        for (positive i = 0; i < sizeof(buffer); i++)
                fail_not_equals(buffer[i], 0x5a);

        return true;
}

#if LINUX

/*
        The fourth argument of a system call.

        Linux takes it in r10 and the syscall instruction overwrites rcx, so a
        register shuffle that put it in rcx handed the kernel whatever the
        return address happened to be -- and openat created its files with a
        random mode. Two checks, because the first needs no struct layout:
        pread's fourth argument is an offset, and reading from three bytes in
        either gives the fourth byte or it does not.
*/
test(syscall_argument_four) {
        string_address path = "/tmp/dawning_argument_four";
        p8 buffer[8];
        bipolar file = system_call_4(syscall(openat), AT_FDCWD, (positive)path,
                                     FILE_CREATE | FILE_WRITE | O_TRUNC, 0644);

        fail(file >= 0);

        system_call_3(syscall(write), file, (positive)"abcdefgh", 8);
        system_call_1(syscall(close), file);

        file = system_call_4(syscall(openat), AT_FDCWD, (positive)path, FILE_READ, 0);
        fail(file >= 0);

        memory_fill(buffer, 0, sizeof(buffer));
        fail_not_equals(system_call_4(syscall(pread64), file, (positive)buffer, 4, 3), 4);
        system_call_1(syscall(close), file);

        fail_not_equals(buffer[0], 'd');
        fail_not_equals(buffer[3], 'g');

        return true;
}

/*
        And the mode that argument carried.

        newfstatat fills a struct whose layout is not the same on the three
        machines -- st_mode is sixteen bytes in on the generic layout and
        twenty four in on x86_64 -- so the offset is chosen here rather than
        named, and the field is read out of a byte buffer.
*/
#if X64
#define STAT_MODE_OFFSET 24
#else
#define STAT_MODE_OFFSET 16
#endif

test(created_file_mode) {
        string_address path = "/tmp/dawning_created_mode";
        p8 status[256];
        p32 mode;
        bipolar file = system_call_4(syscall(openat), AT_FDCWD, (positive)path,
                                     FILE_CREATE | FILE_WRITE | O_TRUNC, 0644);

        fail(file >= 0);
        system_call_1(syscall(close), file);

        memory_fill(status, 0, sizeof(status));
        fail_not_equals(system_call_4(syscall(newfstatat), AT_FDCWD, (positive)path,
                                      (positive)status, 0), 0);

        memory_copy(address_of mode, status + STAT_MODE_OFFSET, sizeof(mode));

        fail_not_equals(mode & 0777, 0644);

        return true;
}

/*
        The folded write loop's three observable exits.

        An invalid descriptor is the ordinary negative-syscall path. The
        zero-length call pairs it with an invalid address so the assembly has
        to take its no-syscall fast path before the pointer can matter.
*/

/*
        The byte classes, against the definitions rather than against a table.

        Every value a C program may pass is checked, which is -1 for EOF and
        then all 256 bytes. The expected answer is written out here as the
        plain condition, so this compares the assembly against what the
        standard says rather than against another copy of the same trick.
*/
test(byte_classes) {
        for (b32 value = -1; value < 256; value++) {
                b32 digit = value >= '0' && value <= '9';
                b32 upper = value >= 'A' && value <= 'Z';
                b32 lower = value >= 'a' && value <= 'z';
                b32 space = value == ' ' || (value >= 9 && value <= 13);
                b32 hex = digit || (value >= 'a' && value <= 'f') ||
                          (value >= 'A' && value <= 'F');

                fail(!byte_is_digit(value) == !digit);
                fail(!byte_is_upper(value) == !upper);
                fail(!byte_is_lower(value) == !lower);
                fail(!byte_is_alpha(value) == !(upper || lower));
                fail(!byte_is_alnum(value) == !(upper || lower || digit));
                fail(!byte_is_space(value) == !space);
                fail(!byte_is_hexadecimal(value) == !hex);
        }

        return true;
}

//      Case, which must change exactly the twenty six that have another case
//      and leave every other byte as it found it.
test(byte_case) {
        for (b32 value = -1; value < 256; value++) {
                b32 up = (value >= 'a' && value <= 'z') ? value - 32 : value;
                b32 down = (value >= 'A' && value <= 'Z') ? value + 32 : value;

                fail(byte_to_upper(value) == up);
                fail(byte_to_lower(value) == down);
        }

        return true;
}

//      Bulk case conversion is exact and bounded even when a span straddles
//      a page boundary. The sentinels on both sides catch widened tails.
test(memory_ascii_case) {
        p8 original[4128];
        p8 expected[4128];
        p8 actual[4128];
        positive offsets[] = {0, 1, 7, 15, 31, 4093, 4095};
        positive lengths[] = {0, 1, 2, 7, 8, 15, 16, 17, 31, 32, 33};

        fail(memory_to_lower_ascii(null, 0) == null);
        fail(memory_to_upper_ascii(null, 0) == null);

        for (positive oi = 0; oi < sizeof(offsets) / sizeof(*offsets); oi++)
                for (positive li = 0; li < sizeof(lengths) / sizeof(*lengths); li++)
                {
                        positive offset = offsets[oi];
                        positive length = lengths[li];

                        if (offset + length > sizeof(original))
                                continue;

                        for (positive at = 0; at < sizeof(original); at++)
                                original[at] = (p8)(at * 37 + offset + length);

                        memory_copy(expected, original, sizeof(original));
                        memory_copy(actual, original, sizeof(original));

                        for (positive at = 0; at < length; at++)
                        {
                                p8 value = expected[offset + at];
                                if (value >= 'A' && value <= 'Z')
                                        expected[offset + at] = value + 32;
                        }

                        fail(memory_to_lower_ascii(actual + offset, length) ==
                             actual + offset);
                        fail(!memory_compare(actual, expected, sizeof(actual)));

                        memory_copy(expected, original, sizeof(original));
                        memory_copy(actual, original, sizeof(original));

                        for (positive at = 0; at < length; at++)
                        {
                                p8 value = expected[offset + at];
                                if (value >= 'a' && value <= 'z')
                                        expected[offset + at] = value - 32;
                        }

                        fail(memory_to_upper_ascii(actual + offset, length) ==
                             actual + offset);
                        fail(!memory_compare(actual, expected, sizeof(actual)));
                }

        return true;
}

test(memory_hash_33) {
        p8 bytes[4128];
        positive offsets[] = {0, 1, 3, 7, 15, 4093, 4095};

        for (positive at = 0; at < sizeof(bytes); at++)
                bytes[at] = (p8)(at * 37 + 11);

        for (positive oi = 0; oi < sizeof(offsets) / sizeof(*offsets); oi++)
                for (positive length = 0; length <= 257; length++)
                {
                        positive offset = offsets[oi];
                        positive wanted = 5381;

                        if (offset + length > sizeof(bytes))
                                continue;

                        for (positive at = 0; at < length; at++)
                                wanted = wanted * 33 + bytes[offset + at];

                        fail(memory_hash_33(bytes + offset, length) == wanted);
                }

        return true;
}

test(memory_span_byte) {
        p8 bytes[4128];
        positive page_lengths[] = {255, 256, 257, 4095};

        for (positive value = 0; value < 256; value++)
                for (positive offset = 0; offset < 16; offset++)
                        for (positive length = 0; length <= 33; length++)
                                for (positive mismatch = 0; mismatch <= length; mismatch++)
                                {
                                        if (offset)
                                                bytes[offset - 1] = 0xa5;
                                        memory_fill(bytes + offset, (p8)value, length);
                                        bytes[offset + length] = 0xa5;

                                        if (mismatch < length)
                                                bytes[offset + mismatch] = (p8)(value + 1);

                                        positive got = memory_span_byte(
                                            bytes + offset, (p8)value, length);

                                        if (got != mismatch)
                                                return false;
                                        if (offset)
                                                fail(bytes[offset - 1] == 0xa5);
                                        fail(bytes[offset + length] == 0xa5);
                                }

        for (positive value = 0; value < 256; value++)
                for (positive offset = 0; offset < 16; offset++)
                        for (positive li = 0;
                             li < sizeof(page_lengths) / sizeof(*page_lengths); li++)
                        {
                                positive length = page_lengths[li];
                                positive positions[] = {0, 1, 15, 16, length / 2,
                                                        length - 1, length};

                                if (offset + length >= sizeof(bytes))
                                        continue;

                                for (positive pi = 0;
                                     pi < sizeof(positions) / sizeof(*positions); pi++)
                                {
                                        positive mismatch = positions[pi];

                                        if (offset)
                                                bytes[offset - 1] = 0xa5;
                                        memory_fill(bytes + offset, (p8)value, length);
                                        bytes[offset + length] = 0xa5;
                                        if (mismatch < length)
                                                bytes[offset + mismatch] = (p8)(value + 1);

                                        fail(memory_span_byte(bytes + offset, (p8)value,
                                                              length) == mismatch);
                                        if (offset)
                                                fail(bytes[offset - 1] == 0xa5);
                                        fail(bytes[offset + length] == 0xa5);
                                }
                        }

        return true;
}

/*
        Square root, which is the hardware's own instruction.

        Exact powers of four have exact roots in binary floating point, so
        those are compared for equality rather than for nearness -- an
        implementation that approximated would fail here and a correct one
        cannot. The general case is checked by squaring the answer back.
*/
test(absolute_whole_and_wide) {
        b32 narrow[] = {0, 1, -1, 7, -7, 2147483647, -2147483647};
        bipolar wide[] = {0, 1, -1, 7, -7, 9223372036854775807LL,
                          -9223372036854775807LL};

        for (positive at = 0; at < sizeof(narrow) / sizeof(*narrow); at++)
                fail(absolute_whole(narrow[at]) ==
                     (narrow[at] < 0 ? -narrow[at] : narrow[at]));

        for (positive at = 0; at < sizeof(wide) / sizeof(*wide); at++)
                fail(absolute_wide(wide[at]) ==
                     (wide[at] < 0 ? -wide[at] : wide[at]));

        //      The narrow one must read a narrow register: a negative int
        //      widened by the caller leaves the top half set, and a 64 bit
        //      read of it would answer about a different number entirely.
        fail(absolute_whole(-5) == 5);
        fail(absolute_wide(-5) == 5);

        return true;
}

test(square_root_is_exact) {
        decimal exact[] = {0.0, 1.0, 4.0, 9.0, 16.0, 25.0, 64.0, 256.0,
                           1024.0, 65536.0, 1048576.0};
        decimal roots[] = {0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 8.0, 16.0,
                           32.0, 256.0, 1024.0};

        for (positive at = 0; at < sizeof(exact) / sizeof(*exact); at++)
                fail(square_root(exact[at]) == roots[at]);

        for (positive at = 1; at < 500; at++) {
                decimal value = (decimal)at;
                decimal root = square_root(value);
                decimal back = root * root;
                decimal off = back - value;

                if (off < 0.0)
                        off = -off;

                fail(off < 0.0001);
        }

        return true;
}

test(absolute_value) {
        decimal ordinary[] = {0.0, 1.5, -1.5, 1e10, -1e10, 1e-10, -1e-10};

        for (positive at = 0; at < sizeof(ordinary) / sizeof(*ordinary); at++) {
                decimal was = ordinary[at];
                decimal now = absolute(was);

                fail(now == (was < 0.0 ? -was : was));
                fail(!(now < 0.0));
        }

        //      Negative zero is the one that catches a subtraction pretending
        //      to be an absolute value: -0.0 is not less than zero, so a
        //      conditional negate leaves it alone and the sign bit stays.
        {
                decimal zero = 0.0;
                decimal minus = -zero;

                fail(absolute(minus) == 0.0);
        }

        return true;
}

test(system_write_all_zero_and_fault) {
        fail_not_equals(system_write_all((positive)-1, address_bad, 0), 0);
        fail_not_equals(system_write_all((positive)-1, (address_any)"x", 1), 0);

        return true;
}

/*
        A nonblocking pipe is a controllable positive-short-write followed by
        EAGAIN: offer more than its empty capacity, then leave it undrained
        while system_write_all runs. The answer must be the positive prefix,
        not the negative second result, and every reported byte must really
        be present in the pipe.
*/
test(system_write_all_partial_then_fault) {
        enum { F_GETPIPE_SZ = 1032 };
        b32 ends[2];
        p8 readback[4096];
        positive seen = 0;
        bool correct = true;

        fail_not_equals(system_call_2(syscall(pipe2), (positive)ends, O_NONBLOCK), 0);

        bipolar capacity = system_call_2(syscall(fcntl), ends[1], F_GETPIPE_SZ);
        fail(capacity > 0);

        positive offered_length = (positive)capacity + sizeof(readback);
        bipolar mapped = system_call_6(syscall(mmap), 0, offered_length,
                                       FILE_PROTECT_READ | FILE_PROTECT_WRITE,
                                       FILE_MAP_PRIVATE | FILE_MAP_ANONYMOUS,
                                       (positive)-1, 0);
        fail(mapped >= 0);

        memory_fill((address_any)(positive)mapped, 0x5a, offered_length);

        positive wrote = system_write_all((positive)ends[1],
                                           (address_any)(positive)mapped,
                                           offered_length);

        if (!wrote || wrote >= offered_length)
                correct = false;

        while (correct && seen < wrote)
        {
                positive want = wrote - seen;

                if (want > sizeof(readback))
                        want = sizeof(readback);

                bipolar got = system_call_3(syscall(read), ends[0],
                                             (positive)readback, want);

                if (got <= 0)
                {
                        correct = false;
                        break;
                }

                for (positive i = 0; i < (positive)got; i++)
                        if (readback[i] != 0x5a)
                                correct = false;

                seen += (positive)got;
        }

        system_call_1(syscall(close), ends[0]);
        system_call_1(syscall(close), ends[1]);
        system_call_2(syscall(munmap), (positive)mapped, offered_length);

        fail(correct && seen == wrote);
        return true;
}

/*
        Linux caps one write syscall at MAX_RW_COUNT. /dev/null consumes no
        pages, so a 2 GiB anonymous mapping makes that cap force two positive
        writes without allocating or storing 2 GiB. Returning the whole span
        proves the loop advanced the pointer/count and retried after progress.
*/
test(system_write_all_retries_short_progress) {
        positive span = (positive)0x80000000u;
        bipolar mapped = system_call_6(syscall(mmap), 0, span, FILE_PROTECT_READ,
                                       FILE_MAP_PRIVATE | FILE_MAP_ANONYMOUS,
                                       (positive)-1, 0);
        fail(mapped >= 0);

        bipolar sink = system_call_4(syscall(openat), AT_FDCWD,
                                     (positive)"/dev/null", FILE_READ_WRITE, 0);

        if (sink < 0)
        {
                system_call_2(syscall(munmap), (positive)mapped, span);
                return false;
        }

        positive wrote = system_write_all((positive)sink,
                                           (address_any)(positive)mapped, span);

        system_call_1(syscall(close), sink);
        system_call_2(syscall(munmap), (positive)mapped, span);

        fail_not_equals(wrote, span);
        return true;
}

#endif


test_case test_cases[] = {

        case_type_basics(p8),
        case_type_basics(b8),
        case_type_basics(p16),
        case_type_basics(b16),
        case_type_basics(p32),
        case_type_basics(b32),
        case_type_basics(p64),
        case_type_basics(b64),
#ifdef HAS_128
        case_type_basics(p128),
        case_type_basics(b128),
#endif
        case_type_decimal(f32),
        case_type_decimal(f64),
        case_type_decimal(f128),

        case(bit_flip_zero_to_one),
        case(bit_flip_one_to_zero),
        case(bit_set_basic),
        case(bit_clear_basic),
        case(bit_test_set_bit),
        case(bit_test_clear_bit),
        case(bit_set_high_bit),

        case(addresses),
        case(is_null),
        case(atomic_operations),
        
        case(memory_fill),
        case(memory_copy),
        
        case(string_length),
        case(string_compare),
        case(string_copy),
        case(string_copy_max),
        case(string_first_of),
        case(string_last_of),
        case(string_cut),
        case(string_replace_all),
        case(string_format_basic),
        case(string_format_numbers),
        case(string_format_mixed),
        
        case(path_basename),
        
        case(str_macro),
        
        case(string_end),

        case(writer_pattern),

        case(str_length),
        case(str_writes_no_terminator),

        case(string_length_padded),
        case(string_compare_padded),
        case(string_first_of_padded),
        case(string_table_find_lengths),

        case(stack_is_aligned),

#if LINUX
        case(syscall_argument_four),
        case(created_file_mode),
        case(byte_classes),
        case(byte_case),
        case(memory_ascii_case),
        case(memory_hash_33),
        case(memory_span_byte),
        case(absolute_whole_and_wide),
        case(square_root_is_exact),
        case(absolute_value),
        case(system_write_all_zero_and_fault),
        case(system_write_all_partial_then_fault),
        case(system_write_all_retries_short_progress),
#endif

        {null, null},
};

// Negative until generate_report opens it, so a stray write cannot land on
// descriptor 0.
bipolar report_file = -1;

// Relative to the working directory the runner is launched from.
const_string report_path = (const_string) "docs/index.html";

fn report_writer(address_any data, positive length)
{
        if (length == 0)
                length = string_length(data);

        system_call_3(syscall(write), report_file, (positive)data, length);
}

fn generate_report(writer write)
{
        // This used to redeclare report_file, shadowing the global that
        // report_writer actually writes through -- so even when the open
        // succeeded every write went to descriptor 0 instead of the report.
        report_file = system_call_4(syscall(openat), AT_FDCWD, (positive)report_path, FILE_CREATE | FILE_WRITE | O_TRUNC, 0666);

        if (report_file < 0) {
                // Writing the report is optional; the pass/fail summary above
                // is the actual test result, so this is a note, not a failure.
                log_direct(str("(no HTML report written; set REPORT_PATH target directory to enable)\n"));
                return;
        }

        write(str("<html><head><title>C library tests</title></head>"));
        write(str("<body><h1>C library tests</h1>"));

        // table
        write(str("<table border=\"1\"><tr><th>Test</th><th>Result</th></tr>"));
        
        test_case address_to test = test_cases;

        while (test->name)
        {
                write(str("<tr><td>"));
                write(test->name, 0);
                write(str("</td><td>"));

                if (test->result) {
                        write(str("Passed"));
                } else {
                        write(str("Failed"));
                }

                write(str("</td></tr>"));
                test++;
        }
        write(str("</table>"));
        write(str("</body></html>"));

        system_call_1(syscall(close), report_file);
}

b32 main()
{
        test_case address_to test = test_cases;
        positive passed = 0;
        positive failed = 0;

        log_direct(str("C library tests\n\n"));

        while (test->name)
        {
                log_direct(test->name, string_length(test->name));

                bool result = test->function();

                test->result = result;

                if (!result) {
                        log_direct(str(" ----- FAILED\n"));
                        failed++;
                }
                else {
                        log_direct(str(" PASSED\n"));
                        passed++;
                }
                test++;
        }

        string_format(log, "\n%p passed, %p failed\n", passed, failed);

        log_flush();

        #if LINUX
        generate_report(report_writer);
        #endif

        return failed > 0 ? 1 : 0;
}
