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

test_group_width=12
test_case_width=24
. "$here/tally.sh"

cat > "$work/harness.c" <<'HARNESS'
#include "src/compiler_memory.c"
#include "src/spark.c"
#include "src/canvas/window.c"
#include "src/sh/term.c"

//      The editor, with nothing under it. The driver is the only part of the
//      file that needs a kernel and it is the only part left out.
#define EDIT_NO_DRIVER
static positive edit_test_requests;
static positive edit_test_failure;
static bool edit_test_persistent;
#define edit_test_reject() \
        (++edit_test_requests == edit_test_failure || \
         (edit_test_failure && edit_test_persistent && edit_test_requests > edit_test_failure))
#define memory_take(bytes) \
        (edit_test_reject() ? null : memory_take(bytes))
#define memory_resize(...) \
        (edit_test_reject() ? null : memory_resize(__VA_ARGS__))
#define memory_resize_growth(...) \
        (edit_test_reject() ? null : memory_resize_growth(__VA_ARGS__))
#include "src/sh/edit.c"
#undef memory_take
#undef memory_resize
#undef memory_resize_growth

#define TERMINAL_FIXTURE_OUTPUT 65536
#include "src/test/terminal_fixture.inc"
static p8 hostile_path[4096];

// Independent byte oracle for the shared cursor/writer fronts. Include
// wrapped coordinates, every byte (not just valid UTF-8), and both sides of
// the display-width boundary so replacing byte padding with a fill stays exact.
static positive render_decimal(p8 *into, positive value)
{
        p8 reverse[20];
        positive count = 0, used = 0;
        do {
                reverse[count++] = '0' + value % 10;
                value /= 10;
        } while (value);
        while (count) into[used++] = reverse[--count];
        return used;
}

static positive check_render_reuse()
{
        static const positive values[] = {0, 1, 8, 9, 10, 99, 100, 999,
                                         (positive)-2, (positive)-1};
        static const positive lengths[] = {0, 1, 2, 31, 255, 256};
        static const positive widths[] = {0, 1, 8, 40, EDIT_COLUMNS_MAX};
        static const string_address extensions[] = {
            ".c", ".h", ".cc", ".cpp", ".hpp", ".js", ".ts", ".go",
            ".rs", ".inc", ".java", ".lua", ".sql"};
        p8 expected[sizeof(edit_status_bytes)];
        positive checks = 0;

        for (positive y = 0; y < array_count(values); y++)
                for (positive x = 0; x < array_count(values); x++)
                {
                        positive used = 2;
                        expected[0] = 27;
                        expected[1] = '[';
                        used += render_decimal(expected + used, values[y] + 1);
                        expected[used++] = ';';
                        used += render_decimal(expected + used, values[x] + 1);
                        expected[used++] = 'H';
                        edit_emitted_length = 0;
                        edit_say_at(values[y], values[x]);
                        if (edit_emitted_length != used ||
                            memory_compare(edit_emitted, expected, used))
                                return 0;
                        checks++;
                }

        edit_prompt_active = true;
        edit_prompt_label = (string_address)"";
        for (positive seed = 0; seed < 256; seed++)
                for (positive n = 0; n < array_count(lengths); n++)
                        for (positive w = 0; w < array_count(widths); w++)
                        {
                                positive used = 1, cells = 1;
                                expected[0] = ' ';
                                edit_prompt_length = lengths[n];
                                edit_columns = widths[w];
                                for (positive at = 0; at < lengths[n]; at++)
                                {
                                        p8 byte = (p8)(seed + at * 17);
                                        edit_prompt_text[at] = byte;
                                        expected[used++] = byte;
                                        if (byte < 128 || byte >= 192) cells++;
                                }
                                while (cells < widths[w])
                                {
                                        expected[used++] = ' ';
                                        cells++;
                                }
                                memory_fill(edit_status_bytes, 0xa5,
                                            sizeof(edit_status_bytes));
                                edit_status_build();
                                if (edit_status_length != used ||
                                    edit_status_cells != cells ||
                                    memory_compare(edit_status_bytes, expected, used) ||
                                    edit_status_bytes[used] != 0xa5)
                                        return 0;
                                checks++;
                        }

        for (positive at = 0; at < array_count(extensions); at++)
        {
                p8 path[32];
                string_copy(path, (string_address)"file");
                string_append(path, extensions[at]);
                edit_path = path;
                if (string_compare(edit_comment_marker(),
                                   (string_address)(at < 11 ? "// " : "-- ")))
                        return 0;
                checks++;
                string_append(path, (string_address)"x");
                if (string_compare(edit_comment_marker(), (string_address)"# "))
                        return 0;
                checks++;
        }
        edit_path = null;
        if (string_compare(edit_comment_marker(), (string_address)"# ")) return 0;
        checks++;
        edit_path = (string_address)"file.";
        if (string_compare(edit_comment_marker(), (string_address)"# ")) return 0;
        return checks + 1;
}

