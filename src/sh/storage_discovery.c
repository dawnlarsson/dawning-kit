/*
        The read-only half of the common Linux storage interface.

        mountinfo is the authority for what this process can see.  fstab is
        only policy for what it may choose to mount.  Keeping their parsers
        here gives mount, umount, findmnt, mountpoint and findfs one spelling
        of both files instead of six subtly different ones.

        The input and entry tables grow through memory_reserve, whose copying,
        growth and allocation floor is in library.c assembly on every target.
        Parsing begins only after the input has stopped moving; entry strings
        are therefore zero-copy views into the owned input block.  There is no
        line, field, path or entry ceiling.
*/

#define STORAGE_OPEN_PATH    010000000
#define STORAGE_OPEN_NOFOLLOW 0400000

/*
        Shared API (the definitions live together because shell sources are
        one translation unit):

          storage_mount_table_load / storage_mount_table_release
          storage_mount_find_target
          storage_fstab_table_load / storage_fstab_table_release

        A successful load owns both table blocks until release.  Callers may
        iterate entry[0..count), and every string remains valid until then.
*/
typedef struct
{
        string_address source;
        string_address target;
        string_address type;
        string_address options;
        string_address dump;
        string_address pass;
        bool defaults;
        bool noauto;
} storage_fstab;

typedef struct
{
        byte_store text;
        storage_fstab address_to entry;
        positive entry_room;
        positive count;
        bool malformed;
} storage_fstab_table;

static fn storage_write_text(writer output, string_address text)
{
        if (output && text)
                output((address_any)text, string_length(text));
}

#define STORAGE_TABLE_RELEASE(name, type)                                    \
        fn name(type address_to table)                                       \
        {                                                                    \
                byte_store_release(address_of table->text);                  \
                array_store_release(table->entry, table->entry_room,         \
                                    table->count);                            \
        }

STORAGE_TABLE_RELEASE(storage_mount_table_release, storage_mount_table)
STORAGE_TABLE_RELEASE(storage_fstab_table_release, storage_fstab_table)
#undef STORAGE_TABLE_RELEASE

static bool storage_octal(p8 address_to at, p8 address_to value)
{
        positive used = 0;
        positive decoded = string_digits_octal_max(at, 3, address_of used);

        if (used != 3)
                return false;

        address_to value = (p8)decoded;
        return true;
}

/* One decoding step shared by the mountinfo and fstab readers: a complete
   backslash-octal escape becomes its byte, anything else copies through. */
static inline INLINE fn storage_unescape_step(p8 address_to address_to read,
                                              p8 address_to address_to write)
{
        p8 address_to at = address_to read;
        p8 address_to out = address_to write;
        p8 value;

        if (*at == '\\' && storage_octal(at + 1, address_of value))
        {
                *out++ = value;
                at += 4;
        }
        else
                *out++ = *at++;

        address_to read = at;
        address_to write = out;
}

/* mountinfo and fstab both use backslash-octal.  Decode every valid escape,
   including \134 itself; limiting this to \040 is how names with tabs or
   backslashes become impossible to round-trip. */
static fn storage_unescape(string_address field)
{
        p8 address_to read = (p8 address_to)string_first_of(field, '\\');
        p8 address_to write;

        /* Most mountinfo fields contain no escape at all.  The assembly scan
           lets those fields remain zero-copy instead of rewriting every byte
           just to discover that there was nothing to decode. */
        if (!read)
                return;

        write = read;

        while (*read)
                storage_unescape_step(address_of read, address_of write);

        *write = end;
}

/* Both kernel tables are newline records over the same owned byte store.
   Return one mutable, terminated record and advance the caller's cursor. */
static inline INLINE p8 address_to storage_line_next(
    p8 address_to address_to cursor, p8 address_to limit)
{
        p8 address_to line = address_to cursor;
        p8 address_to newline;

        if (line >= limit)
                return null;

        newline = (p8 address_to)memory_first_of(
            line, '\n', (positive)(limit - line));
        address_to cursor = newline ? newline + 1 : limit;

        if (newline)
                address_to newline = end;

        return line;
}

/* Return one whitespace-delimited field and terminate it in place. */
static string_address storage_field(p8 address_to address_to cursor)
{
        p8 address_to at = *cursor;
        p8 address_to answer;

        at += string_span_of_set(at, " \t");

        if (!*at)
        {
                address_to cursor = at;
                return null;
        }

        answer = at;
        at += string_span_without_set(at, " \t");

        if (*at)
                *at++ = end;

        address_to cursor = at;
        return answer;
}

