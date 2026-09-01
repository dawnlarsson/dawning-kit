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

#define STORAGE_OPEN_CLOEXEC 02000000
#define STORAGE_OPEN_PATH    010000000
#define STORAGE_OPEN_NOFOLLOW 0400000

/*
        Shared API (the definitions live together because shell sources are
        one translation unit):

          storage_mount_table_load / storage_mount_table_release
          storage_mount_find_id / storage_mount_find_target
          storage_fstab_table_load / storage_fstab_table_release
          storage_fstab_find

        A successful load owns both table blocks until release.  Callers may
        iterate entry[0..count), and every string remains valid until then.
        storage_fstab_find accepts either the source or target spelling, which
        is the lookup mount uses when one operand is omitted.
*/
typedef struct
{
        positive id;
        positive parent_id;
        string_address device;
        string_address root;
        string_address target;
        string_address options;
        string_address type;
        string_address source;
        string_address filesystem_options;
} storage_mount;

typedef struct
{
        p8 address_to text;
        positive text_room;
        positive text_used;
        storage_mount address_to entry;
        positive entry_room;
        positive count;
} storage_mount_table;

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
        p8 address_to text;
        positive text_room;
        positive text_used;
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

static fn storage_write_two(writer output, string_address first,
                            string_address second)
{
        storage_write_text(output, first);
        storage_write_text(output, second);
}

/* Read the whole file.  A short read is not EOF for procfs, and one more byte
   is always reserved for the terminator rather than borrowed from the file. */
static bool storage_read_all(string_address path, p8 address_to address_to text,
                             positive address_to room,
                             positive address_to used)
{
        bipolar handle = system_call_4(syscall(openat), AT_FDCWD,
                                       (positive)path,
                                       STORAGE_OPEN_CLOEXEC, 0);

        if (handle < 0)
                return false;

        address_to used = 0;

        while (true)
        {
                positive want;

                if (*used > positive_max - 4097)
                {
                        system_call_1(syscall(close), (positive)handle);
                        return false;
                }

                want = *used + 4097;

                if (!memory_reserve((address_any address_to)text, room,
                                    *used, want, 1, 4096))
                {
                        system_call_1(syscall(close), (positive)handle);
                        return false;
                }

                bipolar got = system_read_retry((positive)handle,
                                                *text + *used, 4096);

                if (got < 0)
                {
                        system_call_1(syscall(close), (positive)handle);
                        return false;
                }

                if (!got)
                        break;

                address_to used += (positive)got;
        }

        system_call_1(syscall(close), (positive)handle);
        (*text)[*used] = end;
        return true;
}

#define STORAGE_TABLE_RELEASE(name, type)                                    \
        fn name(type address_to table)                                       \
        {                                                                    \
                memory_release((address_any address_to)address_of table->text, \
                               address_of table->text_room,                   \
                               address_of table->text_used, 1);               \
                memory_release((address_any address_to)address_of table->entry, \
                               address_of table->entry_room,                  \
                               address_of table->count, sizeof(table->entry[0])); \
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
        {
                p8 value;

                if (*read == '\\' && storage_octal(read + 1,
                                                   address_of value))
                {
                        *write++ = value;
                        read += 4;
                }
                else
                        *write++ = *read++;
        }

        *write = end;
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

        if (!memory_reserve((address_any address_to)address_of table->entry,
                            address_of table->entry_room, table->count,
                            table->count + 1, sizeof(storage_mount), 32))
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

        if (!storage_read_all((string_address) "/proc/self/mountinfo",
                              address_of table->text,
                              address_of table->text_room,
                              address_of table->text_used))
        {
                storage_write_text(diagnostic,
                                   (string_address) "cannot read /proc/self/mountinfo\n");
                return false;
        }

        p8 address_to line = table->text;
        p8 address_to limit = table->text + table->text_used;

        while (line < limit)
        {
                p8 address_to newline = (p8 address_to)memory_first_of(
                    line, '\n', (positive)(limit - line));
                p8 address_to next = newline ? newline + 1 : limit;

                if (newline)
                        *newline = end;

                if (*line && !storage_mount_line(table, line))
                {
                        storage_write_text(diagnostic,
                                           (string_address) "invalid /proc/self/mountinfo line\n");
                        storage_mount_table_release(table);
                        return false;
                }

                line = next;
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

                while (*read)
                {
                        p8 value;

                        if (*read == ' ' || *read == '\t')
                                break;

                        if (*read == '\\' &&
                            storage_octal(read + 1, address_of value))
                        {
                                *write++ = value;
                                read += 4;
                        }
                        else
                                *write++ = *read++;
                }

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

        if (!storage_read_all(path, address_of table->text,
                              address_of table->text_room,
                              address_of table->text_used))
        {
                if (!missing_ok)
                {
                        storage_write_two(diagnostic,
                                          (string_address) "cannot read ", path);
                        storage_write_text(diagnostic, (string_address) "\n");
                }

                return missing_ok;
        }

        p8 address_to line = table->text;
        p8 address_to limit = table->text + table->text_used;
        positive line_number = 0;

        while (line < limit)
        {
                p8 address_to newline = (p8 address_to)memory_first_of(
                    line, '\n', (positive)(limit - line));
                p8 address_to next = newline ? newline + 1 : limit;
                string_address fields[7];
                positive count;

                line_number++;

                if (newline)
                        *newline = end;

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
                                line = next;
                                continue;
                        }

                        if (!memory_reserve(
                                (address_any address_to)address_of table->entry,
                                address_of table->entry_room, table->count,
                                table->count + 1, sizeof(storage_fstab), 32))
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

                line = next;
        }

        return true;
}