static unsigned word_kind(p8 byte)
{
        if (byte == ' ' || byte == '\t') return 0;
        return byte >= 128 || byte == '_' || (byte >= 'a' && byte <= 'z') ||
               (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ? 1 : 2;
}

static struct edit_place reference_word(struct edit_place from, bool backward)
{
        const struct edit_line *text = edit_lines + from.line;
        if (backward ? from.column == 0 : from.column == text->length)
        {
                if (backward && from.line)
                        return (struct edit_place){from.line - 1,
                            edit_lines[from.line - 1].length};
                if (!backward && from.line + 1 < edit_line_count)
                        return (struct edit_place){from.line + 1, 0};
                return from;
        }
        positive at = from.column;
        while ((backward ? at != 0 : at != text->length) &&
               !word_kind(text->text[backward ? at - 1 : at]))
                at = backward ? at - 1 : at + 1;
        unsigned kind = backward ? at ? word_kind(text->text[at - 1]) : 0
                                 : at < text->length ? word_kind(text->text[at]) : 0;
        while (kind && (backward ? at != 0 : at != text->length) &&
               word_kind(text->text[backward ? at - 1 : at]) == kind)
        {
                at = backward ? at - 1 : at + 1;
                while ((backward ? at != 0 : at != text->length) &&
                       text->text[at] >= 128 && text->text[at] < 192)
                        at = backward ? at - 1 : at + 1;
        }
        return (struct edit_place){from.line, at};
}

static positive check_word_reuse()
{
        static const positive lengths[] = {0, 1, 2, 7, 31, 63};
        p8 bytes[64];
        positive checks = 0;
        if (!edit_load((string_address)"x\ny\nz", 5)) return 0;
        for (positive seed = 0; seed < 256; seed++)
                for (positive n = 0; n < array_count(lengths); n++)
                {
                        for (positive at = 0; at < lengths[n]; at++)
                                bytes[at] = (p8)(seed + at * 17);
                        for (positive line = 0; line < 3; line++)
                                if (!edit_line_splice(line, 0, edit_lines[line].length,
                                                      bytes, lengths[n])) return 0;
                        for (positive line = 0; line < 3; line++)
                                for (positive at = 0; at <= lengths[n]; at++)
                                        for (positive back = 0; back < 2; back++)
                                        {
                                                struct edit_place from = {line, at};
                                                struct edit_place want = reference_word(from, back);
                                                struct edit_place got = back ? edit_word_left(from)
                                                                            : edit_word_right(from);
                                                if (want.line != got.line ||
                                                    want.column != got.column) return 0;
                                                checks++;
                                        }
                }
        return checks;
}

// Stable coalescing must retain the first selected cursor at each position,
// including its anchor and preferred display column. Build the oracle by
// position, independently of the production sort and compaction walks.
static positive check_cursor_compaction()
{
        static const positive sizes[] = {0, 1, 2, 3, 7, 8, 31, 32,
                                         63, 64, 255, 256};
        struct edit_cursor original[256];
        struct edit_cursor expected[256];
        positive checks = 0;

        if (!edit_cursors_room_for(256))
                return 0;

        for (positive seed = 0; seed < 24; seed++)
                for (positive mode = 0; mode < 4; mode++)
                        for (positive size = 0; size < array_count(sizes); size++)
                        {
                                positive count = sizes[size];
                                positive kept = 0;
                                p32 random = (p32)seed + 1;

                                memory_zero(original, sizeof(original));
                                for (positive at = 0; at < count; at++)
                                {
                                        random = random * 1664525u + 1013904223u;
                                        positive key = mode == 0 ? at
                                            : mode == 1 ? at / 3
                                            : mode == 2 ? 0 : random % count;
                                        if (seed & 1)
                                                key = count - key - 1;
                                        original[at] = (struct edit_cursor){
                                            .line = key / 17, .column = key % 17,
                                            .anchor_line = at, .anchor_column = seed,
                                            .wanted = random,
                                            .selecting = (random & (1u << (seed % 8))) != 0};
                                }

                                for (positive key = 0; key < count; key++)
                                {
                                        struct edit_cursor *chosen = null;

                                        for (positive at = 0; at < count; at++)
                                                if (original[at].line == key / 17 &&
                                                    original[at].column == key % 17 &&
                                                    (!chosen || (!chosen->selecting &&
                                                                 original[at].selecting)))
                                                        chosen = original + at;
                                        if (chosen)
                                                expected[kept++] = *chosen;
                                }

                                memory_copy_apart(edit_cursors, original,
                                                  count * sizeof(original[0]));
                                edit_cursor_count = count;
                                edit_cursors_sort();
                                if (edit_cursor_count != kept ||
                                    memory_compare(edit_cursors, expected,
                                                   kept * sizeof(expected[0])))
                                        return checks;
                                checks++;
                        }

        return checks;
}

// Every subset of rows, moved as contiguous groups, retains its bytes and
// cursor columns. One undo/redo must restore the entire key, not one cursor.
static positive check_line_subsets()
{
        positive checks = 0;
        for (positive count = 1; count <= 7; count++)
                for (positive mask = 1; mask < ((positive)1 << count); mask++)
                        for (positive up = 0; up < 2; up++)
                                for (positive column = 0; column < 3; column++)
                                {
                                        p8 source[32];
                                        positive length = 0, selected = 0;
                                        positive order[7], width[7], columns[7];
                                        for (positive row = 0; row < count; row++)
                                        {
                                                order[row] = row;
                                                width[row] = 1 + row % 3;
                                                columns[row] = column == 2 ? width[row] : column;
                                                if (row)
                                                        source[length++] = '\n';
                                                for (positive byte = 0; byte < width[row]; byte++)
                                                        source[length++] = (p8)('A' + row);
                                        }
                                        if (!edit_load(source, length))
                                                return 0;
                                        for (positive row = 0; row < count; row++)
                                                if (mask & ((positive)1 << row))
                                                {
                                                        if (!selected++)
                                                                edit_place_cursor(row, columns[row], false);
                                                        else if (!edit_cursor_add(row, columns[row]))
                                                                return 0;
                                                }
                                        // Reference permutation of whole contiguous
                                        // groups, including groups blocked at an edge.
                                        for (positive first = 0; first < count;)
                                        {
                                                if (!(mask & ((positive)1 << first)))
                                                {
                                                        first++;
                                                        continue;
                                                }
                                                positive last = first;
                                                while (last + 1 < count &&
                                                       (mask & ((positive)1 << (last + 1))))
                                                        last++;
                                                if (up && first)
                                                {
                                                        order[last] = first - 1;
                                                        for (positive row = first; row <= last; row++)
                                                                order[row - 1] = row;
                                                }
                                                else if (!up && last + 1 < count)
                                                {
                                                        order[first] = last + 1;
                                                        for (positive row = first; row <= last; row++)
                                                                order[row + 1] = row;
                                                }
                                                first = last + 1;
                                        }
                                        edit_move_lines(up);
                                        bool changed = false, valid = true;
                                        for (positive row = 0; row < count; row++)
                                                changed |= order[row] != row;
                                        for (positive phase = 0; phase < 3 && valid; phase++)
                                        {
                                                if (phase && changed)
                                                        valid = edit_step_move(phase == 1);
                                                positive cursor = 0;
                                                valid = valid && edit_line_count == count &&
                                                    edit_cursor_count == selected &&
                                                    edit_step_count == (positive)changed;
                                                for (positive row = 0; row < count && valid; row++)
                                                {
                                                        positive original = phase == 1 ? row : order[row];
                                                        valid = edit_lines[row].length == width[original];
                                                        for (positive byte = 0; byte < width[original] && valid; byte++)
                                                                valid = edit_lines[row].text[byte] == 'A' + original;
                                                        if (mask & ((positive)1 << original))
                                                        {
                                                                struct edit_cursor held = edit_cursors[cursor++];
                                                                valid = valid && held.line == row &&
                                                                    held.column == columns[original] &&
                                                                    held.anchor_line == row &&
                                                                    held.anchor_column == columns[original];
                                                        }
                                                }
                                        }
                                        if (!valid)
                                        {
                                                say_number(count * 10000 + mask * 10 + up * 3 + column);
                                                say_byte(':');
                                                return 0;
                                        }
                                        checks++;
                                }
        return checks;
}

// Overlaps, reversed anchors and exclusive column-zero endpoints must move
// the union of selected rows once, with both cursor endpoints and one undo.
static positive check_line_ranges()
{
        positive checks = 0, failures = 0;
        for (positive count = 2; count <= 6; count++)
        for (positive a = 0; a < count; a++)
        for (positive b = a; b < count; b++)
        for (positive c = 0; c < count; c++)
        for (positive d = c; d < count; d++)
        for (positive flags = 0; flags < 96; flags++)
        {
                bool up = flags & 1, copy = (flags & 32) != 0;
                bool prefixing = (flags & 64) != 0;
                p8 source[32];
                positive length = 0, mask = 0, order[12], total = count;
                for (positive row = 0; row < count; row++)
                {
                        order[row] = row;
                        if (row) source[length++] = '\n';
                        source[length++] = (p8)('A' + row);
                        source[length++] = (p8)('A' + row);
                }
                if (!edit_load(source, length) || !edit_cursors_room_for(2)) return 0;
                edit_cursor_count = 2;
                for (positive i = 0; i < 2; i++)
                {
                        positive first = i ? c : a, last = i ? d : b;
                        bool boundary = (flags & (8 << i)) && last + 1 < count;
                        struct edit_place start = { first, 0 };
                        struct edit_place finish = { last + boundary, boundary ? 0 : 2 };
                        if (flags & (2 << i))
                        {
                                struct edit_place temporary = start;
                                start = finish;
                                finish = temporary;
                        }
                        edit_cursors[i] = (struct edit_cursor){
                                .line = start.line, .column = start.column,
                                .anchor_line = finish.line, .anchor_column = finish.column,
                                .selecting = true };
                }
                edit_cursors_sort();
                positive original_count = edit_cursor_count;
                struct edit_cursor original[2], mapped[2];
                positive first[2], last[2];
                memory_copy_apart(original, edit_cursors, original_count * sizeof(original[0]));
                for (positive i = 0; i < original_count; i++)
                {
                        first[i] = min(original[i].line, original[i].anchor_line);
                        last[i] = max(original[i].line, original[i].anchor_line);
                        if ((original[i].line > original[i].anchor_line && !original[i].column) ||
                            (original[i].anchor_line > original[i].line && !original[i].anchor_column))
                                last[i]--;
                        for (positive row = first[i]; row <= last[i]; row++)
                                mask |= (positive)1 << row;
                }
                for (positive first = 0; !copy && !prefixing && first < count;)
                {
                        if (!(mask & ((positive)1 << first))) { first++; continue; }
                        positive last = first;
                        while (last + 1 < count && (mask & ((positive)1 << (last + 1)))) last++;
                        if (up && first)
                        {
                                order[last] = first - 1;
                                for (positive row = first; row <= last; row++) order[row - 1] = row;
                        }
                        else if (!up && last + 1 < count)
                        {
                                order[first] = last + 1;
                                for (positive row = first; row <= last; row++) order[row + 1] = row;
                        }
                        first = last + 1;
                }
                if (copy)
                {
                        // At most two intervals: merge only actual overlap,
                        // then emit each chosen group twice in original order.
                        positive groups = original_count;
                        if (groups == 2 && first[0] > first[1])
                        {
                                positive held = first[0]; first[0] = first[1]; first[1] = held;
                                held = last[0]; last[0] = last[1]; last[1] = held;
                        }
                        if (groups == 2 && first[1] <= last[0])
                        {
                                last[0] = max(last[0], last[1]);
                                groups = 1;
                        }
                        positive next = 0;
                        total = 0;
                        for (positive group = 0; group < groups; group++)
                        {
                                while (next < first[group]) order[total++] = next++;
                                for (positive twice = 0; twice < 2; twice++)
                                        for (positive row = first[group]; row <= last[group]; row++)
                                                order[total++] = row;
                                next = last[group] + 1;
                        }
                        while (next < count) order[total++] = next++;
                }
                bool changed = copy || prefixing;
                for (positive row = 0; row < count; row++) changed |= row != order[row];
                for (positive i = 0; i < original_count; i++)
                {
                        mapped[i] = original[i];
                        if (prefixing)
                        {
                                if (original[i].column && (mask & ((positive)1 << original[i].line)))
                                        mapped[i].column += up ? 8 : 2;
                                if (original[i].anchor_column && (mask & ((positive)1 << original[i].anchor_line)))
                                        mapped[i].anchor_column += up ? 8 : 2;
                        }
                        bool caret_boundary = original[i].selecting && !original[i].column &&
                                                                    original[i].line > original[i].anchor_line;
                        bool anchor_boundary = original[i].selecting && !original[i].anchor_column &&
                                                                      original[i].anchor_line > original[i].line;
                        for (positive step = 0; step < total; step++)
                        {
                                positive row = copy && up ? total - step - 1 : step;
                                if (order[row] == original[i].line - caret_boundary)
                                        mapped[i].line = row + caret_boundary;
                                if (order[row] == original[i].anchor_line - anchor_boundary)
                                        mapped[i].anchor_line = row + anchor_boundary;
                        }
                }
                if (original_count == 2 && (mapped[0].line > mapped[1].line ||
                        (mapped[0].line == mapped[1].line && mapped[0].column > mapped[1].column)))
                {
                        struct edit_cursor held = mapped[0]; mapped[0] = mapped[1]; mapped[1] = held;
                }
                positive mapped_count = original_count;
                if (mapped_count == 2 && mapped[0].line == mapped[1].line &&
                        mapped[0].column == mapped[1].column) mapped_count = 1;
                if (!prefixing) edit_transfer_lines(up, copy);
                else if (up) edit_indent_lines(false);
                else edit_toggle_comment();
                bool valid = true;
                for (positive phase = 0; phase < 3 && valid; phase++)
                {
                        if (phase && changed) valid = edit_step_move(phase == 1);
                        positive rows = phase == 1 ? count : total;
                        valid = valid && edit_line_count == rows && edit_step_count == (positive)changed;
                        for (positive row = 0; row < rows && valid; row++)
                        {
                                positive prefix = prefixing && phase != 1 &&
                                    (mask & ((positive)1 << row)) ? (up ? 8 : 2) : 0;
                                valid = edit_lines[row].length == 2 + prefix &&
                                        edit_lines[row].text[prefix] == 'A' + (phase == 1 ? row : order[row]) &&
                                        edit_lines[row].text[prefix + 1] == 'A' + (phase == 1 ? row : order[row]);
                                for (positive byte = 0; byte < prefix && valid; byte++)
                                        valid = edit_lines[row].text[byte] == (!up && !byte ? '#' : ' ');
                        }
                        positive expected_count = phase == 1 ? original_count : mapped_count;
                        struct edit_cursor *expected = phase == 1 ? original : mapped;
                        valid = valid && edit_cursor_count == expected_count;
                        for (positive i = 0; i < expected_count && valid; i++)
                                valid = edit_cursors[i].line == expected[i].line &&
                                        edit_cursors[i].column == expected[i].column &&
                                        edit_cursors[i].anchor_line == expected[i].anchor_line &&
                                        edit_cursors[i].anchor_column == expected[i].anchor_column &&
                                        edit_cursors[i].selecting == expected[i].selecting;
                }
                checks++;
                if (!valid)
                {
                        if (failures < 12)
                        {
                                say_number(count); say_byte(':'); say_number(a); say_byte('-'); say_number(b);
                                say_byte(','); say_number(c); say_byte('-'); say_number(d);
                                say_byte('/'); say_number(flags); say_byte(' ');
                                for (positive row = 0; row < count; row++) say_byte(edit_lines[row].text[0]);
                                say_byte('/');
                                for (positive row = 0; row < count; row++) say_byte('A' + order[row]);
                                say_byte('\n');
                        }
                        failures++;
                }
        }
        return failures ? 0 : checks;
}


/* Fail each allocation/reservation request in a line operation, including
   journalling and staged document lines. Either the original remains intact,
   or the complete edit, its cursors, and its undo/redo state agree. */
static positive check_multicursor_journal()
{
        positive checks = 0;
        for (positive mode = 0; mode < 6; mode++)
        {
                positive requests = 0;
                for (positive fail_at = 0; fail_at <= requests; fail_at++)
                {
                        edit_test_failure = 0;
                        if (!edit_load("aa\nbb\ncc\ndd\nee", 14)) return 0;
                        edit_place_cursor(1, 1, false);
                        if (!edit_cursor_add(3, 1)) return 0;
                        struct edit_cursor before[2], after[2];
                        memory_copy_apart(before, edit_cursors, sizeof(before));
                        edit_test_requests = 0;
                        edit_test_failure = fail_at;
                        if (mode < 4) edit_transfer_lines(mode & 1, mode & 2);
                        else if (mode == 4) edit_indent_lines(false);
                        else edit_toggle_comment();
                        if (!fail_at) requests = edit_test_requests;
                        edit_test_failure = 0;
                        positive length;
                        p8 address_to bytes = edit_bytes_take(address_of length);
                        if (!bytes || edit_cursor_count != 2) return 0;
                        memory_copy_apart(after, edit_cursors, sizeof(after));
                        bool valid = edit_step_count <= 1;
                        for (positive phase = 0; phase < 2 && valid; phase++)
                        {
                                if (edit_step_count) valid = edit_step_move(!phase);
                                positive size;
                                p8 address_to restored = edit_bytes_take(address_of size);
                                valid = valid && restored && size == (phase ? length : 14) &&
                                    !memory_compare(restored, phase ? bytes : (string_address)"aa\nbb\ncc\ndd\nee", size) &&
                                    edit_cursor_count == 2 &&
                                    !memory_compare(edit_cursors, phase ? after : before, sizeof(before));
                                memory_give(restored);
                        }
                        memory_give(bytes);
                        if (!valid)
                        {
                                say_number(mode * 1000 + fail_at); say_byte(':');
                                return 0;
                        }
                        checks++;
                }
        }
        return checks;
}

struct edit_test_state
{
        p8 bytes[1024];
        struct edit_cursor cursors[8];
        positive length, lines, count, step, steps;
        bool empty, final_newline, modified;
};

static bool edit_test_state_take(struct edit_test_state *state)
{
        state->length = edit_span_length((struct edit_place){0, 0}, edit_place_last());
        if (state->length > sizeof(state->bytes) || edit_cursor_count > array_count(state->cursors))
                return false;
        edit_span_copy((struct edit_place){0, 0}, edit_place_last(), state->bytes);
        memory_copy_apart(state->cursors, edit_cursors, edit_cursor_count * sizeof(*edit_cursors));
        state->lines = edit_line_count;
        state->count = edit_cursor_count;
        state->step = edit_step_at;
        state->steps = edit_step_count;
        state->empty = edit_empty_file;
        state->final_newline = edit_final_newline;
        state->modified = edit_modified;
        return true;
}

static bool edit_test_state_same(struct edit_test_state *expected)
{
        struct edit_test_state actual;
        return edit_test_state_take(&actual) && actual.length == expected->length &&
            actual.lines == expected->lines && actual.count == expected->count &&
            actual.step == expected->step && actual.steps == expected->steps &&
            actual.empty == expected->empty && actual.final_newline == expected->final_newline &&
            actual.modified == expected->modified &&
            !memory_compare(actual.bytes, expected->bytes, actual.length) &&
            !memory_compare(actual.cursors, expected->cursors, actual.count * sizeof(*edit_cursors));
}

// Every three-way transition among an in-line splice, a split and a join;
// plus disjoint line moves, repeated overlapping joins and an empty file.
static bool edit_test_history(positive scenario, struct edit_test_state *before,
                              struct edit_test_state *after)
{
        if (!edit_load((string_address)"aa\nbb\ncc\ndd\nee", scenario == 29 ? 0 : 14))
                return false;
        edit_place_cursor(0, 1, false);
        if (scenario != 29)
        {
                if (!edit_cursor_add(3, 1)) return false;
                edit_cursors[1].selecting = true;
                edit_cursors[1].anchor_line = 2;
                edit_cursors[1].anchor_column = 0;
        }
        if (!edit_test_state_take(before)) return false;
        if (scenario == 27)
                edit_move_lines(false);
        else if (scenario == 28)
        {
                for (positive at = 0; at < 4; at++)
                        edit_change(((struct edit_place){0, edit_lines[0].length}),
                            ((struct edit_place){1, 0}), null, 0, EDIT_STEP_OTHER);
        }
        else
        {
                positive code = scenario == 29 ? 13 : scenario;
                for (positive at = 0; at < 3; at++, code /= 3)
                {
                        struct edit_place from = {0, min(edit_lines[0].length, 1)};
                        struct edit_place to = from;
                        string_address text = (string_address)"w";
                        positive length = 1;
                        if (code % 3 == 1)
                                text = (string_address)"\n";
                        else if (code % 3 == 2)
                        {
                                from.column = edit_lines[0].length;
                                if (edit_line_count > 1)
                                        to = (struct edit_place){1, min(edit_lines[1].length, 1)};
                                else
                                {
                                        from.column = 0;
                                        to.column = min(edit_lines[0].length, 1);
                                }
                                text = (string_address)"J";
                        }
                        edit_change(from, to, text, length, EDIT_STEP_OTHER);
                }
        }
        if (scenario == 30)
                edit_change(((struct edit_place){0, 0}), edit_place_last(),
                            (string_address)"z", 1, EDIT_STEP_OTHER);
        edit_settle(0);
        edit_step_seal();
        before->steps = edit_step_count;
        return edit_step_count == 1 && !edit_steps[0].open && edit_test_state_take(after);
}

static positive check_step_atomicity()
{
        positive checks = 0;
        for (positive scenario = 0; scenario < 31; scenario++)
        for (positive backward = 0; backward < 2; backward++)
        for (positive persistent = 0; persistent < 2; persistent++)
        {
                positive requests = 0;
                for (positive fail_at = 0; fail_at <= requests + 1; fail_at++)
                {
                        struct edit_test_state before, after;
                        edit_test_failure = 0;
                        edit_test_persistent = false;
                        if (!edit_test_history(scenario, &before, &after)) return 0;
                        if (!backward && (!edit_step_move(true) || !edit_test_state_same(&before))) return 0;
                        struct edit_step history = edit_steps[0];
                        struct edit_patch patches[8];
                        p8 payload[1024];
                        positive used = 0;
                        if (history.patch_count > array_count(patches)) return 0;
                        memory_copy_apart(patches, history.patches, history.patch_count * sizeof(*patches));
                        for (positive at = 0; at < history.patch_count; at++)
                        {
                                struct edit_patch *patch = patches + at;
                                if (patch->removed_length + patch->inserted_length > sizeof(payload) - used) return 0;
                                memory_copy_apart(payload + used, patch->removed, patch->removed_length);
                                used += patch->removed_length;
                                memory_copy_apart(payload + used, patch->inserted, patch->inserted_length);
                                used += patch->inserted_length;
                        }
                        edit_test_requests = 0;
                        edit_test_failure = fail_at;
                        edit_test_persistent = persistent;
                        bool moved = edit_step_move(backward);
                        if (!fail_at) requests = edit_test_requests;
                        bool no_rollback_allocations = moved || edit_test_requests == fail_at;
                        edit_test_failure = 0;
                        edit_test_persistent = false;
                        bool valid = no_rollback_allocations &&
                            !memory_compare(edit_steps, &history, sizeof(history)) &&
                            !memory_compare(history.patches, patches, history.patch_count * sizeof(*patches));
                        used = 0;
                        for (positive at = 0; at < history.patch_count && valid; at++)
                        {
                                struct edit_patch *patch = patches + at;
                                valid = !memory_compare(payload + used, patch->removed, patch->removed_length);
                                used += patch->removed_length;
                                valid = valid && !memory_compare(payload + used, patch->inserted, patch->inserted_length);
                                used += patch->inserted_length;
                        }
                        if (!moved)
                                valid = valid && edit_test_state_same(backward ? &after : &before) &&
                                    edit_step_move(backward);
                        valid = valid && edit_test_state_same(backward ? &before : &after) &&
                            edit_step_move(!backward) && edit_test_state_same(backward ? &after : &before);
                        if (!valid)
                        {
                                say_number(scenario * 10000 + backward * 1000 + persistent * 100 + fail_at);
                                say_byte(':');
                                return 0;
                        }
                        checks++;
                }
        }
        for (positive persistent = 0; persistent < 2; persistent++)
        for (positive reserve = 0; reserve < 2; reserve++)
        {
                struct edit_test_state before, after;
                if (!edit_test_history(30, &before, &after) ||
                    edit_cursor_count != 1 || edit_steps[0].before_count != 2) return 0;
                if (reserve)
                {
                        // A valid smaller current store forces the restore
                        // reserve, without falsifying its actual capacity.
                        struct edit_cursor *single = memory_take(sizeof(*single));
                        if (!single) return 0;
                        *single = edit_cursors[0];
                        memory_give(edit_cursors);
                        edit_cursors = single;
                        edit_cursor_room = sizeof(*single);
                }
                else
                {
                        // The state left by an after-snapshot OOM while a
                        // step remains open must be sealed before undo.
                        memory_give(edit_steps[0].after);
                        edit_steps[0].after = null;
                        edit_steps[0].after_count = 0;
                        edit_steps[0].open = true;
                }
                edit_test_requests = 0;
                edit_test_failure = 1;
                edit_test_persistent = persistent;
                bool moved = edit_step_move(true);
                edit_test_failure = 0;
                edit_test_persistent = false;
                if (moved || edit_test_requests != 1 || !edit_test_state_same(&after) ||
                    (!reserve && (!edit_steps[0].open || edit_steps[0].after || edit_steps[0].after_count)) ||
                    !edit_step_move(true) || !edit_test_state_same(&before) ||
                    !edit_step_move(false) || !edit_test_state_same(&after)) return 0;
                checks++;
        }
        for (positive persistent = 0; persistent < 2; persistent++)
        for (positive erasing = 0; erasing < 2; erasing++)
        {
                struct edit_test_state before, first, after;
                if (!edit_load((string_address)"ab", 2)) return 0;
                edit_place_cursor(0, 1, false);
                if (!edit_test_state_take(&before)) return 0;
                edit_insert((string_address)"X", 1, EDIT_STEP_OTHER);
                if (!edit_test_state_take(&first)) return 0;
                memory_give(edit_steps[0].after);
                edit_steps[0].after = null;
                edit_steps[0].after_count = 0;
                edit_steps[0].open = true;
                edit_test_requests = 0;
                edit_test_failure = 1;
                edit_test_persistent = persistent;
                if (erasing) edit_delete_character(false);
                else edit_insert((string_address)"Y", 1, EDIT_STEP_TYPING);
                edit_test_failure = 0;
                edit_test_persistent = false;
                if (!edit_test_state_same(&first) || !edit_steps[0].open) return 0;
                if (erasing) edit_delete_character(false);
                else edit_insert((string_address)"Y", 1, EDIT_STEP_TYPING);
                if (!edit_test_state_take(&after) || after.steps != 2) return 0;
                before.steps = first.steps = 2;
                if (!edit_step_move(true) || !edit_test_state_same(&first) ||
                    !edit_step_move(true) || !edit_test_state_same(&before) ||
                    !edit_step_move(false) || !edit_test_state_same(&first) ||
                    !edit_step_move(false) || !edit_test_state_same(&after)) return 0;
                checks++;
        }
        return checks;
}

static positive check_line_atomicity()
{
        static string_address expected[] = {
            "ab\nef\ncd", "cd\nab\nef", "ab\ncd\ncd\nef", "ab\ncd\ncd\nef"};
        positive checks = 0;
        for (positive mode = 0; mode < 4; mode++)
        {
                positive requests = 0;
                for (positive fail_at = 0; fail_at <= requests; fail_at++)
                {
                        edit_test_failure = 0;
                        if (!edit_load("ab\ncd\nef", 8))
                                return 0;
                        edit_place_cursor(1, 1, false);
                        edit_test_requests = 0;
                        edit_test_failure = fail_at;
                        if (mode & 2)
                                edit_copy_lines(mode & 1);
                        else
                                edit_move_lines(mode & 1);
                        if (!fail_at)
                                requests = edit_test_requests;
                        edit_test_failure = 0;
                        positive length;
                        p8 address_to bytes = edit_bytes_take(address_of length);
                        if (!bytes)
                                return 0;
                        bool unchanged = length == 8 && !memory_compare(bytes, "ab\ncd\nef", 8);
                        bool changed = length == string_length(expected[mode]) &&
                            !memory_compare(bytes, expected[mode], length);
                        memory_give(bytes);
                        positive line = unchanged ? 1 : mode & 1 ? (mode & 2 ? 1 : 0) : 2;
                        if ((!unchanged && !changed) || edit_cursor_count != 1 ||
                            edit_cursors[0].line != line || edit_cursors[0].column != 1)
                        {
                                say_number(mode * 100 + fail_at);
                                say_byte(':');
                                return 0;
                        }
                        if (changed)
                        {
                                if (!edit_step_move(true) || edit_cursors[0].line != 1 ||
                                    edit_cursors[0].column != 1 || !edit_step_move(false) ||
                                    edit_cursors[0].line != line || edit_cursors[0].column != 1)
                                {
                                        say_number(mode * 100 + fail_at);
                                        say_byte('!');
                                        return 0;
                                }
                        }
                        checks++;
                }
        }
        return checks;
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

        terminal_fixture_start(columns, rows);

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
                else if (string_compare(verb, (string_address) "reset") == 0)
                        edit_input_reset();
                else if (string_compare(verb, (string_address) "alt_pending") == 0)
                        say_number(edit_input_alt);
                else if (string_compare(verb, (string_address) "draw") == 0)
                {
                        edit_draw();
                        settle();
                }
                else if (string_compare(verb,
                                        (string_address) "statusfull") == 0)
                {
                        memory_fill(hostile_path, 0x80,
                                    sizeof(hostile_path) - 1);
                        hostile_path[sizeof(hostile_path) - 1] = 0;
                        edit_path = hostile_path;
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
                        edit_draw();
                        settle();
                        say_attribute(argument);
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
                else if (string_compare(verb, (string_address) "render_reuse") == 0)
                {
                        say_number(check_render_reuse());
                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "word_reuse") == 0)
                {
                        say_number(check_word_reuse());
                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "compaction") == 0)
                {
                        say_number(check_cursor_compaction());
                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "line_atomicity") == 0)
                {
                        say_number(check_line_atomicity() != 0);
                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "line_subsets") == 0)
                {
                        say_number(check_line_subsets());
                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "line_ranges") == 0)
                {
                        say_number(check_line_ranges());
                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "multi_journal") == 0)
                {
                        say_number(check_multicursor_journal() != 0);
                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "step_atomicity") == 0)
                {
                        say_number(check_step_atomicity() != 0);
                        say_byte('\n');
                }
                else if (string_compare(verb, (string_address) "step_atomicity_count") == 0)
                {
                        say_number(check_step_atomicity());
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

edit() { ${TEST_RUNNER:-} "$work/edit" "$@" 2>&1 | tr '\n' '|' | sed 's/|$//'; }

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

group whole_key
same 'multicursor move undo' 'a|b|c|d' 40 8 text 'a\nb\nc\nd' keys '<down>' add 3,0 keys '<a-up>^z' buffer
same 'multicursor move redo' 'b|a|d|c' 40 8 text 'a\nb\nc\nd' keys '<down>' add 3,0 keys '<a-up>^z^y' buffer
same 'consecutive moves separate' 'b|a|d|c' 40 8 text 'a\nb\nc\nd' keys '<down>' add 3,0 keys '<a-up><a-up>^z' buffer
same 'multicursor copy undo' 'a|b|c' 40 8 text 'a\nb\nc' add 2,0 keys '<s-a-down>^z' buffer
same 'consecutive copies separate' 'a|a|b|c|c' 40 8 text 'a\nb\nc' add 2,0 keys '<s-a-down><s-a-down>^z' buffer
same 'multicursor tab undo' 'ab|cd' 40 8 text 'ab\ncd' add 1,0 keys '<tab>^z' buffer
same 'multicursor comment undo' 'ab|cd' 40 8 text 'ab\ncd' add 1,0 keys '\x1f^z' buffer
same 'multicursor cut undo' 'a|b|c|d' 40 8 text 'a\nb\nc\nd' add 2,0 keys '^x^z' buffer
same 'multicursor newline undo' 'ab|cd' 40 8 text 'ab\ncd' keys '<right>' add 1,1 keys '<enter>^z' buffer
same 'multicursor newline redo' 'a|b|c|d' 40 8 text 'ab\ncd' keys '<right>' add 1,1 keys '<enter>^z^y' buffer
same 'consecutive newlines separate' 'a|b|c|d' 40 8 text 'ab\ncd' keys '<right>' add 1,1 keys '<enter><enter>^z' buffer
same 'word delete whole key' 'one two|one two' 40 8 text 'one two\none two' keys '<end>' add 1,7 keys '<c-bs>^z' buffer
same 'word delete consecutive' 'one |one ' 40 8 text 'one two\none two' keys '<end>' add 1,7 keys '<c-bs><c-bs>^z' buffer
same 'word selection consecutive' 'a' 40 8 text 'ab' keys '<end><s-left><c-bs><c-bs>^z' buffer
same 'typing before enter separate' 'x' 40 8 keys 'x<enter>^z' buffer

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

group line_positions
same 'all selected row permutations' '1482' 40 8 line_subsets
same 'overlapping range permutations' '77856' 40 8 line_ranges
same 'allocation transition matrix' '1' 40 8 line_atomicity
same 'multicursor journal failures' '1' 40 8 multi_journal
same 'undo redo allocation matrix' '1' 40 8 step_atomicity
for column in '' '<right>' '<end>'; do
        case $column in '') offset=0 ;; '<right>') offset=1 ;; *) offset=2 ;; esac
        same "move up column $offset" "0,$offset" 40 8 text 'ab\ncd\nef' \
                keys "<down>$column<a-up>" carets
        same "move down column $offset" "1,$offset" 40 8 text 'ab\ncd\nef' \
                keys "$column<a-down>" carets
        same "copy up column $offset" "0,$offset" 40 8 text 'ab\ncd\nef' \
                keys "$column<s-a-up>" carets
        same "copy down column $offset" "1,$offset" 40 8 text 'ab\ncd\nef' \
                keys "$column<s-a-down>" carets
        same "move roundtrip $offset" "0,$offset" 40 8 text 'ab\ncd\nef' \
                keys "$column<a-down><a-up>" carets
