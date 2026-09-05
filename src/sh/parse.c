/*
        The shell's parser.

        The lexer hands over words and operators and nothing else. It does not
        know that "if" is a reserved word, and it is right not to: POSIX decides
        that by position, never by spelling, so "if" is a command name wherever
        a command name was expected and a keyword only where a keyword was.
        That decision belongs to a parser that knows where it is, and this is
        it -- recursive descent, one function per production of the grammar in
        XCU section 2.10.

        Two things shape the storage. There is no allocator, so the tree lives
        in one fixed array of nodes; and a function body has to outlive the
        line that defined it, so that array is used from both ends. Parsing a
        line claims nodes upward from the bottom and gives them all back when
        the line has run; a function definition is copied downward from the top
        and stays. The two never meet because the parse stops when they would.
*/

#define PT_END 0
#define PT_WORD 1
#define PT_OP 2
#define PT_NEWLINE 3
#define PT_ARITHMETIC 4
#define PT_CONDITIONAL 5

typedef struct parse_alias_trace parse_alias_trace;

struct parse_alias_trace
{
        parse_alias_trace address_to next;
        string_address name;
};

typedef struct
{
        b32 kind;
        b32 op;
        // Whether this token touches the one before it, which is the whole
        // difference between 2>file and 2 >file.
        b32 joined;
        string_address text;
        positive length;
        // Alias replacement is recursive, except through a name already in
        // the replacement chain. A trailing blank also asks that the next
        // ordinary word be considered even after the command name.
        parse_alias_trace address_to alias_trace;
        b32 alias_forced;
        // Which line of the input this word was read from, which is where a
        // construct built out of it was written.
        b32 line;
} parse_token;

/*
        The words of a line, and the bytes they are made of.

        The table may move -- nothing keeps an address inside it -- so it
        grows by taking a bigger mapping. The bytes may not: every token holds
        a pointer at them and so does everything downstream, so those come out
        of a block store that never moves what it has handed out. The parser
        already saved and restored a position in that arena to unwind a nested
        construct, and a mark in the store is the same idea.

        Both used to be fixed, which a line of any length met: sixteen
        kilobytes of token text is one long argument, and 512 tokens is a
        generated command list.
*/
static parse_token address_to parse_tokens;
static positive parse_token_room;
static positive parse_token_count;
static shell_store parse_store;
static parse_token parse_no_token;

string_address alias_lookup(string_address name);

#define NODE_SIMPLE 1
#define NODE_PIPELINE 2
#define NODE_ANDOR 3
#define NODE_LIST 4
#define NODE_IF 5
#define NODE_WHILE 6
#define NODE_UNTIL 7
#define NODE_FOR 8
#define NODE_CASE 9
#define NODE_CASE_ITEM 10
#define NODE_SUBSHELL 11
#define NODE_GROUP 12
#define NODE_FUNCTION 13
#define NODE_ARITHMETIC 14
#define NODE_CFOR 15
#define NODE_CONDITIONAL 16
#define NODE_SELECT 17
#define NODE_TIME 18
#define NODE_COPROC 19

/*
        One node shape for every production.

        A union would save a little memory and cost the reader the ability to
        see what a field means without first knowing which arm is live. The
        fields that go unused in a given kind are simply zero.

        left/right/extra are children, next is the sibling: a pipeline chains
        its commands through next, an and-or list chains its pipelines, and a
        list chains its and-ors, so the three levels of the grammar are three
        chains and not three shapes.
*/
typedef struct
{
        b32 kind;
        b32 op;
        b32 flags;
        b32 left;
        b32 right;
        b32 extra;
        b32 next;
        b32 word;
        b32 word_count;
        b32 redirect;
        b32 redirect_count;
        // The line this command was written on, taken from the token it
        // begins with. A function body outlives the line that defined it, so
        // this is how a call knows where it was made from.
        b32 line;
} parse_node;

typedef struct
{
        b32 op;
        b32 fd;
        string_address text;
        positive text_length;
        // A here-document carries its body in place of a file name. Which of
        // the two arenas the body sits in depends on whether the command it
        // belongs to outlived the line that wrote it.
        b32 kept;
        // A quoted delimiter makes the body literal; an unquoted one lets the
        // parameters in it expand.
        b32 raw;
        positive body;
        positive body_length;
} parse_redirect;

#define PARSE_NODES 768
#define PARSE_WORDS 768
#define PARSE_REDIRECTS 192
#define PARSE_KEPT_TEXT 8192

static parse_node parse_nodes[PARSE_NODES];
static string_address parse_words[PARSE_WORDS];
static positive parse_word_lengths[PARSE_WORDS];
static positive parse_word_name_lengths[PARSE_WORDS];
static positive parse_word_name_hashes[PARSE_WORDS];
static p8 parse_word_flags[PARSE_WORDS];
static parse_redirect parse_redirects[PARSE_REDIRECTS];

#define PARSE_WORD_LITERAL 1
#define PARSE_WORD_ASSIGNMENT 2
#define PARSE_WORD_APPEND 4
// NAME=( ... ): the value is a list of elements and not one string, so it is
// neither expanded nor assigned the way every other assignment word is.
#define PARSE_WORD_COMPOUND 8

// What a case item's terminator was, kept in the item node's flags.
#define CASE_STOP 0
#define CASE_FALL_THROUGH 1
#define CASE_TEST_ON 2

static b32 parse_node_used;
static b32 parse_node_top;
static b32 parse_word_used;
static b32 parse_word_top;
static b32 parse_redirect_used;
static b32 parse_redirect_top;

static p8 parse_kept_text[PARSE_KEPT_TEXT];
static positive parse_kept_used;

#define PARSE_OK 0
#define PARSE_INCOMPLETE 1
#define PARSE_SYNTAX 2

/*
        Where this parse starts, which is not always the beginning.

        eval runs a line from inside a line that is already running, and the
        tree being walked is in these same arrays. So a nested parse claims
        from where the outer one stopped and gives that back when it is done,
        rather than from zero over the top of what is still in use.
*/
typedef struct
{
        b32 node, word, redirect, position, state;
        positive token;
        shell_mark token_text;
        b32 wanted, filled, taken;
        positive used, names_used;
} parse_frame;

/* Live marks and saved marks intentionally have one shape. One assignment is
   the complete nest transition, so future state cannot drift between entry
   and return. */
static parse_frame parse_context = {.node = 1};

#define parse_node_base parse_context.node
#define parse_word_base parse_context.word
#define parse_redirect_base parse_context.redirect
#define parse_position parse_context.position
#define parse_state parse_context.state
#define parse_token_base parse_context.token
#define parse_text_base parse_context.token_text
#define here_wanted parse_context.wanted
#define here_filled parse_context.filled
#define here_taken parse_context.taken
#define here_used parse_context.used
#define here_names_used parse_context.names_used

/*
        Here-documents, which arrive after the line that asked for them.

        The delimiters are collected while the lines are still being read,
        because the reader has to know that the next line is a body and not a
        command before the parser has seen any of it. The bodies are kept in
        the order the operators appeared, and the parser hands them out in the
        same order.
*/
typedef struct
{
        positive delimiter;
        positive body;
        positive length;
        b32 quoted;
        // <<- rather than <<, which takes the leading tabs off every line of
        // the body and off the line that ends it.
        b32 strip;
        // An unquoted trailing backslash removes its physical newline.  The
        // next physical line is body text even when it spells the delimiter,
        // because the logical line has not begun there.
        b32 continued;
        b32 overflow;
} here_document;

static here_document address_to here_documents;
static positive here_document_room;
static p8 address_to here_text;
static positive here_text_room;
static p8 address_to here_names;
static positive here_names_room;

/*
        A line the language is not finished with.

        A quote, a substitution and a trailing backslash all run past the end of
        the line they start on, and the reader hands over one line at a time.
        The unfinished text waits here until the rest of it arrives, so the
        lexer is only ever shown whole words -- before this, an open quote came
        out as a word with a quote still in it and the next line ran as a
        command of its own.
*/
static p8 address_to parse_pending;
static positive parse_pending_room;
static positive parse_pending_used;

// A terminal backslash removed itself and only asked for the next physical
// line. At EOF an empty next line completes that command; an open quote or
// substitution remains unfinished and is a syntax error instead.
bool parse_eof_can_complete()
{
        return parse_pending_used && !lex_unfinished(parse_pending);
}

fn parse_reset()
{
        parse_pending_used = 0;
        parse_token_count = parse_token_base;
        shell_store_rewind(address_of parse_store, parse_text_base);
        here_wanted = 0;
        here_filled = 0;
        here_taken = 0;
        here_used = 0;
        here_names_used = 0;
}

