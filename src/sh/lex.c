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
#define LEX_ARITHMETIC 3
#define LEX_CONDITIONAL 4

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
#define OP_ANDGREAT 17  // &>
#define OP_ANDDGREAT 18 // &>>
#define OP_HERESTRING 19 // <<<
/* Appended, and never inserted among the ones above: parse.c tells a
   redirect from anything else by the number's range, so a new value in the
   middle would silently reclassify the operators behind it. */
#define OP_PIPEAND 20   // |&
#define OP_SEMIAND 21   // ;&
#define OP_DSEMIAND 22  // ;;&


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

/*
        The tokens of one line, and the bytes they were cut from.

        Both grow. The table may move freely; the bytes may not, because every
        token already handed out holds an address inside them -- so when the
        bytes do move, those addresses are moved with them by the same delta.
        That is cheap and exact: they all point into the one block, so one
        subtraction rebases the lot.

        Eight kilobytes of text and 256 tokens used to be the ceiling, and a
        line past it was refused, which a generated command list or one very
        long argument reaches without trying.
*/
typedef struct
{
        lex_token address_to tokens;
        positive token_room;
        p8 address_to text;
        positive text_room;
        positive used;
        b32 count;
        /*
                Which line of this input the shell is running.

                The lexer is handed one line at a time and used to keep no
                count of them, so a function had no line to say it was called
                from and $LINENO had nothing to read. It belongs in the frame
                for the same reason everything else here does: a nested source
                -- an eval, or a dot file -- counts its own lines from one and
                the outer count comes back when it ends.
        */
        positive line;
} lex_frame;

/*
        A nested source gets empty lexer storage of its own. Keeping the outer
        blocks rather than copying them preserves every token address while
        eval or dot grows and frees the nested blocks independently. Current
        and saved state have one shape, so a new store cannot be omitted from
        one side of the transition.
*/
static lex_frame lex_context;

#define lex_tokens lex_context.tokens
#define lex_token_room lex_context.token_room
#define lex_text lex_context.text
#define lex_text_room lex_context.text_room
#define lex_used lex_context.used
#define lex_count lex_context.count
#define shell_line_number lex_context.line

// The builtins own $LINENO itself and read this for it. What it answers is
// the line the running command was written on, which the executor keeps.
PURE positive shell_line_now();

fn parse_nest_enter();
fn parse_nest_leave();

static fn lex_nest_enter(lex_frame address_to frame)
{
        address_to frame = lex_context;
        lex_context = (lex_frame){0};

        parse_nest_enter();
}

static fn lex_nest_leave(lex_frame address_to frame)
{
        parse_nest_leave();

        if (lex_tokens)
                memory_free(lex_tokens, lex_token_room * sizeof(lex_token));

        if (lex_text)
                memory_free(lex_text, lex_text_room);

        lex_context = address_to frame;
}

//      Room for want bytes of token text, moving what is already handed out
//      if the block itself has to move.
static bool lex_room(positive want)
{
        p8 address_to before = lex_text;
        b32 index;

        if (lex_text_room >= want)
                return true;

        if (!shell_room((address_any address_to)address_of lex_text,
                        address_of lex_text_room, want, 1))
                return false;

        if (lex_text == before || !before)
                return true;

        for (index = 0; index < lex_count; index++)
                if (lex_tokens[index].text)
                        lex_tokens[index].text =
                            lex_text + (lex_tokens[index].text - before);

        return true;
}

/*
        The byte sets, built once.

        blank        what separates one word from the next
        metachar     what ends a word without being part of it
        ordinary     everything a word may contain without further thought,
                     which is the complement of the two above plus the things
                     that begin a quote or an expansion
*/
static b8 lex_blank[STRING_SET_BYTES];
static b8 lex_ordinary[STRING_SET_BYTES];
static b8 lex_operator[STRING_SET_BYTES];
// What decides nothing inside a double quote: wider than lex_ordinary,
// because a blank means nothing in there.
static b8 lex_in_double[STRING_SET_BYTES];
static b32 lex_ready;

/* expand.c is included later in this translation unit.  Its substitution
   child parses a fresh, non-interactive shell source even when the containing
   shell was interactive, which is material to comment policy. */
