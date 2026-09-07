#include "../compiler_memory.c"

static p8 table_output[16384];
static positive table_used, table_calls;
static bool table_overflow;

static fn table_capture(address_any bytes, positive length)
{
        table_calls++;
        if (!length)
                length = string_length(bytes);
        if (length > sizeof(table_output) - table_used)
        {
                table_overflow = true;
                return;
        }
        memory_copy_apart(table_output + table_used, bytes, length);
        table_used += length;
}

#define log table_capture
#include "../spark.c"
#include "../sh/shell.c"
#undef log
#include "counted.inc"

static fn table_reset(void)
{
        table_used = table_calls = 0;
        table_overflow = false;
}

static string_address table_field(address_any row, p8 column,
                                   p8 address_to scratch)
{
        (void)column; (void)scratch;
        return *(string_address address_to)row;
}

static const ul_table_column table_columns[] = {
    {"value", "VALUE", 0, false, UL_TABLE_STRING, true}
};
static p8 table_selected[] = {0};

static fn table_render(string_address value, positive mode)
{
        table_reset();
        ul_table(mode == 2 ? "test" : null, address_of value, 1,
                 table_columns, table_selected, 1, true, mode == 1, table_field);
}

static p8 table_hex_digit(p8 nibble)
{
        return nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
}

static fn name_list_checks(void)
{
        p8 columns[1] = {0xa5};
        positive have = 99;
        check("table defaults without an output option",
              ul_table_column_list(null, table_columns, 1, table_selected, 1,
                                   columns, address_of have) && have == 1 && !columns[0]);
        check("table defaults stay bounded",
              !ul_table_column_list(null, table_columns, 0, table_selected, 1,
                                    columns, address_of have) && !have);
        check("table append keeps defaults and removes duplicates",
              ul_table_column_list("+VALUE,value", table_columns, 1, table_selected, 1,
                                   columns, address_of have) && have == 1 && !columns[0]);
        typedef struct
        {
                string_address name;
                positive payload;
        } named;
        static const named definitions[] = {
            {(string_address)"alpha", 11},
            {(string_address)"Beta", 22},
            {(string_address)"gamma", 33},
        };
        p8 selected[4];
        positive count = 0;

        check("name list case fold and trailing comma",
              name_list_select((string_address)"ALPHA,beta,", definitions,
                               sizeof(definitions[0]),
                               array_count(definitions), selected,
                               address_of count, array_count(selected), 0) &&
              count == 2 && selected[0] == 0 && selected[1] == 1);

        count = 0;
        check("name list duplicates retained",
              name_list_select((string_address)"alpha,alpha", definitions,
                               sizeof(definitions[0]),
                               array_count(definitions), selected,
                               address_of count, array_count(selected), 0) &&
              count == 2 && selected[0] == 0 && selected[1] == 0);

        selected[0] = 0;
        count = 1;
        check("name list unique seeded prefix",
              name_list_select((string_address)"ALPHA,gamma", definitions,
                               sizeof(definitions[0]),
                               array_count(definitions), selected,
                               address_of count, array_count(selected),
                               NAME_LIST_UNIQUE) &&
              count == 2 && selected[0] == 0 && selected[1] == 2);

        count = 0;
        check("name list exact case",
              !name_list_select((string_address)"ALPHA", definitions,
                                sizeof(definitions[0]),
                                array_count(definitions), selected,
                                address_of count, array_count(selected),
                                NAME_LIST_CASE_SENSITIVE));

        count = 0;
        check("name list strict trailing comma",
              !name_list_select((string_address)"alpha,", definitions,
                                sizeof(definitions[0]),
                                array_count(definitions), selected,
                                address_of count, array_count(selected),
                                NAME_LIST_REJECT_TRAILING));

        count = 0;
        check("name list capacity bound",
              !name_list_select((string_address)"alpha,Beta", definitions,
                                sizeof(definitions[0]),
                                array_count(definitions), selected,
                                address_of count, 1, 0));

        selected[0] = 0;
        count = 1;
        check("name list full unique prefix",
              name_list_select((string_address)"alpha", definitions,
                               sizeof(definitions[0]),
                               array_count(definitions), selected,
                               address_of count, 1, NAME_LIST_UNIQUE) &&
              count == 1 &&
              !name_list_select((string_address)"Beta", definitions,
                                sizeof(definitions[0]),
                                array_count(definitions), selected,
                                address_of count, 1, NAME_LIST_UNIQUE));

        named wide[256];
        for (positive i = 0; i < array_count(wide); i++)
                wide[i] = (named){(string_address)"other", i};
        wide[255].name = (string_address)"last";
        count = 0;
        check("name list byte index boundary",
              name_list_select((string_address)"last", wide, sizeof(wide[0]),
                               array_count(wide), selected,
                               address_of count, array_count(selected), 0) &&
              count == 1 && selected[0] == 255);
        count = 0;
        check("name list excessive definitions",
              !name_list_select((string_address)"last", wide,
                                sizeof(wide[0]), 257, selected,
                                address_of count, array_count(selected), 0));

        p8 lock_fields[] = "  1\tPOSIX\vowner";
        p8 address_to field_at = lock_fields;
        string_address id = storage_field(address_of field_at);
        string_address kind = storage_field(address_of field_at);
        check("kernel field grammar is space and tab",
              string_equals(id, "1") &&
              string_equals(kind, "POSIX\vowner"));

        p8 records[] = "first\nlast";
        p8 address_to record_at = records;
        p8 address_to record_limit = records + sizeof(records) - 1;
        string_address first = storage_line_next(address_of record_at,
                                                 record_limit);
        string_address last = storage_line_next(address_of record_at,
                                                record_limit);
        check("kernel final line uses owned sentinel",
              string_equals(first, "first") && string_equals(last, "last") &&
              !storage_line_next(address_of record_at, record_limit));
}

