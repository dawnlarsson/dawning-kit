#include "../compiler_memory.c"

/* Defined by text.c later in the multicall translation unit. */
static address_any text_arena_take(positive bytes);
static positive text_arena_used;

/*
        What the file utilities share.

        Every one of them is its own program under programs/, because a
        utility that is a program can be exec'd by any shell and replaced one
        at a time; a utility that is a builtin can only ever be ours. What
        they have in common is here so that the twenty five of them are thin.

        Nothing below allocates. Every buffer is a fixed one whose ceiling is
        named, and the walkers carry their depth so a directory that links
        into itself stops instead of taking the stack down.
*/

#define FILE_PATH_MAX 4096
#define FILE_NAME_MAX 256

// Every recursive walker here spends one open descriptor and one frame with a
// getdents block in it per level, so this is what a symlink loop costs before
// it is refused.
#define FILE_MAX_DEPTH 32

#define FILE_BLOCK 4096

#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_FOLLOW 0x400
#define AT_NO_AUTOMOUNT 0x800
#define AT_EMPTY_PATH 0x1000

#define ERROR_NOT_PERMITTED 1
#define ERROR_NO_ENTRY 2
#define ERROR_ACCESS 13
#define ERROR_ARGUMENT_LIST 7
#define ERROR_EXISTS 17
#define ERROR_CROSS_DEVICE 18
#define ERROR_NOT_DIRECTORY 20
#define ERROR_IS_DIRECTORY 21
#define ERROR_INVALID 22
#define ERROR_NOT_EMPTY 39

#define STATX_BASIC 0x7ff
#define STATX_BIRTH 0x800

// The basic set stops short of the creation time, and a filesystem that does
// not keep one says so by leaving its bit out of the mask it answers with.
#define STATX_WANTED (STATX_BASIC | STATX_BIRTH)

#define MODE_FORMAT 0170000
#define MODE_SOCKET 0140000
#define MODE_LINK 0120000
#define MODE_FILE 0100000
#define MODE_BLOCK 0060000
#define MODE_DIRECTORY 0040000
#define MODE_CHARACTER 0020000
#define MODE_PIPE 0010000

#define MODE_SET_USER 04000
#define MODE_SET_GROUP 02000
#define MODE_STICKY 01000

#define UTIME_NOW 0x3fffffff
#define UTIME_OMIT 0x3ffffffe

typedef struct
{
        b64 seconds;
        p32 nanoseconds;
        b32 reserved;
} file_moment;

/*
        statx rather than fstat, for the reason builtin.c gives: the kernel's
        struct stat is a different shape on x86_64, arm64 and riscv64, and
        statx has one layout everywhere. Spelled out to the end of the device
        numbers because ls, stat and touch all want the timestamps, and the
        kernel writes all 256 bytes whatever is asked for.
*/
typedef struct
{
        p32 mask;
        p32 blocksize;
        p64 attributes;
        p32 hard_links;
        p32 owner;
        p32 group;
        p16 mode;
        p16 spare;
        p64 inode;
        p64 size;
        p64 blocks;
        p64 attributes_mask;
        file_moment accessed;
        file_moment created;
        file_moment changed;
        file_moment modified;
        p32 rdev_major;
        p32 rdev_minor;
        p32 device_major;
        p32 device_minor;
        p64 mount_id;
        p8 remainder[104];
} file_facts;

_Static_assert(sizeof(file_facts) == 256, "statx writes 256 bytes");

typedef struct
{
        b64 type;
        b64 block_size;
        p64 blocks;
        p64 blocks_free;
        p64 blocks_available;
        p64 files;
        p64 files_free;
        b32 identity[2];
        b64 name_length;
        b64 fragment_size;
        b64 flags;
        b64 spare[4];
} file_mount_facts;

typedef struct
{
        p8 system[65];
        p8 node[65];
        p8 release[65];
        p8 version[65];
        p8 machine[65];
        p8 domain[65];
} file_machine;

// Output ----------------------------------------------------

/*
        Everything here writes with a length of zero and lets the writer
        measure, which is what a literal of unknown length needs anyway.
*/
#define file_fail log_error

fn file_line(string_address text)
{
        log(text, 0);
        log("\n", 1);
}

// What -z asks for, in the tools that have it: the same answer ended with a
// NUL, so that a name with a newline in it survives being read back.
fn file_written(string_address text, bool zero)
{
        log(text, 0);
        log(zero ? "\0" : "\n", 1);
}

// Numbers ---------------------------------------------------

// Modes -----------------------------------------------------

p8 file_kind_letter(positive mode)
{
        positive kind = mode & MODE_FORMAT;

        if (kind == MODE_DIRECTORY)
                return 'd';

        if (kind == MODE_LINK)
                return 'l';

        if (kind == MODE_CHARACTER)
                return 'c';

        if (kind == MODE_BLOCK)
                return 'b';

        if (kind == MODE_PIPE)
                return 'p';

        if (kind == MODE_SOCKET)
                return 's';

        return '-';
}

string_address file_kind_name(positive mode)
{
        positive kind = mode & MODE_FORMAT;

        if (kind == MODE_DIRECTORY)
                return (string_address) "directory";

        if (kind == MODE_LINK)
                return (string_address) "symbolic link";

        if (kind == MODE_CHARACTER)
                return (string_address) "character special file";

        if (kind == MODE_BLOCK)
                return (string_address) "block special file";

        if (kind == MODE_PIPE)
                return (string_address) "fifo";

        if (kind == MODE_SOCKET)
                return (string_address) "socket";

        return (string_address) "regular file";
}

fn file_mode_letters(p8 address_to into, positive mode)
{
        p8 pattern[10] = "rwxrwxrwx";

        into[0] = file_kind_letter(mode);

        for (positive i = 0; i < 9; i++)
                into[1 + i] = (mode & ((positive)1 << (8 - i))) ? pattern[i] : '-';

        if (mode & MODE_SET_USER)
                into[3] = (mode & 0100) ? 's' : 'S';

        if (mode & MODE_SET_GROUP)
                into[6] = (mode & 0010) ? 's' : 'S';

        if (mode & MODE_STICKY)
                into[9] = (mode & 0001) ? 't' : 'T';

        into[10] = end;
}

/*
        chmod's argument, in either spelling. Octal replaces the whole set of
        bits; symbolic is read against what the file already is, which is why
        the current mode comes in rather than being looked up here.

        The X of "a+X" is the one that needs to know whether it is a
        directory: it grants execute only where something already executes, or
        where the thing is a directory.
*/
bool file_mode_of(string_address specification, positive current, bool directory,
                  positive address_to result)
{
        positive mode = current & 07777;

        if (string_get(specification) >= '0' && string_get(specification) <= '7')
        {
                positive used;
                positive value = string_digits_octal_max(
                    specification, (positive)-1, address_of used);
                string_address step = specification + used;

                if (string_get(step))
                        return false;

                address_to result = value & 07777;
                return true;
        }

        string_address step = specification;

        while (string_get(step))
        {
                positive who = 0;
                bool named = false;

                while (string_is(step, 'u') || string_is(step, 'g') ||
                       string_is(step, 'o') || string_is(step, 'a'))
                {
                        if (string_is(step, 'u'))
                                who |= 04700;

                        if (string_is(step, 'g'))
                                who |= 02070;

                        if (string_is(step, 'o'))
                                who |= 00007;

                        if (string_is(step, 'a'))
                                who |= 07777;

                        named = true;
                        step++;
                }

                // No who at all is "a", except that the umask would apply --
                // and with no umask read here, all of them is what is meant.
                if (!named)
                        who = 07777;

                if (!string_is(step, '+') && !string_is(step, '-') && !string_is(step, '='))
                        return false;

                while (string_is(step, '+') || string_is(step, '-') || string_is(step, '='))
                {
                        p8 action = string_get(step);
                        positive bits = 0;

                        step++;

                        while (string_get(step) && !string_is(step, ',') &&
                               !string_is(step, '+') && !string_is(step, '-') &&
                               !string_is(step, '='))
                        {
                                p8 letter = string_get(step);

                                if (letter == 'r')
                                        bits |= 00444;
                                else if (letter == 'w')
                                        bits |= 00222;
                                else if (letter == 'x')
                                        bits |= 00111;
                                else if (letter == 'X')
                                {
                                        if (directory || (mode & 00111))
                                                bits |= 00111;
                                }
                                else if (letter == 's')
                                        bits |= 06000;
                                else if (letter == 't')
                                        bits |= 01000;
                                else if (letter == 'u')
                                        bits |= ((mode & 00700) >> 6) * 00111;
                                else if (letter == 'g')
                                        bits |= ((mode & 00070) >> 3) * 00111;
                                else if (letter == 'o')
                                        bits |= (mode & 00007) * 00111;
                                else
                                        return false;

                                step++;
                        }

                        bits &= who;

                        if (action == '+')
                                mode |= bits;
                        else if (action == '-')
                                mode &= ~bits;
                        else
                                mode = (mode & ~who) | bits;
                }

                if (string_is(step, ','))
                        step++;
                else if (string_get(step))
                        return false;
        }

        address_to result = mode & 07777;

        return true;
}

// Looking at files ------------------------------------------

bool file_look(bipolar directory, string_address path, positive flags,
               file_facts address_to out)
{
        memory_fill(out, 0, sizeof(file_facts));

        return system_call_5(syscall(statx), directory, (positive)path,
                             flags | AT_NO_AUTOMOUNT, STATX_WANTED,
                             (positive)out) == 0;
}

bool file_look_at(string_address path, file_facts address_to out)
{
        return file_look(AT_FDCWD, path, 0, out);
}

bool file_look_link(string_address path, file_facts address_to out)
{
        return file_look(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, out);
}

bool file_is_directory(bipolar directory, string_address path)
{
        file_facts facts;

        if (!file_look(directory, path, AT_SYMLINK_NOFOLLOW, address_of facts))
                return false;

        return (facts.mode & MODE_FORMAT) == MODE_DIRECTORY;
}

bool file_is_directory_through(string_address path)
{
        file_facts facts;

        if (!file_look(AT_FDCWD, path, 0, address_of facts))
                return false;

        return (facts.mode & MODE_FORMAT) == MODE_DIRECTORY;
}

bool file_exists(bipolar directory, string_address path)
{
        file_facts facts;

        return file_look(directory, path, AT_SYMLINK_NOFOLLOW, address_of facts);
}

static bool file_same_identity(file_facts address_to one, file_facts address_to two)
{
        return one->inode == two->inode && one->device_major == two->device_major &&
               one->device_minor == two->device_minor;
}

// Paths -----------------------------------------------------

static string_address file_last_component(string_address path)
{
        string_address last = string_last_of(path, '/');

        return last ? last + 1 : path;
}

bipolar file_link_text(string_address path, p8 address_to into, positive limit)
{
        bipolar length = system_call_4(syscall(readlinkat), AT_FDCWD,
                                       (positive)path, (positive)into, limit - 1);

        if (length < 0)
                return length;

        into[length] = end;

        return length;
}

/*
        realpath without /proc: every component is resolved in turn, and a
        symlink puts its own text back at the front of what is left to
        resolve. The hop count is what ends a loop of links pointing at each
        other, since walking one is not what makes the path longer.

        Not following the links is what realpath -s asks for: the dots are
        still worked out, so what comes back is an absolute path, but every
        name in it is the name that was written and not what it points at.
*/
bool file_resolve(string_address path, p8 address_to into, bool follow)
{
        p8 rest[FILE_PATH_MAX];
        p8 link[FILE_PATH_MAX];
        p8 merged[FILE_PATH_MAX];
        positive at = 0;
        positive length = 0;
        positive hops = 0;

        if (string_is(path, end) || string_length(path) >= FILE_PATH_MAX)
                return false;

        if (string_is(path, '/'))
        {
                into[0] = '/';
                length = 1;
        }
        else
        {
                string_address here = working_directory_get();

                length = string_length_max(here, FILE_PATH_MAX - 1);
                memory_copy_fast(into, here, length);

                if (length == 0)
                {
                        into[0] = '/';
                        length = 1;
                }
        }

        into[length] = end;

        string_copy_max_end(rest, path, FILE_PATH_MAX - 1);

        while (rest[at])
        {
                while (rest[at] == '/')
                        at++;

                if (!rest[at])
                        break;

                positive start = at;

                while (rest[at] && rest[at] != '/')
                        at++;

                positive piece = at - start;

                if (piece == 1 && rest[start] == '.')
                        continue;

                if (piece == 2 && rest[start] == '.' && rest[start + 1] == '.')
                {
                        while (length > 1 && into[length - 1] != '/')
                                length--;

                        if (length > 1)
                                length--;

                        into[length] = end;
                        continue;
                }

                if (length + piece + 2 >= FILE_PATH_MAX)
                        return false;

                if (length > 1)
                        into[length++] = '/';

                length = (positive)(memory_copy_fast_end(
                    into + length, rest + start, piece) - into);

                if (!follow)
                        continue;

                bipolar seen = system_call_4(syscall(readlinkat), AT_FDCWD,
                                             (positive)into, (positive)link,
                                             FILE_PATH_MAX - 1);

                if (seen <= 0)
                        continue;

                if (++hops > 40)
                        return false;

                link[seen] = end;

                positive fill = (positive)seen;

                memory_copy_fast(merged, link, fill);

                if (rest[at] && fill + 1 < FILE_PATH_MAX)
                        merged[fill++] = '/';

                positive left = string_length_max(rest + at, FILE_PATH_MAX - 1 - fill);

                fill = (positive)(memory_copy_fast_end(
                    merged + fill, rest + at, left) - merged);

                memory_copy_fast(rest, merged, fill + 1);

                at = 0;

                if (link[0] == '/')
                {
                        into[0] = '/';
                        length = 1;
                }
                else
                {
                        while (length > 1 && into[length - 1] != '/')
                                length--;

                        if (length > 1)
                                length--;
                }

                into[length] = end;
        }

        if (length == 0)
        {
                into[0] = '/';
                length = 1;
        }

        into[length] = end;

        return true;
}

bool file_real(string_address path, p8 address_to into)
{
        return file_resolve(path, into, true);
}

// Reading a small file whole is shared Linux assembly in platform/linux.inc.

// Users and groups ------------------------------------------

// Enough for a passwd or group file on a machine that is not a directory
// server; past this the numeric id is printed, which is what the lookup falls
// back to anyway.
#define FILE_ACCOUNTS_MAX 65536

static p8 file_password_store[FILE_ACCOUNTS_MAX];
static p8 file_group_store[FILE_ACCOUNTS_MAX];
static bool file_password_read;
static bool file_group_read;

// ls -l asks for a name per entry, so the file is read once and kept rather
// than opened again for every line of a listing.
p8 address_to file_password_text()
{
        if (!file_password_read)
        {
                file_password_read = true;

                if (file_slurp((string_address) "/etc/passwd", file_password_store,
                               FILE_ACCOUNTS_MAX) <= 0)
                        file_password_store[0] = end;
        }

        return file_password_store;
}

p8 address_to file_group_text()
{
        if (!file_group_read)
        {
                file_group_read = true;

                if (file_slurp((string_address) "/etc/group", file_group_store,
                               FILE_ACCOUNTS_MAX) <= 0)
                        file_group_store[0] = end;
        }

        return file_group_store;
}

/*
        colon separated records, name first and the numeric id in the field
        given. /etc/passwd and /etc/group agree on both of those, so one
        reader serves both.
*/
bool file_account_name(p8 address_to text, positive wanted, positive field,
                       p8 address_to into, positive limit)
{
        positive at = 0;

        while (text[at])
        {
                positive line = at;

                at = (positive)(string_first_of_or_end(text + at, '\n') - text);

                positive stop = at;

                if (text[at])
                        at++;

                positive name_start = line;
                positive name_stop = line;

                while (name_stop < stop && text[name_stop] != ':')
                        name_stop++;

                positive column = 0;
                positive step = name_stop;
                positive value_start = 0;
                positive value_stop = 0;

                while (step < stop && column < field)
                {
                        step++;
                        column++;
                        value_start = step;

                        while (step < stop && text[step] != ':')
                                step++;

                        value_stop = step;
                }

                if (column != field)
                        continue;

                positive taken;
                positive value = string_digits_max(text + value_start,
                                                   value_stop - value_start,
                                                   address_of taken);

                if (taken != value_stop - value_start || !taken || value != wanted)
                        continue;

                positive found = name_stop - name_start;

                if (found > limit - 1)
                        found = limit - 1;

                memory_copy_fast_end(into, text + name_start, found);

                return true;
        }

        return false;
}

bipolar file_account_id(p8 address_to text, string_address name, positive field)
{
        positive wanted = string_length(name);
        positive at = 0;

        while (text[at])
        {
                positive line = at;

                at = (positive)(string_first_of_or_end(text + at, '\n') - text);

                positive stop = at;

                if (text[at])
                        at++;

                positive name_stop = line;

                while (name_stop < stop && text[name_stop] != ':')
                        name_stop++;

                if (name_stop - line != wanted)
                        continue;

                if (memory_compare(text + line, name, wanted))
                        continue;

                positive column = 0;
                positive step = name_stop;
                positive value_start = 0;
                positive value_stop = 0;

                while (step < stop && column < field)
                {
                        step++;
                        column++;
                        value_start = step;

                        while (step < stop && text[step] != ':')
                                step++;

                        value_stop = step;
                }

                if (column != field)
                        return -1;

                positive taken;
                positive value = string_digits_max(text + value_start,
                                                   value_stop - value_start,
                                                   address_of taken);

                if (taken != value_stop - value_start)
                        return -1;

                return (bipolar)value;
        }

        return -1;
}

// One remembered answer each, because a directory listing asks the same
// question once per entry and almost every entry gives the same id.
static positive file_user_seen = (positive)-1;
static p8 file_user_seen_name[FILE_NAME_MAX];
static bool file_user_seen_known;

static positive file_group_seen = (positive)-1;
static p8 file_group_seen_name[FILE_NAME_MAX];
static bool file_group_seen_known;

bool file_user_name(positive id, p8 address_to into, positive limit)
{
        if (!limit)
                return false;

        if (id != file_user_seen)
        {
                file_user_seen = id;
                file_user_seen_known = file_account_name(file_password_text(), id, 2,
                                                         file_user_seen_name,
                                                         FILE_NAME_MAX);
        }

        if (!file_user_seen_known)
                return false;

        string_copy_max_end(into, file_user_seen_name, limit - 1);

        return true;
}

bool file_group_name(positive id, p8 address_to into, positive limit)
{
        if (!limit)
                return false;

        if (id != file_group_seen)
        {
                file_group_seen = id;
                file_group_seen_known = file_account_name(file_group_text(), id, 2,
                                                          file_group_seen_name,
                                                          FILE_NAME_MAX);
        }

        if (!file_group_seen_known)
                return false;

        string_copy_max_end(into, file_group_seen_name, limit - 1);

        return true;
}

bipolar file_user_id(string_address name)
{
        return file_account_id(file_password_text(), name, 2);
}

bipolar file_group_id(string_address name)
{
        return file_account_id(file_group_text(), name, 2);
}

// Time ------------------------------------------------------

/*
        Days since the epoch turned back into a date, by the standard shift of
        the year to start in March: with the leap day last, the month lengths
        repeat on a 153 day pattern and the whole thing is arithmetic with no
        table and no loop.

        Everything printed here is UTC. Converting to the machine's own zone
        means reading and understanding /usr/share/zoneinfo, which is a
        binary format none of the rest of this tree has any use for.
*/
fn file_civil(b64 days, b64 address_to year, positive address_to month,
              positive address_to day)
{
        b64 shifted = days + 719468;
        b64 era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
        b64 of_era = shifted - era * 146097;
        b64 year_of_era = (of_era - of_era / 1460 + of_era / 36524 - of_era / 146096) / 365;
        b64 value = year_of_era + era * 400;
        b64 of_year = of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
        b64 pattern = (5 * of_year + 2) / 153;

        address_to day = (positive)(of_year - (153 * pattern + 2) / 5 + 1);
        address_to month = (positive)(pattern + (pattern < 10 ? 3 : -9));
        address_to year = value + (address_to month <= 2 ? 1 : 0);
}

fn file_split_moment(b64 seconds, b64 address_to year, positive address_to month,
                     positive address_to day, positive address_to hour,
                     positive address_to minute, positive address_to second)
{
        b64 days = seconds / 86400;
        b64 rest = seconds % 86400;

        if (rest < 0)
        {
                rest += 86400;
                days--;
        }

        file_civil(days, year, month, day);

        address_to hour = (positive)(rest / 3600);
        address_to minute = (positive)((rest / 60) % 60);
        address_to second = (positive)(rest % 60);
}

fn file_two(writer write, positive value)
{
        p8 pair[2];
        positive length = positive_into_pair(pair, value);

        // Civil month/day/hour/minute/second fields are all below 100, so the
        // historical two-byte field never relied on its implicit modulo 100.
        write(pair, length);
}

fn file_stamp(writer write, b64 seconds, positive nanoseconds)
{
        b64 year;
        positive month, day, hour, minute, second;

        file_split_moment(seconds, address_of year, address_of month, address_of day,
                          address_of hour, address_of minute, address_of second);

        positive_to_string(write, (positive)year);
        write("-", 1);
        file_two(write, month);
        write("-", 1);
        file_two(write, day);
        write(" ", 1);
        file_two(write, hour);
        write(":", 1);
        file_two(write, minute);
        write(":", 1);
        file_two(write, second);
        write(".", 1);

        p8 fraction[9];
        positive fraction_length = positive_into_padded(fraction, nanoseconds, 9, '0');

        write(fraction, fraction_length);
        write(" +0000", 6);
}

b64 file_now()
{
        p64 wall[2] = {0, 0};

        system_call_2(syscall(clock_gettime), 0, (positive)wall);

        return (b64)wall[0];
}