/* Parse one line after its newline has already become NUL. */
static bool storage_mount_line(storage_mount_table address_to table,
                               p8 address_to line)
{
        string_address first[6];
        string_address separator;
        string_address type;
        string_address source;
        string_address filesystem_options;
        p8 address_to cursor = line;
        positive id;
        positive parent;

        for (positive at = 0; at < 6; at++)
        {
                first[at] = storage_field(address_of cursor);

                if (!first[at])
                        return false;
        }

        do
        {
                separator = storage_field(address_of cursor);

                if (!separator)
                        return false;
        }
        while (string_compare(separator, (string_address) "-"));

        type = storage_field(address_of cursor);
        source = storage_field(address_of cursor);
        filesystem_options = storage_field(address_of cursor);

        if (!type || !source || !filesystem_options ||
            !string_digits_exact(first[0], address_of id) ||
            !string_digits_exact(first[1], address_of parent))
                return false;

        if (!array_store_reserve(table->entry, table->entry_room,
                                 table->count, table->count + 1, 32))
                return false;

        storage_unescape(first[3]);
        storage_unescape(first[4]);
        storage_unescape(first[5]);
        storage_unescape(type);
        storage_unescape(source);
        storage_unescape(filesystem_options);

        table->entry[table->count++] = (storage_mount){
            .id = id,
            .parent_id = parent,
            .device = first[2],
            .root = first[3],
            .target = first[4],
            .options = first[5],
            .type = type,
            .source = source,
            .filesystem_options = filesystem_options,
        };

        return true;
}

bool storage_mount_table_load(storage_mount_table address_to table,
                              writer diagnostic)
{
        memory_fill(table, 0, sizeof(*table));

        if (!file_store_slurp((string_address) "/proc/self/mountinfo",
                              address_of table->text))
        {
                storage_write_text(diagnostic,
                                   (string_address) "cannot read /proc/self/mountinfo\n");
                return false;
        }

        p8 address_to cursor = table->text.bytes;
        p8 address_to limit = cursor + table->text.used;
        p8 address_to line;

        while ((line = storage_line_next(address_of cursor, limit)))
        {
                if (*line && !storage_mount_line(table, line))
                {
                        storage_write_text(diagnostic,
                                           (string_address) "invalid /proc/self/mountinfo line\n");
                        storage_mount_table_release(table);
                        return false;
                }
        }

        return true;
}

static string_address storage_comma_next(string_address address_to cursor,
                                         positive address_to length)
{
        string_address at = address_to cursor;
        string_address comma;

        if (!at || !*at)
                return null;

        comma = string_first_of_or_end(at, ',');
        address_to length = (positive)(comma - at);
        address_to cursor = *comma ? comma + 1 : null;
        return at;
}

static PURE bool storage_option_has_length(string_address options,
                                      string_address wanted,
                                      positive wanted_length)
{
        string_address cursor = options;
        string_address at;
        positive length;

        while ((at = storage_comma_next(address_of cursor, address_of length)))
        {
                if (length == wanted_length &&
                    !string_compare_max(at, wanted, length))
                        return true;
        }

        return false;
}

static PURE bool storage_option_has(string_address options, string_address wanted)
{
        return storage_option_has_length(options, wanted,
                                         string_length(wanted));
}

/* fstab permits blank/comment lines and comments after fields.  Quotes are
   ordinary bytes in this grammar, just as they are in util-linux; \040 and
   \011 are the portable way to carry whitespace.  Treating quotes as shell
   syntax silently turns an invalid distro fstab into a different mount. */
static positive storage_fstab_fields(p8 address_to line,
                                     string_address address_to field,
                                     positive room)
{
        p8 address_to read = line;
        p8 address_to write = line;
        positive count = 0;

        while (*read)
        {
                read += string_span_of_set(read, " \t");

                if (!*read || *read == '#')
                        break;

                if (count == room)
                        return count + 1;

                field[count++] = write;

                while (*read && *read != ' ' && *read != '\t')
                        storage_unescape_step(address_of read,
                                              address_of write);

                /* Advance the reader before terminating the compacted field:
                   with no escapes read == write at the delimiter. */
                read += string_span_of_set(read, " \t");

                *write++ = end;
        }

        return count;
}