static fn table_checks(void)
{
        p8 bytes[1030], expected[8192];
        for (positive size = 0; size <= 1024; size++)
        {
                positive width = 0, longest = 0, lines = 1, single_width = 0;
                for (positive at = 0; at < size; at++)
                {
                        p8 byte = (p8)(1 + ((at * 197 + size) % 255));
                        bytes[at] = byte;
                        single_width += byte < ' ' || byte == 127 ? 4 : 1;
                        if (byte == '\n')
                        {
                                longest = max(longest, width);
                                width = 0;
                                lines++;
                        }
                        else
                                width += byte < ' ' || byte == 127 ? 4 : 1;
                }
                bytes[size] = 0;
                longest = max(longest, width);
                check("table multiline width", ul_table_safe_width(bytes, true) == longest);
                check("table single-line width", ul_table_safe_width(bytes, false) == single_width);
#ifdef TABLE_LEGACY_LINE_COUNT
                // For the paired harness built against the pre-fold snapshot.
                check("table newline count", ul_table_line_count(bytes, true) == lines);
#else
                check("table newline count", ul_table_line_count(bytes) == lines);
#endif
                positive start = 0;
                for (positive line = 0; line <= lines; line++)
                {
                        positive stop = start, got_length;
                        while (stop < size && bytes[stop] != '\n')
                                stop++;
                        string_address got = ul_table_line(bytes, line, address_of got_length);
                        positive wanted = line < lines ? stop - start : 0;
                        check("table indexed line", got_length == wanted &&
                              !memory_compare(got, bytes + start, wanted));
                        start = stop < size ? stop + 1 : size;
                }
        }

        // Bounded safe fields include embedded NUL; raw/JSON fields end at it.
        positive used = 0;
        for (positive byte = 0; byte < 256; byte++)
        {
                bytes[byte] = (p8)byte;
                if (byte < ' ' || byte == 127)
                {
                        expected[used++] = '\\'; expected[used++] = 'x';
                        expected[used++] = table_hex_digit(byte >> 4);
                        expected[used++] = table_hex_digit(byte & 15);
                }
                else
                        expected[used++] = byte;
        }
        table_reset();
        ul_lsns_safe_span(bytes, 256);
        check("safe byte policy", !table_overflow && table_used == used &&
              !memory_compare(table_output, expected, used));
        check("safe width agrees with output", ul_lsns_safe_span_length(bytes, 256) == used);

        for (positive byte = 1; byte < 256; byte++)
                bytes[byte - 1] = byte;
        bytes[255] = 0;
        for (positive mode = 1; mode <= 2; mode++)
        {
                string_address prefix = mode == 1 ? "VALUE\n" :
                    "{\n   \"test\": [\n      {\n         \"value\": \"";
                used = string_length(prefix);
                memory_copy(expected, prefix, used);
                for (positive byte = 1; byte < 256; byte++)
                {
                        if (mode == 1 && (byte <= ' ' || byte >= 127 || byte == '\\'))
                        {
                                expected[used++] = '\\'; expected[used++] = 'x';
                        }
                        else if (mode == 2 && byte < ' ')
                        {
                                expected[used++] = '\\'; expected[used++] = 'u';
                                expected[used++] = '0'; expected[used++] = '0';
                        }
                        else
                        {
                                if (mode == 2 && (byte == '"' || byte == '\\'))
                                        expected[used++] = '\\';
                                expected[used++] = byte;
                                continue;
                        }
                        expected[used++] = table_hex_digit(byte >> 4);
                        expected[used++] = table_hex_digit(byte & 15);
                }
                string_address suffix = mode == 1 ? "\n" : "\"\n      }\n   ]\n}\n";
                positive tail = string_length(suffix);
                memory_copy(expected + used, suffix, tail);
                used += tail;
                table_render(bytes, mode);
                if (table_used != used || memory_compare(table_output, expected, used))
                        string_format(log, "table mode=%p got=%p wanted=%p prefix=%p\n",
                                      mode, table_used, used,
                                      memory_common_prefix(table_output, expected,
                                                           min(table_used, used)));
                check("raw/JSON exact byte policy", !table_overflow && table_used == used &&
                      !memory_compare(table_output, expected, used));
        }

        table_render("ab\tcd\nx\n", 0);
        check("normal multiline layout", table_used == 18 &&
              !memory_compare(table_output, "VALUE\nab\\x09cd\nx\n\n", 18));

        table_render("", 1);
        check("empty raw field", table_used == 7 &&
              !memory_compare(table_output, "VALUE\n\n", 7));
        string_address empty_json =
            "{\n   \"test\": [\n      {\n         \"value\": \"\"\n      }\n   ]\n}\n";
        table_render("", 2);
        check("empty JSON field", table_used == string_length(empty_json) &&
              !memory_compare(table_output, empty_json, table_used));
}