/*
        The date a listing puts beside a name, which is two different dates.
        Anything within the last half year gets a time of day, because that is
        what is worth knowing about a file written this week; anything older
        gets a year instead, because the hour it was written in six years ago
        is not. Both are five columns wide so the name still lines up.

        The half year is 15778476 seconds, a Gregorian year divided in two,
        which is the same span the system's own ls draws the line at.
*/
fn file_stamp_short(writer write, b64 seconds, b64 now)
{
        p8 months[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        b64 year;
        positive month, day, hour, minute, second;

        file_split_moment(seconds, address_of year, address_of month, address_of day,
                          address_of hour, address_of minute, address_of second);

        write(months[month - 1], 3);
        write(" ", 1);
        positive_to_padded(write, day, 2, ' ', 0);
        write(" ", 1);

        bool recent = seconds <= now + 3600 && seconds > now - 15778476;

        if (!recent)
                return positive_to_padded(write, (positive)year, 5, ' ', 0);

        file_two(write, hour);
        write(":", 1);
        file_two(write, minute);
}

// Patterns --------------------------------------------------

/*
        A written date read into a number of seconds since the epoch.

        What is understood is written out here and nothing else is guessed at,
        because a date read as something near what it says is worse than one
        that would not read at all:

          @SECONDS                  on its own, and a - in front of the number
          YYYY-MM-DD                midnight on that day, month and day
                                    either width, a two digit year 69 to 99
                                    in the nineteen hundreds and 00 to 68 in
                                    the two thousands
          HH:MM[:SS]                that time, on whatever day is in hand
          a T or a space between the two
          now  today  yesterday  tomorrow
          nothing at all           midnight on the day in hand
          [+-]N UNIT               and next UNIT, last UNIT, a bare UNIT
          ...UNIT... ago           turns every displacement in the string round
          UTC  GMT  Z              passed over: everything here is UTC already

        UNIT is sec, min, hour, day, week, fortnight, month or year, with or
        without an s. A month and a year move the calendar rather than the
        clock, so the 31st of January and a month is the 2nd of March, which
        is what the system's own date answers.

        A signed displacement after a clock time is refused. The system's date
        reads the + in "12:00 +1 day" as a timezone of one hour and not as a
        day, and a tool that quietly read it the other way would be an hour
        and a day out with nothing to say so.
*/
typedef struct
{
        string_address name;
        b64 seconds;
        b64 months;
} file_unit;

static const file_unit file_units[] = {
    {(string_address) "sec", 1, 0},
    {(string_address) "secs", 1, 0},
    {(string_address) "second", 1, 0},
    {(string_address) "seconds", 1, 0},
    {(string_address) "min", 60, 0},
    {(string_address) "mins", 60, 0},
    {(string_address) "minute", 60, 0},
    {(string_address) "minutes", 60, 0},
    {(string_address) "hour", 3600, 0},
    {(string_address) "hours", 3600, 0},
    {(string_address) "day", 86400, 0},
    {(string_address) "days", 86400, 0},
    {(string_address) "week", 604800, 0},
    {(string_address) "weeks", 604800, 0},
    {(string_address) "fortnight", 1209600, 0},
    {(string_address) "fortnights", 1209600, 0},
    {(string_address) "month", 0, 1},
    {(string_address) "months", 0, 1},
    {(string_address) "year", 0, 12},
    {(string_address) "years", 0, 12},
    {null, 0, 0},
};

// The inverse of file_civil: a day number from a calendar date, and the same
// arithmetic run backwards. A day past the end of its month runs on into the
// next one, which is what a month added to the 31st has to do.
b64 file_days(b64 year, b64 month, b64 day)
{
        year -= month <= 2;

        b64 era = (year >= 0 ? year : year - 399) / 400;
        b64 of_era = year - era * 400;
        b64 of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        b64 day_of_era = of_year + 365 * of_era + of_era / 4 - of_era / 100;

        return era * 146097 + day_of_era - 719468;
}

// A day written down has to be a day the month has; a day arrived at by
// adding months to another one does not, and rolls into the month after.
static positive file_month_days(b64 year, b64 month)
{
        static const p8 lengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

        if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
                return 29;

        return lengths[month - 1];
}

static bool file_blank(p8 letter)
{
        return letter == ' ' || letter == '\t';
}

static bool file_digit(p8 letter)
{
        return letter >= '0' && letter <= '9';
}

static p8 file_lower(p8 letter)
{
        return letter >= 'A' && letter <= 'Z' ? (p8)(letter + 32) : letter;
}

static bool file_letter(p8 letter)
{
        return file_lower(letter) >= 'a' && file_lower(letter) <= 'z';
}

static bool file_same_word(string_address text, positive length, string_address word)
{
        positive i = 0;

        while (i < length && string_get(word + i) &&
               file_lower(string_get(text + i)) == string_get(word + i))
                i++;

        return i == length && !string_get(word + i);
}

static const file_unit address_to file_unit_of(string_address text, positive length)
{
        for (positive i = 0; file_units[i].name; i++)
                if (file_same_word(text, length, file_units[i].name))
                        return address_of file_units[i];

        return null;
}

static positive file_read_number(string_address text, positive at, b64 address_to out,
                                 positive address_to digits)
{
        positive have;

        address_to out = (b64)string_digits(text + at, address_of have);
        address_to digits = have;

        return at + have;
}

bool file_moment_read(string_address text, b64 now, b64 address_to out)
{
        positive at = 0;

        at += string_span(text + at, string_set_blanks);

        if (string_is(text + at, '@'))
        {
                bool below = string_is(text + at + 1, '-');
                positive digits;
                b64 value;

                at = file_read_number(text, at + 1 + (below ? 1 : 0), address_of value,
                                      address_of digits);

                if (!digits)
                        return false;

                // A fraction of a second is read and dropped, which is what
                // the system's date does with one.
                if (string_is(text + at, '.'))
                {
                        positive spare;
                        b64 ignored;

                        at = file_read_number(text, at + 1, address_of ignored,
                                              address_of spare);
                }

                at += string_span(text + at, string_set_blanks);

                if (string_get(text + at))
                        return false;

                address_to out = below ? -value : value;

                return true;
        }

        b64 year;
        positive month, day, hour, minute, second;

        file_split_moment(now, address_of year, address_of month, address_of day,
                          address_of hour, address_of minute, address_of second);

        bool dated = false;
        bool timed = false;
        bool anything = false;
        b64 shift = 0;
        b64 months = 0;

        // ago turns round the displacement it follows and not the ones before
        // it: "3 hours 2 days ago" is three hours on and two days back, which
        // is what the system's date makes of it.
        b64 recent = 0;
        b64 recent_months = 0;

        while (string_get(text + at))
        {
                at += string_span(text + at, string_set_blanks);

                if (!string_get(text + at))
                        break;

                anything = true;

                b64 sign = 1;
                bool marked = false;

                if (string_is(text + at, '+') || string_is(text + at, '-'))
                {
                        marked = true;
                        sign = string_is(text + at, '-') ? -1 : 1;
                        at++;

                        at += string_span(text + at, string_set_blanks);
                }

                if (file_digit(string_get(text + at)))
                {
                        positive digits;
                        b64 value;

                        at = file_read_number(text, at, address_of value, address_of digits);

                        if (!marked && string_is(text + at, '-'))
                        {
                                positive wide;
                                b64 rest;

                                if (dated)
                                        return false;

                                at = file_read_number(text, at + 1, address_of rest,
                                                      address_of wide);

                                if (!wide || !string_is(text + at, '-'))
                                        return false;

                                b64 which;

                                at = file_read_number(text, at + 1, address_of which,
                                                      address_of wide);

                                if (!wide || rest < 1 || rest > 12 || which < 1)
                                        return false;

                                year = digits <= 2 ? (value <= 68 ? 2000 + value : 1900 + value)
                                                   : value;

                                if (which > file_month_days(year, rest))
                                        return false;

                                month = (positive)rest;
                                day = (positive)which;
                                hour = 0;
                                minute = 0;
                                second = 0;
                                dated = true;

                                if (string_is(text + at, 'T') || string_is(text + at, 't'))
                                        at++;

                                continue;
                        }

                        if (!marked && string_is(text + at, ':'))
                        {
                                positive wide;
                                b64 rest;
                                b64 last = 0;

                                if (timed)
                                        return false;

                                at = file_read_number(text, at + 1, address_of rest,
                                                      address_of wide);

                                if (!wide)
                                        return false;

                                if (string_is(text + at, ':'))
                                {
                                        at = file_read_number(text, at + 1, address_of last,
                                                              address_of wide);

                                        if (!wide)
                                                return false;
                                }

                                if (value > 23 || rest > 59 || last > 60)
                                        return false;

                                hour = (positive)value;
                                minute = (positive)rest;
                                second = (positive)last;
                                timed = true;

                                continue;
                        }

                        at += string_span(text + at, string_set_blanks);

                        positive length = 0;

                        while (file_letter(string_get(text + at + length)))
                                length++;

                        const file_unit address_to unit = file_unit_of(text + at, length);

                        if (!unit || (marked && timed))
                                return false;

                        at += length;
                        recent = sign * value * unit->seconds;
                        recent_months = sign * value * unit->months;
                        shift += recent;
                        months += recent_months;
                        anything = true;

                        continue;
                }

                if (marked || !file_letter(string_get(text + at)))
                        return false;

                positive length = 0;

                while (file_letter(string_get(text + at + length)))
                        length++;

                string_address word = text + at;

                at += length;

                if (file_same_word(word, length, (string_address) "ago"))
                {
                        shift -= 2 * recent;
                        months -= 2 * recent_months;
                        recent = -recent;
                        recent_months = -recent_months;
                        continue;
                }

                if (file_same_word(word, length, (string_address) "now") ||
                    file_same_word(word, length, (string_address) "today") ||
                    file_same_word(word, length, (string_address) "utc") ||
                    file_same_word(word, length, (string_address) "gmt") ||
                    file_same_word(word, length, (string_address) "z"))
                        continue;

                if (file_same_word(word, length, (string_address) "yesterday") ||
                    file_same_word(word, length, (string_address) "tomorrow"))
                {
                        recent = file_lower(string_get(word)) == 'y' ? -86400 : 86400;
                        recent_months = 0;
                        shift += recent;

                        continue;
                }

                bool ahead = file_same_word(word, length, (string_address) "next");

                if (ahead || file_same_word(word, length, (string_address) "last"))
                {
                        at += string_span(text + at, string_set_blanks);

                        positive wide = 0;

                        while (file_letter(string_get(text + at + wide)))
                                wide++;

                        const file_unit address_to unit = file_unit_of(text + at, wide);

                        if (!unit)
                                return false;

                        at += wide;
                        recent = (ahead ? 1 : -1) * unit->seconds;
                        recent_months = (ahead ? 1 : -1) * unit->months;
                        shift += recent;
                        months += recent_months;

                        continue;
                }

                const file_unit address_to unit = file_unit_of(word, length);

                if (!unit)
                        return false;

                recent = unit->seconds;
                recent_months = unit->months;
                shift += recent;
                months += recent_months;
        }

        // An empty date is the day and not the moment, which is what the
        // system's date answers to one.
        if (!anything && !dated && !timed)
        {
                hour = 0;
                minute = 0;
                second = 0;
        }

        b64 reach = year * 12 + (b64)month - 1 + months;
        b64 landed = reach >= 0 ? reach / 12 : -((-reach + 11) / 12);

        address_to out = file_days(landed, reach - landed * 12 + 1, day) * 86400 +
                         (b64)hour * 3600 + (b64)minute * 60 + (b64)second + shift;

        return true;
}


// Walking directories ---------------------------------------

typedef struct
{
        bipolar handle;
        positive have;
        positive at;
        p8 block[FILE_BLOCK];
} file_walk;

bool file_walk_open(file_walk address_to walk, bipolar directory, string_address path)
{
        walk->handle = system_call_3(syscall(openat), directory, (positive)path,
                                     FILE_READ | O_DIRECTORY);
        walk->have = 0;
        walk->at = 0;

        return walk->handle >= 0;
}

struct linux_dirent64 address_to file_walk_next(file_walk address_to walk)
{
        if (walk->at >= walk->have)
        {
                bipolar taken = system_call_3(syscall(getdents64), walk->handle,
                                              (positive)walk->block, FILE_BLOCK);

                if (taken <= 0)
                        return null;

                walk->have = (positive)taken;
                walk->at = 0;
        }

        struct linux_dirent64 address_to entry =
            (struct linux_dirent64 address_to)(walk->block + walk->at);

        walk->at += entry->d_reclen;

        return entry;
}

fn file_walk_close(file_walk address_to walk)
{
        if (walk->handle >= 0)
                system_call_1(syscall(close), walk->handle);

        walk->handle = -1;
}

bool file_is_dot(string_address name)
{
        if (!string_is(name, '.'))
                return false;

        if (string_is(name + 1, end))
                return true;

        return string_is(name + 1, '.') && string_is(name + 2, end);
}

// Arguments -------------------------------------------------

positive file_letter_bit(p8 letter)
{
        if (letter >= 'a' && letter <= 'z')
                return (positive)(letter - 'a');

        if (letter >= 'A' && letter <= 'Z')
                return 26 + (positive)(letter - 'A');

        if (letter >= '0' && letter <= '9')
                return 52 + (positive)(letter - '0');

        return 62;
}

#define FILE_FLAG(letter) ((positive)1 << file_letter_bit(letter))

fn file_complain(string_address program, string_address message, string_address subject)
{
        string_format(file_fail, "%s: %s: %s\n", program, subject, message);
}

/*
        The question -i asks before something is destroyed.

        Written to the standard error and answered from the standard input,
        because that is where a person is when a script is not. Anything that
        does not begin with a y is a no, and so is an input that has ended --
        which is what makes the tools safe to run with no input at all.
*/
bool file_ask(string_address program, string_address question, string_address subject)
{
        string_format(file_fail, "%s: %s '%s'? ", program, question, subject);

        p8 answer[2];
        bipolar got = system_call_3(syscall(read), 0, (positive)answer, 1);

        if (got != 1)
                return false;

        bool yes = answer[0] == 'y' || answer[0] == 'Y';

        while (answer[0] != '\n' &&
               system_call_3(syscall(read), 0, (positive)answer, 1) == 1)
                ;

        return yes;
}

/*
        The long spellings.

        Every tool here thinks in letters, and GNU's tools answer to a word as
        well: --zero for -z, --canonicalize-missing for -m. One table per tool
        turns the word back into its letter before anything else looks at it,
        so what reads the flags below goes on reading letters.

        A letter that appears only in a table is reachable only by its word,
        which is how --relative-to gets a bit of the flag word to live in
        without inventing a -R that GNU has not got.
*/
typedef struct
{
        string_address name;
        p8 letter;
} file_long;

// file_letter_bit answers 62 for anything that is not a letter or a digit.
#define FILE_LETTERS 63

/*
        The leading options, letters and words both, and a complaint when the
        word is neither.

        A word that is not a known option stops the tool rather than being
        left as an operand, which is what GNU's do and the difference is not
        academic: realpath -E used to print the resolved name of a file
        called -E and exit as though that had been the question.

        Letters named in `valued` take an argument -- the rest of the word, or
        the word after it -- kept under the bit that letter sets, so a tool
        asks for it by letter the way it asks for everything else.
*/
typedef struct
{
        string_address program;
        string_address allowed;
        string_address valued;

        /*
                Letters that take an argument only when it is written onto the
                option itself: date -Ihours, mktemp --tmpdir=/x. A bare one
                means whatever the tool calls the default and never eats the
                word after it, which is the only way --tmpdir and --tmpdir=/x
                can both be spelled by one option.

                A letter named here and in `allowed` too takes the rest of its
                cluster as the value, which is right for -Ihours and -i.bak
                and wrong for every name GNU spells as a plain flag: give
                those a letter of their own and leave it out of `allowed`, or
                --all-repeated turns uniq -Di into a complaint about i.
        */
        string_address optional;
        const file_long address_to longs;

        // seq is the one tool here where -4 is a number and not a flag.
        bool numbers;

        // head -5 and tail -5 and fold -5 are the count said without its
        // letter, and this is the letter it belongs to.
        p8 digits;

        // The text tools go on reading options after an operand, the way
        // GNU's do -- wc -l a -c counts the bytes too. Each operand is handed
        // over as it is reached, in the order it was written, because that
        // order is the whole of what an operand list is.
        fn(address_to operand)(b32 index);

        // env is the one tool here where an option given twice means it
        // twice, and one value per letter is not enough to say so: it is
        // told about each option as the option is read.
        bool(address_to seen)(p8 letter, string_address value);

        /*
                The last letter that carried a value.

                One value per letter cannot say which of two options that
                answer the same question was written last, and GNU answers
                with the last: head -n 2 -c 5 is five bytes and head -c 5 -n 2
                is two lines. A tool with such a pair reads this instead of
                asking which flag is present.
        */
        p8 last;

        positive flags;
        positive first;
        string_address value[FILE_LETTERS];
} file_taking;

static string_address file_option_value(file_taking address_to taking, p8 letter)
{
        return taking->value[file_letter_bit(letter)];
}

static bool file_option_among(string_address set, p8 letter)
{
        return set && string_first_of(set, letter);
}

static bool file_option_needs(file_taking address_to taking, string_address word)
{
        file_complain(taking->program, "option needs an argument", word);

        return false;
}

static p8 file_long_letter(file_taking address_to taking, string_address name,
                           positive length)
{
        if (!taking->longs)
                return 0;

        for (positive i = 0; taking->longs[i].name; i++)
        {
                string_address spelling = taking->longs[i].name;

                // The whole word or nothing: GNU shortens a long option to
                // any unambiguous prefix, which is a convenience for a person
                // typing and a trap for a script that outlives the flag.
                if (!string_compare_max(spelling, name, length) &&
                    string_is(spelling + length, end))
                        return taking->longs[i].letter;
        }

        return 0;
}

static bool file_take(file_taking address_to taking)
{
        positive count = (positive)program_argument_count();
        positive index = 1;

        taking->flags = 0;

        memory_fill(taking->value, 0, sizeof(taking->value));

        while (index < count)
        {
                string_address word = program_argument((b32)index);

                if (!string_is(word, '-') || string_is(word + 1, end))
                {
                        if (!taking->operand)
                                break;

                        taking->operand((b32)index++);
                        continue;
                }

                if (string_is(word + 1, '-') && string_is(word + 2, end))
                {
                        index++;

                        while (taking->operand && index < count)
                                taking->operand((b32)index++);

                        break;
                }

                if (taking->numbers && !string_is(word + 1, '-') &&
                    (file_digit(string_get(word + 1)) ||
                     string_is(word + 1, '.')))
                        break;

                index++;

                if (taking->digits && file_digit(string_get(word + 1)))
                {
                        positive bit = file_letter_bit(taking->digits);

                        taking->flags |= (positive)1 << bit;
                        taking->value[bit] = word + 1;
                        taking->last = taking->digits;
                        continue;
                }

                if (string_is(word + 1, '-'))
                {
                        string_address name = word + 2;
                        string_address mark = string_first_of(name, '=');
                        positive length = mark ? (positive)(mark - name)
                                               : string_length(name);
                        p8 letter = file_long_letter(taking, name, length);

                        if (!letter)
                        {
                                file_complain(taking->program, "unrecognized option", word);
                                return false;
                        }

                        positive bit = file_letter_bit(letter);

                        taking->flags |= (positive)1 << bit;

                        if (file_option_among(taking->optional, letter))
                        {
                                taking->value[bit] = mark ? mark + 1 : null;
                                taking->last = letter;
                        }
                        else if (string_first_of(taking->valued, letter))
                        {
                                taking->last = letter;

                                if (mark)
                                        taking->value[bit] = mark + 1;
                                else if (index < count)
                                        taking->value[bit] = program_argument((b32)index++);
                                else
                                        return file_option_needs(taking, word);
                        }

                        if (taking->seen && !taking->seen(letter, taking->value[bit]))
                                return false;

                        continue;
                }

                for (string_address letter = word + 1; string_get(letter); letter++)
                {
                        p8 named[3] = {'-', string_get(letter), end};
                        positive bit = file_letter_bit(string_get(letter));

                        if (!string_first_of(taking->allowed, string_get(letter)))
                        {
                                file_complain(taking->program, "invalid option", named);
                                return false;
                        }

                        taking->flags |= (positive)1 << bit;

                        bool spare = file_option_among(taking->optional, string_get(letter));

                        if (!spare && !string_first_of(taking->valued, string_get(letter)))
                        {
                                if (taking->seen && !taking->seen(string_get(letter), null))
                                        return false;

                                continue;
                        }

                        // -s.txt and -s .txt are the same option given the
                        // same way; either the rest of the word is the
                        // argument or the next word is.
                        taking->last = string_get(letter);

                        if (string_get(letter + 1))
                                taking->value[bit] = letter + 1;
                        else if (spare)
                                taking->value[bit] = null;
                        else if (index < count)
                                taking->value[bit] = program_argument((b32)index++);
                        else
                                return file_option_needs(taking, named);

                        if (taking->seen && !taking->seen(string_get(letter), taking->value[bit]))
                                return false;

                        break;
                }
        }

        taking->first = index;

        return true;
}

/*
        Utilities reached through the shell must see the shell's exported
        vector, while the same binary reached through a farm link has only
        the process vector. Keeping that distinction here also gives colour
        policy, PATH lookup and temporary-directory lookup one answer.
*/
string_address env_get(const_string name);
string_address address_to shell_environment();

static string_address address_to file_environment_all()
{
        string_address address_to shell = shell_environment();

        if (shell && shell[0])
                return shell;

        string_address address_to process = program_environment_list();

        return process ? process : shell;
}

static string_address file_environment(string_address name)
{
        string_address address_to environment = file_environment_all();

        return environment ? string_get_environment(environment, name) : null;
}

#define FILE_TCGETS 0x5401u

enum
{
        FILE_COLOR_NEVER,
        FILE_COLOR_AUTO,
        FILE_COLOR_ALWAYS
};

typedef struct
{
        string_address text;
        positive length;
} file_color_span;

static bool file_output_terminal()
{
        p8 settings[64];

        return system_call_3(syscall(ioctl), 1, FILE_TCGETS,
                             (positive)settings) == 0;
}

static b32 file_color_when(string_address value, b32 bare)
{
        if (!value)
                return bare;

        if (string_equals(value, "always") || string_equals(value, "yes") ||
            string_equals(value, "force"))
                return FILE_COLOR_ALWAYS;

        if (string_equals(value, "never") || string_equals(value, "no") ||
            string_equals(value, "none"))
                return FILE_COLOR_NEVER;

        if (string_equals(value, "auto") || string_equals(value, "tty") ||
            string_equals(value, "if-tty"))
                return FILE_COLOR_AUTO;

        return -1;
}

static bool file_color_active(b32 when)
{
        if (when == FILE_COLOR_ALWAYS)
                return true;

        if (when != FILE_COLOR_AUTO || !file_output_terminal())
                return false;

        string_address term = file_environment((string_address) "TERM");

        if (term && string_equals(term, "dumb"))
                return false;

        string_address no_color = file_environment((string_address) "NO_COLOR");

        return !no_color || !string_get(no_color);
}

// A colon table such as LS_COLORS or GREP_COLORS. The last spelling wins.
// Values here are SGR fragments. GNU dircolors' escaped lc/rc/ec envelope
// language and escaped colons are deliberately outside this bounded parser;
// the default ESC[ ... m envelope stays fixed and reset-safe instead.
static file_color_span file_color_value(string_address table, string_address key,
                                        string_address fallback)
{
        file_color_span answer = {fallback, fallback ? string_length(fallback) : 0};
        positive wanted = string_length(key);

        if (!table)
                return answer;

        for (string_address at = table; string_get(at);)
        {
                string_address stop = string_first_of_or_end(at, ':');
                string_address mark = string_first_of_or_end(at, '=');

                if (mark < stop && (positive)(mark - at) == wanted &&
                    !string_compare_max(at, key, wanted))
                {
                        answer.text = mark + 1;
                        answer.length = (positive)(stop - mark - 1);
                }

                at = string_get(stop) ? stop + 1 : stop;
        }

        return answer;
}

static bool file_color_has(string_address table, string_address key)
{
        positive wanted = string_length(key);

        for (string_address at = table; at && string_get(at);)
        {
                string_address stop = string_first_of_or_end(at, ':');
                string_address mark = string_first_of_or_end(at, '=');
                string_address end_of_key = mark < stop ? mark : stop;

                if ((positive)(end_of_key - at) == wanted &&
                    !string_compare_max(at, key, wanted))
                        return true;

                at = string_get(stop) ? stop + 1 : stop;
        }

        return false;
}

static bool file_color_span_is(file_color_span span, string_address text)
{
        positive length = string_length(text);

        return span.length == length &&
               !string_compare_max(span.text, text, length);
}

static bool file_color_table_valid(string_address table, bool bare_flags)
{
        for (string_address at = table; at && string_get(at);)
        {
                string_address stop = string_first_of_or_end(at, ':');
                string_address mark = string_first_of_or_end(at, '=');

                if (mark >= stop && stop > at && !bare_flags)
                        return false;

                at = string_get(stop) ? stop + 1 : stop;
        }

        return true;
}

static fn file_color_sgr(writer write, file_color_span color)
{
        write((address_any) "\033[", 2);
        write((address_any)color.text, color.length);
        write((address_any) "m", 1);
}

string_address file_reason(bipolar code)
{
        if (code < 0)
                code = -code;

        if (code == ERROR_NO_ENTRY)
                return (string_address) "No such file or directory";

        if (code == ERROR_NOT_PERMITTED)
                return (string_address) "Operation not permitted";

        if (code == ERROR_ACCESS)
                return (string_address) "Permission denied";

        if (code == ERROR_EXISTS)
                return (string_address) "File exists";

        if (code == ERROR_NOT_DIRECTORY)
                return (string_address) "Not a directory";

        if (code == ERROR_IS_DIRECTORY)
                return (string_address) "Is a directory";

        if (code == ERROR_NOT_EMPTY)
                return (string_address) "Directory not empty";

        if (code == ERROR_INVALID)
                return (string_address) "Invalid argument";

        if (code == ERROR_CROSS_DEVICE)
                return (string_address) "Invalid cross-device link";

        return (string_address) "Error";
}

// Copying, removing, making --------------------------------

static p8 file_transfer[FILE_BLOCK * 8];

bool file_copy_contents(bipolar from_directory, string_address from,
                        bipolar to_directory, string_address to, positive mode)
{
        bipolar in = system_call_3(syscall(openat), from_directory, (positive)from,
                                   FILE_READ);

        if (in < 0)
                return false;

        bipolar out = system_call_4(syscall(openat), to_directory, (positive)to,
                                    FILE_WRITE, mode);

        if (out < 0)
        {
                system_call_1(syscall(close), in);
                return false;
        }

        bool complete = true;

        while (1)
        {
                bipolar taken = system_read_retry((positive)in, file_transfer,
                                                   sizeof(file_transfer));

                if (taken < 0)
                {
                        complete = false;
                        break;
                }

                if (taken == 0)
                        break;

                if (system_write_all((positive)out, file_transfer,
                                     (positive)taken) != (positive)taken)
                {
                        complete = false;
                        break;
                }
        }

        system_call_1(syscall(close), in);
        system_call_1(syscall(close), out);

        return complete;
}

bool file_make_parents(string_address path, positive mode)
{
        p8 work[FILE_PATH_MAX];
        positive length = string_length(path);

        if (length >= FILE_PATH_MAX)
                return false;

        memory_copy_fast_end(work, path, length);

        for (positive i = 1; i < length; i++)
        {
                if (work[i] != '/')
                        continue;

                work[i] = end;

                bipolar made = system_call_3(syscall(mkdirat), AT_FDCWD,
                                             (positive)work, mode);

                if (made < 0 &&
                    (made != -ERROR_EXISTS || !file_is_directory_through(work)))
                {
                        work[i] = '/';
                        return false;
                }

                work[i] = '/';
        }

        bipolar made = system_call_3(syscall(mkdirat), AT_FDCWD, (positive)work, mode);

        return made == 0 ||
               (made == -ERROR_EXISTS && file_is_directory_through(work));
}

/*
        The utilities themselves.

        Each was its own program, with only the layer above shared. They are
        one file now because the shell runs them too, and two copies of ls is
        one copy too many. Every one keeps the name it had: file_ls is what
        ls's main was, reading its words through program_argument the same way
        it did when it was the only thing in the process.
*/

// ls ------------------------------------------------------------
/*
        ls [-laARtShr1din]

        There is an ls builtin in the shell as well. This is the one with the
        flags, and the builtin should call it rather than grow a second copy:
        a listing is not the shell's business, and a program can be replaced
        on its own.

        Output is one name per line. The system's ls does the same the moment
        it is not writing to a terminal, which is every case a script cares
        about, and columns are a terminal's problem rather than a listing's.

        Times are UTC. Nothing in this tree reads /usr/share/zoneinfo, and a
        listing that quietly used the wrong zone would be worse than one that
        says which zone it used.
*/
#define LS_MAX_ENTRIES 8192
#define LS_ARENA (1 << 20)

typedef struct
{
        positive name;
        positive mode;
        positive links;
        positive owner;
        positive group;
        p64 size;
        b64 modified;
        p32 modified_fraction;
        p64 inode;
        p64 blocks;
        bool known;
} ls_entry;

static ls_entry ls_entries[LS_MAX_ENTRIES];
static positive ls_count;
static p8 ls_arena[LS_ARENA];
static positive ls_used;

static bool ls_long;
static bool ls_hidden;
static bool ls_almost;
static bool ls_recursive;
static bool ls_by_time;
static bool ls_by_size;
static bool ls_human;
static bool ls_reversed;
static bool ls_inode;
static bool ls_numeric;
static bool ls_as_itself;
static bool ls_classify;
static bool ls_slash;
static bool ls_headings;
static bool ls_coloring;
static bool ls_color_started;
static bool ls_terminal;
static string_address ls_colors;

static b32 ls_status;
static bool ls_written;
static bool ls_broken;
static b64 ls_now;
static p8 ls_hidden_option;
static p8 ls_order_option;

static bool ls_option_seen(p8 letter, string_address value)
{
        (void)value;

        if (letter == 'a' || letter == 'A')
                ls_hidden_option = letter;

        if (letter == 't' || letter == 'S')
                ls_order_option = letter;

        return true;
}

static fn ls_limit(string_address why)
{
        if (!ls_broken)
        {
                file_fail("ls: ", 0);
                file_fail(why, 0);
                file_fail("\n", 1);
        }

        ls_broken = true;
        ls_status = 1;
}

static bool ls_keep(string_address name, positive address_to where)
{
        positive length = string_length(name);

        if (ls_used + length + 1 > LS_ARENA)
        {
                ls_limit((string_address) "directory names too large");
                return false;
        }

        positive at = ls_used;

        memory_copy_fast(ls_arena + at, name, length + 1);

        ls_used += length + 1;
        address_to where = at;

        return true;
}

static bipolar ls_order(ls_entry address_to left, ls_entry address_to right)
{
        if (ls_by_time)
        {
                if (left->modified != right->modified)
                        return left->modified > right->modified ? -1 : 1;

                // Two files written in the same second are not the same age,
                // and sorting them by name instead puts them in the wrong
                // order rather than an arbitrary one.
                if (left->modified_fraction != right->modified_fraction)
                        return left->modified_fraction > right->modified_fraction ? -1 : 1;
        }
        else if (ls_by_size)
        {
                if (left->size != right->size)
                        return left->size > right->size ? -1 : 1;
        }

        return string_compare(ls_arena + left->name, ls_arena + right->name);
}

// Shell sort with Ciura's gaps: no recursion, no second array, and a
// directory of any size this program will hold sorts in well under the
// quadratic time an insertion sort would take on it.
static fn ls_sort()
{
        positive gaps[8] = {701, 301, 132, 57, 23, 10, 4, 1};

        for (positive g = 0; g < 8; g++)
        {
                positive gap = gaps[g];

                for (positive i = gap; i < ls_count; i++)
                {
                        ls_entry held = ls_entries[i];
                        positive j = i;

                        while (j >= gap && ls_order(address_of ls_entries[j - gap],
                                                    address_of held) > 0)
                        {
                                ls_entries[j] = ls_entries[j - gap];
                                j -= gap;
                        }

                        ls_entries[j] = held;
                }
        }
}

static fn ls_size_field(p64 value)
{
        if (ls_human)
                return positive_to_human_1024(log, value);

        positive_to_string(log, value);
}

static positive ls_width_of(p64 value)
{
        return positive_digits(value);
}

static positive ls_human_width(p64 value)
{
        if (!ls_human)
                return ls_width_of(value);

        p8 text[6];

        return positive_into_human_1024_string(text, value);
}

static fn ls_owner_text(positive id, bool group, p8 address_to into)
{
        if (!ls_numeric)
        {
                bool known = group ? file_group_name(id, into, FILE_NAME_MAX)
                                   : file_user_name(id, into, FILE_NAME_MAX);

                if (known)
                        return;
        }

        positive_into_string(into, id);
}

// -F and -p put a letter after a name saying what it is: the slash for a
// directory that -p asks for on its own, and the rest of them for -F.
static p8 ls_mark(positive mode)
{
        positive kind = mode & MODE_FORMAT;

        if (kind == MODE_DIRECTORY)
                return '/';

        if (!ls_classify)
                return 0;

        if (kind == MODE_LINK)
                return '@';

        if (kind == MODE_PIPE)
                return '|';

        if (kind == MODE_SOCKET)
                return '=';

        if (kind == MODE_FILE && (mode & 0111))
                return '*';

        return 0;
}

static file_color_span ls_suffix_color(string_address name)
{
        file_color_span answer = {null, 0};
        positive name_length = string_length(name);

        for (string_address at = ls_colors; at && string_get(at);)
        {
                string_address stop = string_first_of_or_end(at, ':');
                string_address mark = string_first_of_or_end(at, '=');

                if (mark < stop && string_is(at, '*'))
                {
                        positive suffix = (positive)(mark - at - 1);

                        if (suffix <= name_length &&
                            !string_compare_max(at + 1, name + name_length - suffix,
                                                suffix))
                        {
                                answer.text = mark + 1;
                                answer.length = (positive)(stop - mark - 1);
                        }
                }

                at = string_get(stop) ? stop + 1 : stop;
        }

        return answer;
}

static file_color_span ls_name_color(string_address directory,
                                     ls_entry address_to entry,
                                     string_address name)
{
        positive mode = entry->mode;
        positive kind = mode & MODE_FORMAT;
        string_address key = (string_address) "fi";
        string_address fallback = null;

        if (kind == MODE_DIRECTORY)
        {
                key = (mode & 01000) && (mode & 0002) ? (string_address) "tw"
                      : (mode & 0002)                  ? (string_address) "ow"
                      : (mode & 01000)                 ? (string_address) "st"
                                                       : (string_address) "di";
                fallback = (string_address) ((mode & 01000) && (mode & 0002)
                                                 ? "30;42"
                                             : (mode & 0002) ? "34;42"
                                             : (mode & 01000) ? "37;44"
                                                               : "01;34");
        }
        else if (kind == MODE_LINK)
        {
                key = (string_address) "ln";
                fallback = (string_address) "01;36";

                p8 full[FILE_PATH_MAX];
                file_facts through;

                if (directory)
                        path_join(full, FILE_PATH_MAX, directory, name);
                else
                        string_copy_max_end(full, name, FILE_PATH_MAX - 1);

                if (!file_look_at(full, address_of through))
                {
                        file_color_span orphan = file_color_value(
                            ls_colors, (string_address) "or", null);

                        return orphan.text ? orphan
                                           : file_color_value(ls_colors,
                                                              (string_address) "ln",
                                                              fallback);
                }

                file_color_span link_color = file_color_value(
                    ls_colors, (string_address) "ln", fallback);

                if (file_color_span_is(link_color, (string_address) "target"))
                {
                        ls_entry target = *entry;

                        target.mode = through.mode;
                        return ls_name_color(directory, address_of target, name);
                }

                return link_color;
        }
        else if (kind == MODE_PIPE)
        {
                key = (string_address) "pi";
                fallback = (string_address) "33";
        }
        else if (kind == MODE_SOCKET)
        {
                key = (string_address) "so";
                fallback = (string_address) "01;35";
        }
        else if (kind == MODE_BLOCK)
        {
                key = (string_address) "bd";
                fallback = (string_address) "33;01";
        }
        else if (kind == MODE_CHARACTER)
        {
                key = (string_address) "cd";
                fallback = (string_address) "33;01";
        }
        else if (mode & 04000)
        {
                key = (string_address) "su";
                fallback = (string_address) "37;41";
        }
        else if (mode & 02000)
        {
                key = (string_address) "sg";
                fallback = (string_address) "30;43";
        }
        else
        {
                file_color_span suffix = ls_suffix_color(name);

                if (suffix.text)
                        return suffix;

                if (mode & 0111)
                {
                        key = (string_address) "ex";
                        fallback = (string_address) "01;32";
                }
        }

        return file_color_value(ls_colors, key, fallback);
}

static fn ls_name_text(string_address name)
{
        bool quoted = false;

        if (ls_terminal)
                for (positive i = 0; string_get(name + i); i++)
                        if (string_get(name + i) < 32 || string_get(name + i) == 127)
                        {
                                quoted = true;
                                break;
                        }

        if (!quoted)
        {
                log(name, 0);
                return;
        }

        for (positive at = 0; string_get(name + at);)
        {
                positive first = at;

                while (string_get(name + at) >= 32 && string_get(name + at) != 127)
                        at++;

                if (at > first)
                {
                        log("'", 1);
                        log(name + first, at - first);
                        log("'", 1);
                }

                if (!string_get(name + at))
                        break;

                p8 escaped[4] = {'\\', 0, 0, 0};
                p8 byte = string_get(name + at++);
                positive length = 2;

                escaped[1] = byte == '\n'   ? 'n'
                             : byte == '\t' ? 't'
                             : byte == '\r' ? 'r'
                             : byte == '\b' ? 'b'
                             : byte == '\f' ? 'f'
                             : byte == '\v' ? 'v'
                                             : 0;

                if (!escaped[1])
                {
                        escaped[1] = (p8)('0' + ((byte >> 6) & 7));
                        escaped[2] = (p8)('0' + ((byte >> 3) & 7));
                        escaped[3] = (p8)('0' + (byte & 7));
                        length = 4;
                }

                log("$'", 2);
                log(escaped, length);
                log("'", 1);
        }
}

static fn ls_name_say(string_address directory, ls_entry address_to entry,
                      string_address name)
{
        if (!ls_coloring)
        {
                ls_name_text(name);
                return;
        }

        file_color_span color = ls_name_color(directory, entry, name);

        if (!color.text || !color.length)
        {
                ls_name_text(name);
                return;
        }

        file_color_span reset = file_color_value(ls_colors,
                                                  (string_address) "rs",
                                                  (string_address) "0");

        if (!ls_color_started)
        {
                file_color_sgr(log, reset);
                ls_color_started = true;
        }

        file_color_sgr(log, color);
        ls_name_text(name);
        file_color_sgr(log, reset);
}

static fn ls_print(string_address directory)
{
        positive link_width = 1;
        positive size_width = 1;
        positive owner_width = 1;
        positive group_width = 1;
        positive inode_width = 1;
        p64 blocks = 0;

        for (positive i = 0; i < ls_count; i++)
        {
                ls_entry address_to entry = address_of ls_entries[i];

                if (ls_inode && ls_width_of(entry->inode) > inode_width)
                        inode_width = ls_width_of(entry->inode);

                if (!ls_long)
                        continue;

                if (ls_width_of(entry->links) > link_width)
                        link_width = ls_width_of(entry->links);

                positive entry_size_width = ls_human_width(entry->size);

                if (entry_size_width > size_width)
                        size_width = entry_size_width;

                p8 name[FILE_NAME_MAX];

                ls_owner_text(entry->owner, false, name);

                if (string_length(name) > owner_width)
                        owner_width = string_length(name);

                ls_owner_text(entry->group, true, name);

                if (string_length(name) > group_width)
                        group_width = string_length(name);
        }

        if (ls_long)
        {
                for (positive i = 0; i < ls_count; i++)
                        blocks += ls_entries[i].blocks;

                // The kernel counts in 512 byte blocks and ls has always
                // reported in 1024 byte ones.
                blocks /= 2;

                log("total ", 0);

                if (ls_human)
                        positive_to_human_1024(log, blocks * 1024);
                else
                        positive_to_string(log, blocks);

                log("\n", 1);
        }

        for (positive k = 0; k < ls_count; k++)
        {
                positive i = ls_reversed ? ls_count - 1 - k : k;
                ls_entry address_to entry = address_of ls_entries[i];
                string_address name = ls_arena + entry->name;

                if (ls_inode)
                {
                        positive_to_padded(log, entry->inode, inode_width, ' ', 0);
                        log(" ", 1);
                }

                if (ls_long)
                {
                        p8 letters[12];
                        p8 who[FILE_NAME_MAX];

                        file_mode_letters(letters, entry->mode);
                        log(letters, 10);
                        log(" ", 1);
                        positive_to_padded(log, entry->links, link_width, ' ', 0);
                        log(" ", 1);

                        ls_owner_text(entry->owner, false, who);
                        string_to_field(log, who, owner_width, ' ', true);
                        log(" ", 1);

                        ls_owner_text(entry->group, true, who);
                        string_to_field(log, who, group_width, ' ', true);
                        log(" ", 1);

                        positive field_width = ls_human_width(entry->size);

                        writer_fill(log, size_width > field_width
                                             ? size_width - field_width
                                             : 0,
                                    ' ');

                        ls_size_field(entry->size);
                        log(" ", 1);
                        file_stamp_short(log, entry->modified, ls_now);
                        log(" ", 1);
                }

                ls_name_say(directory, entry, name);

                bool marking = ls_classify || ls_slash;
                bool link = (entry->mode & MODE_FORMAT) == MODE_LINK;
                p8 mark = marking ? ls_mark(entry->mode) : 0;

                // In the long form the arrow is written and the mark goes on
                // what the link points at; on a line of its own the link is
                // the only thing there is to mark.
                if (mark && !(ls_long && link))
                        log(address_of mark, 1);

                if (ls_long && link)
                {
                        p8 where[FILE_PATH_MAX];
                        p8 full[FILE_PATH_MAX];

                        if (directory)
                                path_join(full, FILE_PATH_MAX, directory, name);
                        else
                                string_copy_max_end(full, name, FILE_PATH_MAX - 1);

                        if (file_link_text(full, where, FILE_PATH_MAX) >= 0)
                        {
                                file_facts through;

                                log(" -> ", 0);
                                log(where, 0);

                                // -F classifies where the link points; -p
                                // has only a slash to give and gives it to
                                // the name that stands there.
                                if (ls_classify && file_look_at(full, address_of through))
                                {
                                        p8 there = ls_mark(through.mode);

                                        if (there)
                                                log(address_of there, 1);
                                }
                        }
                }

                log("\n", 1);
        }
}

static bool ls_add(bipolar directory, string_address path, string_address shown)
{
        if (ls_count >= LS_MAX_ENTRIES)
        {
                ls_limit((string_address) "directory has too many entries");
                return false;
        }

        file_facts facts;
        ls_entry address_to entry = address_of ls_entries[ls_count];

        memory_fill(entry, 0, sizeof(ls_entry));

        if (!ls_keep(shown, address_of entry->name))
                return false;

        if (file_look(directory, path, AT_SYMLINK_NOFOLLOW, address_of facts))
        {
                entry->known = true;
                entry->mode = facts.mode;
                entry->links = facts.hard_links;
                entry->owner = facts.owner;
                entry->group = facts.group;
                entry->size = facts.size;
                entry->modified = facts.modified.seconds;
                entry->modified_fraction = facts.modified.nanoseconds;
                entry->inode = facts.inode;
                entry->blocks = facts.blocks;
        }

        ls_count++;
        return true;
}

static fn ls_directory(string_address path, bool heading, positive depth);

static fn ls_below(string_address path, positive depth)
{
        // The names of the subdirectories are taken out of the listing before
        // descending, because the listing buffers are about to be filled with
        // whatever is inside the first of them.
        p8 keep[LS_ARENA / 8];
        positive kept = 0;
        positive found = 0;

        for (positive k = 0; k < ls_count; k++)
        {
                positive i = ls_reversed ? ls_count - 1 - k : k;
                ls_entry address_to entry = address_of ls_entries[i];

                if ((entry->mode & MODE_FORMAT) != MODE_DIRECTORY)
                        continue;

                string_address name = ls_arena + entry->name;

                if (file_is_dot(name))
                        continue;

                positive length = string_length(name);

                if (kept + length + 1 > sizeof(keep))
                {
                        ls_limit((string_address) "recursive directory list too large");
                        return;
                }

                memory_copy_fast(keep + kept, name, length + 1);

                kept += length + 1;
                found++;
        }

        positive at = 0;

        for (positive i = 0; i < found; i++)
        {
                p8 below[FILE_PATH_MAX];

                path_join(below, FILE_PATH_MAX, path, keep + at);
                at += string_length(keep + at) + 1;

                ls_directory(below, true, depth - 1);
        }
}

static fn ls_directory(string_address path, bool heading, positive depth)
{
        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, path))
        {
                string_format(file_fail, "ls: cannot open directory '%s': No such file or directory\n",
                              path);
                ls_status = 1;
                return;
        }

        ls_count = 0;
        ls_used = 0;

        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (entry->d_name[0] == '.' && !ls_hidden && !ls_almost)
                        continue;

                if (ls_almost && file_is_dot(entry->d_name))
                        continue;

                if (!ls_add(walk.handle, entry->d_name, entry->d_name))
                        break;
        }

        file_walk_close(address_of walk);

        if (ls_broken)
                return;

        ls_sort();

        if (heading)
        {
                if (ls_written)
                        log("\n", 1);

                log(path, 0);
                log(":\n", 0);
        }

        ls_written = true;

        ls_print(path);

        // Each level of -R holds a listing and a block of names on the stack,
        // so a tree that links into itself stops here rather than by running
        // out of stack.
        if (ls_recursive && depth > 0)
                ls_below(path, depth);
}