/*
        A line inside a line.

        What eval runs is parsed into the space above whatever is running, and
        the marks come back afterwards. A frame is the marks themselves, so
        nesting costs a few words and not a copy of the arrays. The stack is
        here rather than at the caller because its shape is nobody else's.
*/
static parse_frame address_to parse_frames;
static positive parse_frame_room;
static b32 parse_nest_depth;

fn parse_nest_enter()
{
        parse_frame address_to frame;

        if (parse_nest_depth == 0x7fffffff ||
            !shell_array_room(parse_frames, parse_frame_room, (positive)parse_nest_depth + 1))
        {
                string_format(log_error, "No room for nested shell input\n");
                log_flush();
                system_call_1(syscall(exit_group), 2);
        }

        frame = parse_frames + parse_nest_depth++;

        address_to frame = parse_context;

        /* Node zero is the parser's absent-child sentinel.  A nested source
           can be the first source this process parses (BASH_ENV is one), so
           the live low-water mark has not necessarily been initialized by
           parse_program yet. */
        parse_node_base = parse_node_used ? parse_node_used : 1;
        parse_word_base = parse_word_used;
        parse_redirect_base = parse_redirect_used;
        parse_token_base = parse_token_count;
        parse_text_base = shell_store_mark(address_of parse_store);
}

fn parse_nest_leave()
{
        parse_frame address_to frame;

        if (!parse_nest_depth)
                return;

        frame = parse_frames + --parse_nest_depth;

        /*
                The tokens this nest claimed, given back to where it claimed
                from -- which is what the bases hold now, and not where the
                line outside it began, which is what they held before.

                Giving them back to the outer base threw away the tokens of
                the line that was still running: an eval inside a loop wrote
                its own line over the loop's words, and the second time round
                the name of the loop variable was the empty string. Not giving
                the nodes back at all was the other half of it, and a loop that
                ran eval eighty times ran the tree out.
        */
        parse_node_used = parse_node_base;
        parse_word_used = parse_word_base;
        parse_redirect_used = parse_redirect_base;
        parse_token_count = parse_token_base;
        shell_store_rewind(address_of parse_store, parse_text_base);

        parse_context = address_to frame;
}

// What a child of this shell has to know about being one. Declared here and
// answered in exec.c, which is where being a child makes a difference.
fn exec_child_began();

// A forked substitution has the outer line's marks and no use for them: what
// it runs is the only thing it will ever run.
fn parse_reset_all()
{
        exec_child_began();

        parse_node_base = 1;
        parse_word_base = 0;
        parse_redirect_base = 0;
        parse_token_base = 0;
        //      Back to the beginning of the store, which is what a
        //      mark taken before anything was put in it means.
        shell_store_reset(address_of parse_store);
        parse_text_base = shell_store_mark(address_of parse_store);
        parse_reset();
}

static PURE inline INLINE parse_token address_to parse_look(b32 ahead)
{
        b32 index = parse_position + ahead;

        if (index < 0 || index >= (b32)parse_token_count)
                return address_of parse_no_token;

        return parse_tokens + index;
}

static PURE bool parse_word_is_length(b32 ahead, string_address text,
                                      positive length)
{
        parse_token address_to token = parse_look(ahead);

        return token->kind == PT_WORD && token->length == length &&
               !memory_compare(token->text, text, length);
}

/* Every grammar spelling is a literal. Carry its size through the call so a
   parser comparison becomes the compiler_memory fixed-size floor rather than
   a separate string-length pass followed by a generic comparison. */
#define parse_word_is(ahead, text) \
        parse_word_is_length((ahead), (string_address)(text), sizeof(text) - 1)

enum
{
        PARSE_KEYWORD_NONE,
        PARSE_KEYWORD_IF,
        PARSE_KEYWORD_WHILE,
        PARSE_KEYWORD_UNTIL,
        PARSE_KEYWORD_FOR,
        PARSE_KEYWORD_CASE,
        //      Everything from IF to OPEN begins a command and everything
        //      from THEN to CLOSE ends a list, and two range tests below say
        //      which is which. A new word that begins a command belongs
        //      here, in front of OPEN, and nowhere else.
        PARSE_KEYWORD_SELECT,
        PARSE_KEYWORD_TIME,
        PARSE_KEYWORD_COPROC,
        PARSE_KEYWORD_OPEN,
        PARSE_KEYWORD_THEN,
        PARSE_KEYWORD_ELSE,
        PARSE_KEYWORD_ELIF,
        PARSE_KEYWORD_FI,
        PARSE_KEYWORD_DO,
        PARSE_KEYWORD_DONE,
        PARSE_KEYWORD_ESAC,
        PARSE_KEYWORD_CLOSE,
        PARSE_KEYWORD_IN,
        PARSE_KEYWORD_BANG,
};

/*
        Classify once instead of asking fifteen exact string comparisons.
        Ordinary commands are overwhelmingly not keywords, so length rejects
        them immediately; packed loads verify the few equal-length candidates.
*/
static PURE HOT b32 parse_keyword(b32 ahead)
{
        parse_token address_to token = parse_look(ahead);
        string_address text;

        if (token->kind != PT_WORD)
                return PARSE_KEYWORD_NONE;

        text = token->text;

        switch (token->length)
        {
        case 1:
                switch (text[0])
                {
                case '!': return PARSE_KEYWORD_BANG;
                case '{': return PARSE_KEYWORD_OPEN;
                case '}': return PARSE_KEYWORD_CLOSE;
                default: return PARSE_KEYWORD_NONE;
                }
        case 2:
                switch (memory_load_unaligned(p16, text))
                {
                case byte_word_2('i', 'f'): return PARSE_KEYWORD_IF;
                case byte_word_2('f', 'i'): return PARSE_KEYWORD_FI;
                case byte_word_2('d', 'o'): return PARSE_KEYWORD_DO;
                case byte_word_2('i', 'n'): return PARSE_KEYWORD_IN;
                default: return PARSE_KEYWORD_NONE;
                }
        case 3:
                return memory_is_2(text, 'f', 'o') && text[2] == 'r'
                           ? PARSE_KEYWORD_FOR : PARSE_KEYWORD_NONE;
        case 4:
                switch (memory_load_unaligned(p32, text))
                {
                case byte_word_4('t', 'h', 'e', 'n'): return PARSE_KEYWORD_THEN;
                case byte_word_4('e', 'l', 's', 'e'): return PARSE_KEYWORD_ELSE;
                case byte_word_4('e', 'l', 'i', 'f'): return PARSE_KEYWORD_ELIF;
                case byte_word_4('d', 'o', 'n', 'e'): return PARSE_KEYWORD_DONE;
                case byte_word_4('c', 'a', 's', 'e'): return PARSE_KEYWORD_CASE;
                case byte_word_4('e', 's', 'a', 'c'): return PARSE_KEYWORD_ESAC;
                case byte_word_4('t', 'i', 'm', 'e'): return PARSE_KEYWORD_TIME;
                default: return PARSE_KEYWORD_NONE;
                }
        case 5:
                if (memory_is_4(text, 'w', 'h', 'i', 'l') && text[4] == 'e')
                        return PARSE_KEYWORD_WHILE;
                if (memory_is_4(text, 'u', 'n', 't', 'i') && text[4] == 'l')
                        return PARSE_KEYWORD_UNTIL;
                return PARSE_KEYWORD_NONE;
        case 6:
                if (memory_is_4(text, 's', 'e', 'l', 'e') &&
                    memory_is_2(text + 4, 'c', 't'))
                        return PARSE_KEYWORD_SELECT;
                if (memory_is_4(text, 'c', 'o', 'p', 'r') &&
                    memory_is_2(text + 4, 'o', 'c'))
                        return PARSE_KEYWORD_COPROC;
                return PARSE_KEYWORD_NONE;
        default:
                return PARSE_KEYWORD_NONE;
        }
}

/*
        A here-document delimiter with its quoting taken off.

        The quotes decide whether the body is expanded, so which quotes were
        used matters as much as what is left when they are gone.
*/
static bool parse_here_register(string_address word, bool strip)
{
        string_address step = word;
        positive start = here_names_used;
        positive reserve = string_length(word) + 1;
        here_document address_to document;

        if (!shell_array_room(here_documents, here_document_room, (positive)here_wanted + 1) ||
            !shell_array_room(here_names, here_names_room, here_names_used + reserve))
                return false;

        document = here_documents + here_wanted;
        memory_fill(document, 0, sizeof(*document));
        document->delimiter = start;
        document->strip = strip;

        while (string_get(step))
        {
                p8 c = string_get(step);

                if (c == '\'' || c == '"')
                {
                        document->quoted = true;
                        step++;

                        while (string_get(step) && string_get(step) != c)
                                here_names[here_names_used++] = string_get(step++);

                        if (string_get(step))
                                step++;

                        continue;
                }

                if (c == '\\' && string_get(step + 1))
                {
                        document->quoted = true;
                        step++;
                        c = string_get(step);
                }

                here_names[here_names_used++] = c;
                step++;
        }

        here_names[here_names_used++] = end;
        here_wanted++;

        return true;
}

