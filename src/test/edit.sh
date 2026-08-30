#!/bin/sh
#
#       The editor: the keys it is sent, and the screen they leave behind.
#
#           sh src/test/edit.sh
#
#       src/sh/edit.c makes no system call above its driver heading, which is
#       what makes it testable without a kernel, a pty or a file. The harness
#       below links the core with EDIT_NO_DRIVER and puts term.c behind it, so
#       what the editor writes is fed to the emulator a byte at a time and the
#       assertion is on the cells that came out -- the screen a person would
#       have seen, not the editor's own variables read back.
#
#       That is also what makes partial redraw invisible here: an editor that
#       repaints one row and an editor that repaints the screen leave the same
#       grid, so the cases below say what is on the screen and never how it got
#       there.
#
#       The harness is driven by verbs:
#
#           keys TEXT     keystrokes, in the notation below
#           text TEXT     bytes loaded into the buffer before anything is typed
#           row N         one row of the screen, as [what is on it]
#           dump          every row
#           cursor        where the terminal's cursor is, as row,column
#           attr R,C      the colours of one cell, as ink,paper
#           buffer        the whole buffer, with | between lines
#           idle          a read came back with nothing, which resolves Escape
#
#       Keystroke notation, which is term.sh's with the modifiers added:
#
#           ^A            a control byte
#           <left>        a named key
#           <s-left>      with Shift, <c-left> Control, <a-left> Alt, and any
#                         combination of the three letters
#           anything else itself, with \e \n \r \t \b \xHH
#
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && cd .. && pwd)

cd "$root" || exit 1

compiler=${CC:-gcc}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

pass=0
fail=0
current=""
section_name=""
section_pass=0
section_total=0

section()
{
        [ -z "$section_name" ] ||
                printf '  %-12s %s of %s\n' \
                        "$section_name" "$section_pass" "$section_total"

        [ -z "$section_name" ] || [ -z "${TEST_TALLY:-}" ] ||
                printf '%s %s %s\n' \
                        "$section_name" "$section_pass" "$section_total" \
                        >> "$TEST_TALLY"

        section_name=${1:-}
        section_pass=0
        section_total=0
}

group() { current=$1; }

won()
{
        pass=$((pass + 1))
        section_pass=$((section_pass + 1))
        section_total=$((section_total + 1))
}

lost()
{
        fail=$((fail + 1))
        section_total=$((section_total + 1))
        printf '  %-12s %-24s %s\n' "$current" "$1" "$2"
}

cat > "$work/harness.c" <<'HARNESS'
#include "src/compiler_memory.c"
#include "src/spark.c"
#include "src/canvas/window.c"
#include "src/sh/term.c"

//      The editor, with nothing under it. The driver is the only part of the
//      file that needs a kernel and it is the only part left out.
#define EDIT_NO_DRIVER
#include "src/sh/edit.c"

#define GRID_STRIDE 256
#define GRID_HISTORY 64
#define GRID_LINES (WINDOW_PIXELS + GRID_HISTORY * GRID_STRIDE * (positive)sizeof(struct window_cell))

static p8 page[GRID_LINES + GRID_HISTORY * 4];
static p8 out[65536];
static positive out_length;

static fn say_byte(unsigned int c)
{
        if (out_length < sizeof(out))
                out[out_length++] = (p8)c;
}

static fn tell(const char *text)
{
        while (*text)
                say_byte((p8)*text++);
}

static fn say_number(unsigned int value)
{
        if (value >= 10)
                say_number(value / 10);

        say_byte('0' + value % 10);
}

static fn say_hex(unsigned int value, unsigned int digits)
{
        while (digits--)
                say_byte("0123456789abcdef"[(value >> (digits * 4)) & 15]);
}

static unsigned int number(string_address text)
{
        unsigned int value = 0;

        while (*text >= '0' && *text <= '9')
                value = value * 10 + (unsigned int)(*text++ - '0');

        return value;
}

