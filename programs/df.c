#include "../src/sh/file.c"

/*
        df [-h] [PATH...]

        The mounted filesystems come from /proc/mounts, because the kernel is
        the only thing that knows what is mounted and this is where it says
        so. A filesystem with no blocks at all is one of the kernel's own
        bookkeeping mounts and is left out, the way df has always left it out.

        The whole table is measured before any of it is written: each column
        ends up as wide as the widest thing under it and no wider, which is
        why the file is read twice and why the second pass prints.
*/
#define DF_TEXT (1 << 18)

static p8 df_text[DF_TEXT];
static bool df_human;

static positive df_device_width;
static positive df_blocks_width;
static positive df_used_width;
static positive df_free_width;

// The kernel counts in whatever unit the filesystem uses; df has always
// reported in 1024 byte ones, and rounds a part of one up to a whole.
static positive df_amount(p8 address_to into, p64 blocks, p64 size)
{
        p64 bytes = blocks * size;

        if (!df_human)
                return file_digits(into, (bytes + 1023) / 1024);

        positive divisor = 1;
        positive unit = 0;
        p8 units[8] = "BKMGTPEZ";

        while (bytes / divisor >= 1024 && unit < 7)
        {
                divisor *= 1024;
                unit++;
        }

        if (unit == 0)
                return file_digits(into, bytes);

        positive whole = (bytes + divisor - 1) / divisor;
        positive length;

        if (whole >= 10)
                length = file_digits(into, whole);
        else
        {
                positive quotient = bytes / divisor;
                positive leftover = bytes % divisor;
                positive tenths = quotient * 10 + (leftover * 10 + divisor - 1) / divisor;

                length = file_digits(into, tenths / 10);
                into[length++] = '.';
                length += file_digits(into + length, tenths % 10);
        }

        into[length++] = units[unit];
        into[length] = end;

        return length;
}

static fn df_column(p8 address_to text, positive width)
{
        file_text_aligned(log, text, width);
        log(" ", 1);
}

static fn df_row(string_address device, string_address where,
                 file_mount_facts address_to facts)
{
        p64 size = (p64)(facts->fragment_size ? facts->fragment_size : facts->block_size);
        p64 used = facts->blocks - facts->blocks_free;
        p8 text[64];

        file_text_padded(log, device, df_device_width);
        log(" ", 1);

        df_amount(text, facts->blocks, size);
        df_column(text, df_blocks_width);

        df_amount(text, used, size);
        df_column(text, df_used_width);

        df_amount(text, facts->blocks_available, size);
        df_column(text, df_free_width);

        p64 wanted = used + facts->blocks_available;
        positive percent = wanted ? (positive)((used * 100 + wanted - 1) / wanted) : 0;

        file_number_padded(log, percent, 3);
        log("% ", 0);
        log(where, 0);
        log("\n", 1);
}

// Every line of /proc/mounts is device, mount point, type, options; the first
// two are all df needs and both may carry \040 where a space was.
static positive df_field(p8 address_to text, positive at, p8 address_to into,
                         positive limit)
{
        positive filled = 0;

        while (text[at] == ' ')
                at++;

        while (text[at] && text[at] != ' ' && text[at] != '\n')
        {
                if (text[at] == '\\' && text[at + 1] == '0' && text[at + 2] == '4' &&
                    text[at + 3] == '0')
                {
                        if (filled + 1 < limit)
                                into[filled++] = ' ';

                        at += 4;
                        continue;
                }

                if (filled + 1 < limit)
                        into[filled++] = text[at];

                at++;
        }

        into[filled] = end;

        return at;
}

b32 main()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "hkPT", address_of first);

        df_human = (flags & FILE_FLAG('h')) != 0;

        if (file_slurp((string_address) "/proc/mounts", df_text, DF_TEXT) <= 0 &&
            file_slurp((string_address) "/proc/self/mounts", df_text, DF_TEXT) <= 0)
        {
                file_fail("df: cannot read /proc/mounts\n", 0);
                return 1;
        }

        string_address blocks_heading = df_human ? (string_address) "Size"
                                                 : (string_address) "1K-blocks";
        string_address free_heading = df_human ? (string_address) "Avail"
                                               : (string_address) "Available";

        /*
                Each column is the widest of three things: a floor the column
                has whatever is in it, the heading, and the widest value. The
                floors are what keep "df /tmp" and "df" lining their tables up
                the same way, and they are the system's own: fourteen for the
                filesystem, five for each amount, four for the percentage.
        */
        df_device_width = 14;
        df_blocks_width = 5;
        df_used_width = 5;
        df_free_width = 5;

        if (string_length(blocks_heading) > df_blocks_width)
                df_blocks_width = string_length(blocks_heading);

        if (string_length(free_heading) > df_free_width)
                df_free_width = string_length(free_heading);

        bool filtering = first < count;

        for (positive pass = 0; pass < 2; pass++)
        {
                if (pass == 1)
                {
                        file_text_padded(log, (string_address) "Filesystem", df_device_width);
                        log(" ", 1);
                        df_column(blocks_heading, df_blocks_width);
                        df_column((string_address) "Used", df_used_width);
                        df_column(free_heading, df_free_width);
                        log("Use% Mounted on\n", 0);
                }

                positive at = 0;

                while (df_text[at])
                {
                        p8 device[FILE_PATH_MAX];
                        p8 where[FILE_PATH_MAX];
                        p8 text[64];

                        at = df_field(df_text, at, device, FILE_PATH_MAX);
                        at = df_field(df_text, at, where, FILE_PATH_MAX);

                        while (df_text[at] && df_text[at] != '\n')
                                at++;

                        if (df_text[at])
                                at++;

                        if (!string_get(where))
                                continue;

                        file_mount_facts facts;

                        memory_fill(address_of facts, 0, sizeof(facts));

                        if (system_call_2(syscall(statfs), (positive)where,
                                          (positive)address_of facts) < 0)
                                continue;

                        if (facts.blocks == 0)
                                continue;

                        if (filtering)
                        {
                                bool matched = false;

                                for (positive i = first; i < count; i++)
                                {
                                        p8 wanted[FILE_PATH_MAX];

                                        if (!file_real(program_argument((b32)i), wanted))
                                                continue;

                                        file_mount_facts theirs;

                                        memory_fill(address_of theirs, 0, sizeof(theirs));

                                        if (system_call_2(syscall(statfs), (positive)wanted,
                                                          (positive)address_of theirs) < 0)
                                                continue;

                                        if (theirs.identity[0] == facts.identity[0] &&
                                            theirs.identity[1] == facts.identity[1] &&
                                            theirs.blocks == facts.blocks)
                                                matched = true;
                                }

                                if (!matched)
                                        continue;
                        }

                        if (pass == 1)
                        {
                                df_row(device, where, address_of facts);
                                continue;
                        }

                        p64 size = (p64)(facts.fragment_size ? facts.fragment_size
                                                             : facts.block_size);
                        p64 used = facts.blocks - facts.blocks_free;

                        if (string_length(device) > df_device_width)
                                df_device_width = string_length(device);

                        if (df_amount(text, facts.blocks, size) > df_blocks_width)
                                df_blocks_width = string_length(text);

                        if (df_amount(text, used, size) > df_used_width)
                                df_used_width = string_length(text);

                        if (df_amount(text, facts.blocks_available, size) > df_free_width)
                                df_free_width = string_length(text);
                }
        }

        log_flush();

        return 0;
}