static bool expand_in_substitution;

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
        memory_fill(lex_ordinary + 1, 1, STRING_SET_BYTES - 1);
        memory_fill(lex_in_double + 1, 1, STRING_SET_BYTES - 1);

        {
                static const string_address decides = " \t\n|&;<>()['\"\\$`";
                static const string_address in_double = "\"\\$`";

                for (positive i = 0; decides[i]; i++)
                        lex_ordinary[decides[i]] = 0;

                for (positive i = 0; in_double[i]; i++)
                        lex_in_double[in_double[i]] = 0;
        }

        lex_ready = true;
}

// Which operator starts here, and how long it is. Longest forms are tested
// first, so &>> is never &> followed by > and >> is never two > tokens.
static b32 lex_operator_at(string_address at, positive address_to length)
{
        p8 a = string_get(at);
        p8 b = string_get(at + 1);

        /* <( and >( begin a process substitution, which is part of the word
           in front of it and not an operator at all: cat 2>(x) is one word
           reading 2/dev/fd/N, exactly as Bash reads it. */
        if (b == '(' && (a == '<' || a == '>'))
        {
                address_to length = 0;
                return 0;
        }

        if (a == '<' && b == '<' && string_is(at + 2, '<'))
        {
                address_to length = 3;
                return OP_HERESTRING;
        }

        // These must be single operators. Reading '&' as a background
        // separator first silently runs a different command graph.
        if (a == '&' && b == '>')
        {
                if (string_is(at + 2, '>'))
                {
                        address_to length = 3;
                        return OP_ANDDGREAT;
                }

                address_to length = 2;
                return OP_ANDGREAT;
        }

        address_to length = 2;

        if (a == '&' && b == '&')
                return OP_AND_IF;
        if (a == '|' && b == '|')
                return OP_OR_IF;
        // Both streams into the pipe, and a case item that carries on. Read
        // where the doubled forms already are, so no byte pays for them.
        if (a == '|' && b == '&')
                return OP_PIPEAND;
        if (a == ';' && b == '&')
                return OP_SEMIAND;
        if (a == ';' && b == ';')
        {
                if (string_is(at + 2, '&'))
                {
                        address_to length = 3;
                        return OP_DSEMIAND;
                }

                return OP_DSEMI;
        }
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
        if (!shell_array_room(lex_tokens, lex_token_room, (positive)lex_count + 2))
                return false;

        lex_tokens[lex_count].at = lex_at;
        lex_tokens[lex_count].kind = kind;
        lex_tokens[lex_count].op = op;
        lex_tokens[lex_count].text = text;
        lex_tokens[lex_count].length = length;
        lex_count++;

        return true;
}

// Words already live in lex_text. The two whole Bash tokens must live there
// as well: lex_room rebases every handed-out text pointer when that block
// grows, so keeping one pointed at the caller's input would turn it into an
// unrelated address as soon as a later word enlarged the block.
static b32 lex_add_whole(b32 kind, string_address text, positive length)
{
        positive start = lex_used;

        if (length == positive_max || !lex_room(lex_used + length + 1))
                return false;

        memory_copy_end(lex_text + lex_used, text, length);
        lex_used += length + 1;
        return lex_add(kind, 0, lex_text + start, length);
}

PURE bool shell_extglob_asked();

// The five heads of an extended pattern group, which are ordinary bytes
// everywhere a parenthesis does not follow them.
static CONST inline INLINE bool lex_extended_head(p8 value)
{
        return value == '?' || value == '*' || value == '+' || value == '@' ||
               value == '!';
}

static string_address lex_nested_at(string_address at);
static __attribute__((noinline)) string_address
lex_nesting(string_address at);
static __attribute__((noinline)) string_address
lex_nesting_double(string_address at);
static string_address lex_quote_end(string_address at, p8 quote);
static string_address parse_here_skip_bodies(string_address line,
                                              string_address newline);

/* The expander enforces this same ceiling when it later evaluates the nested
   words.  The earlier syntax walk must not be the unbounded recursive path. */
#define EXPAND_DEPTH 64

// The three bytes that separate words and lines. Asked in five places, which
// used to be five spellings of the same three comparisons.
static CONST inline INLINE bool lex_is_space(p8 value)
{
        return value == ' ' || value == '\t' || value == '\n';
}

/* Whether this unquoted < can be one half of << after line continuation has
   removed its bytes.  The full lexer remains the authority: a false positive
   only asks the parser-owned scanner and a here-string still registers no
   document. */