static bipolar next_byte(string_address text, positive *at)
{
        unsigned int c = text[*at];

        if (!c)
                return -1;

        (*at)++;

        if (c != '\\')
                return (bipolar)c;

        c = text[*at];

        if (!c)
                return '\\';

        (*at)++;

        switch (c)
        {
        case 'e': return 27;
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        case 'b': return 8;
        case '0': return 0;
        case 'x':
        {
                unsigned int value = 0;

                for (int i = 0; i < 2; i++)
                {
                        unsigned int d = text[*at];

                        if (d >= '0' && d <= '9')
                                value = value * 16 + d - '0';
                        else if (d >= 'a' && d <= 'f')
                                value = value * 16 + d - 'a' + 10;
                        else if (d >= 'A' && d <= 'F')
                                value = value * 16 + d - 'A' + 10;
                        else
                                break;

                        (*at)++;
                }

                return (bipolar)value;
        }
        }

        return (bipolar)c;
}

//      Everything the editor emitted, put through the emulator. The two are
//      only ever joined here: the editor writes bytes and the terminal turns
//      them into cells, exactly as they are joined by a pty on a real machine.
static fn settle()
{
        for (positive at = 0; at < edit_emitted_length; at++)
                consume(edit_emitted[at]);

        edit_emitted_length = 0;
}

static const struct
{
        const char *name;
        positive key;
} names[] = {
    {"left", EDIT_KEY_LEFT}, {"right", EDIT_KEY_RIGHT},
    {"up", EDIT_KEY_UP}, {"down", EDIT_KEY_DOWN},
    {"home", EDIT_KEY_HOME}, {"end", EDIT_KEY_END},
    {"pgup", EDIT_KEY_PAGE_UP}, {"pgdn", EDIT_KEY_PAGE_DOWN},
    {"del", EDIT_KEY_REMOVE}, {"bs", EDIT_KEY_BACKSPACE},
    {"tab", EDIT_KEY_TAB}, {"enter", EDIT_KEY_ENTER},
    {"esc", EDIT_KEY_ESCAPE}, {"ins", EDIT_KEY_INSERT},
};