done
same 'selection down boundary' '2,0-1,0' 40 8 text 'ab\ncd\nef' keys '<s-down><a-down>' carets
same 'selection up boundary' '1,0-0,0' 40 8 text 'ab\ncd\nef' keys '<down><s-down><a-up>' carets
same 'reversed boundary' '1,0-2,0' 40 8 text 'ab\ncd\nef' keys '<down><s-up><a-down>' carets
same 'duplicate up boundary' '1,0-0,0' 40 8 text 'ab\ncd\nef' keys '<s-down><s-a-up>' carets
same 'duplicate down boundary' '2,0-1,0' 40 8 text 'ab\ncd\nef' keys '<s-down><s-a-down>' carets
same 'two columns move once' '1,1 1,2' 40 8 text 'abc\ndef\nghi' keys '<right>' add 0,2 keys '<a-down>' carets
same 'adjacent lines move up' 'def|ghi|abc' 40 8 text 'abc\ndef\nghi' keys '<down>' add 2,0 keys '<a-up>' buffer
same 'adjacent lines move down' 'ghi|abc|def' 40 8 text 'abc\ndef\nghi' add 1,0 keys '<a-down>' buffer
same 'whole selection blocks overlap' 'ab|cd' 40 8 text 'ab\ncd' keys '^a' add 1,0 keys '<a-up>' buffer
same 'adjacent copies remain independent' 'ab|ab|cd|cd' 40 8 text 'ab\ncd' add 1,0 keys '<s-a-down>' buffer
same 'overlapping copies duplicate once' 'ab|cd|ab|cd' 40 8 text 'ab\ncd' keys '^a' add 1,0 keys '<s-a-down>' buffer
same 'selection undo positions' '1,0-0,0' 40 8 text 'ab\ncd\nef' keys '<s-down><a-down>^z' carets
same 'selection redo positions' '2,0-1,0' 40 8 text 'ab\ncd\nef' keys '<s-down><a-down>^z^y' carets