PURE storage_mount address_to storage_mount_find_id(
    storage_mount_table address_to table, positive id)
{
        for (positive at = 0; at < table->count; at++)
                if (table->entry[at].id == id)
                        return table->entry + at;

        return null;
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

PURE storage_fstab address_to storage_fstab_find(
    storage_fstab_table address_to table, string_address name)
{
        for (positive at = 0; at < table->count; at++)
                if (!string_compare(table->entry[at].source, name) ||
                    !string_compare(table->entry[at].target, name))
                        return table->entry + at;

        return null;
}

static PURE bool storage_type_match(string_address list, string_address type)
{
        bool include_seen = false;
        bool included = false;
        string_address cursor = list;
        string_address at;
        positive type_length;
        positive length;

        if (!list)
                return true;
        if (!*list)
                return false;

        type_length = string_length(type);

        while ((at = storage_comma_next(address_of cursor, address_of length)))
        {
                bool exclude = length > 2 && at[0] == 'n' && at[1] == 'o';
                string_address name = exclude ? at + 2 : at;
                positive name_length = length - (exclude ? 2 : 0);
                bool equal = type_length == name_length &&
                             !string_compare_max(type, name, name_length);

                if (exclude && equal)
                        return false;

                if (!exclude)
                {
                        include_seen = true;

                        if (equal)
                                included = true;
                }

        }

        return !include_seen || included;
}

enum storage_column
{
        STORAGE_SOURCE,
        STORAGE_TARGET,
        STORAGE_FSTYPE,
        STORAGE_OPTIONS,
        STORAGE_FSROOT,
        STORAGE_MAJMIN,
        STORAGE_ID,
        STORAGE_PARENT,
        STORAGE_VFS_OPTIONS,
        STORAGE_FS_OPTIONS,
};

#define STORAGE_COLUMN_MAX 10

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

static string_address storage_attached_long(string_address argument,
                                            string_address name)
{
        positive length = string_length(name);

        if (string_compare_max(argument, name, length) ||
            argument[length] != '=')
                return null;

        return argument + length + 1;
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
                bool present;

                if (!length)
                        return false;

                present = storage_option_has_length(mount->options, at, length) ||
                          storage_option_has_length(mount->filesystem_options,
                                                    at, length);

                /* The filesystem column is authoritative when it carries a
                   read-only/read-write state opposite to the VFS column. */
                if (length == 2 && !memory_compare(at, "rw", 2) &&
                    storage_option_has(mount->filesystem_options,
                                       (string_address)"ro"))
                        present = false;
                else if (length == 2 && !memory_compare(at, "ro", 2) &&
                         storage_option_has(mount->filesystem_options,
                                            (string_address)"rw"))
                        present = false;

                /* libmount first honours a literal `no...` option.  When
                   there is none, it treats the spelling as a request that
                   the positive option be absent: norw matches a read-only
                   mount, while nodev still matches the actual nodev flag. */
                if (!present && length > 2 && at[0] == 'n' && at[1] == 'o')
                {
                        bool positive_present = storage_option_has_length(
                            mount->options, at + 2, length - 2) ||
                            storage_option_has_length(mount->filesystem_options,
                                                      at + 2, length - 2);

                        if (length == 4 && !memory_compare(at + 2, "rw", 2) &&
                            storage_option_has(mount->filesystem_options,
                                               (string_address)"ro"))
                                positive_present = false;
                        else if (length == 4 &&
                                 !memory_compare(at + 2, "ro", 2) &&
                                 storage_option_has(mount->filesystem_options,
                                                    (string_address)"rw"))
                                positive_present = false;

                        present = !positive_present;
                }

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
                enum storage_column column;

                if (length == 6 && !memory_compare_ascii_case(
                                           at, (string_address) "SOURCE", 6))
                        column = STORAGE_SOURCE;
                else if (length == 6 && !memory_compare_ascii_case(
                                                at, (string_address) "TARGET", 6))
                        column = STORAGE_TARGET;
                else if (length == 6 && !memory_compare_ascii_case(
                                                at, (string_address) "FSTYPE", 6))
                        column = STORAGE_FSTYPE;
                else if (length == 7 && !memory_compare_ascii_case(
                                                at, (string_address) "OPTIONS", 7))
                        column = STORAGE_OPTIONS;
                else if (length == 6 && !memory_compare_ascii_case(
                                                at, (string_address) "FSROOT", 6))
                        column = STORAGE_FSROOT;
                else if (length == 7 && !memory_compare_ascii_case(
                                                at, (string_address) "MAJ:MIN", 7))
                        column = STORAGE_MAJMIN;
                else if (length == 2 && !memory_compare_ascii_case(
                                                at, (string_address) "ID", 2))
                        column = STORAGE_ID;
                else if (length == 6 && !memory_compare_ascii_case(
                                                at, (string_address) "PARENT", 6))
                        column = STORAGE_PARENT;
                else if (length == 11 && !memory_compare_ascii_case(
                                                at, (string_address) "VFS-OPTIONS", 11))
                        column = STORAGE_VFS_OPTIONS;
                else if (length == 10 && !memory_compare_ascii_case(
                                                at, (string_address) "FS-OPTIONS", 10))
                        column = STORAGE_FS_OPTIONS;
                else
                        return false;

                if (options->count == STORAGE_COLUMN_MAX)
                        return false;

                options->columns[options->count++] = column;
        }

        return options->count != 0;
}