/*
        A named key with its modifiers, as bytes on the wire.

        Written as the sequence a terminal would send rather than delivered
        straight to edit_key, so that every case here goes through the decoder
        as well. A test that called edit_key directly would pass with a decoder
        that had never worked.
*/
static fn feed_named(string_address name, positive length)
{
        positive modifiers = 0;
        positive at = 0;
        positive key = 0;
        p8 sequence[32];
        positive built = 0;

        while (at + 1 < length && name[at + 1] == '-')
        {
                if (name[at] == 's')
                        modifiers |= 1;
                else if (name[at] == 'a')
                        modifiers |= 2;
                else if (name[at] == 'c')
                        modifiers |= 4;

                at += 2;
        }

        for (positive i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        {
                const char *want = names[i].name;
                positive c = at;

                while (*want && c < length && name[c] == (p8)*want)
                {
                        want++;
                        c++;
                }

                if (!*want && c == length)
                {
                        key = names[i].key;
                        break;
                }
        }

        if (!key)
                return;

        //      The keys that are a plain byte stay a plain byte, because that
        //      is what a terminal sends for them.
        if (key == EDIT_KEY_ENTER && !modifiers)
        {
                edit_input_byte('\r');
                return;
        }

        if (key == EDIT_KEY_BACKSPACE && !modifiers)
        {
                edit_input_byte(127);
                return;
        }

        if (key == EDIT_KEY_BACKSPACE && modifiers == 4)
        {
                edit_input_byte(8);
                return;
        }

        if (key == EDIT_KEY_TAB && !modifiers)
        {
                edit_input_byte('\t');
                return;
        }

        if (key == EDIT_KEY_TAB && modifiers == 1)
        {
                edit_input_byte(27);
                edit_input_byte('[');
                edit_input_byte('Z');
                return;
        }

        if (key == EDIT_KEY_ESCAPE)
        {
                edit_input_byte(27);
                edit_input_idle();
                return;
        }

        sequence[built++] = 27;
        sequence[built++] = '[';

        {
                p8 final = 0;
                positive tilde = 0;

                switch (key)
                {
                case EDIT_KEY_UP: final = 'A'; break;
                case EDIT_KEY_DOWN: final = 'B'; break;
                case EDIT_KEY_RIGHT: final = 'C'; break;
                case EDIT_KEY_LEFT: final = 'D'; break;
                case EDIT_KEY_HOME: final = 'H'; break;
                case EDIT_KEY_END: final = 'F'; break;
                case EDIT_KEY_PAGE_UP: tilde = 5; break;
                case EDIT_KEY_PAGE_DOWN: tilde = 6; break;
                case EDIT_KEY_REMOVE: tilde = 3; break;
                case EDIT_KEY_INSERT: tilde = 2; break;
                }

                if (tilde)
                {
                        built += (positive)positive_into(sequence + built, tilde);

                        if (modifiers)
                        {
                                sequence[built++] = ';';
                                built += (positive)positive_into(sequence + built,
                                                                 modifiers + 1);
                        }

                        sequence[built++] = '~';
                }
                else
                {
                        if (modifiers)
                        {
                                sequence[built++] = '1';
                                sequence[built++] = ';';
                                built += (positive)positive_into(sequence + built,
                                                                 modifiers + 1);
                        }

                        sequence[built++] = final;
                }
        }

        for (positive i = 0; i < built; i++)
                edit_input_byte(sequence[i]);
}

static fn feed_keys(string_address text)
{
        positive at = 0;

        while (text[at])
        {
                if (text[at] == '<')
                {
                        positive stop = at + 1;

                        while (text[stop] && text[stop] != '>')
                                stop++;

                        feed_named(text + at + 1, stop - at - 1);
                        at = text[stop] ? stop + 1 : stop;
                        continue;
                }

                if (text[at] == '^' && text[at + 1])
                {
                        unsigned int c = text[at + 1];

                        edit_input_byte((p8)((c >= 'a' && c <= 'z' ? c - 'a'
                                                                   : c - 'A') +
                                             1));
                        at += 2;
                        continue;
                }

                {
                        bipolar c = next_byte(text, &at);

                        if (c < 0)
                                break;

                        edit_input_byte((p8)c);
                }
        }
}

static fn say_row(unsigned int r)
{
        struct window_cell *cells = row_cells(r);
        unsigned int length = *row_length(r);

        say_byte('[');

        for (unsigned int c = 0; c < length; c++)
        {
                unsigned int character = cells[c].character;

                if (character >= ' ' && character < 127)
                        say_byte(character);
                else
                {
                        say_byte('<');
                        say_hex(character, character > 0xffff ? 5 : 4);
                        say_byte('>');
                }
        }

        tell("]\n");
}

b32 main()
{
        b32 count = program_argument_count();
        unsigned int columns, rows;

        if (count < 3)
        {
                log_direct(str("usage: harness columns rows verb [argument] ...\n"));
                return 2;
        }

        columns = number(program_argument(1));
        rows = number(program_argument(2));

        window = (struct window *)page;
        window->stride = GRID_STRIDE;
        window->history = GRID_HISTORY;
        window->lines = GRID_LINES;
        window->head = GRID_HISTORY + rows;
        window->columns = columns;
        window->rows = rows;
        window->max_columns = GRID_STRIDE;
        window->max_rows = GRID_HISTORY;

        COLUMNS = columns;
        ROWS = rows;
        full_reset();

        edit_empty();
        edit_input_reset();
        edit_resize(columns, rows);
        edit_running = true;

        for (b32 i = 3; i < count; i++)
        {
                string_address verb = program_argument(i);
                string_address argument = i + 1 < count ? program_argument(i + 1)
                                                        : (string_address) "";

                if (string_compare(verb, (string_address) "text") == 0)
                {
                        p8 held[8192];
                        positive at = 0;
                        positive built = 0;
                        bipolar c;

                        while ((c = next_byte(argument, &at)) >= 0 &&
                               built < sizeof(held))
                                held[built++] = (p8)c;

                        edit_load(held, built);
                        edit_resize(columns, rows);
                        i++;
                }
                else if (string_compare(verb, (string_address) "keys") == 0)
                {
                        feed_keys(argument);
                        edit_draw();
                        settle();
                        i++;
                }
                else if (string_compare(verb, (string_address) "idle") == 0)
                {
                        edit_input_idle();
                        edit_draw();
                        settle();
                }
                else if (string_compare(verb, (string_address) "draw") == 0)
                {
                        edit_draw();
                        settle();
                }
                else if (string_compare(verb, (string_address) "row") == 0)
                {
                        edit_draw();
                        settle();
                        say_row(number(argument));
                        i++;
                }
                else if (string_compare(verb, (string_address) "dump") == 0)
                {
                        edit_draw();
                        settle();

                        for (unsigned int r = 0; r < ROWS; r++)
                                say_row(r);
                }
                else if (string_compare(verb, (string_address) "cursor") == 0)
                {
                        edit_draw();
                        settle();
                        say_number(row);
                        say_byte(',');
                        say_number(column);
                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "attr") == 0)
                {
                        positive at = 0;
                        unsigned int r = number(argument);
                        unsigned int c;

                        edit_draw();
                        settle();

                        while (argument[at] && argument[at] != ',')
                                at++;

                        c = number(argument + at + 1);
                        say_number(row_cells(r)[c].ink);
                        say_byte(',');
                        say_number(row_cells(r)[c].paper);
                        say_byte('\n');
                        i++;
                }
                else if (string_compare(verb, (string_address) "buffer") == 0)
                {
                        for (positive line = 0; line < edit_line_count; line++)
                        {
                                if (line)
                                        say_byte('|');

                                for (positive at = 0;
                                     at < edit_lines[line].length; at++)
                                {
                                        p8 c = edit_lines[line].text[at];

                                        say_byte(c == '\t' ? '>' : c);
                                }
                        }

                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "carets") == 0)
                {
                        for (positive at = 0; at < edit_cursor_count; at++)
                        {
                                if (at)
                                        say_byte(' ');

                                say_number(edit_cursors[at].line);
                                say_byte(',');
                                say_number(edit_cursors[at].column);

                                if (edit_cursor_has_selection(at))
                                {
                                        say_byte('-');
                                        say_number(edit_cursors[at].anchor_line);
                                        say_byte(',');
                                        say_number(
                                            edit_cursors[at].anchor_column);
                                }
                        }

                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "add") == 0)
                {
                        positive at = 0;

                        while (argument[at] && argument[at] != ',')
                                at++;

                        edit_cursor_add(number(argument),
                                        number(argument + at + 1));
                        i++;
                }
                else if (string_compare(verb, (string_address) "painted") == 0)
                {
                        say_number(edit_rows_painted);
                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "running") == 0)
                {
                        say_number(edit_running != 0);
                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "bytes") == 0)
                {
                        positive length = 0;
                        p8 address_to block = edit_bytes_take(address_of length);

                        for (positive at = 0; at < length; at++)
                                say_byte(block[at] == '\n' ? '|'
                                                            : block[at]);

                        memory_give(block);
                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "steps") == 0)
                {
                        say_number(edit_step_count);
                        say_byte(',');
                        say_number(edit_step_at);
                        say_byte('\n');
                }
                else
                {
                        tell("no verb called ");
                        tell((const char *)verb);
                        say_byte('\n');
                }
        }

        log_direct(out, out_length);
        return 0;
}
HARNESS