#
#       Comments.
#

section comments

group toggle
same 'put on'          '# ab'           40 6 text 'ab' keys '\x1f' buffer
same 'taken off'       'ab'             40 6 text 'ab' keys '\x1f\x1f' buffer
same 'a block'         '# ab|# cd'      40 6 text 'ab\ncd' keys '^a\x1f' buffer
same 'adjacent blocks independent' 'a|# b' 40 8 text '# a\nb' add 1,0 keys '\x1f' buffer
same 'overlap covers every row' '# ab|# cd' 40 8 text 'ab\ncd' keys '<c-end><c-s-home>' add 0,2 keys '\x1f' buffer
same 'shallowest block indent' '  #   ab|  # cd' 40 8 text '    ab\n  cd' keys '^a\x1f' buffer
same 'empty first anchors left' '|#   ab' 40 8 text '\n  ab' keys '^a\x1f' buffer
same 'bare marker removed' 'ab|  cd' 40 8 text '#ab\n  # cd' keys '^a\x1f' buffer

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

group compaction
same 'stable generated coalescing' '1152' 40 6 compaction

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
same 'two byte minimum' '[1 <0080>]'    40 6 keys '\xc2\x80' row 0
same 'three byte minimum' '[1 <0800>]'  40 6 keys '\xe0\xa0\x80' row 0
same 'four byte minimum' '[1 <10000>]'  40 6 keys '\xf0\x90\x80\x80' row 0
same 'six hex digits'  '[1 <10ffff>]'   40 6 keys '\xf4\x8f\xbf\xbf' row 0
same 'one caret step'  '0,2'            40 6 keys '\xc3\xa9' carets
same 'backspace whole' ''               40 6 keys '\xc3\xa9<bs>' buffer
same 'overlong ignored' ''               40 6 keys '\xc0\xaf' buffer
same 'surrogate ignored' ''              40 6 keys '\xed\xa0\x80' buffer
same 'alt two bytes'   '0éx'             40 6 keys '\e\xc3\xa9x' alt_pending buffer
same 'alt three bytes' '0界x'            40 6 keys '\e\xe7\x95\x8cx' alt_pending buffer
same 'alt four bytes'  '00,5'            40 6 keys '\e\xf0\x90\x80\x80x' alt_pending carets
same 'alt pending two' '1'               40 6 keys '\e\xc3' alt_pending
same 'alt pending three' '1'             40 6 keys '\e\xe7\x95' alt_pending
same 'alt pending four' '1'              40 6 keys '\e\xf0\x90\x80' alt_pending
same 'alt interrupted' 'x'               40 6 keys '\e\xc3x' buffer
same 'alt malformed'   'x'               40 6 keys '\e\xc0\xafx' buffer
same 'alt idle reset'  'x'               40 6 keys '\e\xc3' idle keys 'x' buffer
same 'out of range ignored' ''           40 6 keys '\xf4\x90\x80\x80' buffer
same 'csi u surrogate ignored' ''         40 6 keys '\e[55296u\e[57343u' buffer
same 'csi u scalar boundaries' '[1 <d7ff><e000><10ffff>]' 40 6 keys '\e[55295u\e[57344u\e[1114111u' row 0

