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

// What -z asks for, in the tools that have it: the same answer ended with a
// NUL, so that a name with a newline in it survives being read back.
fn file_written(string_address text, bool zero)
{
        log(text, 0);
        log(zero ? "\0" : "\n", 1);
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

bool file_real(string_address path, p8 address_to into)
{
        return file_resolve(path, into, true);
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

        file_take_options above leaves a word it does not know as an operand,
        which is what the tools written against it want. GNU's stop instead,
        and the difference is not academic: realpath -E used to print the
        resolved name of a file called -E and exit as though that had been the
        question.

        Letters named in `valued` take an argument -- the rest of the word, or
        the word after it -- kept under the bit that letter sets, so a tool
        asks for it by letter the way it asks for everything else.
*/
typedef struct
{
        string_address program;
        string_address allowed;
        string_address valued;
        const file_long address_to longs;

        // seq is the one tool here where -4 is a number and not a flag.
        bool numbers;

        positive flags;
        positive first;
        string_address value[FILE_LETTERS];
} file_taking;

static string_address file_option_value(file_taking address_to taking, p8 letter)
{
        return taking->value[file_letter_bit(letter)];
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
                positive same = 0;

                while (same < length && string_get(spelling + same) &&
                       string_get(spelling + same) == string_get(name + same))
                        same++;

                // The whole word or nothing: GNU shortens a long option to
                // any unambiguous prefix, which is a convenience for a person
                // typing and a trap for a script that outlives the flag.
                if (same == length && string_is(spelling + same, end))
                        return taking->longs[i].letter;
        }

        return 0;
}

static bool file_take(file_taking address_to taking)
{
        positive count = (positive)program_argument_count();
        positive index = 1;

        taking->flags = 0;

        for (positive i = 0; i < FILE_LETTERS; i++)
                taking->value[i] = null;

        while (index < count)
        {
                string_address word = program_argument((b32)index);

                if (!string_is(word, '-') || string_is(word + 1, end))
                        break;

                if (string_is(word + 1, '-') && string_is(word + 2, end))
                {
                        index++;
                        break;
                }

                if (taking->numbers && !string_is(word + 1, '-') &&
                    ((string_get(word + 1) >= '0' && string_get(word + 1) <= '9') ||
                     string_is(word + 1, '.')))
                        break;

                index++;

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

                        if (!string_first_of(taking->valued, letter))
                                continue;

                        if (mark)
                                taking->value[bit] = mark + 1;
                        else if (index < count)
                                taking->value[bit] = program_argument((b32)index++);
                        else
                                return file_option_needs(taking, word);

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

                        if (!string_first_of(taking->valued, string_get(letter)))
                                continue;

                        // -s.txt and -s .txt are the same option given the
                        // same way; either the rest of the word is the
                        // argument or the next word is.
                        if (string_get(letter + 1))
                                taking->value[bit] = letter + 1;
                        else if (index < count)
                                taking->value[bit] = program_argument((b32)index++);
                        else
                                return file_option_needs(taking, named);

                        break;
                }
        }

        taking->first = index;

        return true;
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
static bool ls_headings;

static b32 ls_status;
static bool ls_written;
static b64 ls_now;

static positive ls_keep(string_address name)
{
        positive length = string_length(name);

        if (ls_used + length + 1 > LS_ARENA)
                return 0;

        positive at = ls_used;

        for (positive i = 0; i <= length; i++)
                ls_arena[at + i] = string_get(name + i);

        ls_used += length + 1;

        return at;
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

        string_address a = ls_arena + left->name;
        string_address b = ls_arena + right->name;

        while (string_get(a) && string_get(a) == string_get(b))
        {
                a++;
                b++;
        }

        if (string_get(a) == string_get(b))
                return 0;

        return string_get(a) < string_get(b) ? -1 : 1;
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
                return file_human(log, value);

        file_number(log, value);
}

static positive ls_width_of(p64 value)
{
        p8 text[24];

        return file_digits(text, value);
}

static positive ls_human_width(p64 value)
{
        if (!ls_human)
                return ls_width_of(value);

        positive divisor = 1;
        positive unit = 0;

        while (value / divisor >= 1024 && unit < 7)
        {
                divisor *= 1024;
                unit++;
        }

        if (unit == 0)
                return ls_width_of(value);

        positive whole = (value + divisor - 1) / divisor;

        if (whole >= 10)
                return ls_width_of(whole) + 1;

        return 3 + 1;
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

        file_digits(into, id);
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

                if (ls_human_width(entry->size) > size_width)
                        size_width = ls_human_width(entry->size);

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
                        file_human(log, blocks * 1024);
                else
                        file_number(log, blocks);

                log("\n", 1);
        }

        for (positive k = 0; k < ls_count; k++)
        {
                positive i = ls_reversed ? ls_count - 1 - k : k;
                ls_entry address_to entry = address_of ls_entries[i];
                string_address name = ls_arena + entry->name;

                if (ls_inode)
                {
                        file_number_padded(log, entry->inode, inode_width);
                        log(" ", 1);
                }

                if (ls_long)
                {
                        p8 letters[12];
                        p8 who[FILE_NAME_MAX];

                        file_mode_letters(letters, entry->mode);
                        log(letters, 10);
                        log(" ", 1);
                        file_number_padded(log, entry->links, link_width);
                        log(" ", 1);

                        ls_owner_text(entry->owner, false, who);
                        file_text_padded(log, who, owner_width);
                        log(" ", 1);

                        ls_owner_text(entry->group, true, who);
                        file_text_padded(log, who, group_width);
                        log(" ", 1);

                        for (positive pad = ls_human_width(entry->size); pad < size_width; pad++)
                                log(" ", 1);

                        ls_size_field(entry->size);
                        log(" ", 1);
                        file_stamp_short(log, entry->modified, ls_now);
                        log(" ", 1);
                }

                log(name, 0);

                if (ls_long && (entry->mode & MODE_FORMAT) == MODE_LINK)
                {
                        p8 where[FILE_PATH_MAX];
                        p8 full[FILE_PATH_MAX];

                        if (directory)
                                file_join(full, FILE_PATH_MAX, directory, name);
                        else
                                string_copy_max(full, name, FILE_PATH_MAX - 1);

                        if (file_link_text(full, where, FILE_PATH_MAX) >= 0)
                        {
                                log(" -> ", 0);
                                log(where, 0);
                        }
                }

                log("\n", 1);
        }
}

static fn ls_add(bipolar directory, string_address path, string_address shown)
{
        if (ls_count >= LS_MAX_ENTRIES)
                return;

        file_facts facts;
        ls_entry address_to entry = address_of ls_entries[ls_count];

        memory_fill(entry, 0, sizeof(ls_entry));
        entry->name = ls_keep(shown);

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
                        break;

                for (positive j = 0; j <= length; j++)
                        keep[kept + j] = string_get(name + j);

                kept += length + 1;
                found++;
        }

        positive at = 0;

        for (positive i = 0; i < found; i++)
        {
                p8 below[FILE_PATH_MAX];

                file_join(below, FILE_PATH_MAX, path, keep + at);
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

                ls_add(walk.handle, entry->d_name, entry->d_name);
        }

        file_walk_close(address_of walk);

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