static PURE bool lex_dless_candidate(string_address line, string_address at)
{
        string_address before = at;
        positive distance = (positive)(at - line);

        if (string_is(at + 1, '<') && string_not(at + 2, '<'))
                return true;

        while (distance >= 2 && string_is(before - 1, '\n') &&
               string_is(before - 2, '\\'))
        {
                before -= 2;
                distance -= 2;
        }

        return distance && string_is(before - 1, '<');
}

/*
        Whether a # standing where a word could begin starts a comment.

        interactive_comments is deliberately narrower than the lexer: Bash
        consults it only while reading interactively.  A script still has
        comments after `shopt -u interactive_comments`, and the sh/dash
        personalities keep their existing always-on comment grammar.  Keep
        the test at the two rare fresh-# sites rather than in lex_ordinary,
        so an ordinary word pays no extra classification or branch.
*/
static inline INLINE bool lex_comments_on()
{
        return !shell_bash_compat || !shell_is_interactive ||
               expand_in_substitution ||
               shell_shopt_on(INTERACTIVE_COMMENTS);
}

/*
        Where a run closes in which a backslash carries the byte behind it.

        A dollar-single-quoted run is one: unlike an ordinary single quote,
        \' does not close it. The inside of a double quote seen from within a
        nesting is the other, where the closing quote is the only thing being
        looked for and a substitution inside it is not opened again. Same walk
        either way, so it is one walk with the quote as its argument.
*/
static PURE string_address lex_escaped_end(string_address at, p8 quote)
{
        while (string_get(at) && string_get(at) != quote)
                at += string_is(at, '\\') && string_get(at + 1) ? 2 : 1;

        return at;
}

/*
        Where a POSIX dollar-single-quoted run closes.

        The escape is interpreted immediately before expansion; the lexer
        only has to keep the quoted bytes together and keep delimiters inside
        them out of the grammar.
*/
static PURE string_address lex_dollar_quote_end(string_address at)
{
        return lex_escaped_end(at, '\'');
}

/*
        Step over whatever begins here that a scanner is not allowed to look
        inside, and say whether anything was stepped over.

        Three scanners walk a word for one thing of their own -- the )) that
        closes an arithmetic command, the ]] that closes a Bash condition, the
        ; that separates the three parts of a C-style for -- and all three
        have to agree about which bytes cannot be it. A backslash carries the
        byte behind it; a quoted run holds everything up to its partner; and
        $( ), ${ } and a backtick pair hold a whole command or name. Written
        out three times, those three lists had to be kept level by hand.

        at is advanced past the run when there was one. An unclosed quote is
        its own answer rather than a plain miss, because it means the word
        never ended: two of the three callers report that as no answer at all
        and the third as the end, and at is left on the terminating null so
        the third can simply hand it back.
*/
#define LEX_SKIP_NOTHING 0
#define LEX_SKIP_STEPPED 1
#define LEX_SKIP_UNCLOSED 2

static b32 lex_skip_held(string_address address_to at)
{
        string_address step = address_to at;
        p8 value = string_get(step);

        if (value == '$' && string_is(step + 1, '\''))
        {
                string_address stop = lex_dollar_quote_end(step + 2);

                if (!string_get(stop))
                {
                        address_to at = stop;
                        return LEX_SKIP_UNCLOSED;
                }

                address_to at = stop + 1;
                return LEX_SKIP_STEPPED;
        }

        if (value == '\\' && string_get(step + 1))
        {
                address_to at = step + 2;
                return LEX_SKIP_STEPPED;
        }

        if (value == '\'' || value == '"')
        {
                string_address stop = lex_quote_end(step + 1, value);

                if (!string_get(stop))
                {
                        address_to at = stop;
                        return LEX_SKIP_UNCLOSED;
                }

                address_to at = stop + 1;
                return LEX_SKIP_STEPPED;
        }

        {
                string_address inner = lex_nested_at(step);
                string_address stop = inner ? lex_nesting(inner) : null;

                if (stop && stop > inner)
                {
                        address_to at = stop;
                        return LEX_SKIP_STEPPED;
                }
        }

        return LEX_SKIP_NOTHING;
}