static const file_long ls_longs[] = {
    {(string_address) "color", 'C'},
    {null, 0},
};

static b32 file_ls()
{
        positive count = (positive)program_argument_count();
        ls_hidden_option = 0;
        ls_order_option = 0;

        file_taking taking = {
            .program = (string_address) "ls",
            .allowed = (string_address) "laARtShr1dinFp",
            .valued = (string_address) "",
            .optional = (string_address) "C",
            .longs = ls_longs,
            .seen = ls_option_seen,
        };

        if (!file_take(address_of taking))
                return 2;

        ls_status = 0;
        ls_written = false;
        ls_broken = false;

        positive flags = taking.flags;
        positive first = taking.first;

        ls_now = file_now();

        ls_long = (flags & FILE_FLAG('l')) != 0 || (flags & FILE_FLAG('n')) != 0;
        ls_hidden = ls_hidden_option == 'a';
        ls_almost = ls_hidden_option == 'A';
        ls_recursive = (flags & FILE_FLAG('R')) != 0;
        ls_by_time = ls_order_option == 't';
        ls_by_size = ls_order_option == 'S';
        ls_human = (flags & FILE_FLAG('h')) != 0;
        ls_reversed = (flags & FILE_FLAG('r')) != 0;
        ls_inode = (flags & FILE_FLAG('i')) != 0;
        ls_numeric = (flags & FILE_FLAG('n')) != 0;
        ls_as_itself = (flags & FILE_FLAG('d')) != 0;
        ls_classify = (flags & FILE_FLAG('F')) != 0;
        ls_slash = (flags & FILE_FLAG('p')) != 0;
        ls_coloring = false;
        ls_color_started = false;
        ls_terminal = file_output_terminal();
        ls_colors = file_environment((string_address) "LS_COLORS");

        if (ls_colors && string_get(ls_colors) &&
            !file_color_table_valid(ls_colors, false))
        {
                file_fail("ls: unparsable value for LS_COLORS environment variable\n", 0);
                ls_colors = null;
        }

        if (flags & FILE_FLAG('C'))
        {
                b32 when = file_color_when(file_option_value(address_of taking, 'C'),
                                           FILE_COLOR_ALWAYS);

                if (when < 0)
                {
                        string_format(file_fail, "ls: invalid argument '%s' for --color\n",
                                      file_option_value(address_of taking, 'C'));
                        return 1;
                }

                ls_coloring = ls_colors && string_get(ls_colors) &&
                              file_color_active(when);
        }

        if (first >= count)
        {
                ls_directory((string_address) ".", false, FILE_MAX_DEPTH);
                log_flush();
                return ls_status;
        }

        positive given = count - first;

        if (ls_as_itself)
        {
                ls_count = 0;
                ls_used = 0;

                for (positive i = first; i < count; i++)
                        if (!ls_add(AT_FDCWD, program_argument((b32)i),
                                    program_argument((b32)i)))
                                break;

                if (ls_broken)
                {
                        log_flush();
                        return ls_status;
                }

                ls_sort();
                ls_print(null);
                log_flush();

                return ls_status;
        }

        // Everything that is not a directory is listed first, together, and
        // then each directory in turn -- which is the order the system's own
        // ls uses and the only one where a mixed set of operands reads.
        ls_count = 0;
        ls_used = 0;

        positive directories = 0;

        for (positive i = first; i < count; i++)
        {
                string_address path = program_argument((b32)i);

                if (!file_exists(AT_FDCWD, path))
                {
                        string_format(file_fail,
                                      "ls: cannot access '%s': No such file or directory\n",
                                      path);
                        ls_status = 1;
                        continue;
                }

                if (file_is_directory_through(path))
                {
                        directories++;
                        continue;
                }

                if (!ls_add(AT_FDCWD, path, path))
                        break;
        }

        if (ls_broken)
        {
                log_flush();
                return ls_status;
        }

        ls_headings = given > 1 || ls_recursive;

        if (ls_count > 0)
        {
                ls_sort();
                ls_print(null);
                ls_written = true;
        }

        // The directory operands are listed in name order however they were
        // typed, which is what ls does with every other list of names.
        if (directories > LS_MAX_ENTRIES)
        {
                ls_limit((string_address) "too many directory operands");
                log_flush();
                return ls_status;
        }

        positive order[LS_MAX_ENTRIES];
        positive have = 0;

        for (positive i = first; i < count && have < LS_MAX_ENTRIES; i++)
        {
                string_address path = program_argument((b32)i);

                if (!file_exists(AT_FDCWD, path) || !file_is_directory_through(path))
                        continue;

                order[have++] = i;
        }

        for (positive i = 1; i < have; i++)
        {
                positive held = order[i];
                positive j = i;

                while (j > 0 &&
                       string_compare(program_argument((b32)order[j - 1]),
                                      program_argument((b32)held)) > 0)
                {
                        order[j] = order[j - 1];
                        j--;
                }

                order[j] = held;
        }

        for (positive i = 0; i < have; i++)
                ls_directory(program_argument((b32)order[i]), ls_headings,
                             FILE_MAX_DEPTH);

        log_flush();

        return ls_status;
}

// Running a command ------------------------------------------------
/* find -exec runs a child through PATH with the current exported vector. */

// Tries every PATH candidate and returns the kernel error if none replaced us.
// The name is separate from words[0], because env -a changes argv[0] without
// changing the file it asks execve to run.
static bipolar file_exec_path_try_in(string_address name,
                                     string_address address_to words,
                                     string_address address_to environment,
                                     string_address path)
{
        p8 candidate[FILE_PATH_MAX];
        bool denied = false;

        if (string_first_of(name, '/'))
        {
                bipolar answer = system_call_3(syscall(execve), (positive)name,
                                               (positive)words, (positive)environment);

                return answer;
        }

        if (!path)
                path = "/bin:/usr/bin:/";

        // An empty PATH component is the current directory. That includes a
        // completely empty PATH and the component after a trailing colon.
        while (1)
        {
                string_address stop = string_first_of_or_end(path, ':');
                positive length = (positive)(stop - path);
                positive named = string_length(name);
                positive filled = length ? length : 1;

                if (named <= positive_max - 2 &&
                    filled <= positive_max - named - 2)
                {
                        positive needed = filled + named + 2;

                        if (length && path[length - 1] == '/')
                                needed--;

                        if (needed <= FILE_PATH_MAX)
                        {
                                if (length)
                                        memory_copy_fast(candidate, path, length);
                                else
                                        candidate[0] = '.';

                                if (candidate[filled - 1] != '/')
                                        candidate[filled++] = '/';

                                memory_copy_fast_end(candidate + filled, name, named);

                                bipolar answer =
                                    system_call_3(syscall(execve), (positive)candidate,
                                                  (positive)words,
                                                  (positive)environment);

                                if (answer == -ERROR_ARGUMENT_LIST)
                                        return answer;

                                if (answer == -ERROR_ACCESS)
                                        denied = true;
                        }
                }

                if (!string_get(stop))
                        break;

                path = stop + 1;
        }

        return denied ? -ERROR_ACCESS : -ERROR_NO_ENTRY;
}

// This only returns in a child process.
static bipolar file_exec_path_try(string_address address_to words)
{
        string_address path = env_get("PATH");

        if (!path)
                path = file_environment("PATH");

        return file_exec_path_try_in(words[0], words, file_environment_all(), path);
}

// Replaces this process, and only ever called in a child of it.
static fn file_exec_path(string_address address_to words)
{
        bipolar answer = file_exec_path_try(words);

        exit(answer == -ERROR_ACCESS ? 126 : 127);
}

// Forks, runs, waits, and answers with what came back.
static b32 file_run(string_address address_to words)
{
        positive status = 0;

        log_flush();

        bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child == 0)
                file_exec_path(words);

        if (child < 0)
                return 127;

        system_wait4_retry(child, address_of status, 0, null);

        if (status & 0x7f)
                return 125;

        return (b32)((status >> 8) & 0xff);
}

// find ------------------------------------------------------------
/*
        find [-H|-L|-P] [PATH...] [EXPRESSION]

        The expression is a language and is read as one: ! binds tighter than
        -a, -a tighter than -o, parentheses group, and the whole of it is
        built into a tree once and walked once per name. A flat list of tests
        that all had to hold cannot say -name '*.c' -o -name '*.h', and that
        is half of what find is asked for.

        An action anywhere in the expression takes the place of the -print
        that is otherwise put on the end. That is the rule that makes
        "-name x -delete" delete rather than print, and it is the one find
        rule everybody has been bitten by.

        -ok and -printf are not here: -ok asks a question of a terminal, and
        -printf is a second format language. Both would be their own work.
*/
#define FIND_BATCH_WORDS 256
#define FIND_BATCH_BYTES 32768

typedef struct
{
        p8 kind;
        b32 unit;
        p8 comparison;
        b32 left;
        b32 right;
        string_address text;
        b64 number;
        b64 extra;
} find_node;

typedef struct
{
        b32 node;
        positive words;
        positive used;
        string_address word[FIND_BATCH_WORDS + 1];
        p8 text[FIND_BATCH_BYTES];
} find_batch;

static find_node address_to find_nodes;
static positive find_node_room;
static positive find_used;
static b32 find_root = -1;
static bool find_bad;
static bool find_has_action;

static find_batch address_to find_batches;
static positive find_batch_room;
static positive find_batch_have;

static p8 address_to find_exec_text;
static positive find_exec_text_room;
static string_address address_to find_exec_words;
static positive find_exec_word_room;

static positive find_at;
static positive find_count;

static positive find_maximum = FILE_MAX_DEPTH;
static positive find_minimum;
static bool find_deepest;
static bool find_one_system;
static bool find_follow;
static bool find_follow_named;
static bool find_quit;
static bool find_pruned;
static b32 find_status;
static b64 find_moment;
static p64 find_device;

static string_address find_path;
static string_address find_name;
static file_facts address_to find_facts;
static positive find_depth;

static bool find_size_holds(find_node address_to test, file_facts address_to facts)
{
        p64 divisor = 512;

        if (test->unit == 'c')
                divisor = 1;
        else if (test->unit == 'k')
                divisor = 1024;
        else if (test->unit == 'M')
                divisor = 1024 * 1024;
        else if (test->unit == 'G')
                divisor = 1024 * 1024 * 1024;

        // find rounds up: a file of one byte is one block, and "-size 1" is
        // meant to find it.
        p64 units = (facts->size + divisor - 1) / divisor;

        if (test->comparison == '+')
                return units > (p64)test->number;

        if (test->comparison == '-')
                return units < (p64)test->number;

        return units == (p64)test->number;
}

static bool find_type_holds(p8 wanted, positive mode)
{
        positive kind = mode & MODE_FORMAT;

        if (wanted == 'f')
                return kind == MODE_FILE;

        if (wanted == 'd')
                return kind == MODE_DIRECTORY;

        if (wanted == 'l')
                return kind == MODE_LINK;

        if (wanted == 'b')
                return kind == MODE_BLOCK;

        if (wanted == 'c')
                return kind == MODE_CHARACTER;

        if (wanted == 'p')
                return kind == MODE_PIPE;

        if (wanted == 's')
                return kind == MODE_SOCKET;

        return false;
}

static bool find_empty(string_address path, file_facts address_to facts)
{
        if ((facts->mode & MODE_FORMAT) != MODE_DIRECTORY)
                return facts->size == 0;

        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, path))
                return false;

        struct linux_dirent64 address_to entry;
        bool empty = true;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                empty = false;
                break;
        }

        file_walk_close(address_of walk);

        return empty;
}

// Building the tree -------------------------------------------------

static b32 find_make(p8 kind)
{
        if (!shell_room((address_any address_to)address_of find_nodes,
                        address_of find_node_room, find_used + 1,
                        sizeof(find_node)))
        {
                file_fail("find: out of memory while reading expression\n", 0);
                find_bad = true;
                return -1;
        }

        find_node address_to node = address_of find_nodes[find_used];

        memory_fill(node, 0, sizeof(find_node));
        node->kind = kind;
        node->left = -1;
        node->right = -1;

        return (b32)find_used++;
}

static string_address find_word()
{
        return find_at < find_count ? program_argument((b32)find_at) : null;
}

static bool find_is(string_address word, string_address name)
{
        return word && string_compare(word, name) == 0;
}

static string_address find_value(string_address word)
{
        if (find_at >= find_count)
        {
                string_format(file_fail, "find: missing argument to %s\n", word);
                find_bad = true;
                return null;
        }

        return program_argument((b32)find_at++);
}

static string_address find_marked(string_address text, p8 address_to comparison)
{
        address_to comparison = ' ';

        if (string_is(text, '+') || string_is(text, '-'))
        {
                address_to comparison = string_get(text);
                text++;
        }

        return text;
}

static bool find_holds_count(p8 comparison, b64 value, b64 wanted)
{
        if (comparison == '+')
                return value > wanted;

        if (comparison == '-')
                return value < wanted;

        return value == wanted;
}

static fn find_lowered(string_address text, p8 address_to into)
{
        positive i = 0;

        while (string_get(text + i) && i < FILE_PATH_MAX - 1)
        {
                into[i] = file_lower(string_get(text + i));
                i++;
        }

        into[i] = end;
}

static b32 find_parse_or();

// A time in whole units, the way find counts one: the fraction is dropped, so
// a file touched thirty hours ago is one day old and not two.
static b64 find_age(p8 which, b64 scale)
{
        file_moment address_to stamp = which == 'a'   ? address_of find_facts->accessed
                                       : which == 'c' ? address_of find_facts->changed
                                                      : address_of find_facts->modified;

        return (find_moment - stamp->seconds) / scale;
}

static b32 find_parse_primary()
{
        string_address word = find_word();

        if (!word)
                return -1;

        if (find_is(word, (string_address) "("))
        {
                find_at++;

                b32 inside = find_parse_or();

                if (find_bad)
                        return -1;

                if (!find_is(find_word(), (string_address) ")"))
                {
                        file_fail("find: expected ')'\n", 0);
                        find_bad = true;
                        return -1;
                }

                find_at++;

                return inside;
        }

        if (find_is(word, (string_address) "!") || find_is(word, (string_address) "-not"))
        {
                find_at++;

                b32 node = find_make('!');
                b32 under = find_parse_primary();

                if (find_bad || node < 0 || under < 0)
                {
                        find_bad = true;
                        return -1;
                }

                find_nodes[node].left = under;

                return node;
        }

        find_at++;

        // The options that say how the walk goes rather than what holds. Each
        // is true wherever it stands, which is how find has always let them
        // be written in the middle of an expression.
        if (find_is(word, (string_address) "-maxdepth") ||
            find_is(word, (string_address) "-mindepth"))
        {
                string_address value = find_value(word);

                if (!value)
                        return -1;

                if (string_is(word + 2, 'a'))
                        find_maximum = string_digits(value, null);
                else
                        find_minimum = string_digits(value, null);

                return find_make('v');
        }

        if (find_is(word, (string_address) "-depth"))
        {
                find_deepest = true;
                return find_make('v');
        }

        if (find_is(word, (string_address) "-xdev") ||
            find_is(word, (string_address) "-mount"))
        {
                find_one_system = true;
                return find_make('v');
        }

        if (find_is(word, (string_address) "-follow"))
        {
                find_follow = true;
                return find_make('v');
        }

        if (find_is(word, (string_address) "-true"))
                return find_make('v');

        if (find_is(word, (string_address) "-false"))
                return find_make('f');

        if (find_is(word, (string_address) "-print"))
        {
                find_has_action = true;
                return find_make('d');
        }

        if (find_is(word, (string_address) "-print0"))
        {
                find_has_action = true;
                return find_make('0');
        }

        if (find_is(word, (string_address) "-delete"))
        {
                find_has_action = true;
                find_deepest = true;
                return find_make('D');
        }

        if (find_is(word, (string_address) "-prune"))
                return find_make('r');

        if (find_is(word, (string_address) "-quit"))
                return find_make('q');

        if (find_is(word, (string_address) "-empty"))
                return find_make('y');

        if (find_is(word, (string_address) "-nouser"))
                return find_make('U');

        if (find_is(word, (string_address) "-nogroup"))
                return find_make('G');

        if (find_is(word, (string_address) "-exec"))
        {
                b32 node = find_make('x');

                if (node < 0)
                        return -1;

                find_nodes[node].number = (b64)find_at;

                while (find_at < find_count &&
                       !find_is(find_word(), (string_address) ";") &&
                       !find_is(find_word(), (string_address) "+"))
                        find_at++;

                if (find_at >= find_count)
                {
                        file_fail("find: -exec has no ending ; or +\n", 0);
                        find_bad = true;
                        return -1;
                }

                bool many = find_is(find_word(), (string_address) "+");

                find_nodes[node].extra = (b64)find_at;
                find_nodes[node].comparison = many ? '+' : ';';
                find_at++;

                if (many)
                {
                        // The + form appends the names where the {} stands,
                        // so the {} has to be the last word of the template
                        // and nowhere else.
                        if (find_nodes[node].extra == find_nodes[node].number ||
                            !find_is(program_argument((b32)(find_nodes[node].extra - 1)),
                                     (string_address) "{}"))
                        {
                                file_fail("find: -exec ... + needs {} just before the +\n", 0);
                                find_bad = true;
                                return -1;
                        }

                        find_nodes[node].extra--;

                        if (!memory_reserve(
                                (address_any address_to)address_of find_batches,
                                address_of find_batch_room, find_batch_have,
                                find_batch_have + 1, sizeof(find_batch), 2))
                        {
                                shell_memory_failed = true;
                                file_fail("find: out of memory while reading -exec\n", 0);
                                find_bad = true;
                                return -1;
                        }

                        memory_fill(address_of find_batches[find_batch_have], 0,
                                    sizeof(find_batch));
                        find_batches[find_batch_have].node = node;
                        find_nodes[node].unit = (b32)find_batch_have++;
                }

                find_has_action = true;

                return node;
        }

        if (find_is(word, (string_address) "-name") ||
            find_is(word, (string_address) "-iname") ||
            find_is(word, (string_address) "-path") ||
            find_is(word, (string_address) "-wholename") ||
            find_is(word, (string_address) "-ipath") ||
            find_is(word, (string_address) "-lname"))
        {
                string_address value = find_value(word);

                if (!value)
                        return -1;

                p8 kind = find_is(word, (string_address) "-name")    ? 'n'
                          : find_is(word, (string_address) "-iname") ? 'N'
                          : find_is(word, (string_address) "-ipath") ? 'P'
                          : find_is(word, (string_address) "-lname") ? 'L'
                                                                     : 'p';

                b32 node = find_make(kind);

                if (node >= 0)
                        find_nodes[node].text = value;

                return node;
        }

        if (find_is(word, (string_address) "-type"))
        {
                string_address value = find_value(word);

                if (!value)
                        return -1;

                b32 node = find_make('t');

                if (node >= 0)
                        find_nodes[node].number = string_get(value);

                return node;
        }

        if (find_is(word, (string_address) "-perm"))
        {
                string_address value = find_value(word);

                if (!value)
                        return -1;

                p8 how = ' ';

                if (string_is(value, '-') || string_is(value, '/'))
                {
                        how = string_get(value);
                        value++;
                }

                positive mode = 0;

                if (!file_mode_of(value, 0, false, address_of mode))
                {
                        string_format(file_fail, "find: invalid mode %s\n", value);
                        find_bad = true;
                        return -1;
                }

                b32 node = find_make('m');

                if (node >= 0)
                {
                        find_nodes[node].number = (b64)mode;
                        find_nodes[node].comparison = how;
                }

                return node;
        }

        if (find_is(word, (string_address) "-size"))
        {
                string_address value = find_value(word);

                if (!value)
                        return -1;

                b32 node = find_make('z');

                if (node < 0)
                        return -1;

                p8 how;
                string_address step = find_marked(value, address_of how);
                positive taken;

                find_nodes[node].comparison = how;
                find_nodes[node].number = (b64)string_digits(step, address_of taken);
                step += taken;

                find_nodes[node].unit = string_get(step) ? string_get(step) : 'b';

                return node;
        }

        if (find_is(word, (string_address) "-links") ||
            find_is(word, (string_address) "-inum"))
        {
                string_address value = find_value(word);

                if (!value)
                        return -1;

                b32 node = find_make(find_is(word, (string_address) "-links") ? 'k' : 'i');

                if (node < 0)
                        return -1;

                p8 how;
                string_address step = find_marked(value, address_of how);

                find_nodes[node].comparison = how;
                find_nodes[node].number = (b64)string_digits(step, null);

                return node;
        }

        if (find_is(word, (string_address) "-mtime") ||
            find_is(word, (string_address) "-atime") ||
            find_is(word, (string_address) "-ctime") ||
            find_is(word, (string_address) "-mmin") ||
            find_is(word, (string_address) "-amin") ||
            find_is(word, (string_address) "-cmin"))
        {
                string_address value = find_value(word);

                if (!value)
                        return -1;

                b32 node = find_make('T');

                if (node < 0)
                        return -1;

                p8 how;
                string_address step = find_marked(value, address_of how);

                find_nodes[node].comparison = how;
                find_nodes[node].number = (b64)string_digits(step, null);
                find_nodes[node].unit = string_get(word + 1);
                find_nodes[node].extra = string_is(word + 2, 't') ? 86400 : 60;

                return node;
        }

        if (find_is(word, (string_address) "-user") ||
            find_is(word, (string_address) "-uid") ||
            find_is(word, (string_address) "-group") ||
            find_is(word, (string_address) "-gid"))
        {
                string_address value = find_value(word);

                if (!value)
                        return -1;

                bool group = string_is(word + 1, 'g');
                positive number;
                bipolar who = string_digits_exact(value, address_of number)
                                  ? (bipolar)number
                                  : (group ? file_group_id(value) : file_user_id(value));

                if (who < 0)
                {
                        string_format(file_fail, "find: '%s' is not the name of a known %s\n",
                                      value,
                                      group ? (string_address) "group"
                                            : (string_address) "user");
                        find_bad = true;
                        return -1;
                }

                b32 node = find_make(group ? 'g' : 'u');

                if (node >= 0)
                        find_nodes[node].number = (b64)who;

                return node;
        }

        if (find_is(word, (string_address) "-newer") ||
            find_is(word, (string_address) "-newermt"))
        {
                string_address value = find_value(word);

                if (!value)
                        return -1;

                b64 when;
                b64 exact = 0;

                if (find_is(word, (string_address) "-newermt"))
                {
                        if (!file_moment_read(value, find_moment, address_of when))
                        {
                                string_format(file_fail, "find: invalid date '%s'\n", value);
                                find_bad = true;
                                return -1;
                        }
                }
                else
                {
                        file_facts facts;

                        if (!file_look_at(value, address_of facts))
                        {
                                string_format(file_fail,
                                              "find: '%s': No such file or directory\n", value);
                                find_bad = true;
                                return -1;
                        }

                        when = facts.modified.seconds;
                        exact = facts.modified.nanoseconds;
                }

                b32 node = find_make('w');

                if (node >= 0)
                {
                        find_nodes[node].number = when;
                        find_nodes[node].extra = exact;
                }

                return node;
        }

        string_format(file_fail, "find: unknown predicate: %s\n", word);
        find_bad = true;

        return -1;
}

static b32 find_parse_and()
{
        b32 left = find_parse_primary();

        if (find_bad)
                return -1;

        while (1)
        {
                string_address word = find_word();

                if (find_is(word, (string_address) "-a") ||
                    find_is(word, (string_address) "-and"))
                {
                        find_at++;
                        word = find_word();
                }
                else if (!word || find_is(word, (string_address) ")") ||
                         find_is(word, (string_address) "-o") ||
                         find_is(word, (string_address) "-or"))
                        break;

                b32 right = find_parse_primary();

                if (find_bad)
                        return -1;

                b32 node = find_make('&');

                if (node < 0)
                        return -1;

                find_nodes[node].left = left;
                find_nodes[node].right = right;
                left = node;
        }

        return left;
}

static b32 find_parse_or()
{
        b32 left = find_parse_and();

        if (find_bad)
                return -1;

        while (find_is(find_word(), (string_address) "-o") ||
               find_is(find_word(), (string_address) "-or"))
        {
                find_at++;

                b32 right = find_parse_and();

                if (find_bad)
                        return -1;

                b32 node = find_make('|');

                if (node < 0)
                        return -1;

                find_nodes[node].left = left;
                find_nodes[node].right = right;
                left = node;
        }

        return left;
}

// Running it --------------------------------------------------------

static fn find_batch_run(positive slot)
{
        find_batch address_to batch = address_of find_batches[slot];

        if (!batch->words)
                return;

        find_node address_to node = address_of find_nodes[batch->node];
        positive have = 0;
        positive template = (positive)(node->extra - node->number);

        if (!shell_room((address_any address_to)address_of find_exec_words,
                        address_of find_exec_word_room,
                        template + batch->words + 1,
                        sizeof(string_address)))
        {
                file_fail("find: out of memory while building -exec arguments\n", 0);
                find_status = 1;
                batch->words = 0;
                batch->used = 0;
                return;
        }

        for (b32 i = (b32)node->number; i < (b32)node->extra; i++)
                find_exec_words[have++] = program_argument(i);

        for (positive i = 0; i < batch->words; i++)
                find_exec_words[have++] = batch->word[i];

        find_exec_words[have] = null;

        if (file_run(find_exec_words) != 0)
                find_status = 1;

        batch->words = 0;
        batch->used = 0;
}

static fn find_batch_add(find_node address_to node, string_address path)
{
        find_batch address_to batch = address_of find_batches[node->unit];
        positive length = string_length(path);
        positive room = (b32)node->extra - (b32)node->number;

        if (batch->words + room + 2 > FIND_BATCH_WORDS ||
            batch->used + length + 1 > FIND_BATCH_BYTES)
                find_batch_run(node->unit);

        memory_copy_end(batch->text + batch->used, path, length);
        batch->word[batch->words++] = batch->text + batch->used;
        batch->used += length + 1;
}

static bool find_exec_once(find_node address_to node)
{
        positive used = 0;
        positive have = 0;
        positive path_length = string_length(find_path);
        positive words = (positive)(node->extra - node->number);

        if (!shell_room((address_any address_to)address_of find_exec_words,
                        address_of find_exec_word_room, words + 1,
                        sizeof(string_address)))
        {
                file_fail("find: out of memory while building -exec arguments\n", 0);
                find_status = 1;
                return false;
        }

        positive needed = 0;

        for (b32 i = (b32)node->number; i < (b32)node->extra; i++)
        {
                string_address word = program_argument(i);

                for (positive k = 0; string_get(word + k);)
                {
                        positive add = 1;

                        if (string_is(word + k, '{') && string_is(word + k + 1, '}'))
                        {
                                add = path_length;
                                k += 2;
                        }
                        else
                                k++;

                        if (needed > (positive)-1 - add)
                        {
                                file_fail("find: -exec arguments are too large\n", 0);
                                find_status = 1;
                                return false;
                        }

                        needed += add;
                }

                if (needed == (positive)-1)
                {
                        file_fail("find: -exec arguments are too large\n", 0);
                        find_status = 1;
                        return false;
                }

                needed++;
        }

        if (!shell_room((address_any address_to)address_of find_exec_text,
                        address_of find_exec_text_room, needed ? needed : 1, 1))
        {
                file_fail("find: out of memory while expanding -exec arguments\n", 0);
                find_status = 1;
                return false;
        }

        for (b32 i = (b32)node->number; i < (b32)node->extra; i++)
        {
                string_address word = program_argument(i);
                positive at = used;

                for (positive k = 0; string_get(word + k);)
                {
                        if (string_is(word + k, '{') && string_is(word + k + 1, '}'))
                        {
                                for (positive j = 0; string_get(find_path + j); j++)
                                        find_exec_text[used++] = string_get(find_path + j);

                                k += 2;
                                continue;
                        }

                        find_exec_text[used++] = string_get(word + k);

                        k++;
                }

                find_exec_text[used++] = end;

                find_exec_words[have++] = find_exec_text + at;
        }

        find_exec_words[have] = null;

        return file_run(find_exec_words) == 0;
}

// The shell's own matcher, which -name and -path and grep's globs all go
// through. Declared here rather than defined because expand.c is read last.
bool shell_match(string_address pattern, string_address text);

static bool find_true(b32 which)
{
        if (which < 0)
                return true;

        find_node address_to node = address_of find_nodes[which];
        p8 name[FILE_PATH_MAX];
        p8 pattern[FILE_PATH_MAX];

        switch (node->kind)
        {
        case '&':
                return find_true(node->left) && find_true(node->right);

        case '|':
                return find_true(node->left) || find_true(node->right);

        case '!':
                return !find_true(node->left);

        case 'v':
                return true;

        case 'f':
                return false;

        case 'n':
                return shell_match(node->text, find_name);

        case 'p':
                return shell_match(node->text, find_path);

        case 'N':
        case 'P':
                find_lowered(node->kind == 'N' ? find_name : find_path, name);
                find_lowered(node->text, pattern);
                return shell_match(pattern, name);

        case 'L':
                if ((find_facts->mode & MODE_FORMAT) != MODE_LINK)
                        return false;

                if (file_link_text(find_path, name, FILE_PATH_MAX) < 0)
                        return false;

                return shell_match(node->text, name);

        case 't':
                return find_type_holds((p8)node->number, find_facts->mode);

        case 'z':
                return find_size_holds(node, find_facts);

        case 'y':
                return find_empty(find_path, find_facts);

        case 'm':
                if (node->comparison == '-')
                        return ((positive)find_facts->mode & (positive)node->number) ==
                               (positive)node->number;

                if (node->comparison == '/')
                        return ((positive)find_facts->mode & (positive)node->number) != 0;

                return (find_facts->mode & 07777) == (positive)node->number;

        case 'u':
                return find_facts->owner == (positive)node->number;

        case 'g':
                return find_facts->group == (positive)node->number;

        case 'U':
                return !file_user_name(find_facts->owner, name, FILE_NAME_MAX);

        case 'G':
                return !file_group_name(find_facts->group, name, FILE_NAME_MAX);

        case 'k':
                return find_holds_count(node->comparison, find_facts->hard_links,
                                        node->number);

        case 'i':
                return find_holds_count(node->comparison, (b64)find_facts->inode,
                                        node->number);

        case 'T':
                return find_holds_count(node->comparison,
                                        find_age(node->unit, node->extra), node->number);

        case 'w':
                if (find_facts->modified.seconds != node->number)
                        return find_facts->modified.seconds > node->number;

                return (b64)find_facts->modified.nanoseconds > node->extra;

        case 'd':
                file_line(find_path);
                return true;

        case '0':
                log(find_path, 0);
                log("\0", 1);
                return true;

        case 'r':
                find_pruned = true;
                return true;

        case 'q':
                find_quit = true;
                return true;

        case 'D':
        {
                bipolar gone = system_call_3(
                    syscall(unlinkat), AT_FDCWD, (positive)find_path,
                    (find_facts->mode & MODE_FORMAT) == MODE_DIRECTORY ? AT_REMOVEDIR : 0);

                if (gone < 0)
                {
                        string_format(file_fail, "find: cannot delete '%s': %s\n", find_path,
                                      file_reason(gone));
                        find_status = 1;
                        return false;
                }

                return true;
        }

        case 'x':
                if (node->comparison == '+')
                {
                        find_batch_add(node, find_path);
                        return true;
                }

                return find_exec_once(node);
        }

        return false;
}

static fn find_walk(string_address path, string_address name, positive depth, bool named)
{
        file_facts facts;
        bool follow = find_follow || (find_follow_named && named);

        if (find_quit)
                return;

        if (!(follow ? file_look_at(path, address_of facts)
                     : file_look_link(path, address_of facts)))
        {
                string_format(file_fail, "find: '%s': No such file or directory\n", path);
                find_status = 1;
                return;
        }

        if (named)
                find_device = ((p64)facts.device_major << 32) | facts.device_minor;

        bool directory = (facts.mode & MODE_FORMAT) == MODE_DIRECTORY;
        bool wanted = depth >= find_minimum && depth <= find_maximum;

        find_path = path;
        find_name = name;
        find_facts = address_of facts;
        find_depth = depth;
        find_pruned = false;

        if (!find_deepest && wanted)
                find_true(find_root);

        bool descend = directory && depth < find_maximum && !find_pruned && !find_quit;

        if (descend && find_one_system &&
            (((p64)facts.device_major << 32) | facts.device_minor) != find_device)
                descend = false;

        if (descend)
        {
                file_walk walk;

                if (!file_walk_open(address_of walk, AT_FDCWD, path))
                {
                        string_format(file_fail, "find: '%s': Permission denied\n", path);
                        find_status = 1;
                }
                else
                {
                        struct linux_dirent64 address_to entry;

                        while (!find_quit && (entry = file_walk_next(address_of walk)))
                        {
                                if (file_is_dot(entry->d_name))
                                        continue;

                                p8 below[FILE_PATH_MAX];
                                p8 held[FILE_NAME_MAX];

                                string_copy_max_end(held, entry->d_name,
                                                    FILE_NAME_MAX - 1);

                                path_join(below, FILE_PATH_MAX, path, held);

                                find_walk(below, held, depth + 1, false);
                        }

                        file_walk_close(address_of walk);
                }
        }

        if (find_deepest && wanted && !find_quit)
        {
                // The walk above wrote over all of these on its way down.
                find_path = path;
                find_name = name;
                find_facts = address_of facts;
                find_depth = depth;

                find_true(find_root);
        }
}

