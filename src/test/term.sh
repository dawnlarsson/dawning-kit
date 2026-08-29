#!/bin/sh
#
#       The terminal: the sequences it is sent, the cells they become, and
#       the line being typed before it is a line.
#
#           sh src/test/term.sh [the farm of names, which this does not use]
#
#       src/sh/term.c makes no system call, which is what makes it testable
#       without a kernel, a compositor or a pty. The harness below is the
#       emulator with a page of memory standing in for the window Canvas
#       would have mapped: bytes go in, the grid of cells comes out, and a
#       case is a byte stream and the rows it should have written.
#
#       There is no reference terminal here to agree with. The other lanes
#       compare against dash or coreutils; nothing on a machine that boots
#       this renders a grid to compare a grid against, so every case below
#       instead names the sequence and asserts what the standard that defines
#       it says happens -- ECMA-48 for the CSI sequences, the VT100 and VT510
#       manuals for the DEC private ones, and Unicode 3.9 for what a
#       malformed UTF-8 sequence becomes. Where ours knowingly does something
#       else, that is a differs case and says so.
#
#       The harness is driven by verbs:
#
#           in TEXT       bytes for the emulator, with \e \n \r \t \b \xHH
#           keys TEXT     keystrokes: <left> <up> <del> and their like,
#                         ^A for a control character, anything else itself
#           edit on|off   whether the line editor is running
#           row N         one row, as [what has been written to it]
#           dump          every row of the screen
#           back N        the N lines above the screen, out of the ring
#           cursor        where the cursor is, as row,column
#           attr R,C      the colours of one cell, as ink,paper
#           sent          what would have gone up the pty, and forgets it
#           wrote RESULT  apply a write result (count, again, intr, fatal)
#           mode          the modes that are set
#           resize CxR    the window is now this, through the resize path
#           pty           what the seam in screen.c makes of a real one
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

        section_name=$1
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

#       The emulator, with a page of memory where the window would be.
cat > "$work/harness.c" <<'HARNESS'
#include "src/compiler_memory.c"
#include "src/spark.c"
#include "src/canvas/window.c"
#include "src/sh/term.c"
#include "src/sh/screen.c"

#define GRID_STRIDE 256
#define GRID_HISTORY 64
#define GRID_LINES (WINDOW_PIXELS + GRID_HISTORY * GRID_STRIDE * (positive)sizeof(struct window_cell))

static p8 page[GRID_LINES + GRID_HISTORY * 4];
static p8 out[16384];
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
        case 'e':
                return 27;
        case 'n':
                return '\n';
        case 'r':
                return '\r';
        case 't':
                return '\t';
        case 'b':
                return 8;
        case 'a':
                return 7;
        case '0':
                return 0;
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