// One Bash arithmetic command token. Keeping its interior whole prevents the
// shell operators inside ((...)) -- notably ;, &&, < and > -- from becoming
// command-language tokens before the arithmetic parser sees them.
static string_address lex_arithmetic_end(string_address start)
{
        string_address at = start + 2;
        positive depth = 0;

        while (string_get(at))
        {
                b32 skipped = lex_skip_held(address_of at);

                if (skipped == LEX_SKIP_UNCLOSED)
                        return null;

                if (skipped)
                        continue;

                if (string_is(at, '('))
                        depth++;
                else if (string_is(at, ')'))
                {
                        if (depth)
                                depth--;
                        else if (string_is(at + 1, ')'))
                                return at + 2;
                }

                at++;
        }

        return null;
}

// A Bash [[...]] condition is one command-language token. Its own &&, ||,
// parentheses, < and > belong to the conditional grammar, while the same
// bytes after the closing ]] belong to the shell grammar again.
static string_address lex_conditional_end(string_address start)
{
        string_address at = start + 2;

        while (string_get(at))
        {
                //      value is read before the step, because the ]] test
                //      below is about the byte the scanner is standing on and
                //      not about wherever a skipped run has left it.
                p8 value = string_get(at);
                b32 skipped = lex_skip_held(address_of at);

                if (skipped == LEX_SKIP_UNCLOSED)
                        return null;

                if (skipped)
                        continue;

                if (value == ']' && string_is(at + 1, ']') &&
                    at > start + 2 && lex_is_space(string_get(at - 1)))
                {
                        p8 after = string_get(at + 2);

                        if (!after || lex_is_space(after) || lex_operator[after])
                                return at + 2;
                }

                at++;
        }

        return null;
}

/*
        Where the nesting that starts here begins, or nothing when none does.

        $( ), ${ } and a backtick pair are the three, and what matters at every
        call site is the same: the byte lex_nesting has to be pointed at, which
        is the bracket and not the dollar in front of it.
*/
static string_address lex_nested_at(string_address at)
{
        if (string_get(at) == '`')
                return at;

        if (string_get(at) == '$' &&
            (string_get(at + 1) == '(' || string_get(at + 1) == '{'))
                return at + 1;

        return null;
}

/*
        Where a quoted run closes, given the byte after the opening quote.

        Single quotes hold everything up to the next one, which is one call.
        Double quotes let a backslash keep the byte behind it and carry $( ),
        ${ } and a backtick pair whole, because the quote that closes one of
        those is not the one that closes this.

        The answer is the closing quote itself, or the terminator when there is
        no partner, or a trailing backslash -- which is not the same as an
        unclosed quote and the caller reading a line wants to tell them apart.
*/
static string_address lex_quote_end_kind(string_address at, p8 quote,
                                         bool address_to command_open)
{
        string_address step = at;

        if (quote != '"')
                return string_first_of_or_end(at, quote);

        while (string_get(step) && string_get(step) != '"')
        {
                positive run = string_span(step, lex_in_double);

                if (run)
                {
                        step += run;
                        continue;
                }

                if (string_get(step) == '\\')
                {
                        if (!string_get(step + 1))
                                return step;

                        step += 2;
                        continue;
                }

                {
                        string_address inner = lex_nested_at(step);
                        string_address stop =
                            inner ? (string_is(inner, '{')
                                         ? lex_nesting_double(inner)
                                         : lex_nesting(inner))
                                  : null;

                        if (stop && stop > inner)
                                step = stop;
                        else if (inner)
                        {
                                /* A nested construct with no mate also leaves
                                   this outer quote open. Stepping over its `$`
                                   let the outer quote appear to close and made
                                   EOF execute the broken construct literally. */
                                if (command_open && string_is(inner, '(') &&
                                    string_not(inner + 1, '('))
                                        *command_open = true;
                                step = inner + string_length(inner);
                        }
                        else
                                step++;
                }
        }

        return step;
}

static string_address lex_quote_end(string_address at, p8 quote)
{
        return lex_quote_end_kind(at, quote, null);
}

/*
        Past a nesting that a word carries whole.

        $( ), ${ } and a backtick pair hold whatever is between them, blanks and
        operators included, because what is in there is a command or a name and
        not this line's business.

        Returns where the nesting ends, one past its closing byte, or where it
        started when there is no closing byte at all -- an unfinished one is the
        parser's to complain about, the same as an unfinished quote.
*/
/* Whether the operator after a parameter name is # or %, the two POSIX
   pattern-removal forms whose words retain single-quote handling inside an
   outer double quote. */