static b32 file_ls()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "laARtShr1dinF", address_of first);

        ls_now = file_now();

        ls_long = (flags & FILE_FLAG('l')) != 0 || (flags & FILE_FLAG('n')) != 0;
        ls_hidden = (flags & FILE_FLAG('a')) != 0;
        ls_almost = (flags & FILE_FLAG('A')) != 0;
        ls_recursive = (flags & FILE_FLAG('R')) != 0;
        ls_by_time = (flags & FILE_FLAG('t')) != 0;
        ls_by_size = (flags & FILE_FLAG('S')) != 0;
        ls_human = (flags & FILE_FLAG('h')) != 0;
        ls_reversed = (flags & FILE_FLAG('r')) != 0;
        ls_inode = (flags & FILE_FLAG('i')) != 0;
        ls_numeric = (flags & FILE_FLAG('n')) != 0;
        ls_as_itself = (flags & FILE_FLAG('d')) != 0;

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
                        ls_add(AT_FDCWD, program_argument((b32)i), program_argument((b32)i));

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

                ls_add(AT_FDCWD, path, path);
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

// find ------------------------------------------------------------
/*
        find [PATH...] [-maxdepth N] [-mindepth N] [-name PATTERN]
             [-type C] [-size N] [-empty] [-print]

        Every test given has to hold, which is the and that find's expression
        language spells by juxtaposition. There is no -exec: running a command
        is the shell's job, and everything here can be piped into one.
*/
#define FIND_TESTS_MAX 32

typedef struct
{
        p8 kind;
        string_address text;
        b64 number;
        p8 unit;
        p8 comparison;
} find_test;

static find_test find_tests[FIND_TESTS_MAX];
static positive find_have;
static positive find_maximum = FILE_MAX_DEPTH;
static positive find_minimum;
static b32 find_status;

static bool find_size_holds(find_test address_to test, file_facts address_to facts)
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

static bool find_holds(string_address path, string_address name, file_facts address_to facts,
                       positive depth)
{
        if (depth < find_minimum)
                return false;

        for (positive i = 0; i < find_have; i++)
        {
                find_test address_to test = address_of find_tests[i];

                if (test->kind == 'n' && !file_match(test->text, name))
                        return false;

                if (test->kind == 'p' && !file_match(test->text, path))
                        return false;

                if (test->kind == 't' && !find_type_holds((p8)test->number, facts->mode))
                        return false;

                if (test->kind == 's' && !find_size_holds(test, facts))
                        return false;

                if (test->kind == 'e' && !find_empty(path, facts))
                        return false;

                if (test->kind == 'm' && (facts->mode & 07777) != (positive)test->number)
                        return false;
        }

        return true;
}

static fn find_walk(string_address path, string_address name, positive depth)
{
        file_facts facts;

        if (!file_look_link(path, address_of facts))
        {
                string_format(file_fail, "find: '%s': No such file or directory\n", path);
                find_status = 1;
                return;
        }

        if (find_holds(path, name, address_of facts, depth))
                file_line(path);

        if ((facts.mode & MODE_FORMAT) != MODE_DIRECTORY || depth >= find_maximum)
                return;

        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, path))
        {
                string_format(file_fail, "find: '%s': Permission denied\n", path);
                find_status = 1;
                return;
        }

        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                p8 below[FILE_PATH_MAX];
                p8 held[FILE_NAME_MAX];

                string_copy_max(held, entry->d_name, FILE_NAME_MAX - 1);
                held[FILE_NAME_MAX - 1] = end;

                file_join(below, FILE_PATH_MAX, path, held);

                find_walk(below, held, depth + 1);
        }

        file_walk_close(address_of walk);
}