fn parse_here_close();

// Which delimiter the reader is waiting for, or nothing when it is waiting for
// a command.
PURE string_address parse_here_open()
{
        if (here_filled >= here_wanted)
                return null;

        return here_names + here_documents[here_filled].delimiter;
}

bool parse_here_line(string_address line)
{
        here_document address_to document;
        positive length;
        bool continues = false;

        if (here_filled >= here_wanted)
                return false;

        /*
                The tabs a <<- body does not keep, and the terminator hiding
                behind them.

                The reader compares the line it read against the delimiter
                before handing it over, and by then the tabs are still on it.
                Which of the two the line is can only be decided where what
                would be stripped is known, which is here.
        */
        document = here_documents + here_filled;

        if (document->strip)
                line += string_span_of_set(line, "\t");

        /* Delimiter recognition belongs beside continuation state.  A line
           joined to its predecessor cannot terminate the document, even if
           that physical line consists only of the delimiter. */
        if (!document->continued &&
            !string_compare(line, here_names + document->delimiter))
        {
                parse_here_close();
                return true;
        }

        length = string_length(line);

        if (!document->quoted && length)
        {
                positive slash = length;

                while (slash && string_is(line + slash - 1, '\\'))
                        slash--;

                continues = ((length - slash) & 1) != 0;
                if (continues)
                        length--;
        }

        if (!document->length)
                document->body = here_used;

        // A body that does not fit is not a body with its end cut off: the
        // reader does not look at what this answers, so the complaint is made
        // here and the command it belongs to is refused when it is parsed.
        if (!shell_array_room(here_text, here_text_room, here_used + length + 2))
        {
                if (!document->overflow)
                        string_format(log_error, "Here-document too long: %s\n",
                                      here_names + document->delimiter);

                document->overflow = true;

                return false;
        }

        memory_copy(here_text + here_used, line, length);
        here_used += length;

        if (!continues)
                here_text[here_used++] = '\n';

        here_text[here_used] = end;
        document->length = here_used - document->body;
        document->continued = continues;

        return true;
}

fn parse_here_close()
{
        if (here_filled < here_wanted)
        {
                here_document address_to document = here_documents + here_filled;

                if (!document->length)
                        document->body = here_used;

                // Past the terminator, so the body after this one does not
                // overwrite it and every body stands as a string.
                if (shell_array_room(here_text, here_text_room, here_used + 1))
                {
                        here_text[here_used] = end;
                        here_used++;
                }
                else
                        document->overflow = true;

                here_filled++;
        }
}

static bool parse_hold(string_address line, b32 unfinished)
{
        positive length = string_length(line);

        if (line != parse_pending)
        {
                if (!shell_array_room(parse_pending, parse_pending_room, length + 2))
                        return false;

                memory_copy(parse_pending, line, length + 1);
        }

        // A backslash before a newline was only ever the mark saying "not
        // yet"; a newline inside a quote or a substitution is a byte of it.
        if (unfinished == LEX_CONTINUES)
                length--;
        else
                parse_pending[length++] = '\n';

        parse_pending[length] = end;
        parse_pending_used = length;

        return true;
}

// Lexer's storage is reused on its next call. Copy one token into parser
// storage, keeping every piece of text in the stable arena.
static bool parse_copy_lex(parse_token address_to into,
                           lex_token address_to source,
                           parse_alias_trace address_to trace)
{
        into->kind = source->kind == LEX_WORD
                         ? PT_WORD
                         : source->kind == LEX_ARITHMETIC
                               ? PT_ARITHMETIC
                               : source->kind == LEX_CONDITIONAL
                                     ? PT_CONDITIONAL
                                     : PT_OP;
        into->op = source->op;
        into->text = null;
        into->length = source->length;
        into->alias_trace = trace;
        into->alias_forced = false;
        into->line = (b32)shell_line_number;

        if (into->kind == PT_OP)
                return true;

        into->text = shell_store_take(address_of parse_store,
                                      source->length + 1);

        if (!into->text)
                return false;

        memory_copy_end(into->text, source->text, source->length);
        return true;
}

// One token of the line the lexer just cut, and whether it touched the one
// before it -- which the lexer's positions say and only the parser keeps.
static bool parse_copy_lexed(parse_token address_to into, b32 index,
                             parse_alias_trace address_to trace)
{
        lex_token address_to source = lex_tokens + index;

        into->joined = index && source->at == lex_tokens[index - 1].at +
                                                  lex_tokens[index - 1].length;

        return parse_copy_lex(into, source, trace);
}

// The newline that ends a line of tokens, which the lexer never makes: it
// separates commands exactly as a semicolon does, and inside a construct it
// is the only thing that does.
static fn parse_token_newline(parse_token address_to into,
                              parse_alias_trace address_to trace)
{
        into->kind = PT_NEWLINE;
        into->op = 0;
        into->joined = 0;
        into->text = null;
        into->length = 0;
        into->alias_trace = trace;
        into->alias_forced = false;
        into->line = (b32)shell_line_number;
}

// Register a here-document as soon as its complete token line is available,
// whether that line came from source or from an alias replacement.
static bool parse_here_at(b32 at)
{
        b32 word = at + 1;
        string_address delimiter =
            word < (b32)parse_token_count &&
                    parse_tokens[word].kind == PT_WORD
                ? parse_tokens[word].text
                : null;
        bool strip = false;

        if (delimiter && parse_tokens[word].joined &&
            string_is(delimiter, '-'))
        {
                strip = true;
                delimiter++;

                if (!string_get(delimiter))
                {
                        word++;
                        delimiter = word < (b32)parse_token_count &&
                                            parse_tokens[word].kind == PT_WORD
                                        ? parse_tokens[word].text
                                        : null;
                }
        }

        return !delimiter || parse_here_register(delimiter, strip);
}

/*
        A line of source, appended to whatever is already waiting.

        The lexer works a line at a time and reuses its own storage, so every
        token is copied out before the next line goes through it. The newline
        is kept as a token of its own: it separates commands exactly as a
        semicolon does, and inside a construct it is the only thing that does.
*/
bool parse_feed(string_address line)
{
        b32 count;
        positive token_start;
        b32 index;
        b32 unfinished;

        // Joining first is what lets the unfinished thing be recognised at
        // all: the quote that closes is on this line and the one that opened
        // it is on the last.
        if (parse_pending_used)
        {
                positive length = string_length(line);

                if (!shell_array_room(parse_pending, parse_pending_room,
                                      parse_pending_used + length + 2))
                        return false;

                memory_copy(parse_pending + parse_pending_used, line, length + 1);
                parse_pending_used += length;
                line = parse_pending;
        }

        unfinished = lex_unfinished(line);

        if (unfinished)
                return parse_hold(line, unfinished);

        parse_pending_used = 0;
        count = lex_line(line);

        if (count < 0)
                return false;

        token_start = parse_token_count;

        for (index = 0; index < count; index++)
        {
                //      Room taken before any address into the table is,
                //      because taking room is what may move it.
                if (!shell_array_room(parse_tokens, parse_token_room, parse_token_count + 2))
                        return false;

                if (!parse_copy_lexed(parse_tokens + parse_token_count, index,
                                      null))
                        return false;

                parse_token_count++;
        }

        for (positive at = token_start; at < parse_token_count; at++)
                if (parse_tokens[at].kind == PT_OP &&
                    parse_tokens[at].op == OP_DLESS && !parse_here_at((b32)at))
                        return false;

        /*
                A comment-only line has no lexer tokens, but it is still a
                line and the parser still needs its newline sentinel.  The
                per-token room check above never runs for that shape.  A
                script beginning with #! therefore wrote through the null
                initial table before it reached its first command.
        */
        if (!shell_array_room(parse_tokens, parse_token_room, parse_token_count + 1))
                return false;

        parse_token_newline(parse_tokens + parse_token_count, null);
        parse_token_count++;

        return true;
}

static b32 parse_node_new(b32 kind)
{
        b32 index;

        if (parse_node_used + 1 >= parse_node_top)
        {
                parse_state = PARSE_SYNTAX;
                return 0;
        }

        index = parse_node_used++;
        memory_fill(parse_nodes + index, 0, sizeof(parse_node));
        parse_nodes[index].kind = kind;
        parse_nodes[index].line = parse_look(0)->line;

        return index;
}