static PURE bool lex_parameter_pattern(string_address at)
{
        string_address step = at;

        if (string_is(step, '!'))
                step++;
        if (string_is(step, '#'))
                step++;

        if ((string_get(step) >= 'a' && string_get(step) <= 'z') ||
            (string_get(step) >= 'A' && string_get(step) <= 'Z') ||
            string_is(step, '_'))
                while ((string_get(step) >= 'a' && string_get(step) <= 'z') ||
                       (string_get(step) >= 'A' && string_get(step) <= 'Z') ||
                       (string_get(step) >= '0' && string_get(step) <= '9') ||
                       string_is(step, '_'))
                        step++;
        else if (string_get(step) >= '0' && string_get(step) <= '9')
                while (string_get(step) >= '0' && string_get(step) <= '9')
                        step++;
        else if (string_get(step))
                step++;

        if (string_is(step, '['))
        {
                positive depth = 1;

                step++;
                while (string_get(step) && depth)
                {
                        if (string_is(step, '\\') && string_get(step + 1))
                                step += 2;
                        else if (string_is(step, '['))
                        {
                                depth++;
                                step++;
                        }
                        else if (string_is(step, ']'))
                        {
                                depth--;
                                step++;
                        }
                        else
                                step++;
                }
        }

        return string_is(step, '#') || string_is(step, '%');
}

static string_address lex_nesting_at(string_address at, positive nesting,
                                     bool posix_double)
{
        p8 open = string_get(at);
        p8 close = open == '(' ? ')' : open == '{' ? '}' : open;
        positive depth = 0;
        string_address step = at;
        string_address line = at + 1;
        /* ${...} and $((...)) have their own # operators. Command and process
           substitutions contain shell commands, so a fresh # hides every
           delimiter through the newline just as it does in the outer lexer.
           Legacy backticks are different: Bash finds their raw closing tick
           before parsing the extracted command, so a comment does not hide
           that delimiter. */
        bool commands = open == '(' && !string_is(at + 1, '(');
        bool fresh = commands;
        bool comment = false;
        bool maybe_here = false;
        bool raw_single = posix_double && open == '{' && shell_posix_on() &&
                          !lex_parameter_pattern(at + 1);

        if (nesting >= EXPAND_DEPTH)
                return at;

        while (string_get(step))
        {
                p8 c = string_get(step);

                if (comment)
                {
                        if (c == '\n')
                        {
                                string_address after = step + 1;

                                if (maybe_here)
                                {
                                        after = parse_here_skip_bodies(line,
                                                                        step);
                                        if (!after)
                                                return at;
                                }

                                step = after;
                                comment = false;
                                fresh = true;
                                maybe_here = false;
                                line = step;
                        }
                        else
                                step++;

                        continue;
                }

                /* A command substitution is parsed as non-interactive input,
                   including while its containing line is interactive. */
                if (commands && fresh && c == '#')
                {
                        comment = true;
                        step++;
                        continue;
                }

                if (c == '$' && string_is(step + 1, '\''))
                {
                        string_address stop = lex_dollar_quote_end(step + 2);

                        if (!string_get(stop))
                                return at;

                        step = stop + 1;
                        fresh = false;
                        continue;
                }

                if (c == '\\' && string_get(step + 1))
                {
                        p8 carried = string_get(step + 1);

                        step += 2;

                        /* Backslash-newline removes both bytes.  It does not
                           begin a word and therefore preserves whether # was
                           fresh on the physical line before it. */
                        if (carried != '\n')
                                fresh = false;
                        continue;
                }

                // Stepped over, not looked into: a bracket in a string closes
                // nothing, and lex_quote_end would call back in here.
                if (c == '\'' && raw_single)
                {
                        step++;
                        fresh = false;
                        continue;
                }

                if (c == '\'' || c == '"')
                {
                        step++;

                        if (c == '\'')
                                step = string_first_of_or_end(step, '\'');
                        else
                                step = lex_escaped_end(step, '"');

                        if (string_get(step))
                                step++;

                        fresh = false;
                        continue;
                }

                /* A nested substitution is one word piece in this command.
                   Walk it independently so its comment state and closing
                   delimiter cannot leak into the containing word. */
                if (c == '$' &&
                    (string_is(step + 1, '(') || string_is(step + 1, '{')))
                {
                        string_address inner = step + 1;
                        string_address stop =
                            lex_nesting_at(inner, nesting + 1,
                                           posix_double && open == '{' &&
                                               string_is(inner, '{'));

                        if (stop == inner)
                                return at;

                        step = stop;
                        fresh = false;
                        continue;
                }

                if ((c == '<' || c == '>') && string_is(step + 1, '('))
                {
                        string_address inner = step + 1;
                        string_address stop =
                            lex_nesting_at(inner, nesting + 1, false);

                        if (stop == inner)
                                return at;

                        step = stop;
                        fresh = false;
                        continue;
                }

                if (c == '`' && open != '`')
                {
                        string_address stop =
                            lex_nesting_at(step, nesting + 1, false);

                        if (stop == step)
                                return at;

                        step = stop;
                        fresh = false;
                        continue;
                }

                if (commands && c == '\n')
                {
                        string_address after = step + 1;

                        if (maybe_here)
                        {
                                after = parse_here_skip_bodies(line, step);
                                if (!after)
                                        return at;
                        }

                        step = after;
                        fresh = true;
                        maybe_here = false;
                        line = step;
                        continue;
                }

                if (commands && c == '<' && lex_dless_candidate(line, step))
                        maybe_here = true;

                // A backtick pair has the same byte at both ends, so it
                // opens on the first one and closes on the next.
                if (open == close)
                {
                        if (c == open)
                                depth = depth ? 0 : 1;
                }
                else if (c == open)
                        depth++;
                else if (c == close)
                        depth--;

                step++;

                if (!depth)
                        return step;

                if (commands)
                        fresh = lex_is_space(c) || lex_operator[c];
        }

        return at;
}