typedef struct
{
        p8 column;
        string_address expected;
} table_projection;

// Paired benchmarks define TABLE_BASELINE on both revisions so these
// reference-only call sites do not change the optimizer's decisions.
#ifndef TABLE_BASELINE
_Static_assert(sizeof(ul_table_column) == 32, "field facts must fit column padding");

static fn table_projection_modes(address_any row,
    const ul_table_column address_to columns, positive count,
    p8 address_to selected, positive selected_count, ul_table_field field)
{
        ul_table_column callbacks[64];
        p8 expected[8192];
        memory_copy_apart(callbacks, (address_any)columns, count * sizeof(columns[0]));
        for (positive i = 0; i < count; i++)
                callbacks[i].decimal = false;
        for (positive mode = 0; mode < 5; mode++)
        {
                positive expected_size = 0;
                for (positive path = 0; path < 2; path++)
                {
                        const ul_table_column address_to definitions = path ? columns : callbacks;
                        table_reset();
                        if (mode == 4)
                                ul_table_json("test", row, 0, 1, definitions,
                                              selected, selected_count, field);
                        else
                                ul_table_out(row, 0, 1, definitions, count,
                                             selected, selected_count, mode & 1,
                                             mode & 2, field);
                        check("projection output bound", !table_overflow && table_used <= sizeof(expected));
                        if (table_overflow || table_used > sizeof(expected))
                                return;
                        if (!path)
                        {
                                expected_size = table_used;
                                memory_copy_apart(expected, table_output, table_used);
                        }
                        else
                                check("decimal/escaped exact output",
                                      table_used == expected_size &&
                                      !memory_compare(table_output, expected, table_used));
                }
        }
}
#endif