bool storage_fstab_table_load(storage_fstab_table address_to table,
                              string_address path, bool missing_ok,
                              writer diagnostic)
{
        memory_fill(table, 0, sizeof(*table));

        if (!file_store_slurp(path, address_of table->text))
        {
                if (!missing_ok)
                {
                        storage_write_text(diagnostic,
                                           (string_address) "cannot read ");
                        storage_write_text(diagnostic, path);
                        storage_write_text(diagnostic, (string_address) "\n");
                }

                return missing_ok;
        }

        p8 address_to cursor = table->text.bytes;
        p8 address_to limit = cursor + table->text.used;
        p8 address_to line;
        positive line_number = 0;

        while ((line = storage_line_next(address_of cursor, limit)))
        {
                string_address fields[7];
                positive count;

                line_number++;

                count = storage_fstab_fields(line, fields, 7);

                if (count)
                {
                        if (count < 4 || count > 6)
                        {
                                if (diagnostic)
                                {
                                        storage_write_text(diagnostic, path);
                                        storage_write_text(diagnostic,
                                            (string_address) ": parse error at line ");
                                        positive_to_string(diagnostic, line_number);
                                        storage_write_text(diagnostic,
                                            (string_address) " -- ignored\n");
                                }
                                table->malformed = true;
                                continue;
                        }

                        if (!array_store_reserve(
                                table->entry, table->entry_room, table->count,
                                table->count + 1, 32))
                        {
                                storage_fstab_table_release(table);
                                return false;
                        }

                        table->entry[table->count++] = (storage_fstab){
                            .source = fields[0],
                            .target = fields[1],
                            .type = fields[2],
                            .options = fields[3],
                            .dump = count > 4 ? fields[4] : (string_address) "0",
                            .pass = count > 5 ? fields[5] : (string_address) "0",
                            .defaults = storage_option_has(fields[3],
                                                           (string_address) "defaults"),
                            .noauto = storage_option_has(fields[3],
                                                         (string_address) "noauto"),
                        };
                }
        }

        return true;
}

PURE storage_mount address_to storage_mount_find_target(
    storage_mount_table address_to table, string_address target)
{
        /* The last record is the visible top of a stacked mount. */
        for (positive at = table->count; at; at--)
                if (!string_compare(table->entry[at - 1].target, target))
                        return table->entry + at - 1;

        return null;
}

/* libmount's match_fstype: one leading `no` inverts the whole list, so
   `nofoo,bar` means `nofoo,nobar`; a token's own `no` prefix is read
   literally, and names compare without case. */
static PURE bool storage_type_match(string_address list, string_address type)
{
        bool negated = false;
        string_address cursor = list;
        string_address at;
        positive length;

        if (!list)
                return true;

        if (list[0] == 'n' && list[1] == 'o')
        {
                negated = true;
                cursor = list + 2;
        }

        while ((at = storage_comma_next(address_of cursor, address_of length)))
        {
                if (at[0] == 'n' && at[1] == 'o' &&
                    file_same_word(at + 2, length - 2, type))
                        return false;
                if (file_same_word(at, length, type))
                        return !negated;
        }

        return negated;
}

/* One column schema drives option parsing, headings and record projection.
   A zero offset names a computed numeric column; every textual member begins
   after the two ids, so zero is an unambiguous sentinel. */
#define STORAGE_COLUMNS(X)                                                   \
        X(SOURCE, "SOURCE", __builtin_offsetof(storage_mount, source))      \
        X(TARGET, "TARGET", __builtin_offsetof(storage_mount, target))      \
        X(FSTYPE, "FSTYPE", __builtin_offsetof(storage_mount, type))        \
        X(OPTIONS, "OPTIONS", __builtin_offsetof(storage_mount, options))   \
        X(FSROOT, "FSROOT", __builtin_offsetof(storage_mount, root))        \
        X(MAJMIN, "MAJ:MIN", __builtin_offsetof(storage_mount, device))     \
        X(ID, "ID", 0) X(PARENT, "PARENT", 0)                             \
        X(VFS_OPTIONS, "VFS-OPTIONS",                                      \
          __builtin_offsetof(storage_mount, options))                        \
        X(FS_OPTIONS, "FS-OPTIONS",                                        \
          __builtin_offsetof(storage_mount, filesystem_options))

#define STORAGE_COLUMN_ENUM(symbol, name, offset) STORAGE_##symbol,
enum storage_column
{
        STORAGE_COLUMNS(STORAGE_COLUMN_ENUM)
        STORAGE_COLUMN_MAX
};
#undef STORAGE_COLUMN_ENUM

typedef struct
{
        string_address name;
        p16 offset;
} storage_column_descriptor;