static b32 file_find()
{
        positive count = (positive)program_argument_count();
        positive index = 1;
        positive roots_first = 1;
        positive roots_last = 1;

        while (index < count && !string_is(program_argument((b32)index), '-'))
                index++;

        roots_last = index;

        while (index < count)
        {
                string_address word = program_argument((b32)index);
                string_address value = index + 1 < count ? program_argument((b32)(index + 1)) : null;

                if (string_compare(word, (string_address) "-print") == 0 ||
                    string_compare(word, (string_address) "-print0") == 0)
                {
                        index++;
                        continue;
                }

                if (string_compare(word, (string_address) "-empty") == 0)
                {
                        if (find_have < FIND_TESTS_MAX)
                                find_tests[find_have++].kind = 'e';

                        index++;
                        continue;
                }

                if (!value)
                {
                        string_format(file_fail, "find: %s needs a value\n", word);
                        return 1;
                }

                if (string_compare(word, (string_address) "-maxdepth") == 0)
                {
                        find_maximum = file_count(value);
                        index += 2;
                        continue;
                }

                if (string_compare(word, (string_address) "-mindepth") == 0)
                {
                        find_minimum = file_count(value);
                        index += 2;
                        continue;
                }

                if (find_have >= FIND_TESTS_MAX)
                {
                        file_fail("find: too many tests\n", 0);
                        return 1;
                }

                find_test address_to test = address_of find_tests[find_have];

                if (string_compare(word, (string_address) "-name") == 0)
                {
                        test->kind = 'n';
                        test->text = value;
                }
                else if (string_compare(word, (string_address) "-path") == 0 ||
                         string_compare(word, (string_address) "-wholename") == 0)
                {
                        test->kind = 'p';
                        test->text = value;
                }
                else if (string_compare(word, (string_address) "-type") == 0)
                {
                        test->kind = 't';
                        test->number = string_get(value);
                }
                else if (string_compare(word, (string_address) "-perm") == 0)
                {
                        positive mode = 0;

                        if (!file_mode_of(value, 0, false, address_of mode))
                        {
                                string_format(file_fail, "find: invalid mode %s\n", value);
                                return 1;
                        }

                        test->kind = 'm';
                        test->number = (b64)mode;
                }
                else if (string_compare(word, (string_address) "-size") == 0)
                {
                        string_address step = value;

                        test->kind = 's';
                        test->comparison = ' ';

                        if (string_is(step, '+') || string_is(step, '-'))
                        {
                                test->comparison = string_get(step);
                                step++;
                        }

                        test->number = (b64)file_count(step);

                        while (string_get(step) >= '0' && string_get(step) <= '9')
                                step++;

                        test->unit = string_get(step) ? string_get(step) : 'b';
                }
                else
                {
                        string_format(file_fail, "find: unknown test: %s\n", word);
                        return 1;
                }

                find_have++;
                index += 2;
        }

        if (roots_last == roots_first)
        {
                find_walk((string_address) ".", (string_address) ".", 0);
                log_flush();
                return find_status;
        }

        for (positive i = roots_first; i < roots_last; i++)
        {
                string_address root = program_argument((b32)i);
                p8 name[FILE_PATH_MAX];

                file_tail(root, name);
                find_walk(root, name, 0);
        }

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
static b32 stat_status;

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
                return file_number(log, facts->size);

        case 'b':
                return file_number(log, facts->blocks);

        case 'B':
                return file_number(log, 512);

        case 'a':
                return file_octal(log, facts->mode & 07777, 1);

        case 'A':
                file_mode_letters(text, facts->mode);
                return log(text, 10);

        case 'f':
                return file_hexadecimal(log, facts->mode, 1);

        case 'F':
                return log(file_kind_name(facts->mode), 0);

        case 'h':
                return file_number(log, facts->hard_links);

        case 'i':
                return file_number(log, facts->inode);

        case 'u':
                return file_number(log, facts->owner);

        case 'g':
                return file_number(log, facts->group);

        case 'U':
                if (file_user_name(facts->owner, text, FILE_NAME_MAX))
                        return log(text, 0);

                return file_number(log, facts->owner);

        case 'G':
                if (file_group_name(facts->group, text, FILE_NAME_MAX))
                        return log(text, 0);

                return file_number(log, facts->group);

        case 'o':
                return file_number(log, facts->blocksize);

        case 'd':
                return file_number(log, facts->device_major * 256 + facts->device_minor);

        case 't':
                return file_hexadecimal(log, facts->rdev_major, 1);

        case 'T':
                return file_hexadecimal(log, facts->rdev_minor, 1);

        case 'X':
                return file_number(log, (positive)facts->accessed.seconds);

        case 'Y':
                return file_number(log, (positive)facts->modified.seconds);

        case 'Z':
                return file_number(log, (positive)facts->changed.seconds);

        case 'W':
                return file_number(log, (facts->mask & STATX_BIRTH)
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
                if (string_is(step, '%') && string_get(step + 1))
                {
                        stat_one_specifier(string_get(step + 1), path, facts);
                        step += 2;
                        continue;
                }

                log(step, 1);
                step++;
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
        file_digits(text, facts->size);
        file_text_padded(log, text, 10);
        log("\tBlocks: ", 0);
        file_digits(text, facts->blocks);
        file_text_padded(log, text, 10);
        log(" IO Block: ", 0);
        file_digits(text, facts->blocksize);
        file_text_padded(log, text, 6);
        log(" ", 1);
        log(file_kind_name(facts->mode), 0);

        log("\nDevice: ", 0);
        file_number(log, facts->device_major);
        log(",", 1);
        file_number(log, facts->device_minor);
        log("\tInode: ", 0);
        file_digits(text, facts->inode);
        file_text_padded(log, text, 10);
        log("  Links: ", 0);
        file_number(log, facts->hard_links);

        log("\nAccess: (", 0);
        file_octal(log, facts->mode & 07777, 4);
        log("/", 1);
        file_mode_letters(text, facts->mode);
        log(text, 10);
        log(")  Uid: (", 0);
        file_number_padded(log, facts->owner, 5);
        log("/", 1);

        if (!file_user_name(facts->owner, text, FILE_NAME_MAX))
                file_digits(text, facts->owner);

        file_text_aligned(log, text, 8);

        log(")   Gid: (", 0);
        file_number_padded(log, facts->group, 5);
        log("/", 1);

        if (!file_group_name(facts->group, text, FILE_NAME_MAX))
                file_digits(text, facts->group);

        file_text_aligned(log, text, 8);

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

static b32 file_stat()
{
        positive count = (positive)program_argument_count();
        positive index = 1;
        string_address format = null;

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

                if (string_is(argument + 1, 'L') && string_is(argument + 2, end))
                {
                        stat_follow = true;
                        index++;
                        continue;
                }

                if ((string_is(argument + 1, 'c') || string_is(argument + 1, 'f')) &&
                    string_is(argument + 2, end) && index + 1 < count)
                {
                        format = program_argument((b32)(index + 1));
                        index += 2;
                        continue;
                }

                if (string_is(argument + 1, 'c') && string_get(argument + 2))
                {
                        format = argument + 2;
                        index++;
                        continue;
                }

                string_format(file_fail, "stat: unknown option: %s\n", argument);
                return 1;
        }

        if (index >= count)
        {
                file_fail("stat: missing operand\n", 0);
                return 1;
        }

        while (index < count)
        {
                string_address path = program_argument((b32)index++);
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
        du [-a] [-s] [-h] [-k] [-b] [-c] [PATH...]

        What a file costs on the disk, not how long it is: the kernel's block
        count, which is what makes a sparse file cheap and a tiny file cost a
        whole block. -b is the other question, and asks for the length.
*/
static bool du_all;
static bool du_summary;
static bool du_human;
static bool du_apparent;
static bool du_total;
static b32 du_status;
static p64 du_grand;

static fn du_report(p64 blocks, string_address path)
{
        if (du_human)
                file_human(log, du_apparent ? blocks : blocks * 512);
        else if (du_apparent)
                file_number(log, blocks);
        else
                file_number(log, blocks / 2);

        log("\t", 1);
        log(path, 0);
        log("\n", 1);
}

// Returns what the tree costs, and prints the parts of it that were asked for
// on the way back up, which is the order du has always reported in.
static p64 du_walk(string_address path, positive depth, bool named)
{
        file_facts facts;

        if (!file_look_link(path, address_of facts))
        {
                string_format(file_fail, "du: cannot access '%s': No such file or directory\n",
                              path);
                du_status = 1;
                return 0;
        }

        p64 mine = du_apparent ? facts.size : facts.blocks;

        // --apparent-size is asking how much was written, and nothing was
        // written into the directory itself; only what is under it counts.
        if (du_apparent && (facts.mode & MODE_FORMAT) == MODE_DIRECTORY)
                mine = 0;

        if ((facts.mode & MODE_FORMAT) != MODE_DIRECTORY)
        {
                if (du_all || named)
                        du_report(mine, path);

                return mine;
        }

        p64 total = mine;

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

                                p8 below[FILE_PATH_MAX];

                                file_join(below, FILE_PATH_MAX, path, entry->d_name);
                                total += du_walk(below, depth - 1, false);
                        }

                        file_walk_close(address_of walk);
                }
                else
                {
                        string_format(file_fail, "du: cannot read directory '%s'\n", path);
                        du_status = 1;
                }
        }

        if (named || !du_summary)
                du_report(total, path);

        return total;
}

static b32 file_du()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "ashkbcSx", address_of first);

        du_all = (flags & FILE_FLAG('a')) != 0;
        du_summary = (flags & FILE_FLAG('s')) != 0;
        du_human = (flags & FILE_FLAG('h')) != 0;
        du_apparent = (flags & FILE_FLAG('b')) != 0;
        du_total = (flags & FILE_FLAG('c')) != 0;

        if (first >= count)
        {
                du_grand += du_walk((string_address) ".", FILE_MAX_DEPTH, true);
        }
        else
        {
                while (first < count)
                        du_grand += du_walk(program_argument((b32)first++),
                                            FILE_MAX_DEPTH, true);
        }

        if (du_total)
                du_report(du_grand, (string_address) "total");

        log_flush();

        return du_status;
}

// df ------------------------------------------------------------
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

static b32 file_df()
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

// chmod ------------------------------------------------------------
// chmod [-R] MODE FILE..., with MODE octal or symbolic.
static string_address chmod_specification;
static b32 chmod_status;

static bool chmod_one(bipolar directory, string_address name, string_address shown)
{
        file_facts facts;

        // Following the link, because Linux has no mode on a symlink of its
        // own to change and chmod has always meant the thing pointed at.
        if (!file_look(directory, name, 0, address_of facts))
        {
                file_facts itself;

                // A link with nothing at the end of it is not an error worth
                // a word, but there is nothing to change either.
                if (file_look(directory, name, AT_SYMLINK_NOFOLLOW, address_of itself))
                        return true;

                string_format(file_fail, "chmod: cannot access '%s': No such file or directory\n",
                              shown);
                chmod_status = 1;
                return false;
        }

        positive wanted = 0;

        if (!file_mode_of(chmod_specification, facts.mode,
                          (facts.mode & MODE_FORMAT) == MODE_DIRECTORY, address_of wanted))
        {
                string_format(file_fail, "chmod: invalid mode: %s\n", chmod_specification);
                chmod_status = 1;
                return false;
        }

        bipolar done = system_call_4(syscall(fchmodat), directory, (positive)name, wanted, 0);

        if (done < 0)
        {
                string_format(file_fail, "chmod: changing permissions of '%s': %s\n",
                              shown, file_reason(done));
                chmod_status = 1;
                return false;
        }

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

                file_join(below, FILE_PATH_MAX, shown, entry->d_name);
                chmod_walk(walk.handle, entry->d_name, below, depth - 1);
        }

        file_walk_close(address_of walk);
}

