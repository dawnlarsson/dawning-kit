/*
        What every counted test in this directory does the same way.

        A counted test asks a list of questions, keeps two numbers -- how many
        it asked and how many disagreed -- prints one line the runner greps
        for, and leaves with one when anything disagreed. Twenty one files in
        here were each carrying their own copy of exactly that: the same two
        counters, the same eight line macro, the same three line ending. The
        drift you get from writing one thing twenty one times had already
        arrived -- two spellings of the macro's braces, four spellings of the
        label in front of the count, two spellings of the same return -- so
        this is the one copy and those are gone.

        The counters and the ending are what all twenty one share. check is
        what most of them ask with; the rest have a typed helper of their own
        -- same, judge, fail -- that counts the same two numbers and says more
        about the disagreement than a name can, and they keep it. Including
        this file is a claim about the counters and the ending, not a promise
        to ask through check.

        check is a macro and not a function, which is not a style choice.
        Two things depend on it:

        The name is pasted into the format string rather than passed as an
        argument, so the whole message is one literal the compiler assembles
        and a test that never fails never builds a string at runtime.

        The condition is evaluated where it is written rather than as an
        argument. bool is p8 here, so a condition that answers a pointer or a
        wide word would be narrowed to its low byte on the way into a call --
        and a non-null pointer whose low byte happens to be zero would come
        out false. Written as a macro, !(condition) is the C the author meant.
*/

static positive checks;
static positive failures;

#define check(name, condition)                                          \
        do {                                                            \
                checks++;                                               \
                if (!(condition)) {                                     \
                        failures++;                                     \
                        string_format(log, "  FAIL " name "\n");        \
                }                                                       \
        } while (0)

/*
        The last thing a counted test does, and the answer main returns.

        label is what goes in front of the count for the lanes that name
        themselves there -- "bounded: ", a bare newline -- and null for the
        ones that do not. The flush is here because the count line is the
        only thing the runner reads, and a test that computed it and left it
        in the buffer would look like a test that never ran.
*/
static b32 test_report(string_address label)
{
        string_format(log, "%s%p checks, %p failures\n",
                      is_null(label) ? (string_address) "" : label,
                      checks, failures);
        log_flush();

        return failures ? 1 : 0;
}