static b32 parse_word_new(string_address text, positive length)
{
        positive name_length = 0;
        p8 assignment;
        p8 flags;

        if (parse_word_used + 1 >= parse_word_top)
        {
                parse_state = PARSE_SYNTAX;
                return 0;
        }

        parse_words[parse_word_used] = text;
        parse_word_lengths[parse_word_used] = length;
        assignment = shell_assignment_kind(text, address_of name_length);
        flags = shell_expand_literal(text, length) ? PARSE_WORD_LITERAL : 0;

        if (assignment)
        {
                flags |= PARSE_WORD_ASSIGNMENT;

                if (assignment == 2)
                        flags |= PARSE_WORD_APPEND;

                if (string_is(text + name_length + assignment, '('))
                        flags |= PARSE_WORD_COMPOUND;

                parse_word_name_hashes[parse_word_used] =
                    memory_hash_33(text, name_length);
        }
        else
                parse_word_name_hashes[parse_word_used] = 0;

        parse_word_name_lengths[parse_word_used] = name_length;
        parse_word_flags[parse_word_used] = flags;

        return parse_word_used++;
}

static fn parse_attach_word(b32 index, string_address text, positive length)
{
        b32 slot = parse_word_new(text, length);

        if (parse_state)
                return;

        if (!parse_nodes[index].word_count)
                parse_nodes[index].word = slot;

        parse_nodes[index].word_count++;
}

static fn parse_skip_newlines()
{
        while (parse_look(0)->kind == PT_NEWLINE)
                parse_position++;
}

static fn parse_skip_separators()
{
        while (parse_look(0)->kind == PT_NEWLINE ||
               (parse_look(0)->kind == PT_OP && parse_look(0)->op == OP_SEMI))
                parse_position++;
}

// Running out of tokens inside a construct is not an error: it means the rest
// of it is on a line nobody has typed yet.
static COLD fn parse_fail()
{
        parse_state = parse_look(0)->kind == PT_END ? PARSE_INCOMPLETE : PARSE_SYNTAX;
}

static bool parse_expect_word_length(string_address text, positive length)
{
        if (parse_word_is_length(0, text, length))
        {
                parse_position++;
                return true;
        }

        parse_fail();
        return false;
}

#define parse_expect_word(text) \
        parse_expect_word_length((string_address)(text), sizeof(text) - 1)

static bool parse_expect_operator(b32 op)
{
        if (parse_look(0)->kind == PT_OP && parse_look(0)->op == op)
        {
                parse_position++;
                return true;
        }

        parse_fail();
        return false;
}

/*
        Where a list of commands stops.

        Every one of these is a word that only means what it says at the start
        of a command, which is exactly where a list would look for the next
        one -- so the test belongs here and nowhere else. An argument that
        happens to be spelled "done" is read as an argument, because argument
        position never asks this question.
*/
static PURE bool parse_at_list_end()
{
        parse_token address_to token = parse_look(0);
        b32 keyword;

        if (token->kind == PT_END)
                return true;

        //      The three case terminators and the closing parenthesis.
        //      OP_SEMIAND and OP_DSEMIAND are the two highest operator
        //      numbers there are, so one comparison covers both.
        if (token->kind == PT_OP &&
            (token->op == OP_RPAREN || token->op == OP_DSEMI ||
             token->op >= OP_SEMIAND))
                return true;

        if (token->kind != PT_WORD)
                return false;

        keyword = parse_keyword(0);

        return keyword > PARSE_KEYWORD_OPEN &&
               keyword <= PARSE_KEYWORD_CLOSE;
}

/*
        The reserved words, which are not names.

        Position decides whether one of these is a keyword, but nothing makes
        it a function name: "if() { ...; }" defines something that can never
        be reached, because the word in front of a command is read as the
        keyword every time. dash calls it a syntax error and so does this.
*/
static PURE bool parse_reserved(b32 ahead)
{
        return parse_keyword(ahead) != PARSE_KEYWORD_NONE;
}

static CONST bool parse_redirect_operator(b32 op)
{
        /* lex.c keeps redirect operators in three contiguous families. */
        return (op >= OP_DLESS && op <= OP_CLOBBER) ||
               (op >= OP_LESS && op <= OP_GREAT) ||
               (op >= OP_ANDGREAT && op <= OP_HERESTRING);
}

/* Return the number of descriptor tokens before a redirect operator, or -1.
   Alias scans and the grammar must agree on this exact two-token prefix. */
static PURE b32 parse_redirect_prefix(b32 at)
{
        if (at >= (b32)parse_token_count)
                return -1;

        parse_token address_to token = parse_tokens + at;
        positive descriptor;

        if (token->kind == PT_OP && parse_redirect_operator(token->op))
                return 0;

        if (at + 1 >= (b32)parse_token_count)
                return -1;

        parse_token address_to next = token + 1;

        // &> always means descriptors one and two. In "echo 2&>file", the 2
        // is therefore an argument, unlike the descriptor prefix in 2>file.
        return token->kind == PT_WORD &&
               string_digits_exact(token->text, address_of descriptor) &&
               descriptor <= 0x7fffffff &&
               next->kind == PT_OP && next->joined &&
               next->op != OP_ANDGREAT && next->op != OP_ANDDGREAT &&
               parse_redirect_operator(next->op) ? 1 : -1;
}

/* The token after a redirect: past its descriptor prefix, the operator, and
   the operand word when there is one. The two alias scans step over
   redirects this way and have to agree with each other about how far. */
static PURE b32 parse_after_redirect(b32 at, b32 prefix)
{
        at += prefix + 1;

        if (at < (b32)parse_token_count && parse_tokens[at].kind == PT_WORD)
                at++;

        return at;
}

/*
        Replace one eligible word with the tokens of its alias value.

        This happens in the parser, not at execution. An alias may therefore
        produce a reserved word, a separator, a redirect, or several commands;
        rewriting argv after expansion cannot express any of those. The token
        text is copied into the parser's stable store before the lexer is used
        again, exactly as source-line tokens are.

        Each replacement token carries the chain which produced it. That is
        the cycle rule for both the direct `a=a` case and a chain with words or
        assignments between its names: a name already being expanded is left
        as a word instead of beginning the chain again.
*/
static bool parse_alias_replace(b32 position)
{
        parse_token address_to token = parse_tokens + position;
        parse_alias_trace address_to chain;
        parse_alias_trace address_to trace;
        parse_token address_to replacement = null;
        positive replacement_room = 0;
        positive replacement_count = 0;
        string_address value;
        string_address line;
        positive value_length;
        positive removed = 1;
        b32 forced;
        b32 joined;
        bool final_comment = false;
        bool trailing_blank;

        if (token->kind != PT_WORD || token->alias_forced == 2)
                return false;

        for (chain = token->alias_trace; chain; chain = chain->next)
                if (!string_compare(token->text, chain->name))
                {
                        // Do not reconsider a cycle when an incomplete parse
                        // is walked again after another physical line arrives.
                        token->alias_forced = 2;
                        return false;
                }

        value = alias_lookup(token->text);

        if (!value)
                return false;

        trace = (parse_alias_trace address_to)shell_store_take(
            address_of parse_store, sizeof(*trace));

        if (!trace)
        {
                parse_state = PARSE_SYNTAX;
                return false;
        }

        trace->next = token->alias_trace;
        trace->name = token->text;
        forced = token->alias_forced;
        joined = token->joined;
        value_length = string_length(value);
        trailing_blank = value_length &&
                         (value[value_length - 1] == ' ' ||
                          value[value_length - 1] == '\t');
        line = value;

        while (true)
        {
                b32 count = lex_line(line);
                b32 index;
                string_address next_line;

                if (count < 0)
                {
                        parse_state = PARSE_SYNTAX;
                        goto alias_done;
                }

                if (!shell_array_room(
                        replacement, replacement_room,
                        replacement_count + (positive)count + 1))
                {
                        parse_state = PARSE_SYNTAX;
                        goto alias_done;
                }

                for (index = 0; index < count; index++)
                        if (!parse_copy_lexed(replacement + replacement_count++,
                                              index, trace))
                        {
                                parse_state = PARSE_SYNTAX;
                                goto alias_done;
                        }

                {
                        /*
                                Where the lexer stopped, which its end token
                                holds: the newline that ends this line, a
                                comment in front of it, or the end of the
                                value. A newline inside a quote was lexed as a
                                byte of a word, and cutting the value at the
                                first newline regardless put the rest of that
                                word on a line of its own.
                        */
                        positive stopped = lex_tokens[count].at;

                        next_line = string_first_of(line + stopped, '\n');

                        if (!next_line)
                                final_comment = string_is(line + stopped, '#');
                }

                line = next_line;

                if (!line)
                        break;

                if (!shell_array_room(replacement, replacement_room, replacement_count + 1))
                {
                        parse_state = PARSE_SYNTAX;
                        goto alias_done;
                }

                parse_token_newline(replacement + replacement_count, trace);
                replacement_count++;
                line++;
        }

        if (!shell_array_room(parse_tokens, parse_token_room,
                              parse_token_count + replacement_count + 1))
        {
                parse_state = PARSE_SYNTAX;
                goto alias_done;
        }

        if (final_comment)
                while (position + (b32)removed < (b32)parse_token_count &&
                       parse_tokens[position + removed].kind != PT_NEWLINE)
                        removed++;

        memory_copy(parse_tokens + position + replacement_count,
                    parse_tokens + position + removed,
                    (parse_token_count - (positive)position - removed) *
                        sizeof(parse_tokens[0]));
        parse_token_count = parse_token_count - removed + replacement_count;

        if (replacement_count)
        {
                memory_copy(parse_tokens + position, replacement,
                            replacement_count * sizeof(replacement[0]));
                parse_tokens[position].joined = joined;
                parse_tokens[position].alias_forced = forced;
        }

        if (position + (b32)replacement_count < (b32)parse_token_count)
                parse_tokens[position + replacement_count].joined =
                    replacement_count && !trailing_blank &&
                    parse_tokens[position + replacement_count].joined;

        // A blank at the end of the value makes the next ordinary word an
        // alias candidate too. Redirect operands are not command words and
        // separators begin a fresh command under the ordinary rule.
        if (trailing_blank)
        {
                b32 at = position + (b32)replacement_count;

                while (at < (b32)parse_token_count)
                {
                        b32 prefix = parse_redirect_prefix(at);

                        if (prefix < 0 && parse_tokens[at].kind == PT_WORD)
                        {
                                parse_tokens[at].alias_forced = true;
                                break;
                        }

                        if (parse_tokens[at].kind == PT_NEWLINE ||
                            parse_tokens[at].kind == PT_END ||
                            (parse_tokens[at].kind == PT_OP &&
                             !parse_redirect_operator(parse_tokens[at].op)))
                                break;

                        if (prefix >= 0)
                        {
                                at = parse_after_redirect(at, prefix);
                                continue;
                        }

                        at++;
                }
        }

        // Here-documents introduced by an alias are first visible now. The
        // next physical line is still collected before parsing resumes.
        for (b32 at = position;
             at < position + (b32)replacement_count; at++)
                if (parse_tokens[at].kind == PT_OP &&
                    parse_tokens[at].op == OP_DLESS && !parse_here_at(at))
                {
                        parse_state = PARSE_SYNTAX;
                        break;
                }

alias_done:
        if (replacement)
                memory_free(replacement,
                            replacement_room * sizeof(replacement[0]));

        return !parse_state;
}