static b32 file_chmod()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "Rfvc", address_of first);

        if (first + 1 >= count)
        {
                file_fail("chmod: missing operand\n", 0);
                return 1;
        }

        chmod_specification = program_argument((b32)first++);

        while (first < count)
        {
                string_address path = program_argument((b32)first++);

                if (flags & FILE_FLAG('R'))
                        chmod_walk(AT_FDCWD, path, path, FILE_MAX_DEPTH);
                else
                        chmod_one(AT_FDCWD, path, path);
        }

        log_flush();

        return chmod_status;
}

// chown ------------------------------------------------------------
// chown [-R] [-h] USER[:GROUP] FILE..., and the same program answers to a
// USER of nothing at all so that ":group" changes only the group.
static bipolar chown_user = -1;
static bipolar chown_group = -1;
static b32 chown_status;
static positive chown_flags;

static fn chown_one(bipolar directory, string_address name, string_address shown)
{
        bipolar done = system_call_5(syscall(fchownat), directory, (positive)name,
                                     (positive)chown_user, (positive)chown_group,
                                     (chown_flags & FILE_FLAG('h')) ? AT_SYMLINK_NOFOLLOW : 0);

        if (done < 0)
        {
                string_format(file_fail, "chown: changing ownership of '%s': %s\n",
                              shown, file_reason(done));
                chown_status = 1;
        }
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

                file_join(below, FILE_PATH_MAX, shown, entry->d_name);
                chown_walk(walk.handle, entry->d_name, below, depth - 1);
        }

        file_walk_close(address_of walk);
}

static b32 file_chown()
{
        positive first = 0;
        positive count = (positive)program_argument_count();

        chown_flags = file_take_options((string_address) "Rhfvc", address_of first);

        if (first + 1 >= count)
        {
                file_fail("chown: missing operand\n", 0);
                return 1;
        }

        string_address who = program_argument((b32)first++);
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
                chown_user = file_all_digits(user) ? (bipolar)file_count(user)
                                                   : file_user_id(user);

                if (chown_user < 0)
                {
                        string_format(file_fail, "chown: invalid user: %s\n", who);
                        return 1;
                }
        }

        if (group && string_get(group))
        {
                chown_group = file_all_digits(group) ? (bipolar)file_count(group)
                                                     : file_group_id(group);

                if (chown_group < 0)
                {
                        string_format(file_fail, "chown: invalid group: %s\n", who);
                        return 1;
                }
        }

        while (first < count)
        {
                string_address path = program_argument((b32)first++);

                if (chown_flags & FILE_FLAG('R'))
                        chown_walk(AT_FDCWD, path, path, FILE_MAX_DEPTH);
                else
                        chown_one(AT_FDCWD, path, path);
        }

        log_flush();

        return chown_status;
}

// ln ------------------------------------------------------------
// ln [-s] [-f] TARGET [NAME], and ln [-s] [-f] TARGET... DIRECTORY.
static bool ln_make(string_address target, string_address name, bool symbolic, bool force)
{
        if (force)
                system_call_3(syscall(unlinkat), AT_FDCWD, (positive)name, 0);

        bipolar done;

        if (symbolic)
                done = system_call_3(syscall(symlinkat), (positive)target, AT_FDCWD,
                                     (positive)name);
        else
                done = system_call_5(syscall(linkat), AT_FDCWD, (positive)target,
                                     AT_FDCWD, (positive)name, 0);

        if (done < 0)
        {
                string_format(file_fail, "ln: failed to create link '%s': %s\n",
                              name, file_reason(done));
                return false;
        }

        return true;
}