static fn table_projection_check(string_address name, address_any row,
                                 const ul_table_column address_to columns,
                                 positive count, ul_table_field field,
                                 const table_projection address_to expected,
                                 positive projection_count)
{
        p8 scratch[96], selected[64];
        for (positive i = 0; i < projection_count; i++)
        {
                memory_fill(scratch, 0xa5, sizeof(scratch));
                selected[i] = expected[i].column;
                bool same = string_equals(field(row, selected[i], scratch),
                                           expected[i].expected);
                if (!same)
                        string_format(log, "projection %s column=%p\n", name, (positive)selected[i]);
                check("column projection", same);
                check("projection scratch bound", scratch[32] == 0xa5);
        }
#ifndef TABLE_BASELINE
        table_projection_modes(row, columns, count, selected, projection_count, field);
#endif
#ifdef TABLE_BENCHMARK
        for (positive mode = 0; mode < 3; mode++)
        {
                p64 start = get_cpu_time();
                for (positive round = 0; round < 16384; round++)
                {
                        table_reset();
                        if (mode == 2)
                                ul_table_json(name, row, 0, 1, columns,
                                              selected, projection_count, field);
                        else
                                ul_table_out(row, 0, 1, columns, count,
                                             selected, projection_count, true,
                                             mode == 1, field);
                }
                p64 elapsed = get_cpu_time() - start;
                string_format(log, "projection %s mode=%p ticks=%p\n",
                              name, mode, (positive)elapsed);
        }
#endif
}