static b32 file_find()
{
        positive count = (positive)program_argument_count();
        positive index = 1;

        find_used = 0;
        find_root = -1;
        find_bad = false;
        find_has_action = false;
        find_batch_have = 0;
        find_at = 0;
        find_count = 0;
        find_maximum = FILE_MAX_DEPTH;
        find_minimum = 0;
        find_deepest = false;
        find_one_system = false;
        find_follow = false;
        find_follow_named = false;
        find_quit = false;
        find_pruned = false;
        find_status = 0;
        find_device = 0;
        find_moment = file_now();

        while (index < count)
        {
                string_address word = program_argument((b32)index);

                if (find_is(word, (string_address) "-H"))
                {
                        find_follow_named = true;
                        find_follow = false;
                }
                else if (find_is(word, (string_address) "-L"))
                        find_follow = true;
                else if (find_is(word, (string_address) "-P"))
                {
                        find_follow = false;
                        find_follow_named = false;
                }
                else
                        break;

                index++;
        }

        positive roots_first = index;

        while (index < count)
        {
                string_address word = program_argument((b32)index);

                if (string_is(word, '-') && string_get(word + 1))
                        break;

                if (find_is(word, (string_address) "(") ||
                    find_is(word, (string_address) ")") ||
                    find_is(word, (string_address) "!"))
                        break;

                index++;
        }

        positive roots_last = index;

        find_at = index;
        find_count = count;
        find_root = find_parse_or();

        if (find_bad)
                return 1;

        if (find_at < count)
        {
                string_format(file_fail, "find: paths must precede expression: %s\n",
                              program_argument((b32)find_at));
                return 1;
        }

        // The -print that is only there when nothing else acts.
        if (!find_has_action)
        {
                b32 said = find_make('d');

                if (said < 0)
                        return 1;

                if (find_root < 0)
                        find_root = said;
                else
                {
                        b32 both = find_make('&');

                        if (both < 0)
                                return 1;

                        find_nodes[both].left = find_root;
                        find_nodes[both].right = said;
                        find_root = both;
                }
        }

        if (roots_last == roots_first)
                find_walk((string_address) ".", (string_address) ".", 0, true);
        else
                for (positive i = roots_first; i < roots_last && !find_quit; i++)
                {
                        string_address root = program_argument((b32)i);
                        p8 name[FILE_PATH_MAX];

                        path_tail_copy(name, FILE_PATH_MAX, root);
                        find_walk(root, name, 0, true);
                }

        for (positive i = 0; i < find_batch_have; i++)
                find_batch_run(i);

        log_flush();

        return find_status;
}

// stat ------------------------------------------------------------
/*
        stat [-L] [-c FORMAT] FILE...

        The default is a readable block. -c is the one that is meant to be
        parsed, so every specifier there prints exactly one field and nothing
        else -- %s is the size and not "size: 12".

        Times are UTC, and say so in the +0000 they carry.
*/
static bool stat_follow;
static bool stat_file_system;
static b32 stat_status;

static string_address statfs_type_name(b64 type)
{
        if ((p64)type == 0xef53)
                return (string_address) "ext2/ext3";
        if ((p64)type == 0x01021994)
                return (string_address) "tmpfs";
        if ((p64)type == 0x794c7630)
                return (string_address) "overlayfs";
        if ((p64)type == 0x9fa0)
                return (string_address) "proc";
        if ((p64)type == 0x62656572)
                return (string_address) "sysfs";
        if ((p64)type == 0x63677270)
                return (string_address) "cgroup2fs";
        if ((p64)type == 0x58465342)
                return (string_address) "xfs";
        if ((p64)type == 0x9123683e)
                return (string_address) "btrfs";

        return (string_address) "UNKNOWN";
}

static p64 statfs_identity(file_mount_facts address_to facts)
{
        /* GNU writes the kernel's two fsid words in their array order. */
        return ((p64)(p32)facts->identity[0] << 32) | (p32)facts->identity[1];
}

static fn statfs_one_specifier(p8 letter, string_address path,
                               file_mount_facts address_to facts)
{
        switch (letter)
        {
        case 'n':
                return log(path, 0);
        case 'i':
                return positive_to_base_field(log, statfs_identity(facts), 16, 16,
                                              -1, (positive)1 << 28);
        case 'l':
                return positive_to_string(log, facts->name_length);
        case 's':
                return positive_to_string(log, facts->block_size);
        case 'S':
                return positive_to_string(log, facts->fragment_size);
        case 'b':
                return positive_to_string(log, facts->blocks);
        case 'f':
                return positive_to_string(log, facts->blocks_free);
        case 'a':
                return positive_to_string(log, facts->blocks_available);
        case 'c':
                return positive_to_string(log, facts->files);
        case 'd':
                return positive_to_string(log, facts->files_free);
        case 'T':
                return log(statfs_type_name(facts->type), 0);
        case 't':
                return positive_to_base_field(log, (p64)facts->type, 16, 1, -1, 0);
        case '%':
                return log("%", 1);
        }

        log("?", 1);
}

static fn statfs_formatted(string_address format, string_address path,
                           file_mount_facts address_to facts)
{
        string_address step = format;

        while (string_get(step))
        {
                string_address mark = string_first_of_or_end(step, '%');

                if (mark != step)
                        log(step, (positive)(mark - step));

                if (!string_get(mark))
                        break;

                if (string_get(mark + 1))
                {
                        statfs_one_specifier(string_get(mark + 1), path, facts);
                        step = mark + 2;
                        continue;
                }

                log("%", 1);
                break;
        }

        log("\n", 1);
}

static fn statfs_readable(string_address path, file_mount_facts address_to facts)
{
        p8 text[64];

        log("  File: \"", 0);
        log(path, 0);
        log("\"\n    ID: ", 0);
        positive_to_base_field(log, statfs_identity(facts), 16, 16, -1,
                               (positive)1 << 28);
        log(" Namelen: ", 0);
        positive_into_string(text, facts->name_length);
        string_to_field(log, text, 8, ' ', true);
        log("Type: ", 0);
        log(statfs_type_name(facts->type), 0);

        log("\nBlock size: ", 0);
        positive_into_string(text, facts->block_size);
        string_to_field(log, text, 11, ' ', true);
        log("Fundamental block size: ", 0);
        positive_to_string(log, facts->fragment_size);

        log("\nBlocks: Total: ", 0);
        positive_into_string(text, facts->blocks);
        string_to_field(log, text, 11, ' ', true);
        log("Free: ", 0);
        positive_into_string(text, facts->blocks_free);
        string_to_field(log, text, 11, ' ', true);
        log("Available: ", 0);
        positive_to_string(log, facts->blocks_available);

        log("\nInodes: Total: ", 0);
        positive_into_string(text, facts->files);
        string_to_field(log, text, 11, ' ', true);
        log("Free: ", 0);
        positive_to_string(log, facts->files_free);
        log("\n", 1);
}

static fn stat_one_specifier(p8 letter, string_address path, file_facts address_to facts)
{
        p8 text[FILE_PATH_MAX];

        switch (letter)
        {
        case 'n':
                log(path, 0);
                return;

        case 'N':
                log("'", 1);
                log(path, 0);
                log("'", 1);

                if ((facts->mode & MODE_FORMAT) == MODE_LINK &&
                    file_link_text(path, text, FILE_PATH_MAX) >= 0)
                {
                        log(" -> '", 0);
                        log(text, 0);
                        log("'", 1);
                }

                return;

        case 's':
                return positive_to_string(log, facts->size);

        case 'b':
                return positive_to_string(log, facts->blocks);

        case 'B':
                return positive_to_string(log, 512);

        case 'a':
                return positive_to_base_field(log, facts->mode & 07777, 8, 1,
                                              -1, (positive)1 << 28);

        case 'A':
                file_mode_letters(text, facts->mode);
                return log(text, 10);

        case 'f':
                return positive_to_base_field(log, facts->mode, 16, 1,
                                              -1, (positive)1 << 28);

        case 'F':
                return log(file_kind_name(facts->mode), 0);

        case 'h':
                return positive_to_string(log, facts->hard_links);

        case 'i':
                return positive_to_string(log, facts->inode);

        case 'u':
                return positive_to_string(log, facts->owner);

        case 'g':
                return positive_to_string(log, facts->group);

        case 'U':
                if (file_user_name(facts->owner, text, FILE_NAME_MAX))
                        return log(text, 0);

                return positive_to_string(log, facts->owner);

        case 'G':
                if (file_group_name(facts->group, text, FILE_NAME_MAX))
                        return log(text, 0);

                return positive_to_string(log, facts->group);

        case 'o':
                return positive_to_string(log, facts->blocksize);

        case 'd':
                return positive_to_string(log, facts->device_major * 256 + facts->device_minor);

        case 't':
                return positive_to_base_field(log, facts->rdev_major, 16, 1,
                                              -1, (positive)1 << 28);

        case 'T':
                return positive_to_base_field(log, facts->rdev_minor, 16, 1,
                                              -1, (positive)1 << 28);

        case 'X':
                return positive_to_string(log, (positive)facts->accessed.seconds);

        case 'Y':
                return positive_to_string(log, (positive)facts->modified.seconds);

        case 'Z':
                return positive_to_string(log, (positive)facts->changed.seconds);

        case 'W':
                return positive_to_string(log, (facts->mask & STATX_BIRTH)
                                            ? (positive)facts->created.seconds
                                            : 0);

        case 'x':
                return file_stamp(log, facts->accessed.seconds, facts->accessed.nanoseconds);

        case 'y':
                return file_stamp(log, facts->modified.seconds, facts->modified.nanoseconds);

        case 'z':
                return file_stamp(log, facts->changed.seconds, facts->changed.nanoseconds);

        case 'w':
                if (!(facts->mask & STATX_BIRTH))
                        return log("-", 1);

                return file_stamp(log, facts->created.seconds, facts->created.nanoseconds);

        case '%':
                return log("%", 1);
        }

        log("?", 1);
}

/*
        -c is a format, not a printf: the system's own stat reads backslash
        escapes only under --printf, and a format that said \t would print
        those two characters. So does this one.
*/
static fn stat_formatted(string_address format, string_address path,
                         file_facts address_to facts)
{
        string_address step = format;

        while (string_get(step))
        {
                string_address mark = string_first_of_or_end(step, '%');

                if (mark != step)
                        log(step, (positive)(mark - step));

                if (!string_get(mark))
                        break;

                if (string_get(mark + 1))
                {
                        stat_one_specifier(string_get(mark + 1), path, facts);
                        step = mark + 2;
                        continue;
                }

                log("%", 1);
                break;
        }

        log("\n", 1);
}

static fn stat_readable(string_address path, file_facts address_to facts)
{
        p8 text[FILE_PATH_MAX];

        log("  File: ", 0);
        log(path, 0);

        if ((facts->mode & MODE_FORMAT) == MODE_LINK &&
            file_link_text(path, text, FILE_PATH_MAX) >= 0)
        {
                log(" -> ", 0);
                log(text, 0);
        }

        log("\n  Size: ", 0);
        positive_into_string(text, facts->size);
        string_to_field(log, text, 10, ' ', true);
        log("\tBlocks: ", 0);
        positive_into_string(text, facts->blocks);
        string_to_field(log, text, 10, ' ', true);
        log(" IO Block: ", 0);
        positive_into_string(text, facts->blocksize);
        string_to_field(log, text, 6, ' ', true);
        log(" ", 1);
        log(file_kind_name(facts->mode), 0);

        log("\nDevice: ", 0);
        positive_to_string(log, facts->device_major);
        log(",", 1);
        positive_to_string(log, facts->device_minor);
        log("\tInode: ", 0);
        positive_into_string(text, facts->inode);
        string_to_field(log, text, 10, ' ', true);
        log("  Links: ", 0);
        positive_to_string(log, facts->hard_links);

        log("\nAccess: (", 0);
        positive_to_base_field(log, facts->mode & 07777, 8, 4, -1,
                               (positive)1 << 28);
        log("/", 1);
        file_mode_letters(text, facts->mode);
        log(text, 10);
        log(")  Uid: (", 0);
        positive_to_padded(log, facts->owner, 5, ' ', 0);
        log("/", 1);

        if (!file_user_name(facts->owner, text, FILE_NAME_MAX))
                positive_into_string(text, facts->owner);

        string_to_field(log, text, 8, ' ', false);

        log(")   Gid: (", 0);
        positive_to_padded(log, facts->group, 5, ' ', 0);
        log("/", 1);

        if (!file_group_name(facts->group, text, FILE_NAME_MAX))
                positive_into_string(text, facts->group);

        string_to_field(log, text, 8, ' ', false);

        log(")\nAccess: ", 0);
        file_stamp(log, facts->accessed.seconds, facts->accessed.nanoseconds);
        log("\nModify: ", 0);
        file_stamp(log, facts->modified.seconds, facts->modified.nanoseconds);
        log("\nChange: ", 0);
        file_stamp(log, facts->changed.seconds, facts->changed.nanoseconds);
        log("\n Birth: ", 0);

        if (facts->mask & STATX_BIRTH)
                file_stamp(log, facts->created.seconds, facts->created.nanoseconds);
        else
                log("-", 1);

        log("\n", 1);
}

static const file_long stat_longs[] = {
    {(string_address) "dereference", 'L'},
    {(string_address) "file-system", 'f'},
    {(string_address) "format", 'c'},
    {null, 0},
};

static b32 file_stat()
{
        positive count = (positive)program_argument_count();
        stat_status = 0;
        file_taking taking = {
            .program = (string_address) "stat",
            .allowed = (string_address) "Lcf",
            .valued = (string_address) "c",
            .longs = stat_longs,
        };

        if (!file_take(address_of taking))
                return 1;

        positive index = taking.first;
        string_address format = file_option_value(address_of taking, 'c');

        stat_follow = (taking.flags & FILE_FLAG('L')) != 0;
        stat_file_system = (taking.flags & FILE_FLAG('f')) != 0;

        if (index >= count)
        {
                file_fail("stat: missing operand\n", 0);
                return 1;
        }

        while (index < count)
        {
                string_address path = program_argument((b32)index++);

                if (stat_file_system)
                {
                        file_mount_facts facts;
                        bipolar done = system_call_2(syscall(statfs), (positive)path,
                                                     (positive)address_of facts);

                        if (done < 0)
                        {
                                string_format(file_fail,
                                              "stat: cannot read file system information for '%s': %s\n",
                                              path, file_reason(done));
                                stat_status = 1;
                                continue;
                        }

                        if (format)
                                statfs_formatted(format, path, address_of facts);
                        else
                                statfs_readable(path, address_of facts);

                        continue;
                }

                file_facts facts;

                if (!file_look(AT_FDCWD, path, stat_follow ? 0 : AT_SYMLINK_NOFOLLOW,
                               address_of facts))
                {
                        string_format(file_fail,
                                      "stat: cannot statx '%s': No such file or directory\n",
                                      path);
                        stat_status = 1;
                        continue;
                }

                if (format)
                        stat_formatted(format, path, address_of facts);
                else
                        stat_readable(path, address_of facts);
        }

        log_flush();

        return stat_status;
}

// du ------------------------------------------------------------
/*
        du [-a] [-s] [-h] [-k] [-m] [-b] [-c] [-x] [-S] [-l] [-d N] [PATH...]

        What a file costs on the disk, not how long it is: the kernel's block
        count, which is what makes a sparse file cheap and a tiny file cost a
        whole block. -b is the other question, and asks for the length.

        A file with two names in the tree is one file and costs what one file
        costs, so the second name is passed over entirely -- no line for it
        under -a and nothing added to the total. -l is the flag for the other
        answer.
*/
static bool du_all;
static bool du_summary;
static bool du_human;
static bool du_apparent;
static bool du_total;
static bool du_separate;
static bool du_one_system;
static bool du_count_links;
static bool du_follow;
static positive du_unit = 1024;
static positive du_maximum = FILE_MAX_DEPTH;
static b32 du_status;
static p64 du_grand;
static p64 du_device;

static string_address address_to du_excludes;
static positive du_exclude_room;
static positive du_exclude_have;

// -S needs to know whether the cost that just came back was a directory's,
// and d_type is a hint some filesystems decline to give.
static bool du_was_directory;

typedef struct
{
        p64 inode;
        p64 device;
} du_seen_name;

// An inode of zero is not one the kernel hands out, so it is what an unused
// slot holds. The open-addressed table grows before it is half full, keeping
// repeated hard-link lookup constant-time without imposing a file ceiling.
static du_seen_name address_to du_seen;
static positive du_seen_room;
static positive du_seen_have;
static bool du_seen_broken;
static bool du_depth_broken;
static p8 du_unit_option;

static p64 du_where(file_facts address_to facts)
{
        return ((p64)facts->device_major << 32) | facts->device_minor;
}

static bool du_seen_grow()
{
        if (du_seen_room && du_seen_have + 1 < du_seen_room / 2)
                return true;

        positive room = du_seen_room ? du_seen_room << 1 : 64;

        if (room < du_seen_room || room > (positive)-1 / sizeof(du_seen_name))
                room = 0;

        du_seen_name address_to made = room
                                          ? (du_seen_name address_to)memory(
                                                room * sizeof(du_seen_name))
                                          : null;

        if (!made || (positive)made >= (positive)-4095)
        {
                shell_memory_failed = true;
                file_fail("du: out of memory while tracking hard links\n", 0);
                du_seen_broken = true;
                du_status = 1;
                return false;
        }

        memory_fill(made, 0, room * sizeof(du_seen_name));

        for (positive i = 0; i < du_seen_room; i++)
        {
                if (!du_seen[i].inode)
                        continue;

                positive at = (positive)(du_seen[i].inode * 1099511628211u +
                                         du_seen[i].device) &
                              (room - 1);

                while (made[at].inode)
                        at = (at + 1) & (room - 1);

                made[at] = du_seen[i];
        }

        if (du_seen)
                memory_free(du_seen, du_seen_room * sizeof(du_seen_name));

        du_seen = made;
        du_seen_room = room;

        return true;
}

static bool du_already(file_facts address_to facts)
{
        if (du_count_links || facts->hard_links < 2)
                return false;

        if ((facts->mode & MODE_FORMAT) == MODE_DIRECTORY)
                return false;

        if (!du_seen_grow())
                return true;

        p64 device = du_where(facts);
        positive slot = (positive)(facts->inode * 1099511628211u + device) &
                        (du_seen_room - 1);

        for (positive step = 0; step < du_seen_room; step++)
        {
                positive at = (slot + step) & (du_seen_room - 1);

                if (du_seen[at].inode == facts->inode && du_seen[at].device == device)
                        return true;

                if (du_seen[at].inode)
                        continue;

                du_seen[at].inode = facts->inode;
                du_seen[at].device = device;
                du_seen_have++;

                return false;
        }

        file_fail("du: hard-link table is unexpectedly full\n", 0);
        du_seen_broken = true;
        du_status = 1;

        /* Counting it again would be the silent over-count this table avoids. */
        return true;
}

// The system's du takes a pattern against the whole path it built and
// against the last component of it, so --exclude=b and --exclude=a/b both
// leave out a/b.
static bool du_excluded(string_address path)
{
        p8 name[FILE_PATH_MAX];

        if (!du_exclude_have)
                return false;

        path_tail_copy(name, FILE_PATH_MAX, path);

        for (positive i = 0; i < du_exclude_have; i++)
                if (shell_match(du_excludes[i], path) || shell_match(du_excludes[i], name))
                        return true;

        return false;
}

static fn du_report(p64 bytes, string_address path)
{
        if (du_human)
                positive_to_human_1024(log, bytes);
        else
                positive_to_string(log, bytes / du_unit + (bytes % du_unit != 0));

        log("\t", 1);
        log(path, 0);
        log("\n", 1);
}

// Returns what the tree costs, and prints the parts of it that were asked for
// on the way back up, which is the order du has always reported in.
static p64 du_walk(string_address path, positive depth, bool named, positive level)
{
        file_facts facts;

        if (!(du_follow ? file_look_at(path, address_of facts)
                        : file_look_link(path, address_of facts)))
        {
                string_format(file_fail, "du: cannot access '%s': No such file or directory\n",
                              path);
                du_status = 1;
                du_was_directory = false;
                return 0;
        }

        du_was_directory = (facts.mode & MODE_FORMAT) == MODE_DIRECTORY;

        if (named)
                du_device = du_where(address_of facts);
        else if (du_one_system && du_where(address_of facts) != du_device)
                return 0;

        if (du_already(address_of facts))
                return 0;

        p64 mine = du_apparent ? (p64)facts.size : facts.blocks * 512;

        // --apparent-size is asking how much was written, and nothing was
        // written into the directory itself; only what is under it counts.
        if (du_apparent && (facts.mode & MODE_FORMAT) == MODE_DIRECTORY)
                mine = 0;

        if ((facts.mode & MODE_FORMAT) != MODE_DIRECTORY)
        {
                if ((du_all || named) && level <= du_maximum)
                        du_report(mine, path);

                du_was_directory = false;

                return mine;
        }

        p64 total = mine;
        p64 below = 0;

        if (depth > 0)
        {
                file_walk walk;

                if (file_walk_open(address_of walk, AT_FDCWD, path))
                {
                        struct linux_dirent64 address_to entry;

                        while ((entry = file_walk_next(address_of walk)))
                        {
                                if (file_is_dot(entry->d_name))
                                        continue;

                                p8 under[FILE_PATH_MAX];

                                path_join(under, FILE_PATH_MAX, path, entry->d_name);

                                if (du_excluded(under))
                                        continue;

                                p64 cost = du_walk(under, depth - 1, false, level + 1);

                                if (du_seen_broken || du_depth_broken)
                                        break;

                                total += cost;

                                if (du_was_directory)
                                        below += cost;
                        }

                        file_walk_close(address_of walk);
                }
                else
                {
                        string_format(file_fail, "du: cannot read directory '%s'\n", path);
                        du_status = 1;
                }
        }
        else
        {
                file_walk walk;

                if (file_walk_open(address_of walk, AT_FDCWD, path))
                {
                        struct linux_dirent64 address_to entry;

                        while ((entry = file_walk_next(address_of walk)))
                        {
                                if (file_is_dot(entry->d_name))
                                        continue;

                                file_fail("du: tree is nested too deep\n", 0);
                                du_depth_broken = true;
                                du_status = 1;
                                break;
                        }

                        file_walk_close(address_of walk);
                }
        }

        if (du_seen_broken || du_depth_broken)
        {
                du_was_directory = true;
                return 0;
        }

        if (level <= du_maximum)
                du_report(du_separate ? total - below : total, path);

        du_was_directory = true;

        return total;
}

static bool du_exclude_seen(p8 letter, string_address value)
{
        if (letter == 'b' || letter == 'k' || letter == 'm')
                du_unit_option = letter;

        if (letter != 'e' || !value)
                return true;

        if (!shell_room((address_any address_to)address_of du_excludes,
                        address_of du_exclude_room, du_exclude_have + 1,
                        sizeof(string_address)))
        {
                file_fail("du: out of memory while reading exclude patterns\n", 0);
                return false;
        }

        du_excludes[du_exclude_have++] = value;

        return true;
}

static const file_long du_longs[] = {
    {(string_address) "all", 'a'},
    {(string_address) "apparent-size", 'A'},
    {(string_address) "bytes", 'b'},
    {(string_address) "count-links", 'l'},
    {(string_address) "dereference", 'L'},
    {(string_address) "exclude", 'e'},
    {(string_address) "human-readable", 'h'},
    {(string_address) "max-depth", 'd'},
    {(string_address) "one-file-system", 'x'},
    {(string_address) "separate-dirs", 'S'},
    {(string_address) "summarize", 's'},
    {(string_address) "total", 'c'},
    {null, 0},
};

static b32 file_du()
{
        positive count = (positive)program_argument_count();

        du_status = 0;
        du_grand = 0;
        du_unit = 1024;
        du_maximum = FILE_MAX_DEPTH;
        du_exclude_have = 0;
        du_seen_have = 0;
        du_seen_broken = false;
        du_depth_broken = false;
        du_unit_option = 0;
        if (du_seen_room)
                memory_fill(du_seen, 0, du_seen_room * sizeof(du_seen_name));

        file_taking taking = {
            .program = (string_address) "du",
            .allowed = (string_address) "abcdhklLmsSx",
            .valued = (string_address) "de",
            .longs = du_longs,
            .seen = du_exclude_seen,
        };

        if (!file_take(address_of taking))
                return 1;

        positive flags = taking.flags;
        positive first = taking.first;

        du_all = (flags & FILE_FLAG('a')) != 0;
        du_summary = (flags & FILE_FLAG('s')) != 0;
        du_human = (flags & FILE_FLAG('h')) != 0;
        du_apparent = (flags & (FILE_FLAG('b') | FILE_FLAG('A'))) != 0;
        du_total = (flags & FILE_FLAG('c')) != 0;
        du_separate = (flags & FILE_FLAG('S')) != 0;
        du_one_system = (flags & FILE_FLAG('x')) != 0;
        du_count_links = (flags & FILE_FLAG('l')) != 0;
        du_follow = (flags & FILE_FLAG('L')) != 0;

        if (du_unit_option == 'b')
                du_unit = 1;
        else if (du_unit_option == 'm')
                du_unit = 1048576;

        if (du_summary && (du_all || (flags & FILE_FLAG('d'))))
        {
                file_fail("du: summarizing conflicts with --all or --max-depth\n", 0);
                return 1;
        }

        // -s is --max-depth=0 said another way, and the two are the same
        // switch here so that giving both cannot mean two things.
        if (du_summary)
                du_maximum = 0;

        if (flags & FILE_FLAG('d'))
        {
                positive maximum;
                string_address written = file_option_value(address_of taking, 'd');
                bool negative = string_is(written, '-');

                if (negative || string_is(written, '+'))
                        written++;

                if (!string_digits_exact(written, address_of maximum))
                {
                        file_fail("du: invalid maximum depth\n", 0);
                        return 1;
                }

                du_maximum = negative ? 0 : maximum;
        }

        if (first >= count)
        {
                du_grand += du_walk((string_address) ".", FILE_MAX_DEPTH, true, 0);
        }
        else
        {
                while (first < count && !du_seen_broken && !du_depth_broken)
                        du_grand += du_walk(program_argument((b32)first++),
                                            FILE_MAX_DEPTH, true, 0);
        }

        if (du_total && !du_seen_broken && !du_depth_broken)
                du_report(du_grand, (string_address) "total");

        log_flush();

        return du_status;
}

// df ------------------------------------------------------------
/*
        df [-h] [-i] [-T] [-a] [-P] [PATH...]

        The mounted filesystems come from /proc/mounts, because the kernel is
        the only thing that knows what is mounted and this is where it says
        so. A filesystem with no blocks at all is one of the kernel's own
        bookkeeping mounts and is left out unless -a asks for it, the way df
        has always left it out.

        The whole table is measured before any of it is written: each column
        ends up as wide as the widest thing under it and no wider, which is
        why the file is read twice and why the second pass prints.
*/
#define DF_TEXT (1 << 18)

static p8 df_text[DF_TEXT];
static bool df_human;
static bool df_inodes;
static bool df_types;
static bool df_all;
static bool df_posix;

static positive df_device_width;
static positive df_type_width;
static positive df_full_width;
static positive df_blocks_width;
static positive df_used_width;
static positive df_free_width;

// The kernel counts in whatever unit the filesystem uses; df has always
// reported in 1024 byte ones, and rounds a part of one up to a whole. An
// inode is not a byte and is reported as the number it is.
static positive df_amount(p8 address_to into, p64 blocks, p64 size)
{
        p64 bytes = blocks * size;

        if (!df_human)
                return df_inodes ? positive_into_string(into, blocks)
                                 : positive_into_string(
                                       into, bytes / 1024 + (bytes % 1024 != 0));

        return positive_into_human_1024_string(into, bytes);
}

static fn df_column(p8 address_to text, positive width)
{
        string_to_field(log, text, width, ' ', false);
        log(" ", 1);
}

// What is being measured: blocks by default, and the inode table under -i.
static fn df_reading(file_mount_facts address_to facts, p64 address_to total,
                     p64 address_to used, p64 address_to spare, p64 address_to size)
{
        if (df_inodes)
        {
                address_to size = 1;
                address_to total = facts->files;
                address_to used = facts->files - facts->files_free;
                address_to spare = facts->files_free;

                return;
        }

        address_to size = (p64)(facts->fragment_size ? facts->fragment_size
                                                     : facts->block_size);
        address_to total = facts->blocks;
        address_to used = facts->blocks - facts->blocks_free;
        address_to spare = facts->blocks_available;
}

static fn df_row(string_address device, string_address type, string_address where,
                 file_mount_facts address_to facts, bool measured)
{
        p64 total, used, spare, size;
        p8 text[64];
        string_address dash = (string_address) "-";

        df_reading(facts, address_of total, address_of used, address_of spare,
                   address_of size);

        string_to_field(log, device, df_device_width, ' ', true);
        log(" ", 1);

        if (df_types)
        {
                string_to_field(log, measured ? type : dash, df_type_width, ' ', true);
                log(" ", 1);
        }

        for (positive column = 0; column < 3; column++)
        {
                positive width = column == 0   ? df_blocks_width
                                 : column == 1 ? df_used_width
                                               : df_free_width;

                if (!measured)
                {
                        df_column(dash, width);
                        continue;
                }

                df_amount(text, column == 0 ? total : column == 1 ? used : spare, size);
                df_column(text, width);
        }

        p64 wanted = used + spare;

        // A filesystem with nothing in it to fill has no proportion full, and
        // saying nought percent would be an answer where there is none.
        if (!measured || !wanted)
                df_column(dash, df_full_width);
        else
        {
                positive_to_padded(log,
                                   (positive)((used * 100 + wanted - 1) / wanted),
                                   df_full_width - 1, ' ', 0);
                log("% ", 2);
        }

        log(where, 0);
        log("\n", 1);
}

// Every line of /proc/mounts is device, mount point, type, options; the first
// three are all df needs and both names may carry \040 where a space was.
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

static const file_long df_longs[] = {
    {(string_address) "all", 'a'},
    {(string_address) "human-readable", 'h'},
    {(string_address) "inodes", 'i'},
    {(string_address) "portability", 'P'},
    {(string_address) "print-type", 'T'},
    {null, 0},
};

static b32 file_df()
{
        positive count = (positive)program_argument_count();
        file_taking taking = {
            .program = (string_address) "df",
            .allowed = (string_address) "ahikPT",
            .valued = (string_address) "",
            .longs = df_longs,
        };

        if (!file_take(address_of taking))
                return 1;

        positive first = taking.first;

        df_human = (taking.flags & FILE_FLAG('h')) != 0;
        df_inodes = (taking.flags & FILE_FLAG('i')) != 0;
        df_types = (taking.flags & FILE_FLAG('T')) != 0;
        df_all = (taking.flags & FILE_FLAG('a')) != 0;
        df_posix = (taking.flags & FILE_FLAG('P')) != 0;

        if (file_slurp((string_address) "/proc/mounts", df_text, DF_TEXT) <= 0 &&
            file_slurp((string_address) "/proc/self/mounts", df_text, DF_TEXT) <= 0)
        {
                file_fail("df: cannot read /proc/mounts\n", 0);
                return 1;
        }

        string_address blocks_heading = df_inodes ? (string_address) "Inodes"
                                        : df_human ? (string_address) "Size"
                                        : df_posix ? (string_address) "1024-blocks"
                                                   : (string_address) "1K-blocks";
        string_address used_heading = df_inodes ? (string_address) "IUsed"
                                                : (string_address) "Used";
        string_address free_heading = df_inodes  ? (string_address) "IFree"
                                      : df_human ? (string_address) "Avail"
                                                 : (string_address) "Available";
        string_address full_heading = df_inodes ? (string_address) "IUse%"
                                      : df_posix && !df_human ? (string_address) "Capacity"
                                                              : (string_address) "Use%";

        /*
                Each column is the widest of three things: a floor the column
                has whatever is in it, the heading, and the widest value. The
                floors are what keep "df /tmp" and "df" lining their tables up
                the same way, and they are the system's own: fourteen for the
                filesystem, five for each amount, four for the percentage.
        */
        df_device_width = 14;
        df_type_width = 4;
        df_blocks_width = 5;
        df_used_width = 5;
        df_free_width = 5;

        if (string_length(blocks_heading) > df_blocks_width)
                df_blocks_width = string_length(blocks_heading);

        if (string_length(used_heading) > df_used_width)
                df_used_width = string_length(used_heading);

        if (string_length(free_heading) > df_free_width)
                df_free_width = string_length(free_heading);

        df_full_width = string_length(full_heading);

        bool filtering = first < count;

        for (positive pass = 0; pass < 2; pass++)
        {
                if (pass == 1)
                {
                        string_to_field(log, (string_address) "Filesystem",
                                        df_device_width, ' ', true);
                        log(" ", 1);

                        if (df_types)
                        {
                                string_to_field(log, (string_address) "Type",
                                                df_type_width, ' ', true);
                                log(" ", 1);
                        }

                        df_column(blocks_heading, df_blocks_width);
                        df_column(used_heading, df_used_width);
                        df_column(free_heading, df_free_width);
                        log(full_heading, 0);
                        log(" Mounted on\n", 0);
                }

                positive at = 0;

                while (df_text[at])
                {
                        p8 device[FILE_PATH_MAX];
                        p8 where[FILE_PATH_MAX];
                        p8 type[FILE_NAME_MAX];
                        p8 text[64];

                        at = df_field(df_text, at, device, FILE_PATH_MAX);
                        at = df_field(df_text, at, where, FILE_PATH_MAX);
                        at = df_field(df_text, at, type, FILE_NAME_MAX);

                        at = (positive)(string_first_of_or_end(df_text + at, '\n') - df_text);

                        if (df_text[at])
                                at++;

                        if (!string_get(where))
                                continue;

                        file_mount_facts facts;

                        memory_fill(address_of facts, 0, sizeof(facts));

                        /*
                                An automount point is not asked how full it
                                is: asking is what mounts it, and df is a
                                question and not an instruction.

                                A mount under a directory this user cannot
                                walk into is there and is not measurable, and
                                is said with dashes. Anything else that will
                                not answer is a fault rather than a wall, and
                                is complained about and left out.
                        */
                        bool automount = string_compare(type, "autofs") == 0;
                        bipolar answered = automount
                                               ? -ERROR_ACCESS
                                               : system_call_2(syscall(statfs),
                                                               (positive)where,
                                                               (positive)address_of facts);

                        if (answered < 0 && answered != -ERROR_ACCESS)
                        {
                                string_format(file_fail, "df: %s: %s\n", where,
                                              file_reason(answered));
                                continue;
                        }

                        bool measured = answered >= 0;

                        if (!measured && !df_all)
                                continue;

                        if (facts.blocks == 0 && !df_all)
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
                                df_row(device, type, where, address_of facts, measured);
                                continue;
                        }

                        p64 total, used, spare, size;

                        df_reading(address_of facts, address_of total, address_of used,
                                   address_of spare, address_of size);

                        if (string_length(device) > df_device_width)
                                df_device_width = string_length(device);

                        if (measured && string_length(type) > df_type_width)
                                df_type_width = string_length(type);

                        if (!measured)
                                continue;

                        positive length = df_amount(text, total, size);

                        if (length > df_blocks_width)
                                df_blocks_width = length;

                        length = df_amount(text, used, size);

                        if (length > df_used_width)
                                df_used_width = length;

                        length = df_amount(text, spare, size);

                        if (length > df_free_width)
                                df_free_width = length;
                }
        }

        log_flush();

        return 0;
}