group paste
same 'keeps newlines exact' 'a|  b'      40 6 keys '\e[200~a\n  b\e[201~' buffer
same 'is one undo step' ''               40 6 keys '\e[200~a\n  b\e[201~^z' buffer
same 'multicursor paste undo' 'ab|cd'      40 8 text 'ab\ncd' add 1,0 keys '\e[200~X\e[201~^z' buffer
same 'multicursor paste redo' 'Xab|Xcd'    40 8 text 'ab\ncd' add 1,0 keys '\e[200~X\e[201~^z^y' buffer
same 'successive pastes separate' 'Xab|Xcd' 40 8 text 'ab\ncd' add 1,0 keys '\e[200~X\e[201~\e[200~Y\e[201~^z' buffer
same 'typing before paste separate' 'x'   40 8 keys 'x\e[200~Y\e[201~^z' buffer
same 'reset drops partial frame' 'x'     40 6 keys '\e[200~abc' reset keys 'x' buffer

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

group bounds
same 'full status returns' ''             40 6 statusfull buffer
same 'shared renderer variance' '7808'    40 6 render_reuse
same 'shared word variance' '168960'      40 6 word_reuse

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
same 'only newline'    '|'               40 6 text '\n' bytes
same 'edited empty has newline' '|'      40 6 keys 'x<bs>' bytes
same 'empty file'      ''               40 6 bytes
same 'undo restores empty bytes' ''      40 6 keys 'x^z' bytes
same 'redo undo restores empty bytes' '' 40 6 keys 'x^z^y^z' bytes


section

printf '  %-12s %s passed, %s failed\n' 'edit' "$pass" "$fail"

[ "$fail" -eq 0 ]
