/* Build with kit/build. Compare complete-literal shortcuts with their original
   VM fallback, using the real shared library and injected resource budgets. */
#include "../src/compiler_memory.c"
#include "../src/spark.c"
#include "../src/sh/shell.c"

static positive proof_checks, proof_failures;

static fn proof_check(bool condition)
{
        proof_checks++;
        proof_failures += !condition;
}

b32 main(void)
{
        static string_address patterns[] = {
            "(ab){32}", "((ab){4}){8}", "(ab){128}", "(ab){129}",
            "(a){1}", "a{0}b", "(a())b", "(a)(b)", "(ab){32}z",
            "(ab){1,2}", "(ab)*", "(a|b){2}", "(ab){2}\\1", "(^ab){2}",
        };
        static string_address subjects[] = {
            "", "a", "ab", "abab", "aabb", "xabab", "ABAB", "ab\nab",
        };
        static positive budgets[] = {0, 1, 5, 50, 100000000};
        static p32 capacities[] = {0, 2, RX_NODE_MAX - 1, RX_NODE_MAX};
        p8 repeated[260];
        for (positive i = 0; i < sizeof(repeated); i++)
                repeated[i] = i & 1 ? 'b' : 'a';

        for (positive p = 0; p < array_count(patterns); p++)
        for (b32 fold = 0; fold < 2; fold++)
        {
                regex_pool.used = (rx_mark){0};
                regex_program program;
                bool compiled = rx_compile(&regex_pool, &program, patterns[p], true, fold, true,
                                           REGEX_POLICY_DEFAULT);
                proof_check(compiled);
                if (!compiled)
                        continue;
                if (p < 4)
                {
                        proof_check(!(program.flags & RX_LITERAL_PROVES));
                        proof_check(program.hints->literal_length == 2 &&
                                    !memory_compare(program.hints->literal, "ab", 2));
                        proof_check(program.hints->fixed_length == (p < 2 ? 64 : p == 2 ? 256 : 258));
                        proof_check(program.hints->fixed_work != 0);
                }
                rx_hints interpreter_hints = *program.hints;
                interpreter_hints.fixed_work = 0;
                regex_program interpreter = program;
                interpreter.hints = &interpreter_hints;
                for (positive s = 0; s < array_count(subjects) + 5; s++)
                {
                        string_address bytes = s < array_count(subjects) ? subjects[s] : repeated;
                        positive length = s < array_count(subjects) ? string_length(bytes) :
                            s == array_count(subjects) ? 63 :
                            s == array_count(subjects) + 1 ? 64 :
                            s == array_count(subjects) + 2 ? 65 :
                            s == array_count(subjects) + 3 ? 256 : 258;
                        for (p8 mode = REGEX_FIRST; mode <= REGEX_EXACT_LONGEST; mode++)
                        for (p8 boundary = REGEX_BOUNDARY_NONE; boundary <= REGEX_BOUNDARY_LINE; boundary++)
                        for (p8 capture = 0; capture < 2; capture++)
                        for (p8 pending = 0; pending < 2; pending++)
                        for (positive work = 0; work < array_count(budgets); work++)
                        for (positive room = 0; room < array_count(capacities); room++)
                        {
                                program.boundary = interpreter.boundary = boundary;
                                rx_match fast = regex_match;
                                fast.work_limit = budgets[work];
                                fast.frame_capacity = capacities[room];
                                fast.choice_capacity = fast.undo_capacity = room ? REGEX_SCRATCH_MAX : 0;
                                fast.pending_exhaustion = pending;
                                rx_match slow = fast;
                                p8 one = rx_find(&fast, &program, mode, capture, bytes, length, 0);
                                p8 two = rx_find(&slow, &interpreter, mode, capture, bytes, length, 0);
                                proof_check(one == two && fast.pending_exhaustion == slow.pending_exhaustion);
                                if (one == RX_MATCH && two == RX_MATCH)
                                        proof_check(!memory_compare(fast.slots, slow.slots,
                                            (capture ? (program.groups + 1) * 2 : 2) * sizeof(positive)));
                        }
                }
        }
        /* Reuse the same hint after its complete buffer held a maximal
           literal. Short, empty-count and unsupported proofs must not see it. */
        p8 longest[RX_NODE_MAX];
        memory_fill(longest, 'a', sizeof(longest) - 1);
        longest[sizeof(longest) - 1] = 0;
        string_address reuse_patterns[] = {longest, "(ab){2}", "a{0}b", "[a-z]+", "((a){0}b){2}"};
        string_address reuse_subjects[] = {longest, "abab", "b", "xyz", "bb"};
        regex_retained = (rx_mark){0};
        for (positive round = 0; round < 4; round++)
        for (positive i = 0; i < array_count(reuse_patterns); i++)
        {
                proof_check(regex_compile(reuse_patterns[i], true, false, true, REGEX_POLICY_DEFAULT));
                positive length = string_length(reuse_subjects[i]);
                proof_check(regex_find(REGEX_LONGEST, reuse_subjects[i], length, 0));
                proof_check(regex_slots[0] == 0 && regex_slots[1] == length);
        }
        string_format(log, "%p checks, %p failures\n", proof_checks, proof_failures);
        log_flush();
        return proof_failures != 0;
}