// Expand the command-name candidate, skipping assignment prefixes and
// redirects. A replacement can change which token is the candidate, so the
// short scan restarts until it reaches a reserved word or a stable name.
static fn parse_alias_command()
{
        /*
                The commonest shell has no aliases at all, and the scan below
                is per command: it walks the assignment prefixes and the
                redirects of every command in the script to find the name,
                and then asks a table with nothing in it. Both helpers it
                walks with are pure, so with no alias defined the whole pass
                can only end in the same place it starts. It measured 7% of
                a script that is all commands and no aliases.
        */
        if (!alias_count)
                return;

        while (!parse_state)
        {
                b32 at = parse_position;

                while (at < (b32)parse_token_count)
                {
                        parse_token address_to token = parse_tokens + at;
                        positive name_length;

                        if (token->kind == PT_WORD &&
                            shell_assignment_kind(token->text,
                                                  address_of name_length))
                        {
                                at++;
                                continue;
                        }

                        b32 prefix = parse_redirect_prefix(at);

                        if (prefix >= 0)
                        {
                                at = parse_after_redirect(at, prefix);
                                continue;
                        }

                        if (token->kind != PT_WORD ||
                            parse_keyword(at - parse_position) ||
                            parse_word_is_length(at - parse_position,
                                                 "function", 8) ||
                            !parse_alias_replace(at))
                                return;

                        break;
                }

                if (at >= (b32)parse_token_count)
                        return;
        }
}

static bool parse_take_redirect(b32 index)
{
        string_address delimiter;
        b32 descriptor = -1;
        b32 op;
        b32 slot;

        if (parse_look(0)->kind == PT_WORD)
        {
                positive parsed;

                if (!string_digits_exact(parse_look(0)->text,
                                         address_of parsed) ||
                    parsed > 0x7fffffff)
                {
                        parse_state = PARSE_SYNTAX;
                        return false;
                }

                descriptor = (b32)parsed;
                parse_position++;
        }

        op = parse_look(0)->op;
        parse_position++;

        if (descriptor < 0)
                descriptor = (op == OP_LESS || op == OP_DLESS ||
                              op == OP_HERESTRING ||
                              op == OP_LESSAND || op == OP_LESSGREAT)
                                 ? 0
                                 : 1;

        if (parse_look(0)->kind != PT_WORD)
        {
                parse_fail();
                return false;
        }

        delimiter = parse_look(0)->text;

        // The dash of <<-, read here exactly as parse_feed read it when it
        // registered the delimiter. The two have to agree on how many tokens
        // the operator covers or the body and the command come apart.
        if (op == OP_DLESS && parse_look(0)->joined && string_is(delimiter, '-'))
        {
                delimiter++;

                if (!string_get(delimiter))
                {
                        parse_position++;

                        if (parse_look(0)->kind != PT_WORD)
                        {
                                parse_fail();
                                return false;
                        }

                        delimiter = parse_look(0)->text;
                }
        }

        if (parse_redirect_used + 1 >= parse_redirect_top)
        {
                parse_state = PARSE_SYNTAX;
                return false;
        }

        slot = parse_redirect_used++;
        parse_redirects[slot].op = op;
        parse_redirects[slot].fd = descriptor;
        parse_redirects[slot].kept = false;
        parse_redirects[slot].raw = false;
        parse_redirects[slot].body = 0;
        parse_redirects[slot].body_length = 0;
        parse_redirects[slot].text = delimiter;
        parse_redirects[slot].text_length = string_length(delimiter);

        if (op == OP_DLESS)
        {
                if (here_taken >= here_filled)
                {
                        parse_state = PARSE_INCOMPLETE;
                        return false;
                }

                parse_redirects[slot].body = here_documents[here_taken].body;
                parse_redirects[slot].body_length =
                    here_documents[here_taken].length;
                parse_redirects[slot].raw = here_documents[here_taken].quoted;

                if (here_documents[here_taken].overflow)
                {
                        parse_state = PARSE_SYNTAX;
                        return false;
                }

                here_taken++;
        }

        parse_position++;

        if (!parse_nodes[index].redirect_count)
                parse_nodes[index].redirect = slot;

        parse_nodes[index].redirect_count++;

        return !parse_state;
}

static fn parse_take_redirects(b32 index)
{
        while (!parse_state && parse_redirect_prefix(parse_position) >= 0)
                parse_take_redirect(index);
}

static b32 parse_list();
static b32 parse_command();

// The condition of an if and the body of a loop have to contain something.
// An empty one is a script that lost a line, not a command that does nothing.
static b32 parse_list_required()
{
        b32 index = parse_list();

        if (parse_state)
                return 0;

        if (!index)
        {
                parse_fail();
                return 0;
        }

        return index;
}

static b32 parse_simple()
{
        b32 index = parse_node_new(NODE_SIMPLE);

        while (!parse_state)
        {
                if (parse_look(0)->kind == PT_WORD &&
                    parse_look(0)->alias_forced &&
                    parse_look(0)->alias_forced != 2 &&
                    parse_alias_replace(parse_position))
                        continue;

                if (parse_redirect_prefix(parse_position) >= 0)
                {
                        parse_take_redirect(index);
                        continue;
                }

                if (parse_look(0)->kind != PT_WORD)
                        break;

                parse_attach_word(index, parse_look(0)->text,
                                  parse_look(0)->length);
                parse_position++;
        }

        if (parse_state)
                return 0;

        if (!parse_nodes[index].word_count && !parse_nodes[index].redirect_count)
        {
                parse_fail();
                return 0;
        }

        return index;
}

/*
        if, and every elif hanging off it.

        Called with the position just past the "if" or the "elif", which is why
        the two share one function: an elif is an if whose fi belongs to
        somebody further out. The innermost one consumes it and every level
        above returns without looking.
*/
static b32 parse_if_tail()
{
        b32 index = parse_node_new(NODE_IF);

        if (parse_state)
                return 0;

        parse_nodes[index].left = parse_list_required();

        if (parse_state || !parse_expect_word("then"))
                return 0;

        parse_nodes[index].right = parse_list_required();

        if (parse_state)
                return 0;

        if (parse_word_is(0, "elif"))
        {
                parse_position++;
                parse_nodes[index].extra = parse_if_tail();

                return parse_state ? 0 : index;
        }

        if (parse_word_is(0, "else"))
        {
                parse_position++;
                parse_nodes[index].extra = parse_list_required();

                if (parse_state)
                        return 0;
        }

        if (!parse_expect_word("fi"))
                return 0;

        return index;
}