static fn table_projection_checks(void)
{
#ifndef TABLE_BASELINE
        // Exercise the numeric fast path against escaping, including empty
        // nullable fields, both alignments, heading escapes and wider padding.
        static const string_address numbers[] = {
            "", "0", "1", "4294967295", "18446744073709551615",
        };
        p8 selected[] = {0, 0};
        ul_table_column column = {"number", "N\tUM", 0, false,
                                   UL_TABLE_STRING, .decimal = true};
        for (positive i = 0; i < array_count(numbers); i++)
                for (positive shape = 0; shape < 8; shape++)
                        for (p8 json = UL_TABLE_STRING; json <= UL_TABLE_NULL_NUMBER; json++)
                        {
                                string_address value = numbers[i];
                                column.width = (shape >> 1) * 10;
                                column.number = shape & 1;
                                column.json = json;
                                table_projection_modes(address_of value,
                                    address_of column, 1, selected, 2, table_field);
                        }
#endif
        struct snapshot_process process = {
            .pid = p32_max, .ppid = 1, .uid = 42, .command = "worker",
        };
        ul_lsns_entry ns = {.inode = p64_max, .process = address_of process,
                            .processes = p32_max, .command = "session", .user = "owner"};
        static const table_projection ns_expected[] = {
            {UL_LSNS_NS, "18446744073709551615"}, {UL_LSNS_NPROCS, "4294967295"},
            {UL_LSNS_PID, "4294967295"}, {UL_LSNS_PPID, "1"},
            {UL_LSNS_COMMAND, "session"}, {UL_LSNS_UID, "42"}, {UL_LSNS_USER, "owner"},
        };
        ul_lsfd_entry fd = {.process = address_of process, .user = "owner",
            .fd = p32_max, .inode = p64_max, .size = (p64)1 << 32,
            .type = "REG", .name = "a\tb\nc", .deleted = true, .access = 2};
        static const table_projection fd_expected[] = {
            {UL_LSFD_COMMAND, "worker"}, {UL_LSFD_PID, "4294967295"},
            {UL_LSFD_USER, "owner"}, {UL_LSFD_FD, "4294967295"},
            {UL_LSFD_TYPE, "REG"}, {UL_LSFD_NAME, "a\tb\nc"}, {UL_LSFD_KNAME, "a\tb\nc"},
            {UL_LSFD_INODE, "18446744073709551615"}, {UL_LSFD_SIZE, "4294967296"},
            {UL_LSFD_UID, "42"}, {UL_LSFD_DELETED, "1"},
            {UL_LSFD_MNTID, ""}, {UL_LSFD_POS, ""}, {UL_LSFD_MODE, ""},
        };
        ul_lslocks_entry lock = {.inode = p64_max, .start = 1,
                                 .finish = p64_max, .mandatory = true, .pid = -1};
        static const table_projection lock_expected[] = {
            {UL_LOCKS_COMMAND, ""}, {UL_LOCKS_INODE, "18446744073709551615"},
            {UL_LOCKS_MANDATORY, "1"}, {UL_LOCKS_START, "1"},
            {UL_LOCKS_END, "18446744073709551615"}, {UL_LOCKS_PATH, ""}, {UL_LOCKS_PID, "-1"},
        };
        ul_wipefs_row signature = {.device = "/dev/x", .type = "ext4",
            .label = "data", .length = 255, .magic = {0xa5, 0x5a, 1, 2, 3, 4, 5, 6},
            .usage = "filesystem"};
        static const table_projection signature_expected[] = {
            {UL_WIPEFS_DEVICE, "/dev/x"}, {UL_WIPEFS_TYPE, "ext4"},
            {UL_WIPEFS_UUID, ""}, {UL_WIPEFS_LABEL, "data"},
            {UL_WIPEFS_LENGTH, "255"}, {UL_WIPEFS_USAGE, "filesystem"},
        };
        ul_lsblk_device device = {.kname = "disk0", .path = "/dev/disk0", .type = "disk",
            .mount_text = "m1\nm2", .fstype = "ext4", .fsver = "1.0", .label = "data",
            .uuid = "fs-uuid", .partuuid = "part-uuid", .partlabel = "part-label",
            .owner = "root", .group = "disk", .mode = "brw-", .scheduler = "none",
            .transport = "pcie", .vendor = "vendor", .model = "model", .revision = "rev",
            .serial = "serial", .hctl = "0:1:2:3",
            .removable = true, .rotational = true, .alignment = p64_max,
            .minimum_io = 1, .optimal_io = (positive)1 << 32,
            .physical_sector = 4096, .logical_sector = 512, .read_ahead = 128};
        static const table_projection device_expected[] = {
            {UL_LSBLK_KNAME, "disk0"}, {UL_LSBLK_PATH, "/dev/disk0"},
            {UL_LSBLK_RM, "1"}, {UL_LSBLK_RO, "0"}, {UL_LSBLK_TYPE, "disk"},
            {UL_LSBLK_MOUNTPOINTS, "m1\nm2"}, {UL_LSBLK_FSTYPE, "ext4"}, {UL_LSBLK_FSVER, "1.0"},
            {UL_LSBLK_LABEL, "data"}, {UL_LSBLK_UUID, "fs-uuid"}, {UL_LSBLK_PARTUUID, "part-uuid"},
            {UL_LSBLK_PARTLABEL, "part-label"}, {UL_LSBLK_OWNER, "root"}, {UL_LSBLK_GROUP, "disk"},
            {UL_LSBLK_MODE, "brw-"}, {UL_LSBLK_ALIGNMENT, "18446744073709551615"},
            {UL_LSBLK_MINIO, "1"}, {UL_LSBLK_OPTIO, "4294967296"},
            {UL_LSBLK_PHYSEC, "4096"}, {UL_LSBLK_LOGSEC, "512"},
            {UL_LSBLK_ROTA, "1"}, {UL_LSBLK_SCHED, "none"}, {UL_LSBLK_RQSIZE, ""},
            {UL_LSBLK_RA, "128"}, {UL_LSBLK_TRAN, "pcie"}, {UL_LSBLK_VENDOR, "vendor"},
            {UL_LSBLK_MODEL, "model"}, {UL_LSBLK_REV, "rev"}, {UL_LSBLK_SERIAL, "serial"}, {UL_LSBLK_HCTL, "0:1:2:3"},
        };
        ul_ipc_row ipc = {.id = p64_max, .uid = 42, .gid = 43, .cuid = 44,
                          .cgid = 45, .count = (positive)1 << 32,
                          .pid_one = p32_max, .pid_two = 1};
        static const table_projection ipc_expected[] = {
            {UL_IPC_ID, "18446744073709551615"}, {UL_IPC_CUID, "44"},
            {UL_IPC_CGID, "45"}, {UL_IPC_UID, "42"}, {UL_IPC_GID, "43"},
            {UL_IPC_NATTCH, "4294967296"}, {UL_IPC_MSGS, "4294967296"},
            {UL_IPC_NSEMS, "4294967296"}, {UL_IPC_CPID, "4294967295"},
            {UL_IPC_LSPID, "4294967295"}, {UL_IPC_LPID, "1"}, {UL_IPC_LRPID, "1"},
        };
#define PROJECTIONS(name, row, columns, field, expected)                     \
        table_projection_check(name, address_of row, columns,               \
                               array_count(columns), field, expected,       \
                               array_count(expected))
        PROJECTIONS("lsns", ns, ul_lsns_columns, ul_lsns_table_field, ns_expected);
        PROJECTIONS("lsfd", fd, ul_lsfd_columns, ul_lsfd_field, fd_expected);
        PROJECTIONS("lslocks", lock, ul_lslocks_columns, ul_lslocks_field, lock_expected);
        PROJECTIONS("wipefs", signature, ul_wipefs_columns, ul_wipefs_field, signature_expected);
        PROJECTIONS("lsblk", device, ul_lsblk_columns, ul_lsblk_field, device_expected);
        PROJECTIONS("ipc", ipc, ul_ipc_columns, ul_ipc_field, ipc_expected);
#undef PROJECTIONS
}

