/* Counted lexer views borrow input; parser text must survive its reuse.
   Build with kit/build or the shell lane's freestanding-checks runner. */
#include "../src/compiler_memory.c"
#include "../src/spark.c"
#include "../src/sh/shell.c"
#define SHARED_counted
#include "checks.c"
#undef SHARED_counted

static fn lexer_span_owned(p8 *line, p8 *continuation,
                           string_address const *words, positive count)
{
        parse_reset();
        bool ready = parse_feed(line);
        if (ready && continuation) ready = parse_feed(continuation);
        check("parser accepts source", ready);
        if (!ready) return;
        memory_fill(line, 'x', string_length(line));
        if (continuation) memory_fill(continuation, 'y', string_length(continuation));
        check("parser token count", parse_token_count == count + 1);
        if (parse_token_count != count + 1) return;
        for (positive i = 0; i < count; i++)
                check("parser owns terminated spelling",
                      parse_tokens[i].kind == PT_WORD &&
                      parse_tokens[i].length == string_length(words[i]) &&
                      !string_compare(parse_tokens[i].text, words[i]));
}

b32 main(void)
{
        shell_bash_compat = true;
        static const struct { string_address text; positive length; b32 kind; } cases[] = {
#define SPAN(word, kind) {word ";tail", sizeof(word) - 1, kind}
            SPAN("plain", LEX_WORD), SPAN("'one two'", LEX_WORD),
            SPAN("\"${x:-two words}\"", LEX_WORD), SPAN("escaped\\ word", LEX_WORD),
            SPAN("a[1 + 2]=x", LEX_WORD), SPAN("a=(one 'two three')", LEX_WORD),
            SPAN("[[ x == x ]]", LEX_CONDITIONAL), SPAN("((a=1+2))", LEX_ARITHMETIC),
            SPAN("\"$(cat <<'E'\ninside ) text\nE\n)\"", LEX_WORD),
#undef SPAN
        };
        for (positive i = 0; i < array_count(cases); i++)
        {
                b32 count = lex_line(cases[i].text);
                check("word, separator, tail", count == 3);
                if (count != 3) continue;
                check("whole token borrows exact source", lex_tokens[0].kind == cases[i].kind &&
                      lex_tokens[0].text == cases[i].text && lex_tokens[0].length == cases[i].length);
                check("counted end is a separator", lex_tokens[0].text[lex_tokens[0].length] == ';' &&
                      lex_tokens[1].kind == LEX_OPERATOR && lex_tokens[1].op == OP_SEMI);
                check("following token retains its own span", lex_tokens[2].kind == LEX_WORD &&
                      lex_tokens[2].text == cases[i].text + cases[i].length + 1 && lex_tokens[2].length == 4);
        }

        p8 *pages = memory(3 * 4096);
        check("guard allocation", (bipolar)(positive)pages > 0);
        if ((bipolar)(positive)pages <= 0) return test_report(null);
        bool guarded = !system_call_3(syscall(mprotect), (positive)pages, 4096, 0) &&
            !system_call_3(syscall(mprotect), (positive)(pages + 8192), 4096, 0);
        check("guard protection", guarded);
        if (guarded)
        {
                static const positive sizes[] = {1,2,3,4,7,8,15,16,31,32,63,64,127,128,255,256};
                for (positive i = 0; i < array_count(sizes); i++)
                {
                        positive length = sizes[i];
                        p8 *line = pages + 8191 - length;
                        memory_fill(line, 'a', length);
                        line[length] = 0;
                        check("guarded word borrows complete span", lex_line(line) == 1 &&
                              lex_tokens[0].text == line && lex_tokens[0].length == length);
                }
        }
        memory_free(pages, 3 * 4096);

        static p8 many[2049];
        for (positive i = 0; i < 1024; i++) { many[2*i] = 'a'; many[2*i+1] = ' '; }
        b32 grown = lex_line(many);
        check("grown token table", grown == 1024);
        if (grown != 1024) return test_report(null);
        for (positive i = 0; i < 1024; i++)
                check("table growth preserves each source address",
                      lex_tokens[i].text == many + 2*i && lex_tokens[i].length == 1);
        lex_token held = lex_tokens[500];
        lex_frame frame;
        lex_nest_enter(&frame);
        check("nested lexer input", lex_line("'nested source'") == 1);
        lex_nest_leave(&frame);
        check("outer table restored", lex_count == 1025 && lex_tokens[500].text == held.text &&
              lex_tokens[500].length == held.length && lex_tokens[500].text[0] == 'a');

        p8 plain[] = ": plain 'quoted word' \\x";
        string_address plain_words[] = {":", "plain", "'quoted word'", "\\x"};
        lexer_span_owned(plain, null, plain_words, array_count(plain_words));
        p8 first[] = ": 'held", second[] = "more' tail";
        string_address held_words[] = {":", "'held\nmore'", "tail"};
        lexer_span_owned(first, second, held_words, array_count(held_words));
        p8 nested[] = ": \"$(cat <<'E'\ninside\nE\n)\" tail";
        string_address nested_words[] = {":", "\"$(cat <<'E'\ninside\nE\n)\"", "tail"};
        lexer_span_owned(nested, null, nested_words, array_count(nested_words));
        return test_report(null);
}