// chmod ------------------------------------------------------------
// chmod [-R] MODE FILE..., with MODE octal or symbolic.
static string_address chmod_specification;
static b32 chmod_status;

static bool chmod_loud;
static bool chmod_changes;
static bool chmod_quiet;
static bool chmod_referenced;
static positive chmod_reference_mode;

static fn chmod_mode_said(positive mode)
{
        p8 letters[12];

        positive_to_base_field(log, mode & 07777, 8, 4, -1,
                               (positive)1 << 28);
        file_mode_letters(letters, mode);
        log(" (", 2);
        log(letters + 1, 9);
        log(")", 1);
}

static fn chmod_said(string_address shown, positive was, positive now)
{
        if (!chmod_loud && !(chmod_changes && (was & 07777) != (now & 07777)))
                return;

        string_format(log, "mode of '%s' ", shown);

        if ((was & 07777) == (now & 07777))
        {
                log("retained as ", 0);
                chmod_mode_said(now);
                log("\n", 1);
                return;
        }

        log("changed from ", 0);
        chmod_mode_said(was);
        log(" to ", 4);
        chmod_mode_said(now);
        log("\n", 1);
}

static bool chmod_one(bipolar directory, string_address name, string_address shown)
{
        file_facts facts;

        // Following the link, because Linux has no mode on a symlink of its
        // own to change and chmod has always meant the thing pointed at.
        if (!file_look(directory, name, 0, address_of facts))
        {
                if (!chmod_quiet)
                        string_format(file_fail,
                                      "chmod: cannot access '%s': No such file or directory\n",
                                      shown);

                chmod_status = 1;
                return false;
        }

        positive wanted = chmod_reference_mode & 07777;

        if (!chmod_referenced &&
            !file_mode_of(chmod_specification, facts.mode,
                          (facts.mode & MODE_FORMAT) == MODE_DIRECTORY, address_of wanted))
        {
                if (!chmod_quiet)
                        string_format(file_fail, "chmod: invalid mode: %s\n",
                                      chmod_specification);

                chmod_status = 1;
                return false;
        }

        bipolar done = system_call_4(syscall(fchmodat), directory, (positive)name, wanted, 0);

        if (done < 0)
        {
                if (!chmod_quiet)
                        string_format(file_fail, "chmod: changing permissions of '%s': %s\n",
                                      shown, file_reason(done));

                chmod_status = 1;
                return false;
        }

        chmod_said(shown, facts.mode, wanted | (facts.mode & MODE_FORMAT));

        return true;
}

static fn chmod_walk(bipolar directory, string_address name, string_address shown,
                     positive depth)
{
        chmod_one(directory, name, shown);

        // is_directory here asks about the link itself, so a link to a
        // directory is changed and not walked into.
        if (depth == 0 || !file_is_directory(directory, name))
                return;

        file_walk walk;

        if (!file_walk_open(address_of walk, directory, name))
                return;

        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                p8 below[FILE_PATH_MAX];

                path_join(below, FILE_PATH_MAX, shown, entry->d_name);
                chmod_walk(walk.handle, entry->d_name, below, depth - 1);
        }

        file_walk_close(address_of walk);
}

static const file_long chmod_longs[] = {
    {(string_address) "changes", 'c'},
    {(string_address) "quiet", 'f'},
    {(string_address) "recursive", 'R'},
    {(string_address) "reference", 'e'},
    {(string_address) "silent", 'f'},
    {(string_address) "verbose", 'v'},
    {null, 0},
};

static b32 file_chmod()
{
        positive count = (positive)program_argument_count();
        chmod_status = 0;
        chmod_referenced = false;

        file_taking taking = {
            .program = (string_address) "chmod",
            .allowed = (string_address) "Rcfv",
            .valued = (string_address) "e",
            .longs = chmod_longs,
        };

        if (!file_take(address_of taking))
                return 1;

        positive first = taking.first;

        chmod_loud = (taking.flags & FILE_FLAG('v')) != 0;
        chmod_changes = (taking.flags & FILE_FLAG('c')) != 0;
        chmod_quiet = (taking.flags & FILE_FLAG('f')) != 0;

        string_address like = file_option_value(address_of taking, 'e');

        // --reference says the mode without spelling it, and takes the place
        // of the mode operand rather than standing beside it.
        if (like)
        {
                file_facts facts;

                if (!file_look_at(like, address_of facts))
                {
                        string_format(file_fail,
                                      "chmod: cannot access '%s': No such file or directory\n",
                                      like);
                        return 1;
                }

                chmod_referenced = true;
                chmod_reference_mode = facts.mode;
        }

        if (first >= count || (!chmod_referenced && first + 1 >= count))
        {
                file_fail("chmod: missing operand\n", 0);
                return 1;
        }

        if (!chmod_referenced)
                chmod_specification = program_argument((b32)first++);

        while (first < count)
        {
                string_address path = program_argument((b32)first++);

                if (taking.flags & FILE_FLAG('R'))
                        chmod_walk(AT_FDCWD, path, path, FILE_MAX_DEPTH);
                else
                        chmod_one(AT_FDCWD, path, path);
        }

        log_flush();

        return chmod_status;
}

// chown and chgrp --------------------------------------------------
// Both names use the same ownership walker.  chown accepts USER[:GROUP]
// (and a USER of nothing so that ":group" changes only the group); chgrp's
// first operand names only a group.
static bipolar chown_user = -1;
static bipolar chown_group = -1;
static b32 chown_status;
static positive chown_flags;
static bool chown_loud;
static bool chown_changes;
static bool chown_quiet;
static p8 chown_dereference_option;
static string_address chown_program;
static bool chown_groups_only;

static bool chown_option_seen(p8 letter, string_address value)
{
        (void)value;

        if (letter == 'd' || letter == 'h')
                chown_dereference_option = letter;

        return true;
}

// Who a file will belong to, said the way chown says it: the user alone when
// only a user was named, and user:group when a group was.
static fn chown_who(positive user, positive group, p8 address_to into)
{
        positive length = 0;

        if (chown_groups_only)
        {
                if (!file_group_name(group, into, FILE_NAME_MAX))
                        positive_into_string(into, group);

                return;
        }

        if (!file_user_name(user, into, FILE_NAME_MAX))
                length = positive_into_string(into, user);
        else
                length = string_length(into);

        if (chown_group < 0)
                return;

        into[length++] = ':';

        if (!file_group_name(group, into + length, FILE_NAME_MAX))
                positive_into_string(into + length, group);
}

static fn chown_said(string_address shown, file_facts address_to was, bool changed)
{
        if (!chown_loud && !(chown_changes && changed))
                return;

        p8 who[FILE_PATH_MAX];

        if (!changed)
        {
                chown_who(was->owner, was->group, who);
                string_format(log, chown_groups_only ? "group of '%s' retained as %s\n"
                                                     : "ownership of '%s' retained as %s\n",
                              shown, who);
                return;
        }

        p8 before[FILE_PATH_MAX];

        chown_who(was->owner, was->group, before);
        chown_who(chown_user < 0 ? was->owner : (positive)chown_user,
                  chown_group < 0 ? was->group : (positive)chown_group, who);

        string_format(log, chown_groups_only ? "changed group of '%s' from %s to %s\n"
                                             : "changed ownership of '%s' from %s to %s\n",
                      shown, before, who);
}

static fn chown_one(bipolar directory, string_address name, string_address shown)
{
        positive through = chown_dereference_option == 'h' ? AT_SYMLINK_NOFOLLOW : 0;
        file_facts facts;
        bool known = file_look(directory, name, through, address_of facts);

        bipolar done = system_call_5(syscall(fchownat), directory, (positive)name,
                                     (positive)chown_user, (positive)chown_group, through);

        if (done < 0)
        {
                if (!chown_quiet)
                        string_format(file_fail, "%s: changing ownership of '%s': %s\n",
                                      chown_program, shown, file_reason(done));

                chown_status = 1;
                return;
        }

        if (!known)
                return;

        bool changed = (chown_user >= 0 && facts.owner != (positive)chown_user) ||
                       (chown_group >= 0 && facts.group != (positive)chown_group);

        chown_said(shown, address_of facts, changed);
}

static fn chown_walk(bipolar directory, string_address name, string_address shown,
                     positive depth)
{
        chown_one(directory, name, shown);

        if (depth == 0 || !file_is_directory(directory, name))
                return;

        file_walk walk;

        if (!file_walk_open(address_of walk, directory, name))
                return;

        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                p8 below[FILE_PATH_MAX];

                path_join(below, FILE_PATH_MAX, shown, entry->d_name);
                chown_walk(walk.handle, entry->d_name, below, depth - 1);
        }

        file_walk_close(address_of walk);
}

static const file_long chown_longs[] = {
    {(string_address) "changes", 'c'},
    {(string_address) "dereference", 'd'},
    {(string_address) "no-dereference", 'h'},
    {(string_address) "quiet", 'f'},
    {(string_address) "recursive", 'R'},
    {(string_address) "reference", 'e'},
    {(string_address) "silent", 'f'},
    {(string_address) "verbose", 'v'},
    {null, 0},
};

static fn chown_paths(positive first, positive count)
{
        while (first < count)
        {
                string_address path = program_argument((b32)first++);

                if (chown_flags & FILE_FLAG('R'))
                        chown_walk(AT_FDCWD, path, path, FILE_MAX_DEPTH);
                else
                        chown_one(AT_FDCWD, path, path);
        }

        log_flush();
}

static b32 file_chown_common(string_address program, bool groups_only)
{
        positive count = (positive)program_argument_count();
        chown_user = -1;
        chown_group = -1;
        chown_status = 0;
        chown_dereference_option = 'd';
        chown_program = program;
        chown_groups_only = groups_only;

        file_taking taking = {
            .program = program,
            .allowed = (string_address) "Rcfhv",
            .valued = (string_address) "e",
            .longs = chown_longs,
            .seen = chown_option_seen,
        };

        if (!file_take(address_of taking))
                return 1;

        positive first = taking.first;

        chown_flags = taking.flags;
        chown_loud = (taking.flags & FILE_FLAG('v')) != 0;
        chown_changes = (taking.flags & FILE_FLAG('c')) != 0;
        chown_quiet = (taking.flags & FILE_FLAG('f')) != 0;

        string_address like = file_option_value(address_of taking, 'e');

        if (like)
        {
                file_facts facts;

                if (!file_look_at(like, address_of facts))
                {
                        string_format(file_fail,
                                      "%s: cannot access '%s': No such file or directory\n",
                                      program, like);
                        return 1;
                }

                if (!groups_only)
                        chown_user = (bipolar)facts.owner;

                chown_group = (bipolar)facts.group;
        }

        if (first >= count || (!like && first + 1 >= count))
        {
                string_format(file_fail, "%s: missing operand\n", program);
                return 1;
        }

        if (like)
        {
                chown_paths(first, count);

                return chown_status;
        }

        string_address who = program_argument((b32)first++);

        if (groups_only)
        {
                positive number;

                chown_group = string_digits_exact(who, address_of number)
                                  ? (bipolar)number
                                  : file_group_id(who);

                if (chown_group < 0)
                {
                        string_format(file_fail, "%s: invalid group: %s\n", program, who);
                        return 1;
                }

                chown_paths(first, count);

                return chown_status;
        }

        p8 user[FILE_NAME_MAX];
        positive length = 0;

        while (string_get(who + length) && !string_is(who + length, ':') &&
               !string_is(who + length, '.') && length + 1 < FILE_NAME_MAX)
        {
                user[length] = string_get(who + length);
                length++;
        }

        user[length] = end;

        string_address group = null;

        if (string_is(who + length, ':') || string_is(who + length, '.'))
                group = who + length + 1;

        if (length > 0)
        {
                positive number;

                chown_user = string_digits_exact(user, address_of number)
                                 ? (bipolar)number
                                 : file_user_id(user);

                if (chown_user < 0)
                {
                        string_format(file_fail, "%s: invalid user: %s\n", program, who);
                        return 1;
                }
        }

        if (group && string_get(group))
        {
                positive number;

                chown_group = string_digits_exact(group, address_of number)
                                  ? (bipolar)number
                                  : file_group_id(group);

                if (chown_group < 0)
                {
                        string_format(file_fail, "%s: invalid group: %s\n", program, who);
                        return 1;
                }
        }

        chown_paths(first, count);

        return chown_status;
}

static b32 file_chown()
{
        return file_chown_common((string_address) "chown", false);
}

static b32 file_chgrp()
{
        return file_chown_common((string_address) "chgrp", true);
}

// ln ------------------------------------------------------------
// ln [-s] [-f] TARGET [NAME], and ln [-s] [-f] TARGET... DIRECTORY.
static bool ln_symbolic;
static bool ln_force;
static bool ln_ask;
static bool ln_loud;
static bool ln_relative;
static bool ln_through;
static p8 ln_collision_option;
static p8 ln_dereference_option;

static bool ln_option_seen(p8 letter, string_address value)
{
        (void)value;

        if (letter == 'f' || letter == 'i')
                ln_collision_option = letter;

        if (letter == 'L' || letter == 'P')
                ln_dereference_option = letter;

        return true;
}

// realpath's, and named here because ln is written before it.
static fn realpath_relative(string_address from, string_address path, p8 address_to into);

// -r says where the target is from where the link will sit rather than from
// here, which is the only spelling of a symbolic link that survives the whole
// tree being moved somewhere else.
static fn ln_relative_text(string_address target, string_address name,
                           p8 address_to into)
{
        p8 there[FILE_PATH_MAX];
        p8 here[FILE_PATH_MAX];
        p8 above[FILE_PATH_MAX];

        if (!file_resolve(target, there, true) || !file_resolve(name, here, false))
        {
                string_copy_max_end(into, target, FILE_PATH_MAX - 1);
                return;
        }

        path_head_copy(above, FILE_PATH_MAX, here);
        realpath_relative(above, there, into);
}

static bool ln_make(string_address target, string_address name)
{
        p8 relative[FILE_PATH_MAX];

        if (ln_relative && ln_symbolic)
        {
                ln_relative_text(target, name, relative);
                target = relative;
        }

        if (ln_ask && file_exists(AT_FDCWD, name) &&
            !file_ask((string_address) "ln", (string_address) "replace", name))
                return false;

        if (ln_force || ln_ask)
                system_call_3(syscall(unlinkat), AT_FDCWD, (positive)name, 0);

        bipolar done;

        if (ln_symbolic)
                done = system_call_3(syscall(symlinkat), (positive)target, AT_FDCWD,
                                     (positive)name);
        else
                done = system_call_5(syscall(linkat), AT_FDCWD, (positive)target,
                                     AT_FDCWD, (positive)name,
                                     ln_through ? AT_SYMLINK_FOLLOW : 0);

        if (done < 0)
        {
                string_format(file_fail, "ln: failed to create %s link '%s': %s\n",
                              ln_symbolic ? (string_address) "symbolic"
                                          : (string_address) "hard",
                              name, file_reason(done));
                return false;
        }

        if (ln_loud)
                string_format(log, "'%s' -> '%s'\n", name, target);

        return true;
}

static const file_long ln_longs[] = {
    {(string_address) "force", 'f'},
    {(string_address) "interactive", 'i'},
    {(string_address) "logical", 'L'},
    {(string_address) "no-dereference", 'n'},
    {(string_address) "no-target-directory", 'T'},
    {(string_address) "physical", 'P'},
    {(string_address) "relative", 'r'},
    {(string_address) "symbolic", 's'},
    {(string_address) "target-directory", 't'},
    {(string_address) "verbose", 'v'},
    {null, 0},
};

static b32 file_ln()
{
        positive count = (positive)program_argument_count();
        ln_collision_option = 0;
        ln_dereference_option = 0;

        file_taking taking = {
            .program = (string_address) "ln",
            .allowed = (string_address) "fiLnPrstTv",
            .valued = (string_address) "t",
            .longs = ln_longs,
            .seen = ln_option_seen,
        };

        if (!file_take(address_of taking))
                return 1;

        positive flags = taking.flags;
        positive first = taking.first;

        ln_symbolic = (flags & FILE_FLAG('s')) != 0;
        ln_force = ln_collision_option == 'f';
        ln_ask = ln_collision_option == 'i';
        ln_loud = (flags & FILE_FLAG('v')) != 0;
        ln_relative = (flags & FILE_FLAG('r')) != 0;

        // -L makes a hard link to what a symbolic target points at rather
        // than to the link, which is the one thing -L and -P are about.
        ln_through = ln_dereference_option == 'L';

        if (first >= count)
        {
                file_fail("ln: missing operand\n", 0);
                return 1;
        }

        string_address into = file_option_value(address_of taking, 't');
        bool alone = (flags & FILE_FLAG('T')) != 0;

        if (into && alone)
        {
                file_fail("ln: cannot combine --target-directory and --no-target-directory\n", 0);
                return 1;
        }

        // -n is about the destination and not the target: a link that already
        // points at a directory is a thing to replace rather than a directory
        // to link into.
        bool through = (flags & FILE_FLAG('n')) == 0;

        if (!into && count - first == 1)
        {
                // One operand links into the working directory under the
                // target's own last component.
                string_address target = program_argument((b32)first);
                p8 name[FILE_PATH_MAX];

                path_tail_copy(name, FILE_PATH_MAX, target);

                return ln_make(target, name) ? 0 : 1;
        }

        string_address last = into ? into : program_argument((b32)(count - 1));
        positive after = into ? count : count - 1;
        bool directory = into || (through ? file_is_directory_through(last)
                                          : file_is_directory(AT_FDCWD, last));

        if (alone || !directory)
        {
                if (after - first != 1)
                {
                        string_format(file_fail, "ln: target '%s' is not a directory\n", last);
                        return 1;
                }

                return ln_make(program_argument((b32)first), last) ? 0 : 1;
        }

        b32 status = 0;

        while (first < after)
        {
                string_address target = program_argument((b32)first++);
                p8 tail[FILE_PATH_MAX];
                p8 name[FILE_PATH_MAX];

                path_tail_copy(tail, FILE_PATH_MAX, target);
                path_join(name, FILE_PATH_MAX, last, tail);

                if (!ln_make(target, name))
                        status = 1;
        }

        log_flush();

        return status;
}

// readlink ------------------------------------------------------------
/*
        readlink [-f|-e|-m] [-n] [-q] [-z] FILE...

        With none of -f, -e and -m it reads the one link it is given and says
        nothing about the rest of the path; those three resolve the whole name
        and differ only in how much of it has to be there.
*/
static const file_long readlink_longs[] = {
    {(string_address) "canonicalize", 'f'},
    {(string_address) "canonicalize-existing", 'e'},
    {(string_address) "canonicalize-missing", 'm'},
    {(string_address) "no-newline", 'n'},
    {(string_address) "quiet", 'q'},
    {(string_address) "silent", 's'},
    {(string_address) "verbose", 'v'},
    {(string_address) "zero", 'z'},
    {null, 0},
};

static p8 readlink_canonical_option;

static bool readlink_option_seen(p8 letter, string_address value)
{
        (void)value;

        if (letter == 'f' || letter == 'e' || letter == 'm')
                readlink_canonical_option = letter;

        return true;
}

static b32 file_readlink()
{
        readlink_canonical_option = 0;

        file_taking taking = {
            .program = (string_address) "readlink",
            .allowed = (string_address) "fneqsvmz",
            .valued = (string_address) "",
            .longs = readlink_longs,
            .seen = readlink_option_seen,
        };

        if (!file_take(address_of taking))
                return 1;

        positive first = taking.first;
        positive count = (positive)program_argument_count();
        positive flags = taking.flags;

        if (first >= count)
        {
                file_fail("readlink: missing operand\n", 0);
                return 1;
        }

        bool resolve = readlink_canonical_option != 0;
        bool no_newline = (flags & FILE_FLAG('n')) != 0;
        // Silent unless asked: readlink says nothing about a name it could
        // not read, and -q and -s are there only to say so twice.
        bool loud = (flags & FILE_FLAG('v')) != 0;
        bool zero = (flags & FILE_FLAG('z')) != 0;
        b32 status = 0;

        if (count - first > 1)
                no_newline = false;

        while (first < count)
        {
                string_address path = program_argument((b32)first++);
                p8 answer[FILE_PATH_MAX];

                if (resolve)
                {
                        if (!file_real(path, answer))
                        {
                                if (loud)
                                        string_format(file_fail,
                                                      "readlink: %s: No such file or directory\n",
                                                      path);

                                status = 1;
                                continue;
                        }

                        p8 above[FILE_PATH_MAX];

                        path_head_copy(above, FILE_PATH_MAX, answer);

                        // -f wants the parent to be real, -e wants the whole
                        // path to be, -m wants neither.
                        if (readlink_canonical_option != 'm' &&
                            !file_is_directory_through(above))
                        {
                                if (loud)
                                        string_format(file_fail,
                                                      "readlink: %s: No such file or directory\n",
                                                      path);

                                status = 1;
                                continue;
                        }

                        if (readlink_canonical_option == 'e' &&
                            !file_exists(AT_FDCWD, answer))
                        {
                                if (loud)
                                        string_format(file_fail,
                                                      "readlink: %s: No such file or directory\n",
                                                      path);

                                status = 1;
                                continue;
                        }
                }
                else
                {
                        bipolar length = file_link_text(path, answer, FILE_PATH_MAX);

                        if (length < 0)
                        {
                                // A name that is not there and a name that is
                                // not a link are two different answers, and
                                // the kernel has already told them apart.
                                if (loud)
                                        string_format(file_fail, "readlink: %s: %s\n", path,
                                                      file_reason(length));

                                status = 1;
                                continue;
                        }
                }

                // -n drops the delimiter rather than choosing one, so it wins
                // over -z when both are given.
                if (no_newline)
                        log(answer, 0);
                else
                        file_written(answer, zero);
        }

        log_flush();

        return status;
}

// basename ------------------------------------------------------------
// basename NAME [SUFFIX], and the -a / -s / -z forms that take many names.
static const file_long basename_longs[] = {
    {(string_address) "multiple", 'a'},
    {(string_address) "suffix", 's'},
    {(string_address) "zero", 'z'},
    {null, 0},
};

static fn basename_one(string_address name, string_address suffix, bool zero)
{
        p8 answer[FILE_PATH_MAX];
        positive length = path_tail_copy(answer, FILE_PATH_MAX, name);

        if (suffix)
        {
                positive cut = string_length(suffix);

                // A name that is nothing but its suffix keeps it: stripping
                // would leave an empty line where a name was asked for.
                if (cut > 0 && cut < length)
                {
                        if (!memory_compare(answer + length - cut, suffix, cut))
                                answer[length - cut] = end;
                }
        }

        file_written(answer, zero);
}

static b32 file_basename()
{
        file_taking taking = {
            .program = (string_address) "basename",
            .allowed = (string_address) "asz",
            .valued = (string_address) "s",
            .longs = basename_longs,
        };

        if (!file_take(address_of taking))
                return 1;

        positive index = taking.first;
        positive count = (positive)program_argument_count();
        string_address suffix = file_option_value(address_of taking, 's');
        bool zero = (taking.flags & FILE_FLAG('z')) != 0;

        // -s says what to strip and thereby says there is more than one name;
        // without it the second word is the suffix and there is exactly one.
        bool many = (taking.flags & (FILE_FLAG('a') | FILE_FLAG('s'))) != 0;

        if (index >= count)
        {
                file_fail("basename: missing operand\n", 0);
                return 1;
        }

        if (!many && index + 1 < count)
                suffix = program_argument((b32)(index + 1));

        if (many)
        {
                while (index < count)
                        basename_one(program_argument((b32)index++), suffix, zero);
        }
        else
                basename_one(program_argument((b32)index), suffix, zero);

        log_flush();

        return 0;
}

// dirname ------------------------------------------------------------
// dirname [-z] NAME..., the directory part of every name given.
static const file_long dirname_longs[] = {
    {(string_address) "zero", 'z'},
    {null, 0},
};

static b32 file_dirname()
{
        file_taking taking = {
            .program = (string_address) "dirname",
            .allowed = (string_address) "z",
            .valued = (string_address) "",
            .longs = dirname_longs,
        };

        if (!file_take(address_of taking))
                return 1;

        positive first = taking.first;
        positive count = (positive)program_argument_count();

        if (first >= count)
        {
                file_fail("dirname: missing operand\n", 0);
                return 1;
        }

        while (first < count)
        {
                p8 answer[FILE_PATH_MAX];

                path_head_copy(answer, FILE_PATH_MAX,
                               program_argument((b32)first++));
                file_written(answer, (taking.flags & FILE_FLAG('z')) != 0);
        }

        log_flush();

        return 0;
}

// realpath ------------------------------------------------------------
/*
        realpath [-E|-e|-m] [-L|-P] [-s] [-q] [-z] [--relative-to=DIR]
                 [--relative-base=DIR] PATH...

        -E is the default and asks only that everything above the last name
        is there, which is what makes the tool worth having: naming a file
        that has yet to be created is the usual reason for asking. -e wants
        the whole path to exist, -m wants none of it, and -s answers what the
        name says rather than what is on the disk, which puts existence
        beside the point.

        -L and -P are taken and both resolve as -P does, which is GNU's own
        default: the two only part company over a .. that follows a link.
*/
static const file_long realpath_longs[] = {
    {(string_address) "canonicalize", 'E'},
    {(string_address) "canonicalize-existing", 'e'},
    {(string_address) "canonicalize-missing", 'm'},
    {(string_address) "logical", 'L'},
    {(string_address) "physical", 'P'},
    {(string_address) "quiet", 'q'},
    {(string_address) "relative-to", 'R'},
    {(string_address) "relative-base", 'B'},
    {(string_address) "strip", 's'},
    {(string_address) "no-symlinks", 's'},
    {(string_address) "zero", 'z'},
    {null, 0},
};

static p8 realpath_missing_option;
static p8 realpath_walk_option;

static bool realpath_option_seen(p8 letter, string_address value)
{
        (void)value;

        if (letter == 'E' || letter == 'e' || letter == 'm')
                realpath_missing_option = letter;

        if (letter == 'L' || letter == 'P')
                realpath_walk_option = letter;

        return true;
}

// Whether one canonical path is the other or lies under it. Whole components
// only: /usr/lib is not under /usr/li.
static bool realpath_under(string_address directory, string_address path)
{
        positive length = string_length(directory);

        if (length > 0 && string_is(directory + length - 1, '/'))
                length--;

        if (string_compare_max(path, directory, length))
                return false;

        return string_is(path + length, end) || string_is(path + length, '/');
}

// Whether every component of a path but the last is a directory. -L drops
// the .. before any of them is followed, and a .. after something that is not
// a directory is still a path that does not exist.
static bool realpath_walkable(string_address path)
{
        p8 prefix[FILE_PATH_MAX];
        positive length = 0;

        while (string_get(path + length))
        {
                if (!string_is(path + length, '/') || length == 0)
                {
                        length++;
                        continue;
                }

                string_copy_max_end(prefix, path, length);

                if (!file_is_directory_through(prefix))
                        return false;

                length++;
        }

        return true;
}

/*
        One canonical path said from where another stands: what they share
        dropped, one .. for every step still to climb, and a lone dot when
        the two name the same place.
*/
static fn realpath_relative(string_address from, string_address path, p8 address_to into)
{
        positive same = 0;
        positive mark = 0;

        while (string_get(from + same) && string_get(from + same) == string_get(path + same))
        {
                if (string_is(from + same, '/'))
                        mark = same + 1;

                same++;
        }

        // A whole component or none of it: /usr/lib and /usr/libexec share
        // five letters and no directory below the first.
        if (string_is(from + same, end) &&
            (string_is(path + same, '/') || string_is(path + same, end)))
                mark = same + (string_is(path + same, '/') ? 1 : 0);
        else if (string_is(path + same, end) && string_is(from + same, '/'))
                mark = same + 1;

        positive length = 0;
        string_address step = from + mark;

        while (string_get(step))
        {
                while (string_is(step, '/'))
                        step++;

                if (string_is(step, end))
                        break;

                while (string_get(step) && !string_is(step, '/'))
                        step++;

                if (length)
                        into[length++] = '/';

                into[length++] = '.';
                into[length++] = '.';
        }

        if (string_get(path + mark))
        {
                if (length)
                        into[length++] = '/';

                positive rest = string_length_max(path + mark, FILE_PATH_MAX - 1 - length);

                memory_copy_fast(into + length, path + mark, rest);
                length += rest;
        }

        if (!length)
                into[length++] = '.';

        into[length] = end;
}

static b32 file_realpath()
{
        realpath_missing_option = 'E';
        realpath_walk_option = 'P';

        file_taking taking = {
            .program = (string_address) "realpath",
            .allowed = (string_address) "EeLmPqsz",
            .valued = (string_address) "RB",
            .longs = realpath_longs,
            .seen = realpath_option_seen,
        };

        if (!file_take(address_of taking))
                return 1;

        positive first = taking.first;
        positive count = (positive)program_argument_count();

        if (first >= count)
        {
                file_fail("realpath: missing operand\n", 0);
                return 1;
        }

        bool allow_missing = realpath_missing_option == 'm';
        bool written_name = (taking.flags & FILE_FLAG('s')) != 0;
        bool logical = realpath_walk_option == 'L';
        bool quiet = (taking.flags & FILE_FLAG('q')) != 0;
        bool zero = (taking.flags & FILE_FLAG('z')) != 0;
        b32 status = 0;

        p8 base_real[FILE_PATH_MAX];
        p8 against_real[FILE_PATH_MAX];
        string_address base = file_option_value(address_of taking, 'B');
        string_address against = file_option_value(address_of taking, 'R');

        // Both directories are made canonical before anything is said
        // relative to them, or /tmp/./x would not look like /tmp/x.
        if (base && file_real(base, base_real))
                base = base_real;

        if (!against)
                against = base;
        else if (file_real(against, against_real))
                against = against_real;

        while (first < count)
        {
                string_address path = program_argument((b32)first++);
                p8 answer[FILE_PATH_MAX];
                p8 above[FILE_PATH_MAX];

                /*
                        -L takes the .. out of the name before any link in it
                        is followed, so link/.. is where the name was written
                        and not where the link went. That is two passes: the
                        lexical one, and then the real one over what it left.
                */
                if (logical && !written_name)
                {
                        p8 lexical[FILE_PATH_MAX];

                        if (!realpath_walkable(path) ||
                            !file_resolve(path, lexical, false) ||
                            !file_resolve(lexical, answer, true))
                        {
                                if (!quiet)
                                        string_format(file_fail,
                                                      "realpath: %s: Invalid argument\n", path);

                                status = 1;
                                continue;
                        }
                }
                else if (!file_resolve(path, answer, !written_name))
                {
                        if (!quiet)
                                string_format(file_fail, "realpath: %s: Invalid argument\n", path);

                        status = 1;
                        continue;
                }

                path_head_copy(above, FILE_PATH_MAX, answer);

                if (!written_name &&
                    ((!allow_missing && !file_is_directory_through(above)) ||
                     (realpath_missing_option == 'e' &&
                      !file_exists(AT_FDCWD, answer))))
                {
                        if (!quiet)
                                string_format(file_fail, "realpath: %s: No such file or directory\n",
                                              path);

                        status = 1;
                        continue;
                }

                // --relative-base names where the shorthand stops being worth
                // it: a path outside that directory is said in full.
                if (against && (!base || realpath_under(base, answer)))
                {
                        p8 relative[FILE_PATH_MAX];

                        realpath_relative(against, answer, relative);
                        file_written(relative, zero);
                }
                else
                        file_written(answer, zero);
        }

        log_flush();

        return status;
}

// mkdir ------------------------------------------------------------
// mkdir [-p] [-m MODE] DIRECTORY...
static b32 file_mkdir()
{
        positive count = (positive)program_argument_count();
        file_taking taking = {
            .program = (string_address) "mkdir",
            .allowed = (string_address) "mp",
            .valued = (string_address) "m",
        };

        if (!file_take(address_of taking))
                return 1;

        positive index = taking.first;
        positive mode = 0777;
        bool parents = (taking.flags & FILE_FLAG('p')) != 0;
        bool given_mode = (taking.flags & FILE_FLAG('m')) != 0;

        if (given_mode &&
            !file_mode_of(file_option_value(address_of taking, 'm'), 0777, true,
                          address_of mode))
        {
                file_fail("mkdir: bad mode\n", 0);
                return 1;
        }

        if (index >= count)
        {
                file_fail("mkdir: missing operand\n", 0);
                return 1;
        }

        b32 status = 0;

        while (index < count)
        {
                string_address path = program_argument((b32)index++);

                if (parents)
                {
                        // The parents are made with the default, and only the
                        // directory that was named gets the mode asked for.
                        if (!file_make_parents(path, 0777))
                        {
                                file_complain((string_address) "mkdir",
                                              (string_address) "Cannot create directory",
                                              path);
                                status = 1;
                                continue;
                        }

                        if (given_mode)
                                system_call_4(syscall(fchmodat), AT_FDCWD, (positive)path,
                                              mode, 0);

                        continue;
                }

                bipolar made = system_call_3(syscall(mkdirat), AT_FDCWD, (positive)path, mode);

                if (made < 0)
                {
                        string_format(file_fail, "mkdir: cannot create directory '%s': %s\n",
                                      path, file_reason(made));
                        status = 1;
                }
        }

        log_flush();

        return status;
}