/* Build the legacy prlimit projection independently: width comes from the
   complete resource set, duplicate selected columns remain significant,
   only spaces need escaping in these fixed metadata/decimal fields. */
static fn table_limit_checks(void)
{
        ul_limit_row rows[UL_RESOURCES];
        p8 scratch[96], expected[8192], selected[5];
        static const p64 values[] = {0, 1, 999, 10000000000ull,
                                     UL_LIMIT_INFINITE - 1, UL_LIMIT_INFINITE};
        for (positive shape = 0; shape < 32; shape++)
        {
                positive count = shape % (UL_RESOURCES + 1);
                for (positive row = 0; row < count; row++)
                        rows[row] = (ul_limit_row){ul_resources + row,
                            {values[(row + shape) % array_count(values)],
                             values[(row * 3 + shape) % array_count(values)]}};
                for (positive field = 0; field < array_count(selected); field++)
                        selected[field] = (p8)((field * (shape % 5) + shape) % 5);
                for (positive mode = 0; mode < 4; mode++)
                {
                        bool headings = mode & 1, raw = mode & 2;
                        positive widths[5] = {0}, used = 0;
                        for (positive field = 0; field < 5; field++)
                        {
                                if (headings)
                                        widths[field] = string_length(ul_limit_definitions[field].heading);
                                for (positive row = 0; row < count; row++)
                                        widths[field] = max(widths[field],
                                            string_length(ul_limit_field(rows + row, field, scratch)));
                        }
                        for (positive row = 0; count && row < count + headings; row++)
                        {
                                bool heading = headings && !row;
                                for (positive field = 0; field < 5; field++)
                                {
                                        p8 column = selected[field];
                                        string_address value = heading
                                            ? ul_limit_definitions[column].heading
                                            : ul_limit_field(rows + row - headings, column, scratch);
                                        positive length = string_length(value);
                                        bool number = column == UL_LIMIT_SOFT || column == UL_LIMIT_HARD;
                                        positive pad = raw || (field == 4 && !number) ? 0
                                            : widths[column] - length;
                                        if (field) expected[used++] = ' ';
                                        if (number) while (pad) expected[used++] = ' ', pad--;
                                        for (positive at = 0; at < length; at++)
                                                if (raw && value[at] == ' ')
                                                {
                                                        memory_copy_apart(expected + used, "\\x20", 4);
                                                        used += 4;
                                                }
                                                else expected[used++] = value[at];
                                        while (pad) expected[used++] = ' ', pad--;
                                }
                                expected[used++] = '\n';
                        }
                        table_reset();
                        ul_limit_table(rows, count, selected, 5, headings, raw);
                        check("prlimit shared rendering exact bytes", !table_overflow &&
                              table_used == used && !memory_compare(table_output, expected, used));
                }
        }
        positive count = 0;
        check("prlimit preserves duplicate selected columns",
              ul_limit_columns("soft,SOFT,units", selected, &count) &&
              count == 3 && selected[0] == UL_LIMIT_SOFT &&
              selected[1] == UL_LIMIT_SOFT && selected[2] == UL_LIMIT_UNITS);
        check("prlimit rejects appended defaults syntax",
              !ul_limit_columns("+SOFT", selected, &count));
}

static string_address table_pair_field(address_any opaque, p8 column,
                                       p8 address_to scratch)
{
        (void)scratch;
        return ((string_address address_to)opaque)[column];
}

