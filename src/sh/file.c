#include "../library.c"

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
fn file_say(string_address text)
{
        log(text, 0);
}

fn file_fail(address_any data, positive length)
{
        if (length == 0)
                length = string_length(data);

        log_flush();

        system_call_3(syscall(write), stderr, (positive)data, length);
}

fn file_line(string_address text)
{
        log(text, 0);
        log("\n", 1);
}

// Numbers ---------------------------------------------------

positive file_digits(p8 address_to into, positive value)
{
        p8 backwards[24];
        positive length = 0;

        if (value == 0)
                backwards[length++] = '0';

        while (value > 0)
        {
                backwards[length++] = '0' + (p8)(value % 10);
                value /= 10;
        }

        for (positive i = 0; i < length; i++)
                into[i] = backwards[length - 1 - i];

        into[length] = end;

        return length;
}

fn file_number(writer write, positive value)
{
        p8 text[24];
        positive length = file_digits(text, value);

        write(text, length);
}

fn file_number_padded(writer write, positive value, positive width)
{
        p8 text[24];
        positive length = file_digits(text, value);

        for (positive i = length; i < width; i++)
                write(" ", 1);

        write(text, length);
}

fn file_text_padded(writer write, string_address text, positive width)
{
        positive length = string_length(text);

        write(text, length);

        for (positive i = length; i < width; i++)
                write(" ", 1);
}

fn file_text_aligned(writer write, string_address text, positive width)
{
        positive length = string_length(text);

        for (positive i = length; i < width; i++)
                write(" ", 1);

        write(text, length);
}

fn file_octal(writer write, positive value, positive width)
{
        p8 text[24];
        positive length = 0;
        p8 backwards[24];

        if (value == 0)
                backwards[length++] = '0';

        while (value > 0)
        {
                backwards[length++] = '0' + (p8)(value & 7);
                value >>= 3;
        }

        for (positive i = length; i < width; i++)
                write("0", 1);

        for (positive i = 0; i < length; i++)
                text[i] = backwards[length - 1 - i];

        write(text, length);
}

fn file_hexadecimal(writer write, positive value, positive width)
{
        p8 alphabet[17] = "0123456789abcdef";
        p8 text[24];
        positive length = 0;
        p8 backwards[24];

        if (value == 0)
                backwards[length++] = '0';

        while (value > 0)
        {
                backwards[length++] = alphabet[value & 15];
                value >>= 4;
        }

        for (positive i = length; i < width; i++)
                write("0", 1);

        for (positive i = 0; i < length; i++)
                text[i] = backwards[length - 1 - i];

        write(text, length);
}

/*
        The human readable sizes the -h flags print. Powers of 1024, rounded
        up rather than to nearest, and a single decimal only while the value
        is under ten -- which is what makes "1.5K" and "23K" both three or
        four columns wide instead of a ragged edge.
*/
fn file_human(writer write, positive value)
{
        p8 units[8] = "BKMGTPEZ";
        positive divisor = 1;
        positive unit = 0;

        while (value / divisor >= 1024 && unit < 7)
        {
                divisor *= 1024;
                unit++;
        }

        if (unit == 0)
                return file_number(write, value);

        positive whole = (value + divisor - 1) / divisor;

        if (whole >= 10)
        {
                file_number(write, whole);
                write(units + unit, 1);
                return;
        }

        positive quotient = value / divisor;
        positive leftover = value % divisor;
        positive tenths = quotient * 10 + (leftover * 10 + divisor - 1) / divisor;

        file_number(write, tenths / 10);
        write(".", 1);
        file_number(write, tenths % 10);
        write(units + unit, 1);
}

positive file_count(string_address text)
{
        positive value = 0;

        while (string_get(text) >= '0' && string_get(text) <= '9')
        {
                value = value * 10 + (positive)(string_get(text) - '0');
                text++;
        }

        return value;
}

bipolar file_signed(string_address text)
{
        bipolar sign = 1;

        if (string_is(text, '-'))
        {
                sign = -1;
                text++;
        }
        else if (string_is(text, '+'))
                text++;

        return sign * (bipolar)file_count(text);
}