static b32 parse_loop(b32 kind)
{
        b32 index = parse_node_new(kind);

        if (parse_state)
                return 0;

        parse_position++;

        parse_nodes[index].left = parse_list_required();

        if (parse_state || !parse_expect_word("do"))
                return 0;

        parse_nodes[index].right = parse_list_required();

        if (parse_state || !parse_expect_word("done"))
                return 0;

        return index;
}

/*
        for and select, which have one production between them.

        A select is a for that reads the item back from the operator instead
        of walking the list, so everything up to the do is the same words in
        the same places. Only the C-style head belongs to for alone.
*/
static b32 parse_for(b32 kind)
{
        b32 index = parse_node_new(kind);

        if (parse_state)
                return 0;

        parse_position++;

        if (kind == NODE_FOR && parse_look(0)->kind == PT_ARITHMETIC)
        {
                parse_nodes[index].kind = NODE_CFOR;
                parse_attach_word(index, parse_look(0)->text,
                                  parse_look(0)->length);
                parse_position++;

                if (parse_look(0)->kind == PT_OP &&
                    parse_look(0)->op == OP_SEMI)
                        parse_position++;

                parse_skip_newlines();

                if (!parse_expect_word("do"))
                        return 0;

                parse_nodes[index].right = parse_list_required();

                if (parse_state || !parse_expect_word("done"))
                        return 0;

                return index;
        }

        if (parse_look(0)->kind != PT_WORD)
        {
                parse_fail();
                return 0;
        }

        parse_attach_word(index, parse_look(0)->text,
                          parse_look(0)->length);
        parse_position++;

        /* The linebreak production is allowed between the loop variable and
           `in` (or `do`). Without consuming it here, a perfectly ordinary
           multiline `for name\nin ...` reached the separator cleanup below
           without ever recognizing its word list. */
        parse_skip_newlines();

        // Without "in" the loop walks the positional parameters, which is a
        // different thing from walking an empty list.
        if (parse_word_is(0, "in"))
        {
                parse_nodes[index].flags = 1;
                parse_position++;

                /*
                        The list ends at the separator, not at the first word
                        spelled "do".

                        POSIX puts a semicolon or a newline between the list
                        and the do, so "do" among the words is a word like any
                        other -- and stopping at it made "for i in then do"
                        walk one item and then fail to find its own do.
                */
                while (parse_look(0)->kind == PT_WORD)
                {
                        parse_attach_word(index, parse_look(0)->text,
                                          parse_look(0)->length);
                        parse_position++;
                }
        }

        parse_skip_separators();

        if (parse_state || !parse_expect_word("do"))
                return 0;

        parse_nodes[index].right = parse_list_required();

        if (parse_state || !parse_expect_word("done"))
                return 0;

        return index;
}

static b32 parse_case()
{
        b32 index = parse_node_new(NODE_CASE);
        b32 head = 0;
        b32 tail = 0;

        if (parse_state)
                return 0;

        parse_position++;

        if (parse_look(0)->kind != PT_WORD)
        {
                parse_fail();
                return 0;
        }

        parse_attach_word(index, parse_look(0)->text,
                          parse_look(0)->length);
        parse_position++;
        parse_skip_newlines();

        if (!parse_expect_word("in"))
                return 0;

        parse_skip_newlines();

        while (!parse_word_is(0, "esac"))
        {
                b32 item;

                if (parse_look(0)->kind == PT_END)
                {
                        parse_state = PARSE_INCOMPLETE;
                        return 0;
                }

                item = parse_node_new(NODE_CASE_ITEM);

                if (parse_state)
                        return 0;

                if (parse_look(0)->kind == PT_OP && parse_look(0)->op == OP_LPAREN)
                        parse_position++;

                while (1)
                {
                        if (parse_look(0)->kind != PT_WORD)
                        {
                                parse_fail();
                                return 0;
                        }

                        parse_attach_word(item, parse_look(0)->text,
                                          parse_look(0)->length);
                        parse_position++;

                        if (parse_look(0)->kind == PT_OP && parse_look(0)->op == OP_PIPE)
                        {
                                parse_position++;
                                continue;
                        }

                        break;
                }

                if (!parse_expect_operator(OP_RPAREN))
                        return 0;

                parse_skip_newlines();

                parse_nodes[item].right = parse_list();

                if (parse_state)
                        return 0;

                /*
                        Which of the three terminators closed the item, kept
                        on the item because the executor is the only thing
                        that can tell them apart: ;; stops, ;& runs the next
                        body without asking, and ;;& carries on asking.
                */
                if (parse_look(0)->kind == PT_OP)
                {
                        if (parse_look(0)->op == OP_SEMIAND)
                                parse_nodes[item].flags = CASE_FALL_THROUGH;
                        else if (parse_look(0)->op == OP_DSEMIAND)
                                parse_nodes[item].flags = CASE_TEST_ON;

                        if (parse_look(0)->op == OP_DSEMI ||
                            parse_look(0)->op == OP_SEMIAND ||
                            parse_look(0)->op == OP_DSEMIAND)
                                parse_position++;
                }

                parse_skip_newlines();

                if (tail)
                        parse_nodes[tail].next = item;
                else
                        head = item;

                tail = item;
        }

        parse_position++;
        parse_nodes[index].left = head;

        return index;
}

static b32 parse_enclosed(b32 kind)
{
        b32 index = parse_node_new(kind);

        if (parse_state)
                return 0;

        parse_position++;

        parse_nodes[index].left = parse_list_required();

        if (parse_state)
                return 0;

        if (kind == NODE_SUBSHELL)
        {
                if (!parse_expect_operator(OP_RPAREN))
                        return 0;
        }
        else if (!parse_expect_word("}"))
                return 0;

        return index;
}

static b32 parse_function()
{
        b32 index = parse_node_new(NODE_FUNCTION);

        if (parse_state)
                return 0;

        parse_attach_word(index, parse_look(0)->text,
                          parse_look(0)->length);
        parse_position += 3;
        parse_skip_newlines();

        parse_nodes[index].right = parse_command();

        return parse_state ? 0 : index;
}

static b32 parse_function_keyword()
{
        b32 index = parse_node_new(NODE_FUNCTION);

        if (parse_state)
                return 0;

        parse_position++;

        if (parse_look(0)->kind != PT_WORD || parse_reserved(0))
        {
                parse_fail();
                return 0;
        }

        parse_attach_word(index, parse_look(0)->text,
                          parse_look(0)->length);
        parse_position++;

        if (parse_look(0)->kind == PT_OP && parse_look(0)->op == OP_LPAREN &&
            parse_look(1)->kind == PT_OP && parse_look(1)->op == OP_RPAREN)
                parse_position += 2;

        parse_skip_newlines();

        b32 keyword = parse_keyword(0);

        if (!((keyword > PARSE_KEYWORD_NONE &&
               keyword <= PARSE_KEYWORD_OPEN) ||
              (parse_look(0)->kind == PT_OP &&
               parse_look(0)->op == OP_LPAREN)))
        {
                parse_fail();
                return 0;
        }

        parse_nodes[index].right = parse_command();

        return parse_state ? 0 : index;
}

// Whether a compound command begins at this token, which is the one thing
// that tells "coproc NAME { ... }" from "coproc command arguments".
static PURE bool parse_at_compound(b32 ahead)
{
        b32 keyword = parse_keyword(ahead);

        if (keyword > PARSE_KEYWORD_NONE && keyword <= PARSE_KEYWORD_OPEN)
                return true;

        return parse_look(ahead)->kind == PT_ARITHMETIC ||
               parse_look(ahead)->kind == PT_CONDITIONAL ||
               (parse_look(ahead)->kind == PT_OP &&
                parse_look(ahead)->op == OP_LPAREN);
}

/*
        coproc, and what it is going to run.

        The word behind it is a name only when a compound command follows it,
        which is the rule Bash's grammar states and the only thing that tells
        "coproc C { ... }" from "coproc cat". Without one the pair is called
        COPROC, which is the name a script that never said otherwise reads.
*/
static b32 parse_coproc()
{
        b32 index = parse_node_new(NODE_COPROC);

        if (parse_state)
                return 0;

        parse_position++;

        if (parse_look(0)->kind == PT_WORD && !parse_reserved(0) &&
            parse_at_compound(1))
        {
                parse_attach_word(index, parse_look(0)->text,
                                  parse_look(0)->length);
                parse_position++;
        }
        else
                parse_attach_word(index, (string_address) "COPROC", 6);

        if (parse_state)
                return 0;

        parse_nodes[index].left = parse_command();

        return parse_state ? 0 : index;
}