static __attribute__((noinline)) string_address
lex_nesting(string_address at)
{
        return lex_nesting_at(at, 0, false);
}

static __attribute__((noinline)) string_address
lex_nesting_double(string_address at)
{
        return lex_nesting_at(at, 0, true);
}

/*
        Whether the line is all of the line.

        A shell reads a line at a time and the language does not: a quote, a
        substitution and a trailing backslash all say "the rest of this is
        further down". Nothing here looked, so the three of them arrived as a
        word with a stray quote in it, a $ with no command behind it, and a
        backslash somebody meant to be invisible.

        LEX_CONTINUES asks for the next line joined on with nothing between,
        which is what a backslash before a newline means. LEX_OPEN asks for it
        joined on with the newline kept, because a newline inside a quote or a
        substitution is a byte of the thing, not the end of it.
*/
#define LEX_COMPLETE 0
#define LEX_CONTINUES 1
#define LEX_OPEN 2
// Bash treats an EOF inside a word differently from incomplete command
// grammar when eval/dot returns to its caller. The parser still joins both
// kinds with a newline; only its EOF boundary needs this distinction.
#define LEX_OPEN_WORD 3

b32 lex_unfinished(string_address line)
{
        string_address step = line;
        // A # is a comment only where a word could have started, which is the
        // same rule lex_line uses -- echo a#b is one word and not half of one.
        bool fresh = true;
        bool comments = lex_comments_on();

        lex_prepare();

        while (string_get(step))
        {
                p8 c = string_get(step);
                positive run;

                if (fresh && c == '[' && string_is(step + 1, '[') &&
                    lex_is_space(string_get(step + 2)))
                {
                        string_address stop = lex_conditional_end(step);

                        if (!stop)
                                return LEX_OPEN;

                        step = stop;
                        fresh = false;
                        continue;
                }

                if (c == '#' && fresh)
                {
                        if (comments)
                                return LEX_COMPLETE;
                }

                if (lex_blank[c] || lex_operator[c])
                {
                        //      A process substitution holds a command, and
                        //      the line is not finished until it closes.
                        if ((c == '<' || c == '>') && string_is(step + 1, '('))
                        {
                                string_address stop = lex_nesting(step + 1);

                                if (stop == step + 1)
                                        return LEX_OPEN;

                                step = stop;
                                fresh = false;
                                continue;
                        }

                        fresh = true;
                        step++;
                        continue;
                }

                fresh = false;

                // A # past the first byte of a run is a byte of the word, so
                // the run may swallow it and the test above still sees the one
                // that begins a comment.
                run = string_span(step, lex_ordinary);

                if (run)
                {
                        step += run;
                        continue;
                }

                if (c == '\\')
                {
                        if (!string_get(step + 1))
                                return LEX_CONTINUES;

                        step += 2;
                        continue;
                }

                if (c == '$' && string_is(step + 1, '\''))
                {
                        step = lex_dollar_quote_end(step + 2);

                        if (!string_get(step))
                                return LEX_OPEN_WORD;

                        step++;
                        continue;
                }

                if (c == '\'' || c == '"')
                {
                        bool command_open = false;
                        step = lex_quote_end_kind(step + 1, c,
                                                  address_of command_open);

                        // A backslash at the end inside double quotes is still
                        // a continuation: the quote is open and the line is
                        // short, and joining first is what makes the quote
                        // close on the next pass.
                        if (string_get(step) == '\\')
                                return LEX_CONTINUES;

                        if (!string_get(step))
                                return command_open ? LEX_OPEN : LEX_OPEN_WORD;

                        step++;
                        continue;
                }

                if (lex_nested_at(step))
                {
                        string_address inner = lex_nested_at(step);
                        string_address stop = lex_nesting(inner);

                        if (stop == inner)
                                return string_is(inner, '(') &&
                                               string_not(inner + 1, '(')
                                           ? LEX_OPEN : LEX_OPEN_WORD;

                        step = stop;
                        continue;
                }

                step++;
        }

        return LEX_COMPLETE;
}

