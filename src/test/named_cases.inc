/*
        The named-case shape, which is the other way a test here is written.

        A counted test asks its questions inline. A named-case test is a list
        of named functions that each answer true or false, walked in order,
        with the name printed as it starts and PASSED or FAILED printed after
        it. Four files -- standard, stdlib, math, leaving -- were each
        carrying their own copy of the same three macros, the same two type
        declarations and the same twenty line walk.

        counted.inc comes in below, and is where checks, failures and
        test_report come from: a case that ran is a check and a case that
        failed is a failure, so the two shapes count the same two numbers and
        three of these four suites end with the same line a counted test ends
        with. Nothing here needs its check macro, but a suite is free to ask
        through it.

        test(name) opens one. fail(condition) is how a case gives up: it
        returns false the moment something is not what it should be, so what
        gets reported is the first disagreement rather than the last.
        case(name) puts one in the table and spells the name out of the token,
        so the printed name and the called function cannot drift apart. A
        table ends with a zeroed entry.

        result is kept on the entry rather than only counted because one of
        the four walks the table a second time afterwards to write an HTML
        report out of it.

        What follows the walk stays with each suite, because the four of them
        deliberately do not end the same way: one leaves through exit rather
        than a return, because it is the family that knows the difference; one
        writes its verdict and deliberately never flushes, because the bytes
        arriving at all is the claim that file exists to make; and one says
        passed and failed where the others say checks and failures.
*/

#include "counted.inc"

#define test(test_name) bool test_##test_name(void)
#define case(test_name)                        \
        {                                      \
                (string_address)#test_name,    \
                test_##test_name               \
        }
#define fail(condition)   \
        if (!(condition)) \
        return false

typedef bool(address_to test_function)(void);

typedef struct
{
        string_address name;
        test_function function;
        bool result;
} test_case;

/*
        Runs a table in order and leaves the two counters holding how many
        ran and how many failed, which is what every one of the four ending
        lines is written out of.
*/
static fn test_cases_walk(test_case address_to walk)
{
        while (walk->name)
        {
                log_direct(walk->name, string_length(walk->name));

                walk->result = walk->function();

                checks++;

                if (walk->result)
                        log_direct(str(" PASSED\n"));
                else
                {
                        failures++;
                        log_direct(str(" ----- FAILED\n"));
                }

                walk++;
        }
}