bool file_all_digits(string_address text)
{
        if (string_is(text, end))
                return false;

        while (string_get(text))
        {
                if (string_get(text) < '0' || string_get(text) > '9')
                        return false;

                text++;
        }

        return true;
}

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
                positive value = 0;
                string_address step = specification;

                while (string_get(step) >= '0' && string_get(step) <= '7')
                {
                        value = (value << 3) | (positive)(string_get(step) - '0');
                        step++;
                }

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

// Paths -----------------------------------------------------

positive file_join(p8 address_to into, positive limit, string_address directory,
                   string_address name)
{
        positive length = 0;

        while (string_get(directory + length) && length + 1 < limit)
        {
                into[length] = string_get(directory + length);
                length++;
        }

        // "/" already ends in one, and "//name" is a path the kernel is
        // allowed to treat as its own root on some systems.
        if (length > 0 && into[length - 1] != '/' && length + 1 < limit)
                into[length++] = '/';

        positive i = 0;

        while (string_get(name + i) && length + 1 < limit)
                into[length++] = string_get(name + i++);

        into[length] = end;

        return length;
}

/*
        basename and dirname as POSIX defines them, which is not simply "text
        after the last slash": a trailing slash is not a component, "/" is its
        own answer, and a path with no slash at all has "." for a directory.
*/
fn file_tail(string_address path, p8 address_to into)
{
        positive length = string_length(path);

        // An empty name has an empty last component, not a "." -- POSIX
        // leaves it unspecified and every system tool here prints nothing.
        if (length == 0)
        {
                into[0] = end;
                return;
        }

        while (length > 1 && path[length - 1] == '/')
                length--;

        if (length == 1 && path[0] == '/')
        {
                into[0] = '/';
                into[1] = end;
                return;
        }

        positive start = length;

        while (start > 0 && path[start - 1] != '/')
                start--;

        positive i = 0;

        while (start + i < length && i + 1 < FILE_PATH_MAX)
        {
                into[i] = path[start + i];
                i++;
        }

        into[i] = end;
}