#define STORAGE_COLUMN_ENTRY(symbol, text, member_offset)                   \
        [STORAGE_##symbol] = {(string_address)text, (p16)(member_offset)},
static const storage_column_descriptor storage_column_table[] = {
    STORAGE_COLUMNS(STORAGE_COLUMN_ENTRY)};
#undef STORAGE_COLUMN_ENTRY
#undef STORAGE_COLUMNS

typedef struct
{
        enum storage_column columns[STORAGE_COLUMN_MAX];
        positive count;
        string_address operand;
        string_address source;
        string_address target;
        string_address type;
        string_address option_filter;
        bool path_query;
        bool mountpoint_query;
        bool no_headings;
        bool raw;
        bool no_fsroot;
        bool pairs;
        bool first_only;
        bool invert;
} storage_findmnt_options;

static PURE bool storage_rw_opposite(string_address options,
                                     string_address option, positive length)
{
        return length == 2 &&
            ((memory_is_2(option, 'r', 'w') &&
              storage_option_has_length(options, (string_address)"ro", 2)) ||
             (memory_is_2(option, 'r', 'o') &&
              storage_option_has_length(options, (string_address)"rw", 2)));
}

/* The filesystem column wins when its ro/rw state overrides the VFS column.
   Both a literal option and the positive half of a requested no-option ask
   this same question. */
static PURE bool storage_mount_option_present(storage_mount address_to mount,
                                               string_address option,
                                               positive length)
{
        bool present = storage_option_has_length(mount->options, option, length) ||
            storage_option_has_length(mount->filesystem_options, option, length);

        return present && !storage_rw_opposite(mount->filesystem_options,
                                               option, length);
}

static PURE bool storage_mount_options_match(storage_mount address_to mount,
                                        string_address list)
{
        string_address cursor = list;
        string_address at;
        positive length;

        if (!list)
                return true;

        while ((at = storage_comma_next(address_of cursor, address_of length)))
        {
                if (!length)
                        return false;

                bool present = storage_mount_option_present(mount, at, length);

                /* libmount first honours a literal `no...` option.  When
                   there is none, it treats the spelling as a request that
                   the positive option be absent: norw matches a read-only
                   mount, while nodev still matches the actual nodev flag. */
                if (!present && length > 2 && at[0] == 'n' && at[1] == 'o')
                        present = !storage_mount_option_present(
                            mount, at + 2, length - 2);

                if (!present)
                        return false;
        }

        return true;
}

static bool storage_columns(string_address list,
                            storage_findmnt_options address_to options)
{
        string_address cursor = list;
        string_address at;
        positive length;

        options->count = 0;

        while ((at = storage_comma_next(address_of cursor, address_of length)))
        {
                positive column;

                for (column = 0; column < STORAGE_COLUMN_MAX; column++)
                {
                        const storage_column_descriptor address_to descriptor =
                            storage_column_table + column;

                        if (file_same_word(at, length, descriptor->name))
                                break;
                }

                if (column == STORAGE_COLUMN_MAX)
                        return false;

                if (options->count == STORAGE_COLUMN_MAX)
                        return false;

                options->columns[options->count++] =
                    (enum storage_column)column;
        }

        return options->count != 0;
}

#define storage_column_name(column) storage_column_table[(column)].name

static fn storage_findmnt_value(writer output, string_address value, bool raw)
{
        if (!raw)
        {
                storage_write_text(output, value);
                return;
        }

        storage_write_hex_escaped(output, value, true, false);
}

static string_address storage_findmnt_cell(storage_mount address_to mount,
                                           enum storage_column column,
                                           bool heading)
{
        if (heading)
                return storage_column_name(column);

        p16 offset = storage_column_table[column].offset;

        return offset ? memory_load_unaligned(
                            string_address, (p8 address_to)mount + offset)
                      : null;
}

static PURE inline INLINE bool storage_filesystem_option_represented(
    storage_mount address_to mount, string_address option, positive length)
{
        return storage_option_has_length(mount->options, option, length) ||
            storage_rw_opposite(mount->options, option, length);
}

static PURE positive storage_combined_options_length(storage_mount address_to mount)
{
        positive length = 0;

        for (positive filesystem = 0; filesystem < 2; filesystem++)
        {
                string_address cursor = filesystem ? mount->filesystem_options
                                                   : mount->options;
                string_address at;
                positive token_length;

                while ((at = storage_comma_next(address_of cursor,
                                                 address_of token_length)))
                        if (token_length &&
                            (!filesystem ||
                             !storage_filesystem_option_represented(
                                 mount, at, token_length)))
                                length += token_length + (length ? 1 : 0);
        }

        return length;
}

static fn storage_findmnt_option_write(writer output, string_address at,
                                       positive length, string_address shown,
                                       bool raw, bool pairs)
{
        if (!raw && !pairs)
        {
                output((address_any)shown, length);
                return;
        }

        p8 saved = at[length];
        bool borrowed = shown == at;

        if (borrowed)
                at[length] = end;
        if (pairs)
                storage_write_encoded(output, shown);
        else
                storage_findmnt_value(output, shown, true);
        if (borrowed)
                at[length] = saved;
}

static fn storage_combined_options_write(writer output,
                                         storage_mount address_to mount,
                                         bool raw, bool pairs)
{
        bool any = false;

        for (positive filesystem = 0; filesystem < 2; filesystem++)
        {
                string_address cursor = filesystem ? mount->filesystem_options
                                                   : mount->options;
                string_address at;
                positive token_length;

                while ((at = storage_comma_next(address_of cursor,
                                                 address_of token_length)))
                {
                        if (!token_length ||
                            (filesystem &&
                             storage_filesystem_option_represented(
                                 mount, at, token_length)))
                                continue;

                        bool overridden = !filesystem && storage_rw_opposite(
                            mount->filesystem_options, at, token_length);
                        string_address shown = overridden
                            ? (memory_is_2(at, 'r', 'w')
                                   ? (string_address)"ro"
                                   : (string_address)"rw")
                            : at;

                        if (any)
                                output((address_any)",", 1);
                        storage_findmnt_option_write(
                            output, at, token_length, shown, raw, pairs);
                        any = true;
                }
        }
}

static PURE bool storage_source_has_root(storage_mount address_to mount)
{
        return mount->root && mount->root[0] &&
               !string_equals(mount->root, "/");
}

static PURE positive storage_findmnt_cell_length(storage_mount address_to mount,
                                            enum storage_column column,
                                            bool show_fsroot)
{
        positive length;

        if (column == STORAGE_ID)
                return positive_digits(mount->id);
        if (column == STORAGE_PARENT)
                return positive_digits(mount->parent_id);
        if (column == STORAGE_OPTIONS)
                return storage_combined_options_length(mount);

        length = string_length(storage_findmnt_cell(mount, column, false));

        if (column == STORAGE_SOURCE && show_fsroot &&
            storage_source_has_root(mount))
                length += string_length(mount->root) + 2;

        return length;
}

static bool storage_source_matches(storage_mount address_to mount,
                                   string_address wanted)
{
        positive source_length;
        positive root_length;

        if (!string_compare(wanted, mount->source))
                return true;
        if (!storage_source_has_root(mount))
                return false;

        source_length = string_length(mount->source);
        root_length = string_length(mount->root);
        return string_length(wanted) == source_length + root_length + 2 &&
               !memory_compare(wanted, mount->source, source_length) &&
               wanted[source_length] == '[' &&
               !memory_compare(wanted + source_length + 1,
                               mount->root, root_length) &&
               wanted[source_length + root_length + 1] == ']';
}

/* A cell pads to its column only between columns and never in raw mode;
   the last column stays ragged so no line carries trailing blanks. */
static fn storage_findmnt_pad(writer output,
                              storage_findmnt_options address_to options,
                              positive address_to widths, positive at,
                              positive length)
{
        if (!options->raw && at + 1 < options->count && widths[at] > length)
                writer_fill(output, widths[at] - length, ' ');
}

static fn storage_findmnt_row(writer output, storage_mount address_to mount,
                              storage_findmnt_options address_to options,
                              positive address_to widths, bool heading)
{
        if (!heading && options->pairs)
        {
                for (positive at = 0; at < options->count; at++)
                {
                        enum storage_column column = options->columns[at];
                        string_address value = storage_findmnt_cell(
                            mount, column, false);

                        if (at)
                                output((address_any)" ", 1);
                        storage_write_text(output, storage_column_name(column));
                        output((address_any)"=\"", 2);

                        if (column == STORAGE_ID || column == STORAGE_PARENT)
                                positive_to_string(output,
                                    column == STORAGE_ID ? mount->id
                                                         : mount->parent_id);
                        else if (column == STORAGE_SOURCE &&
                                 storage_source_has_root(mount) &&
                                 !options->no_fsroot)
                        {
                                storage_write_encoded(output, mount->source);
                                output((address_any)"[", 1);
                                storage_write_encoded(output, mount->root);
                                output((address_any)"]", 1);
                        }
                        else if (column == STORAGE_OPTIONS)
                                storage_combined_options_write(output, mount,
                                                               false, true);
                        else
                                storage_write_encoded(output, value);

                        output((address_any)"\"", 1);
                }

                output((address_any)"\n", 1);
                return;
        }

        for (positive at = 0; at < options->count; at++)
        {
                string_address value = storage_findmnt_cell(
                    mount, options->columns[at], heading);

                if (at)
                        output((address_any)" ", 1);

                if (!heading && (options->columns[at] == STORAGE_ID ||
                                 options->columns[at] == STORAGE_PARENT))
                {
                        positive value_number = options->columns[at] == STORAGE_ID
                                                    ? mount->id
                                                    : mount->parent_id;

                        positive_to_string(output, value_number);
                        storage_findmnt_pad(output, options, widths, at,
                                            positive_digits(value_number));
                }
                else if (!heading && options->columns[at] == STORAGE_OPTIONS)
                {
                        storage_combined_options_write(output, mount,
                                                       options->raw, false);
                        storage_findmnt_pad(
                            output, options, widths, at,
                            storage_combined_options_length(mount));
                }
                else if (!heading && options->columns[at] == STORAGE_SOURCE &&
                         storage_source_has_root(mount) && !options->no_fsroot)
                {
                        storage_findmnt_value(output, mount->source,
                                              options->raw);
                        output((address_any)"[", 1);
                        storage_findmnt_value(output, mount->root,
                                              options->raw);
                        output((address_any)"]", 1);
                        storage_findmnt_pad(
                            output, options, widths, at,
                            storage_findmnt_cell_length(mount, STORAGE_SOURCE,
                                                        true));
                }
                else if (!options->raw && at + 1 < options->count)
                        string_to_field(output, value, widths[at], ' ', true);
                else
                        storage_findmnt_value(output, value, options->raw);
        }

        output((address_any)"\n", 1);
}

static PURE bool storage_findmnt_match(storage_mount address_to mount,
                                  storage_findmnt_options address_to options,
                                  bool have_query_id, positive query_id)
{
        bool matched = true;

        if (options->source &&
            !storage_source_matches(mount, options->source))
                matched = false;

        if (matched && !storage_type_match(options->type, mount->type))
                matched = false;

        if (matched && !storage_mount_options_match(
                           mount, options->option_filter))
                matched = false;

        if (matched && have_query_id && mount->id != query_id)
                matched = false;

        if (matched && options->operand &&
            string_compare(options->operand, mount->target) &&
            !storage_source_matches(mount, options->operand))
                matched = false;

        if (matched && options->target && !options->path_query &&
            string_compare(options->target, mount->target))
                matched = false;

        if (options->invert &&
            (options->operand || options->source || options->target ||
             options->type || options->option_filter || have_query_id))
                return !matched;
        return matched;
}

/* Reentrant core used unchanged by builtin and multicall dispatch. */
b32 storage_findmnt(positive argc, string_address address_to argv,
                    writer output, writer diagnostic)
{
        static const storage_argument_name arguments[] = {
            STORAGE_ARGUMENT("noheadings", 'n'),
            STORAGE_ARGUMENT("raw", 'r'),
            STORAGE_ARGUMENT("list", 'l'),
            STORAGE_ARGUMENT("nofsroot", 'v'),
            STORAGE_ARGUMENT("pairs", 'P'),
            STORAGE_ARGUMENT("first-only", 'f'),
            STORAGE_ARGUMENT("invert", 'i'),
            STORAGE_ARGUMENT("source", 'S'),
            STORAGE_ARGUMENT("target", 'T'),
            STORAGE_ARGUMENT("mountpoint", 'M'),
            STORAGE_ARGUMENT("types", 't'),
            STORAGE_ARGUMENT("options", 'O'),
            STORAGE_ARGUMENT("output", 'o'),
        };
        storage_findmnt_options options = {
            .columns = {STORAGE_TARGET, STORAGE_SOURCE,
                        STORAGE_FSTYPE, STORAGE_OPTIONS},
            .count = 4,
        };
        storage_mount_table table;
        positive query_id = 0;
        bool have_query_id = false;
        positive widths[STORAGE_COLUMN_MAX] = {0};
        storage_arguments taking = {.argc = argc, .argv = argv, .at = 1};
        string_address value;
        b32 option;

        while ((option = storage_argument_next(
                    address_of taking, (string_address)"nrlvPfiSTMtoO",
                    (string_address)"STMtoO", arguments,
                    array_count(arguments), address_of value)) !=
               STORAGE_ARGUMENT_END)
        {
                if (option == STORAGE_ARGUMENT_OPERAND)
                {
                        if (options.operand)
                        {
                                storage_write_text(diagnostic,
                                    (string_address) "findmnt: too many arguments\n");
                                return 1;
                        }

                        options.operand = *value ? value : (string_address) "/";
                        continue;
                }

                if (option == STORAGE_ARGUMENT_MISSING)
                {
                        storage_write_text(diagnostic,
                            (string_address) "findmnt: option needs an argument\n");
                        return 1;
                }

                if (option == 'n')
                        options.no_headings = true;
                else if (option == 'r')
                        options.raw = true;
                else if (option == 'v')
                        options.no_fsroot = true;
                else if (option == 'l')
                        ; /* This implementation is already list-shaped. */
                else if (option == 'P')
                {
                        options.pairs = true;
                        options.no_headings = true;
                }
                else if (option == 'f')
                        options.first_only = true;
                else if (option == 'i')
                        options.invert = true;
                else if (option == 'S')
                        options.source = value;
                else if (option == 'T' || option == 'M')
                {
                        options.target = *value ? value : (string_address) "/";
                        if (option == 'T')
                                options.path_query = true;
                        else
                                options.mountpoint_query = true;
                }
                else if (option == 't')
                        options.type = value;
                else if (option == 'O')
                        options.option_filter = value;
                else if (option != 'o')
                {
                        storage_write_text(diagnostic,
                            (string_address) "findmnt: unsupported option\n");
                        return 1;
                }
                else if (!storage_columns(value, address_of options))
                {
                        storage_write_text(diagnostic,
                            (string_address) "findmnt: unsupported output column\n");
                        return 1;
                }
        }

        if ((options.path_query && options.mountpoint_query) ||
            (options.operand &&
             (options.source || options.path_query || options.mountpoint_query)))
        {
                storage_write_text(diagnostic,
                    (string_address) "findmnt: incompatible query arguments\n");
                return 1;
        }

        if (!storage_mount_table_load(address_of table, diagnostic))
                return 1;

        if (options.path_query)
        {
                file_facts facts;

                if (!file_look_at(options.target, address_of facts))
                {
                        storage_mount_table_release(address_of table);
                        return 1;
                }

                query_id = facts.mount_id;
                have_query_id = true;
        }

        positive matched = 0;
        bool direct = options.raw || options.pairs;

        for (positive at = 0; at < table.count; at++)
        {
                storage_mount address_to mount = table.entry + at;

                if (!storage_findmnt_match(mount, address_of options,
                                           have_query_id, query_id))
                        continue;

                matched++;

                if (direct)
                {
                        if (matched == 1 && !options.no_headings)
                                storage_findmnt_row(output, null,
                                                    address_of options,
                                                    widths, true);

                        storage_findmnt_row(output, mount, address_of options,
                                            widths, false);
                }
                else
                {
                        for (positive column = 0; column < options.count;
                             column++)
                        {
                                positive length = storage_findmnt_cell_length(
                                    mount, options.columns[column],
                                    !options.no_fsroot);

                                if (length > widths[column])
                                        widths[column] = length;
                        }
                }

                if (options.first_only)
                        break;
        }

        if (matched && !direct)
        {
                if (!options.no_headings)
                {
                        for (positive column = 0; column < options.count; column++)
                        {
                                positive length = string_length(
                                    storage_column_name(options.columns[column]));

                                if (length > widths[column])
                                        widths[column] = length;
                        }

                        storage_findmnt_row(output, null, address_of options,
                                            widths, true);
                }

                for (positive at = 0; at < table.count; at++)
                {
                        storage_mount address_to mount = table.entry + at;

                        if (!storage_findmnt_match(mount, address_of options,
                                                   have_query_id, query_id))
                                continue;

                        storage_findmnt_row(output, mount, address_of options,
                                            widths, false);

                        if (options.first_only)
                                break;
                }
        }

        storage_mount_table_release(address_of table);

        return matched ? 0 : 1;
}

/* readlink(/proc/self/fd/N) gives the followed, absolute spelling without a
   PATH_MAX buffer.  The link can grow between calls, so equality means retry. */
static p8 address_to storage_fd_path(bipolar handle, positive address_to room)
{
        p8 name[64];
        p8 number[32];
        byte_store path = {0};
        positive used = positive_into_string(number, (positive)handle);

        memory_copy_apart(name, "/proc/self/fd/", 14);
        memory_copy_apart(name + 14, number, used + 1);

        while (true)
        {
                positive want = path.room ? path.room * 2 : 256;

                if (want <= path.room ||
                    !byte_store_reserve(address_of path, want, 256))
                {
                        byte_store_release(address_of path);
                        address_to room = 0;
                        return null;
                }

                bipolar got = system_read_link_at(
                    AT_FDCWD, name, path.bytes, path.room - 1);

                if (got < 0)
                {
                        byte_store_release(address_of path);
                        address_to room = 0;
                        return null;
                }

                if ((positive)got < path.room - 1)
                {
                        path.bytes[got] = end;
                        address_to room = path.room;
                        return path.bytes;
                }
        }
}

/* Reentrant core used unchanged by builtin and multicall dispatch. */
static fn storage_device_number(writer output, positive major, positive minor)
{
        positive_to_string(output, major);
        output((address_any)":", 1);
        positive_to_string(output, minor);
        output((address_any)"\n", 1);
}

static fn storage_mountpoint_error(writer diagnostic, bool quiet,
                                   string_address path,
                                   string_address reason)
{
        if (quiet)
                return;

        storage_write_text(diagnostic, (string_address) "mountpoint: ");
        storage_write_text(diagnostic, path);
        storage_write_text(diagnostic, reason);
}

b32 storage_mountpoint(positive argc, string_address address_to argv,
                       writer output, writer diagnostic)
{
        static const storage_argument_name options[] = {
            STORAGE_ARGUMENT("quiet", 'q'),
            STORAGE_ARGUMENT("fs-devno", 'd'),
            STORAGE_ARGUMENT("devno", 'x'),
            STORAGE_ARGUMENT("nofollow", 'N'),
        };
        bool quiet = false;
        bool fs_devno = false;
        bool devno = false;
        bool nofollow = false;
        string_address path = null;
        storage_arguments taking = {.argc = argc, .argv = argv, .at = 1};
        string_address value;
        b32 option;

        while ((option = storage_argument_next(
                    address_of taking, (string_address)"qdx",
                    (string_address)"", options, array_count(options),
                    address_of value)) != STORAGE_ARGUMENT_END)
        {
                if (option == 'q')
                        quiet = true;
                else if (option == 'd')
                        fs_devno = true;
                else if (option == 'x')
                        devno = true;
                else if (option == 'N')
                        nofollow = true;
                else if (option == STORAGE_ARGUMENT_OPERAND)
                {
                        if (path)
                        {
                                storage_write_text(diagnostic,
                                    (string_address) "mountpoint: too many paths\n");
                                return 1;
                        }
                        path = value;
                }
                else
                {
                        storage_write_text(diagnostic,
                            (string_address) "mountpoint: unsupported option\n");
                        return 1;
                }
        }

        if (!path)
        {
                storage_write_text(diagnostic,
                    (string_address) "mountpoint: exactly one path is required\n");
                return 1;
        }

        if (devno)
        {
                file_facts facts;

                if (!file_look(AT_FDCWD, path,
                               nofollow ? AT_SYMLINK_NOFOLLOW : 0,
                               address_of facts))
                {
                        storage_mountpoint_error(
                            diagnostic, quiet, path,
                            (string_address) ": cannot inspect\n");
                        return 1;
                }

                if ((facts.mode & MODE_FORMAT) != MODE_BLOCK)
                {
                        storage_mountpoint_error(
                            diagnostic, quiet, path,
                            (string_address) ": not a block device\n");
                        return 32;
                }

                storage_device_number(output, facts.rdev_major,
                                       facts.rdev_minor);
                return 0;
        }

        bipolar handle = system_open_at(AT_FDCWD,
                                       path,
                                       STORAGE_OPEN_PATH |
                                           O_CLOEXEC |
                                           (nofollow ? STORAGE_OPEN_NOFOLLOW : 0));
        bool mounted = false;
        bool inspected = false;
        file_facts here;

        if (handle < 0)
        {
                storage_mountpoint_error(
                    diagnostic, quiet, path,
                    (string_address) ": cannot inspect\n");
                return 1;
        }

        {
                file_facts parent;
                positive resolved_room = 0;
                p8 address_to resolved = storage_fd_path(handle,
                                                         address_of resolved_room);
                bool have_here = file_look(handle, (string_address) "",
                                           AT_EMPTY_PATH, address_of here);

                if (resolved && !string_compare(resolved,
                                                (string_address) "/"))
                {
                        mounted = true;
                        inspected = have_here;
                }
                else if (resolved)
                {
                        positive length = string_length(resolved);

                        while (length > 1 && resolved[length - 1] == '/')
                                resolved[--length] = end;

                        while (length > 1 && resolved[length - 1] != '/')
                                length--;

                        while (length > 1 && resolved[length - 1] == '/')
                                length--;

                        resolved[length] = end;

                        if (have_here && file_look_at(resolved,
                                                     address_of parent))
                        {
                                mounted = here.mount_id != parent.mount_id;
                                inspected = true;
                        }
                }

                if (resolved)
                        memory_free(resolved, resolved_room);

                system_close(handle);
        }

        if (!inspected)
        {
                storage_mountpoint_error(
                    diagnostic, quiet, path,
                    (string_address) ": cannot inspect\n");
                return 1;
        }

        if (mounted && fs_devno)
        {
                storage_device_number(output, here.device_major,
                                       here.device_minor);
                return 0;
        }

        if (!quiet)
        {
                storage_write_text(output, path);
                storage_write_text(output, mounted
                    ? (string_address) " is a mountpoint\n"
                    : (string_address) " is not a mountpoint\n");
        }

        /* util-linux reserves 1 for invocation/inspection errors and uses
           32 for the ordinary, script-readable "not a mountpoint" answer. */
        return mounted ? 0 : 32;
}
