/*
        The shell's lexer.

        Turns a line into tokens: words, operators and the end. What a word
        means is not decided here -- the quotes stay in it, because POSIX does
        quote removal last, after every expansion has had its turn, and a lexer
        that strips them early cannot tell "$x" from $x afterwards.

        The scanning is string_span from library.c, which is assembly on every
        architecture and reads about two bytes a cycle. A lexer is almost
        entirely that one operation: run to the next thing that matters. Every
        set below is prepared once, so the inner loop never asks what kind of
        byte it is holding.
*/

#define LEX_END 0
#define LEX_WORD 1
#define LEX_OPERATOR 2

// The operators, longest first, so that && is never read as two &.
#define OP_AND_IF 1     // &&
#define OP_OR_IF 2      // ||
#define OP_DSEMI 3      // ;;
#define OP_DLESS 4      // <<
#define OP_DGREAT 5     // >>
#define OP_LESSAND 6    // <&
#define OP_GREATAND 7   // >&
#define OP_LESSGREAT 8  // <>
#define OP_CLOBBER 9    // >|
#define OP_SEMI 10      // ;
#define OP_PIPE 11      // |
#define OP_AMP 12       // &
#define OP_LESS 13      // <
#define OP_GREAT 14     // >
#define OP_LPAREN 15    // (
#define OP_RPAREN 16    // )

#define LEX_TOKENS 256
#define LEX_TEXT 8192

typedef struct
{
        b32 kind;
        b32 op;
        string_address text;
        positive length;
        // Where this token began in the line it came from, so a caller can cut
        // the line at an operator without rebuilding what it already read.
        positive at;
} lex_token;

static lex_token lex_tokens[LEX_TOKENS];
static p8 lex_text[LEX_TEXT];
static positive lex_used;
static b32 lex_count;

/*
        The byte sets, built once.

        blank        what separates one word from the next
        metachar     what ends a word without being part of it
        ordinary     everything a word may contain without further thought,
                     which is the complement of the two above plus the things
                     that begin a quote or an expansion
*/
/*
        One class per byte, not one set per question.

        The scanner asks what kind of byte it is holding once, and the answer
        is a single load. Three separate membership tests would be three, and
        the assembly below is built around this table being the only thing it
        has to consult.
*/
#define LEX_ORDINARY 0
#define LEX_BLANK 1
#define LEX_OP 2
#define LEX_QUOTE 3
#define LEX_ESCAPE 4
#define LEX_STOP 5

static b8 lex_class[256];
static b8 lex_blank[STRING_SET_BYTES];
static b8 lex_ordinary[STRING_SET_BYTES];
static b8 lex_operator[STRING_SET_BYTES];
static b32 lex_ready;

fn lex_prepare()
{
        if (lex_ready)
                return;

        memory_fill(lex_blank, 0, sizeof(lex_blank));
        memory_fill(lex_ordinary, 0, sizeof(lex_ordinary));
        memory_fill(lex_operator, 0, sizeof(lex_operator));

        string_set_add(lex_blank, " \t");
        string_set_add(lex_operator, "|&;<>()");

        /*
                Everything a word may hold without a decision being needed.

                The complement of what ends a word, begins a quote, escapes, or
                starts an expansion -- and never the terminator, or a run would
                walk off the end of the string.
        */
        for (positive c = 1; c < STRING_SET_BYTES; c++)
                lex_ordinary[c] = 1;

        {
                static const string_address decides = " \t\n|&;<>()'\"\\$`";

                for (positive i = 0; decides[i]; i++)
                        lex_ordinary[decides[i]] = 0;
        }

        // The same knowledge as the sets above, as one byte per byte value.
        memory_fill(lex_class, LEX_ORDINARY, sizeof(lex_class));

        lex_class[0] = LEX_STOP;
        lex_class['\n'] = LEX_STOP;
        lex_class['#'] = LEX_STOP;
        lex_class[' '] = LEX_BLANK;
        lex_class['\t'] = LEX_BLANK;
        lex_class['\''] = LEX_QUOTE;
        lex_class['"'] = LEX_QUOTE;
        lex_class['\\'] = LEX_ESCAPE;

        {
                static const string_address ops = "|&;<>()";

                for (positive i = 0; ops[i]; i++)
                        lex_class[ops[i]] = LEX_OP;
        }

        lex_ready = true;
}