static string_address storage_column_name(enum storage_column column)
{
        switch (column)
        {
        case STORAGE_SOURCE: return (string_address) "SOURCE";
        case STORAGE_TARGET: return (string_address) "TARGET";
        case STORAGE_FSTYPE: return (string_address) "FSTYPE";
        case STORAGE_OPTIONS: return (string_address) "OPTIONS";
        case STORAGE_FSROOT: return (string_address) "FSROOT";
        case STORAGE_MAJMIN: return (string_address) "MAJ:MIN";
        case STORAGE_ID: return (string_address) "ID";
        case STORAGE_PARENT: return (string_address) "PARENT";
        case STORAGE_VFS_OPTIONS: return (string_address) "VFS-OPTIONS";
        default: return (string_address) "FS-OPTIONS";
        }
}

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

        switch (column)
        {
        case STORAGE_SOURCE:  return mount->source;
        case STORAGE_TARGET:  return mount->target;
        case STORAGE_FSTYPE:  return mount->type;
        case STORAGE_VFS_OPTIONS: return mount->options;
        case STORAGE_OPTIONS: return mount->options;
        case STORAGE_FSROOT: return mount->root;
        case STORAGE_MAJMIN: return mount->device;
        case STORAGE_FS_OPTIONS: return mount->filesystem_options;
        default: return null;
        }
}

static PURE inline INLINE bool storage_filesystem_option_represented(
    storage_mount address_to mount, string_address option, positive length)
{
        return storage_option_has_length(mount->options, option, length) ||
            (length == 2 &&
             ((!memory_compare(option, "ro", 2) &&
               storage_option_has(mount->options, (string_address)"rw")) ||
              (!memory_compare(option, "rw", 2) &&
               storage_option_has(mount->options, (string_address)"ro"))));
}

static PURE positive storage_combined_options_length(storage_mount address_to mount)
{
        positive length = 0;
        string_address cursor = mount->options;
        string_address at;
        positive token_length;

        while ((at = storage_comma_next(address_of cursor,
                                         address_of token_length)))
        {
                if (token_length)
                        length += token_length + (length ? 1 : 0);
        }

        cursor = mount->filesystem_options;
        while ((at = storage_comma_next(address_of cursor,
                                         address_of token_length)))
        {
                bool represented = storage_filesystem_option_represented(
                    mount, at, token_length);

                if (token_length && !represented)
                        length += token_length + (length ? 1 : 0);
        }

        return length;
}