// rmdir ------------------------------------------------------------
// rmdir [-p] DIRECTORY..., where -p goes on removing the parents while they
// are empty too.
static b32 file_rmdir()
{
        positive count = (positive)program_argument_count();
        file_taking taking = {
            .program = (string_address) "rmdir",
            .allowed = (string_address) "p",
            .valued = (string_address) "",
        };

        if (!file_take(address_of taking))
                return 1;

        positive flags = taking.flags;
        positive first = taking.first;

        if (first >= count)
        {
                file_fail("rmdir: missing operand\n", 0);
                return 1;
        }

        b32 status = 0;

        while (first < count)
        {
                string_address path = program_argument((b32)first++);
                bipolar gone = system_call_3(syscall(unlinkat), AT_FDCWD, (positive)path,
                                             AT_REMOVEDIR);

                if (gone < 0)
                {
                        string_format(file_fail, "rmdir: failed to remove '%s': %s\n",
                                      path, file_reason(gone));
                        status = 1;
                        continue;
                }

                if (!(flags & FILE_FLAG('p')))
                        continue;

                p8 parent[FILE_PATH_MAX];

                string_copy_max_end(parent, path, FILE_PATH_MAX - 1);

                while (1)
                {
                        p8 above[FILE_PATH_MAX];

                        path_head_copy(above, FILE_PATH_MAX, parent);

                        if (string_is(above, '.') && string_is(above + 1, end))
                                break;

                        if (string_is(above, '/') && string_is(above + 1, end))
                                break;

                        if (system_call_3(syscall(unlinkat), AT_FDCWD, (positive)above,
                                          AT_REMOVEDIR) < 0)
                                break;

                        string_copy_max_end(parent, above, FILE_PATH_MAX - 1);
                }
        }

        log_flush();

        return status;
}

// cp ------------------------------------------------------------
/*
        cp [-r] [-p] SOURCE... DESTINATION

        A destination that is a directory takes each source under its own last
        component; two operands where the second is not a directory make the
        copy itself. -t names the directory instead of positioning it, and -T
        refuses to treat the destination as one at all.

        A symbolic link named on the command line is followed and what it
        points at is copied, which is what cp is for; -R walks a tree and
        copies the links inside it as links, which is what a tree is. -P, -d
        and -a hold to the second everywhere and -L to the first.
*/
static bool cp_recursive;
static bool cp_preserve;
static bool cp_force;
static bool cp_ask;
static bool cp_never_clobber;
static bool cp_newer_only;
static bool cp_hard;
static bool cp_symbolic;
static bool cp_loud;
static b32 cp_status;
static p8 cp_collision_option;
static p8 cp_dereference_option;

static bool cp_option_seen(p8 letter, string_address value)
{
        (void)value;

        /* -f is independent; only -i and -n supersede one another. */
        if (letter == 'i' || letter == 'n')
                cp_collision_option = letter;

        if (letter == 'H' || letter == 'L' || letter == 'P' || letter == 'd' ||
            letter == 'a')
                cp_dereference_option = letter;

        return true;
}

// 0 copies a symbolic link as itself, 1 copies what it points at, 2 does
// that only for the links named on the command line.
static positive cp_dereference;

static fn cp_keep(string_address destination, file_facts address_to facts)
{
        if (!cp_preserve)
                return;

        p64 times[4];

        times[0] = (p64)facts->accessed.seconds;
        times[1] = facts->accessed.nanoseconds;
        times[2] = (p64)facts->modified.seconds;
        times[3] = facts->modified.nanoseconds;

        system_call_5(syscall(fchownat), AT_FDCWD, (positive)destination,
                      facts->owner, facts->group, AT_SYMLINK_NOFOLLOW);

        system_call_4(syscall(utimensat), AT_FDCWD, (positive)destination,
                      (positive)times, AT_SYMLINK_NOFOLLOW);

        // A symbolic link has no mode of its own, and fchmodat has no way to
        // stop at one: the chmod would land on whatever it points at.
        if ((facts->mode & MODE_FORMAT) == MODE_LINK)
                return;

        system_call_4(syscall(fchmodat), AT_FDCWD, (positive)destination,
                      facts->mode & 07777, 0);
}

// -n, -i and -u are three ways of asking the same question about a
// destination that is already there, and a destination that is not there is
// never in question.
static bool cp_allowed(string_address destination, file_facts address_to facts)
{
        file_facts there;

        if (!cp_never_clobber && !cp_newer_only && !cp_ask)
                return true;

        if (!file_look_link(destination, address_of there))
                return true;

        if (cp_never_clobber)
                return false;

        if (cp_newer_only)
        {
                if (facts->modified.seconds < there.modified.seconds)
                        return false;

                if (facts->modified.seconds == there.modified.seconds &&
                    facts->modified.nanoseconds <= there.modified.nanoseconds)
                        return false;
        }

        if (cp_ask && !file_ask((string_address) "cp", (string_address) "overwrite",
                                destination))
        {
                cp_status = 1;
                return false;
        }

        return true;
}

static fn cp_said(string_address source, string_address destination)
{
        if (cp_loud)
                string_format(log, "'%s' -> '%s'\n", source, destination);
}

// -l and -s make a name for the file rather than a copy of it, and neither
// has anything to say about a directory: with -r the directory is still made
// and it is what lands inside that is linked.
static bool cp_linked(string_address source, string_address destination)
{
        if (cp_force || cp_ask || cp_never_clobber)
                system_call_3(syscall(unlinkat), AT_FDCWD, (positive)destination, 0);

        bipolar done;

        if (cp_symbolic)
        {
                // A relative target is read from where the link sits, so a
                // link made anywhere but here would point somewhere else.
                if (!string_is(source, '/') && string_first_of(destination, '/'))
                {
                        string_format(file_fail,
                                      "cp: %s: can make relative symbolic links only in current directory\n",
                                      destination);
                        cp_status = 1;
                        return false;
                }

                done = system_call_3(syscall(symlinkat), (positive)source, AT_FDCWD,
                                     (positive)destination);
        }
        else
                done = system_call_5(syscall(linkat), AT_FDCWD, (positive)source,
                                     AT_FDCWD, (positive)destination, 0);

        if (done < 0)
        {
                string_format(file_fail, "cp: cannot create link '%s': %s\n",
                              destination, file_reason(done));
                cp_status = 1;
                return false;
        }

        cp_said(source, destination);

        return true;
}

static bool cp_one(string_address source, string_address destination, positive depth,
                   bool named)
{
        file_facts facts;
        file_facts there;
        bool follow = cp_dereference == 1 || (cp_dereference == 2 && named);

        if (!(follow ? file_look_at(source, address_of facts)
                     : file_look_link(source, address_of facts)))
        {
                string_format(file_fail, "cp: cannot stat '%s': No such file or directory\n",
                              source);
                cp_status = 1;
                return false;
        }

        positive kind = facts.mode & MODE_FORMAT;

        bool destination_exists = kind == MODE_LINK && !follow
                                      ? file_look_link(destination, address_of there)
                                      : file_look_at(destination, address_of there);

        if (destination_exists && file_same_identity(address_of facts, address_of there))
        {
                string_format(file_fail, "cp: '%s' and '%s' are the same file\n", source,
                              destination);
                cp_status = 1;
                return false;
        }

        if (kind == MODE_DIRECTORY)
        {
                p8 from[FILE_PATH_MAX];
                p8 to[FILE_PATH_MAX];

                if (file_resolve(source, from, true) &&
                    file_resolve(destination, to, false) && realpath_under(from, to))
                {
                        string_format(file_fail,
                                      "cp: cannot copy a directory, '%s', into itself, '%s'\n",
                                      source, destination);
                        cp_status = 1;
                        return false;
                }
        }

        if (kind != MODE_DIRECTORY && !cp_allowed(destination, address_of facts))
                return true;

        if ((cp_hard || cp_symbolic) && kind != MODE_DIRECTORY)
                return cp_linked(source, destination);

        if (kind == MODE_LINK)
        {
                p8 target[FILE_PATH_MAX];

                if (file_link_text(source, target, FILE_PATH_MAX) < 0)
                {
                        cp_status = 1;
                        return false;
                }

                system_call_3(syscall(unlinkat), AT_FDCWD, (positive)destination, 0);

                if (system_call_3(syscall(symlinkat), (positive)target, AT_FDCWD,
                                  (positive)destination) < 0)
                {
                        string_format(file_fail, "cp: cannot create link '%s'\n", destination);
                        cp_status = 1;
                        return false;
                }

                cp_keep(destination, address_of facts);
                cp_said(source, destination);
                return true;
        }

        if (kind != MODE_DIRECTORY)
        {
                if (!file_copy_contents(AT_FDCWD, source, AT_FDCWD, destination,
                                        facts.mode & 07777))
                {
                        // -f is for a destination that cannot be written to
                        // but can be replaced, which is what an unwritable
                        // file in a writable directory is.
                        if (!cp_force)
                        {
                                string_format(file_fail, "cp: cannot copy '%s'\n", source);
                                cp_status = 1;
                                return false;
                        }

                        system_call_3(syscall(unlinkat), AT_FDCWD, (positive)destination, 0);

                        if (!file_copy_contents(AT_FDCWD, source, AT_FDCWD, destination,
                                                facts.mode & 07777))
                        {
                                string_format(file_fail, "cp: cannot copy '%s'\n", source);
                                cp_status = 1;
                                return false;
                        }
                }

                // The open above only sets the mode on a file it created, so a
                // destination that was already there keeps whatever it had
                // unless the mode is written afterwards.
                system_call_4(syscall(fchmodat), AT_FDCWD, (positive)destination,
                              facts.mode & 07777, 0);

                cp_keep(destination, address_of facts);
                cp_said(source, destination);
                return true;
        }

        if (!cp_recursive)
        {
                string_format(file_fail, "cp: -r not specified; omitting directory '%s'\n",
                              source);
                cp_status = 1;
                return false;
        }

        if (depth == 0)
        {
                string_format(file_fail, "cp: '%s' is nested too deep\n", source);
                cp_status = 1;
                return false;
        }

        bipolar made = system_call_3(syscall(mkdirat), AT_FDCWD, (positive)destination,
                                     facts.mode & 07777);

        if (made < 0 && made != -ERROR_EXISTS)
        {
                string_format(file_fail, "cp: cannot create directory '%s': %s\n",
                              destination, file_reason(made));
                cp_status = 1;
                return false;
        }

        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, source))
        {
                string_format(file_fail, "cp: cannot read directory '%s'\n", source);
                cp_status = 1;
                return false;
        }

        cp_said(source, destination);

        struct linux_dirent64 address_to entry;
        bool complete = true;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                p8 from[FILE_PATH_MAX];
                p8 to[FILE_PATH_MAX];

                path_join(from, FILE_PATH_MAX, source, entry->d_name);
                path_join(to, FILE_PATH_MAX, destination, entry->d_name);

                if (!cp_one(from, to, depth - 1, false))
                        complete = false;
        }

        file_walk_close(address_of walk);

        cp_keep(destination, address_of facts);

        return complete;
}

static const file_long cp_longs[] = {
    {(string_address) "archive", 'a'},
    {(string_address) "dereference", 'L'},
    {(string_address) "force", 'f'},
    {(string_address) "interactive", 'i'},
    {(string_address) "link", 'l'},
    {(string_address) "no-clobber", 'n'},
    {(string_address) "no-dereference", 'P'},
    {(string_address) "no-target-directory", 'T'},
    {(string_address) "preserve", 'p'},
    {(string_address) "recursive", 'R'},
    {(string_address) "symbolic-link", 's'},
    {(string_address) "target-directory", 't'},
    {(string_address) "update", 'u'},
    {(string_address) "verbose", 'v'},
    {null, 0},
};

static b32 file_cp()
{
        positive count = (positive)program_argument_count();
        cp_status = 0;
        cp_collision_option = 0;
        cp_dereference_option = 0;

        file_taking taking = {
            .program = (string_address) "cp",
            .allowed = (string_address) "aHLPRdfilnprstTuv",
            .valued = (string_address) "t",
            .longs = cp_longs,
            .seen = cp_option_seen,
        };

        if (!file_take(address_of taking))
                return 1;

        positive flags = taking.flags;
        positive first = taking.first;

        cp_recursive = (flags & (FILE_FLAG('r') | FILE_FLAG('R') | FILE_FLAG('a'))) != 0;
        cp_preserve = (flags & (FILE_FLAG('p') | FILE_FLAG('a'))) != 0;
        cp_force = (flags & FILE_FLAG('f')) != 0;
        cp_ask = cp_collision_option == 'i';
        cp_never_clobber = cp_collision_option == 'n';
        cp_newer_only = (flags & FILE_FLAG('u')) != 0;
        cp_hard = (flags & FILE_FLAG('l')) != 0;
        cp_symbolic = (flags & FILE_FLAG('s')) != 0;
        cp_loud = (flags & FILE_FLAG('v')) != 0;

        // A link named as a source is followed, because copying a file is
        // what cp was asked for; a link found inside a tree being walked is
        // not, because the tree is what -R was asked for.
        cp_dereference = cp_recursive ? 0 : 1;

        if (cp_dereference_option == 'H')
                cp_dereference = 2;
        else if (cp_dereference_option == 'L')
                cp_dereference = 1;
        else if (cp_dereference_option)
                cp_dereference = 0;

        string_address into = file_option_value(address_of taking, 't');
        bool alone = (flags & FILE_FLAG('T')) != 0;

        if (into && alone)
        {
                file_fail("cp: cannot combine --target-directory and --no-target-directory\n", 0);
                return 1;
        }

        if (first >= count || (!into && first + 1 >= count))
        {
                file_fail("cp: missing operand\n", 0);
                return 1;
        }

        string_address last = into ? into : program_argument((b32)(count - 1));
        positive after = into ? count : count - 1;

        // -T says the destination is the copy itself however many names it
        // has and whatever is already there, which is the one case where a
        // directory on the right is not a directory to copy into.
        if (alone || (!into && count - first == 2 && !file_is_directory_through(last)))
        {
                if (after - first != 1)
                {
                        string_format(file_fail, "cp: extra operand '%s'\n",
                                      program_argument((b32)(first + 1)));
                        return 1;
                }

                cp_one(program_argument((b32)first), last, FILE_MAX_DEPTH, true);
                log_flush();
                return cp_status;
        }

        if (!file_is_directory_through(last))
        {
                string_format(file_fail, "cp: target '%s' is not a directory\n", last);
                return 1;
        }

        while (first < after)
        {
                string_address source = program_argument((b32)first++);
                p8 tail[FILE_PATH_MAX];
                p8 destination[FILE_PATH_MAX];

                path_tail_copy(tail, FILE_PATH_MAX, source);
                path_join(destination, FILE_PATH_MAX, last, tail);

                cp_one(source, destination, FILE_MAX_DEPTH, true);
        }

        log_flush();

        return cp_status;
}

// mv ------------------------------------------------------------
/*
        mv [-f] [-i] [-n] [-t DIR] [-T] SOURCE... DESTINATION

        renameat2 rather than renameat, because riscv64 never had renameat and
        this tree builds for it; a flags word of zero is the same operation.
*/
static b32 mv_status;
static bool mv_ask;
static bool mv_never_clobber;
static bool mv_loud;
static p8 mv_collision_option;

static bool mv_option_seen(p8 letter, string_address value)
{
        (void)value;

        if (letter == 'f' || letter == 'i' || letter == 'n')
                mv_collision_option = letter;

        return true;
}

static bool mv_across(string_address source, string_address destination, positive depth);

static bool mv_across_directory(string_address source, string_address destination,
                                positive depth)
{
        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, source))
                return false;

        struct linux_dirent64 address_to entry;
        bool complete = true;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                p8 from[FILE_PATH_MAX];
                p8 to[FILE_PATH_MAX];

                path_join(from, FILE_PATH_MAX, source, entry->d_name);
                path_join(to, FILE_PATH_MAX, destination, entry->d_name);

                if (!mv_across(from, to, depth - 1))
                        complete = false;
        }

        file_walk_close(address_of walk);

        if (complete)
                system_call_3(syscall(unlinkat), AT_FDCWD, (positive)source, AT_REMOVEDIR);

        return complete;
}

// A rename that crosses a mount point is not a rename at all, so the bytes
// have to be carried over and the original taken away afterwards.
static bool mv_across(string_address source, string_address destination, positive depth)
{
        file_facts facts;

        if (!file_look_link(source, address_of facts))
                return false;

        if (depth == 0)
                return false;

        positive kind = facts.mode & MODE_FORMAT;

        if (kind == MODE_DIRECTORY)
        {
                bipolar made = system_call_3(syscall(mkdirat), AT_FDCWD,
                                             (positive)destination, facts.mode & 07777);

                if (made < 0 && made != -ERROR_EXISTS)
                        return false;

                return mv_across_directory(source, destination, depth);
        }

        if (kind == MODE_LINK)
        {
                p8 target[FILE_PATH_MAX];

                if (file_link_text(source, target, FILE_PATH_MAX) < 0)
                        return false;

                system_call_3(syscall(unlinkat), AT_FDCWD, (positive)destination, 0);

                if (system_call_3(syscall(symlinkat), (positive)target, AT_FDCWD,
                                  (positive)destination) < 0)
                        return false;
        }
        else if (!file_copy_contents(AT_FDCWD, source, AT_FDCWD, destination,
                                     facts.mode & 07777))
                return false;

        p64 times[4];

        times[0] = (p64)facts.accessed.seconds;
        times[1] = facts.accessed.nanoseconds;
        times[2] = (p64)facts.modified.seconds;
        times[3] = facts.modified.nanoseconds;

        system_call_4(syscall(utimensat), AT_FDCWD, (positive)destination,
                      (positive)times, AT_SYMLINK_NOFOLLOW);

        return system_call_3(syscall(unlinkat), AT_FDCWD, (positive)source, 0) == 0;
}

// -n, -i and -f are the same question mv asks about a destination that is
// already there, and -f is the default it asks nothing under.
static bool mv_allowed(string_address destination)
{
        if (!mv_never_clobber && !mv_ask)
                return true;

        if (!file_exists(AT_FDCWD, destination))
                return true;

        if (mv_never_clobber)
                return false;

        if (file_ask((string_address) "mv", (string_address) "overwrite", destination))
                return true;

        mv_status = 1;
        return false;
}

static fn mv_one(string_address source, string_address destination)
{
        if (!mv_allowed(destination))
                return;

        file_facts from;
        file_facts to;

        if (file_look_link(source, address_of from) &&
            file_look_link(destination, address_of to) &&
            file_same_identity(address_of from, address_of to))
        {
                string_format(file_fail, "mv: '%s' and '%s' are the same file\n", source,
                              destination);
                mv_status = 1;
                return;
        }

        bipolar done = system_call_5(syscall(renameat2), AT_FDCWD, (positive)source,
                                     AT_FDCWD, (positive)destination, 0);

        if (done == 0)
        {
                if (mv_loud)
                        string_format(log, "renamed '%s' -> '%s'\n", source, destination);

                return;
        }

        if (done == -ERROR_CROSS_DEVICE && mv_across(source, destination, FILE_MAX_DEPTH))
        {
                if (mv_loud)
                        string_format(log, "renamed '%s' -> '%s'\n", source, destination);

                return;
        }

        string_format(file_fail, "mv: cannot move '%s' to '%s': %s\n", source,
                      destination, file_reason(done));
        mv_status = 1;
}

static const file_long mv_longs[] = {
    {(string_address) "force", 'f'},
    {(string_address) "interactive", 'i'},
    {(string_address) "no-clobber", 'n'},
    {(string_address) "no-target-directory", 'T'},
    {(string_address) "target-directory", 't'},
    {(string_address) "verbose", 'v'},
    {null, 0},
};

static b32 file_mv()
{
        positive count = (positive)program_argument_count();
        mv_status = 0;
        mv_collision_option = 0;

        file_taking taking = {
            .program = (string_address) "mv",
            .allowed = (string_address) "finTtv",
            .valued = (string_address) "t",
            .longs = mv_longs,
            .seen = mv_option_seen,
        };

        if (!file_take(address_of taking))
                return 1;

        positive first = taking.first;

        mv_ask = mv_collision_option == 'i';
        mv_never_clobber = mv_collision_option == 'n';
        mv_loud = (taking.flags & FILE_FLAG('v')) != 0;

        string_address into = file_option_value(address_of taking, 't');
        bool alone = (taking.flags & FILE_FLAG('T')) != 0;

        if (into && alone)
        {
                file_fail("mv: cannot combine --target-directory and --no-target-directory\n", 0);
                return 1;
        }

        if (first >= count || (!into && first + 1 >= count))
        {
                file_fail("mv: missing operand\n", 0);
                return 1;
        }

        string_address last = into ? into : program_argument((b32)(count - 1));
        positive after = into ? count : count - 1;

        if (alone || (!into && count - first == 2 && !file_is_directory_through(last)))
        {
                if (after - first != 1)
                {
                        string_format(file_fail, "mv: extra operand '%s'\n",
                                      program_argument((b32)(first + 1)));
                        return 1;
                }

                mv_one(program_argument((b32)first), last);
                log_flush();
                return mv_status;
        }

        if (!file_is_directory_through(last))
        {
                string_format(file_fail, "mv: target '%s' is not a directory\n", last);
                return 1;
        }

        while (first < after)
        {
                string_address source = program_argument((b32)first++);
                p8 tail[FILE_PATH_MAX];
                p8 destination[FILE_PATH_MAX];

                path_tail_copy(tail, FILE_PATH_MAX, source);
                path_join(destination, FILE_PATH_MAX, last, tail);

                mv_one(source, destination);
        }

        log_flush();

        return mv_status;
}

// rm ------------------------------------------------------------
/*
        rm [-r] [-f] [-d] [-i] [-v] FILE...

        -r takes a tree, -d takes a directory that is already empty and
        nothing else, and neither of them takes a directory that is in the way
        of the other.
*/
static bool rm_force;
static bool rm_recursive;
static bool rm_empty_directories;
static bool rm_ask;
static bool rm_loud;
static bool rm_one_system;
static bool rm_careful;
static p32 rm_device_major;
static p32 rm_device_minor;
static b32 rm_status;
static p8 rm_collision_option;

static bool rm_option_seen(p8 letter, string_address value)
{
        (void)value;

        if (letter == 'f' || letter == 'i')
                rm_collision_option = letter;

        return true;
}

static bool rm_tree(bipolar directory, string_address name, string_address shown,
                    positive depth);

static string_address rm_wording(file_facts address_to facts)
{
        positive kind = facts->mode & MODE_FORMAT;

        if (kind == MODE_DIRECTORY)
                return (string_address) "remove directory";

        if (kind == MODE_LINK)
                return (string_address) "remove symbolic link";

        if (kind != MODE_FILE)
                return (string_address) "remove";

        return facts->size ? (string_address) "remove regular file"
                           : (string_address) "remove regular empty file";
}

static fn rm_said(string_address shown, bool directory)
{
        if (!rm_loud)
                return;

        string_format(log, directory ? "removed directory '%s'\n" : "removed '%s'\n",
                      shown);
}

static bool rm_contents(bipolar directory, string_address shown, positive depth)
{
        bool complete = true;

        /*
                Removing an entry while a getdents block is being walked moves
                the ones behind it, so the block is refilled from the start
                after each pass and the directory is read again until a pass
                finds nothing left to take.
        */
        while (1)
        {
                file_walk walk;

                walk.handle = directory;
                walk.have = 0;
                walk.at = 0;

                system_call_3(syscall(lseek), directory, 0, FILE_SEEK_SET);

                struct linux_dirent64 address_to entry;
                positive removed = 0;
                positive seen = 0;

                while ((entry = file_walk_next(address_of walk)))
                {
                        if (file_is_dot(entry->d_name))
                                continue;

                        seen++;

                        p8 below[FILE_PATH_MAX];

                        path_join(below, FILE_PATH_MAX, shown, entry->d_name);

                        if (rm_tree(directory, entry->d_name, below, depth))
                                removed++;
                        else
                                complete = false;
                }

                if (seen == 0 || removed == 0)
                        break;
        }

        return complete;
}

static bool rm_tree(bipolar directory, string_address name, string_address shown,
                    positive depth)
{
        file_facts facts;

        /*
                The unlink is tried before the name is looked at, because for
                a file that is the whole of the work and a stat first would
                double the calls it takes. Only the flags that have a question
                to ask about what a name is pay for the answer.
        */
        if (!rm_careful)
        {
                if (system_call_3(syscall(unlinkat), directory, (positive)name, 0) == 0)
                        return true;
        }
        else if (!file_look(directory, name, AT_SYMLINK_NOFOLLOW, address_of facts))
        {
                if (!rm_force)
                {
                        string_format(file_fail, "rm: cannot remove '%s': %s\n", shown,
                                      file_reason(-ERROR_NO_ENTRY));
                        rm_status = 1;
                }

                return false;
        }
        else if ((facts.mode & MODE_FORMAT) != MODE_DIRECTORY)
        {
                if (rm_ask && !file_ask((string_address) "rm", rm_wording(address_of facts),
                                        shown))
                        return false;

                if (system_call_3(syscall(unlinkat), directory, (positive)name, 0) == 0)
                {
                        rm_said(shown, false);
                        return true;
                }
        }

        if (!file_is_directory(directory, name))
        {
                if (!rm_force)
                {
                        string_format(file_fail, "rm: cannot remove '%s': %s\n", shown,
                                      file_reason(-ERROR_NO_ENTRY));
                        rm_status = 1;
                }

                return false;
        }

        if (rm_one_system &&
            file_look(directory, name, AT_SYMLINK_NOFOLLOW, address_of facts) &&
            (facts.device_major != rm_device_major ||
             facts.device_minor != rm_device_minor))
        {
                string_format(file_fail,
                              "rm: skipping '%s', since it's on a different device\n", shown);
                rm_status = 1;
                return false;
        }

        // -d asks for the directory and not for what is under it, so the
        // remove below is the whole of it and a directory with anything in it
        // says so rather than being emptied.
        bool complete = true;

        if (rm_recursive)
        {
                if (depth == 0)
                {
                        string_format(file_fail, "rm: '%s' is nested too deep\n", shown);
                        rm_status = 1;
                        return false;
                }

                if (rm_ask && !file_ask((string_address) "rm",
                                        (string_address) "descend into directory", shown))
                        return false;

                bipolar inside = system_call_3(syscall(openat), directory, (positive)name,
                                               FILE_READ | O_DIRECTORY);

                if (inside < 0)
                {
                        if (!rm_force)
                        {
                                string_format(file_fail, "rm: cannot read '%s': %s\n", shown,
                                              file_reason(inside));
                                rm_status = 1;
                        }

                        return false;
                }

                complete = rm_contents(inside, shown, depth - 1);

                system_call_1(syscall(close), inside);
        }

        if (rm_ask && !rm_recursive &&
            !file_ask((string_address) "rm", (string_address) "remove directory", shown))
                return false;

        bipolar gone = system_call_3(syscall(unlinkat), directory, (positive)name,
                                     AT_REMOVEDIR);

        if (gone < 0)
        {
                if (!rm_force || (!rm_recursive && gone == -ERROR_NOT_EMPTY))
                {
                        string_format(file_fail, "rm: cannot remove '%s': %s\n", shown,
                                      file_reason(gone));
                        rm_status = 1;
                }

                return false;
        }

        rm_said(shown, true);

        return complete;
}

static const file_long rm_longs[] = {
    {(string_address) "dir", 'd'},
    {(string_address) "force", 'f'},
    {(string_address) "interactive", 'i'},
    {(string_address) "one-file-system", 'o'},
    {(string_address) "no-preserve-root", 'N'},
    {(string_address) "preserve-root", 'P'},
    {(string_address) "recursive", 'R'},
    {(string_address) "verbose", 'v'},
    {null, 0},
};

static b32 file_rm()
{
        positive count = (positive)program_argument_count();
        rm_status = 0;
        rm_collision_option = 0;

        file_taking taking = {
            .program = (string_address) "rm",
            .allowed = (string_address) "dfirRv",
            .valued = (string_address) "",
            .longs = rm_longs,
            .seen = rm_option_seen,
        };

        if (!file_take(address_of taking))
                return 1;

        positive flags = taking.flags;
        positive first = taking.first;

        rm_force = rm_collision_option == 'f';
        rm_ask = rm_collision_option == 'i';
        rm_loud = (flags & FILE_FLAG('v')) != 0;
        rm_one_system = (flags & FILE_FLAG('o')) != 0;
        rm_empty_directories = (flags & FILE_FLAG('d')) != 0;
        rm_recursive = (flags & (FILE_FLAG('r') | FILE_FLAG('R'))) != 0;
        rm_careful = rm_ask || rm_loud || rm_one_system;

        if (first >= count)
        {
                if (rm_force)
                        return 0;

                file_fail("rm: missing operand\n", 0);
                return 1;
        }

        while (first < count)
        {
                string_address path = program_argument((b32)first++);
                file_facts facts;

                if (!file_look(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, address_of facts))
                {
                        if (!rm_force)
                        {
                                string_format(file_fail,
                                              "rm: cannot remove '%s': No such file or directory\n",
                                              path);
                                rm_status = 1;
                        }

                        continue;
                }

                bool here = (facts.mode & MODE_FORMAT) == MODE_DIRECTORY;

                if (here && rm_recursive && !(flags & FILE_FLAG('N')) &&
                    string_is(path, '/') && string_is(path + 1, end))
                {
                        file_fail("rm: it is dangerous to operate recursively on '/'\n", 0);
                        file_fail("rm: use --no-preserve-root to override this failsafe\n", 0);
                        rm_status = 1;
                        continue;
                }

                if (here && !rm_recursive && !rm_empty_directories)
                {
                        string_format(file_fail, "rm: cannot remove '%s': Is a directory\n",
                                      path);
                        rm_status = 1;
                        continue;
                }

                rm_device_major = facts.device_major;
                rm_device_minor = facts.device_minor;

                rm_tree(AT_FDCWD, path, path, FILE_MAX_DEPTH);
        }

        log_flush();

        return rm_status;
}

// touch ------------------------------------------------------------
/*
        touch [-a] [-m] [-c] [-h] [-r REFERENCE] [-d DATE] [-t STAMP] FILE...

        -a and -m are what pick which of the two stamps moves; naming neither
        moves both, and naming one leaves the other exactly where it was
        rather than setting it to now, which is what UTIME_OMIT is for.

        -d takes everything file_moment_read takes. -t takes the older
        spelling, [[CC]YY]MMDDhhmm[.ss], where a two digit year of 69 or more
        is in the nineteen hundreds and one below it is in the two thousands
        -- POSIX's rule, and the reason 68 and 69 are a century apart.
*/
static bool touch_stamp(string_address text, b64 now, b64 address_to out)
{
        positive digits;
        b64 fraction = 0;
        bool has_fraction = false;

        string_digits(text, address_of digits);

        if (string_is(text + digits, '.'))
        {
                positive wide;

                if (file_read_number(text, digits + 1, address_of fraction,
                                     address_of wide) != digits + 3 || wide != 2)
                        return false;

                has_fraction = true;
        }
        else if (string_get(text + digits))
                return false;

        if (digits != 8 && digits != 10 && digits != 12)
                return false;

        b64 field[6];
        positive at = 0;

        for (positive i = 0; i < 6; i++)
        {
                if (i == 0 && digits < 12)
                {
                        field[0] = -1;
                        continue;
                }

                if (i == 1 && digits < 10)
                {
                        field[1] = -1;
                        continue;
                }

                field[i] = (string_get(text + at) - '0') * 10 +
                           (string_get(text + at + 1) - '0');
                at += 2;
        }

        b64 year;
        positive month, day, hour, minute, second;

        file_split_moment(now, address_of year, address_of month, address_of day,
                          address_of hour, address_of minute, address_of second);

        if (field[1] < 0)
                ;
        else if (field[0] < 0)
                year = field[1] >= 69 ? 1900 + field[1] : 2000 + field[1];
        else
                year = field[0] * 100 + field[1];

        if (field[2] < 1 || field[2] > 12 || field[3] < 1 || field[3] > 31 ||
            field[4] > 23 || field[5] > 59)
                return false;

        address_to out = file_days(year, field[2], field[3]) * 86400 +
                         field[4] * 3600 + field[5] * 60 +
                         (has_fraction ? fraction : 0);

        return true;
}

static const file_long touch_longs[] = {
    {(string_address) "date", 'd'},
    {(string_address) "no-create", 'c'},
    {(string_address) "no-dereference", 'h'},
    {(string_address) "reference", 'r'},
    {(string_address) "time", 'T'},
    {null, 0},
};