static b32 file_ln()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "sfnvTP", address_of first);

        bool symbolic = (flags & FILE_FLAG('s')) != 0;
        bool force = (flags & FILE_FLAG('f')) != 0;

        if (first >= count)
        {
                file_fail("ln: missing operand\n", 0);
                return 1;
        }

        positive given = count - first;

        if (given == 1)
        {
                // One operand links into the working directory under the
                // target's own last component.
                string_address target = program_argument((b32)first);
                p8 name[FILE_PATH_MAX];

                file_tail(target, name);

                return ln_make(target, name, symbolic, force) ? 0 : 1;
        }

        string_address last = program_argument((b32)(count - 1));

        if (given == 2 && !file_is_directory_through(last))
        {
                return ln_make(program_argument((b32)first), last, symbolic, force) ? 0 : 1;
        }

        if (!file_is_directory_through(last))
        {
                string_format(file_fail, "ln: target '%s' is not a directory\n", last);
                return 1;
        }

        b32 status = 0;

        while (first < count - 1)
        {
                string_address target = program_argument((b32)first++);
                p8 tail[FILE_PATH_MAX];
                p8 name[FILE_PATH_MAX];

                file_tail(target, tail);
                file_join(name, FILE_PATH_MAX, last, tail);

                if (!ln_make(target, name, symbolic, force))
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

static b32 file_readlink()
{
        file_taking taking = {
            .program = (string_address) "readlink",
            .allowed = (string_address) "fneqsvmz",
            .valued = (string_address) "",
            .longs = readlink_longs,
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

        bool resolve = (flags & (FILE_FLAG('f') | FILE_FLAG('e') | FILE_FLAG('m'))) != 0;
        bool no_newline = (flags & FILE_FLAG('n')) != 0;
        bool quiet = (flags & (FILE_FLAG('q') | FILE_FLAG('s'))) != 0;
        bool zero = (flags & FILE_FLAG('z')) != 0;
        b32 status = 0;

        while (first < count)
        {
                string_address path = program_argument((b32)first++);
                p8 answer[FILE_PATH_MAX];

                if (resolve)
                {
                        if (!file_real(path, answer))
                        {
                                status = 1;
                                continue;
                        }

                        p8 above[FILE_PATH_MAX];

                        file_head(answer, above);

                        // -f wants the parent to be real, -e wants the whole
                        // path to be, -m wants neither.
                        if (!(flags & FILE_FLAG('m')) && !file_is_directory_through(above))
                        {
                                status = 1;
                                continue;
                        }

                        if ((flags & FILE_FLAG('e')) && !file_exists(AT_FDCWD, answer))
                        {
                                status = 1;
                                continue;
                        }
                }
                else if (file_link_text(path, answer, FILE_PATH_MAX) < 0)
                {
                        if (!quiet)
                                string_format(file_fail, "readlink: %s: Invalid argument\n", path);

                        status = 1;
                        continue;
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

        file_tail(name, answer);

        if (suffix)
        {
                positive length = string_length(answer);
                positive cut = string_length(suffix);

                // A name that is nothing but its suffix keeps it: stripping
                // would leave an empty line where a name was asked for.
                if (cut > 0 && cut < length)
                {
                        positive i = 0;

                        while (i < cut && answer[length - cut + i] == string_get(suffix + i))
                                i++;

                        if (i == cut)
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

                file_head(program_argument((b32)first++), answer);
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

// Whether one canonical path is the other or lies under it. Whole components
// only: /usr/lib is not under /usr/li.
static bool realpath_under(string_address directory, string_address path)
{
        positive length = string_length(directory);

        if (length > 0 && string_is(directory + length - 1, '/'))
                length--;

        for (positive i = 0; i < length; i++)
                if (string_get(path + i) != string_get(directory + i))
                        return false;

        return string_is(path + length, end) || string_is(path + length, '/');
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

                for (positive i = 0; string_get(path + mark + i) && length + 1 < FILE_PATH_MAX; i++)
                        into[length++] = string_get(path + mark + i);
        }

        if (!length)
                into[length++] = '.';

        into[length] = end;
}

static b32 file_realpath()
{
        file_taking taking = {
            .program = (string_address) "realpath",
            .allowed = (string_address) "EeLmPqsz",
            .valued = (string_address) "RB",
            .longs = realpath_longs,
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

        bool allow_missing = (taking.flags & FILE_FLAG('m')) != 0;
        bool written_name = (taking.flags & FILE_FLAG('s')) != 0;
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

                if (!file_resolve(path, answer, !written_name))
                {
                        if (!quiet)
                                string_format(file_fail, "realpath: %s: Invalid argument\n", path);

                        status = 1;
                        continue;
                }

                file_head(answer, above);

                if (!written_name &&
                    ((!allow_missing && !file_is_directory_through(above)) ||
                     ((taking.flags & FILE_FLAG('e')) && !file_exists(AT_FDCWD, answer))))
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
        positive index = 1;
        bool parents = false;
        positive mode = 0777;
        bool given_mode = false;

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

                if (string_is(argument + 1, 'm') && string_is(argument + 2, end) &&
                    index + 1 < count)
                {
                        if (!file_mode_of(program_argument((b32)(index + 1)), 0777, true,
                                          address_of mode))
                        {
                                file_fail("mkdir: bad mode\n", 0);
                                return 1;
                        }

                        given_mode = true;
                        index += 2;
                        continue;
                }

                if (string_is(argument + 1, 'p') && string_is(argument + 2, end))
                {
                        parents = true;
                        index++;
                        continue;
                }

                string_format(file_fail, "mkdir: unknown option: %s\n", argument);
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
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "p", address_of first);

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

                string_copy_max(parent, path, FILE_PATH_MAX - 1);
                parent[FILE_PATH_MAX - 1] = end;

                while (1)
                {
                        p8 above[FILE_PATH_MAX];

                        file_head(parent, above);

                        if (string_is(above, '.') && string_is(above + 1, end))
                                break;

                        if (string_is(above, '/') && string_is(above + 1, end))
                                break;

                        if (system_call_3(syscall(unlinkat), AT_FDCWD, (positive)above,
                                          AT_REMOVEDIR) < 0)
                                break;

                        string_copy_max(parent, above, FILE_PATH_MAX - 1);
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
        copy itself.
*/
static bool cp_recursive;
static bool cp_preserve;
static b32 cp_status;

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

        system_call_4(syscall(fchmodat), AT_FDCWD, (positive)destination,
                      facts->mode & 07777, 0);
}

static bool cp_one(string_address source, string_address destination, positive depth)
{
        file_facts facts;

        if (!file_look_link(source, address_of facts))
        {
                string_format(file_fail, "cp: cannot stat '%s': No such file or directory\n",
                              source);
                cp_status = 1;
                return false;
        }

        positive kind = facts.mode & MODE_FORMAT;

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
                return true;
        }

        if (kind != MODE_DIRECTORY)
        {
                if (!file_copy_contents(AT_FDCWD, source, AT_FDCWD, destination,
                                        facts.mode & 07777))
                {
                        string_format(file_fail, "cp: cannot copy '%s'\n", source);
                        cp_status = 1;
                        return false;
                }

                // The open above only sets the mode on a file it created, so a
                // destination that was already there keeps whatever it had
                // unless the mode is written afterwards.
                system_call_4(syscall(fchmodat), AT_FDCWD, (positive)destination,
                              facts.mode & 07777, 0);

                cp_keep(destination, address_of facts);
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

        struct linux_dirent64 address_to entry;
        bool complete = true;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                p8 from[FILE_PATH_MAX];
                p8 to[FILE_PATH_MAX];

                file_join(from, FILE_PATH_MAX, source, entry->d_name);
                file_join(to, FILE_PATH_MAX, destination, entry->d_name);

                if (!cp_one(from, to, depth - 1))
                        complete = false;
        }

        file_walk_close(address_of walk);

        cp_keep(destination, address_of facts);

        return complete;
}

static b32 file_cp()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "rRpafdvit", address_of first);

        cp_recursive = (flags & (FILE_FLAG('r') | FILE_FLAG('R') | FILE_FLAG('a'))) != 0;
        cp_preserve = (flags & (FILE_FLAG('p') | FILE_FLAG('a'))) != 0;

        if (first + 1 >= count)
        {
                file_fail("cp: missing operand\n", 0);
                return 1;
        }

        string_address last = program_argument((b32)(count - 1));

        if (count - first == 2 && !file_is_directory_through(last))
        {
                cp_one(program_argument((b32)first), last, FILE_MAX_DEPTH);
                log_flush();
                return cp_status;
        }

        if (!file_is_directory_through(last))
        {
                string_format(file_fail, "cp: target '%s' is not a directory\n", last);
                return 1;
        }

        while (first < count - 1)
        {
                string_address source = program_argument((b32)first++);
                p8 tail[FILE_PATH_MAX];
                p8 destination[FILE_PATH_MAX];

                file_tail(source, tail);
                file_join(destination, FILE_PATH_MAX, last, tail);

                cp_one(source, destination, FILE_MAX_DEPTH);
        }

        log_flush();

        return cp_status;
}

// mv ------------------------------------------------------------
/*
        mv [-f] SOURCE... DESTINATION

        renameat2 rather than renameat, because riscv64 never had renameat and
        this tree builds for it; a flags word of zero is the same operation.
*/
static b32 mv_status;

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

                file_join(from, FILE_PATH_MAX, source, entry->d_name);
                file_join(to, FILE_PATH_MAX, destination, entry->d_name);

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

static fn mv_one(string_address source, string_address destination)
{
        bipolar done = system_call_5(syscall(renameat2), AT_FDCWD, (positive)source,
                                     AT_FDCWD, (positive)destination, 0);

        if (done == 0)
                return;

        if (done == -ERROR_CROSS_DEVICE && mv_across(source, destination, FILE_MAX_DEPTH))
                return;

        string_format(file_fail, "mv: cannot move '%s' to '%s': %s\n", source,
                      destination, file_reason(done));
        mv_status = 1;
}

static b32 file_mv()
{
        positive first = 0;
        positive count = (positive)program_argument_count();

        file_take_options((string_address) "fivnT", address_of first);

        if (first + 1 >= count)
        {
                file_fail("mv: missing operand\n", 0);
                return 1;
        }

        string_address last = program_argument((b32)(count - 1));

        if (count - first == 2 && !file_is_directory_through(last))
        {
                mv_one(program_argument((b32)first), last);
                log_flush();
                return mv_status;
        }

        if (!file_is_directory_through(last))
        {
                string_format(file_fail, "mv: target '%s' is not a directory\n", last);
                return 1;
        }

        while (first < count - 1)
        {
                string_address source = program_argument((b32)first++);
                p8 tail[FILE_PATH_MAX];
                p8 destination[FILE_PATH_MAX];

                file_tail(source, tail);
                file_join(destination, FILE_PATH_MAX, last, tail);

                mv_one(source, destination);
        }

        log_flush();

        return mv_status;
}

// rm ------------------------------------------------------------
// rm [-r] [-f] FILE...
static bool rm_force;
static b32 rm_status;

static bool rm_tree(bipolar directory, string_address name, string_address shown,
                    positive depth);

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

                        file_join(below, FILE_PATH_MAX, shown, entry->d_name);

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
        if (system_call_3(syscall(unlinkat), directory, (positive)name, 0) == 0)
                return true;

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

        if (depth == 0)
        {
                string_format(file_fail, "rm: '%s' is nested too deep\n", shown);
                rm_status = 1;
                return false;
        }

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

        bool complete = rm_contents(inside, shown, depth - 1);

        system_call_1(syscall(close), inside);

        bipolar gone = system_call_3(syscall(unlinkat), directory, (positive)name,
                                     AT_REMOVEDIR);

        if (gone < 0)
        {
                if (!rm_force)
                {
                        string_format(file_fail, "rm: cannot remove '%s': %s\n", shown,
                                      file_reason(gone));
                        rm_status = 1;
                }

                return false;
        }

        return complete;
}

static b32 file_rm()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "rRfivd", address_of first);

        rm_force = (flags & FILE_FLAG('f')) != 0;

        bool recursive = (flags & (FILE_FLAG('r') | FILE_FLAG('R'))) != 0;

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

                if (!file_exists(AT_FDCWD, path))
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

                if (file_is_directory(AT_FDCWD, path) && !recursive)
                {
                        string_format(file_fail, "rm: cannot remove '%s': Is a directory\n",
                                      path);
                        rm_status = 1;
                        continue;
                }

                rm_tree(AT_FDCWD, path, path, FILE_MAX_DEPTH);
        }

        log_flush();

        return rm_status;
}