static const struct
{
        const char *name;
        unsigned int code;
} named_keys[] = {
    {"left", KEY_LEFT}, {"right", KEY_RIGHT}, {"up", KEY_UP}, {"down", KEY_DOWN},
    {"home", KEY_HOME}, {"end", KEY_END}, {"del", KEY_DELETE},
    {"pgup", 104}, {"f1", 59},
};

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

                        for (positive i = 0; i < sizeof(named_keys) / sizeof(named_keys[0]); i++)
                        {
                                const char *name = named_keys[i].name;
                                positive c = at + 1;

                                while (*name && text[c] == (p8)*name)
                                {
                                        name++;
                                        c++;
                                }

                                if (!*name && c == stop)
                                {
                                        term_key(0, named_keys[i].code);
                                        break;
                                }
                        }

                        at = text[stop] ? stop + 1 : stop;
                        continue;
                }

                if (text[at] == '^' && text[at + 1])
                {
                        unsigned int c = text[at + 1];

                        term_key((c >= 'a' && c <= 'z' ? c - 'a' : c - 'A') + 1, 0);
                        at += 2;
                        continue;
                }

                bipolar c = next_byte(text, &at);

                if (c < 0)
                        break;

                term_key((unsigned int)c, 0);
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
                        say_hex(character, character > 0xfffff ? 6 : character > 0xffff ? 5 : 4);
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

        for (b32 i = 3; i < count; i++)
        {
                string_address verb = program_argument(i);
                string_address argument = i + 1 < count ? program_argument(i + 1) : (string_address) "";

                if (string_compare(verb, (string_address) "in") == 0)
                {
                        positive at = 0;
                        bipolar c;

                        while ((c = next_byte(argument, &at)) >= 0)
                                consume((unsigned int)c);

                        i++;
                }
                else if (string_compare(verb, (string_address) "keys") == 0)
                {
                        feed_keys(argument);
                        i++;
                }
                else if (string_compare(verb, (string_address) "edit") == 0)
                {
                        term_line_editing(argument[0] == 'o' && argument[1] == 'n');
                        i++;
                }
                else if (string_compare(verb, (string_address) "row") == 0)
                {
                        say_row(number(argument));
                        i++;
                }
                else if (string_compare(verb, (string_address) "dump") == 0)
                {
                        for (unsigned int r = 0; r < ROWS; r++)
                                say_row(r);
                }
                else if (string_compare(verb, (string_address) "back") == 0)
                {
                        unsigned int how_many = number(argument);

                        for (unsigned int n = how_many; n; n--)
                        {
                                struct window_cell *cells =
                                    window_line(window, window->head - ROWS - n);
                                unsigned int length =
                                    *window_length(window, window->head - ROWS - n);

                                say_byte('[');

                                for (unsigned int c = 0; c < length; c++)
                                        say_byte(cells[c].character >= ' ' &&
                                                         cells[c].character < 127
                                                     ? cells[c].character
                                                     : '?');

                                tell("]\n");
                        }

                        i++;
                }
                else if (string_compare(verb, (string_address) "cursor") == 0)
                {
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

                        while (argument[at] && argument[at] != ',')
                                at++;

                        c = number(argument + at + 1);
                        say_number(row_cells(r)[c].ink);
                        say_byte(',');
                        say_number(row_cells(r)[c].paper);
                        say_byte('\n');
                        i++;
                }
                else if (string_compare(verb, (string_address) "sent") == 0)
                {
                        say_byte('[');

                        for (unsigned int at = 0; at < to_shell_length; at++)
                        {
                                unsigned int c = to_shell[at];

                                if (c == 27)
                                        tell("\\e");
                                else if (c >= ' ' && c < 127)
                                        say_byte(c);
                                else
                                {
                                        tell("\\x");
                                        say_hex(c, 2);
                                }
                        }

                        tell("]\n");
                        to_shell_length = 0;
                }
                else if (string_compare(verb, (string_address) "wrote") == 0)
                {
                        bipolar wrote;

                        if (string_compare(argument, (string_address) "again") == 0)
                                wrote = -EAGAIN;
                        else if (string_compare(argument, (string_address) "intr") == 0)
                                wrote = -EINTR;
                        else if (string_compare(argument, (string_address) "fatal") == 0)
                                wrote = -4095;
                        else
                                wrote = (bipolar)number(argument);

                        tell(term_sent(wrote) ? "alive\n" : "dead\n");
                        i++;
                }
                /*
                        The seam the shipped program actually turns the editor
                        on at, run against a real pty.

                        Every other case says edit on and asks what the editor
                        does. This one asks what says so: a slave in the state
                        the kernel gives it should read as canonical, and the
                        echo should come off it, and the struct that is asked
                        those questions has to be the one the kernel fills in
                        rather than the larger one a C library hands out.
                */
                else if (string_compare(verb, (string_address) "pty") == 0)
                {
                        terminal_modes modes;
                        unsigned int slot = 0;
                        int unlock = 0;
                        p8 name[16] = "/dev/pts/";
                        positive at = 9;
                        b32 master, slave;

                        master = system_call_4(syscall(openat), AT_FDCWD,
                                               (positive) "/dev/ptmx",
                                               FILE_READ_WRITE | O_NONBLOCK, 0);

                        if (master < 0)
                        {
                                tell("no pty\n");
                                continue;
                        }

                        system_call_3(syscall(ioctl), master, TIOCSPTLCK,
                                      (positive)address_of unlock);
                        system_call_3(syscall(ioctl), master, TIOCGPTN,
                                      (positive)address_of slot);

                        if (slot >= 10)
                                name[at++] = (p8)('0' + slot / 10 % 10);

                        name[at++] = (p8)('0' + slot % 10);
                        name[at] = 0;

                        slave = system_call_4(syscall(openat), AT_FDCWD,
                                              (positive)name, FILE_READ_WRITE, 0);

                        if (slave < 0)
                        {
                                tell("no slave\n");
                                continue;
                        }

                        term_follow_modes(master);
                        system_call_3(syscall(ioctl), master, TCGETS,
                                      (positive)address_of modes);

                        tell("editing=");
                        say_number(line_editing != 0);
                        tell(" canonical=");
                        say_number((modes.behaviour & TERMINAL_CANONICAL) != 0);
                        tell(" echo=");
                        say_number((modes.behaviour & TERMINAL_ECHO) != 0);
                        say_byte('\n');

                        // And the far end going raw takes the editor with it.
                        modes.behaviour &= ~TERMINAL_CANONICAL;
                        system_call_3(syscall(ioctl), slave, TCSETS,
                                      (positive)address_of modes);
                        term_follow_modes(master);

                        tell("raw editing=");
                        say_number(line_editing != 0);
                        say_byte('\n');

                        system_call_1(syscall(close), slave);
                        system_call_1(syscall(close), master);
                }
                else if (string_compare(verb, (string_address) "mode") == 0)
                {
                        tell("wrap=");
                        say_number(autowrap != 0);
                        tell(" insert=");
                        say_number(insert_mode != 0);
                        tell(" alternate=");
                        say_number(alternate != 0);
                        tell(" cursor=");
                        say_number(cursor_visible != 0);
                        tell(" region=");
                        say_number(region_top);
                        say_byte(',');
                        say_number(region_bottom);
                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "resize") == 0)
                {
                        positive at = 0;

                        while (argument[at] && argument[at] != 'x')
                                at++;

                        window->columns = number(argument);
                        window->rows = number(argument + at + 1);
                        regrid(-1);
                        i++;
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
        -o "$work/term" "$work/harness.c" 2> "$work/err"
then
        echo "  term         the harness does not build here, skipped"
        sed 's/^/    /' "$work/err" | head -10
        exit 2
fi

#       Rows arrive one to a line and are joined by a bar, so a case that
#       asks for several of them is still one string to compare.
term() { "$work/term" "$@" 2>&1 | tr '\n' '|' | sed 's/|$//'; }

same()
{
        name=$1
        want=$2
        shift 2

        got=$(term "$@")

        if [ "$want" = "$got" ]; then
                won
                return 0
        fi

        lost "$name" "want [$want] got [$got]"
}

#       Ours and the standard disagree, and this is exactly what ours says
#       today. Written down rather than left out, so that closing the gap
#       fails here and says so.
differs() { same "$@"; }

#
#       Characters into cells.
#

section printing

group text
same 'plain'          '[hello]'                  20 3 in 'hello' row 0
same 'carriage'       '[world]'                  20 3 in 'hello\rworld' row 0
same 'linefeed'       '[one]|[two]|[]'           20 3 in 'one\r\ntwo' dump
same 'backspace'      '[hi ]'                    20 3 in 'hix\b \b' row 0
same 'bell is silent' '[ab]'                     20 3 in 'a\ab' row 0

#       ECMA-48 8.3.74: a line feed moves down a line and does not return to
#       the start of it. A vertical tab and a form feed do the same, and both
#       used to be dropped on the floor.
same 'no implicit return' '[abc]|[   de]|[]'     20 3 in 'abc\nde' dump
same 'vertical tab'       '[a]|[ b]|[]'          20 3 in 'a\x0bb' dump
same 'form feed'          '[a]|[ b]|[]'          20 3 in 'a\x0cb' dump

group wrap
#       The last cell of a row is filled and the wrap happens on the next
#       character, not on the one that filled it. A terminal that wraps early
#       puts the cursor on the row below with nothing written on it.
same 'fills the row'    '[abcde]|[]|[]'          5 3 in 'abcde' dump
same 'cursor waits'     '0,5'                    5 3 in 'abcde' cursor
same 'then wraps'       '[abcde]|[f]|[]'         5 3 in 'abcdef' dump
same 'off the bottom'   '[def]|[gh]'             3 2 in 'abcdefgh' dump
same 'kept in the ring' '[abc]'                  3 2 in 'abcdefgh' back 1

group tab
#       Every eighth column to begin with. HTS puts a stop where the cursor
#       is and TBC with a parameter of 3 takes them all away.
same 'to the eighth'     '[a       b]'           20 3 in 'a\tb' row 0
same 'stops at the edge' '0,5'                   6 3 in 'a\t' cursor
same 'set and cleared'   '[Y    XZ b]'           20 3 in 'a\tb\e[3g\e[1G\e[5CX\eH\e[1GY\tZ' row 0

#
#       Where the cursor is put.
#

section cursor

group move
#       ECMA-48 numbers rows and columns from one, and the default for a
#       missing or zero parameter is one.
same 'cup'          '2,4'                        20 5 in '\e[3;5H' cursor
same 'cup defaults' '0,0'                        20 5 in '\e[9;9H\e[H' cursor
same 'cup empty first' '0,4'                     20 5 in '\e[;5H' cursor
same 'up down'      '2,0'                        20 5 in '\e[5;1H\e[3A\e[1B' cursor
same 'right left'   '0,3'                        20 5 in '\e[9C\e[6D' cursor
same 'clamped'      '4,19'                       20 5 in '\e[99;99H' cursor
#       CHA and VPA, which move on one axis and leave the other alone.
same 'cha'          '2,6'                        20 5 in '\e[3;3H\e[7G' cursor
same 'vpa'          '3,2'                        20 5 in '\e[3;3H\e[4d' cursor
same 'cnl cpl'      '2,0'                        20 5 in '\e[1;5H\e[3E\e[1F' cursor

group save
#       DECSC and DECRC keep the colours with the position, which is what
#       makes them usable around a status line.
same 'dec save'     '[abcX]|[def]|[]'            20 3 in 'abc\e7\r\ndef\e8X' dump
same 'dec colours'  '1,0'                        20 3 in '\e[31m\e7\e[32m\e8x' attr 0,0
same 'ansi save'    '[abcX]|[def]|[]'            20 3 in 'abc\e[s\r\ndef\e[uX' dump

group report
#       DSR with a parameter of 6 is answered with CPR, one based, which is
#       the counting CUP takes back.
same 'cursor position' '[\e[3;4R]'               20 5 in '\e[3;4H\e[6n' sent
same 'cursor position three digits' '[\e[3;160R]' 200 5 in '\e[3;160H\e[6n' sent
same 'device status'   '[\e[0n]'                 20 5 in '\e[5n' sent
same 'device attributes' '[\e[?6c]'              20 5 in '\e[c' sent

#
#       Erasing.
#

section erasing

group ed
same 'to the end'   '[ab]|[]|[]'                 20 3 in 'abc\r\ndef\e[1;3H\e[J' dump
#       ED and EL take the cursor's own cell with them: erasing to the start
#       of the page reaches the active position inclusive.
same 'to the start' '[]|[   gh]|[]'              20 3 in 'abc\r\ndefgh\e[2;3H\e[1J' dump
same 'all of it'    '[]|[]|[]'                   20 3 in 'abc\r\ndef\e[2J' dump

group el
same 'line to end'   '[ab]'                      20 3 in 'abcdef\e[1;3H\e[K' row 0
same 'line to start' '[    ef]'                  20 3 in 'abcdef\e[1;4H\e[1K' row 0
same 'whole line'    '[]'                        20 3 in 'abcdef\e[2K' row 0

group ech
#       ECH blanks in place and does not move the cursor or shorten the line.
same 'erases in place' '[   def]'                20 3 in 'abcdef\e[1G\e[3X' row 0
same 'cursor stays'    '0,0'                     20 3 in 'abcdef\e[1G\e[3X' cursor

group background
#       Erasing takes the background in force. Without it a program that
#       paints a coloured panel by clearing to the end of a line gets a black
#       hole instead.
same 'erase in colour' '7,1'                     8 2 in '\e[41m\e[2Jx' attr 0,1
same 'blank in colour' '7,2'                     8 2 in '\e[42m\e[5Cx' attr 0,0

#
#       Lines and characters put in and taken out.
#

section shifting

group characters
#       ICH pushes what is to the right of the cursor further right and
#       DCH pulls it back. What is pushed past the last column is gone.
same 'insert'        '[  abcdef]'                20 3 in 'abcdef\e[1G\e[2@' row 0
same 'delete'        '[def]'                     20 3 in 'abcdef\e[1G\e[3P' row 0
same 'insert at edge' '[  ab]'                   4 3 in 'abcd\e[1G\e[2@' row 0
same 'delete past end' '[abc]'                   20 3 in 'abc\e[1;9H\e[5P' row 0
same 'insert mode'   '[XYabc]'                   20 3 in 'abc\e[1G\e[4hXY' row 0
same 'insert mode off' '[XYc]'                   20 3 in 'abc\e[1G\e[4h\e[4lXY' row 0

group lines
#       IL and DL move the rest of the region and never the rows above the
#       cursor.
same 'insert line'   '[]|[a]|[b]|[c]'            20 4 in 'a\r\nb\r\nc\e[1;1H\e[L' dump
same 'delete line'   '[b]|[c]|[]|[]'             20 4 in 'a\r\nb\r\nc\e[1;1H\e[M' dump
same 'insert below'  '[a]|[]|[b]|[c]'            20 4 in 'a\r\nb\r\nc\e[2;1H\e[L' dump
same 'line attributes move' '1,4'                 20 4 in 'a\r\nb\r\n\e[31;44mX\e[0m\e[2;1H\e[M' attr 1,0
same 'scroll up'     '[b]|[c]|[]|[]'             20 4 in 'a\r\nb\r\nc\e[S' dump
same 'scroll down'   '[]|[a]|[b]|[c]'            20 4 in 'a\r\nb\r\nc\e[T' dump

group region
#       DECSTBM names the rows a scroll happens between. The VT100 manual
#       puts the cursor at the home position of the page, origin mode being
#       what would make that the region instead.
same 'set'           'wrap=1 insert=0 alternate=0 cursor=1 region=1,4' \
                                                 20 6 in '\e[2;4r' mode
same 'homes'         '0,0'                       20 6 in '\e[3;3H\e[2;4r' cursor
same 'one row is none' 'wrap=1 insert=0 alternate=0 cursor=1 region=0,6' \
                                                 20 6 in '\e[3;3r' mode
#       A line feed at the bottom of the region scrolls the region and leaves
#       everything outside it alone.
same 'scrolls inside' '[a]|[c]|[d]|[]|[e]'       20 5 in 'a\r\nb\r\nc\r\nd\e[5;1He\e[2;4r\e[4;1H\n' dump
same 'reset by resize' 'wrap=1 insert=0 alternate=0 cursor=1 region=0,5' \
                                                 20 5 in '\e[2;4r' resize '20x5' mode

#
#       Colour.
#

section colour

group basic
#       ECMA-48 8.3.117 numbers the foreground 30 to 37 and the background
#       40 to 47, with 39 and 49 the defaults and 0 all of it back.
same 'foreground'    '3,0'                       8 2 in '\e[33mx' attr 0,0
same 'background'    '7,4'                       8 2 in '\e[44mx' attr 0,0
same 'both'          '1,6'                       8 2 in '\e[31;46mx' attr 0,0
same 'bold is bright' '9,0'                      8 2 in '\e[1;31mx' attr 0,0
same 'aixterm bright' '12,0'                     8 2 in '\e[94mx' attr 0,0
same 'bright paper'  '7,13'                      8 2 in '\e[105mx' attr 0,0
same 'defaults back' '7,0'                       8 2 in '\e[31;44m\e[39;49mx' attr 0,0
same 'reset'         '7,0'                       8 2 in '\e[31;44m\e[0mx' attr 0,0
same 'no parameter'  '7,0'                       8 2 in '\e[31;44m\e[mx' attr 0,0

group reverse
#       Reverse video is the two colours the other way round when the cell is
#       written, not a flag the compositor is asked to understand.
same 'on'            '0,7'                       8 2 in '\e[7mx' attr 0,0
same 'off'           '3,0'                       8 2 in '\e[33;7m\e[27mx' attr 0,0
same 'with colours'  '4,1'                       8 2 in '\e[31;44;7mx' attr 0,0

group extended
#       The cell carries one of sixteen, so 38;5;n has to land on one of them
#       rather than be ignored -- and ignored is what it was: the number after
#       it fell through to the plain range and painted the text a colour
#       nobody asked for.
same 'first sixteen' '9,0'                       8 2 in '\e[38;5;9mx' attr 0,0
same 'not the plain range' '4,0'                 8 2 in '\e[38;5;31mx' attr 0,0
same 'the cube'      '11,0'                      8 2 in '\e[38;5;226mx' attr 0,0
same 'the greys'     '15,0'                      8 2 in '\e[38;5;255mx' attr 0,0
same 'paper too'     '7,1'                       8 2 in '\e[48;5;1mx' attr 0,0
same 'true colour'   '9,0'                       8 2 in '\e[38;2;250;10;10mx' attr 0,0
same 'and what follows it' '9,4'                 8 2 in '\e[38;2;250;10;10;44mx' attr 0,0
same 'sub parameters' '9,0'                      8 2 in '\e[38:5:9mx' attr 0,0

#
#       Modes.
#

section modes

group autowrap
#       DECAWM off pins the cursor to the last column and every further
#       character overwrites it, which is what stops a status line from
#       scrolling the screen it is drawn on.
same 'on by default' '[abcdef]|[ghi]|[]'         6 3 in 'abcdefghi' dump
same 'off overwrites' '[abcdei]|[]|[]'           6 3 in '\e[?7labcdefghi' dump
same 'on again'      '[abcdef]|[gh]|[]'          6 3 in '\e[?7l\e[?7habcdefgh' dump

group cursor
#       DECTCEM says whether there is a block to draw at all.
same 'hidden'        'wrap=1 insert=0 alternate=0 cursor=0 region=0,3' \
                                                 20 3 in '\e[?25l' mode
same 'shown again'   'wrap=1 insert=0 alternate=0 cursor=1 region=0,3' \
                                                 20 3 in '\e[?25l\e[?25h' mode

group alternate
#       A program is given a screen, scribbles on it and hands it back with
#       what was underneath still there. The ring already holds what was
#       underneath, so entering is head moved on by a screenful and leaving
#       is head moved back: there is no second buffer and nothing is copied.
same 'covers'        '[gone]|[]|[]'              20 3 in 'keep\r\n\e[?1049hgone' dump
same 'gives back'    '[keep]|[]|[]'              20 3 in 'keep\r\n\e[?1049hgone\e[?1049l' dump
same 'the old screen is still there' '[keep]|[]|[]' \
                                                 20 3 in 'keep\r\n\e[?1049hgone' back 3
same 'the shell leaving one it never entered' '[hi]' \
                                                 20 3 in '\e[?1049lhi' row 0
same 'the older spelling' '[gone]|[]|[]'         20 3 in 'keep\r\n\e[?47hgone' dump

group reset
#       RIS puts every one of them back.
same 'ris'           'wrap=1 insert=0 alternate=0 cursor=1 region=0,4' \
                                                 20 4 in '\e[?7l\e[?25l\e[4h\e[2;3r\ec' mode
same 'ris clears'    '[]|[]|[]|[]'               20 4 in 'abc\r\ndef\ec' dump

#
#       Sequences nobody here answers.
#

section swallowed

group string
#       A window title arrives as OSC and every byte of it used to be
#       printed, because the parser gave up at the ] and went back to putting
#       characters in cells.
same 'osc to bell'   '[XY]'                      20 3 in '\e]0;a title\aXY' row 0
same 'osc to st'     '[XY]'                      20 3 in '\e]0;a title\e\\XY' row 0
same 'dcs'           '[XY]'                      20 3 in '\ePq#0;2;0;0;0\e\\XY' row 0
same 'apc'           '[XY]'                      20 3 in '\e_Gf=100,a=T\e\\XY' row 0
same 'privacy message' '[XY]'                    20 3 in '\e^anything\e\\XY' row 0

group private
same 'bracketed paste' '[AB]'                    20 3 in '\e[?2004hA\e[?2004lB' row 0
same 'a private final nobody has' '[AB]'         20 3 in 'A\e[?1000hB' row 0
same 'modifier options'  '[AB]'                  20 3 in 'A\e[>4;2mB' row 0
#       A private marker is not a parameter: ESC[?25l is the cursor and
#       ESC[25l is a mode number nobody uses. Reading the first as the second
#       would put the cursor out at the wrong time.
same 'the marker is kept apart' 'wrap=1 insert=0 alternate=0 cursor=1 region=0,3' \
                                                 20 3 in '\e[25l' mode

group escape
#       An intermediate byte says the sequence carries on to a final one, and
#       the final belongs to that sequence: ESC # 8 is one thing and not a
#       DECRC hiding behind a hash.
same 'character set'  '[XY]'                     20 3 in '\e(BXY' row 0
same 'screen alignment' '[XY]'                   20 3 in '\e#8XY' row 0
same 'not a restore'  '[abXY]'                   20 3 in 'ab\e#8XY' row 0
same 'nothing at all' '[XY]'                     20 3 in '\e\x7fXY' row 0
#       CAN and SUB abandon whatever was being parsed, which is what they are
#       for.
same 'cancelled'      '[X]'                      20 3 in '\e[3;3\x18X' row 0
#       A string sequence that is cut short has to be endable too. Leaving
#       only BEL and ST to end one meant an OSC with neither wedged the
#       parser, and every byte after it went nowhere for good.
same 'cancelled string' '[X]'                    20 3 in '\e]0;ab\x18X' row 0
#       An escape inside a string ends it either way: with a backslash after
#       it that is ST, and with anything else it is the next sequence
#       beginning. Taking it as one is what stops a title nobody terminated
#       from swallowing every sequence sent after it.
same 'a string nobody ended' '[X]'               20 3 in '\e]0;ab\e[41mX' row 0
same 'and what follows it works' '7,1'           20 3 in '\e]0;ab\e[41mX' attr 0,0

group index
#       IND, RI and NEL, which are a line feed, a reverse one, and a line
#       feed that returns.
same 'ind'            '[a]|[ b]|[]'              20 3 in 'a\eDb' dump
same 'nel'            '[a]|[b]|[]'               20 3 in 'a\eEb' dump
same 'ri'             '[ X]|[a]|[b]'             20 3 in 'a\r\nb\eM\eMX' dump

#
#       Bytes into characters.
#

section utf8

group multibyte
#       A cell holds a character and the stream is UTF-8, so the two are not
#       the same thing. The bytes above 127 were being dropped one at a time,
#       and anything a person typed that was not English never arrived.
same 'two bytes'      '[a<00e9>b]'               8 2 in 'a\xc3\xa9b' row 0
same 'three bytes'    '[<2500>]'                 8 2 in '\xe2\x94\x80' row 0
same 'four bytes'     '[<1f600>]'                8 2 in '\xf0\x9f\x98\x80' row 0
same 'one cell each'  '0,3'                      8 2 in '\xc3\xa9\xc3\xa9\xc3\xa9' cursor

group malformed
#       Unicode 3.9, and the substitution of maximal subparts: a sequence
#       that stopped short is one character that never arrived, and the byte
#       that interrupted it is another that did.
same 'cut short'      '[a<fffd>z]'               8 2 in 'a\xc3z' row 0
same 'stray continuation' '[<fffd>a]'            8 2 in '\x80a' row 0
same 'overlong'       '[<fffd>]'                 8 2 in '\xc0\xaf' row 0
same 'a surrogate half' '[<fffd>]'               8 2 in '\xed\xa0\x80' row 0
same 'past the last character' '[<fffd>]'        8 2 in '\xf7\xbf\xbf\xbf' row 0
same 'an escape ends one' '[<fffd>]|[]'          8 2 in '\xc3\e[2;1H' dump

#
#       Keys, on the way out.
#

section keys

group raw
#       With no line editor running a key is its byte and a key that means no
#       character is the sequence ANSI names for it, which is what TERM=ansi
#       claims and this is what makes the claim true.
same 'a character'   '[abc]'                     20 3 edit off keys 'abc' sent
same 'arrows'        '[\e[A\e[B\e[C\e[D]'         20 3 edit off keys '<up><down><right><left>' sent
same 'home and end'  '[\e[H\e[F]'                20 3 edit off keys '<home><end>' sent
same 'delete'        '[\e[3~]'                   20 3 edit off keys '<del>' sent
same 'control'       '[\x01\x03\x1a]'            20 3 edit off keys '^A^C^Z' sent
#       DECCKM sends the arrows as SS3 instead, which is what an application
#       that asked for it is reading for.
same 'application arrows' '[\eOA\eOD]'           20 3 edit off in '\e[?1h' keys '<up><left>' sent
same 'and back again' '[\e[A]'                   20 3 edit off in '\e[?1h\e[?1l' keys '<up>' sent

group writes
#       A nonblocking pty can accept only a prefix or none of a key burst.
#       Only a positive prefix leaves the queue; retryable errors leave every
#       byte, while a fatal/no-progress result tells the event loop to stop.
same 'short keeps tail' 'alive|[cdef]'             20 3 edit off keys 'abcdef' wrote 2 sent
same 'full consumes all' 'alive|[]'                20 3 edit off keys 'abcdef' wrote 6 sent
same 'large count clamps' 'alive|[]'               20 3 edit off keys 'abcdef' wrote 99 sent
same 'again keeps all' 'alive|[abcdef]'            20 3 edit off keys 'abcdef' wrote again sent
same 'interrupt keeps all' 'alive|[abcdef]'        20 3 edit off keys 'abcdef' wrote intr sent
same 'fatal keeps evidence' 'dead|[abcdef]'        20 3 edit off keys 'abcdef' wrote fatal sent
same 'zero cannot spin' 'dead|[abcdef]'            20 3 edit off keys 'abcdef' wrote 0 sent

#
#       The line being typed, before it is a line.
#

section editing

group typing
#       Nothing goes out until the line is finished, and what is typed is
#       drawn here rather than echoed back by the far end.
same 'echoed here'   '[ $ echo hi]'              20 3 edit on in ' $ ' keys 'echo hi' row 0
same 'nothing sent yet' '[]'                     20 3 edit on in ' $ ' keys 'echo hi' sent
same 'sent on enter' '[echo hi\x0a]'             20 3 edit on in ' $ ' keys 'echo hi\r' sent
same 'and moves on'  '1,0'                       20 3 edit on in ' $ ' keys 'echo hi\r' cursor
same 'the prompt is left alone' '[ $ echo hi]'   20 3 edit on in ' $ ' keys 'echo hi\r' row 0

group moving
#       Left and right, and a character put in the middle rather than only at
#       the end. None of this was here: the line discipline can erase the
#       last character and kill the line, and nothing else.
same 'left'          '0,5'                       20 3 edit on in '> ' keys 'echo<left>' cursor
same 'insert inside' '[> ecXho]'                 20 3 edit on in '> ' keys 'echo<left><left>X' row 0
same 'right again'   '[> echXo]'                 20 3 edit on in '> ' keys 'echo<left><left><right>X' row 0
same 'home and end'  '[> Xecho]'                 20 3 edit on in '> ' keys 'echo<home>X' row 0
same 'end'           '[> echoX]'                 20 3 edit on in '> ' keys 'echo<home><end>X' row 0
same 'sent in order' '[abXc\x0a]'                20 3 edit on keys 'abc<left>X\r' sent

group erasing
#       Backspace takes the character before the cursor and Delete takes the
#       one under it. They were the same key: Delete sent ESC [ 3 ~ and the
#       line discipline put all four bytes in the line.
same 'backspace'     '[ab ]'                     20 3 edit on keys 'abc\x7f' row 0
same 'backspace inside' '[ac ]'                  20 3 edit on keys 'abc<left>\x7f' row 0
same 'delete'        '[ac ]'                     20 3 edit on keys 'abc<left><left><del>' row 0
same 'delete at the end' '[abc]'                 20 3 edit on keys 'abc<del>' row 0
same 'backspace at the start' '[abc]'            20 3 edit on keys 'abc<home>\x7f' row 0
same 'what is sent'  '[ac\x0a]'                  20 3 edit on keys 'abc<left>\x7f\r' sent

group control
#       The bindings every shell has had since they were named.
same 'ctrl a'        '0,0'                       20 3 edit on keys 'abcdef^A' cursor
same 'ctrl e'        '0,6'                       20 3 edit on keys 'abcdef^A^E' cursor
same 'ctrl b and f'  '0,5'                       20 3 edit on keys 'abcdef^B^B^F' cursor
same 'ctrl k'        '[abc   ]'                  20 3 edit on keys 'abcdef^A^F^F^F^K' row 0
same 'ctrl u'        '[def   ]'                  20 3 edit on keys 'abcdef^A^F^F^F^U' row 0
same 'ctrl w'        '[one two      ]'           20 3 edit on keys 'one two three^W' row 0
same 'ctrl w again'  '[one          ]'           20 3 edit on keys 'one two three^W^W' row 0
#       Ctrl-D is end of file with nothing to delete and the forward delete
#       with something, which is what every editor with these bindings does.
same 'ctrl d on an empty line' '[\x04]'          20 3 edit on keys '^D' sent
same 'ctrl d with text'        '[]'              20 3 edit on keys 'ab^A^D' sent
same 'ctrl d deletes'          '[b ]'            20 3 edit on keys 'ab^A^D' row 0
#       Ctrl-C leaves what was typed where it is, the way a line that was run
#       would be, and sends the byte for the line discipline to make a signal
#       out of.
same 'ctrl c sends'  '[\x03]'                    20 3 edit on in '$ ' keys 'rubbish^C' sent
same 'ctrl c leaves it' '[$ rubbish]'            20 3 edit on in '$ ' keys 'rubbish^C' row 0
same 'ctrl c moves on'  '1,0'                    20 3 edit on in '$ ' keys 'rubbish^C' cursor

group screen
#       Ctrl-L clears the screen and puts the prompt back. What is to the
#       left of where the line began is the prompt and it is the only part of
#       it this ever knows: the shell wrote it and never said what it was.
same 'cleared'       '[ $ abc]|[]|[]'            20 3 in ' $ ' edit on in 'x\r\n $ ' keys 'abc^L' dump
same 'cursor kept'   '0,6'                       20 3 edit on in ' $ ' keys 'abc^L' cursor
same 'nothing sent'  '[]'                        20 3 edit on in ' $ ' keys 'abc^L' sent

#       Prompt preservation follows the grid width. It used to stop at 128
#       cells and silently move a wider prompt's line back by the missing
#       part when Ctrl-L redrew it.
wide_prompt=p
while [ "${#wide_prompt}" -lt 160 ]; do
        wide_prompt=$wide_prompt$wide_prompt
done
wide_prompt=$(printf '%.*s' 160 "$wide_prompt")
same 'wide prompt kept' "[${wide_prompt}abc]"     200 3 in "$wide_prompt" edit on keys 'abc^L' row 0

group history
#       A history that lasts as long as the session, and the line being typed
#       kept while it is walked so that coming back down returns it.
same 'up'            '[two]'                     20 4 edit on keys 'one\rtwo\r<up>' row 2
same 'up twice'      '[one]'                     20 4 edit on keys 'one\rtwo\r<up><up>' row 2
same 'and down'      '[two]'                     20 4 edit on keys 'one\rtwo\r<up><up><down>' row 2
same 'back to nothing' '[   ]'                   20 4 edit on keys 'one\rtwo\r<up><down>' row 2
same 'what was typed comes back' '[xy ]'         20 4 edit on keys 'one\rxy<up><down>' row 1
same 'the same twice is one' '[same]'            20 4 edit on keys 'same\rsame\r<up><up>' row 2
same 'runs what it found' '[one\x0atwo\x0a]|[one\x0a]' \
                                                 20 4 edit on keys 'one\rtwo\r' sent keys '<up><up>\r' sent
same 'nothing to go back to' '[ab]'              20 4 edit on keys 'ab<up>' row 0

#       Only the newest 32 entries are retained. Walking above that boundary
#       used to circle through overwritten slots and show a newer command as
#       though it were older than the oldest one still held.
history_keys=
history_ups=
history_at=0
while [ "$history_at" -lt 40 ]; do
        history_keys="${history_keys}$(printf '%02d' "$history_at")\\r"
        history_ups="${history_ups}<up>"
        history_at=$((history_at + 1))
done
same 'oldest retained stops' '[08]'               20 4 edit on keys "$history_keys$history_ups" row 3

#       Leaving canonical mode abandons its partial line and its position in
#       a history walk. Re-entering it starts at the newest command.
same 'history resets across modes' '[one\x0atwo\x0a]|[two\x0a]' \
                                                 20 4 edit on keys 'one\rtwo\r<up>' sent \
                                                 edit off edit on keys '<up>\r' sent

group capacity
#       The editor, its history and the pty-bound byte queue grow together.
#       The old independent 1024 and 2048-byte arrays made a long command
#       look complete on screen while executing only a prefix of it.
long_line=x
while [ "${#long_line}" -lt 4096 ]; do
        long_line=$long_line$long_line
done
same 'long line sent whole' "[${long_line}\\x0a]"  256 64 edit on keys "${long_line}\r" sent
same 'long history recalled' "[${long_line}\\x0a]|[${long_line}\\x0a]" \
                                                 256 64 edit on keys "${long_line}\r" sent \
                                                 keys '<up>\r' sent
same 'long raw burst sent' "[${long_line}]"       20 3 edit off keys "$long_line" sent

group unbound
#       A key that means no character and that the editor has no use for is
#       nothing at all. Its sequence would otherwise be sent, and the line
#       discipline puts the bytes of it in the line: Page Up at a prompt ran a
#       command with an escape, a bracket and a tilde in it.
same 'page up sends nothing' '[]'                20 3 edit on keys 'ab<pgup>' sent
same 'and leaves the line'   '[ab]'              20 3 edit on keys 'ab<pgup>' row 0
same 'a function key too'    '[]'                20 3 edit on keys 'ab<f1>' sent
same 'but not with the editor off' '[ab\e[5~]'   20 3 edit off keys 'ab<pgup>' sent

group across
#       A line longer than the row it started on, and one that scrolls the
#       screen out from under itself. The line is anchored to a line of the
#       ring rather than to a row, so it is still where it was drawn.
same 'wraps'         '[> abcdef]|[ghijk]|[]'     8 3 edit on in '> ' keys 'abcdefghijk' dump
same 'cursor wraps'  '1,5'                       8 3 edit on in '> ' keys 'abcdefghijk' cursor
same 'scrolls'       '1,7|[> abcdef]|[ghijklm]'  8 2 edit on in '\r\n> ' keys 'abcdefghijklm' cursor row 0 row 1
same 'edits after wrapping' '[> abcdXe]|[fghijk]|[]' \
                                                 8 3 edit on in '> ' keys 'abcdefghijk<left><left><left><left><left><left><left>X' dump

group off
#       ICANON off is a program that wants its own keys, and handing it a
#       line at a time would be handing it nothing at all until Enter.
same 'passes through' '[a\e[D]'                  20 3 edit off keys 'a<left>' sent
same 'and back on'    '[a]|[]'                   20 3 edit off keys 'a' sent edit on keys 'b' sent
same 'the line goes with it' '[ab ]|[x]'         20 3 edit on keys 'abc\x7f' row 0 edit off keys 'x' sent

#
#       What says the editor is running at all.
#
#       Every case above turns it on itself. These ask the seam that does it
#       in the shipped program: a pty, in the state the kernel gives one, read
#       through the struct the kernel fills in.
#

section discipline

group pty
if [ -c /dev/ptmx ]; then
        same 'canonical is a line editor' \
                'editing=1 canonical=1 echo=0|raw editing=0' 20 3 pty
else
        echo "  discipline   no /dev/ptmx here, skipped"
fi

#
#       The window changing shape.
#

section resize

group grid
same 'wider'         '[hello]'                   10 3 in 'hello' resize '20x3' row 0
same 'the cursor comes with it' '[c]|[d]|[e]|2,1' \
                                                 20 5 in 'a\r\nb\r\nc\r\nd\r\ne' resize '20x3' dump cursor
#       The rows are the last lines of the ring however many there are of
#       them, so a window made shorter while the cursor is at the top of it
#       leaves what was on screen above the screen -- there to scroll back
#       to, and not where it was. The cursor is nearly always at the bottom,
#       which is why this has never been seen.
differs 'shrunk from the top' '[]|0,5'           20 5 in 'hello' resize '20x3' row 0 cursor
#       A window narrower or shorter than one cell is not a grid, and every
#       wrap and scroll divides by these: no rows at all had put wrapping
#       forever looking for a row to land on.
same 'no cells at all' '0,0'                     8 3 in 'abc' resize '0x0' cursor
same 'one cell'        '[c]'                     1 1 in 'abc' row 0
#       Lines are clipped rather than folded: the compositor wraps a line
#       longer than the window it is drawn in, and folding a live row here
#       would move the cursor of the program on the other end of the pty.
same 'clipped not folded' '[hel]'                10 3 in 'hello' resize '3x3' row 0

section ""

total=$((pass + fail))
echo
printf '  %s of %s\n' "$pass" "$total"

[ "$fail" = 0 ]