static b32 file_touch()
{
        positive count = (positive)program_argument_count();
        file_taking taking = {
            .program = (string_address) "touch",
            .allowed = (string_address) "acdfhmrt",
            .valued = (string_address) "drtT",
            .longs = touch_longs,
        };

        if (!file_take(address_of taking))
                return 1;

        positive flags = taking.flags;
        positive first = taking.first;

        bool access = (flags & FILE_FLAG('a')) != 0;
        bool modify = (flags & FILE_FLAG('m')) != 0;
        bool no_create = (flags & FILE_FLAG('c')) != 0;
        bool through = (flags & FILE_FLAG('h')) == 0;
        bool given = false;
        b64 seconds = 0;
        p32 nanoseconds = 0;

        string_address which = file_option_value(address_of taking, 'T');

        if (which)
        {
                if (!string_compare(which, "access") || !string_compare(which, "atime") ||
                    !string_compare(which, "use"))
                        access = true;
                else if (!string_compare(which, "modify") ||
                         !string_compare(which, "mtime"))
                        modify = true;
                else
                {
                        string_format(file_fail,
                                      "touch: invalid argument '%s' for '--time'\n", which);
                        return 1;
                }
        }

        string_address from = file_option_value(address_of taking, 'r');

        if (from)
        {
                file_facts facts;

                if (!file_look_at(from, address_of facts))
                {
                        string_format(file_fail,
                                      "touch: failed to get attributes of '%s'\n", from);
                        return 1;
                }

                seconds = facts.modified.seconds;
                nanoseconds = facts.modified.nanoseconds;
                given = true;
        }

        string_address written = file_option_value(address_of taking, 'd');

        if (written)
        {
                if (!file_moment_read(written, given ? seconds : file_now(),
                                      address_of seconds))
                {
                        string_format(file_fail, "touch: invalid date format '%s'\n", written);
                        return 1;
                }

                nanoseconds = 0;
                given = true;
        }

        string_address older = file_option_value(address_of taking, 't');

        if (older)
        {
                if (!touch_stamp(older, file_now(), address_of seconds))
                {
                        string_format(file_fail, "touch: invalid date format '%s'\n", older);
                        return 1;
                }

                nanoseconds = 0;
                given = true;
        }

        if (first >= count)
        {
                file_fail("touch: missing file operand\n", 0);
                return 1;
        }

        if (!access && !modify)
        {
                access = true;
                modify = true;
        }

        p64 times[4];

        times[0] = given ? (p64)seconds : 0;
        times[1] = access ? (given ? (p64)nanoseconds : (p64)UTIME_NOW) : (p64)UTIME_OMIT;
        times[2] = given ? (p64)seconds : 0;
        times[3] = modify ? (given ? (p64)nanoseconds : (p64)UTIME_NOW) : (p64)UTIME_OMIT;

        b32 status = 0;

        while (first < count)
        {
                string_address path = program_argument((b32)first++);

                file_facts existing;
                bool exists = through ? file_look_at(path, address_of existing)
                                      : file_look_link(path, address_of existing);

                if (!exists)
                {
                        if (no_create)
                                continue;

                        bipolar made = system_call_4(syscall(openat), AT_FDCWD,
                                                     (positive)path, FILE_WRITE & ~O_TRUNC,
                                                     0666);

                        if (made < 0)
                        {
                                string_format(file_fail, "touch: cannot touch '%s': %s\n",
                                              path, file_reason(made));
                                status = 1;
                                continue;
                        }

                        system_call_1(syscall(close), made);
                }

                bipolar done = system_call_4(syscall(utimensat), AT_FDCWD, (positive)path,
                                             (positive)times,
                                             through ? 0 : AT_SYMLINK_NOFOLLOW);

                if (done < 0)
                {
                        string_format(file_fail, "touch: setting times of '%s': %s\n",
                                      path, file_reason(done));
                        status = 1;
                }
        }

        log_flush();

        return status;
}

// sleep ------------------------------------------------------------
/*
        sleep NUMBER[SUFFIX]..., where the number may have a fraction and the
        suffix is s, m, h or d. The fraction is read digit by digit into
        nanoseconds rather than through a decimal, so there is no rounding
        anywhere between the text and the timespec.
*/
static bool sleep_read(string_address text, p64 address_to seconds,
                       p64 address_to nanoseconds)
{
        positive taken;
        p64 whole = string_digits(text, address_of taken);
        p64 fraction = 0;
        p64 scale = 100000000;
        bool any = taken != 0;

        text += taken;

        if (string_is(text, '.'))
        {
                text++;

                while (file_digit(string_get(text)))
                {
                        if (scale > 0)
                        {
                                fraction += (p64)(string_get(text) - '0') * scale;
                                scale /= 10;
                        }

                        text++;
                        any = true;
                }
        }

        if (!any)
                return false;

        p64 multiplier = 1;

        if (string_is(text, 's'))
                text++;
        else if (string_is(text, 'm'))
        {
                multiplier = 60;
                text++;
        }
        else if (string_is(text, 'h'))
        {
                multiplier = 3600;
                text++;
        }
        else if (string_is(text, 'd'))
        {
                multiplier = 86400;
                text++;
        }

        if (string_get(text))
                return false;

        p64 total = whole * multiplier * 1000000000 + fraction * multiplier;

        address_to seconds = total / 1000000000;
        address_to nanoseconds = total % 1000000000;

        return true;
}

static b32 file_sleep()
{
        positive count = (positive)program_argument_count();

        if (count < 2)
        {
                file_fail("sleep: missing operand\n", 0);
                return 1;
        }

        for (positive i = 1; i < count; i++)
        {
                p64 wanted[2] = {0, 0};

                if (!sleep_read(program_argument((b32)i), address_of wanted[0],
                                address_of wanted[1]))
                {
                        string_format(file_fail, "sleep: invalid time interval: %s\n",
                                      program_argument((b32)i));
                        return 1;
                }

                // A signal that arrives partway through leaves the remainder
                // in the second timespec, and the sleep goes on from there.
                p64 left[2] = {wanted[0], wanted[1]};

                while (system_call_2(syscall(nanosleep), (positive)left,
                                     (positive)left) < 0)
                {
                        if (left[0] == 0 && left[1] == 0)
                                break;
                }
        }

        return 0;
}

// seq ------------------------------------------------------------
/*
        seq LAST, seq FIRST LAST, seq FIRST INCREMENT LAST.

        Whole numbers only. The rest of this tree has no way to read a decimal
        out of a string, and a seq that printed a rounded 0.1 would be worse
        than one that says it cannot.
*/
static fn seq_write(writer write, bipolar value, positive width)
{
        if (value < 0)
        {
                positive_to_padded(write, (positive)0 - (positive)value,
                                   width, '0', '-');
                return;
        }

        positive_to_padded(write, (positive)value, width, '0', 0);
}

static positive seq_width(bipolar value)
{
        positive magnitude = (positive)value;

        if (value < 0)
                magnitude = (positive)0 - magnitude;

        return positive_digits(magnitude) + (value < 0);
}

static const file_long seq_longs[] = {
    {(string_address) "equal-width", 'w'},
    {(string_address) "format", 'f'},
    {(string_address) "separator", 's'},
    {null, 0},
};

static b32 file_seq()
{
        file_taking taking = {
            .program = (string_address) "seq",
            .allowed = (string_address) "fsw",
            .valued = (string_address) "fs",
            .longs = seq_longs,
            .numbers = true,
        };

        if (!file_take(address_of taking))
                return 1;

        // -f is printf's format for a double. Everything here counts in whole
        // numbers, and saying so is better than printing the right numbers in
        // the wrong shape.
        if (file_option_value(address_of taking, 'f'))
        {
                file_fail("seq: -f: this seq counts in whole numbers and has no format\n", 0);
                return 1;
        }

        positive count = (positive)program_argument_count();
        positive index = taking.first;
        bool pad = (taking.flags & FILE_FLAG('w')) != 0;
        string_address separator = file_option_value(address_of taking, 's');

        if (!separator)
                separator = (string_address) "\n";

        positive given = count - index;

        if (given < 1 || given > 3)
        {
                file_fail("seq: needs one, two or three numbers\n", 0);
                return 1;
        }

        bipolar first = 1;
        bipolar step = 1;
        bipolar last;

        if (given == 1)
                last = string_bipolar(program_argument((b32)index), null);
        else if (given == 2)
        {
                first = string_bipolar(program_argument((b32)index), null);
                last = string_bipolar(program_argument((b32)(index + 1)), null);
        }
        else
        {
                first = string_bipolar(program_argument((b32)index), null);
                step = string_bipolar(program_argument((b32)(index + 1)), null);
                last = string_bipolar(program_argument((b32)(index + 2)), null);
        }

        if (step == 0)
        {
                file_fail("seq: increment must not be zero\n", 0);
                return 1;
        }

        positive width = 0;

        if (pad)
        {
                width = seq_width(first);

                if (seq_width(last) > width)
                        width = seq_width(last);
        }

        bipolar value = first;

        bool written = false;

        while (step > 0 ? value <= last : value >= last)
        {
                if (written)
                        log(separator, 0);

                seq_write(log, value, width);
                written = true;
                value += step;
        }

        // The separator goes between the numbers; the line still ends the way
        // every other line does.
        if (written)
                log("\n", 1);

        log_flush();

        return 0;
}

// yes ------------------------------------------------------------
// yes [STRING]..., until something downstream stops reading.
static b32 file_yes()
{
        // No flags at all, which still has to be said: yes -x is a mistake
        // and printing -x for ever is not what was meant by it.
        file_taking taking = {
            .program = (string_address) "yes",
            .allowed = (string_address) "",
            .valued = (string_address) "",
            .longs = null,
        };

        if (!file_take(address_of taking))
                return 1;

        positive count = (positive)program_argument_count();
        positive first = taking.first;
        positive length = 1; // newline

        if (first >= count)
                length++;
        else
        {
                for (positive i = first; i < count; i++)
                {
                        string_address word = program_argument((b32)i);
                        positive have = string_length(word);
                        positive space = i > first;

                        if (have > positive_max - length - space)
                        {
                                file_fail("yes: arguments are too large\n", 0);
                                return 1;
                        }

                        length += space + have;
                }
        }

        positive mapped = (positive)memory(length);

        if (!mapped || mapped >= (positive)-4095)
        {
                file_fail("yes: out of memory\n", 0);
                return 1;
        }

        p8 address_to line = (p8 address_to)mapped;
        positive used = 0;

        if (first >= count)
                line[used++] = 'y';
        else
        {
                for (positive i = first; i < count; i++)
                {
                        string_address word = program_argument((b32)i);
                        positive have = string_length(word);

                        if (i > first)
                                line[used++] = ' ';

                        used = (positive)(memory_copy_fast_end(
                            line + used, word, have) - line);
                }
        }

        line[used++] = '\n';

        // One write of many copies rather than one write per line: the same
        // bytes leave the program in a fraction of the system calls. A line
        // larger than the batching block is already a large write by itself.
        p8 block[FILE_BLOCK * 4];
        positive filled = 0;
        p8 address_to output = line;

        while (length <= sizeof(block) - filled)
        {
                memory_copy(block + filled, line, length);
                filled += length;
        }

        if (filled)
                output = block;
        else
                filled = length;

        while (1)
        {
                if (system_write_all(stdout, output, filled) != filled)
                {
                        memory_free(line, length);
                        return 1;
                }
        }

        return 0;
}

// env ------------------------------------------------------------
/*
        env [-i] [-u NAME]... [-C DIR] [-a ARG] [-S STRING] [-0]
            [NAME=VALUE]... [COMMAND [ARGUMENT]...]

        With no command it prints the environment it would have used, which is
        also the only way anything here can look at its own environment.

        The signal options are taken, say on the error stream that they
        change nothing, and change nothing. Refusing them outright would fail
        a shebang line whose command runs perfectly well without the mask it
        asked for; accepting them in silence would be a lie about what the
        command inherits. -v and --list-signal-handling are ignored in
        silence, since all they ever wrote was the error stream itself.
*/
static const file_long env_longs[] = {
    {(string_address) "argv0", 'a'},
    {(string_address) "ignore-environment", 'i'},
    {(string_address) "null", '0'},
    {(string_address) "unset", 'u'},
    {(string_address) "chdir", 'C'},
    {(string_address) "split-string", 'S'},
    {(string_address) "block-signal", 'b'},
    {(string_address) "default-signal", 'd'},
    {(string_address) "ignore-signal", 'g'},
    {(string_address) "list-signal-handling", 'l'},
    {(string_address) "debug", 'v'},
    {null, 0},
};

static string_address address_to env_list;
static positive env_room;
static positive env_have;

static bool env_same_key(string_address entry, string_address name, positive length)
{
        string_address mark = string_first_of(entry, '=');

        if (!mark || (positive)(mark - entry) != length)
                return false;

        return memory_compare(entry, name, length) == 0;
}

static fn env_drop(string_address name)
{
        positive length = string_length(name);
        positive keep = 0;

        for (positive i = 0; i < env_have; i++)
                if (!env_same_key(env_list[i], name, length))
                        env_list[keep++] = env_list[i];

        env_have = keep;
}

static bool env_put(string_address entry)
{
        string_address mark = string_first_of(entry, '=');

        if (mark)
        {
                positive length = (positive)(mark - entry);

                for (positive i = 0; i < env_have; i++)
                {
                        if (env_same_key(env_list[i], entry, length))
                        {
                                env_list[i] = entry;
                                return true;
                        }
                }
        }

        if (!shell_room((address_any address_to)address_of env_list,
                        address_of env_room, env_have + 2,
                        sizeof(env_list[0])))
        {
                file_fail("env: environment is too large\n", 0);
                return false;
        }

        env_list[env_have++] = entry;
        return true;
}

static string_address address_to env_dropped;
static positive env_dropped_room;
static positive env_drops;

// -u is the one option here that means it every time it is given, and the
// scanner keeps one value a letter, so each one is written down as it is read
// and they are all applied once the environment to drop them from exists.
static bool env_seen(p8 letter, string_address value)
{
        if (letter != 'u')
                return true;

        if (!shell_room((address_any address_to)address_of env_dropped,
                        address_of env_dropped_room, env_drops + 1,
                        sizeof(env_dropped[0])))
        {
                file_fail("env: unset list is too large\n", 0);
                return false;
        }

        env_dropped[env_drops++] = value;

        return true;
}

/*
        -S, which exists because a shebang line is one argument however many
        words are written on it: the string is cut at its spaces and the
        pieces stand where it stood.

        GNU's -S also reads quotes, backslashes and $VAR out of that string. A
        shebang line has none of them, and cutting a quoted string at the
        wrong space is worse than saying so, so one carrying any of them is
        refused instead.
*/
static p8 address_to env_split_store;
static positive env_split_room;
static string_address address_to env_words;
static positive env_words_room;

static bool env_split(string_address text, positive address_to have)
{
        positive filled = 0;
        positive given = address_to have;
        positive i = 0;
        positive length = string_length(text);

        for (positive j = 0; string_get(text + j); j++)
        {
                p8 letter = string_get(text + j);

                if (letter == '"' || letter == '\'' || letter == '\\' || letter == '$')
                {
                        file_fail("env: -S here cuts at spaces and reads nothing else\n", 0);
                        return false;
                }
        }

        /*
                Reserve before storing pointers into the byte block: growing
                it after the first word would move the text underneath those
                pointers. At most every second byte begins a one-byte word.
        */
        if (!shell_room((address_any address_to)address_of env_split_store,
                        address_of env_split_room, length + 1,
                        sizeof(env_split_store[0])) ||
            !shell_room((address_any address_to)address_of env_words,
                        address_of env_words_room,
                        given + length / 2 + 2, sizeof(env_words[0])))
        {
                file_fail("env: split string is too large\n", 0);
                return false;
        }

        while (string_get(text + i))
        {
                i += string_span(text + i, string_set_blanks);

                if (string_is(text + i, end))
                        break;

                env_words[given++] = env_split_store + filled;

                while (string_get(text + i) && !string_is(text + i, ' ') &&
                       !string_is(text + i, '\t'))
                        env_split_store[filled++] = string_get(text + i++);

                env_split_store[filled++] = end;
        }

        address_to have = given;

        return true;
}

static b32 file_env()
{
        env_have = 0;
        env_drops = 0;

        file_taking taking = {
            .program = (string_address) "env",
            .allowed = (string_address) "ai0uCSv",
            .valued = (string_address) "auCS",
            .longs = env_longs,
            .seen = env_seen,
        };

        // 125 is env's own failure, told apart from 126 for a command that
        // cannot be run and 127 for one that is not there.
        if (!file_take(address_of taking))
                return 125;

        if (taking.flags & (FILE_FLAG('b') | FILE_FLAG('d') | FILE_FLAG('g')))
                file_fail("env: the signal options are taken here and change nothing\n", 0);

        positive index = taking.first;
        positive count = (positive)program_argument_count();
        bool empty = (taking.flags & FILE_FLAG('i')) != 0;

        // A mere -, from before env had options to spell it with, means -i.
        if (index < count && string_is(program_argument((b32)index), '-') &&
            string_is(program_argument((b32)index) + 1, end))
        {
                empty = true;
                index++;
        }

        if (!empty)
        {
                string_address address_to process = file_environment_all();

                for (b32 i = 0; process && process[i]; i++)
                        if (!env_put(process[i]))
                                return 125;
        }

        for (positive i = 0; i < env_drops; i++)
                env_drop(env_dropped[i]);

        /*
                What -S carries stands where -S stood, ahead of the words that
                followed it, and the whole lot is read as though it had been
                written out: assignments first and then the command.
        */
        positive have = 0;
        string_address split = file_option_value(address_of taking, 'S');

        if (split && !env_split(split, address_of have))
                return 125;

        if (!shell_room((address_any address_to)address_of env_words,
                        address_of env_words_room,
                        have + count - index + 1, sizeof(env_words[0])))
        {
                file_fail("env: argument list is too large\n", 0);
                return 125;
        }

        while (index < count)
                env_words[have++] = program_argument((b32)index++);

        env_words[have] = null;

        positive at = 0;

        while (at < have && string_first_of(env_words[at], '='))
        {
                if (!env_put(env_words[at++]))
                        return 125;
        }

        if (!shell_room((address_any address_to)address_of env_list,
                        address_of env_room, env_have + 1,
                        sizeof(env_list[0])))
                return 125;

        env_list[env_have] = null;

        string_address where = file_option_value(address_of taking, 'C');

        if (at >= have)
        {
                // -C and -a are instructions for running something, and
                // printing the environment is not running something; there is
                // nothing to do with either of them but say so.
                if (where)
                {
                        file_fail("env: must specify command with --chdir (-C)\n", 0);
                        return 125;
                }

                if (file_option_value(address_of taking, 'a'))
                {
                        file_fail("env: must specify command with --argv0 (-a)\n", 0);
                        return 125;
                }

                bool zero = (taking.flags & FILE_FLAG('0')) != 0;

                for (positive i = 0; i < env_have; i++)
                        file_written(env_list[i], zero);

                log_flush();
                return 0;
        }

        if (where && system_call_1(syscall(chdir), (positive)where) < 0)
        {
                string_format(file_fail, "env: cannot change directory to %s\n", where);
                return 125;
        }

        string_address address_to arguments = env_words + at;
        string_address name = env_words[at];

        // -a renames the command without changing which file is run, which is
        // the whole of what argv[0] is for.
        string_address zeroth = file_option_value(address_of taking, 'a');

        if (zeroth)
                arguments[0] = zeroth;

        log_flush();

        // PATH from the environment being handed on, not from the one this
        // program was started with: env -i changes both. The shared search
        // also keeps -a's argv[0] separate from the file name.
        string_address path =
            string_get_environment(env_list, (string_address) "PATH");
        bipolar answer =
            file_exec_path_try_in(name, arguments, env_list, path);

        string_format(file_fail, "env: '%s': %s\n", name,
                      answer == -ERROR_ACCESS ? "Permission denied"
                                              : "No such file or directory");

        return answer == -ERROR_ACCESS ? 126 : 127;
}

// id ------------------------------------------------------------
/*
        id [-u|-g|-G] [-n] [-r] [-z] [USER]...

        -n and -r say how to print an id rather than which one to print, so
        neither means anything without -u, -g or -G and GNU refuses them
        there rather than guessing; -z is refused in the readable default for
        the same reason, since that line is for a person to read.

        With a USER the answer comes out of /etc/passwd and /etc/group: the
        kernel can only be asked about this process, and this process is not
        the user being asked about.
*/
#define ID_GROUPS_MAX 64

static const file_long id_longs[] = {
    {(string_address) "context", 'Z'},
    {(string_address) "group", 'g'},
    {(string_address) "groups", 'G'},
    {(string_address) "name", 'n'},
    {(string_address) "real", 'r'},
    {(string_address) "user", 'u'},
    {(string_address) "zero", 'z'},
    {null, 0},
};

static fn id_named(positive value, bool group)
{
        p8 name[FILE_NAME_MAX];
        bool known = group ? file_group_name(value, name, FILE_NAME_MAX)
                           : file_user_name(value, name, FILE_NAME_MAX);

        positive_to_string(log, value);

        if (known)
        {
                log("(", 1);
                log(name, 0);
                log(")", 1);
        }
}

static fn id_alone(positive value, bool group, bool names, bool zero)
{
        p8 text[FILE_NAME_MAX];

        if (!names || !(group ? file_group_name(value, text, FILE_NAME_MAX)
                              : file_user_name(value, text, FILE_NAME_MAX)))
                positive_into_string(text, value);

        file_written(text, zero);
}

/*
        The primary group first and then every group whose member list names
        the user. The kernel hands the supplementary groups back in its own
        order and does not promise the primary one is among them, so the group
        actually in effect belongs at the front either way.
*/
static positive id_groups_named(string_address name, positive primary,
                                p32 address_to into, positive limit)
{
        p8 address_to text = file_group_text();
        positive wanted = string_length(name);
        positive have = 0;
        positive at = 0;

        if (limit)
                into[have++] = (p32)primary;

        while (text[at] && have < limit)
        {
                positive stop = (positive)(string_first_of_or_end(text + at, '\n') - text);

                // name:password:gid:member,member
                positive colon[3] = {stop, stop, stop};
                positive seen = 0;

                for (positive i = at; i < stop && seen < 3; i++)
                        if (text[i] == ':')
                                colon[seen++] = i;

                positive taken = 0;
                positive value = seen == 3
                                     ? string_digits_max(text + colon[1] + 1,
                                                         colon[2] - colon[1] - 1,
                                                         address_of taken)
                                     : 0;
                bool numeric = seen == 3 && colon[2] > colon[1] + 1 &&
                               taken == colon[2] - colon[1] - 1;

                positive i = colon[2] + 1;

                while (numeric && value != primary && i < stop)
                {
                        positive from = i;

                        while (i < stop && text[i] != ',')
                                i++;

                        if (i - from == wanted &&
                            !memory_compare(text + from, name, wanted))
                        {
                                into[have++] = (p32)value;
                                break;
                        }

                        if (i < stop)
                                i++;
                }

                at = stop;

                if (text[at])
                        at++;
        }

        return have;
}

static fn id_written(positive user, positive group, p32 address_to members,
                     positive have, positive flags, bool names, bool zero)
{
        if (flags & FILE_FLAG('u'))
        {
                id_alone(user, false, names, zero);
                return;
        }

        if (flags & FILE_FLAG('g'))
        {
                id_alone(group, true, names, zero);
                return;
        }

        if (flags & FILE_FLAG('G'))
        {
                for (positive i = 0; i < have; i++)
                {
                        p8 text[FILE_NAME_MAX];

                        if (!names || !file_group_name(members[i], text, FILE_NAME_MAX))
                                positive_into_string(text, members[i]);

                        if (zero)
                                file_written(text, true);
                        else
                        {
                                if (i)
                                        log(" ", 1);

                                log(text, 0);
                        }
                }

                if (!zero)
                        log("\n", 1);

                return;
        }

        log("uid=", 0);
        id_named(user, false);
        log(" gid=", 0);
        id_named(group, true);

        if (have > 0)
        {
                log(" groups=", 0);

                for (positive i = 0; i < have; i++)
                {
                        if (i)
                                log(",", 1);

                        id_named(members[i], true);
                }
        }

        log("\n", 1);
}

static b32 file_id()
{
        file_taking taking = {
            .program = (string_address) "id",
            .allowed = (string_address) "aguGnrzZ",
            .valued = (string_address) "",
            .longs = id_longs,
        };

        if (!file_take(address_of taking))
                return 1;

        positive flags = taking.flags;
        bool names = (flags & FILE_FLAG('n')) != 0;
        bool real = (flags & FILE_FLAG('r')) != 0;
        bool zero = (flags & FILE_FLAG('z')) != 0;
        bool one = (flags & (FILE_FLAG('u') | FILE_FLAG('g') | FILE_FLAG('G'))) != 0;

        // -Z asks for a security context. Nothing here keeps one, and an
        // empty answer would read as a process that has no context rather
        // than as a tool with nothing to say about it.
        if (flags & FILE_FLAG('Z'))
        {
                file_fail("id: --context (-Z) works only on an SELinux-enabled kernel\n", 0);
                return 1;
        }

        if ((names || real) && !one)
        {
                file_fail("id: printing only names or real IDs requires -u, -g, or -G\n", 0);
                return 1;
        }

        if (zero && !one)
        {
                file_fail("id: option --zero not permitted in default format\n", 0);
                return 1;
        }

        positive first = taking.first;
        positive count = (positive)program_argument_count();
        p32 members[ID_GROUPS_MAX];

        if (first < count)
        {
                b32 status = 0;

                while (first < count)
                {
                        string_address who = program_argument((b32)first++);
                        bipolar user = file_user_id(who);
                        bipolar group = file_account_id(file_password_text(), who, 3);

                        if (user < 0 || group < 0)
                        {
                                string_format(file_fail, "id: '%s': no such user\n", who);
                                status = 1;
                                continue;
                        }

                        positive have = id_groups_named(who, (positive)group, members,
                                                        ID_GROUPS_MAX);

                        id_written((positive)user, (positive)group, members, have, flags,
                                   names, zero);
                }

                log_flush();

                return status;
        }

        positive user = (positive)system_call(syscall(getuid));
        positive effective_user = (positive)system_call(syscall(geteuid));
        positive group = (positive)system_call(syscall(getgid));
        positive effective_group = (positive)system_call(syscall(getegid));

        if (!real)
        {
                user = effective_user;
                group = effective_group;
        }

        bipolar have = system_call_2(syscall(getgroups), ID_GROUPS_MAX - 1,
                                     (positive)(members + 1));

        if (have < 0)
                have = 0;

        // The kernel hands the supplementary groups back in its own order and
        // does not promise the primary one is among them; the group actually
        // in effect belongs at the front.
        members[0] = (p32)group;

        positive keep = 1;

        for (positive i = 1; i <= (positive)have; i++)
                if (members[i] != (p32)group)
                        members[keep++] = members[i];

        id_written(user, group, members, keep, flags, names, zero);

        log_flush();

        return 0;
}

// hostname ------------------------------------------------------------
// hostname, and hostname -s for the part before the first dot.
static b32 file_hostname()
{
        file_machine facts;

        // -f is not here. The kernel's node name is the whole of what this
        // knows, and the full name -f asks for is a question for a resolver.
        file_taking taking = {
            .program = (string_address) "hostname",
            .allowed = (string_address) "s",
            .valued = (string_address) "",
        };

        if (!file_take(address_of taking))
                return 1;

        memory_fill(address_of facts, 0, sizeof(facts));

        if (system_call_1(syscall(uname), (positive)address_of facts) < 0)
        {
                file_fail("hostname: cannot read system name\n", 0);
                return 1;
        }

        if (taking.flags & FILE_FLAG('s'))
        {
                string_address dot = string_first_of(facts.node, '.');

                if (dot)
                        address_to dot = end;
        }

        file_line(facts.node);
        log_flush();

        return 0;
}

// uname ------------------------------------------------------------
/*
        uname [-asnrvmpio]

        -a is every field the kernel actually keeps. The system's own uname
        adds a compiled in operating system name after them, which is not in
        struct utsname and is not ours to claim, so -o names this system and
        -a stops at the machine.

        By the same rule -p answers unknown, as GNU's does on Linux: the
        processor type is not a field the kernel keeps either, and the machine
        name is a different question wearing its coat.
*/
static const file_long uname_longs[] = {
    {(string_address) "all", 'a'},
    {(string_address) "kernel-name", 's'},
    {(string_address) "nodename", 'n'},
    {(string_address) "kernel-release", 'r'},
    {(string_address) "kernel-version", 'v'},
    {(string_address) "machine", 'm'},
    {(string_address) "processor", 'p'},
    {(string_address) "hardware-platform", 'i'},
    {(string_address) "operating-system", 'o'},
    {null, 0},
};

static b32 file_uname()
{
        file_machine facts;
        file_taking taking = {
            .program = (string_address) "uname",
            .allowed = (string_address) "asnrvmpio",
            .valued = (string_address) "",
            .longs = uname_longs,
        };

        if (!file_take(address_of taking))
                return 1;

        positive flags = taking.flags;

        memory_fill(address_of facts, 0, sizeof(facts));

        if (system_call_1(syscall(uname), (positive)address_of facts) < 0)
        {
                file_fail("uname: cannot read system name\n", 0);
                return 1;
        }

        bool all = (flags & FILE_FLAG('a')) != 0;
        bool any = flags != 0;
        positive written = 0;

        if (all || (flags & FILE_FLAG('s')) || !any)
        {
                log(facts.system, 0);
                written++;
        }

        if (all || (flags & FILE_FLAG('n')))
        {
                if (written++)
                        log(" ", 1);

                log(facts.node, 0);
        }

        if (all || (flags & FILE_FLAG('r')))
        {
                if (written++)
                        log(" ", 1);

                log(facts.release, 0);
        }

        if (all || (flags & FILE_FLAG('v')))
        {
                if (written++)
                        log(" ", 1);

                log(facts.version, 0);
        }

        if (all || (flags & FILE_FLAG('m')))
        {
                if (written++)
                        log(" ", 1);

                log(facts.machine, 0);
        }

        // Asked for on their own they answer unknown; asked for as part of
        // -a they are left out, which is what -a means by "except omit -p and
        // -i if unknown" and is why -a is not simply all the others.
        if ((flags & FILE_FLAG('p')) && !all)
        {
                if (written++)
                        log(" ", 1);

                log("unknown", 0);
        }

        if ((flags & FILE_FLAG('i')) && !all)
        {
                if (written++)
                        log(" ", 1);

                log("unknown", 0);
        }

        if (flags & FILE_FLAG('o'))
        {
                if (written++)
                        log(" ", 1);

                log("Moonwater", 0);
        }

        log("\n", 1);
        log_flush();

        return 0;
}

// mktemp ----------------------------------------------------------
/*
        A name nothing else is using, and the file or directory that claims it.

        The claim is the open: O_EXCL is what makes the name ours rather than
        merely unlikely, and is the difference between this and printing a
        name that looks random. -u asks for exactly that lesser thing.

        The X's do not have to be at the end. The last run of them is what is
        replaced, so run.XXXXXX.log keeps its suffix.
*/
#define MKTEMP_ATTEMPTS 200
#define MKTEMP_LEAST 3

#define FILE_EXCLUSIVE 0200

static string_address mktemp_letters =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

// The kernel's randomness, and the clock when there is none to be had.
static fn mktemp_letters_into(p8 address_to at, positive count)
{
        p8 raw[64];
        positive have = count > sizeof(raw) ? sizeof(raw) : count;

        if (system_call_3(syscall(getrandom), (positive)raw, have, 0) != (bipolar)have)
        {
                p64 now[2] = {0, 0};

                system_call_2(syscall(clock_gettime), 0, (positive)now);

                positive mixed = (positive)now[1] ^ ((positive)now[0] << 20) ^
                                 ((positive)system_call_1(syscall(getpid), 0) << 40);

                for (positive i = 0; i < have; i++)
                {
                        mixed = mixed * 6364136223846793005u + 1442695040888963407u;
                        raw[i] = (p8)(mixed >> 33);
                }
        }

        for (positive i = 0; i < count; i++)
                at[i] = mktemp_letters[raw[i % have] % 62];
}

// --tmpdir is the one option here spelled without a letter, and T is a letter
// mktemp has not got: it is left out of `allowed` so -T is still a mistake.
static const file_long mktemp_longs[] = {
    {(string_address) "directory", 'd'},
    {(string_address) "dry-run", 'u'},
    {(string_address) "quiet", 'q'},
    {(string_address) "tmpdir", 'T'},
    {null, 0},
};

static b32 file_mktemp()
{
        positive count = (positive)program_argument_count();
        file_taking taking = {
            .program = (string_address) "mktemp",
            .allowed = (string_address) "dpqtu",
            .valued = (string_address) "p",
            .optional = (string_address) "T",
            .longs = mktemp_longs,
        };

        if (!file_take(address_of taking))
                return 1;

        positive index = taking.first;
        bool directory = (taking.flags & FILE_FLAG('d')) != 0;
        bool dry = (taking.flags & FILE_FLAG('u')) != 0;
        bool quiet = (taking.flags & FILE_FLAG('q')) != 0;
        bool rooted = (taking.flags & (FILE_FLAG('p') | FILE_FLAG('t') |
                                       FILE_FLAG('T'))) != 0;
        string_address base = file_option_value(address_of taking, 'T');
        string_address template = null;
        p8 path[FILE_PATH_MAX];
        positive length = 0;
        positive marks_at;
        positive marks = 0;

        if (!base)
                base = file_option_value(address_of taking, 'p');

        if (index < count)
                template = program_argument((b32)index++);

        if (index < count)
        {
                file_fail("mktemp: too many templates\n", 0);
                return 1;
        }

        if (!template)
        {
                template = "tmp.XXXXXXXXXX";
                rooted = true;
        }

        if (rooted && !string_is(template, '/'))
        {
                if (!base)
                        base = file_environment("TMPDIR");

                if (!base || !string_get(base))
                        base = "/tmp";

                length = string_length(base);

                if (length > FILE_PATH_MAX - 2)
                {
                        file_fail("mktemp: template too long\n", 0);
                        return 1;
                }

                memory_copy_fast(path, base, length);

                while (length > 1 && path[length - 1] == '/')
                        length--;

                path[length++] = '/';
        }

        p8 address_to stopped = string_copy_max_end(
            path + length, template, FILE_PATH_MAX - 1 - length);
        positive added = (positive)(stopped - (path + length));

        if (string_get(template + added))
        {
                file_fail("mktemp: template too long\n", 0);
                return 1;
        }

        length += added;

        marks_at = length;

        while (marks_at && path[marks_at - 1] != 'X')
                marks_at--;

        while (marks_at && path[marks_at - 1] == 'X')
        {
                marks_at--;
                marks++;
        }

        if (marks < MKTEMP_LEAST)
        {
                string_format(file_fail, "mktemp: too few X's in template '%s'\n",
                              template);
                return 1;
        }

        for (positive attempt = 0; attempt < MKTEMP_ATTEMPTS; attempt++)
        {
                bipolar answer;

                mktemp_letters_into(path + marks_at, marks);

                if (dry)
                        break;

                if (directory)
                        answer = system_call_3(syscall(mkdirat),
                                               (positive)(bipolar)AT_FDCWD,
                                               (positive)path, 0700);
                else
                {
                        answer = system_call_4(syscall(openat),
                                               (positive)(bipolar)AT_FDCWD,
                                               (positive)path,
                                               FILE_WRITE | FILE_EXCLUSIVE, 0600);

                        if (answer >= 0)
                                system_call_1(syscall(close), (positive)answer);
                }

                if (answer >= 0)
                {
                        file_line(path);
                        log_flush();
                        return 0;
                }

                if (answer != -ERROR_EXISTS)
                {
                        if (!quiet)
                                string_format(file_fail,
                                              "mktemp: failed to create %s via template '%s'\n",
                                              directory ? "directory" : "file",
                                              template);

                        return 1;
                }
        }

        if (dry)
        {
                file_line(path);
                log_flush();
                return 0;
        }

        if (!quiet)
                string_format(file_fail,
                              "mktemp: failed to create %s via template '%s'\n",
                              directory ? "directory" : "file", template);

        return 1;
}