// touch ------------------------------------------------------------
/*
        touch [-a] [-m] [-c] [-r REFERENCE] [-d SECONDS] FILE...

        -a and -m are what pick which of the two stamps moves; naming neither
        moves both, and naming one leaves the other exactly where it was
        rather than setting it to now, which is what UTIME_OMIT is for.

        -d here takes seconds since the epoch rather than a written date. A
        date parser would be most of a calendar library, and every test that
        wants a repeatable stamp can say it as a number.
*/
static b32 file_touch()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive index = 1;
        bool access = false;
        bool modify = false;
        bool no_create = false;
        bool given = false;
        b64 seconds = 0;
        p32 nanoseconds = 0;

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

                if ((string_is(argument + 1, 'r') || string_is(argument + 1, 'd')) &&
                    string_is(argument + 2, end) && index + 1 < count)
                {
                        string_address value = program_argument((b32)(index + 1));

                        if (string_is(argument + 1, 'r'))
                        {
                                file_facts facts;

                                if (!file_look_at(value, address_of facts))
                                {
                                        string_format(file_fail,
                                                      "touch: failed to get attributes of '%s'\n",
                                                      value);
                                        return 1;
                                }

                                seconds = facts.modified.seconds;
                                nanoseconds = facts.modified.nanoseconds;
                        }
                        else
                        {
                                // @N is how a date is spelled as a number of
                                // seconds, and it is the only spelling read
                                // here; a written date would be a calendar.
                                string_address digits = string_is(value, '@') ? value + 1
                                                                              : value;

                                if (!file_all_digits(string_is(digits, '-') ? digits + 1
                                                                            : digits))
                                {
                                        string_format(file_fail,
                                                      "touch: invalid date format: %s\n",
                                                      value);
                                        return 1;
                                }

                                seconds = file_signed(digits);
                                nanoseconds = 0;
                        }

                        given = true;
                        index += 2;
                        continue;
                }

                string_address letter = argument + 1;
                bool known = true;

                while (string_get(letter) && known)
                {
                        if (string_is(letter, 'a'))
                                access = true;
                        else if (string_is(letter, 'm'))
                                modify = true;
                        else if (string_is(letter, 'c'))
                                no_create = true;
                        else
                                known = false;

                        letter++;
                }

                if (!known)
                {
                        string_format(file_fail, "touch: unknown option: %s\n", argument);
                        return 1;
                }

                index++;
        }

        first = index;

        if (first >= count)
        {
                file_fail("touch: missing operand\n", 0);
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

                if (!file_exists(AT_FDCWD, path))
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
                                             (positive)times, 0);

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
        p64 whole = 0;
        p64 fraction = 0;
        p64 scale = 100000000;
        bool any = false;

        while (string_get(text) >= '0' && string_get(text) <= '9')
        {
                whole = whole * 10 + (p64)(string_get(text) - '0');
                text++;
                any = true;
        }

        if (string_is(text, '.'))
        {
                text++;

                while (string_get(text) >= '0' && string_get(text) <= '9')
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
        p8 text[24];
        positive length;

        if (value < 0)
        {
                length = file_digits(text, (positive)(-value));

                for (positive i = length + 1; i < width; i++)
                        write("0", 1);

                write("-", 1);
                write(text, length);
                return;
        }

        length = file_digits(text, (positive)value);

        for (positive i = length; i < width; i++)
                write("0", 1);

        write(text, length);
}

static positive seq_width(bipolar value)
{
        p8 text[24];
        positive length = file_digits(text, value < 0 ? (positive)(-value) : (positive)value);

        return value < 0 ? length + 1 : length;
}