/*
        One word, quotes and all.

        A run of ordinary bytes is taken whole by string_span; anything else is
        one byte of decision and then another run. A quote swallows to its
        partner, a backslash swallows the byte after it, and both stay in the
        text for the expander to deal with in its own order.
*/
/*
        Whether what has been read so far is the left of an assignment.

        This is the only question that makes a=(x y z) one word rather than a
        name followed by a subshell, and it is asked at one byte -- directly
        after the equals -- so that a command's own parentheses, a function
        definition and a case pattern are all untouched by it.
*/
static PURE bool lex_assignment_head(string_address text, positive length)
{
        positive at = 0;

        if (length && text[length - 1] == '+')
                length--;

        if (!length || (text[0] >= '0' && text[0] <= '9'))
                return false;

        while (at < length && (byte_is_alnum(text[at]) || text[at] == '_'))
                at++;

        if (!at)
                return false;

        if (at == length)
                return true;

        // A subscript holds anything, but it has to be closed at the end.
        return text[at] == '[' && text[length - 1] == ']' && length - at > 2;
}

/* Find the closing bracket whose following bytes prove this is an assignment.
   Reuse the lexer's quote/substitution walker so a `]` held inside either one
   cannot close the subscript. */
static string_address lex_assignment_subscript_end(string_address at)
{
        string_address step = at;
        positive depth = 1;

        while (string_get(step) && string_not(step, '\n'))
        {
                b32 skipped = lex_skip_held(address_of step);

                if (skipped)
                        continue;

                if (string_is(step, '['))
                        depth++;
                else if (string_is(step, ']'))
                {
                        string_address after;

                        if (--depth)
                        {
                                step++;
                                continue;
                        }

                        after = step + 1;

                        if (string_is(after, '+'))
                                after++;

                        return string_is(after, '=') ? step + 1 : null;
                }

                step++;
        }

        return null;
}