if ! $compiler -O2 -static -nostdlib -nostartfiles -fno-stack-protector \
        -fno-builtin -Wall -Wextra -Werror=unused-variable \
        -Werror=unused-but-set-variable -I "$root" -T kit/spark.ld -Wl,-e,_start \
        -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
        -o "$work/edit" "$work/harness.c" 2> "$work/err"
then
        echo "  edit         the harness does not build here, skipped"
        sed 's/^/    /' "$work/err" | head -30
        exit 2
fi

edit() { "$work/edit" "$@" 2>&1 | tr '\n' '|' | sed 's/|$//'; }

same()
{
        name=$1
        want=$2
        shift 2

        got=$(edit "$@")

        if [ "$want" = "$got" ]; then
                won
                return 0
        fi

        lost "$name" "want [$want] got [$got]"
}

#
#       Typing, which is the whole of what an editor has to get right before
#       anything else it does matters.
#

section typing

group letters
same 'a word'          'hello'          40 6 keys 'hello' buffer
same 'on the screen'   '[1 hello]'      40 6 keys 'hello' row 0
same 'caret follows'   '0,7'            40 6 keys 'hello' cursor
same 'no mode to leave' 'q'             40 6 keys 'q' buffer
same 'digits are text' '123'            40 6 keys '123' buffer

group newline
same 'splits'          'ab|cd'          40 6 text 'abcd' keys '<right><right><enter>' buffer
same 'two lines drawn' '[1 ab]|[2 cd]'  40 6 text 'abcd' keys '<right><right><enter>' row 0 row 1
same 'at the end'      'ab|'            40 6 keys 'ab<enter>' buffer