static fn table_printable_checks(void)
{
        p8 value[97], expected[8192], selected[] = {0, 1, 0};
        string_address row[] = {value, "one\ntwo\n"};
        ul_table_column columns[] = {
            {"value", "VALUE", 0, false, UL_TABLE_STRING},
            {"lines", "LINES", 0, false, UL_TABLE_STRING, .multiline=true},
        };
        for (positive length = 0; length <= 95; length++)
        {
                for (positive at = 0; at < length; at++) value[at] = (p8)(' ' + at);
                value[length] = 0;
                for (positive mode = 0; mode < 8; mode++)
                {
                        columns[0].number = (mode & 4) != 0;
                        positive wanted = 0;
                        for (positive fast = 0; fast < 2; fast++)
                        {
                                columns[0].printable = fast;
                                table_reset();
                                ul_table_out(row, sizeof(row), 1, columns, 2,
                                             selected, 3, mode & 1, mode & 2, table_pair_field);
                                if (!fast)
                                {
                                        wanted = table_used;
                                        memory_copy_apart(expected, table_output, wanted);
                                }
                                else
                                        check("printable fields match escaped path with multiline neighbors",
                                              !table_overflow && table_used == wanted &&
                                              !memory_compare(table_output, expected, wanted));
                        }
                }
        }
}

static fn table_ipcs_checks(void)
{
        ul_ipc_row rows[3] = {
            {.key=1, .id=11, .mode=0600, .uid=4294967295u, .size=17,
             .count=2, .type=UL_IPC_MESSAGE},
            {.key=2, .id=22, .mode=0600, .uid=4294967295u, .size=4096,
             .count=3, .type=UL_IPC_SHARED},
            {.key=3, .id=33, .mode=0600, .uid=4294967295u,
             .count=4, .type=UL_IPC_SEMAPHORE},
        };
        static const string_address expected[] = {
            "\n------ Message Queues --------\n"
            "key        msqid      owner      perms      used-bytes   messages\n"
            "0x00000001 11         4294967295 600        17           2\n",
            "\n------ Shared Memory Segments --------\n"
            "key        shmid      owner      perms      bytes      nattch     status\n"
            "0x00000002 22         4294967295 600        4096       3          \n",
            "\n------ Semaphore Arrays --------\n"
            "key        semid      owner      perms      nsems\n"
            "0x00000003 33         4294967295 600        4\n",
        };
        static const string_address empty[] = {
            "\n------ Message Queues --------\n"
            "key        msqid      owner      perms      used-bytes   messages    \n",
            "\n------ Shared Memory Segments --------\n"
            "key        shmid      owner      perms      bytes      nattch     status      \n",
            "\n------ Semaphore Arrays --------\n"
            "key        semid      owner      perms      nsems     \n",
        };
        ul_ipc.rows = rows;
        ul_ipc_bytes = ul_ipc_numeric_permissions = true;
        // Revisit each resource after changing the ID heading for another.
        for (positive visit = 0; visit < 18; visit++)
        {
                p8 type = (p8)(visit % 3);
                ul_ipc.count = visit < 9 ? array_count(rows) : 0;
                string_address want = visit < 9 ? expected[type] : empty[type];
                table_reset();
                ul_ipcs_table(type);
                check("legacy IPC shared field IDs", !table_overflow &&
                      table_used == string_length(want) &&
                      !memory_compare(table_output, want, table_used));
        }
}

b32 main(void)
{
        name_list_checks();
        table_checks();
        table_projection_checks();
        table_limit_checks();
        table_printable_checks();
        table_ipcs_checks();
#ifdef TABLE_BENCHMARK
        if (!failures)
        {
                static p8 text[2049];
                for (positive shape = 0; shape < 3; shape++)
                {
                        for (positive at = 0; at < sizeof(text) - 1; at++)
                                text[at] = shape == 1 && at % 64 == 63 ? '\n' :
                                           shape == 2 && at % 64 == 63 ? '\t' : 'a';
                        for (positive mode = 0; mode < 3; mode++)
                        {
                                p64 start = get_cpu_time();
                                for (positive round = 0; round < 4096; round++)
                                        table_render(text, mode);
                                p64 elapsed = get_cpu_time() - start;
                                string_format(log, "table shape=%p mode=%p ticks=%p writes=%p\n",
                                              shape, mode, (positive)elapsed, table_calls);
                        }
                }
        }
#endif
        return test_report(null);
}