// kill ------------------------------------------------------------
/*
        A signal, sent.

        The names are one table read two ways: printed for -l, and walked to
        turn a name back into a number, so the two can never disagree about
        what SIGRTMIN+3 is called.

        Sixteen through thirty three have no name here for the same reason
        they have none in dash: what they are called is not the same on every
        machine, and a number is always right.
*/
#define KILL_NAMED 34
#define KILL_LEAST_REAL 34
#define KILL_MOST 64

static string_address kill_names[KILL_NAMED] = {
    "0", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS",
    "FPE", "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM",
    "16", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG",
    "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS",
    "32", "33",
};

static fn kill_name(positive number, p8 address_to into)
{
        positive at = 0;

        if (number < KILL_NAMED)
        {
                string_copy(into, kill_names[number]);
                return;
        }

        if (number > KILL_MOST)
        {
                positive_into_string(into, number);
                return;
        }

        into[at++] = 'R';
        into[at++] = 'T';
        into[at++] = 'M';

        if (number <= KILL_LEAST_REAL + 15)
        {
                into[at++] = 'I';
                into[at++] = 'N';

                if (number == KILL_LEAST_REAL)
                {
                        into[at] = end;
                        return;
                }

                into[at++] = '+';
                positive_into_string(into + at, number - KILL_LEAST_REAL);
                return;
        }

        into[at++] = 'A';
        into[at++] = 'X';

        if (number == KILL_MOST)
        {
                into[at] = end;
                return;
        }

        into[at++] = '-';
        positive_into_string(into + at, KILL_MOST - number);
}

static bipolar kill_number(string_address word)
{
        p8 name[16];
        positive number;

        if (string_digits_exact(word, address_of number))
                return (bipolar)number;

        if (string_is(word, 'S') && string_is(word + 1, 'I') && string_is(word + 2, 'G'))
                word += 3;

        positive found = string_table_find(word, kill_names + 1,
                                           sizeof(kill_names[0]), KILL_NAMED - 1);

        if (found != KILL_NAMED - 1)
                return (bipolar)(found + 1);

        for (positive i = KILL_LEAST_REAL; i <= KILL_MOST; i++)
        {
                kill_name(i, name);

                if (!string_compare(word, name))
                        return (bipolar)i;
        }

        return -1;
}

static b32 kill_list(positive count, positive index)
{
        p8 name[16];

        if (index >= count)
        {
                for (positive i = 0; i <= KILL_MOST; i++)
                {
                        kill_name(i, name);
                        file_line(name);
                }

                log_flush();
                return 0;
        }

        {
                string_address word = program_argument((b32)index);
                positive number;

                if (!string_digits_exact(word, address_of number))
                {
                        string_format(file_fail, "kill: Illegal number: %s\n", word);
                        return 2;
                }

                // A status carries the signal that ended a process in its low
                // seven bits, which is what a caller of -l usually has.
                if (number > 128)
                        number -= 128;

                if (!number || number > KILL_MOST)
                {
                        string_format(file_fail, "kill: Illegal number: %s\n", word);
                        return 2;
                }

                kill_name(number, name);
                file_line(name);
        }

        log_flush();
        return 0;
}

static b32 file_kill()
{
        positive count = (positive)program_argument_count();
        positive index = 1;
        bipolar number = 15;
        b32 answer = 0;

        while (index < count)
        {
                string_address argument = program_argument((b32)index);

                if (!string_is(argument, '-') || string_is(argument + 1, end))
                        break;

                if (string_is(argument + 1, '-') && string_is(argument + 2, end))
                {
                        index++;
                        break;
                }

                if (string_is(argument + 1, 'l') && string_is(argument + 2, end))
                        return kill_list(count, index + 1);

                if (string_is(argument + 1, 's') && string_is(argument + 2, end))
                {
                        if (index + 1 >= count)
                        {
                                file_fail("kill: -s needs a signal\n", 0);
                                return 2;
                        }

                        number = kill_number(program_argument((b32)(index + 1)));
                        index += 2;
                }
                else
                {
                        number = kill_number(argument + 1);
                        index++;
                }

                if (number < 0 || number > KILL_MOST)
                {
                        string_format(file_fail, "kill: invalid signal: %s\n", argument);
                        return 2;
                }

                // One signal and no more, or a negative process group would be
                // read as a second one.
                break;
        }

        if (index >= count)
        {
                file_fail("kill: usage: kill [-s signal | -signal] pid ...\n", 0);
                return 2;
        }

        while (index < count)
        {
                string_address word = program_argument((b32)index++);
                positive used;

                // Unlike the shared signed grammar, kill historically does
                // not accept a leading plus on a process id.
                bipolar who = string_bipolar(word, address_of used);
                if (string_is(word, '+') || !used || string_get(word + used))
                {
                        string_format(file_fail, "kill: Illegal number: %s\n", word);
                        return 2;
                }

                if (system_call_2(syscall(kill), (positive)who, (positive)number) < 0)
                {
                        string_format(file_fail, "kill: (%s) - No such process\n", word);
                        answer = 1;
                }
        }

        return answer;
}

// date ------------------------------------------------------------
/*
        A moment, printed how the format asks for it.

        Everything here is UTC, for the reason file_civil gives: the machine's
        own zone is a binary file this tree has no reader for. TZ=UTC0 is what
        the system's own date has to be told to agree.

        The conversion is already written -- file_split_moment -- so what is
        left is the walk over the format, and the four tables a name comes out
        of.
*/
static string_address date_day_short[7] = {"Sun", "Mon", "Tue", "Wed",
                                           "Thu", "Fri", "Sat"};
static string_address date_day_long[7] = {"Sunday", "Monday", "Tuesday",
                                          "Wednesday", "Thursday", "Friday",
                                          "Saturday"};
static string_address date_month_short[12] = {"Jan", "Feb", "Mar", "Apr",
                                              "May", "Jun", "Jul", "Aug",
                                              "Sep", "Oct", "Nov", "Dec"};
static string_address date_month_long[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};

typedef struct
{
        b64 when;
        b64 year;
        positive month;
        positive day;
        positive hour;
        positive minute;
        positive second;
        positive weekday;
        positive yearday;
} date_moment;

static fn date_take(b64 when, date_moment address_to out)
{
        b64 days = when / 86400;
        positive before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        bool leap;

        if (when % 86400 < 0)
                days--;

        out->when = when;

        file_split_moment(when, address_of out->year, address_of out->month,
                          address_of out->day, address_of out->hour,
                          address_of out->minute, address_of out->second);

        // The first of January 1970 was a Thursday, which is what the four is.
        out->weekday = (positive)(((days % 7) + 11) % 7);

        leap = (out->year % 4 == 0 && out->year % 100 != 0) || out->year % 400 == 0;
        out->yearday = before[out->month - 1] + out->day +
                       (leap && out->month > 2 ? 1 : 0);
}

// A one means the format asked for no padding at all, which is not the same
// as not asking, and a zero cannot say both.
static fn date_field(writer write, positive value, positive width, p8 pad, p8 fallback)
{
        positive_to_padded(write, value, width,
                           pad == 1 ? 0 : pad ? pad : fallback, 0);
}

static fn date_signed(writer write, b64 value)
{
        bipolar_to_string(write, (bipolar)value);
}

static fn date_shape(writer write, date_moment address_to at, string_address format);

/*
        The week of the year, three ways.

        %U and %W count from the first Sunday and the first Monday. ISO 8601
        counts from the week holding the first Thursday, which is why it is
        found by walking to this week's Thursday and asking what year and what
        day of the year that landed on -- the last days of December belong to
        the next year's week one, and the first of January often to the last.
*/
static positive date_week(date_moment address_to at, p8 which)
{
        positive from_monday = (at->weekday + 6) % 7;

        if (which == 'U')
                return (at->yearday + 6 - at->weekday) / 7;

        return (at->yearday + 6 - from_monday) / 7;
}

static fn date_thursday(date_moment address_to at, date_moment address_to out)
{
        positive iso = at->weekday ? at->weekday : 7;

        date_take(at->when + (b64)(4 - (bipolar)iso) * 86400, out);
}

static fn date_letter(writer write, date_moment address_to at, p8 letter, p8 pad)
{
        positive twelve;

        switch (letter)
        {
        case 'Y':
                return date_field(write, (positive)at->year, 4, pad, '0');
        case 'y':
                return date_field(write, (positive)(at->year % 100), 2, pad, '0');
        case 'C':
                return date_field(write, (positive)(at->year / 100), 2, pad, '0');
        case 'm':
                return date_field(write, at->month, 2, pad, '0');
        case 'd':
                return date_field(write, at->day, 2, pad, '0');
        case 'e':
                return date_field(write, at->day, 2, pad, ' ');
        case 'H':
                return date_field(write, at->hour, 2, pad, '0');
        case 'k':
                return date_field(write, at->hour, 2, pad, ' ');
        case 'I':
        case 'l':
                twelve = at->hour % 12;
                twelve = twelve ? twelve : 12;
                return date_field(write, twelve, 2, pad,
                                  letter == 'l' ? ' ' : '0');
        case 'M':
                return date_field(write, at->minute, 2, pad, '0');
        case 'S':
                return date_field(write, at->second, 2, pad, '0');
        case 'j':
                return date_field(write, at->yearday, 3, pad, '0');
        case 'a':
                return write(date_day_short[at->weekday], 0);
        case 'A':
                return write(date_day_long[at->weekday], 0);
        case 'b':
        case 'h':
                return write(date_month_short[at->month - 1], 0);
        case 'B':
                return write(date_month_long[at->month - 1], 0);
        case 'p':
                return write(at->hour < 12 ? "AM" : "PM", 2);
        case 'P':
                return write(at->hour < 12 ? "am" : "pm", 2);
        case 'u':
                return positive_to_string(write, at->weekday ? at->weekday : 7);
        case 'w':
                return positive_to_string(write, at->weekday);
        case 'Z':
                return write("UTC", 3);
        case 'z':
                return write("+0000", 5);
        case 's':
                return date_signed(write, at->when);
        case 'n':
                return write("\n", 1);
        case 't':
                return write("\t", 1);
        case '%':
                return write("%", 1);
        case 'q':
                return positive_to_string(write, (at->month + 2) / 3);
        case 'N':
                return write("000000000", 9);
        case 'U':
        case 'W':
                return date_field(write, date_week(at, letter), 2, pad, '0');
        case 'V':
        case 'G':
        case 'g':
        {
                date_moment middle;

                date_thursday(at, address_of middle);

                if (letter == 'V')
                        return date_field(write, (middle.yearday - 1) / 7 + 1, 2, pad, '0');

                if (letter == 'g')
                        return date_field(write, (positive)(middle.year % 100), 2, pad, '0');

                return date_field(write, (positive)middle.year, 4, pad, '0');
        }
        case 'c':
                return date_shape(write, at, "%a %b %e %H:%M:%S %Y");
        case 'x':
                return date_shape(write, at, "%m/%d/%y");
        case 'X':
                return date_shape(write, at, "%H:%M:%S");
        case 'F':
                return date_shape(write, at, "%Y-%m-%d");
        case 'T':
                return date_shape(write, at, "%H:%M:%S");
        case 'R':
                return date_shape(write, at, "%H:%M");
        case 'D':
                return date_shape(write, at, "%m/%d/%y");
        case 'r':
                return date_shape(write, at, "%I:%M:%S %p");
        }

        // What the tool this is measured against does with a letter it has no
        // meaning for: hand it back.
        write("%", 1);
        write(address_of letter, 1);
}

static fn date_shape(writer write, date_moment address_to at, string_address format)
{
        while (string_get(format))
        {
                string_address mark = string_first_of_or_end(format, '%');
                p8 pad = 0;

                if (mark != format)
                        write(format, (positive)(mark - format));

                if (!string_get(mark))
                        return;

                format = mark + 1;
                p8 letter = string_get(format);

                while (letter == '-' || letter == '_' || letter == '0')
                {
                        pad = letter == '-' ? 1 : letter == '_' ? ' ' : '0';
                        format++;
                        letter = string_get(format);
                }

                if (!letter)
                {
                        write("%", 1);
                        return;
                }

                format++;
                date_letter(write, at, letter, pad);
        }
}

static const file_long date_longs[] = {
    {(string_address) "date", 'd'},
    {(string_address) "reference", 'r'},
    {(string_address) "utc", 'u'},
    {(string_address) "universal", 'u'},
    {(string_address) "rfc-2822", 'R'},
    {(string_address) "rfc-email", 'R'},
    {(string_address) "iso-8601", 'I'},
    {null, 0},
};

static b32 file_date()
{
        positive count = (positive)program_argument_count();
        file_taking taking = {
            .program = (string_address) "date",
            .allowed = (string_address) "IRdru",
            .valued = (string_address) "dr",
            .optional = (string_address) "I",
            .longs = date_longs,
        };

        if (!file_take(address_of taking))
                return 1;

        positive index = taking.first;
        string_address format = null;
        string_address given = file_option_value(address_of taking, 'd');
        string_address of_file = file_option_value(address_of taking, 'r');
        string_address iso = null;
        bool rfc = (taking.flags & FILE_FLAG('R')) != 0;
        b64 when;
        date_moment at;

        if (taking.flags & FILE_FLAG('I'))
        {
                string_address precision = file_option_value(address_of taking, 'I');

                if (!precision || string_is(precision, end) ||
                    !string_compare(precision, "date"))
                        iso = (string_address) "%Y-%m-%d";
                else if (!string_compare(precision, "hours"))
                        iso = (string_address) "%Y-%m-%dT%H+00:00";
                else if (!string_compare(precision, "minutes"))
                        iso = (string_address) "%Y-%m-%dT%H:%M+00:00";
                else if (!string_compare(precision, "seconds"))
                        iso = (string_address) "%Y-%m-%dT%H:%M:%S+00:00";
                else
                {
                        string_format(file_fail,
                                      "date: invalid argument '%s' for '--iso-8601'\n",
                                      precision);
                        return 1;
                }
        }

        if (index < count)
        {
                string_address argument = program_argument((b32)index++);

                if (!string_is(argument, '+'))
                {
                        string_format(file_fail, "date: cannot set the date: %s\n",
                                      argument);
                        return 1;
                }

                format = argument + 1;
        }

        if (index < count)
        {
                file_fail("date: too many operands\n", 0);
                return 1;
        }

        if (of_file)
        {
                file_facts facts;

                if (!file_look_at(of_file, address_of facts))
                {
                        string_format(file_fail,
                                      "date: cannot access '%s': No such file or directory\n",
                                      of_file);
                        return 1;
                }

                when = (b64)facts.modified.seconds;
        }
        else if (given)
        {
                if (!file_moment_read(given, file_now(), address_of when))
                {
                        string_format(file_fail, "date: invalid date '%s'\n", given);
                        return 1;
                }
        }
        else
                when = file_now();

        date_take(when, address_of at);

        if (!format)
                format = iso   ? iso
                         : rfc ? (string_address) "%a, %d %b %Y %H:%M:%S %z"
                               : (string_address) "%a %b %e %H:%M:%S %Z %Y";

        date_shape(log, address_of at, format);
        log("\n", 1);
        log_flush();

        return 0;
}

// xargs -----------------------------------------------------------
/*
        Standard input turned into arguments, and a command run with them.

        The splitting is the part with the rules: blanks and newlines end an
        item, a backslash takes away whatever follows it, and a quote runs to
        its own kind again with nothing special inside. -0 has none of that
        and reads to the next zero byte, which is what find -print0 is for.

        Running is file_run's fork and execve, which find -exec uses too.
*/
#define XARGS_BATCH_BYTES 131072
#define XARGS_READ_BYTES 65536

static positive xargs_used;
static string_address address_to xargs_words;
static positive xargs_word_count;
static positive xargs_word_room;
static positive xargs_prefix_bytes;
static positive xargs_prefix_words;
static string_address address_to xargs_template;
static p8 address_to xargs_item;
static positive xargs_item_length;
static positive xargs_item_room;
static bool xargs_item_broken;
static p8 address_to xargs_buffer;

static bool xargs_null;
static bool xargs_trace;
static bool xargs_needs_input;
static positive xargs_most;
static string_address xargs_replace;
static string_address xargs_ending;
static positive xargs_lines;
static b32 xargs_answer;
static bool xargs_done;
static bool xargs_ended;
static bool xargs_ran;
static positive xargs_line_count;

static bool xargs_words_room(positive extra)
{
        if (xargs_word_count == positive_max ||
            extra > positive_max - xargs_word_count - 1)
                return false;

        positive need = xargs_word_count + extra + 1;

        if (need <= xargs_word_room)
                return true;

        positive room = xargs_word_room ? xargs_word_room : 32;

        while (room < need)
        {
                if (room > positive_max / 2)
                        return false;

                room *= 2;
        }

        if (room > positive_max / sizeof(string_address))
                return false;

        string_address address_to grown =
            (string_address address_to)text_arena_take(
                room * sizeof(string_address));

        if (!grown)
                return false;

        if (xargs_word_count)
                memory_copy_fast(grown, xargs_words,
                                 xargs_word_count * sizeof(string_address));

        xargs_words = grown;
        xargs_word_room = room;
        return true;
}

static bool xargs_add(string_address text, positive length)
{
        if (length == positive_max || xargs_used > positive_max - length - 1 ||
            !xargs_words_room(1))
                return false;

        p8 address_to made = (p8 address_to)text_arena_take(length + 1);

        if (!made)
                return false;

        memory_copy_end(made, text, length);
        xargs_words[xargs_word_count++] = made;
        xargs_used += length + 1;

        return true;
}

static fn xargs_item_put(p8 letter)
{
        if (xargs_item_length > positive_max - 2)
        {
                xargs_item_broken = true;
                return;
        }

        if (xargs_item_length + 2 > xargs_item_room)
        {
                positive room = xargs_item_room ? xargs_item_room : 256;

                while (room < xargs_item_length + 2)
                {
                        if (room > positive_max / 2)
                        {
                                xargs_item_broken = true;
                                return;
                        }

                        room *= 2;
                }

                p8 address_to grown = (p8 address_to)text_arena_take(room);

                if (!grown)
                {
                        xargs_item_broken = true;
                        return;
                }

                if (xargs_item_length)
                        memory_copy_fast(grown, xargs_item, xargs_item_length);

                xargs_item = grown;
                xargs_item_room = room;
        }

        xargs_item[xargs_item_length++] = letter;
}

/*
        The command, found and started.

        Every candidate is tried by execing it: asking first whether a file is
        there and executable and then running it is two answers where one will
        do, and the one that matters is the kernel's. What is remembered is
        whether anything said permission denied, because that is a different
        number to come back with than nothing being there at all.
*/
#define XARGS_EXEC_SIGNAL (-4097)
#define XARGS_EXEC_SYSTEM (-4098)
#define XARGS_O_CLOEXEC 02000000

static bipolar xargs_execute(string_address address_to words,
                             positive word_count)
{
        b32 ends[2];
        positive status = 0;

        words[word_count] = null;

        if (xargs_trace)
        {
                for (positive i = 0; i < word_count; i++)
                {
                        if (i)
                                file_fail(" ", 1);

                        file_fail(words[i], 0);
                }

                file_fail("\n", 1);
        }

        log_flush();

        if (system_call_2(syscall(pipe2), (positive)ends,
                          XARGS_O_CLOEXEC) < 0)
                return XARGS_EXEC_SYSTEM;

        bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child == 0)
        {
                system_call_1(syscall(close), (positive)ends[0]);
                bipolar answer = file_exec_path_try(words);

                system_write_all((positive)ends[1], address_of answer,
                                 sizeof(answer));
                exit(answer == -ERROR_ACCESS ? 126 : 127);
        }

        system_call_1(syscall(close), (positive)ends[1]);


        if (child < 0)
        {
                system_call_1(syscall(close), (positive)ends[0]);
                return XARGS_EXEC_SYSTEM;
        }

        bipolar exec_error = 0;
        bipolar got = system_read_retry((positive)ends[0], address_of exec_error,
                                        sizeof(exec_error));

        system_call_1(syscall(close), (positive)ends[0]);
        system_wait4_retry(child, address_of status, 0, null);

        if (got == sizeof(exec_error))
                return exec_error;

        if (status & 0x7f)
                return XARGS_EXEC_SIGNAL;

        return (bipolar)((status >> 8) & 0xff);
}

static bool xargs_execute_range(positive first, positive count)
{
        if (count == positive_max ||
            xargs_prefix_words > positive_max - count - 1)
        {
                xargs_answer = 1;
                xargs_done = true;
                return false;
        }

        positive total = xargs_prefix_words + count;

        if (total + 1 > positive_max / sizeof(string_address))
        {
                xargs_answer = 1;
                xargs_done = true;
                return false;
        }

        positive arena_mark = text_arena_used;
        string_address address_to words =
            (string_address address_to)text_arena_take(
                (total + 1) * sizeof(string_address));

        if (!words)
        {
                xargs_answer = 1;
                xargs_done = true;
                return false;
        }

        if (xargs_prefix_words)
                memory_copy_fast(words, xargs_words,
                                 xargs_prefix_words * sizeof(string_address));

        for (positive i = 0; i < count; i++)
                words[xargs_prefix_words + i] =
                    xargs_words[xargs_prefix_words + first + i];

        string_address command = words[0];
        bipolar code = xargs_execute(words, total);

        /* The argv table is per attempt; recursive E2BIG splits reuse it. */
        text_arena_used = arena_mark;

        if (code == -ERROR_ARGUMENT_LIST)
        {
                if (xargs_replace || count <= 1)
                {
                        file_fail("xargs: argument list too long\n", 0);
                        xargs_answer = 1;
                        xargs_done = true;
                        return false;
                }

                positive left = count / 2;

                return xargs_execute_range(first, left) &&
                       xargs_execute_range(first + left, count - left);
        }

        if (code == XARGS_EXEC_SYSTEM)
        {
                file_fail("xargs: cannot fork or execute\n", 0);
                xargs_answer = 125;
                xargs_done = true;
                return false;
        }

        if (code == XARGS_EXEC_SIGNAL)
        {
                string_format(file_fail, "xargs: %s: terminated by a signal\n",
                              command);
                xargs_answer = 125;
                xargs_done = true;
                return false;
        }

        if (!code)
                return true;

        if (code < 0)
        {
                string_format(file_fail, "xargs: failed to run command '%s'\n",
                              command);
                xargs_answer = code == -ERROR_ACCESS ? 126 : 127;
                xargs_done = true;
                return false;
        }

        if (code == 255)
        {
                string_format(file_fail, "xargs: %s: exited with status 255; aborting\n",
                              command);
                xargs_answer = 124;
                xargs_done = true;
                return false;
        }

        xargs_answer = 123;
        return true;
}

static fn xargs_run()
{
        xargs_ran = true;

        positive count = xargs_replace
                             ? 0
                             : xargs_word_count - xargs_prefix_words;

        xargs_execute_range(0, count);
}

static fn xargs_reset()
{
        xargs_used = xargs_prefix_bytes;
        xargs_word_count = xargs_prefix_words;
        xargs_line_count = 0;
}

/*
        The command as it was written down, kept where the built one cannot
        reach it. -I rebuilds the whole command for every item, and reading
        the words out of the block it is writing into gives the second item
        the first one's answer.
*/
static bool xargs_keep_template()
{
        if (!xargs_word_count)
                return true;

        if (xargs_word_count > positive_max / sizeof(string_address))
                return false;

        xargs_template = (string_address address_to)text_arena_take(
            xargs_word_count * sizeof(string_address));

        if (!xargs_template)
                return false;

        memory_copy_fast(xargs_template, xargs_words,
                         xargs_word_count * sizeof(string_address));
        return true;
}

// The command with the mark in each of its words replaced, which is what -I
// is and the only mode where one item makes one whole command line.
static bool xargs_replaced(string_address item)
{
        positive mark = string_length(xargs_replace);
        positive item_length = string_length(item);

        xargs_used = 0;
        xargs_word_count = 0;

        for (positive at = 0; at < xargs_prefix_words; at++)
        {
                string_address word = xargs_template[at];
                string_address scan = word;
                positive length = 0;

                for (;;)
                {
                        string_address hit = mark ? string_find(scan, xargs_replace) : null;
                        positive kept = hit ? (positive)(hit - scan) : string_length(scan);

                        if (kept > positive_max - length)
                                return false;

                        length += kept;

                        if (!hit)
                                break;

                        if (item_length > positive_max - length)
                                return false;

                        length += item_length;
                        scan = hit + mark;
                }

                if (length == positive_max)
                        return false;

                p8 address_to made = (p8 address_to)text_arena_take(length + 1);

                if (!made)
                        return false;

                scan = word;
                positive used = 0;

                for (;;)
                {
                        string_address hit = mark ? string_find(scan, xargs_replace) : null;
                        positive kept = hit ? (positive)(hit - scan) : string_length(scan);

                        memory_copy_fast(made + used, scan, kept);
                        used += kept;

                        if (!hit)
                                break;

                        memory_copy_fast(made + used, item, item_length);
                        used += item_length;
                        scan = hit + mark;
                }

                made[length] = end;

                if (!xargs_add(made, length))
                        return false;
        }

        return true;
}

static fn xargs_item_done()
{
        if (xargs_item_broken)
        {
                file_fail("xargs: argument too long\n", 0);
                xargs_answer = 1;
                xargs_done = true;
                return;
        }

        // The logical end of the input stops the reading; what was gathered
        // before it is still a command to run.
        if (xargs_ending && !xargs_null &&
            !string_compare(xargs_item, xargs_ending))
        {
                xargs_ended = true;
                return;
        }

        if (xargs_replace)
        {
                if (!xargs_replaced(xargs_item))
                {
                        file_fail("xargs: argument list too long\n", 0);
                        xargs_answer = 1;
                        xargs_done = true;
                        return;
                }

                xargs_run();
                return;
        }

        if (xargs_word_count > xargs_prefix_words &&
            (xargs_item_length == positive_max ||
             xargs_used > positive_max - xargs_item_length - 1 ||
             xargs_used + xargs_item_length + 1 > XARGS_BATCH_BYTES))
        {
                xargs_run();

                if (xargs_done)
                        return;

                xargs_reset();
        }

        if (!xargs_add(xargs_item, xargs_item_length))
        {
                file_fail("xargs: argument list too long\n", 0);
                xargs_answer = 1;
                xargs_done = true;
                return;
        }

        if (xargs_most && xargs_word_count - xargs_prefix_words >= xargs_most)
        {
                xargs_run();
                xargs_reset();
        }
}

static bool xargs_count_value(string_address value, positive address_to out)
{
        positive taken = 0;
        positive made = string_digits(value, address_of taken);

        if (!taken || string_get(value + taken) || !made)
                return false;

        address_to out = made;
        return true;
}

static b32 file_xargs()
{
        positive count = (positive)program_argument_count();
        p8 quote = 0;
        bool escaped = false;
        bool started = false;
        bool blank_last = false;
        bool line_had_item = false;

        text_arena_used = 0;
        xargs_words = null;
        xargs_word_room = 0;
        xargs_template = null;
        xargs_item = null;
        xargs_item_room = 0;
        xargs_buffer = null;
        xargs_used = 0;
        xargs_word_count = 0;
        xargs_item_length = 0;
        xargs_item_broken = false;
        xargs_answer = 0;
        xargs_done = false;
        xargs_ended = false;
        xargs_ran = false;
        xargs_lines = 0;

        file_taking taking = {
            .program = (string_address) "xargs",
            .allowed = (string_address) "0EILinrt",
            .valued = (string_address) "EILn",
        };

        if (!file_take(address_of taking))
                return 1;

        positive index = taking.first;

        xargs_null = (taking.flags & FILE_FLAG('0')) != 0;
        xargs_trace = (taking.flags & FILE_FLAG('t')) != 0;
        xargs_needs_input = (taking.flags & FILE_FLAG('r')) != 0;
        xargs_ending = file_option_value(address_of taking, 'E');
        xargs_replace = file_option_value(address_of taking, 'I');
        xargs_most = 0;
        xargs_lines = 0;

        if ((taking.flags & FILE_FLAG('n')) &&
            !xargs_count_value(file_option_value(address_of taking, 'n'),
                               address_of xargs_most))
        {
                file_fail("xargs: invalid number for -n\n", 0);
                return 1;
        }

        if ((taking.flags & FILE_FLAG('L')) &&
            !xargs_count_value(file_option_value(address_of taking, 'L'),
                               address_of xargs_lines))
        {
                file_fail("xargs: invalid number for -L\n", 0);
                return 1;
        }

        if (!xargs_replace && (taking.flags & FILE_FLAG('i')))
                xargs_replace = "{}";

        if (index >= count)
                xargs_add("echo", 4);

        while (index < count)
        {
                string_address word = program_argument((b32)index++);

                if (!xargs_add(word, string_length(word)))
                {
                        file_fail("xargs: command too long\n", 0);
                        return 1;
                }
        }

        xargs_prefix_bytes = xargs_used;
        xargs_prefix_words = xargs_word_count;

        if (!xargs_keep_template())
        {
                file_fail("xargs: command too long\n", 0);
                return 1;
        }

        xargs_item = (p8 address_to)text_arena_take(256);
        xargs_buffer = (p8 address_to)text_arena_take(XARGS_READ_BYTES);

        if (!xargs_item || !xargs_buffer)
                return 1;

        xargs_item_room = 256;

        for (;;)
        {
                bipolar got = system_read_retry(0, xargs_buffer,
                                                 XARGS_READ_BYTES);

                if (got < 0)
                {
                        file_fail("xargs: read error\n", 0);
                        xargs_answer = 1;
                        break;
                }

                if (!got)
                        break;

                for (positive at = 0;
                     at < (positive)got && !xargs_done && !xargs_ended; at++)
                {
                        p8 letter = xargs_buffer[at];

                        if (xargs_null)
                        {
                                if (letter)
                                {
                                        xargs_item_put(letter);

                                        started = true;
                                        continue;
                                }

                                xargs_item[xargs_item_length] = end;
                                xargs_item_done();
                                xargs_item_length = 0;
                                started = false;
                                continue;
                        }

                        if (escaped)
                        {
                                xargs_item_put(letter);

                                escaped = false;
                                started = true;
                                blank_last = false;
                                continue;
                        }

                        if (quote)
                        {
                                if (letter == quote)
                                {
                                        quote = 0;
                                        continue;
                                }

                                xargs_item_put(letter);

                                started = true;
                                blank_last = false;
                                continue;
                        }

                        if (letter == '\\')
                        {
                                escaped = true;
                                started = true;
                                blank_last = false;
                                continue;
                        }

                        if (letter == '\'' || letter == '"')
                        {
                                quote = letter;
                                started = true;
                                blank_last = false;
                                continue;
                        }

                        // -I reads a line at a time, and the blanks around it
                        // are not part of what the mark stands for.
                        if (file_blank(letter))
                        {
                                if (xargs_replace)
                                {
                                        if (started)
                                                xargs_item_put(letter);

                                        continue;
                                }

                                if (!started)
                                        continue;

                                xargs_item[xargs_item_length] = end;
                                xargs_item_done();
                                line_had_item = true;
                                blank_last = true;
                                xargs_item_length = 0;
                                started = false;
                                continue;
                        }

                        if (letter == '\n')
                        {
                                if (started)
                                {
                                        xargs_item[xargs_item_length] = end;
                                        xargs_item_done();
                                        line_had_item = true;
                                        xargs_item_length = 0;
                                        started = false;
                                }

                                if (xargs_replace)
                                {
                                        blank_last = false;
                                        line_had_item = false;
                                        continue;
                                }
                                if (line_had_item && !blank_last)
                                {
                                        xargs_line_count++;

                                        if (xargs_lines &&
                                            xargs_line_count >= xargs_lines &&
                                            xargs_word_count > xargs_prefix_words)
                                        {
                                                xargs_run();
                                                xargs_reset();
                                        }

                                        line_had_item = false;
                                }

                                blank_last = false;
                                continue;
                        }

                        xargs_item_put(letter);

                        started = true;
                        blank_last = false;
                }

                if (xargs_done || xargs_ended)
                        break;
        }

        if (quote)
        {
                file_fail("xargs: unmatched quote\n", 0);
                return 1;
        }

        if (started && !xargs_done && !xargs_ended)
        {
                xargs_item[xargs_item_length] = end;
                xargs_item_done();
                xargs_item_length = 0;
        }

        if (xargs_replace || xargs_done)
                return xargs_answer;

        if (xargs_word_count > xargs_prefix_words)
                xargs_run();
        else if (!xargs_ran && !xargs_needs_input)
        {
                // An input with nothing in it still runs the command once,
                // with no arguments, unless -r says not to. -I is the one
                // mode where no item means nothing to stand in for.
                xargs_run();
        }

        return xargs_answer;
}