group indent
#       Enter takes the indent of the line it left with it, which is the one
#       piece of cleverness in this editor that a person notices by its absence.
same 'carried down'    '    ab|    cd'  40 6 keys '    ab<enter>cd' buffer
same 'only up to the caret' '  |    ab' 40 6 text '    ab' keys '<home><home><right><right><enter>' buffer
same 'nothing to carry' 'ab|cd'         40 6 keys 'ab<enter>cd' buffer

#
#       Taking it out again.
#

section deleting

group backspace
same 'one character'   'ab'             40 6 keys 'abc<bs>' buffer
same 'joins lines'     'abcd'           40 6 text 'ab\ncd' keys '<down><home><bs>' buffer
same 'caret at the join' '0,4'          40 6 text 'ab\ncd' keys '<down><home><bs>' cursor
same 'at the start does nothing' 'ab'   40 6 text 'ab' keys '<home><bs>' buffer

group delete
same 'forward'         'ac'             40 6 text 'abc' keys '<right><del>' buffer
same 'joins forward'   'abcd'           40 6 text 'ab\ncd' keys '<end><del>' buffer

group word
same 'ctrl backspace'  'one '           40 6 keys 'one two<c-bs>' buffer
same 'stops at the space' 'one two '    40 6 keys 'one two three<c-bs>' buffer

#
#       Selection.
#

section selecting

group shift
same 'right takes one' '0,2-0,0'        40 6 text 'abcd' keys '<s-right><s-right>' carets
same 'shown reversed'  '0,7'            40 6 text 'abcd' keys '<s-right><s-right>' attr 0,2
same 'typing replaces' 'Xcd'            40 6 text 'abcd' keys '<s-right><s-right>X' buffer
same 'left collapses'  '0,0'            40 6 text 'abcd' keys '<s-right><s-right><left>' carets
same 'down selects lines' 'ab|cd'       40 6 text 'ab\ncd' keys '<s-down><s-end>' buffer

group all
same 'ctrl a'          '1,2-0,0'        40 6 text 'ab\ncd' keys '^a' carets
same 'then typed over' 'X'              40 6 text 'ab\ncd' keys '^aX' buffer

#
#       The clipboard.
#

section clipboard

group line
#       Ctrl+C with nothing selected takes the line, and pasting it puts a line
#       back. This is the pair people use without ever selecting anything.
same 'copy and paste'  'ab|ab|cd'       40 6 text 'ab\ncd' keys '^c^v' buffer
same 'cut takes it'    'cd'             40 6 text 'ab\ncd' keys '^x' buffer
same 'cut then paste'  'cd|ab|ef'       40 6 text 'ab\ncd\nef' keys '^x<down>^v' buffer

group selection
same 'copy a span'     'abab'           40 6 text 'ab' keys '^a^c<end>^v' buffer
same 'cut a span'      'cd'             40 6 text 'abcd' keys '<s-right><s-right>^x' buffer

#
#       Undo, which is the part that is usually half done.
#

section undo

group coalescing
#       A run of typed characters is one step, not one a character. Ten
#       characters and one Ctrl+Z is an empty line.
same 'a typed run'     ''               40 6 keys 'hello^z' buffer
same 'one step'        '1,1'            40 6 keys 'hello' steps
same 'broken by a move' 'he'            40 6 keys 'he<left>llo^z' buffer
same 'redo'            'hello'          40 6 keys 'hello^z^y' buffer

group carets
#       Undo puts the carets back where the run started, which is the half of
#       undo that is immediately obvious when it is missing.
same 'restored'        '0,2'            40 6 text 'abcd' keys '<right><right>xyz^z' carets
same 'after a join'    '1,0'            40 6 text 'ab\ncd' keys '<down><home><bs>^z' carets
same 'text after join' 'ab|cd'          40 6 text 'ab\ncd' keys '<down><home><bs>^z' buffer

#
#       Indenting.
#

section indenting

group tab
same 'to the stop'     '        a'      40 6 keys '<tab>a' buffer
same 'a block'         '        ab|        cd' 40 6 text 'ab\ncd' keys '^a<tab>' buffer
same 'and back'        'ab|cd'          40 6 text 'ab\ncd' keys '^a<tab><s-tab>' buffer
same 'dedent one line' 'ab'             40 6 text '        ab' keys '<s-tab>' buffer

#
#       Lines moved and copied.
#

section lines