static fn storage_combined_options_write(writer output,
                                         storage_mount address_to mount,
                                         bool raw, bool pairs)
{
        string_address cursor = mount->options;
        string_address at;
        positive token_length;
        bool any = false;

        while ((at = storage_comma_next(address_of cursor,
                                         address_of token_length)))
        {
                bool overridden = token_length == 2 &&
                    ((!memory_compare(at, "rw", 2) &&
                      storage_option_has(mount->filesystem_options,
                                         (string_address)"ro")) ||
                     (!memory_compare(at, "ro", 2) &&
                      storage_option_has(mount->filesystem_options,
                                         (string_address)"rw")));

                if (token_length)
                {
                        string_address shown = overridden
                            ? (!memory_compare(at, "rw", 2)
                                   ? (string_address)"ro"
                                   : (string_address)"rw")
                            : at;

                        if (any)
                                output((address_any)",", 1);

                        if (pairs)
                        {
                                p8 saved = at[token_length];

                                if (!overridden)
                                        at[token_length] = end;
                                storage_write_encoded(output, shown);
                                if (!overridden)
                                        at[token_length] = saved;
                        }
                        else if (raw)
                        {
                                p8 saved = at[token_length];

                                if (!overridden)
                                        at[token_length] = end;
                                storage_findmnt_value(output, shown, true);
                                if (!overridden)
                                        at[token_length] = saved;
                        }
                        else
                                output((address_any)shown, token_length);

                        any = true;
                }
        }

        cursor = mount->filesystem_options;
        while ((at = storage_comma_next(address_of cursor,
                                         address_of token_length)))
        {
                bool represented = storage_filesystem_option_represented(
                    mount, at, token_length);

                if (token_length && !represented)
                {
                        if (any)
                                output((address_any)",", 1);

                        if (pairs)
                        {
                                p8 saved = at[token_length];

                                at[token_length] = end;
                                storage_write_encoded(output, at);
                                at[token_length] = saved;
                        }
                        else if (raw)
                        {
                                p8 saved = at[token_length];

                                at[token_length] = end;
                                storage_findmnt_value(output, at, true);
                                at[token_length] = saved;
                        }
                        else
                                output((address_any)at, token_length);

                        any = true;
                }
        }
}

