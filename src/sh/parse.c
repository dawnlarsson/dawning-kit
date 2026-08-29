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


typedef struct
{
        b32 kind;
        b32 op;
        // Whether this token touches the one before it, which is the whole
        // difference between 2>file and 2 >file.
        b32 joined;
        string_address text;
        positive length;
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
static shell_mark parse_text_at;
static parse_token parse_no_token;

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
} parse_node;

typedef struct
{
        b32 op;
        b32 fd;
        b32 word;
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
static parse_redirect parse_redirects[PARSE_REDIRECTS];

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

static b32 parse_position;
static b32 parse_state;

/*
        Where this parse starts, which is not always the beginning.

        eval runs a line from inside a line that is already running, and the
        tree being walked is in these same arrays. So a nested parse claims
        from where the outer one stopped and gives that back when it is done,
        rather than from zero over the top of what is still in use.
*/
static b32 parse_node_base = 1;
static b32 parse_word_base;
static b32 parse_redirect_base;
static positive parse_token_base;
static shell_mark parse_text_base;

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
        b32 overflow;
} here_document;

static here_document address_to here_documents;
static positive here_document_room;
static b32 here_wanted;
static b32 here_filled;
static b32 here_taken;
static p8 address_to here_text;
static positive here_text_room;
static positive here_used;
static p8 address_to here_names;
static positive here_names_room;
static positive here_names_used;

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
typedef struct
{
        b32 node, word, redirect, position, state;
        positive token;
        shell_mark token_text;
        b32 wanted, filled, taken;
        positive used, names_used;
} parse_frame;

static parse_frame address_to parse_frames;
static positive parse_frame_room;
static b32 parse_nest_depth;