group move
same 'down'            'cd|ab'          40 6 text 'ab\ncd' keys '<a-down>' buffer
same 'up'              'cd|ab'          40 6 text 'ab\ncd' keys '<down><a-up>' buffer
same 'caret goes too'  '1,0'            40 6 text 'ab\ncd' keys '<a-down>' carets
same 'at the bottom'   'ab|cd'          40 6 text 'ab\ncd' keys '<down><a-down>' buffer
same 'undone whole'    'ab|cd'          40 6 text 'ab\ncd' keys '<a-down>^z' buffer

group copy
same 'down'            'ab|ab|cd'       40 6 text 'ab\ncd' keys '<s-a-down>' buffer
same 'up'              'ab|ab|cd'       40 6 text 'ab\ncd' keys '<s-a-up>' buffer

#
#       Comments.
#

section comments

group toggle
same 'put on'          '# ab'           40 6 text 'ab' keys '\x1f' buffer
same 'taken off'       'ab'             40 6 text 'ab' keys '\x1f\x1f' buffer
same 'a block'         '# ab|# cd'      40 6 text 'ab\ncd' keys '^a\x1f' buffer

#
#       Moving about.
#

section moving

group home
same 'to the text'     '0,4'            40 6 text '    ab' keys '<end><home>' carets
same 'then to nothing' '0,0'            40 6 text '    ab' keys '<end><home><home>' carets

group word
same 'right'           '0,3'            40 6 text 'one two' keys '<c-right>' carets
same 'left'            '0,4'            40 6 text 'one two' keys '<end><c-left>' carets

group file
same 'to the end'      '2,2'            40 6 text 'ab\ncd\nef' keys '<c-end>' carets
same 'to the start'    '0,0'            40 6 text 'ab\ncd\nef' keys '<c-end><c-home>' carets

#
#       The screen itself.
#

section screen

group gutter
same 'numbers'         '[1 ab]|[2 cd]|[3 ef]' 40 6 text 'ab\ncd\nef' row 0 row 1 row 2
same 'past the end'    '[~]'            40 6 text 'ab' row 1
same 'status line'     '0,7'            40 6 text 'ab' attr 5,0

group scroll
#       Six rows is five of text, so a seventh line is off the bottom and the
#       window has to have moved by one.
same 'follows down'    '[2 b]'          40 8 text 'a\nb\nc\nd\ne\nf\ng\nh\ni' keys '<down>' row 1
same 'past the bottom' '[3 c]'          40 8 text 'a\nb\nc\nd\ne\nf\ng\nh\ni' keys '<c-end>' row 0

#
#       Several cursors, which every operation above was written against even
#       though only one of them was ever in the list.
#

section cursors

group two
same 'both type'       'Xab|Xcd'        40 6 text 'ab\ncd' add 1,0 keys 'X' buffer
same 'both keep going' 'XYab|XYcd'      40 6 text 'ab\ncd' add 1,0 keys 'XY' buffer
same 'one undo'        'ab|cd'          40 6 text 'ab\ncd' add 1,0 keys 'XY^z' buffer
same 'both backspace'  'b|c'            40 6 text 'ab\ncd' add 1,1 keys '<right><bs>' buffer
same 'escape drops one' '0,0'           40 6 text 'ab\ncd' add 1,0 keys '<esc>' carets
same 'both listed'     '0,1 1,1'        40 6 text 'ab\ncd' add 1,0 keys 'X' carets
same 'a line moved under one' 'cd|ab|ef' 40 8 text 'ab\ncd\nef' keys '<a-down>' buffer

group same_line
#       Two on one line type into two places at once, and the earlier one being
#       edited must not move the later one out from under itself.
same 'two places'      '0,1 0,3 0,5|XaXbXc' 40 6 text 'abc' add 0,1 add 0,2 keys 'X' carets buffer
same 'and again'       'XYaXYbXYc'      40 6 text 'abc' add 0,1 add 0,2 keys 'XY' buffer

group one_line
#       Two carets on one line are one line for everything that works by the
#       line. Without that, Tab indents it twice and Ctrl+X cuts two.
same 'dedent once'     '        abc'    40 6 text '                abc' add 0,2 keys '<s-tab>' buffer
same 'comment once'    '# abc'          40 6 text 'abc' add 0,2 keys '\x1f' buffer
same 'cut once'        'def'            40 6 text 'abc\ndef' add 0,2 keys '^x' buffer
same 'moved once'      'def|abc'        40 6 text 'abc\ndef' add 0,2 keys '<a-down>' buffer

