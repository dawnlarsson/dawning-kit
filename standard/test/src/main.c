#include "../../library.c"

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

typedef bool(address_to dawn_test_function)();

typedef struct
{
        string_address name;
        dawn_test_function function;
        bool result;
} dawn_test;

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

#define case_type_basics(type_name) \
        case(type_name##_sizeof), \
        case(type_name##_bytes_constant), \
        case(type_name##_max), \
        case(type_name##_min), \
        case(type_name##_overflow), \
        case(type_name##_underflow)

test_type_basics(p8, 255, 0);
test_type_basics(b8, 127, -128);

test_type_basics(p16, 65535, 0);
test_type_basics(b16, 32767, -32768);

test_type_basics(p32, 4294967295U, 0);
test_type_basics(b32, 2147483647, -2147483648);

test_type_basics(p64, 18446744073709551615U, 0);
test_type_basics(b64, 9223372036854775807, -9223372036854775808);

test_type_basics(p128, 340282366920938463463374607431768211455U, 0);
test_type_basics(b128, 170141183460469231731687303715884105727, -170141183460469231731687303715884105728);

test_type_basics(f32, 3.402823466e+38f, 1.175494351e-38f);
test_type_basics(f64, 1.7976931348623157e+308, 2.2250738585072014e-308);
test_type_basics(f128, 1.189731495357231765e+4932L, 3.362103143112093506e-4932L);

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
        fail_not_equals(string_length("Dawning\0Hidden"), 7);
        
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

        /*
        string_copy_max(dest, "Hi", 10);
        fail_not_equals(dest[2], end);
        */

        string_copy_max(dest, "Hello", 0);
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
        
        string_format(test_writer, "%s has %p items worth %b each", "Dawn", 5, -10);

        test_write_buffer[test_write_pos] = end;

        fail_not_equals(string_compare(test_write_buffer, "Dawn has 5 items worth -10 each"), 0);
        
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


dawn_test dawn_tests[] = {

        case_type_basics(p8),
        case_type_basics(b8),
        case_type_basics(p16),
        case_type_basics(b16),
        case_type_basics(p32),
        case_type_basics(b32),
        case_type_basics(p64),
        case_type_basics(b64),
        case_type_basics(p128),
        case_type_basics(b128),
        case_type_basics(f32),
        case_type_basics(f64),
        case_type_basics(f128),

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
        
        {null, null},
};

bipolar report_file = 0;

fn report_writer(address_any data, positive length)
{
        if (length == 0)
                length = string_length(data);

        system_call_3(syscall(write), report_file, (positive)data, length);
}

fn generate_report(writer write)
{
        bipolar report_file = system_call_4(syscall(openat), AT_FDCWD, (positive)"../../docs/index.html", FILE_CREATE | FILE_WRITE | O_TRUNC, 0666);

        if (report_file < 0) {
                log_direct(str("Failed to open output report file.\n"));
                return;
        }

        write(str("<html><head><title>Dawning C Library Tests</title></head>"));
        write(str("<body><h1>Dawning C Library Tests</h1>"));

        // table
        write(str("<table border=\"1\"><tr><th>Test</th><th>Result</th></tr>"));
        
        dawn_test address_to test = dawn_tests;

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
        dawn_test address_to test = dawn_tests;
        positive passed = 0;
        positive failed = 0;

        log_direct(str("Dawning C Library Tests\n\n"));

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