// Which operator starts here, and how long it is. Two-byte forms are tested
// first, so >> is never a > followed by another.
static b32 lex_operator_at(string_address at, positive address_to length)
{
        p8 a = string_get(at);
        p8 b = string_get(at + 1);

        address_to length = 2;

        if (a == '&' && b == '&')
                return OP_AND_IF;
        if (a == '|' && b == '|')
                return OP_OR_IF;
        if (a == ';' && b == ';')
                return OP_DSEMI;
        if (a == '<' && b == '<')
                return OP_DLESS;
        if (a == '>' && b == '>')
                return OP_DGREAT;
        if (a == '<' && b == '&')
                return OP_LESSAND;
        if (a == '>' && b == '&')
                return OP_GREATAND;
        if (a == '<' && b == '>')
                return OP_LESSGREAT;
        if (a == '>' && b == '|')
                return OP_CLOBBER;

        address_to length = 1;

        if (a == ';')
                return OP_SEMI;
        if (a == '|')
                return OP_PIPE;
        if (a == '&')
                return OP_AMP;
        if (a == '<')
                return OP_LESS;
        if (a == '>')
                return OP_GREAT;
        if (a == '(')
                return OP_LPAREN;
        if (a == ')')
                return OP_RPAREN;

        address_to length = 0;
        return 0;
}

static positive lex_at;

static b32 lex_add(b32 kind, b32 op, string_address text, positive length)
{
        if (lex_count >= LEX_TOKENS)
                return false;

        lex_tokens[lex_count].at = lex_at;
        lex_tokens[lex_count].kind = kind;
        lex_tokens[lex_count].op = op;
        lex_tokens[lex_count].text = text;
        lex_tokens[lex_count].length = length;
        lex_count++;

        return true;
}

/*
        One word, quotes and all.

        A run of ordinary bytes is taken whole by string_span; anything else is
        one byte of decision and then another run. A quote swallows to its
        partner, a backslash swallows the byte after it, and both stay in the
        text for the expander to deal with in its own order.
*/
static b32 lex_word(string_address address_to at)
{
        string_address step = address_to at;
        positive start = lex_used;

        while (1)
        {
                positive run = string_span(step, lex_ordinary);

                if (run)
                {
                        if (lex_used + run >= LEX_TEXT)
                                return false;

                        memory_copy(lex_text + lex_used, step, run);
                        lex_used += run;
                        step += run;
                        continue;
                }

                p8 c = string_get(step);

                if (!c || lex_blank[c] || lex_operator[c] || c == '\n')
                        break;

                if (lex_used + 2 >= LEX_TEXT)
                        return false;

                lex_text[lex_used++] = c;
                step++;

                if (c == '\'' || c == '"')
                {
                        // To the matching quote, or to the end if there is
                        // none -- an unterminated quote is the parser's to
                        // complain about, not the lexer's to guess at.
                        while (string_get(step) && string_get(step) != c)
                        {
                                if (lex_used + 2 >= LEX_TEXT)
                                        return false;

                                if (c == '"' && string_get(step) == '\\' &&
                                    string_get(step + 1))
                                        lex_text[lex_used++] = string_get(step++);

                                lex_text[lex_used++] = string_get(step++);
                        }

                        if (string_get(step))
                                lex_text[lex_used++] = string_get(step++);

                        continue;
                }

                if (c == '\\' && string_get(step))
                        lex_text[lex_used++] = string_get(step++);
        }

        lex_text[lex_used++] = end;
        address_to at = step;

        return lex_add(LEX_WORD, 0, lex_text + start, lex_used - start - 1);
}

/*
        A whole line into tokens.

        Returns how many, or -1 when there were more than there is room for.
        A comment runs to the end of the line and is not a token.
*/
b32 lex_line(string_address line)
{
        string_address step = line;

        lex_prepare();
        lex_count = 0;
        lex_used = 0;

        while (1)
        {
                positive length;
                b32 op;

                step += string_span(step, lex_blank);

                if (!string_get(step) || string_get(step) == '\n')
                        break;

                // A comment only begins where a word could have.
                if (string_get(step) == '#')
                        break;

                lex_at = (positive)(step - line);
                op = lex_operator_at(step, address_of length);

                if (op)
                {
                        if (!lex_add(LEX_OPERATOR, op, step, length))
                                return -1;

                        step += length;
                        continue;
                }

                lex_at = (positive)(step - line);

                if (!lex_word(address_of step))
                        return -1;
        }

        lex_at = (positive)(step - line);
        lex_add(LEX_END, 0, null, 0);

        return lex_count - 1;
}