static bool storage_source_has_root(storage_mount address_to mount)
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
                        if (!options->raw && at + 1 < options->count)
                        {
                                positive length = positive_digits(value_number);

                                if (widths[at] > length)
                                        writer_fill(output,
                                                    widths[at] - length, ' ');
                        }
                }
                else if (!heading && options->columns[at] == STORAGE_OPTIONS)
                {
                        storage_combined_options_write(output, mount,
                                                       options->raw, false);
                        if (!options->raw && at + 1 < options->count)
                        {
                                positive length =
                                    storage_combined_options_length(mount);

                                if (widths[at] > length)
                                        writer_fill(output,
                                                    widths[at] - length, ' ');
                        }
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

                        if (!options->raw && at + 1 < options->count)
                        {
                                positive length = storage_findmnt_cell_length(
                                    mount, STORAGE_SOURCE, true);

                                if (widths[at] > length)
                                        writer_fill(output,
                                                    widths[at] - length, ' ');
                        }
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
        storage_findmnt_options options = {
            .columns = {STORAGE_TARGET, STORAGE_SOURCE,
                        STORAGE_FSTYPE, STORAGE_OPTIONS},
            .count = 4,
        };
        storage_mount_table table;
        positive query_id = 0;
        bool have_query_id = false;
        positive widths[STORAGE_COLUMN_MAX] = {0};

        for (positive at = 1; at < argc; at++)
        {
                string_address word = argv[at];
                string_address attached;

                if (word[0] != '-' || !word[1])
                {
                        if (options.operand)
                        {
                                storage_write_text(diagnostic,
                                    (string_address) "findmnt: too many arguments\n");
                                return 1;
                        }

                        options.operand = *word ? word : (string_address) "/";
                        continue;
                }

                if (!string_compare(word, (string_address) "--"))
                {
                        if (++at < argc)
                        {
                                options.operand = *argv[at] ? argv[at] :
                                                               (string_address) "/";
                                at++;
                        }

                        if (at != argc)
                        {
                                storage_write_text(diagnostic,
                                    (string_address) "findmnt: too many arguments\n");
                                return 1;
                        }

                        break;
                }

                if (!string_compare(word, (string_address) "--noheadings"))
                {
                        options.no_headings = true;
                        continue;
                }
                if (!string_compare(word, (string_address) "--raw"))
                {
                        options.raw = true;
                        continue;
                }
                if (!string_compare(word, (string_address) "--list"))
                        continue;
                if (!string_compare(word, (string_address) "--nofsroot"))
                {
                        options.no_fsroot = true;
                        continue;
                }
                if (!string_compare(word, (string_address) "--pairs"))
                {
                        options.pairs = true;
                        options.no_headings = true;
                        continue;
                }
                if (!string_compare(word, (string_address) "--first-only"))
                {
                        options.first_only = true;
                        continue;
                }
                if (!string_compare(word, (string_address) "--invert"))
                {
                        options.invert = true;
                        continue;
                }

                attached = storage_attached_long(
                    word, (string_address) "--source");
                if (attached)
                {
                        options.source = attached;
                        continue;
                }
                attached = storage_attached_long(
                    word, (string_address) "--target");
                if (attached)
                {
                        options.target = *attached ? attached :
                                                     (string_address) "/";
                        options.path_query = true;
                        continue;
                }
                attached = storage_attached_long(
                    word, (string_address) "--mountpoint");
                if (attached)
                {
                        options.target = *attached ? attached :
                                                     (string_address) "/";
                        options.mountpoint_query = true;
                        continue;
                }
                attached = storage_attached_long(
                    word, (string_address) "--types");
                if (attached)
                {
                        options.type = attached;
                        continue;
                }
                attached = storage_attached_long(
                    word, (string_address) "--options");
                if (attached)
                {
                        options.option_filter = attached;
                        continue;
                }
                attached = storage_attached_long(
                    word, (string_address) "--output");
                if (attached)
                {
                        if (!storage_columns(attached, address_of options))
                        {
                                storage_write_text(diagnostic,
                                    (string_address) "findmnt: unsupported output column\n");
                                return 1;
                        }
                        continue;
                }
                if (!string_compare(word, (string_address) "--source") ||
                    !string_compare(word, (string_address) "--target") ||
                    !string_compare(word, (string_address) "--types") ||
                    !string_compare(word, (string_address) "--output") ||
                    !string_compare(word, (string_address) "--mountpoint") ||
                    !string_compare(word, (string_address) "--options"))
                {
                        string_address option = word;
                        string_address value;

                        if (++at >= argc)
                        {
                                storage_write_text(diagnostic,
                                    (string_address) "findmnt: option needs an argument\n");
                                return 1;
                        }
                        value = argv[at];

                        if (!string_compare(option,
                                            (string_address) "--source"))
                                options.source = value;
                        else if (!string_compare(option,
                                                 (string_address) "--target"))
                        {
                                options.target = *value ? value :
                                                          (string_address) "/";
                                options.path_query = true;
                        }
                        else if (!string_compare(option,
                                                 (string_address) "--mountpoint"))
                        {
                                options.target = *value ? value :
                                                          (string_address) "/";
                                options.mountpoint_query = true;
                        }
                        else if (!string_compare(option,
                                                 (string_address) "--types"))
                                options.type = value;
                        else if (!string_compare(option,
                                                 (string_address) "--options"))
                                options.option_filter = value;
                        else if (!storage_columns(value, address_of options))
                        {
                                storage_write_text(diagnostic,
                                    (string_address) "findmnt: unsupported output column\n");
                                return 1;
                        }

                        continue;
                }

                for (positive letter = 1; word[letter]; letter++)
                {
                        string_address value;

                        if (word[letter] == 'n')
                                options.no_headings = true;
                        else if (word[letter] == 'r')
                                options.raw = true;
                        else if (word[letter] == 'v')
                                options.no_fsroot = true;
                        else if (word[letter] == 'l')
                                ; /* This implementation is already list-shaped. */
                        else if (word[letter] == 'P')
                        {
                                options.pairs = true;
                                options.no_headings = true;
                        }
                        else if (word[letter] == 'f')
                                options.first_only = true;
                        else if (word[letter] == 'i')
                                options.invert = true;
                        else if (word[letter] == 'S' || word[letter] == 'T' ||
                                 word[letter] == 'M' ||
                                 word[letter] == 't' || word[letter] == 'o' ||
                                 word[letter] == 'O')
                        {
                                /* storage_option_value expects the option at
                                   word[1]; attached values are handled here. */
                                if (word[letter + 1])
                                        value = word + letter + 1;
                                else if (at + 1 < argc)
                                        value = argv[++at];
                                else
                                {
                                        storage_write_text(diagnostic,
                                            (string_address) "findmnt: option needs an argument\n");
                                        return 1;
                                }

                                if (word[letter] == 'S')
                                        options.source = value;
                                else if (word[letter] == 'T')
                                {
                                        options.target = *value ? value :
                                                                  (string_address) "/";
                                        options.path_query = true;
                                }
                                else if (word[letter] == 'M')
                                {
                                        options.target = *value ? value :
                                                                  (string_address) "/";
                                        options.mountpoint_query = true;
                                }
                                else if (word[letter] == 't')
                                        options.type = value;
                                else if (word[letter] == 'O')
                                        options.option_filter = value;
                                else if (!storage_columns(value,
                                                          address_of options))
                                {
                                        storage_write_text(diagnostic,
                                            (string_address) "findmnt: unsupported output column\n");
                                        return 1;
                                }

                                break;
                        }
                        else
                        {
                                storage_write_text(diagnostic,
                                    (string_address) "findmnt: unsupported option\n");
                                return 1;
                        }
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
        p8 address_to path = null;
        positive path_room = 0;
        positive used = positive_into_string(number, (positive)handle);

        memory_copy_apart(name, "/proc/self/fd/", 14);
        memory_copy_apart(name + 14, number, used + 1);

        while (true)
        {
                positive want = path_room ? path_room * 2 : 256;

                if (want <= path_room ||
                    !memory_reserve((address_any address_to)address_of path,
                                    address_of path_room, 0, want, 1, 256))
                {
                        memory_release((address_any address_to)address_of path,
                                       address_of path_room, room, 1);
                        return null;
                }

                bipolar got = system_call_4(syscall(readlinkat), AT_FDCWD,
                                             (positive)name, (positive)path,
                                             path_room - 1);

                if (got < 0)
                {
                        memory_release((address_any address_to)address_of path,
                                       address_of path_room, room, 1);
                        return null;
                }

                if ((positive)got < path_room - 1)
                {
                        path[got] = end;
                        address_to room = path_room;
                        return path;
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

        storage_write_two(diagnostic, (string_address) "mountpoint: ", path);
        storage_write_text(diagnostic, reason);
}

b32 storage_mountpoint(positive argc, string_address address_to argv,
                       writer output, writer diagnostic)
{
        bool quiet = false;
        bool fs_devno = false;
        bool devno = false;
        bool nofollow = false;
        string_address path = null;

        for (positive at = 1; at < argc; at++)
        {
                string_address argument = argv[at];

                if (!string_compare(argument, (string_address) "--quiet"))
                        quiet = true;
                else if (!string_compare(argument,
                                         (string_address) "--fs-devno"))
                        fs_devno = true;
                else if (!string_compare(argument, (string_address) "--devno"))
                        devno = true;
                else if (!string_compare(argument,
                                         (string_address) "--nofollow"))
                        nofollow = true;
                else if (!string_compare(argument, (string_address) "--"))
                {
                        if (++at < argc)
                        {
                                if (path || at + 1 != argc)
                                {
                                        storage_write_text(diagnostic,
                                            (string_address) "mountpoint: too many paths\n");
                                        return 1;
                                }

                                path = argv[at++];
                        }

                        if (at != argc)
                        {
                                storage_write_text(diagnostic,
                                    (string_address) "mountpoint: too many paths\n");
                                return 1;
                        }
                        break;
                }
                else if (argument[0] == '-' && argument[1])
                {
                        for (positive letter = 1; argument[letter]; letter++)
                        {
                                if (argument[letter] == 'q')
                                        quiet = true;
                                else if (argument[letter] == 'd')
                                        fs_devno = true;
                                else if (argument[letter] == 'x')
                                        devno = true;
                                else
                                {
                                        storage_write_text(diagnostic,
                                            (string_address) "mountpoint: unsupported option\n");
                                        return 1;
                                }
                        }
                }
                else if (!path)
                        path = argv[at];
                else
                {
                        storage_write_text(diagnostic,
                            (string_address) "mountpoint: too many paths\n");
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

        bipolar handle = system_call_4(syscall(openat), AT_FDCWD,
                                       (positive)path,
                                       STORAGE_OPEN_PATH |
                                           STORAGE_OPEN_CLOEXEC |
                                           (nofollow ? STORAGE_OPEN_NOFOLLOW : 0),
                                       0);
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

                system_call_1(syscall(close), (positive)handle);
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