fn file_head(string_address path, p8 address_to into)
{
        positive length = string_length(path);

        while (length > 1 && path[length - 1] == '/')
                length--;

        positive cut = length;

        while (cut > 0 && path[cut - 1] != '/')
                cut--;

        if (cut == 0)
        {
                into[0] = '.';
                into[1] = end;
                return;
        }

        while (cut > 1 && path[cut - 1] == '/')
                cut--;

        if (cut == 0)
        {
                into[0] = '/';
                into[1] = end;
                return;
        }

        for (positive i = 0; i < cut; i++)
                into[i] = path[i];

        into[cut] = end;
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
*/
bool file_real(string_address path, p8 address_to into)
{
        p8 rest[FILE_PATH_MAX];
        p8 link[FILE_PATH_MAX];
        p8 merged[FILE_PATH_MAX];
        positive at = 0;
        positive length = 0;
        positive hops = 0;

        if (string_is(path, end))
                return false;

        if (string_is(path, '/'))
        {
                into[0] = '/';
                length = 1;
        }
        else
        {
                string_address here = working_directory_get();

                while (string_get(here + length) && length + 1 < FILE_PATH_MAX)
                {
                        into[length] = string_get(here + length);
                        length++;
                }

                if (length == 0)
                {
                        into[0] = '/';
                        length = 1;
                }
        }

        into[length] = end;

        string_copy_max(rest, path, FILE_PATH_MAX - 1);
        rest[FILE_PATH_MAX - 1] = end;

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

                for (positive i = 0; i < piece; i++)
                        into[length++] = rest[start + i];

                into[length] = end;

                bipolar seen = system_call_4(syscall(readlinkat), AT_FDCWD,
                                             (positive)into, (positive)link,
                                             FILE_PATH_MAX - 1);

                if (seen <= 0)
                        continue;

                if (++hops > 40)
                        return false;

                link[seen] = end;

                positive fill = 0;

                for (positive i = 0; i < (positive)seen && fill + 1 < FILE_PATH_MAX; i++)
                        merged[fill++] = link[i];

                if (rest[at] && fill + 1 < FILE_PATH_MAX)
                        merged[fill++] = '/';

                for (positive i = at; rest[i] && fill + 1 < FILE_PATH_MAX; i++)
                        merged[fill++] = rest[i];

                merged[fill] = end;

                for (positive i = 0; i <= fill; i++)
                        rest[i] = merged[i];

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

// Reading a small file whole --------------------------------

bipolar file_slurp(string_address path, p8 address_to into, positive limit)
{
        bipolar handle = system_call_3(syscall(openat), AT_FDCWD, (positive)path,
                                       FILE_READ);

        if (handle < 0)
                return handle;

        positive filled = 0;

        while (filled + 1 < limit)
        {
                bipolar taken = system_call_3(syscall(read), handle,
                                              (positive)(into + filled),
                                              limit - 1 - filled);

                if (taken <= 0)
                        break;

                filled += (positive)taken;
        }

        system_call_1(syscall(close), handle);

        into[filled] = end;

        return (bipolar)filled;
}

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

                while (text[at] && text[at] != '\n')
                        at++;

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

                positive value = 0;
                bool numeric = value_stop > value_start;

                for (positive i = value_start; i < value_stop; i++)
                {
                        if (text[i] < '0' || text[i] > '9')
                        {
                                numeric = false;
                                break;
                        }

                        value = value * 10 + (positive)(text[i] - '0');
                }

                if (!numeric || value != wanted)
                        continue;

                positive i = 0;

                while (name_start + i < name_stop && i + 1 < limit)
                {
                        into[i] = text[name_start + i];
                        i++;
                }

                into[i] = end;

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

                while (text[at] && text[at] != '\n')
                        at++;

                positive stop = at;

                if (text[at])
                        at++;

                positive name_stop = line;

                while (name_stop < stop && text[name_stop] != ':')
                        name_stop++;

                if (name_stop - line != wanted)
                        continue;

                positive i = 0;

                while (i < wanted && text[line + i] == string_get(name + i))
                        i++;

                if (i != wanted)
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

                positive value = 0;

                for (positive j = value_start; j < value_stop; j++)
                {
                        if (text[j] < '0' || text[j] > '9')
                                return -1;

                        value = value * 10 + (positive)(text[j] - '0');
                }

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
        if (id != file_user_seen)
        {
                file_user_seen = id;
                file_user_seen_known = file_account_name(file_password_text(), id, 2,
                                                         file_user_seen_name,
                                                         FILE_NAME_MAX);
        }

        if (!file_user_seen_known)
                return false;

        string_copy_max(into, file_user_seen_name, limit - 1);
        into[limit - 1] = end;

        return true;
}

bool file_group_name(positive id, p8 address_to into, positive limit)
{
        if (id != file_group_seen)
        {
                file_group_seen = id;
                file_group_seen_known = file_account_name(file_group_text(), id, 2,
                                                          file_group_seen_name,
                                                          FILE_NAME_MAX);
        }

        if (!file_group_seen_known)
                return false;

        string_copy_max(into, file_group_seen_name, limit - 1);
        into[limit - 1] = end;

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

        pair[0] = '0' + (p8)((value / 10) % 10);
        pair[1] = '0' + (p8)(value % 10);

        write(pair, 2);
}

fn file_stamp(writer write, b64 seconds, positive nanoseconds)
{
        b64 year;
        positive month, day, hour, minute, second;

        file_split_moment(seconds, address_of year, address_of month, address_of day,
                          address_of hour, address_of minute, address_of second);

        file_number(write, (positive)year);
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

        p8 fraction[10];
        positive scale = 100000000;

        for (positive i = 0; i < 9; i++)
        {
                fraction[i] = '0' + (p8)((nanoseconds / scale) % 10);
                scale /= 10;
        }

        write(fraction, 9);
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
        file_number_padded(write, day, 2);
        write(" ", 1);

        bool recent = seconds <= now + 3600 && seconds > now - 15778476;

        if (!recent)
                return file_number_padded(write, (positive)year, 5);

        file_two(write, hour);
        write(":", 1);
        file_two(write, minute);
}

// Patterns --------------------------------------------------

/*
        The shell's glob, for find -name and nothing else. Written as a walk
        with one remembered star rather than as recursion, so a pattern of
        many stars against a long name cannot go exponential.
*/
bool file_match(string_address pattern, string_address text)
{
        positive p = 0;
        positive t = 0;
        bipolar star = -1;
        positive resume = 0;

        while (text[t])
        {
                if (pattern[p] == '?' || (pattern[p] && pattern[p] == text[t]))
                {
                        p++;
                        t++;
                        continue;
                }

                if (pattern[p] == '[')
                {
                        positive scan = p + 1;
                        bool negated = false;
                        bool hit = false;

                        if (pattern[scan] == '!' || pattern[scan] == '^')
                        {
                                negated = true;
                                scan++;
                        }

                        bool first = true;

                        while (pattern[scan] && (pattern[scan] != ']' || first))
                        {
                                first = false;

                                if (pattern[scan + 1] == '-' && pattern[scan + 2] &&
                                    pattern[scan + 2] != ']')
                                {
                                        if (text[t] >= pattern[scan] && text[t] <= pattern[scan + 2])
                                                hit = true;

                                        scan += 3;
                                        continue;
                                }

                                if (pattern[scan] == text[t])
                                        hit = true;

                                scan++;
                        }

                        if (pattern[scan] == ']' && hit != negated)
                        {
                                p = scan + 1;
                                t++;
                                continue;
                        }
                }

                if (pattern[p] == '*')
                {
                        star = (bipolar)p;
                        p++;
                        resume = t;
                        continue;
                }

                if (star >= 0)
                {
                        p = (positive)star + 1;
                        resume++;
                        t = resume;
                        continue;
                }

                return false;
        }

        while (pattern[p] == '*')
                p++;

        return pattern[p] == end;
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

/*
        The leading option words, and only the letters the caller names. A
        word with anything unrecognised in it is left alone as an operand
        rather than swallowed, so a mistyped flag is complained about by the
        program instead of vanishing.
*/
positive file_take_options(string_address allowed, positive address_to first)
{
        positive flags = 0;
        positive index = 1;
        positive count = (positive)program_argument_count();

        while (index < count)
        {
                string_address argument = program_argument((b32)index);

                if (string_is(argument, '-') && string_is(argument + 1, '-') &&
                    string_is(argument + 2, end))
                {
                        index++;
                        break;
                }

                if (!string_is(argument, '-') || string_is(argument + 1, end))
                        break;

                string_address letter = argument + 1;
                positive taken = 0;

                while (string_get(letter))
                {
                        if (!string_first_of(allowed, string_get(letter)))
                                break;

                        taken |= (positive)1 << file_letter_bit(string_get(letter));
                        letter++;
                }

                if (string_get(letter))
                        break;

                flags |= taken;
                index++;
        }

        address_to first = index;

        return flags;
}

fn file_complain(string_address program, string_address message, string_address subject)
{
        string_format(file_fail, "%s: %s: %s\n", program, subject, message);
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
                bipolar taken = system_call_3(syscall(read), in, (positive)file_transfer,
                                              sizeof(file_transfer));

                if (taken < 0)
                {
                        complete = false;
                        break;
                }

                if (taken == 0)
                        break;

                positive written = 0;

                while (written < (positive)taken)
                {
                        bipolar step = system_call_3(syscall(write), out,
                                                     (positive)(file_transfer + written),
                                                     (positive)taken - written);

                        if (step <= 0)
                        {
                                complete = false;
                                break;
                        }

                        written += (positive)step;
                }

                if (!complete)
                        break;
        }

        system_call_1(syscall(close), in);
        system_call_1(syscall(close), out);

        return complete;
}

bool file_make_parents(string_address path, positive mode)
{
        p8 work[FILE_PATH_MAX];
        positive length = 0;

        while (string_get(path + length) && length + 1 < FILE_PATH_MAX)
        {
                work[length] = string_get(path + length);
                length++;
        }

        work[length] = end;

        for (positive i = 1; i < length; i++)
        {
                if (work[i] != '/')
                        continue;

                work[i] = end;

                bipolar made = system_call_3(syscall(mkdirat), AT_FDCWD,
                                             (positive)work, mode);

                if (made < 0 && made != -ERROR_EXISTS)
                {
                        work[i] = '/';
                        return false;
                }

                work[i] = '/';
        }

        bipolar made = system_call_3(syscall(mkdirat), AT_FDCWD, (positive)work, mode);

        return made == 0 || made == -ERROR_EXISTS;
}