static b32 lex_word(string_address address_to at)
{
        string_address step = address_to at;
        positive start = lex_used;

        while (1)
        {
                positive run = string_span(step, lex_ordinary);

                if (run)
                {
                        if (!lex_room(lex_used + run + 1))
                                return false;

                        memory_copy(lex_text + lex_used, step, run);
                        lex_used += run;
                        step += run;
                        continue;
                }

                p8 c = string_get(step);

                /* `[` is a decision byte so the ordinary word loop reaches
                   this only for bracketed words. A NAME prefix plus a close
                   followed by = or += makes the complete subscript one piece,
                   including otherwise separating blanks. */
                if (c == '[' &&
                    lex_assignment_head(lex_text + start, lex_used - start))
                {
                        string_address stop =
                            lex_assignment_subscript_end(step + 1);

                        if (stop)
                        {
                                run = (positive)(stop - step);

                                if (!lex_room(lex_used + run + 2))
                                        return false;

                                memory_copy(lex_text + lex_used, step, run);
                                lex_used += run;
                                step = stop;
                                continue;
                        }
                }

                if (c == '(' && lex_used > start &&
                    lex_text[lex_used - 1] == '=' &&
                    lex_assignment_head(lex_text + start,
                                        lex_used - start - 1))
                {
                        string_address stop = lex_nesting(step);

                        if (stop > step)
                        {
                                run = (positive)(stop - step);

                                if (!lex_room(lex_used + run + 2))
                                        return false;

                                memory_copy(lex_text + lex_used, step, run);
                                lex_used += run;
                                step = stop;
                                continue;
                        }
                }

                /*
                        An extended pattern group is one piece of the word,
                        parentheses and alternations and all -- which is what
                        makes case aab in +(a)b) a pattern rather than a
                        syntax error. Only under the option: with it off the
                        parenthesis is what it has always been.
                */
                if (c == '(' && lex_used > start && shell_extglob_asked() &&
                    lex_extended_head(lex_text[lex_used - 1]))
                {
                        string_address stop = lex_nesting(step);

                        if (stop > step)
                        {
                                run = (positive)(stop - step);

                                if (!lex_room(lex_used + run + 2))
                                        return false;

                                memory_copy(lex_text + lex_used, step, run);
                                lex_used += run;
                                step = stop;
                                continue;
                        }
                }

                //      <( and >( carry a whole command inside the word,
                //      the same way $( ) does, so the blanks and operators in
                //      it are not this line's business.
                if ((c == '<' || c == '>') && string_is(step + 1, '('))
                {
                        string_address stop = lex_nesting(step + 1);

                        if (stop > step + 1)
                        {
                                run = (positive)(stop - step);

                                if (!lex_room(lex_used + run + 2))
                                        return false;

                                memory_copy(lex_text + lex_used, step, run);
                                lex_used += run;
                                step = stop;
                                continue;
                        }
                }

                if (!c || lex_blank[c] || lex_operator[c] || c == '\n')
                        break;

                if (!lex_room(lex_used + 3))
                        return false;

                lex_text[lex_used++] = c;
                step++;

                if (c == '$' && string_is(step, '\''))
                {
                        string_address stop = lex_dollar_quote_end(step + 1);

                        run = (positive)(stop - step) +
                              (string_get(stop) ? 1 : 0);

                        if (!lex_room(lex_used + run + 3))
                                return false;

                        memory_copy_apart(lex_text + lex_used, step, run);
                        lex_used += run;
                        step += run;
                        continue;
                }

                if (c == '\'' || c == '"')
                {
                        // Every byte of a quoted run is kept as it stands, so
                        // it is one copy once its end is known. An unterminated
                        // quote is the parser's to complain about, not the
                        // lexer's to guess at.
                        string_address stop = lex_quote_end(step, c);

                        run = (positive)(stop - step) + (string_get(stop) ? 1 : 0);

                        if (!lex_room(lex_used + run + 3))
                                return false;

                        memory_copy_apart(lex_text + lex_used, step, run);
                        lex_used += run;
                        step += run;

                        continue;
                }

                if (c == '\\' && string_get(step))
                {
                        lex_text[lex_used++] = string_get(step++);
                        continue;
                }

                // $( ), ${ } and ` ` are one piece of the word however much
                // blank or operator is inside them.
                {
                        string_address stop = null;

                        if (c == '`')
                                stop = lex_nesting(step - 1);
                        else if (c == '$' && (string_get(step) == '(' ||
                                              string_get(step) == '{'))
                                stop = lex_nesting(step);

                        if (stop && stop > step)
                        {
                                positive run = (positive)(stop - step);

                                if (!lex_room(lex_used + run + 2))
                                        return false;

                                memory_copy(lex_text + lex_used, step, run);
                                lex_used += run;
                                step = stop;
                        }
                }
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
HOT b32 lex_line(string_address line)
{
        string_address step = line;
        bool comments = lex_comments_on();

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
                {
                        if (comments)
                                break;
                }

                lex_at = (positive)(step - line);

                if (string_is(step, '[') && string_is(step + 1, '[') &&
                    lex_is_space(string_get(step + 2)))
                {
                        string_address stop = lex_conditional_end(step);

                        if (stop)
                        {
                                if (!lex_add_whole(LEX_CONDITIONAL, step,
                                                   (positive)(stop - step)))
                                        return -1;

                                step = stop;
                                continue;
                        }
                }

                if (string_is(step, '(') && string_is(step + 1, '('))
                {
                        string_address stop = lex_arithmetic_end(step);

                        if (stop)
                        {
                                if (!lex_add_whole(LEX_ARITHMETIC, step,
                                                   (positive)(stop - step)))
                                        return -1;

                                step = stop;
                                continue;
                        }
                }

                op = lex_operator_at(step, address_of length);

                if (op)
                {
                        if (!lex_add(LEX_OPERATOR, op, null, length))
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