static b32 parse_command()
{
        b32 index;
        b32 compound = true;

        parse_alias_command();

        if (parse_state)
                return 0;

        if (parse_word_is(0, "function"))
                return parse_function_keyword();

        if (parse_look(0)->kind == PT_ARITHMETIC)
        {
                index = parse_node_new(NODE_ARITHMETIC);

                if (!parse_state)
                {
                        parse_attach_word(index, parse_look(0)->text,
                                          parse_look(0)->length);
                        parse_position++;
                }

                goto command_done;
        }

        if (parse_look(0)->kind == PT_CONDITIONAL)
        {
                index = parse_node_new(NODE_CONDITIONAL);

                if (!parse_state)
                {
                        parse_attach_word(index, parse_look(0)->text,
                                          parse_look(0)->length);
                        parse_position++;
                }

                goto command_done;
        }

        b32 keyword = parse_keyword(0);

        if (parse_look(0)->kind == PT_WORD && keyword == PARSE_KEYWORD_NONE &&
            parse_look(1)->kind == PT_OP && parse_look(1)->op == OP_LPAREN &&
            parse_look(2)->kind == PT_OP && parse_look(2)->op == OP_RPAREN)
                return parse_function();

        if (keyword == PARSE_KEYWORD_IF)
        {
                parse_position++;
                index = parse_if_tail();
        }
        else if (keyword == PARSE_KEYWORD_WHILE)
                index = parse_loop(NODE_WHILE);
        else if (keyword == PARSE_KEYWORD_UNTIL)
                index = parse_loop(NODE_UNTIL);
        else if (keyword == PARSE_KEYWORD_FOR)
                index = parse_for(NODE_FOR);
        else if (keyword == PARSE_KEYWORD_SELECT)
                index = parse_for(NODE_SELECT);
        else if (keyword == PARSE_KEYWORD_COPROC)
                index = parse_coproc();
        else if (keyword == PARSE_KEYWORD_CASE)
                index = parse_case();
        else if (keyword == PARSE_KEYWORD_OPEN)
                index = parse_enclosed(NODE_GROUP);
        else if (parse_look(0)->kind == PT_OP && parse_look(0)->op == OP_LPAREN)
                index = parse_enclosed(NODE_SUBSHELL);
        else
        {
                compound = false;
                index = parse_simple();
        }

command_done:
        if (parse_state)
                return 0;

        if (compound)
                parse_take_redirects(index);

        return parse_state ? 0 : index;
}

/*
        The 2>&1 that |& adds to the command in front of the pipe.

        Bash puts it behind whatever redirections the command wrote for
        itself, so a command that sent its errors elsewhere has them brought
        back to the pipe rather than the other way about. Appending is what
        that means here: the redirections of one command are one run, and
        parse_redirect_used is still standing at the end of it.
*/
static bool parse_merge_streams(b32 index)
{
        b32 slot;

        if (parse_redirect_used + 1 >= parse_redirect_top)
        {
                parse_state = PARSE_SYNTAX;
                return false;
        }

        slot = parse_redirect_used++;
        parse_redirects[slot].op = OP_GREATAND;
        parse_redirects[slot].fd = 2;
        parse_redirects[slot].kept = false;
        parse_redirects[slot].raw = false;
        parse_redirects[slot].body = 0;
        parse_redirects[slot].body_length = 0;
        parse_redirects[slot].text = (string_address) "1";
        parse_redirects[slot].text_length = 1;

        if (!parse_nodes[index].redirect_count)
                parse_nodes[index].redirect = slot;

        parse_nodes[index].redirect_count++;

        return true;
}

static PURE inline INLINE bool parse_at_pipe()
{
        return parse_look(0)->kind == PT_OP &&
               (parse_look(0)->op == OP_PIPE ||
                parse_look(0)->op == OP_PIPEAND);
}

static b32 parse_pipeline(bool inverted);

/*
        time, and what it is put in front of.

        The whole of a pipeline is timed, so time takes one and everything
        that may stand in front of a pipeline may stand behind a time: a bang,
        and another time. -p asks for the three POSIX lines instead of
        TIMEFORMAT, and time with nothing behind it times the null command,
        which is how a script asks what the shell has used so far.
*/
static b32 parse_time(bool inverted)
{
        b32 index = parse_node_new(NODE_TIME);

        if (parse_state)
                return 0;

        parse_nodes[index].op = inverted;

        //      Bash marks the command it times rather than wrapping it, so a
        //      time in front of a time times once and not twice.
        while (parse_keyword(0) == PARSE_KEYWORD_TIME)
        {
                parse_position++;

                if (parse_word_is(0, "-p"))
                {
                        parse_nodes[index].flags = 1;
                        parse_position++;
                }

                //      Bash ends the options here and reports the POSIX
                //      three lines whether or not -p was among them.
                if (parse_word_is(0, "--"))
                {
                        parse_nodes[index].flags = 1;
                        parse_position++;
                        break;
                }
        }

        //      Nothing to time is not a missing command: a separator, the end
        //      of a list, or the end of the input all end the construct here
        //      and the null command is what gets measured.
        if (parse_look(0)->kind == PT_NEWLINE || parse_at_list_end() ||
            (parse_look(0)->kind == PT_OP &&
             (parse_look(0)->op == OP_SEMI || parse_look(0)->op == OP_AMP)))
                return index;

        parse_nodes[index].left = parse_pipeline(false);

        return parse_state ? 0 : index;
}

static b32 parse_pipeline(bool inverted)
{
        if (parse_state)
                return 0;

        parse_alias_command();

        if (parse_state)
                return 0;

        /*
                time is four bytes long, and every command in the script comes
                through here. Asking the classifier about each of them to find
                that out costs more than the length does, and this is the one
                place a keyword is looked for before the command is read.
        */
        if (parse_look(0)->kind == PT_WORD && parse_look(0)->length == 4 &&
            parse_keyword(0) == PARSE_KEYWORD_TIME)
                return parse_time(inverted);

        if (!inverted && parse_word_is(0, "!"))
        {
                inverted = true;
                parse_position++;
                parse_skip_newlines();
                parse_alias_command();

                // The grammar has one optional Bang, not a repeatable list.
                // dash rejects a second one rather than cancelling the first.
                if (parse_word_is(0, "!"))
                {
                        parse_state = PARSE_SYNTAX;
                        return 0;
                }

                // A time behind the bang is a whole pipeline of its own, and
                // what the bang inverts is the answer it gives.
                if (parse_look(0)->kind == PT_WORD &&
                    parse_look(0)->length == 4 &&
                    parse_keyword(0) == PARSE_KEYWORD_TIME)
                        return parse_time(true);
        }

        b32 head = parse_command();

        if (parse_state)
                return 0;

        bool piped = parse_at_pipe();

        // The grammar level carries no information in the overwhelmingly
        // common singleton case. Do not put a node in the executor merely to
        // rediscover that fact on every iteration of a loop.
        if (!inverted && !piped)
                return head;

        b32 index = parse_node_new(NODE_PIPELINE);
        b32 tail = head;

        if (parse_state)
                return 0;

        parse_nodes[index].flags = inverted;

        while (piped)
        {
                if (parse_look(0)->op == OP_PIPEAND &&
                    !parse_merge_streams(tail))
                        return 0;

                parse_position++;
                parse_skip_newlines();

                b32 child = parse_command();

                if (parse_state)
                        return 0;

                parse_nodes[tail].next = child;
                tail = child;
                piped = parse_at_pipe();
        }

        parse_nodes[index].left = head;

        return index;
}

static b32 parse_and_or()
{
        if (parse_state)
                return 0;

        b32 head = parse_pipeline(false);

        if (parse_state)
                return 0;

        bool joined = parse_look(0)->kind == PT_OP &&
                      (parse_look(0)->op == OP_AND_IF ||
                       parse_look(0)->op == OP_OR_IF);

        if (!joined)
                return head;

        b32 index = parse_node_new(NODE_ANDOR);
        b32 tail = head;

        if (parse_state)
                return 0;

        while (joined)
        {
                b32 op = parse_look(0)->op;

                parse_position++;
                parse_skip_newlines();

                b32 child = parse_pipeline(false);

                if (parse_state)
                        return 0;

                parse_nodes[child].op = op;
                parse_nodes[tail].next = child;
                tail = child;
                joined = parse_look(0)->kind == PT_OP &&
                         (parse_look(0)->op == OP_AND_IF ||
                          parse_look(0)->op == OP_OR_IF);
        }

        parse_nodes[index].left = head;

        return index;
}

