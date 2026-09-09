/*
        Does calling our utilities one after another in one address space give
        the same answers as calling each in its own process?

        The instance audit says tools keep static arenas and must not share an
        address space concurrently. Sequentially is a different claim and this
        is the ten minute experiment that settles it before build.c is written
        around the answer.
*/
#include "../src/compiler_memory.c"
#include "../src/spark.c"
#include "../src/sh/shell.c"

static string_address probe_words[16];

static b32 probe_run(string_address address_to words)
{
        string_address address_to saved = program_argument_list();
        b32 saved_count = program_argument_count();
        b32 count = 0;
        b32 answer;

        while (words[count])
                count++;

        log_flush();
        program_arguments_use((string_address address_to)words, count);
        answer = shell_tool_as_called();
        program_arguments_use(saved, saved_count);
        log_flush();

        return answer;
}

static b32 probe(string_address one, string_address two, string_address three,
                 string_address four, string_address five)
{
        probe_words[0] = one;
        probe_words[1] = two;
        probe_words[2] = three;
        probe_words[3] = four;
        probe_words[4] = five;
        probe_words[5] = null;

        return probe_run(probe_words);
}

b32 main()
{
        b32 answer;

        string_format(log, "-- round one\n");
        log_flush();
        answer = probe("mkdir", "-p", "scratch/probework/a/b", null, null);
        string_format(log, "mkdir -> %d\n", answer);
        answer = probe("cp", "src/spark.c", "scratch/probework/a/b/one", null, null);
        string_format(log, "cp -> %d\n", answer);
        answer = probe("grep", "-c", "spark", "scratch/probework/a/b/one", null);
        string_format(log, "grep -> %d\n", answer);
        answer = probe("wc", "-c", "scratch/probework/a/b/one", null, null);
        string_format(log, "wc -> %d\n", answer);

        string_format(log, "-- round two, the same calls again\n");
        log_flush();
        answer = probe("mkdir", "-p", "scratch/probework/a/b", null, null);
        string_format(log, "mkdir -> %d\n", answer);
        answer = probe("cp", "src/spark.c", "scratch/probework/a/b/two", null, null);
        string_format(log, "cp -> %d\n", answer);
        answer = probe("grep", "-c", "spark", "scratch/probework/a/b/two", null);
        string_format(log, "grep -> %d\n", answer);
        answer = probe("wc", "-c", "scratch/probework/a/b/two", null, null);
        string_format(log, "wc -> %d\n", answer);

        string_format(log, "-- round three, options that set state\n");
        log_flush();
        answer = probe("ln", "-sf", "one", "scratch/probework/a/b/link", null);
        string_format(log, "ln -> %d\n", answer);
        answer = probe("find", "scratch/probework", "-type", "f", null);
        string_format(log, "find -> %d\n", answer);
        answer = probe("sort", "-r", "scratch/probework/a/b/one", null, null);
        string_format(log, "sort(discarded) -> %d\n", answer);
        answer = probe("find", "scratch/probework", "-type", "l", null);
        string_format(log, "find again -> %d\n", answer);
        answer = probe("rm", "-rf", "scratch/probework", null, null);
        string_format(log, "rm -> %d\n", answer);

        log_flush();
        return 0;
}