fn parse_nest_enter()
{
        parse_frame address_to frame;

        if (parse_nest_depth == 0x7fffffff ||
            !shell_room((address_any address_to)address_of parse_frames,
                        address_of parse_frame_room,
                        (positive)parse_nest_depth + 1,
                        sizeof(parse_frames[0])))
        {
                string_format(log_error, "No room for nested shell input\n");
                log_flush();
                system_call_1(syscall(exit_group), 2);
        }

        frame = parse_frames + parse_nest_depth++;

        frame->node = parse_node_base;
        frame->word = parse_word_base;
        frame->redirect = parse_redirect_base;
        frame->token = parse_token_base;
        frame->token_text = parse_text_base;
        frame->position = parse_position;
        frame->state = parse_state;
        frame->wanted = here_wanted;
        frame->filled = here_filled;
        frame->taken = here_taken;
        frame->used = here_used;
        frame->names_used = here_names_used;

        parse_node_base = parse_node_used;
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

        parse_node_base = frame->node;
        parse_word_base = frame->word;
        parse_redirect_base = frame->redirect;
        parse_token_base = frame->token;
        parse_text_base = frame->token_text;
        parse_position = frame->position;
        parse_state = frame->state;
        here_wanted = frame->wanted;
        here_filled = frame->filled;
        here_taken = frame->taken;
        here_used = frame->used;
        here_names_used = frame->names_used;
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

static parse_token address_to parse_look(b32 ahead)
{
        b32 index = parse_position + ahead;

        if (index < 0 || index >= (b32)parse_token_count)
                return address_of parse_no_token;

        return parse_tokens + index;
}

static bool parse_word_is_length(b32 ahead, string_address text,
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
        PARSE_KEYWORD_THEN,
        PARSE_KEYWORD_ELSE,
        PARSE_KEYWORD_ELIF,
        PARSE_KEYWORD_FI,
        PARSE_KEYWORD_DO,
        PARSE_KEYWORD_DONE,
        PARSE_KEYWORD_CASE,
        PARSE_KEYWORD_ESAC,
        PARSE_KEYWORD_WHILE,
        PARSE_KEYWORD_UNTIL,
        PARSE_KEYWORD_FOR,
        PARSE_KEYWORD_IN,
        PARSE_KEYWORD_BANG,
        PARSE_KEYWORD_OPEN,
        PARSE_KEYWORD_CLOSE,
};

/*
        Classify once instead of asking fifteen exact string comparisons.
        Ordinary commands are overwhelmingly not keywords, so length rejects
        them immediately; the few equal-length candidates are verified by the
        hardware-floor memory comparator.
*/
static b32 parse_keyword(b32 ahead)
{
        parse_token address_to token = parse_look(ahead);
        string_address text;

        if (token->kind != PT_WORD)
                return PARSE_KEYWORD_NONE;

        text = token->text;

        switch (token->length)
        {
        case 1:
                if (text[0] == '!') return PARSE_KEYWORD_BANG;
                if (text[0] == '{') return PARSE_KEYWORD_OPEN;
                if (text[0] == '}') return PARSE_KEYWORD_CLOSE;
                return PARSE_KEYWORD_NONE;
        case 2:
                if (!memory_compare(text, "if", 2)) return PARSE_KEYWORD_IF;
                if (!memory_compare(text, "fi", 2)) return PARSE_KEYWORD_FI;
                if (!memory_compare(text, "do", 2)) return PARSE_KEYWORD_DO;
                if (!memory_compare(text, "in", 2)) return PARSE_KEYWORD_IN;
                return PARSE_KEYWORD_NONE;
        case 3:
                return !memory_compare(text, "for", 3) ? PARSE_KEYWORD_FOR
                                                        : PARSE_KEYWORD_NONE;
        case 4:
                if (!memory_compare(text, "then", 4)) return PARSE_KEYWORD_THEN;
                if (!memory_compare(text, "else", 4)) return PARSE_KEYWORD_ELSE;
                if (!memory_compare(text, "elif", 4)) return PARSE_KEYWORD_ELIF;
                if (!memory_compare(text, "done", 4)) return PARSE_KEYWORD_DONE;
                if (!memory_compare(text, "case", 4)) return PARSE_KEYWORD_CASE;
                if (!memory_compare(text, "esac", 4)) return PARSE_KEYWORD_ESAC;
                return PARSE_KEYWORD_NONE;
        case 5:
                if (!memory_compare(text, "while", 5)) return PARSE_KEYWORD_WHILE;
                if (!memory_compare(text, "until", 5)) return PARSE_KEYWORD_UNTIL;
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

        if (!shell_room((address_any address_to)address_of here_documents,
                        address_of here_document_room,
                        (positive)here_wanted + 1,
                        sizeof(here_documents[0])) ||
            !shell_room((address_any address_to)address_of here_names,
                        address_of here_names_room,
                        here_names_used + reserve, sizeof(here_names[0])))
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
string_address parse_here_open()
{
        if (here_filled >= here_wanted)
                return null;

        return here_names + here_documents[here_filled].delimiter;
}

bool parse_here_line(string_address line)
{
        here_document address_to document;
        positive length;

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
        {
                while (string_is(line, '\t'))
                        line++;

                if (!string_compare(line, here_names + document->delimiter))
                {
                        parse_here_close();
                        return true;
                }
        }

        length = string_length(line);

        if (!document->length)
                document->body = here_used;

        // A body that does not fit is not a body with its end cut off: the
        // reader does not look at what this answers, so the complaint is made
        // here and the command it belongs to is refused when it is parsed.
        if (!shell_room((address_any address_to)address_of here_text,
                        address_of here_text_room,
                        here_used + length + 2, sizeof(here_text[0])))
        {
                if (!document->overflow)
                        string_format(log_error, "Here-document too long: %s\n",
                                      here_names + document->delimiter);

                document->overflow = true;

                return false;
        }

        memory_copy(here_text + here_used, line, length);
        here_used += length;
        here_text[here_used++] = '\n';
        here_text[here_used] = end;
        document->length = here_used - document->body;

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
                if (shell_room((address_any address_to)address_of here_text,
                               address_of here_text_room, here_used + 1,
                               sizeof(here_text[0])))
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
                if (!shell_room((address_any address_to)address_of parse_pending,
                                address_of parse_pending_room, length + 2,
                                sizeof(parse_pending[0])))
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
        positive previous_stop = 0;
        b32 index;
        b32 unfinished;

        // Joining first is what lets the unfinished thing be recognised at
        // all: the quote that closes is on this line and the one that opened
        // it is on the last.
        if (parse_pending_used)
        {
                positive length = string_length(line);

                if (!shell_room((address_any address_to)address_of parse_pending,
                                address_of parse_pending_room,
                                parse_pending_used + length + 2,
                                sizeof(parse_pending[0])))
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

        for (index = 0; index < count; index++)
        {
                lex_token address_to source = lex_tokens + index;
                parse_token address_to into;

                //      Room taken before any address into the table is,
                //      because taking room is what may move it.
                if (!shell_room((address_any address_to)address_of parse_tokens,
                                address_of parse_token_room, parse_token_count + 2,
                                sizeof(parse_token)))
                        return false;

                into = parse_tokens + parse_token_count;
                into->kind = source->kind == LEX_WORD
                                 ? PT_WORD
                                 : source->kind == LEX_ARITHMETIC
                                       ? PT_ARITHMETIC
                                       : source->kind == LEX_CONDITIONAL
                                             ? PT_CONDITIONAL
                                       : PT_OP;
                into->op = source->op;
                into->joined = index && source->at == previous_stop;
                into->text = null;
                into->length = source->length;

                previous_stop = source->at + source->length;

                if (source->kind == LEX_WORD ||
                    source->kind == LEX_ARITHMETIC ||
                    source->kind == LEX_CONDITIONAL)
                {
                        positive length = source->length;

                        p8 address_to kept = shell_store_take(address_of parse_store,
                                                             length + 1);

                        if (!kept)
                                return false;

                        memory_copy_end(kept, source->text, length);
                        into->text = kept;
                }

                parse_token_count++;

                // The body of a here-document is read by the caller before the
                // parser ever runs, so the delimiter has to be noticed now.
                if (into->kind == PT_OP && into->op == OP_DLESS)
                {
                        b32 word = index + 1;
                        string_address delimiter = null;
                        bool strip = false;

                        if (word < count && lex_tokens[word].kind == LEX_WORD)
                                delimiter = lex_tokens[word].text;

                        /*
                                <<- is one operator that arrives as two tokens.

                                The lexer knows << and it does not know <<-, so
                                the dash comes back as the start of the word
                                behind it -- on its own when a blank follows,
                                and stuck to the delimiter when none does. A
                                dash with a blank in front of it is a delimiter
                                beginning with a dash and not the operator.
                        */
                        if (delimiter && string_is(delimiter, '-') &&
                            lex_tokens[word].at == source->at + source->length)
                        {
                                strip = true;
                                delimiter++;

                                if (!string_get(delimiter))
                                {
                                        word++;
                                        delimiter =
                                            word < count &&
                                                    lex_tokens[word].kind == LEX_WORD
                                                ? lex_tokens[word].text
                                                : null;
                                }
                        }

                        if (delimiter && !parse_here_register(delimiter, strip))
                                return false;
                }
        }

        /*
                A comment-only line has no lexer tokens, but it is still a
                line and the parser still needs its newline sentinel.  The
                per-token room check above never runs for that shape.  A
                script beginning with #! therefore wrote through the null
                initial table before it reached its first command.
        */
        if (!shell_room((address_any address_to)address_of parse_tokens,
                        address_of parse_token_room, parse_token_count + 1,
                        sizeof(parse_token)))
                return false;

        parse_tokens[parse_token_count].kind = PT_NEWLINE;
        parse_tokens[parse_token_count].op = 0;
        parse_tokens[parse_token_count].joined = 0;
        parse_tokens[parse_token_count].text = null;
        parse_tokens[parse_token_count].length = 0;
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

        return index;
}

static b32 parse_word_new(string_address text, positive length)
{
        if (parse_word_used + 1 >= parse_word_top)
        {
                parse_state = PARSE_SYNTAX;
                return 0;
        }

        parse_words[parse_word_used] = text;
        parse_word_lengths[parse_word_used] = length;

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
static fn parse_fail()
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
static bool parse_at_list_end()
{
        parse_token address_to token = parse_look(0);
        b32 keyword;

        if (token->kind == PT_END)
                return true;

        if (token->kind == PT_OP &&
            (token->op == OP_RPAREN || token->op == OP_DSEMI))
                return true;

        if (token->kind != PT_WORD)
                return false;

        keyword = parse_keyword(0);

        return keyword == PARSE_KEYWORD_THEN ||
               keyword == PARSE_KEYWORD_ELIF ||
               keyword == PARSE_KEYWORD_ELSE ||
               keyword == PARSE_KEYWORD_FI ||
               keyword == PARSE_KEYWORD_DO ||
               keyword == PARSE_KEYWORD_DONE ||
               keyword == PARSE_KEYWORD_ESAC ||
               keyword == PARSE_KEYWORD_CLOSE;
}

/*
        The reserved words, which are not names.

        Position decides whether one of these is a keyword, but nothing makes
        it a function name: "if() { ...; }" defines something that can never
        be reached, because the word in front of a command is read as the
        keyword every time. dash calls it a syntax error and so does this.
*/
static bool parse_reserved(b32 ahead)
{
        return parse_keyword(ahead) != PARSE_KEYWORD_NONE;
}

static bool parse_redirect_operator(b32 op)
{
        return op == OP_LESS || op == OP_GREAT || op == OP_DGREAT ||
               op == OP_DLESS || op == OP_LESSAND || op == OP_GREATAND ||
               op == OP_LESSGREAT || op == OP_CLOBBER ||
               op == OP_ANDGREAT || op == OP_ANDDGREAT ||
               op == OP_HERESTRING;
}

static bool parse_at_redirect()
{
        parse_token address_to token = parse_look(0);
        positive descriptor;

        if (token->kind == PT_OP && parse_redirect_operator(token->op))
                return true;

        b32 op = parse_look(1)->op;

        // &> always means descriptors one and two. In "echo 2&>file", the 2
        // is therefore an argument, unlike the descriptor prefix in 2>file.
        return token->kind == PT_WORD &&
               string_digits_exact(token->text, address_of descriptor) &&
               descriptor <= 0x7fffffff &&
               parse_look(1)->kind == PT_OP && parse_look(1)->joined &&
               op != OP_ANDGREAT && op != OP_ANDDGREAT &&
               parse_redirect_operator(op);
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
        parse_redirects[slot].word =
            parse_word_new(delimiter, string_length(delimiter));

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
        while (!parse_state && parse_at_redirect())
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

        if (!parse_nodes[index].left)
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
                if (parse_at_redirect())
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

static b32 parse_for()
{
        b32 index = parse_node_new(NODE_FOR);

        if (parse_state)
                return 0;

        parse_position++;

        if (parse_look(0)->kind == PT_ARITHMETIC)
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

                if (parse_look(0)->kind == PT_OP && parse_look(0)->op == OP_DSEMI)
                        parse_position++;

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

        if (!(parse_word_is(0, "if") || parse_word_is(0, "while") ||
              parse_word_is(0, "until") || parse_word_is(0, "for") ||
              parse_word_is(0, "case") || parse_word_is(0, "{") ||
              (parse_look(0)->kind == PT_OP &&
               parse_look(0)->op == OP_LPAREN)))
        {
                parse_fail();
                return 0;
        }

        parse_nodes[index].right = parse_command();

        return parse_state ? 0 : index;
}

static b32 parse_command()
{
        b32 index;
        b32 compound = true;

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

        if (parse_look(0)->kind == PT_WORD && !parse_reserved(0) &&
            parse_look(1)->kind == PT_OP && parse_look(1)->op == OP_LPAREN &&
            parse_look(2)->kind == PT_OP && parse_look(2)->op == OP_RPAREN)
                return parse_function();

        if (parse_word_is(0, "if"))
        {
                parse_position++;
                index = parse_if_tail();
        }
        else if (parse_word_is(0, "while"))
                index = parse_loop(NODE_WHILE);
        else if (parse_word_is(0, "until"))
                index = parse_loop(NODE_UNTIL);
        else if (parse_word_is(0, "for"))
                index = parse_for();
        else if (parse_word_is(0, "case"))
                index = parse_case();
        else if (parse_word_is(0, "{"))
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

static b32 parse_pipeline()
{
        b32 index = parse_node_new(NODE_PIPELINE);
        b32 head = 0;
        b32 tail = 0;

        if (parse_state)
                return 0;

        if (parse_word_is(0, "!"))
        {
                parse_nodes[index].flags = 1;
                parse_position++;

                // The grammar has one optional Bang, not a repeatable list.
                // dash rejects a second one rather than cancelling the first.
                if (parse_word_is(0, "!"))
                {
                        parse_state = PARSE_SYNTAX;
                        return 0;
                }
        }

        while (1)
        {
                b32 child = parse_command();

                if (parse_state)
                        return 0;

                if (tail)
                        parse_nodes[tail].next = child;
                else
                        head = child;

                tail = child;

                if (parse_look(0)->kind == PT_OP && parse_look(0)->op == OP_PIPE)
                {
                        parse_position++;
                        parse_skip_newlines();
                        continue;
                }

                break;
        }

        parse_nodes[index].left = head;

        return index;
}

static b32 parse_and_or()
{
        b32 index = parse_node_new(NODE_ANDOR);
        b32 head = 0;
        b32 tail = 0;
        b32 op = 0;

        if (parse_state)
                return 0;

        while (1)
        {
                b32 child = parse_pipeline();

                if (parse_state)
                        return 0;

                parse_nodes[child].op = op;

                if (tail)
                        parse_nodes[tail].next = child;
                else
                        head = child;

                tail = child;

                if (parse_look(0)->kind == PT_OP &&
                    (parse_look(0)->op == OP_AND_IF || parse_look(0)->op == OP_OR_IF))
                {
                        op = parse_look(0)->op;
                        parse_position++;
                        parse_skip_newlines();
                        continue;
                }

                break;
        }

        parse_nodes[index].left = head;

        return index;
}

static b32 parse_list()
{
        b32 index = parse_node_new(NODE_LIST);
        b32 head = 0;
        b32 tail = 0;

        if (parse_state)
                return 0;

        parse_skip_newlines();

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

                if (parse_state)
                        return 0;

                if (tail)
                        parse_nodes[tail].next = child;
                else
                        head = child;

                tail = child;

                if (parse_look(0)->kind == PT_OP && parse_look(0)->op == OP_AMP)
                {
                        parse_nodes[child].flags = 1;
                        parse_position++;
                }
                else if (parse_look(0)->kind == PT_OP && parse_look(0)->op == OP_SEMI)
                        parse_position++;
                else if (parse_look(0)->kind == PT_NEWLINE)
                        parse_position++;
                else
                        break;

                parse_skip_newlines();

                if (parse_look(0)->kind == PT_OP && parse_look(0)->op == OP_SEMI)
                {
                        parse_state = PARSE_SYNTAX;
                        return 0;
                }
        }

        parse_nodes[index].left = head;

        return index;
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
                b32 word;

                parse_redirects[base + index] = parse_redirects[first + index];
                word = parse_keep_words(parse_redirects[first + index].word, 1);

                if (word < 0)
                        return -1;

                parse_redirects[base + index].word = word;

                // A here-document body lives in storage the next line reuses,
                // so a kept redirection carries a copy of its own.
                if (parse_redirects[first + index].body_length)
                {
                        positive length = parse_redirects[first + index].body_length;

                        if (parse_kept_used + length + 1 > PARSE_KEPT_TEXT)
                                return -1;

                        memory_copy_end(parse_kept_text + parse_kept_used,
                                        here_text + parse_redirects[first + index].body,
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