#
#       What a terminal sends, and what this makes of it.
#

section input

group escape
#       An escape with nothing behind it is the Escape key, and the only thing
#       that says so is that nothing followed it. Escape with a sequence behind
#       it is the sequence.
same 'alone'           '0,0'            40 6 text 'ab\ncd' add 1,0 keys '\e' idle carets
same 'is not an arrow' '0,0'            40 6 text 'ab' keys '<right>' keys '\e[D' carets
same 'twice'           '1'              40 6 text 'ab' keys '\e\e' running

group modifiers
same 'shift right'     '0,1-0,0'        40 6 text 'abc' keys '\e[1;2C' carets
same 'control right'   '0,3'            40 6 text 'one two' keys '\e[1;5C' carets
same 'shift tab is dedent' 'ab'         40 6 text '        ab' keys '\e[Z' buffer
same 'application arrows' '0,1'         40 6 text 'abc' keys '\eOC' carets
same 'csi u redo'      'hi'             40 6 keys 'hi^z\e[122;6u' buffer

group utf8
same 'two bytes'       '[1 <00e9>]'     40 6 keys '\xc3\xa9' row 0
same 'one caret step'  '0,2'            40 6 keys '\xc3\xa9' carets
same 'backspace whole' ''               40 6 keys '\xc3\xa9<bs>' buffer

#
#       The rest of the keys the list names.
#

section keys

group prompts
same 'go to line'      '2,0'            40 8 text 'a\nb\nc\nd' keys '^g3<enter>' carets
same 'cancelled'       '0,0'            40 8 text 'a\nb\nc\nd' keys '^g3<esc>' carets
same 'quit asks'       '1'              40 6 text 'ab' keys 'X^q' running
same 'and lets go'     '0'              40 6 text 'ab' keys 'X^qn' running
same 'unchanged just goes' '0'          40 6 text 'ab' keys '^q' running

group select_line
same 'ctrl l'          '1,0-0,0'        40 6 text 'ab\ncd' keys '^l' carets
same 'again grows'     '1,2-0,0'        40 6 text 'ab\ncd' keys '^l^l' carets

group page
same 'down'            '4,0'            40 6 text 'a\nb\nc\nd\ne\nf\ng' keys '<pgdn>' carets
same 'and back'        '0,0'            40 6 text 'a\nb\nc\nd\ne\nf\ng' keys '<pgdn><pgup>' carets

group scrolling
same 'ctrl down moves the view' '[2 b]' 40 6 text 'a\nb\nc\nd\ne\nf\ng' keys '\e[1;5B' row 0
same 'caret stays'     '0,0'            40 6 text 'a\nb\nc\nd\ne\nf\ng' keys '\e[1;5B' carets
#       The caret is now above the top of the window, so there is nowhere to
#       put the terminal's cursor: it stays hidden and parked, rather than
#       being sent to a row worked out by subtracting one from zero, which on
#       an unsigned count of rows is eighteen quintillion.
same 'cursor parked'   '5,0'            40 6 text 'a\nb\nc\nd\ne\nf\ng' keys '\e[1;5B' cursor
same 'and comes back'  '0,2'            40 6 text 'a\nb\nc\nd\ne\nf\ng' keys '\e[1;5B<down>' cursor

#
#       What a keystroke costs the wire.
#

section painting

group rows
#       Typing a character writes the one row it changed. A full repaint of a
#       screen this size would be five, and over a serial line that is the
#       difference between usable and not.
same 'one row typed'   '1'              40 6 text 'ab\ncd\nef' draw keys 'X' painted
same 'first draw is all' '5'            40 6 text 'ab\ncd\nef' draw painted
same 'nothing changed' '0'              40 6 text 'ab\ncd\nef' draw draw painted

#
#       The file, back out again.
#

section writing

group newline
same 'kept'            'ab|cd|'         40 6 text 'ab\ncd\n' bytes
same 'absent stays absent' 'ab|cd'      40 6 text 'ab\ncd' bytes
same 'empty file'      ''               40 6 bytes


section

printf '  %-12s %s passed, %s failed\n' 'edit' "$pass" "$fail"

[ "$fail" -eq 0 ]