static b32 parse_list()
{
        b32 index = 0;
        b32 head = 0;
        b32 tail = 0;

        if (parse_state)
                return 0;

        parse_skip_newlines();
        parse_alias_command();

        // A semicolon separates two commands; it cannot stand where no
        // command precedes it. Treating it like a blank line made `;` a
        // successful empty program and accepted repeated separators.
        if (parse_look(0)->kind == PT_OP && parse_look(0)->op == OP_SEMI)
        {
                parse_state = PARSE_SYNTAX;
                return 0;
        }

        while (!parse_at_list_end())
        {
                b32 child = parse_and_or();
                bool separated = false;

                if (parse_state)
                        return 0;

                if (parse_look(0)->kind == PT_OP && parse_look(0)->op == OP_AMP)
                {
                        /* A direct singleton may use flags for its own node
                           kind. Background state belongs to an and-or list,
                           so restore that grammar node only for this case. */
                        if (parse_nodes[child].kind != NODE_ANDOR)
                        {
                                b32 wrapper = parse_node_new(NODE_ANDOR);

                                if (parse_state)
                                        return 0;

                                parse_nodes[wrapper].left = child;
                                child = wrapper;
                        }

                        parse_nodes[child].flags = 1;
                        parse_position++;
                        separated = true;
                }
                else if (parse_look(0)->kind == PT_OP && parse_look(0)->op == OP_SEMI)
                {
                        parse_position++;
                        separated = true;
                }
                else if (parse_look(0)->kind == PT_NEWLINE)
                {
                        parse_position++;
                        separated = true;
                }

                if (tail)
                {
                        if (!index)
                        {
                                index = parse_node_new(NODE_LIST);

                                if (parse_state)
                                        return 0;

                                parse_nodes[index].left = head;
                        }

                        parse_nodes[tail].next = child;
                }
                else
                        head = child;

                tail = child;

                if (!separated)
                        break;

                parse_skip_newlines();
                parse_alias_command();

                if (parse_look(0)->kind == PT_OP && parse_look(0)->op == OP_SEMI)
                {
                        parse_state = PARSE_SYNTAX;
                        return 0;
                }
        }

        if (!head)
                return 0;

        return index ? index : head;
}

// Everything read so far, as one tree. Zero with parse_state set to
// PARSE_INCOMPLETE means the source stops in the middle of a construct and the
// caller should ask for another line rather than complain.
b32 parse_program()
{
        b32 root;

        // Half a word is not a program, and the reader is the one who has to
        // be told: there is another line to ask for.
        if (parse_pending_used)
        {
                parse_state = PARSE_INCOMPLETE;
                return 0;
        }

        if (!parse_node_top)
        {
                parse_node_top = PARSE_NODES;
                parse_word_top = PARSE_WORDS;
                parse_redirect_top = PARSE_REDIRECTS;
        }

        parse_position = (b32)parse_token_base;
        parse_state = PARSE_OK;
        parse_node_used = parse_node_base;
        parse_word_used = parse_word_base;
        parse_redirect_used = parse_redirect_base;
        here_taken = 0;

        root = parse_list();

        if (parse_state)
                return 0;

        if (parse_look(0)->kind != PT_END)
        {
                parse_state = PARSE_SYNTAX;
                return 0;
        }

        return root;
}

/*
        A subtree moved out of the way of the next line.

        Everything above is thrown away when the line has run, and a function
        body must not be. The copy goes to the far end of the same three arrays
        and the text to storage of its own, so the tree the executor walks a
        hundred lines later points at nothing that has been reused.

        The far end is a stack, and one definition is one contiguous block in
        each of the four arenas. Where a block ends is therefore enough to say
        whether it is the last one taken, and where it began is enough to give
        it back -- which is what a redefinition does with the body it replaces.
*/
typedef struct
{
        b32 node, word, redirect;
        positive text;
} parse_marks;

fn parse_mark(parse_marks address_to marks)
{
        marks->node = parse_node_top;
        marks->word = parse_word_top;
        marks->redirect = parse_redirect_top;
        marks->text = parse_kept_used;
}

static fn parse_give_back(parse_marks address_to from)
{
        parse_node_top = from->node;
        parse_word_top = from->word;
        parse_redirect_top = from->redirect;
        parse_kept_used = from->text;
}

// A block still on top of the stack, taken back. One that is not stays where
// it is: its space is lost, and nothing that points into it is.
bool parse_release(parse_marks address_to from, parse_marks address_to to)
{
        if (parse_node_top != to->node || parse_word_top != to->word ||
            parse_redirect_top != to->redirect || parse_kept_used != to->text)
                return false;

        parse_give_back(from);

        return true;
}

static b32 parse_keep_words(b32 first, b32 count)
{
        b32 base;
        b32 index;

        if (!count)
                return 0;

        if (parse_word_top - count <= parse_word_used)
                return -1;

        parse_word_top -= count;
        base = parse_word_top;

        for (index = 0; index < count; index++)
        {
                positive text_length = parse_word_lengths[first + index];
                positive length = text_length + 1;

                if (parse_kept_used + length > PARSE_KEPT_TEXT)
                        return -1;

                memory_copy(parse_kept_text + parse_kept_used,
                            parse_words[first + index], length);
                parse_words[base + index] = parse_kept_text + parse_kept_used;
                parse_word_lengths[base + index] = text_length;
                parse_word_name_lengths[base + index] =
                    parse_word_name_lengths[first + index];
                parse_word_name_hashes[base + index] =
                    parse_word_name_hashes[first + index];
                parse_word_flags[base + index] = parse_word_flags[first + index];
                parse_kept_used += length;
        }

        return base;
}

static b32 parse_keep_redirects(b32 first, b32 count)
{
        b32 base;
        b32 index;

        if (!count)
                return 0;

        if (parse_redirect_top - count <= parse_redirect_used)
                return -1;

        parse_redirect_top -= count;
        base = parse_redirect_top;

        for (index = 0; index < count; index++)
        {
                parse_redirects[base + index] = parse_redirects[first + index];

                if (parse_kept_used +
                        parse_redirects[first + index].text_length + 1 >
                    PARSE_KEPT_TEXT)
                        return -1;

                memory_copy_end(
                    parse_kept_text + parse_kept_used,
                    parse_redirects[first + index].text,
                    parse_redirects[first + index].text_length);
                parse_redirects[base + index].text =
                    parse_kept_text + parse_kept_used;
                parse_kept_used +=
                    parse_redirects[first + index].text_length + 1;

                // A here-document body lives in storage the next line reuses,
                // so a kept redirection carries a copy of its own -- taken
                // from the kept text again when the command it belongs to
                // was itself kept, which is a function defined inside one:
                // an offset into the kept text read against here_text is a
                // body from some other line, or none.
                if (parse_redirects[first + index].body_length)
                {
                        positive length = parse_redirects[first + index].body_length;

                        if (parse_kept_used + length + 1 > PARSE_KEPT_TEXT)
                                return -1;

                        memory_copy_end(parse_kept_text + parse_kept_used,
                                        (parse_redirects[first + index].kept
                                             ? parse_kept_text
                                             : here_text) +
                                            parse_redirects[first + index].body,
                                        length);
                        parse_redirects[base + index].body = parse_kept_used;
                        parse_redirects[base + index].kept = true;
                        parse_kept_used += length + 1;
                }
        }

        return base;
}

/*
        Whether the copy ran out anywhere in it.

        Nought is a child that is not there as much as a child that would not
        fit, and the recursion below hands its parent one for the other. A
        body whose loop failed to copy came back as a body that does nothing,
        and the definition was recorded as good.
*/
static bool parse_keep_short;

static b32 parse_keep_tree(b32 index)
{
        b32 copy;

        if (!index)
                return 0;

        if (parse_node_top - 1 <= parse_node_used)
        {
                parse_keep_short = true;
                return 0;
        }

        copy = --parse_node_top;
        parse_nodes[copy] = parse_nodes[index];

        if (parse_nodes[index].word_count)
        {
                b32 base = parse_keep_words(parse_nodes[index].word,
                                            parse_nodes[index].word_count);

                if (base < 0)
                {
                        parse_keep_short = true;
                        return 0;
                }

                parse_nodes[copy].word = base;
        }

        if (parse_nodes[index].redirect_count)
        {
                b32 base = parse_keep_redirects(parse_nodes[index].redirect,
                                                parse_nodes[index].redirect_count);

                if (base < 0)
                {
                        parse_keep_short = true;
                        return 0;
                }

                parse_nodes[copy].redirect = base;
        }

        parse_nodes[copy].left = parse_keep_tree(parse_nodes[index].left);
        parse_nodes[copy].right = parse_keep_tree(parse_nodes[index].right);
        parse_nodes[copy].extra = parse_keep_tree(parse_nodes[index].extra);
        parse_nodes[copy].next = parse_keep_tree(parse_nodes[index].next);

        return copy;
}

/*
        The same copy, and the marks it ends on.

        The recursion gives up wherever it runs out, which leaves four tops
        somewhere in the middle of a tree that will never be walked. Putting
        them back is what turns running out into a message: without it the
        space was gone for good and, worse, the next definition was written
        into the middle of the one that failed.
*/
b32 parse_keep(b32 index, parse_marks address_to marks)
{
        parse_marks before;
        b32 copy;

        parse_mark(address_of before);
        parse_keep_short = false;
        copy = parse_keep_tree(index);

        if (parse_keep_short)
                copy = 0;

        if (!copy)
                parse_give_back(address_of before);

        parse_mark(marks);

        return copy;
}