static b32 file_seq()
{
        positive count = (positive)program_argument_count();
        positive index = 1;
        bool pad = false;
        string_address separator = (string_address) "\n";

        while (index < count)
        {
                string_address argument = program_argument((b32)index);

                if (string_is(argument, '-') && string_is(argument + 1, 'w') &&
                    string_is(argument + 2, end))
                {
                        pad = true;
                        index++;
                        continue;
                }

                if (string_is(argument, '-') && string_is(argument + 1, 's') &&
                    string_is(argument + 2, end) && index + 1 < count)
                {
                        separator = program_argument((b32)(index + 1));
                        index += 2;
                        continue;
                }

                break;
        }

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
                last = file_signed(program_argument((b32)index));
        else if (given == 2)
        {
                first = file_signed(program_argument((b32)index));
                last = file_signed(program_argument((b32)(index + 1)));
        }
        else
        {
                first = file_signed(program_argument((b32)index));
                step = file_signed(program_argument((b32)(index + 1)));
                last = file_signed(program_argument((b32)(index + 2)));
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
        p8 line[FILE_PATH_MAX];
        positive count = (positive)program_argument_count();
        positive length = 0;

        if (count < 2)
        {
                line[length++] = 'y';
        }
        else
        {
                for (positive i = 1; i < count; i++)
                {
                        string_address word = program_argument((b32)i);

                        if (i > 1 && length + 1 < FILE_PATH_MAX)
                                line[length++] = ' ';

                        for (positive j = 0; string_get(word + j) && length + 1 < FILE_PATH_MAX; j++)
                                line[length++] = string_get(word + j);
                }
        }

        line[length++] = '\n';

        // One write of many copies rather than one write per line: the same
        // bytes leave the program in a fraction of the system calls.
        p8 block[FILE_BLOCK * 4];
        positive filled = 0;

        while (filled + length <= sizeof(block))
        {
                memory_copy(block + filled, line, length);
                filled += length;
        }

        while (1)
        {
                if (system_call_3(syscall(write), stdout, (positive)block, filled) <= 0)
                        return 1;
        }

        return 0;
}

// env ------------------------------------------------------------
/*
        env [-i] [-u NAME] [NAME=VALUE]... [COMMAND [ARGUMENT]...]

        With no command it prints the environment it would have used, which is
        also the only way anything here can look at its own environment.
*/
#define ENV_MAX 512
#define ENV_ARGUMENTS_MAX 64

static string_address env_list[ENV_MAX + 1];
static positive env_have;

static string_address env_key_end(string_address entry)
{
        return string_first_of(entry, '=');
}

static bool env_same_key(string_address entry, string_address name, positive length)
{
        string_address mark = env_key_end(entry);

        if (!mark || (positive)(mark - entry) != length)
                return false;

        for (positive i = 0; i < length; i++)
                if (string_get(entry + i) != string_get(name + i))
                        return false;

        return true;
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

static fn env_put(string_address entry)
{
        string_address mark = env_key_end(entry);

        if (mark)
        {
                positive length = (positive)(mark - entry);

                for (positive i = 0; i < env_have; i++)
                {
                        if (env_same_key(env_list[i], entry, length))
                        {
                                env_list[i] = entry;
                                return;
                        }
                }
        }

        if (env_have < ENV_MAX)
                env_list[env_have++] = entry;
}

static b32 file_env()
{
        positive count = (positive)program_argument_count();
        positive index = 1;
        bool empty = false;

        while (index < count)
        {
                string_address argument = program_argument((b32)index);

                if (string_is(argument, '-') && string_is(argument + 1, '-') &&
                    string_is(argument + 2, end))
                {
                        index++;
                        break;
                }

                if (string_is(argument, '-') && string_is(argument + 1, 'i') &&
                    string_is(argument + 2, end))
                {
                        empty = true;
                        index++;
                        continue;
                }

                if (string_is(argument, '-') && string_is(argument + 1, end))
                {
                        empty = true;
                        index++;
                        continue;
                }

                break;
        }

        if (!empty)
        {
                for (b32 i = 0; program_environment(i) && env_have < ENV_MAX; i++)
                        env_list[env_have++] = program_environment(i);
        }

        while (index < count)
        {
                string_address argument = program_argument((b32)index);

                if (string_is(argument, '-') && string_is(argument + 1, 'u') &&
                    string_is(argument + 2, end) && index + 1 < count)
                {
                        env_drop(program_argument((b32)(index + 1)));
                        index += 2;
                        continue;
                }

                if (!env_key_end(argument))
                        break;

                env_put(argument);
                index++;
        }

        env_list[env_have] = null;

        if (index >= count)
        {
                for (positive i = 0; i < env_have; i++)
                        file_line(env_list[i]);

                log_flush();
                return 0;
        }

        string_address arguments[ENV_ARGUMENTS_MAX + 1];
        positive have = 0;

        while (index < count && have < ENV_ARGUMENTS_MAX)
                arguments[have++] = program_argument((b32)index++);

        arguments[have] = null;

        log_flush();

        p8 candidate[FILE_PATH_MAX];
        string_address name = arguments[0];

        if (string_first_of(name, '/'))
        {
                system_call_3(syscall(execve), (positive)name, (positive)arguments,
                              (positive)env_list);
        }
        else
        {
                // PATH from the environment being handed on, not from the one
                // this program was started with: env -i changes both.
                string_address path = null;

                for (positive i = 0; i < env_have; i++)
                {
                        if (env_same_key(env_list[i], (string_address) "PATH", 4))
                        {
                                path = env_list[i] + 5;
                                break;
                        }
                }

                if (!path)
                        path = (string_address) "/bin:/usr/bin:/";

                while (string_get(path))
                {
                        positive length = 0;

                        while (string_get(path + length) && !string_is(path + length, ':'))
                                length++;

                        positive filled = 0;

                        for (positive i = 0; i < length && filled + 1 < FILE_PATH_MAX; i++)
                                candidate[filled++] = string_get(path + i);

                        if (filled == 0)
                                candidate[filled++] = '.';

                        if (candidate[filled - 1] != '/')
                                candidate[filled++] = '/';

                        for (positive i = 0; string_get(name + i) && filled + 1 < FILE_PATH_MAX; i++)
                                candidate[filled++] = string_get(name + i);

                        candidate[filled] = end;

                        system_call_3(syscall(execve), (positive)candidate,
                                      (positive)arguments, (positive)env_list);

                        path += length;

                        if (string_is(path, ':'))
                                path++;
                }
        }

        string_format(file_fail, "env: '%s': No such file or directory\n", name);

        return 127;
}

// id ------------------------------------------------------------
// id [-u|-g|-G] [-n] [-r], and the readable default when none of them is given.
#define ID_GROUPS_MAX 64

static fn id_named(positive value, bool group)
{
        p8 name[FILE_NAME_MAX];
        bool known = group ? file_group_name(value, name, FILE_NAME_MAX)
                           : file_user_name(value, name, FILE_NAME_MAX);

        file_number(log, value);

        if (known)
        {
                log("(", 1);
                log(name, 0);
                log(")", 1);
        }
}

static b32 file_id()
{
        positive first = 0;
        positive flags = file_take_options((string_address) "ugGnr", address_of first);

        bool real = (flags & FILE_FLAG('r')) != 0;
        bool names = (flags & FILE_FLAG('n')) != 0;

        positive user = (positive)system_call(syscall(getuid));
        positive effective_user = (positive)system_call(syscall(geteuid));
        positive group = (positive)system_call(syscall(getgid));
        positive effective_group = (positive)system_call(syscall(getegid));

        if (!real)
        {
                user = effective_user;
                group = effective_group;
        }

        p8 name[FILE_NAME_MAX];

        if (flags & FILE_FLAG('u'))
        {
                if (names && file_user_name(user, name, FILE_NAME_MAX))
                        file_line(name);
                else
                {
                        file_number(log, user);
                        log("\n", 1);
                }

                log_flush();
                return 0;
        }

        if (flags & FILE_FLAG('g'))
        {
                if (names && file_group_name(group, name, FILE_NAME_MAX))
                        file_line(name);
                else
                {
                        file_number(log, group);
                        log("\n", 1);
                }

                log_flush();
                return 0;
        }

        p32 members[ID_GROUPS_MAX];
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

        have = (bipolar)keep;

        if (flags & FILE_FLAG('G'))
        {
                for (positive i = 0; i < (positive)have; i++)
                {
                        if (i)
                                log(" ", 1);

                        if (names && file_group_name(members[i], name, FILE_NAME_MAX))
                                log(name, 0);
                        else
                                file_number(log, members[i]);
                }

                log("\n", 1);
                log_flush();
                return 0;
        }

        log("uid=", 0);
        id_named(user, false);
        log(" gid=", 0);
        id_named(group, true);

        if (have > 0)
        {
                log(" groups=", 0);

                for (positive i = 0; i < (positive)have; i++)
                {
                        if (i)
                                log(",", 1);

                        id_named(members[i], true);
                }
        }

        log("\n", 1);
        log_flush();

        return 0;
}

// hostname ------------------------------------------------------------
// hostname, and hostname -s for the part before the first dot.
static b32 file_hostname()
{
        file_machine facts;
        positive first = 0;
        positive flags = file_take_options((string_address) "sf", address_of first);

        memory_fill(address_of facts, 0, sizeof(facts));

        if (system_call_1(syscall(uname), (positive)address_of facts) < 0)
        {
                file_fail("hostname: cannot read system name\n", 0);
                return 1;
        }

        if (flags & FILE_FLAG('s'))
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
*/
static b32 file_uname()
{
        file_machine facts;
        positive first = 0;
        positive flags = file_take_options((string_address) "asnrvmpio", address_of first);

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

        if (flags & FILE_FLAG('p'))
        {
                if (written++)
                        log(" ", 1);

                log(facts.machine, 0);
        }

        if (flags & FILE_FLAG('i'))
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

static string_address file_environment(string_address name)
{
        positive length = string_length(name);

        for (b32 i = 0; program_environment(i); i++)
        {
                string_address entry = program_environment(i);
                positive at = 0;

                while (at < length && string_get(entry + at) == string_get(name + at))
                        at++;

                if (at == length && string_get(entry + at) == '=')
                        return entry + at + 1;
        }

        return null;
}

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

static b32 file_mktemp()
{
        positive count = (positive)program_argument_count();
        positive index = 1;
        bool directory = false;
        bool dry = false;
        bool quiet = false;
        bool rooted = false;
        string_address base = null;
        string_address template = null;
        p8 path[FILE_PATH_MAX];
        positive length = 0;
        positive marks_at;
        positive marks = 0;

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

                if (string_is(argument + 1, 'p') && string_is(argument + 2, end))
                {
                        if (index + 1 >= count)
                        {
                                file_fail("mktemp: -p needs a directory\n", 0);
                                return 1;
                        }

                        base = program_argument((b32)(index + 1));
                        rooted = true;
                        index += 2;
                        continue;
                }

                if (string_is(argument + 1, '-'))
                {
                        positive at = 0;
                        string_address named = "--tmpdir=";

                        while (string_get(named + at) &&
                               string_get(argument + at) == string_get(named + at))
                                at++;

                        if (!string_get(named + at))
                        {
                                base = argument + at;
                                rooted = true;
                                index++;
                                continue;
                        }

                        if (string_compare(argument, "--tmpdir") == 0)
                        {
                                rooted = true;
                                index++;
                                continue;
                        }

                        if (string_compare(argument, "--directory") == 0)
                                directory = true;
                        else if (string_compare(argument, "--dry-run") == 0)
                                dry = true;
                        else if (string_compare(argument, "--quiet") == 0)
                                quiet = true;
                        else
                        {
                                string_format(file_fail, "mktemp: unknown option: %s\n",
                                              argument);
                                return 1;
                        }

                        index++;
                        continue;
                }

                for (positive letter = 1; string_get(argument + letter); letter++)
                {
                        p8 which = string_get(argument + letter);

                        if (which == 'd')
                                directory = true;
                        else if (which == 'u')
                                dry = true;
                        else if (which == 'q')
                                quiet = true;
                        else if (which == 't')
                                rooted = true;
                        else
                        {
                                string_format(file_fail,
                                              "mktemp: unknown option: %s\n", argument);
                                return 1;
                        }
                }

                index++;
        }

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

                while (string_get(base + length) && length < FILE_PATH_MAX - 2)
                {
                        path[length] = string_get(base + length);
                        length++;
                }

                while (length > 1 && path[length - 1] == '/')
                        length--;

                path[length++] = '/';
        }

        for (positive at = 0; string_get(template + at); at++)
        {
                if (length >= FILE_PATH_MAX - 1)
                {
                        file_fail("mktemp: template too long\n", 0);
                        return 1;
                }

                path[length++] = string_get(template + at);
        }

        path[length] = end;

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
                file_digits(into, number);
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
                file_digits(into + at, number - KILL_LEAST_REAL);
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
        file_digits(into + at, KILL_MOST - number);
}

static bipolar kill_number(string_address word)
{
        p8 name[16];

        if (file_all_digits(word))
                return (bipolar)string_to_positive(word);

        if (string_is(word, 'S') && string_is(word + 1, 'I') && string_is(word + 2, 'G'))
                word += 3;

        for (positive i = 1; i <= KILL_MOST; i++)
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

                if (!file_all_digits(word))
                {
                        string_format(file_fail, "kill: Illegal number: %s\n", word);
                        return 2;
                }

                number = string_to_positive(word);

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
                bipolar who = file_signed(word);

                if (!file_all_digits(string_is(word, '-') ? word + 1 : word))
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

static fn date_pad(writer write, positive value, positive width, p8 pad)
{
        p8 text[24];
        positive length = file_digits(text, value);

        if (pad)
                for (positive i = length; i < width; i++)
                        write(address_of pad, 1);

        write(text, length);
}

// A one means the format asked for no padding at all, which is not the same
// as not asking, and a zero cannot say both.
static fn date_field(writer write, positive value, positive width, p8 pad, p8 fallback)
{
        date_pad(write, value, width, pad == 1 ? 0 : pad ? pad : fallback);
}

static fn date_signed(writer write, b64 value)
{
        if (value < 0)
        {
                write("-", 1);
                value = -value;
        }

        file_number(write, (positive)value);
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
                return file_number(write, at->weekday ? at->weekday : 7);
        case 'w':
                return file_number(write, at->weekday);
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
                return file_number(write, (at->month + 2) / 3);
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
                p8 letter = string_get(format++);
                p8 pad = 0;

                if (letter != '%')
                {
                        write(address_of letter, 1);
                        continue;
                }

                letter = string_get(format);

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

// Only an epoch. A date written out in words is a parser, and one that
// half worked would be worse than one that says it cannot.
static bool date_read(string_address text, b64 address_to out)
{
        bool negative = false;

        if (!string_is(text, '@'))
                return false;

        text++;

        if (string_is(text, '-'))
        {
                negative = true;
                text++;
        }

        if (!file_all_digits(text))
                return false;

        address_to out = (b64)file_count(text);

        if (negative)
                address_to out = -address_to out;

        return true;
}

static b32 file_date()
{
        positive count = (positive)program_argument_count();
        positive index = 1;
        string_address format = null;
        string_address given = null;
        string_address of_file = null;
        bool rfc = false;
        b64 when;
        date_moment at;

        while (index < count)
        {
                string_address argument = program_argument((b32)index);
                positive named = 0;
                string_address wanted = "--date=";

                if (!string_is(argument, '-') || string_is(argument + 1, end))
                        break;

                if (string_is(argument + 1, '-') && string_is(argument + 2, end))
                {
                        index++;
                        break;
                }

                while (string_get(wanted + named) &&
                       string_get(argument + named) == string_get(wanted + named))
                        named++;

                if (!string_get(wanted + named))
                {
                        given = argument + named;
                        index++;
                        continue;
                }

                if (string_is(argument + 1, 'd') && string_is(argument + 2, end))
                {
                        if (index + 1 >= count)
                        {
                                file_fail("date: -d needs a date\n", 0);
                                return 1;
                        }

                        given = program_argument((b32)(index + 1));
                        index += 2;
                        continue;
                }

                if (string_is(argument + 1, 'r') && string_is(argument + 2, end))
                {
                        if (index + 1 >= count)
                        {
                                file_fail("date: -r needs a file\n", 0);
                                return 1;
                        }

                        of_file = program_argument((b32)(index + 1));
                        index += 2;
                        continue;
                }

                if (string_is(argument + 1, 'u') && string_is(argument + 2, end))
                {
                        index++;
                        continue;
                }

                if (string_is(argument + 1, 'R') && string_is(argument + 2, end))
                {
                        rfc = true;
                        index++;
                        continue;
                }

                if (!string_compare(argument, "--utc") ||
                    !string_compare(argument, "--universal"))
                {
                        index++;
                        continue;
                }

                if (!string_compare(argument, "--rfc-2822") ||
                    !string_compare(argument, "--rfc-email"))
                {
                        rfc = true;
                        index++;
                        continue;
                }

                string_format(file_fail, "date: unknown option: %s\n", argument);
                return 1;
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
                if (!date_read(given, address_of when))
                {
                        string_format(file_fail, "date: invalid date '%s'\n", given);
                        return 1;
                }
        }
        else
                when = file_now();

        date_take(when, address_of at);

        if (!format)
                format = rfc ? "%a, %d %b %Y %H:%M:%S %z" : "%a %b %e %H:%M:%S %Z %Y";

        date_shape(log, address_of at, format);
        log("\n", 1);
        log_flush();

        return 0;
}
