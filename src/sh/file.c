#include "../compiler_memory.c"

/* Defined by text.c later in the multicall translation unit. */
static address_any text_arena_take(positive bytes);
static positive text_arena_used;
static p8 address_to text_arena_read_all(positive handle, positive first,
                                         positive address_to length,
                                         bool address_to read_failed);

/* file.c is included before text.c in the multicall translation unit.  These
   two leaves let csplit use that one BRE compiler and matcher without growing
   a second regular-expression implementation here. */
static bool regex_compile(string_address pattern, bool extended, bool icase,
                          bool escapes, p8 policy);
static bool regex_search(string_address text, positive length, positive from);

/* Every arena-backed vector shares one rare grow/copy path.  The common
   typed front keeps a full store to existing room on the caller's hot path. */
#if X64
#define TEXT_ARENA_GROW
#else
#define TEXT_ARENA_GROW COLD __attribute__((noinline))
#endif
static TEXT_ARENA_GROW bool text_arena_grow(
    address_any table, positive address_to room, positive used,
    positive wanted, positive unit, positive first)
{
        if (wanted < used)
                return false;
        if (wanted <= address_to room)
                return true;

        positive larger = memory_growth(address_to room, wanted, first);

        if (!larger || larger > positive_max / unit)
                return false;

        address_any grown = text_arena_take(larger * unit);

        if (!grown)
                return false;
        if (used)
                memory_copy_apart(grown,
                                  address_to(address_any address_to)table,
                                  used * unit);

        address_to(address_any address_to)table = grown;
        address_to room = larger;
        return true;
}
#undef TEXT_ARENA_GROW

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
#define ERROR_NO_PROCESS 3
#define ERROR_BAD_DESCRIPTOR 9
#define ERROR_NO_DEVICE_ADDRESS 6
#define ERROR_ACCESS 13
#define ERROR_ARGUMENT_LIST 7
#define ERROR_EXISTS 17
#define ERROR_CROSS_DEVICE 18
#define ERROR_NOT_DIRECTORY 20
#define ERROR_IS_DIRECTORY 21
#define ERROR_INVALID 22
#define ERROR_NOT_TERMINAL 25
#define ERROR_ILLEGAL_SEEK 29
#define ERROR_NO_SYSTEM_CALL 38
#define ERROR_NAME_TOO_LONG 36
#define ERROR_NOT_EMPTY 39
#define ERROR_NOT_SUPPORTED 95

#define STATX_BASIC 0x7ff
#define STATX_BIRTH 0x800
#define STATX_MOUNT_ID 0x1000

// The basic set stops short of both creation time and mount identity.  A
// filesystem that does not keep creation time says so in the returned mask;
// mount identity is kernel topology and is what findmnt -T and mountpoint use
// to distinguish a bind mount from its parent.
#define STATX_WANTED (STATX_BASIC | STATX_BIRTH | STATX_MOUNT_ID)

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

/* Linux deliberately gives d_type the same low nibble that st_mode carries
   in bits 12..15. One sparse schema therefore drives stat's letter/name,
   find's d_type and -type mappings, and ls's classification and colours. */
typedef struct
{
        string_address name;
        string_address colour_key;
        string_address colour_fallback;
        p8 letter;
        p8 mark;
} file_kind_descriptor;

#define FILE_KIND_ROWS(X)                                                   \
        X(PIPE,      'p', '|', "fifo",                   "pi", "33")       \
        X(CHARACTER, 'c',  0,  "character special file", "cd", "33;01")    \
        X(DIRECTORY, 'd', '/', "directory",              0,    0)           \
        X(BLOCK,     'b',  0,  "block special file",     "bd", "33;01")    \
        X(FILE,      'f',  0,  "regular file",           0,    0)           \
        X(LINK,      'l', '@', "symbolic link",          0,    0)           \
        X(SOCKET,    's', '=', "socket",                 "so", "01;35")

#define FILE_KIND_DESCRIPTOR(kind, letter, mark, name, colour, fallback)    \
        [MODE_##kind >> 12] = {(string_address)name,                         \
                               (string_address)colour,                       \
                               (string_address)fallback, letter, mark},
static const file_kind_descriptor file_kinds[16] = {
    FILE_KIND_ROWS(FILE_KIND_DESCRIPTOR)};
#undef FILE_KIND_DESCRIPTOR

#define FILE_KIND_LETTER(kind, letter, mark, name, colour, fallback)        \
        [letter] = MODE_##kind >> 12,
static const p8 file_kind_from_letter[128] = {
    FILE_KIND_ROWS(FILE_KIND_LETTER)};
#undef FILE_KIND_LETTER
#undef FILE_KIND_ROWS

#define file_kind_of(mode)                                                   \
        (address_of file_kinds[((mode) & MODE_FORMAT) >> 12])

CONST p8 file_kind_letter(positive mode)
{
        const file_kind_descriptor address_to kind = file_kind_of(mode);

        return kind->name && kind->letter != 'f' ? kind->letter : '-';
}

CONST RETURNS_NONNULL string_address file_kind_name(positive mode)
{
        string_address name = file_kind_of(mode)->name;

        return name ? name : file_kinds[MODE_FILE >> 12].name;
}

/*
        A directory entry already tells us its file kind on the filesystems
        where d_type is available, and the kind is the whole of what a walk
        needs to decide where to descend and what a plain listing needs to
        print.  Zero is an entry the filesystem would not describe, which is
        what makes the caller ask the kernel.
*/
static CONST positive file_mode_from_type(p8 type)
{
        return type < array_count(file_kinds) && file_kinds[type].name
                   ? (positive)type << 12
                   : 0;
}

static inline INLINE CONST p64 file_device_key(p32 major, p32 minor)
{
        return ((p64)major << 32) | minor;
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

        A directory's set-user-ID and set-group-ID bits are kept unless the
        specification mentions them, which is how the reference chmod reads
        "chmod 755 dir" on a set-group-ID directory: the bit stays, and it
        takes a fifth octal digit, an s, or a class that carries one to say
        otherwise.  A clause with no class named applies the unnamed set the
        caller supplies -- the umask-filtered set for chmod and for the tools
        that create something, every bit for find -- and "=" with no class
        clears everything else, as the reference does.

        A copied class ("g=u") is read from what the clauses so far have
        made, which is chmod's reading; the shell's umask reads it from what
        the mask was before the command, so "u=rw,g+u" gives the group all
        three bits there, and the caller says which it wants.
*/
static bool file_mode_adjust(string_address specification, positive current,
                             bool directory, positive unnamed,
                             bool copies_original, positive address_to result)
{
        positive mode = current & 07777;
        positive kept = directory ? 06000 : 0;

        if (string_get(specification) >= '0' && string_get(specification) <= '7')
        {
                positive used;
                positive value = string_digits_octal_max(
                    specification, (positive)-1, address_of used);
                string_address step = specification + used;

                if (string_get(step))
                        return false;

                if (value > 07777)
                        return false;

                // Fewer than five digits mention only the special bits they
                // set; five or more mention all of them.
                positive mentioned = used < 5 ? value & 06000 : 06000;

                address_to result = value | (mode & kept & ~mentioned);
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
                                who |= 01007;

                        if (string_is(step, 'a'))
                                who |= 07777;

                        named = true;
                        step++;
                }

                if (!named)
                        who = unnamed;

                if (!string_is(step, '+') && !string_is(step, '-') && !string_is(step, '='))
                        return false;

                while (string_is(step, '+') || string_is(step, '-') || string_is(step, '='))
                {
                        p8 action = string_get(step);
                        positive copied = copies_original ? current & 07777 : mode;
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
                                        bits |= ((copied & 00700) >> 6) * 00111;
                                else if (letter == 'g')
                                        bits |= ((copied & 00070) >> 3) * 00111;
                                else if (letter == 'o')
                                        bits |= (copied & 00007) * 00111;
                                else
                                        return false;

                                step++;
                        }

                        positive mentioned = named ? who & bits : bits;
                        positive omit = kept & ~mentioned;

                        bits &= who & ~omit;

                        if (action == '+')
                                mode |= bits;
                        else if (action == '-')
                                mode &= ~bits;
                        else if (named)
                                mode = (mode & ~(who & ~omit)) | bits;
                        else
                                mode = (mode & omit) | bits;
                }

                if (string_is(step, ','))
                        step++;
                else if (string_get(step))
                        return false;
        }

        address_to result = mode & 07777;

        return true;
}

bool file_mode_of(string_address specification, positive current, bool directory,
                  positive address_to result)
{
        return file_mode_adjust(specification, current, directory, 07777,
                                false, result);
}

// The process umask, read without changing it.  Asked once per command and
// not once per process, because the shell's own umask builtin can change it
// between two commands that run in the same process.
static positive file_umask()
{
        bipolar mask = system_call_1(syscall(umask), 0);

        if (mask < 0)
                return 0;

        system_call_1(syscall(umask), (positive)mask);

        return (positive)mask & 0777;
}

// The same reading, for the tools whose unnamed class is filtered through the
// umask: chmod, and mkdir, mkfifo and mknod when -m names a mode.
static bool file_mode_masked(string_address specification, positive current,
                             bool directory, positive mask,
                             positive address_to result)
{
        return file_mode_adjust(specification, current, directory,
                                07000 | (0777 & ~mask), false, result);
}

// Looking at files ------------------------------------------

// The kernel's own code, for the callers that report why a look failed; the
// bool form below is what the tests of existence and kind read.
static bipolar file_look_code(bipolar directory, string_address path,
                              positive flags, file_facts address_to out)
{
        memory_fill(out, 0, sizeof(file_facts));

        return system_stat_at(directory, path, flags | AT_NO_AUTOMOUNT,
                              STATX_WANTED, out);
}

bool file_look(bipolar directory, string_address path, positive flags,
               file_facts address_to out)
{
        return file_look_code(directory, path, flags, out) == 0;
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

static CONST positive file_device(p32 major, p32 minor);

static bool file_same_identity(file_facts address_to one, file_facts address_to two)
{
        return one->inode == two->inode && one->device_major == two->device_major &&
               one->device_minor == two->device_minor;
}

// The access and modification times in the shape utimensat takes, which is
// what cp -p and a move across devices both carry over.
static fn file_times_of(file_facts address_to facts, p64 address_to times)
{
        times[0] = (p64)facts->accessed.seconds;
        times[1] = facts->accessed.nanoseconds;
        times[2] = (p64)facts->modified.seconds;
        times[3] = facts->modified.nanoseconds;
}

// Paths -----------------------------------------------------

static string_address file_last_component(string_address path)
{
        string_address last = string_last_of(path, '/');

        return last ? last + 1 : path;
}

/*
        path_join takes as much of a name as fits and says nothing about the
        rest.  A path cut to fit is some other path, so every walker asks
        whether the whole of it went in before it touches what the name
        stands for, and refuses with the kernel's own words when it did not.
*/
static bool file_path_join(p8 address_to into, string_address directory,
                           string_address name)
{
        positive head = string_length(directory);
        positive wanted = head + string_length(name) +
                          (head && directory[head - 1] != '/' ? 1 : 0);

        return path_join(into, FILE_PATH_MAX, directory, name) == wanted;
}

/*
        One walk for every colon-separated search: CDPATH, the PATH that .
        reads along, the one command -v and the executor look along, and
        the one execvp tries.  Each used to split at the colon and glue
        "segment/name" on its own, and the empty component -- after a
        leading, doubled or trailing colon -- was a separate decision in
        each of them.
*/
typedef struct
{
        string_address at;
        string_address segment;
        positive length;
        bool done;
} path_walk;

// Every component, the empty ones included: a colon at the end names one
// more after it, and a value with no colon in it at all is exactly one.
static bool path_walk_next(path_walk address_to walk)
{
        string_address stop;

        if (walk->done)
                return false;

        stop = string_first_of_or_end(walk->at, ':');
        walk->segment = walk->at;
        walk->length = (positive)(stop - walk->at);

        if (string_get(stop))
                walk->at = stop + 1;
        else
                walk->done = true;

        return true;
}

/*
        "segment/name" into room bytes, the slash added only when the
        segment does not already end in one.  What an empty component
        stands for belongs to the caller: "." for execvp, nothing at all
        for a name the shell opens bare, and the directory the shell is in
        for cd.  A candidate that does not fit is refused rather than cut,
        for the reason file_path_join gives.
*/
static bool path_walk_join(p8 address_to into, positive room,
                           string_address segment, positive length,
                           string_address name, string_address empty_as)
{
        positive named = string_length(name);
        positive slash;

        if (!length)
        {
                segment = empty_as;
                length = string_length(segment);
        }

        slash = length && segment[length - 1] != '/';

        // Each part is held under room before the parts are added, so the
        // pair of positive_max guards every walker carried is this one.
        if (length > room || named > room - length ||
            room - length - named < slash + 1)
                return false;

        memory_copy_apart(into, segment, length);

        if (slash)
                into[length++] = '/';

        memory_copy_apart_end(into + length, name, named);

        return true;
}

CONST RETURNS_NONNULL string_address file_reason(bipolar code);

static COLD fn file_too_long(string_address program, string_address verb,
                             string_address directory, string_address name)
{
        string_format(file_fail, "%s: %s '%s/%s': %s\n", program, verb,
                      directory, name, file_reason(-ERROR_NAME_TOO_LONG));
}

/* Claim an exclusive temporary name beside a destination, so the eventual
   rename cannot cross a filesystem.  Editors and in-place text filters need
   the same retry machine; only their marker, nonce and creation mode differ. */
static COLD bipolar file_temporary_open(string_address path, p8 address_to into,
                                        positive room, string_address marker,
                                        positive marker_length, positive value,
                                        positive attempts, positive mode)
{
        string_address slash = string_last_of(path, '/');
        positive prefix = slash ? (positive)(slash - path) + 1 : 0;
        p8 number[24];

        if (!room || prefix > room || marker_length >= room - prefix)
        {
                if (room)
                        into[0] = end;
                return -ERROR_INVALID;
        }

        memory_copy_apart(into, path, prefix);
        memory_copy_apart(into + prefix, marker, marker_length);

        for (positive attempt = 0; attempt < attempts; attempt++)
        {
                positive length = positive_into_string(number, value + attempt);

                if (length >= room - prefix - marker_length)
                {
                        into[0] = end;
                        return -ERROR_INVALID;
                }

                memory_copy_end(into + prefix + marker_length, number, length);

                bipolar handle = system_open_at_mode(
                    AT_FDCWD, into, FILE_WRITE | FILE_CREATE | FILE_EXCLUSIVE,
                    mode);

                if (handle >= 0 || handle != -ERROR_EXISTS)
                        return handle;
        }

        return -ERROR_EXISTS;
}

bipolar file_link_text(string_address path, p8 address_to into, positive limit)
{
        bipolar length = system_read_link_at(AT_FDCWD, path, into, limit - 1);

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
                memory_copy_apart(into, here, length);

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

                length = (positive)(memory_copy_apart_end(
                    into + length, rest + start, piece) - into);

                if (!follow)
                        continue;

                bipolar seen = system_read_link_at(
                    AT_FDCWD, into, link, FILE_PATH_MAX - 1);

                if (seen <= 0)
                        continue;

                if (++hops > 40)
                        return false;

                link[seen] = end;

                positive fill = (positive)seen;

                memory_copy_apart(merged, link, fill);

                if (rest[at] && fill + 1 < FILE_PATH_MAX)
                        merged[fill++] = '/';

                positive left = string_length_max(rest + at, FILE_PATH_MAX - fill);

                // What follows the link has to fit behind it whole; a tail
                // cut to fit is some other path.
                if (left >= FILE_PATH_MAX - fill)
                        return false;

                fill = (positive)(memory_copy_apart_end(
                    merged + fill, rest + at, left) - merged);

                memory_copy_apart(rest, merged, fill + 1);

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

typedef struct
{
        p8 text[FILE_ACCOUNTS_MAX];
        p8 seen_name[FILE_NAME_MAX];
        positive seen;
        bool read;
        bool seen_set;
        bool seen_known;
} file_account_cache;

#define FILE_ACCOUNT_USER 0
#define FILE_ACCOUNT_GROUP 1

static file_account_cache file_accounts[2];
static const string_address file_account_paths[2] = {"/etc/passwd", "/etc/group"};

// ls -l asks for a name per entry, so the file is read once and kept rather
// than opened again for every line of a listing.
static RETURNS_NONNULL p8 address_to file_account_text(positive which)
{
        file_account_cache address_to cache = file_accounts + which;

        if (!cache->read)
        {
                cache->read = true;

                if (file_slurp(file_account_paths[which], cache->text,
                               FILE_ACCOUNTS_MAX) <= 0)
                        cache->text[0] = end;
        }

        return cache->text;
}

/*
        colon separated records, name first and the numeric id in the field
        given. /etc/passwd and /etc/group agree on both of those, so one
        reader serves both.
*/
typedef struct
{
        string_address name;
        positive name_length;
        string_address value;
        positive value_length;
        bool has_value;
} file_account_record;

/* One bounded record and the requested field inside it. A malformed record
   still comes back so a matching name can be distinguished from no name at
   all; has_value says whether it reached the requested column. */
static bool file_account_next(p8 address_to text, positive address_to at,
                              positive field,
                              file_account_record address_to record)
{
        if (!text[address_to at])
                return false;

        positive line = address_to at;
        positive stop = (positive)(string_first_of_or_end(text + line, '\n') -
                                   text);
        p8 address_to mark = (p8 address_to)memory_first_of(
            text + line, ':', stop - line);
        positive step = mark ? (positive)(mark - text) : stop;
        positive column = 0;

        address_to at = text[stop] ? stop + 1 : stop;
        record->name = text + line;
        record->name_length = step - line;
        record->value = text;
        record->value_length = 0;

        while (step < stop && column < field)
        {
                positive start = ++step;

                mark = (p8 address_to)memory_first_of(text + step, ':',
                                                      stop - step);
                step = mark ? (positive)(mark - text) : stop;
                record->value = text + start;
                record->value_length = step - start;
                column++;
        }

        record->has_value = column == field;
        return true;
}

bool file_account_name(p8 address_to text, positive wanted, positive field,
                       p8 address_to into, positive limit)
{
        positive at = 0;
        file_account_record record;

        while (file_account_next(text, address_of at, field,
                                 address_of record))
        {
                if (!record.has_value)
                        continue;

                positive taken;
                positive value = string_digits_max(record.value,
                                                   record.value_length,
                                                   address_of taken);

                if (taken != record.value_length || !taken || value != wanted)
                        continue;

                positive found = record.name_length;

                if (found > limit - 1)
                        found = limit - 1;

                memory_copy_apart_end(into, record.name, found);

                return true;
        }

        return false;
}

bipolar file_account_id(p8 address_to text, string_address name, positive field)
{
        positive wanted = string_length(name);
        positive at = 0;
        file_account_record record;

        while (file_account_next(text, address_of at, field,
                                 address_of record))
        {
                if (record.name_length != wanted)
                        continue;

                if (memory_compare(record.name, name, wanted))
                        continue;

                if (!record.has_value)
                        return -1;

                positive taken;
                positive value = string_digits_max(record.value,
                                                   record.value_length,
                                                   address_of taken);

                if (taken != record.value_length)
                        return -1;

                return (bipolar)value;
        }

        return -1;
}

// One remembered answer per table, because a directory listing asks the same
// question once per entry and almost every entry gives the same id.
static bool file_account_cached_name(positive which, positive id,
                                     p8 address_to into, positive limit)
{
        file_account_cache address_to cache = file_accounts + which;

        if (!limit)
                return false;

        if (!cache->seen_set || id != cache->seen)
        {
                cache->seen_set = true;
                cache->seen = id;
                cache->seen_known = file_account_name(
                    file_account_text(which), id, 2, cache->seen_name,
                    FILE_NAME_MAX);
        }

        if (!cache->seen_known)
                return false;

        string_copy_max_end(into, cache->seen_name, limit - 1);

        return true;
}

#define file_user_name(id, into, limit)                                      \
        file_account_cached_name(FILE_ACCOUNT_USER, (id), (into), (limit))
#define file_group_name(id, into, limit)                                     \
        file_account_cached_name(FILE_ACCOUNT_GROUP, (id), (into), (limit))
#define file_user_id(name)                                                   \
        file_account_id(file_account_text(FILE_ACCOUNT_USER), (name), 2)
#define file_group_id(name)                                                  \
        file_account_id(file_account_text(FILE_ACCOUNT_GROUP), (name), 2)

// A user or group the way every listing says one: the name when there is
// one and a name was wanted, the number otherwise.  Answers whether a name
// was found, which is what groups alone has something to say about.
static bool file_account_label(positive id, bool group, bool named,
                               p8 address_to into)
{
        if (named && (group ? file_group_name(id, into, FILE_NAME_MAX)
                            : file_user_name(id, into, FILE_NAME_MAX)))
                return true;

        positive_into_string(into, id);

        return false;
}

// Time ------------------------------------------------------

fn file_split_moment(b64 seconds, b64 address_to year, positive address_to month,
                     positive address_to day, positive address_to hour,
                     positive address_to minute, positive address_to second)
{
        b64 days = clock_floor_divide(seconds, CLOCK_SECONDS_PER_DAY);
        b64 rest = seconds - days * CLOCK_SECONDS_PER_DAY;
        bipolar civil_year;
        bipolar civil_month;
        bipolar civil_day;

        clock_civil_from_days(days, address_of civil_year,
                              address_of civil_month, address_of civil_day);

        address_to year = civil_year;
        address_to month = (positive)civil_month;
        address_to day = (positive)civil_day;
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

// Month and weekday names as the reference date's own output spells them:
// the whole word or its first three letters, and "sept" for the one month
// whose four letter form is the usual one.
static const string_address file_month_names[12] = {
    "january", "february", "march",     "april",   "may",      "june",
    "july",    "august",   "september", "october", "november", "december"};

// The three-letter form the listings print, spelt with a capital the way
// the reference ls and ps spell it, taken from the one table rather than
// kept as a second one in every printer.
static fn file_month_short(writer write, positive month)
{
        string_address full = file_month_names[month - 1];
        p8 name[3] = {(p8)(full[0] - ('a' - 'A')), full[1], full[2]};

        write(name, 3);
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
        b64 year;
        positive month, day, hour, minute, second;

        file_split_moment(seconds, address_of year, address_of month, address_of day,
                          address_of hour, address_of minute, address_of second);

        file_month_short(write, month);
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
          HH:MM[:SS[.FRACTION]]     that time, on whatever day is in hand
          a T or a space between the two
          MONTH DAY[,] [YEAR]       and DAY MONTH [YEAR], the month by its
          DAY MONTH [YEAR]          name or its first three letters, the year
                                    this one when it is left out
          WEEKDAY[,]                the next such day, today included, when no
                                    date was given; passed over beside one
          [+-]HH[MM]  [+-]HH:MM     after a clock time, the zone that time is
                                    in, and it is turned back into UTC
          now  today  yesterday  tomorrow
          nothing at all           midnight on the day in hand
          [+-]N UNIT               and next UNIT, last UNIT, a bare UNIT
          ...UNIT... ago           turns every displacement in the string round
          UTC  GMT  Z              passed over: everything here is UTC already

        That is what the system's own date and stat print, so their output
        can be given back to touch -d and date -d.

        UNIT is sec, min, hour, day, week, fortnight, month or year, with or
        without an s. A month and a year move the calendar rather than the
        clock, so the 31st of January and a month is the 2nd of March, which
        is what the system's own date answers.

        A signed number after a clock time is a zone and not a displacement.
        The system's date reads the + in "12:00 +1 day" as a zone of one hour
        followed by a bare day, and so does this; a tool that read it as a
        day alone would be an hour out with nothing to say so.
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

// A day written down has to be a day the month has; a day arrived at by
// adding months to another one does not, and rolls into the month after.
static positive file_month_days(b64 year, b64 month)
{
        static const p8 lengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

        if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
                return 29;

        return lengths[month - 1];
}

static bool file_same_word(string_address text, positive length, string_address word)
{
        return string_length(word) == length &&
               !memory_compare_ascii_case(text, word, length);
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

// The fraction of a second after a clock time or an epoch, kept to the
// nanosecond that the kernel's timestamps carry; digits past the ninth say
// nothing a file can hold.
static positive file_read_fraction(string_address text, positive at,
                                   positive address_to nanoseconds)
{
        if ((!string_is(text + at, '.') && !string_is(text + at, ',')) ||
            !byte_is_digit(string_get(text + at + 1)))
                return at;

        positive scale = 100000000;

        at++;

        while (byte_is_digit(string_get(text + at)))
        {
                address_to nanoseconds += (positive)(string_get(text + at) - '0') * scale;
                scale /= 10;
                at++;
        }

        return at;
}

static const string_address file_weekday_names[7] = {
    "sunday",   "monday", "tuesday", "wednesday",
    "thursday", "friday", "saturday"};

static bipolar file_name_among(string_address text, positive length,
                               const string_address address_to names,
                               positive count)
{
        for (positive i = 0; i < count; i++)
                if ((length == 3 || length == string_length(names[i])) &&
                    !memory_compare_ascii_case(text, names[i], length))
                        return (bipolar)i;

        if (count == 12 && file_same_word(text, length, (string_address) "sept"))
                return 8;

        return -1;
}

bool file_moment_read_exact(string_address text, b64 now, b64 address_to out,
                            positive address_to nanoseconds)
{
        positive at = 0;

        address_to nanoseconds = 0;

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

                at = file_read_fraction(text, at, nanoseconds);

                at += string_span(text + at, string_set_blanks);

                if (string_get(text + at))
                        return false;

                // The fraction counts forward from the second before, which
                // is what makes @-1.5 the half second before the epoch's
                // own second and not the one after it.
                if (below && address_to nanoseconds)
                {
                        address_to out = -value - 1;
                        address_to nanoseconds = 1000000000 - address_to nanoseconds;
                }
                else
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

        // A date said by its month's name may leave the year for later, and
        // a zone is read only once and only after the clock time it
        // qualifies; a weekday counts only when no date pins the day.
        bool year_wanted = false;
        bool named_date = false;
        bool zoned = false;
        bipolar weekday = -1;
        b64 zone = 0;

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

                if (byte_is_digit(string_get(text + at)))
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
                                dated = true;

                                // A clock time already read stands; the day
                                // is midnight only when no time was said.
                                if (!timed)
                                {
                                        hour = 0;
                                        minute = 0;
                                        second = 0;
                                }

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

                                        at = file_read_fraction(text, at, nanoseconds);
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

                        while (byte_is_alpha(string_get(text + at + length)))
                                length++;

                        const file_unit address_to unit = file_unit_of(text + at, length);

                        // A signed number after a clock time is the zone that
                        // time was said in: hours alone, hours and minutes
                        // run together, or with a colon between them. The
                        // system's date reads the + in "12:00 +1 day" that
                        // way too, as a zone of one hour and then a bare
                        // day, so the unit is left for the next word.
                        if (marked && timed && !zoned)
                        {
                                b64 hours = value;
                                b64 minutes = 0;

                                if (digits == 3 || digits == 4)
                                {
                                        hours = value / 100;
                                        minutes = value % 100;
                                }
                                else if (digits > 4)
                                        return false;
                                else if (string_is(text + at, ':'))
                                {
                                        positive wide;

                                        at = file_read_number(text, at + 1,
                                                              address_of minutes,
                                                              address_of wide);

                                        if (wide != 2)
                                                return false;
                                }

                                if (hours > 23 || minutes > 59)
                                        return false;

                                zone = sign * (hours * 3600 + minutes * 60);
                                zoned = true;

                                continue;
                        }

                        if (!unit)
                        {
                                // The day before its month's name, "9 Sep".
                                bipolar named = file_name_among(text + at, length,
                                                                file_month_names, 12);

                                if (!marked && named >= 0)
                                {
                                        if (dated || value < 1 || value > 31)
                                                return false;

                                        month = (positive)named + 1;
                                        day = (positive)value;
                                        dated = true;
                                        named_date = true;
                                        year_wanted = true;

                                        if (!timed)
                                        {
                                                hour = 0;
                                                minute = 0;
                                                second = 0;
                                        }

                                        at += length;

                                        if (string_is(text + at, ','))
                                                at++;

                                        continue;
                                }

                                // The year a named date left for later, once
                                // a clock time or a third digit says the
                                // number is not a time of day.
                                if (!marked && year_wanted && (timed || digits > 2))
                                {
                                        year = value;
                                        year_wanted = false;

                                        continue;
                                }

                                return false;
                        }

                        at += length;
                        recent = sign * value * unit->seconds;
                        recent_months = sign * value * unit->months;
                        shift += recent;
                        months += recent_months;
                        anything = true;

                        continue;
                }

                if (marked || !byte_is_alpha(string_get(text + at)))
                        return false;

                positive length = 0;

                while (byte_is_alpha(string_get(text + at + length)))
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
                    file_same_word(word, length, (string_address) "today"))
                        continue;

                // A zone by name after a zone by number is a second zone,
                // which the system's date refuses too.
                if (file_same_word(word, length, (string_address) "utc") ||
                    file_same_word(word, length, (string_address) "gmt") ||
                    file_same_word(word, length, (string_address) "z"))
                {
                        if (zoned)
                                return false;

                        continue;
                }

                bipolar named = file_name_among(word, length, file_month_names, 12);

                if (named >= 0)
                {
                        // The month's name before its day, "Sep 9" or
                        // "Sep 9, 2001"; the year, when there is one, is
                        // read by the number that comes to it.
                        positive digits;
                        b64 value;

                        if (dated)
                                return false;

                        at += string_span(text + at, string_set_blanks);
                        at = file_read_number(text, at, address_of value, address_of digits);

                        if (!digits || value < 1 || value > 31)
                                return false;

                        month = (positive)named + 1;
                        day = (positive)value;
                        dated = true;
                        named_date = true;
                        year_wanted = true;

                        if (!timed)
                        {
                                hour = 0;
                                minute = 0;
                                second = 0;
                        }

                        if (string_is(text + at, ','))
                                at++;

                        continue;
                }

                named = file_name_among(word, length, file_weekday_names, 7);

                if (named >= 0)
                {
                        weekday = named;

                        if (string_is(text + at, ','))
                                at++;

                        continue;
                }

                if (file_same_word(word, length, (string_address) "yesterday") ||
                    file_same_word(word, length, (string_address) "tomorrow"))
                {
                        recent = byte_to_lower(string_get(word)) == 'y' ? -86400 : 86400;
                        recent_months = 0;
                        shift += recent;

                        continue;
                }

                bool ahead = file_same_word(word, length, (string_address) "next");

                if (ahead || file_same_word(word, length, (string_address) "last"))
                {
                        at += string_span(text + at, string_set_blanks);

                        positive wide = 0;

                        while (byte_is_alpha(string_get(text + at + wide)))
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

        // A named date is checked once its year is known, because the day
        // February has depends on it.
        if (named_date && day > file_month_days(year, (b64)month))
                return false;

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
        b64 days = clock_days_from_civil(landed, reach - landed * 12 + 1, day);

        // A weekday on its own is the next such day, today included, at
        // midnight unless a time was said; beside a date it is passed over,
        // which is what the system's date does with the one its own output
        // carries.
        if (weekday >= 0 && !dated)
        {
                b64 today = ((days % 7) + 7 + 4) % 7;

                days += (weekday - today + 7) % 7;

                if (!timed)
                {
                        hour = 0;
                        minute = 0;
                        second = 0;
                }
        }

        address_to out = days * 86400 + (b64)hour * 3600 + (b64)minute * 60 +
                         (b64)second + shift - zone;

        return true;
}

bool file_moment_read(string_address text, b64 now, b64 address_to out)
{
        positive nanoseconds;

        return file_moment_read_exact(text, now, out, address_of nanoseconds);
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
        walk->handle = system_open_at(directory, path,
                                     FILE_READ | O_DIRECTORY);
        walk->have = 0;
        walk->at = 0;

        return walk->handle >= 0;
}

struct linux_dirent64 address_to file_walk_next(file_walk address_to walk)
{
        if (walk->at >= walk->have)
        {
                bipolar taken = system_read_directory(
                    walk->handle, walk->block, FILE_BLOCK);

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
                system_close(walk->handle);

        walk->handle = -1;
}

PURE bool file_is_dot(string_address name)
{
        if (!string_is(name, '.'))
                return false;

        if (string_is(name + 1, end))
                return true;

        return string_is(name + 1, '.') && string_is(name + 2, end);
}

/* The recursive cp walk and the cross-device mv walk consume exactly the
   same pair of child paths.  A child whose path on either side would not
   fit whole is passed over and counted, so the caller's status says so. */
static bool file_walk_pair(file_walk address_to walk, string_address program,
                           string_address source, string_address destination,
                           p8 address_to from, p8 address_to to,
                           positive address_to skipped)
{
        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                if (!file_path_join(from, source, entry->d_name) ||
                    !file_path_join(to, destination, entry->d_name))
                {
                        file_too_long(program, (string_address) "cannot copy",
                                      source, entry->d_name);
                        address_to skipped += 1;
                        continue;
                }

                return true;
        }

        return false;
}

/*
        A tool that changes something about a name, and under -R about
        everything beneath it. chmod, chown and chgrp are this one walk with a
        different visit at the leaf, so the visit is what comes in and the
        walk is written once.

        is_directory here asks about the link itself, so a link to a directory
        is changed and not walked into; the depth is what a directory that
        links into itself runs out of before the stack does.
*/
typedef fn(address_to file_visit)(bipolar directory, string_address name,
                                  string_address shown);

static fn file_change_walk(bipolar directory, string_address name, string_address shown,
                           positive depth, string_address program,
                           b32 address_to status, file_visit visit)
{
        visit(directory, name, shown);

        if (!file_is_directory(directory, name))
                return;

        if (depth == 0)
        {
                string_format(file_fail, "%s: '%s' is nested too deep\n",
                              program, shown);
                address_to status = 1;
                return;
        }

        file_walk walk;

        if (!file_walk_open(address_of walk, directory, name))
                return;

        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                p8 below[FILE_PATH_MAX];

                if (!file_path_join(below, shown, entry->d_name))
                {
                        file_too_long(program, (string_address) "cannot access",
                                      shown, entry->d_name);
                        address_to status = 1;
                        continue;
                }

                file_change_walk(walk.handle, entry->d_name, below, depth - 1,
                                 program, status, visit);
        }

        file_walk_close(address_of walk);
}

// The operand list those three read, which is the same list every time: each
// name is visited, and under -R so is everything under it.
static fn file_change_paths(positive first, positive count, bool recursive,
                            string_address program, b32 address_to status,
                            file_visit visit)
{
        while (first < count)
        {
                string_address path = program_argument((b32)first++);

                if (recursive)
                        file_change_walk(AT_FDCWD, path, path, FILE_MAX_DEPTH,
                                         program, status, visit);
                else
                        visit(AT_FDCWD, path, path);
        }

        log_flush();
}

// Arguments -------------------------------------------------

CONST positive file_letter_bit(p8 letter)
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

COLD fn file_complain(string_address program, string_address message, string_address subject)
{
        string_format(file_fail, "%s: %s: %s\n", program, subject, message);
}

// What every tool says when it was given nothing to work on, and the status
// each of them answers with.
static COLD b32 file_missing(string_address program)
{
        string_format(file_fail, "%s: missing operand\n", program);

        return 1;
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
        bipolar got = system_read_once(0, answer, 1);

        if (got != 1)
                return false;

        bool yes = answer[0] == 'y' || answer[0] == 'Y';

        while (answer[0] != '\n' && system_read_once(0, answer, 1) == 1)
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
typedef named_byte file_long;

/*
        The options that supersede one another.

        -i and -n both say what to do about a collision and the last one
        written is the one that means it; -H, -L and -P all say how far a link
        is followed and likewise. One bit per letter cannot say which came
        last, so each row names a set of letters and the place the last of
        them seen is kept, and the tool reads that place instead of the bits.

        This is the flagless twin of `last` below, which answers the same
        question for the options that carry a value.
*/
typedef struct
{
        string_address letters;
        p8 address_to into;
} file_supersede;

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
        // Long-only optional values keep short namespace flags clusterable.
        string_address long_optional;
        // A later bare occurrence records `bare` without erasing a path.
        string_address sticky_optional;
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

        // Sets of options that supersede one another, each remembered in the
        // place its row names. Filled in as the option is read, before the
        // tool's own `seen` hook is told about it.
        const file_supersede address_to supersedes;

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
        positive bare;
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

// The last letter of each superseding set, remembered where its row says.
static fn file_option_supersede(file_taking address_to taking, p8 letter)
{
        for (positive i = 0; taking->supersedes && taking->supersedes[i].letters; i++)
                if (file_option_among(taking->supersedes[i].letters, letter))
                        address_to taking->supersedes[i].into = letter;
}

static bool file_option_needs(file_taking address_to taking, string_address word)
{
        file_complain(taking->program, "option needs an argument", word);

        return false;
}

static p8 file_long_letter(file_taking address_to taking, string_address name,
                           positive length)
{
        if (!taking->longs || !length)
                return 0;

        p8 candidate = 0;

        for (positive i = 0; taking->longs[i].name; i++)
        {
                string_address spelling = taking->longs[i].name;

                if (string_compare_max(spelling, name, length))
                        continue;

                // Prefer an exact spelling; otherwise accept only one GNU
                // style unambiguous prefix.
                if (string_is(spelling + length, end))
                        return taking->longs[i].value;

                if (candidate)
                        return 0;

                candidate = taking->longs[i].value;
        }

        return candidate;
}

static bool file_take_from(file_taking address_to taking, positive index)
{
        positive count = (positive)program_argument_count();

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
                    (byte_is_digit(string_get(word + 1)) ||
                     string_is(word + 1, '.')))
                        break;

                index++;

                if (taking->digits && byte_is_digit(string_get(word + 1)))
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
                        bool optional = file_option_among(taking->optional,
                                                          letter) ||
                                        file_option_among(taking->long_optional,
                                                          letter);
                        bool valued = file_option_among(taking->valued, letter);

                        if (mark && !optional && !valued)
                        {
                                file_complain(taking->program,
                                              "option does not allow an argument",
                                              word);
                                return false;
                        }

                        taking->flags |= (positive)1 << bit;

                        if (optional)
                        {
                                if (mark)
                                        taking->value[bit] = mark + 1;
                                else
                                {
                                        taking->bare |= (positive)1 << bit;
                                        if (!file_option_among(taking->sticky_optional,
                                                               letter))
                                                taking->value[bit] = null;
                                }
                                taking->last = letter;
                        }
                        else if (valued)
                        {
                                taking->last = letter;

                                if (mark)
                                        taking->value[bit] = mark + 1;
                                else if (index < count)
                                        taking->value[bit] = program_argument((b32)index++);
                                else
                                        return file_option_needs(taking, word);
                        }

                        file_option_supersede(taking, letter);

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

                        file_option_supersede(taking, string_get(letter));

                        if (!spare &&
                            !file_option_among(taking->valued,
                                               string_get(letter)))
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
                        {
                                taking->bare |= (positive)1 << bit;
                                if (!file_option_among(taking->sticky_optional,
                                                       string_get(letter)))
                                        taking->value[bit] = null;
                        }
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

static bool file_take(file_taking address_to taking)
{
        return file_take_from(taking, 1);
}

/*
        Utilities reached through the shell must see the shell's exported
        vector, while the same binary reached through a farm link has only
        the process vector. Keeping that distinction here also gives colour
        policy, PATH lookup and temporary-directory lookup one answer.
*/
PURE string_address env_get(const_string name);
string_address address_to shell_environment();
bool shell_environment_is_initialized();

static string_address address_to file_environment_all()
{
        string_address address_to shell = shell_environment();

        /* Once the shell owns export state, even an intentionally empty
           vector is authoritative and allocation failure must stay visible.
           Before shell startup, farm-linked utilities still use the process
           vector directly. */
        if (shell_environment_is_initialized())
                return shell;

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

typedef struct
{
        file_color_span key;
        file_color_span value;
        bool assigned;
} file_color_entry;

/* LS_COLORS and GREP_COLORS share one key[=value]: record machine.  Looking
   for ':' and '=' together avoids the two full scans each former consumer
   performed, while still exposing bare flags to the one grammar allowing
   them. */
static inline INLINE bool file_color_next(string_address address_to cursor,
                                          file_color_entry address_to entry)
{
        string_address start = address_to cursor;
        string_address split;
        string_address stop;

        if (!start || !string_get(start))
                return false;

        split = string_first_of_set(start, (string_address)":=");
        entry->assigned = split && string_is(split, '=');

        if (entry->assigned)
                stop = string_first_of_or_end(split + 1, ':');
        else
                stop = split ? split : start + string_length(start);

        entry->key = (file_color_span){start,
            (positive)((entry->assigned ? split : stop) - start)};
        entry->value = entry->assigned
                           ? (file_color_span){split + 1,
                               (positive)(stop - split - 1)}
                           : (file_color_span){null, 0};
        address_to cursor = string_get(stop) ? stop + 1 : stop;
        return true;
}

static bipolar file_input_terminal_name(p8 address_to path, positive limit)
{
        if (!stream_is_terminal(0))
                return -ENOTTY;

        bipolar length = system_call_4(
            syscall(readlinkat), AT_FDCWD,
            (positive)(string_address) "/proc/self/fd/0", (positive)path,
            limit - 1);

        if (length >= 0)
                path[length] = end;

        return length;
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

        if (when != FILE_COLOR_AUTO || !stream_is_terminal(1))
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
static PURE file_color_span file_color_value_aliased(string_address table,
                                                     string_address key,
                                                     string_address alias,
                                                     string_address fallback)
{
        file_color_span answer = {fallback, fallback ? string_length(fallback) : 0};
        positive wanted = string_length(key);
        positive alias_length = alias ? string_length(alias) : 0;

        if (!table)
                return answer;

        string_address at = table;
        file_color_entry entry;

        while (file_color_next(address_of at, address_of entry))
        {
                if (entry.assigned &&
                    ((entry.key.length == wanted &&
                      !string_compare_max(entry.key.text, key, wanted)) ||
                     (alias && entry.key.length == alias_length &&
                      !string_compare_max(entry.key.text, alias,
                                          alias_length))))
                        answer = entry.value;
        }

        return answer;
}

static PURE bool file_color_has(string_address table, string_address key)
{
        positive wanted = string_length(key);
        file_color_entry entry;

        while (file_color_next(address_of table, address_of entry))
                if (entry.key.length == wanted &&
                    !string_compare_max(entry.key.text, key, wanted))
                        return true;

        return false;
}

static bool file_color_span_is(file_color_span span, string_address text)
{
        positive length = string_length(text);

        return span.length == length &&
               !string_compare_max(span.text, text, length);
}

static PURE bool file_color_table_valid(string_address table, bool bare_flags)
{
        file_color_entry entry;

        while (file_color_next(address_of table, address_of entry))
                if (!entry.assigned && entry.key.length && !bare_flags)
                        return false;

        return true;
}

static fn file_color_sgr(writer write, file_color_span color)
{
        write((address_any) "\033[", 2);
        write((address_any)color.text, color.length);
        write((address_any) "m", 1);
}

CONST RETURNS_NONNULL string_address file_reason(bipolar code)
{
        if (code < 0)
                code = -code;

        switch (code)
        {
        case ERROR_NO_ENTRY: return (string_address)"No such file or directory";
        case ERROR_NO_PROCESS: return (string_address)"No such process";
        case ERROR_BAD_DESCRIPTOR: return (string_address)"Bad file descriptor";
        case ERROR_NOT_PERMITTED: return (string_address)"Operation not permitted";
        case ERROR_ACCESS: return (string_address)"Permission denied";
        case ERROR_EXISTS: return (string_address)"File exists";
        case ERROR_NOT_DIRECTORY: return (string_address)"Not a directory";
        case ERROR_IS_DIRECTORY: return (string_address)"Is a directory";
        case ERROR_NOT_EMPTY: return (string_address)"Directory not empty";
        case ERROR_INVALID: return (string_address)"Invalid argument";
        case ERROR_NOT_TERMINAL: return (string_address)"Inappropriate ioctl for device";
        case ERROR_CROSS_DEVICE: return (string_address)"Invalid cross-device link";
        case ERROR_ILLEGAL_SEEK: return (string_address)"Illegal seek";
        case ERROR_NAME_TOO_LONG: return (string_address)"File name too long";
        default: return (string_address)"Error";
        }
}

// Copying, removing, making --------------------------------

/*
        One kernel copy, shared by cp and util-linux's copyfilerange.

        Null offsets advance the descriptors; explicit offsets leave them
        alone.  Linux caps an individual transfer below two gigabytes even on
        a 64-bit machine, so callers use this ceiling instead of asking with
        positive_max and making the kernel trim it every time.
*/
#define FILE_KERNEL_COPY_SIZE 0x7ffff000

static bipolar file_copy_range_once(bipolar in, p64 address_to in_offset,
                                    bipolar out, p64 address_to out_offset,
                                    positive length)
{
        return system_call_6(syscall(copy_file_range), (positive)in,
                             (positive)in_offset, (positive)out,
                             (positive)out_offset, length, 0);
}

/* sendfile is the second kernel-copy floor. Unlike copy_file_range it has no
   destination offset, so the caller positions that descriptor before entering
   this loop. Keeping the source offset explicit means sparse extents never
   disturb the descriptor position used to discover the next hole. */
static bipolar file_send_range_once(bipolar in, p64 address_to in_offset,
                                    bipolar out, positive length)
{
        return system_call_4(syscall(sendfile), (positive)out, (positive)in,
                             (positive)in_offset, length);
}

/* Regular copies use the kernel path below. The buffer exists only for a
   filesystem, kernel or seccomp policy that cannot perform range copies. */
#define FILE_TRANSFER_SIZE (FILE_BLOCK * 32)
static p8 file_transfer[FILE_TRANSFER_SIZE];

static bool file_copy_range_fallback(bipolar result)
{
        return result == -ERROR_NOT_PERMITTED || result == -ERROR_CROSS_DEVICE ||
               result == -ERROR_INVALID || result == -ERROR_NO_SYSTEM_CALL ||
               result == -ERROR_NOT_SUPPORTED;
}

static bool file_copy_buffered(bipolar in, bipolar out)
{
        while (1)
        {
                bipolar taken = system_read_retry((positive)in, file_transfer,
                                                   sizeof(file_transfer));

                if (taken < 0)
                        return false;
                if (!taken)
                        return true;
                if (system_write_all((positive)out, file_transfer,
                                     (positive)taken) != (positive)taken)
                        return false;
        }
}

/* Copy one known data extent. A capability miss switches every later extent
   to the buffered path, but never copies the holes between them. */
static bool file_copy_extent(bipolar in, bipolar out, p64 start,
                             positive length, bool address_to range_copy,
                             bool address_to send_copy)
{
        p64 in_offset = start;
        p64 out_offset = start;

        while (length && address_to range_copy)
        {
                positive chunk = length > FILE_KERNEL_COPY_SIZE
                                     ? FILE_KERNEL_COPY_SIZE : length;
                bipolar copied = file_copy_range_once(
                    in, address_of in_offset, out, address_of out_offset, chunk);

                if (copied > 0)
                {
                        length -= (positive)copied;
                        continue;
                }
                if (!copied)
                        return true;
                if (copied == -4)
                        continue;
                if (!file_copy_range_fallback(copied))
                        return false;

                address_to range_copy = false;
        }

        if (length && address_to send_copy)
        {
                if (system_seek(out, out_offset, FILE_SEEK_SET) < 0)
                        return false;

                while (length)
                {
                        positive chunk = length > FILE_KERNEL_COPY_SIZE
                                             ? FILE_KERNEL_COPY_SIZE : length;
                        bipolar copied = file_send_range_once(
                            in, address_of in_offset, out, chunk);

                        if (copied > 0)
                        {
                                out_offset += (positive)copied;
                                length -= (positive)copied;
                                continue;
                        }
                        if (!copied)
                                return true;
                        if (copied == -4)
                                continue;
                        if (!file_copy_range_fallback(copied))
                                return false;

                        address_to send_copy = false;
                        break;
                }
        }

        if (!length)
                return true;
        if (system_seek(in, in_offset, FILE_SEEK_SET) < 0 ||
            system_seek(out, out_offset, FILE_SEEK_SET) < 0)
                return false;

        while (length)
        {
                positive ask = length < sizeof(file_transfer)
                                   ? length : sizeof(file_transfer);
                bipolar taken = system_read_retry((positive)in, file_transfer,
                                                   ask);

                if (taken < 0)
                        return false;
                if (!taken)
                        return true;
                if (system_write_all((positive)out, file_transfer,
                                     (positive)taken) != (positive)taken)
                        return false;

                length -= (positive)taken;
        }

        return true;
}

/*
        copy_file_range alone is not sparse-preserving: on tmpfs a 64 MiB
        image with one four-byte extent becomes 64 MiB of allocated pages.
        SEEK_DATA/SEEK_HOLE keeps the logical layout at syscall granularity;
        only real extents cross copy_file_range, and ftruncate restores a
        trailing hole. Zero-sized procfs files are left to the stream path,
        because their reported size is not their readable length.

        1 means copied, 0 means the filesystem has no extent interface and
        asks for the stream path, -1 means an actual copy failure.
*/
static bipolar file_copy_sparse(bipolar in, bipolar out,
                                file_facts address_to facts)
{
        if ((facts->mode & MODE_FORMAT) != MODE_FILE || !facts->size ||
            facts->size > (p64)b64_max)
                return 0;

        bipolar data = system_seek(in, 0, 3);

        if (data == -ERROR_NO_DEVICE_ADDRESS)
                return system_truncate_handle(out, facts->size) < 0 ? -1 : 1;
        if (data < 0)
                return 0;

        bool range_copy = true;
        bool send_copy = true;

        while ((p64)data < facts->size)
        {
                bipolar hole = system_seek(in, (positive)data, 4);

                if (hole < data)
                        return -1;
                if ((p64)hole > facts->size)
                        hole = (bipolar)facts->size;

                if (!file_copy_extent(in, out, (p64)data,
                                      (positive)(hole - data),
                                      address_of range_copy,
                                      address_of send_copy))
                        return -1;

                data = system_seek(in, (positive)hole, 3);
                if (data == -ERROR_NO_DEVICE_ADDRESS)
                        break;
                if (data < 0)
                        return -1;
        }

        return system_truncate_handle(out, facts->size) < 0 ? -1 : 1;
}

static bool file_copy_contents_open(bipolar from_directory, string_address from,
                                    bipolar to_directory, string_address to,
                                    positive mode, positive flags)
{
        bipolar in = system_open_at(from_directory, from,
                                   FILE_READ);

        if (in < 0)
                return false;

        bipolar out = system_open_at_mode(to_directory, to, flags, mode);

        if (out < 0)
        {
                system_close(in);
                return false;
        }

        file_facts facts;
        bipolar sparse = file_look(in, (string_address)"", AT_EMPTY_PATH,
                                   address_of facts)
                             ? file_copy_sparse(in, out, address_of facts) : 0;
        bool complete = sparse > 0;

        if (!sparse)
        {
                bool range_copy = true;
                bool send_copy = true;

                while (1)
                {
                        bipolar copied = file_copy_range_once(
                            in, null, out, null, FILE_KERNEL_COPY_SIZE);

                        if (copied > 0)
                                continue;
                        if (!copied)
                        {
                                complete = true;
                                break;
                        }
                        if (copied == -4)
                                continue;
                        if (file_copy_range_fallback(copied))
                        {
                                range_copy = false;
                                break;
                        }

                        break;
                }

                while (!complete && !range_copy && send_copy)
                {
                        bipolar copied = file_send_range_once(
                            in, null, out, FILE_KERNEL_COPY_SIZE);

                        if (copied > 0)
                                continue;
                        if (!copied)
                        {
                                complete = true;
                                break;
                        }
                        if (copied == -4)
                                continue;
                        if (file_copy_range_fallback(copied))
                        {
                                send_copy = false;
                                break;
                        }

                        break;
                }

                if (!complete && !range_copy && !send_copy)
                        complete = file_copy_buffered(in, out);
        }

        system_close(in);
        system_close(out);

        return complete;
}

bool file_copy_contents(bipolar from_directory, string_address from,
                        bipolar to_directory, string_address to, positive mode)
{
        return file_copy_contents_open(from_directory, from, to_directory, to,
                                       mode, FILE_WRITE);
}

bool file_make_parents(string_address path, positive mode)
{
        p8 work[FILE_PATH_MAX];
        positive length = string_length(path);

        if (length >= FILE_PATH_MAX)
                return false;

        memory_copy_apart_end(work, path, length);

        for (positive i = 1; i < length; i++)
        {
                if (work[i] != '/')
                        continue;

                work[i] = end;

                bipolar made = system_make_directory_at(AT_FDCWD, work, mode);

                if (made < 0 &&
                    (made != -ERROR_EXISTS || !file_is_directory_through(work)))
                {
                        work[i] = '/';
                        return false;
                }

                work[i] = '/';
        }

        bipolar made = system_make_directory_at(AT_FDCWD, work, mode);

        return made == 0 ||
               (made == -ERROR_EXISTS && file_is_directory_through(work));
}

/*
        SOURCE... DESTINATION, read the way cp and mv both read it.

        -t names the directory to put things in instead of the last operand,
        and -T says the last operand is the thing itself, so the two of them
        cannot both be given; with neither, a lone pair whose right hand is
        not a directory is a rename rather than a move into a directory.

        What is done with each source and destination pair is the whole of
        what cp and mv differ by here, so that is what comes in. False means
        an operand was refused and the caller exits 1; true means the pairs
        were handed over, and the caller's own status says how they went.

        ln is not a third caller: it has a one operand form, its -n makes the
        directory test ask about a link rather than about what the link points
        at, and it says "target is not a directory" where these two say "extra
        operand".
*/
static bool file_source_destination(string_address program, positive first,
                                    positive count, string_address into, bool alone,
                                    fn(address_to pair)(string_address source,
                                                        string_address destination))
{
        if (into && alone)
        {
                string_format(
                    file_fail,
                    "%s: cannot combine --target-directory and --no-target-directory\n",
                    program);
                return false;
        }

        if (first >= count || (!into && first + 1 >= count))
        {
                file_missing(program);
                return false;
        }

        string_address last = into ? into : program_argument((b32)(count - 1));
        positive after = into ? count : count - 1;

        // -T says the destination is the thing itself however many names it
        // has and whatever is already there, which is the one case where a
        // directory on the right is not a directory to put things into.
        if (alone || (!into && count - first == 2 && !file_is_directory_through(last)))
        {
                if (after - first != 1)
                {
                        string_format(file_fail, "%s: extra operand '%s'\n", program,
                                      program_argument((b32)(first + 1)));
                        return false;
                }

                pair(program_argument((b32)first), last);
                log_flush();

                return true;
        }

        if (!file_is_directory_through(last))
        {
                string_format(file_fail, "%s: target '%s' is not a directory\n", program,
                              last);
                return false;
        }

        bool complete = true;

        while (first < after)
        {
                string_address source = program_argument((b32)first++);
                p8 tail[FILE_PATH_MAX];
                p8 destination[FILE_PATH_MAX];

                path_tail_copy(tail, FILE_PATH_MAX, source);

                if (!file_path_join(destination, last, tail))
                {
                        file_too_long(program, (string_address) "cannot create", last,
                                      tail);
                        complete = false;
                        continue;
                }

                pair(source, destination);
        }

        log_flush();

        return complete;
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
        p32 rdev_major;
        p32 rdev_minor;
        bool known;
} ls_entry;

static ls_entry ls_entries[LS_MAX_ENTRIES];
static positive ls_sorted[LS_MAX_ENTRIES];
static positive ls_sort_spare[LS_MAX_ENTRIES];
static positive ls_count;
static p8 ls_arena[LS_ARENA];
static positive ls_used;

static bool ls_long;
static bool ls_columns;
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
static bool ls_escape;
static string_address ls_colors;

static b32 ls_status;
static bool ls_written;
static bool ls_broken;
static b64 ls_now;
static p8 ls_hidden_option;
static p8 ls_order_option;
static string_address ls_program;

static const file_supersede ls_supersedes[] = {
    {(string_address) "aA", address_of ls_hidden_option},
    {(string_address) "tS", address_of ls_order_option},
    {null, null},
};

static fn ls_limit(string_address why)
{
        if (!ls_broken)
        {
                file_fail(ls_program, 0);
                file_fail(": ", 2);
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

        memory_copy_apart(ls_arena + at, name, length + 1);

        ls_used += length + 1;
        address_to where = at;

        return true;
}

static PURE HOT bipolar ls_order(ls_entry address_to left,
                                 ls_entry address_to right)
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

#define ls_index_order(left, right) \
        ls_order(ls_entries + (left), ls_entries + (right))

/* Bottom-up merge sort keeps comparison count at n log n on the full 8192
   entry surface. Only eight-byte indexes move: the old Shell sort moved whole
   ls_entry records repeatedly and still did superlinear extra comparisons on
   reverse/random directories. BSS carries the two small index arrays without
   adding image bytes or startup writes; they are touched only when ls sorts. */
static fn ls_sort()
{
        for (positive i = 0; i < ls_count; i++)
                ls_sorted[i] = i;

        if (ls_count < 2)
                return;

        positive address_to from = array_merge_sort(
            ls_sorted, ls_sort_spare, ls_count, ls_index_order);

        if (from != ls_sorted)
                memory_copy_apart(ls_sorted, from,
                                  ls_count * sizeof(positive));
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

// A character or block device has no size worth a column; the reference ls
// prints its major and minor numbers there instead.
static bool ls_is_device(ls_entry address_to entry)
{
        positive kind = entry->mode & MODE_FORMAT;

        return entry->known && (kind == MODE_CHARACTER || kind == MODE_BLOCK);
}

// -F and -p put a letter after a name saying what it is: the slash for a
// directory that -p asks for on its own, and the rest of them for -F.
static p8 ls_mark(positive mode)
{
        p8 mark = file_kind_of(mode)->mark;

        if (!ls_classify)
                return mark == '/' ? mark : 0;

        if ((mode & MODE_FORMAT) == MODE_FILE && (mode & 0111))
                return '*';

        return mark;
}

/*
        LS_COLORS read once per listing rather than once per entry. The keys
        a kind or a mode can ask for are looked up by index; the suffix
        entries are gathered in the order they were written, because the
        last one that matches a name is the one that colours it. A table
        with more suffixes than the gathering holds is read the slow way,
        entry by entry as before, so nothing about the answer changes.
*/
enum
{
        LS_COLOR_FI,
        LS_COLOR_DI,
        LS_COLOR_TW,
        LS_COLOR_OW,
        LS_COLOR_ST,
        LS_COLOR_LN,
        LS_COLOR_OR,
        LS_COLOR_PI,
        LS_COLOR_CD,
        LS_COLOR_BD,
        LS_COLOR_SO,
        LS_COLOR_SU,
        LS_COLOR_SG,
        LS_COLOR_EX,
        LS_COLOR_RS,
        LS_COLOR_KEYS
};

#define LS_COLOR_SUFFIXES 2048

static const string_address ls_color_keys[LS_COLOR_KEYS] = {
    "fi", "di", "tw", "ow", "st", "ln", "or", "pi",
    "cd", "bd", "so", "su", "sg", "ex", "rs"};
static file_color_span ls_color_table[LS_COLOR_KEYS];
static bool ls_color_set[LS_COLOR_KEYS];
static file_color_entry ls_color_suffixes[LS_COLOR_SUFFIXES];
static positive ls_color_suffix_count;
static bool ls_color_suffix_overflow;

static positive ls_color_index(string_address key)
{
        for (positive i = 0; i < LS_COLOR_KEYS; i++)
                if (!string_compare(ls_color_keys[i], key))
                        return i;

        return LS_COLOR_FI;
}

static fn ls_color_parse()
{
        string_address at = ls_colors;
        file_color_entry entry;

        memory_fill(ls_color_set, 0, sizeof(ls_color_set));
        ls_color_suffix_count = 0;
        ls_color_suffix_overflow = false;

        if (!at)
                return;

        while (file_color_next(address_of at, address_of entry))
        {
                if (!entry.assigned)
                        continue;

                if (string_is(entry.key.text, '*'))
                {
                        if (ls_color_suffix_count < LS_COLOR_SUFFIXES)
                                ls_color_suffixes[ls_color_suffix_count++] = entry;
                        else
                                ls_color_suffix_overflow = true;

                        continue;
                }

                if (entry.key.length != 2)
                        continue;

                for (positive i = 0; i < LS_COLOR_KEYS; i++)
                        if (!string_compare_max(entry.key.text, ls_color_keys[i], 2))
                        {
                                ls_color_table[i] = entry.value;
                                ls_color_set[i] = true;
                                break;
                        }
        }
}

// The colour a key was given, or the one it has when the table says nothing.
static file_color_span ls_color_of(positive key, string_address fallback)
{
        if (ls_color_set[key])
                return ls_color_table[key];

        return (file_color_span){fallback, fallback ? string_length(fallback) : 0};
}

static bool ls_suffix_match(file_color_entry address_to entry, string_address name,
                            positive name_length)
{
        positive suffix = entry->key.length - 1;

        return suffix <= name_length &&
               !string_compare_max(entry->key.text + 1, name + name_length - suffix,
                                   suffix);
}

static file_color_span ls_suffix_color(string_address name)
{
        file_color_span answer = {null, 0};
        positive name_length = string_length(name);

        if (ls_color_suffix_overflow)
        {
                string_address at = ls_colors;
                file_color_entry entry;

                while (file_color_next(address_of at, address_of entry))
                        if (entry.assigned && string_is(entry.key.text, '*') &&
                            ls_suffix_match(address_of entry, name, name_length))
                                answer = entry.value;

                return answer;
        }

        for (positive i = 0; i < ls_color_suffix_count; i++)
                if (ls_suffix_match(address_of ls_color_suffixes[i], name, name_length))
                        answer = ls_color_suffixes[i].value;

        return answer;
}

static file_color_span ls_name_color(string_address directory,
                                     ls_entry address_to entry,
                                     string_address name)
{
        positive mode = entry->mode;
        positive kind = mode & MODE_FORMAT;
        positive key = LS_COLOR_FI;
        string_address fallback = null;

        if (kind == MODE_DIRECTORY)
        {
                key = (mode & 01000) && (mode & 0002) ? LS_COLOR_TW
                      : (mode & 0002)                  ? LS_COLOR_OW
                      : (mode & 01000)                 ? LS_COLOR_ST
                                                       : LS_COLOR_DI;
                fallback = (string_address) ((mode & 01000) && (mode & 0002)
                                                 ? "30;42"
                                             : (mode & 0002) ? "34;42"
                                             : (mode & 01000) ? "37;44"
                                                               : "01;34");
        }
        else if (kind == MODE_LINK)
        {
                key = LS_COLOR_LN;
                fallback = (string_address) "01;36";

                p8 full[FILE_PATH_MAX];
                file_facts through;
                bool fits = true;

                if (directory)
                        fits = file_path_join(full, directory, name);
                else
                        string_copy_max_end(full, name, FILE_PATH_MAX - 1);

                // A link whose path would not fit whole cannot be followed,
                // and is coloured as the orphan it might as well be.
                if (!fits || !file_look_at(full, address_of through))
                {
                        file_color_span orphan = ls_color_of(LS_COLOR_OR, null);

                        return orphan.text ? orphan
                                           : ls_color_of(LS_COLOR_LN, fallback);
                }

                file_color_span link_color = ls_color_of(LS_COLOR_LN, fallback);

                if (file_color_span_is(link_color, (string_address) "target"))
                {
                        ls_entry target = *entry;

                        target.mode = through.mode;
                        return ls_name_color(directory, address_of target, name);
                }

                return link_color;
        }
        else if (file_kind_of(mode)->colour_key)
        {
                key = ls_color_index(file_kind_of(mode)->colour_key);
                fallback = file_kind_of(mode)->colour_fallback;
        }
        else if (mode & 04000)
        {
                key = LS_COLOR_SU;
                fallback = (string_address) "37;41";
        }
        else if (mode & 02000)
        {
                key = LS_COLOR_SG;
                fallback = (string_address) "30;43";
        }
        else
        {
                file_color_span suffix = ls_suffix_color(name);

                if (suffix.text)
                        return suffix;

                if (mode & 0111)
                {
                        key = LS_COLOR_EX;
                        fallback = (string_address) "01;32";
                }
        }

        return ls_color_of(key, fallback);
}

/*
        One byte of a name spelled as an escape, the way -b writes it and the
        way a quoted name on a terminal writes it. The two differ only in
        that -b's C spelling has a letter for the bell and for the backslash
        itself; the quoted form never meets a backslash and writes the bell
        in octal. Everything else is the same table.
*/
static positive ls_escape_byte(p8 byte, p8 address_to into, bool c_style)
{
        into[0] = '\\';
        into[1] = byte == '\n'                ? 'n'
                  : byte == '\t'              ? 't'
                  : byte == '\r'              ? 'r'
                  : byte == '\b'              ? 'b'
                  : byte == '\f'              ? 'f'
                  : byte == '\v'              ? 'v'
                  : c_style && byte == 7      ? 'a'
                  : c_style && byte == '\\'   ? '\\'
                                              : 0;

        if (into[1])
                return 2;

        into[1] = (p8)('0' + ((byte >> 6) & 7));
        into[2] = (p8)('0' + ((byte >> 3) & 7));
        into[3] = (p8)('0' + (byte & 7));

        return 4;
}

static fn ls_name_text(string_address name)
{
        if (ls_escape)
        {
                static b8 safe[STRING_SET_BYTES];
                static bool ready;

                if (!ready)
                {
                        memory_fill(safe + 32, 1, STRING_SET_BYTES - 32);
                        safe['\\'] = 0;
                        safe[127] = 0;
                        ready = true;
                }

                for (positive at = 0; string_get(name + at);)
                {
                        positive plain = string_span(name + at, safe);

                        if (plain)
                        {
                                log(name + at, plain);
                                at += plain;
                        }

                        if (!string_get(name + at))
                                break;

                        p8 escaped[4];

                        log(escaped, ls_escape_byte(string_get(name + at++),
                                                    escaped, true));
                }

                return;
        }

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

                p8 escaped[4];
                positive length = ls_escape_byte(string_get(name + at++),
                                                 escaped, false);

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

        file_color_span reset = ls_color_of(LS_COLOR_RS, (string_address) "0");

        if (!ls_color_started)
        {
                file_color_sgr(log, reset);
                ls_color_started = true;
        }

        file_color_sgr(log, color);
        ls_name_text(name);
        file_color_sgr(log, reset);
}

/* dir's -C presentation counts the bytes its shared name writer will emit.
   The names are byte strings throughout this ls implementation; escaped
   control bytes therefore have the exact two- or four-column spelling below
   without needing a second quoting buffer. */
static positive ls_name_width(string_address name)
{
        if (!ls_escape)
                return string_length(name);

        positive width = 0;

        for (positive i = 0; string_get(name + i); i++)
        {
                p8 byte = string_get(name + i);

                if (byte >= 32 && byte != 127 && byte != '\\')
                        width++;
                else if (byte == '\n' || byte == '\t' || byte == '\r' ||
                         byte == '\b' || byte == '\f' || byte == '\v' ||
                         byte == 7 || byte == '\\')
                        width += 2;
                else
                        width += 4;
        }

        return width;
}

static positive ls_column_limit()
{
        string_address given = file_environment((string_address) "COLUMNS");
        positive width = 80;

        if (given && string_get(given))
        {
                string_address at = given;
                positive parsed;

                if (string_digits_checked(address_of at, 10,
                                           address_of parsed) &&
                    !string_get(at) && parsed)
                        width = parsed;
        }

        return width;
}

static positive ls_column_entry(positive shown)
{
        return ls_reversed ? ls_count - 1 - shown : shown;
}

/* GNU's vertical -C layout: choose the widest number of columns which leaves
   the cursor short of COLUMNS, then fill down those columns.  Widths live in
   ls_sort_spare after sorting has finished, so column mode adds no arena or
   permanent buffer and keeps the directory walk and quoting path shared. */
static fn ls_print_columns(string_address directory)
{
        if (!ls_count)
                return;

        positive limit = ls_column_limit();

        for (positive shown = 0; shown < ls_count; shown++)
        {
                positive sorted = ls_column_entry(shown);
                ls_entry address_to entry = address_of ls_entries[ls_sorted[sorted]];

                ls_sort_spare[shown] = ls_name_width(ls_arena + entry->name);
        }

        positive columns = 1;

        for (positive candidate = ls_count; candidate > 1; candidate--)
        {
                positive rows = (ls_count + candidate - 1) / candidate;
                positive total = 0;
                bool fits = true;

                for (positive column = 0; column < candidate; column++)
                {
                        positive first = column * rows;

                        if (first >= ls_count)
                                break;

                        positive widest = 0;
                        positive after = first + rows;

                        if (after > ls_count)
                                after = ls_count;

                        for (positive shown = first; shown < after; shown++)
                                if (ls_sort_spare[shown] > widest)
                                        widest = ls_sort_spare[shown];

                        if (total > positive_max - widest - 2)
                        {
                                fits = false;
                                break;
                        }

                        total += widest + 2;
                }

                /* No padding follows the final column.  Staying strictly
                   short avoids a terminal's exact-width automatic wrap. */
                if (fits && total >= 2 && total - 2 < limit)
                {
                        columns = candidate;
                        break;
                }
        }

        positive rows = (ls_count + columns - 1) / columns;

        for (positive row = 0; row < rows; row++)
        {
                positive position = 0;

                for (positive column = 0; column < columns; column++)
                {
                        positive shown = column * rows + row;

                        if (shown >= ls_count)
                                continue;

                        positive sorted = ls_column_entry(shown);
                        ls_entry address_to entry =
                            address_of ls_entries[ls_sorted[sorted]];
                        string_address name = ls_arena + entry->name;

                        ls_name_say(directory, entry, name);

                        positive next = (column + 1) * rows + row;

                        if (next < ls_count)
                        {
                                positive widest = 0;
                                positive first = column * rows;
                                positive after = first + rows;

                                if (after > ls_count)
                                        after = ls_count;

                                for (positive item = first; item < after; item++)
                                        if (ls_sort_spare[item] > widest)
                                                widest = ls_sort_spare[item];

                                positive target = position + widest + 2;
                                positive at = position + ls_sort_spare[shown];
                                positive tab = (at + 8) & ~(positive)7;

                                while (!(target & 7) && tab <= target)
                                {
                                        log("\t", 1);
                                        at = tab;
                                        tab += 8;
                                }

                                writer_fill(log, target - at, ' ');
                                position = target;
                        }
                }

                log("\n", 1);
        }
}

static fn ls_print(string_address directory)
{
        if (ls_columns)
        {
                ls_print_columns(directory);
                return;
        }

        positive link_width = 1;
        positive size_width = 1;
        positive owner_width = 1;
        positive group_width = 1;
        positive inode_width = 1;
        positive major_width = 0;
        positive minor_width = 0;
        p64 blocks = 0;

        // An entry the kernel would not describe is a "?" in every column,
        // which is one character wide and so counts for nothing here.
        for (positive i = 0; i < ls_count; i++)
        {
                ls_entry address_to entry = address_of ls_entries[i];

                if (!entry->known)
                        continue;

                if (ls_inode && ls_width_of(entry->inode) > inode_width)
                        inode_width = ls_width_of(entry->inode);

                if (!ls_long)
                        continue;

                if (ls_width_of(entry->links) > link_width)
                        link_width = ls_width_of(entry->links);

                if (ls_is_device(entry))
                {
                        if (ls_width_of(entry->rdev_major) > major_width)
                                major_width = ls_width_of(entry->rdev_major);

                        if (ls_width_of(entry->rdev_minor) > minor_width)
                                minor_width = ls_width_of(entry->rdev_minor);
                }
                else
                {
                        positive entry_size_width = ls_human_width(entry->size);

                        if (entry_size_width > size_width)
                                size_width = entry_size_width;
                }

                p8 name[FILE_NAME_MAX];

                file_account_label(entry->owner, false, !ls_numeric, name);

                if (string_length(name) > owner_width)
                        owner_width = string_length(name);

                file_account_label(entry->group, true, !ls_numeric, name);

                if (string_length(name) > group_width)
                        group_width = string_length(name);
        }

        // The device column is "major, minor", and the size column is wide
        // enough for whichever of the two spellings is the wider.
        positive device_width = major_width + 2 + minor_width;

        if (major_width && device_width > size_width)
                size_width = device_width;

        if (ls_long && directory)
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
                ls_entry address_to entry = address_of ls_entries[ls_sorted[i]];
                string_address name = ls_arena + entry->name;

                if (ls_inode)
                {
                        if (entry->known)
                                positive_to_padded(log, entry->inode, inode_width, ' ', 0);
                        else
                                string_to_field(log, (string_address) "?", inode_width,
                                                ' ', false);

                        log(" ", 1);
                }

                if (ls_long && !entry->known)
                {
                        // What the reference ls prints for an entry it could
                        // not ask about: the kind the directory gave, a
                        // question mark for every bit and every column, and
                        // the time column held at its width.
                        p8 letters[12];

                        letters[0] = (entry->mode & MODE_FORMAT)
                                         ? file_kind_letter(entry->mode)
                                         : '?';
                        memory_fill(letters + 1, '?', 9);
                        log(letters, 10);
                        log(" ", 1);
                        string_to_field(log, (string_address) "?", link_width, ' ', false);
                        log(" ", 1);
                        string_to_field(log, (string_address) "?", owner_width, ' ', true);
                        log(" ", 1);
                        string_to_field(log, (string_address) "?", group_width, ' ', true);
                        log(" ", 1);
                        string_to_field(log, (string_address) "?", size_width, ' ', false);
                        log(" ", 1);
                        string_to_field(log, (string_address) "?", 12, ' ', false);
                        log(" ", 1);
                }
                else if (ls_long)
                {
                        p8 letters[12];
                        p8 who[FILE_NAME_MAX];

                        file_mode_letters(letters, entry->mode);
                        log(letters, 10);
                        log(" ", 1);
                        positive_to_padded(log, entry->links, link_width, ' ', 0);
                        log(" ", 1);

                        file_account_label(entry->owner, false, !ls_numeric, who);
                        string_to_field(log, who, owner_width, ' ', true);
                        log(" ", 1);

                        file_account_label(entry->group, true, !ls_numeric, who);
                        string_to_field(log, who, group_width, ' ', true);
                        log(" ", 1);

                        if (ls_is_device(entry))
                        {
                                // The major number takes whatever the size
                                // column has over the device spelling, so
                                // the comma lines up down the listing.
                                positive_to_padded(log, entry->rdev_major,
                                                   major_width + size_width - device_width,
                                                   ' ', 0);
                                log(", ", 2);
                                positive_to_padded(log, entry->rdev_minor, minor_width,
                                                   ' ', 0);
                        }
                        else
                        {
                                positive field_width = ls_human_width(entry->size);

                                writer_fill(log, size_width > field_width
                                                     ? size_width - field_width
                                                     : 0,
                                            ' ');

                                ls_size_field(entry->size);
                        }

                        log(" ", 1);
                        file_stamp_short(log, entry->modified, ls_now);
                        log(" ", 1);
                }

                ls_name_say(directory, entry, name);

                bool marking = ls_classify || ls_slash;
                bool link = entry->known && (entry->mode & MODE_FORMAT) == MODE_LINK;
                // An entry the kernel would not describe still carries the
                // kind the directory gave, and a kind is all a mark needs
                // short of a regular file's execute bits.
                p8 mark = marking && (entry->known || (entry->mode & MODE_FORMAT))
                              ? ls_mark(entry->mode)
                              : 0;

                // In the long form the arrow is written and the mark goes on
                // what the link points at; on a line of its own the link is
                // the only thing there is to mark.
                if (mark && !(ls_long && link))
                        log(address_of mark, 1);

                if (ls_long && link)
                {
                        p8 where[FILE_PATH_MAX];
                        p8 full[FILE_PATH_MAX];
                        bool fits = true;

                        if (directory)
                                fits = file_path_join(full, directory, name);
                        else
                                string_copy_max_end(full, name, FILE_PATH_MAX - 1);

                        if (!fits)
                        {
                                file_too_long(ls_program,
                                              (string_address) "cannot read symbolic link",
                                              directory, name);
                                ls_status = 1;
                        }
                        else if (file_link_text(full, where, FILE_PATH_MAX) >= 0)
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

/*
        One entry into the listing. An operand has no directory above it and
        its dirent kind is unknown; an entry read out of a directory brings
        both, and when the kernel will not describe it the kind the directory
        gave is what the listing has to go on.

        The reference ls asks the kernel about an entry only when a column or
        an order wants the answer, and a plain listing of a directory whose
        entries cannot be looked at prints their names and says nothing. The
        moment -l, -i, -t, -S, -F or colour is asked for, the failure is
        reported, the entry is printed as unknown, and the status says so; a
        failed operand is a failed operand, and answers 2.
*/
static bool ls_add(bipolar directory, string_address path, string_address shown,
                   p8 type, string_address under)
{
        if (ls_count >= LS_MAX_ENTRIES)
        {
                ls_limit((string_address) "directory has too many entries");
                return false;
        }

        file_facts facts;
        ls_entry address_to entry = address_of ls_entries[ls_count];

        memory_fill(entry, 0, sizeof(ls_entry));

        bipolar looked = file_look_code(directory, path, AT_SYMLINK_NOFOLLOW,
                                        address_of facts);

        if (looked < 0 && !under)
        {
                string_format(file_fail, "%s: cannot access '%s': %s\n",
                              ls_program, shown, file_reason(looked));
                ls_status = 2;
                return true;
        }

        if (!ls_keep(shown, address_of entry->name))
                return false;

        if (looked == 0)
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
                entry->rdev_major = facts.rdev_major;
                entry->rdev_minor = facts.rdev_minor;
        }
        else
        {
                entry->mode = file_mode_from_type(type);

                // The reference ls asks about an entry the directory has
                // already described only when a column needs more than its
                // kind: -F must see a regular file's execute bits, colour
                // must see a directory's sticky and writable bits, and
                // both must see whatever the directory declined to describe.
                positive format = entry->mode & MODE_FORMAT;

                if (ls_long || ls_inode || ls_by_time || ls_by_size ||
                    ((ls_classify || ls_coloring) && !format) ||
                    (ls_classify && format == MODE_FILE) ||
                    (ls_coloring && (format == MODE_FILE ||
                                     format == MODE_DIRECTORY)))
                {
                        p8 full[FILE_PATH_MAX];

                        string_format(file_fail, "%s: cannot access '%s': %s\n",
                                      ls_program,
                                      file_path_join(full, under, shown) ? full : shown,
                                      file_reason(looked));
                        ls_status = 1;
                }
        }

        ls_count++;
        return true;
}

static fn ls_directory(string_address path, bool heading, positive depth,
                       bool named);

// Whether an operand is a directory whose contents are listed. A link to
// one is followed only when nothing asked about the link itself: -l, -d
// and -F all name the link, and the reference ls prints it as one.
static bool ls_operand_lists(string_address path)
{
        return ls_long || ls_classify || ls_as_itself
                   ? file_is_directory(AT_FDCWD, path)
                   : file_is_directory_through(path);
}

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
                ls_entry address_to entry = address_of ls_entries[ls_sorted[i]];

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

                memory_copy_apart(keep + kept, name, length + 1);

                kept += length + 1;
                found++;
        }

        positive at = 0;

        for (positive i = 0; i < found; i++)
        {
                p8 below[FILE_PATH_MAX];
                string_address name = keep + at;

                at += string_length(name) + 1;

                // Each level of -R holds a listing and a block of names on
                // the stack, so a tree that links into itself stops here,
                // and says so, rather than by running out of stack.
                if (depth == 0)
                {
                        string_format(file_fail, "%s: '%s/%s' is nested too deep\n",
                                      ls_program, path, name);
                        ls_status = 1;
                        continue;
                }

                if (!file_path_join(below, path, name))
                {
                        file_too_long(ls_program, (string_address) "cannot open directory",
                                      path, name);
                        ls_status = 1;
                        continue;
                }

                ls_directory(below, true, depth - 1, false);
        }
}

// A directory that will not open is answered with 2 when it was named on
// the command line and 1 when -R met it on the way down, as the reference
// ls answers.
static fn ls_directory(string_address path, bool heading, positive depth,
                       bool named)
{
        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, path))
        {
                string_format(file_fail, "%s: cannot open directory '%s': %s\n",
                              ls_program, path, file_reason(walk.handle));
                ls_status = named ? 2 : 1;
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

                if (!ls_add(walk.handle, entry->d_name, entry->d_name,
                            entry->d_type, path))
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

        if (ls_recursive)
                ls_below(path, depth);
}

static const file_long ls_longs[] = {
    {(string_address) "color", 'C'},
    {null, 0},
};

static b32 file_ls_as(string_address program, bool long_default,
                      bool escape_default, bool column_default)
{
        positive count = (positive)program_argument_count();
        ls_hidden_option = 0;
        ls_order_option = 0;
        ls_program = program;
        ls_escape = escape_default;

        file_taking taking = {
            .program = program,
            .allowed = (string_address) "laARtShr1dinFp",
            .valued = (string_address) "",
            .optional = (string_address) "C",
            .longs = ls_longs,
            .supersedes = ls_supersedes,
        };

        if (!file_take(address_of taking))
                return 2;

        ls_status = 0;
        ls_written = false;
        ls_broken = false;

        positive flags = taking.flags;
        positive first = taking.first;

        ls_now = file_now();

        ls_long = (flags & (FILE_FLAG('l') | FILE_FLAG('n'))) != 0 ||
                  (long_default && !(flags & FILE_FLAG('1')));
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
        ls_terminal = stream_is_terminal(1);
        ls_colors = file_environment((string_address) "LS_COLORS");

        if (ls_colors && string_get(ls_colors) &&
            !file_color_table_valid(ls_colors, false))
        {
                string_format(file_fail,
                              "%s: unparsable value for LS_COLORS environment variable\n",
                              program);
                ls_colors = null;
        }

        if (flags & FILE_FLAG('C'))
        {
                b32 when = file_color_when(file_option_value(address_of taking, 'C'),
                                           FILE_COLOR_ALWAYS);

                if (when < 0)
                {
                        string_format(file_fail,
                                      "%s: invalid argument '%s' for --color\n",
                                      program,
                                      file_option_value(address_of taking, 'C'));
                        return 1;
                }

                ls_coloring = ls_colors && string_get(ls_colors) &&
                              file_color_active(when);
        }

        if (ls_coloring)
                ls_color_parse();

        ls_columns = column_default && !ls_long &&
                     !(flags & FILE_FLAG('1')) && !ls_inode &&
                     !ls_classify && !ls_slash && !ls_coloring;

        /*
                No operand is the working directory: under -d that is the
                one entry ".", as the reference ls prints it, and under -R
                the listing starts with the ".:" heading every directory
                below it gets, so the output reads the same at every level.
        */
        if (first >= count)
        {
                if (ls_as_itself)
                {
                        ls_count = 0;
                        ls_used = 0;

                        if (ls_add(AT_FDCWD, (string_address) ".",
                                   (string_address) ".", 0, null))
                        {
                                ls_sort();
                                ls_print(null);
                        }
                }
                else
                        ls_directory((string_address) ".", ls_recursive,
                                     FILE_MAX_DEPTH, true);

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
                                    program_argument((b32)i), 0, null))
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
                file_facts facts;
                bipolar looked = file_look_code(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW,
                                                address_of facts);

                if (looked < 0)
                {
                        string_format(file_fail, "%s: cannot access '%s': %s\n",
                                      program, path, file_reason(looked));
                        ls_status = 2;
                        continue;
                }

                if (ls_operand_lists(path))
                {
                        directories++;
                        continue;
                }

                if (!ls_add(AT_FDCWD, path, path, 0, null))
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

                if (!file_exists(AT_FDCWD, path) || !ls_operand_lists(path))
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
                             FILE_MAX_DEPTH, true);

        log_flush();

        return ls_status;
}

static b32 file_ls()
{
        return file_ls_as((string_address) "ls", false, false, false);
}

/* GNU dir is the shared ls engine with -C and -b selected by default. */
static b32 file_dir()
{
        return file_ls_as((string_address) "dir", false, true, true);
}

static b32 file_vdir()
{
        return file_ls_as((string_address) "vdir", true, true, false);
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
        path_walk walk = {path, null, 0, false};

        if (string_first_of(name, '/'))
        {
                bipolar answer = system_execute(name, words, environment);

                return answer;
        }

        if (!path)
                walk.at = "/bin:/usr/bin:/";

        // An empty PATH component is the current directory. That includes a
        // completely empty PATH and the component after a trailing colon.
        while (path_walk_next(address_of walk))
        {
                if (!path_walk_join(candidate, FILE_PATH_MAX, walk.segment,
                                    walk.length, name, "."))
                        continue;

                bipolar answer = system_execute(candidate, words, environment);

                if (answer == -ERROR_ARGUMENT_LIST)
                        return answer;

                if (answer == -ERROR_ACCESS)
                        denied = true;
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

        bipolar child = system_fork();

        if (child == 0)
                file_exec_path(words);

        if (child < 0)
                return 127;

        system_wait4_retry(child, address_of status, 0, null);

        if (status & 0x7f)
                return 125;

        return (b32)((status >> 8) & 0xff);
}

// nice -------------------------------------------------------------
#define NICE_PROCESS 0

static bool nice_adjustment(string_address text, bipolar address_to value)
{
        text += string_span(text, string_set_blanks);

        bool below = string_is(text, '-');

        if (below || string_is(text, '+'))
                text++;

        positive used;
        positive magnitude = string_digits_max(
            text, below ? (positive)bipolar_max + 1 : (positive)bipolar_max,
            address_of used);

        if (!used || string_get(text + used))
                return false;

        address_to value = bipolar_from_magnitude(magnitude, below);
        return true;
}

static bool nice_current(bipolar address_to current)
{
        bipolar raw = system_call_2(syscall(getpriority), NICE_PROCESS, 0);

        if (raw < 0)
                return false;

        address_to current = 20 - raw;
        return true;
}

static b32 file_nice()
{
        positive count = (positive)program_argument_count();
        positive first = 1;
        string_address given = null;

        while (first < count)
        {
                string_address word = program_argument((b32)first);
                positive offset = 1 + (string_is(word + 1, '-') ||
                                       string_is(word + 1, '+'));

                if (string_is(word, '-') && byte_is_digit(string_get(word + offset)))
                {
                        given = word + 1;
                        first++;
                        continue;
                }

                if (string_equals(word, "--"))
                {
                        first++;
                        break;
                }

                if (string_is(word, '-') && string_is(word + 1, 'n'))
                {
                        if (string_get(word + 2))
                                given = word + 2;
                        else if (++first < count)
                                given = program_argument((b32)first);
                        else
                        {
                                file_fail("nice: option needs an argument: -n\n", 0);
                                return 125;
                        }

                        first++;
                        continue;
                }

                if (string_is(word, '-') && string_is(word + 1, '-'))
                {
                        string_address name = word + 2;
                        string_address mark = string_first_of(name, '=');
                        positive length = mark ? (positive)(mark - name)
                                               : string_length(name);

                        if (length && length <= string_length("adjustment") &&
                            !string_compare_max(name, "adjustment", length))
                        {
                                if (mark)
                                        given = mark + 1;
                                else if (++first < count)
                                        given = program_argument((b32)first);
                                else
                                {
                                        file_fail("nice: option needs an argument: --adjustment\n",
                                                  0);
                                        return 125;
                                }

                                first++;
                                continue;
                        }
                }

                if (string_is(word, '-') && !string_is(word + 1, end))
                {
                        string_format(file_fail, "nice: invalid option '%s'\n", word);
                        return 125;
                }

                break;
        }

        bipolar adjustment = 10;

        if (given)
        {
                if (!nice_adjustment(given, address_of adjustment))
                {
                        string_format(file_fail, "nice: invalid adjustment '%s'\n",
                                      given);
                        return 125;
                }

                if (adjustment < -39)
                        adjustment = -39;
                else if (adjustment > 39)
                        adjustment = 39;
        }

        if (first >= count)
        {
                if (given)
                {
                        file_fail("nice: a command must be given with an adjustment\n",
                                  0);
                        return 125;
                }

                bipolar current;

                if (!nice_current(address_of current))
                {
                        file_fail("nice: cannot get niceness\n", 0);
                        return 125;
                }

                bipolar_to_string(log, current);
                log("\n", 1);
                log_flush();
                return 0;
        }

        bipolar current;

        if (!nice_current(address_of current))
        {
                file_fail("nice: cannot get niceness\n", 0);
                return 125;
        }

        bipolar wanted = current + adjustment;

        if (wanted < -20)
                wanted = -20;
        else if (wanted > 19)
                wanted = 19;

        bipolar changed = system_call_3(syscall(setpriority), NICE_PROCESS, 0,
                                        (positive)wanted);

        if (changed < 0)
        {
                string_format(file_fail, "nice: cannot set niceness: %s\n",
                              file_reason(changed));

                if (changed != -ERROR_ACCESS && changed != -ERROR_NOT_PERMITTED)
                        return 125;
        }

        string_address address_to words = program_argument_list() + first;

        log_flush();

        bipolar answer = file_exec_path_try(words);

        string_format(file_fail, "nice: '%s': %s\n", words[0],
                      file_reason(answer));
        return answer == -ERROR_NO_ENTRY ? 127 : 126;
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

// -maxdepth alone limits the walk; without it the reference find goes as
// deep as the tree does, and the frame ceiling below is reported as what it
// is rather than passed off as a depth the caller asked for.
static positive find_maximum = positive_max;
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
static bipolar find_parent;
static string_address find_entry;
static bool find_facts_known;
static bool find_facts_follow;

/*
        Name/path predicates and printing need no inode facts at all; -type
        needs only the bits the directory entry gave. Keep the statx lazy
        until a predicate asks for size, ownership, time or another fact.
*/
static bool find_facts_ready()
{
        if (find_facts_known)
                return true;

        bipolar looked = file_look_code(find_parent, find_entry,
                                        find_facts_follow ? 0 : AT_SYMLINK_NOFOLLOW,
                                        find_facts);

        if (looked < 0)
        {
                /* -L follows links that have targets. A dangling link is
                   still an entry and GNU find tests it as a link rather than
                   turning the failed follow into a failed walk. */
                if (find_facts_follow &&
                    file_look(find_parent, find_entry, AT_SYMLINK_NOFOLLOW,
                              find_facts))
                {
                        find_facts_known = true;
                        return true;
                }

                string_format(file_fail, "find: '%s': %s\n", find_path,
                              file_reason(looked));
                find_status = 1;
                return false;
        }

        find_facts_known = true;
        return true;
}

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
        p8 kind = wanted < 128 ? file_kind_from_letter[wanted] : 0;

        return kind && kind == ((mode & MODE_FORMAT) >> 12);
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
        if (!shell_array_room(find_nodes, find_node_room, find_used + 1))
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
                into[i] = (p8)byte_to_lower(string_get(text + i));
                i++;
        }

        into[i] = end;
}

/*
        The overwhelmingly common find patterns are literals, *.suffix and
        prefix.*. Their shape is invariant across the whole walk, so record
        it once and let the hardware-floor bounded compare answer directly.
        Escapes, sets, questions and interior stars retain the full shell
        matcher; this is a strict fast subset, not another glob language.
*/
static fn find_pattern_prepare(find_node address_to node)
{
        string_address pattern = node->text;
        positive length = string_length(pattern);
        positive stars = 0;
        positive star = 0;

        for (positive at = 0; at < length; at++)
        {
                p8 character = string_get(pattern + at);

                if (character == '\\' || character == '?' || character == '[')
                        return;

                if (character == '*')
                {
                        stars++;
                        star = at;
                }
        }

        if (!stars)
        {
                node->comparison = '=';
                node->number = (b64)length;
        }
        else if (stars == 1 && star == 0)
        {
                node->comparison = '$';
                node->number = (b64)(length - 1);
        }
        else if (stars == 1 && star + 1 == length)
        {
                node->comparison = '^';
                node->number = (b64)(length - 1);
        }
}

static bool find_pattern_holds(find_node address_to node, string_address text,
                               bool insensitive)
{
        positive wanted = (positive)node->number;
        positive length;
        string_address pattern = node->text;

        if (!node->comparison)
                return false;

        length = string_length(text);

        if (node->comparison == '=')
        {
                if (length != wanted)
                        return false;
        }
        else
        {
                if (length < wanted)
                        return false;

                if (node->comparison == '$')
                {
                        pattern++;
                        text += length - wanted;
                }
        }

        return insensitive ? memory_compare_ascii_case(pattern, text, wanted) == 0
                           : memory_compare(pattern, text, wanted) == 0;
}

/*
        The predicates that take no value: the word, the node kind it makes,
        and which of the walk's switches it throws on the way past. find_is is
        an exact compare, so no row shadows another and the order here is the
        order they were written in.

        A switch is thrown where the word stands rather than where the
        expression holds, which is how find has always let -depth and -xdev be
        written in the middle of one.
*/
#define FIND_SETS_DEEPEST 1
#define FIND_SETS_ONE_SYSTEM 2
#define FIND_SETS_FOLLOW 4
#define FIND_SETS_ACTION 8

static const struct
{
        string_address name;
        p8 kind;
        p8 sets;
} find_plain[] = {
    {(string_address) "-depth", 'v', FIND_SETS_DEEPEST},
    {(string_address) "-xdev", 'v', FIND_SETS_ONE_SYSTEM},
    {(string_address) "-mount", 'v', FIND_SETS_ONE_SYSTEM},
    {(string_address) "-follow", 'v', FIND_SETS_FOLLOW},
    {(string_address) "-true", 'v', 0},
    {(string_address) "-false", 'f', 0},
    {(string_address) "-print", 'd', FIND_SETS_ACTION},
    {(string_address) "-print0", '0', FIND_SETS_ACTION},
    {(string_address) "-delete", 'D', FIND_SETS_ACTION | FIND_SETS_DEEPEST},
    {(string_address) "-prune", 'r', 0},
    {(string_address) "-quit", 'q', 0},
    {(string_address) "-empty", 'y', 0},
    {(string_address) "-nouser", 'U', 0},
    {(string_address) "-nogroup", 'G', 0},
    {null, 0, 0},
};

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

        for (positive i = 0; find_plain[i].name; i++)
        {
                if (!find_is(word, find_plain[i].name))
                        continue;

                if (find_plain[i].sets & FIND_SETS_DEEPEST)
                        find_deepest = true;

                if (find_plain[i].sets & FIND_SETS_ONE_SYSTEM)
                        find_one_system = true;

                if (find_plain[i].sets & FIND_SETS_FOLLOW)
                        find_follow = true;

                if (find_plain[i].sets & FIND_SETS_ACTION)
                        find_has_action = true;

                return find_make(find_plain[i].kind);
        }

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

                        if (!array_store_reserve(
                                find_batches, find_batch_room, find_batch_have,
                                find_batch_have + 1, 2))
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
                {
                        // The expression is built once and tested for every
                        // directory entry. Case-insensitive patterns are
                        // invariant, so fold their private argv string here
                        // instead of copying and folding it on every visit.
                        if (kind == 'N' || kind == 'P')
                                find_lowered(value, (p8 address_to)value);

                        find_nodes[node].text = value;
                        find_pattern_prepare(address_of find_nodes[node]);
                }

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
                        bipolar looked = file_look_code(AT_FDCWD, value, 0,
                                                        address_of facts);

                        if (looked < 0)
                        {
                                string_format(file_fail, "find: '%s': %s\n", value,
                                              file_reason(looked));
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

        if (!shell_array_room(find_exec_words, find_exec_word_room, template + batch->words + 1))
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

        if (!shell_array_room(find_exec_words, find_exec_word_room, words + 1))
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
                if (node->comparison)
                        return find_pattern_holds(node, find_name, false);
                return shell_match(node->text, find_name);

        case 'p':
                if (node->comparison)
                        return find_pattern_holds(node, find_path, false);
                return shell_match(node->text, find_path);

        case 'N':
        case 'P':
                if (node->comparison)
                        return find_pattern_holds(node,
                                                  node->kind == 'N' ? find_name
                                                                    : find_path,
                                                  true);

                find_lowered(node->kind == 'N' ? find_name : find_path, name);
                return shell_match(node->text, name);

        case 'L':
                if (!find_facts_ready())
                        return false;

                if ((find_facts->mode & MODE_FORMAT) != MODE_LINK)
                        return false;

                if (file_link_text(find_path, name, FILE_PATH_MAX) < 0)
                        return false;

                return shell_match(node->text, name);

        case 't':
                return find_type_holds((p8)node->number, find_facts->mode);

        case 'z':
                if (!find_facts_ready())
                        return false;
                return find_size_holds(node, find_facts);

        case 'y':
                if (!find_facts_ready())
                        return false;
                return find_empty(find_path, find_facts);

        case 'm':
                if (!find_facts_ready())
                        return false;

                if (node->comparison == '-')
                        return ((positive)find_facts->mode & (positive)node->number) ==
                               (positive)node->number;

                if (node->comparison == '/')
                        return ((positive)find_facts->mode & (positive)node->number) != 0;

                return (find_facts->mode & 07777) == (positive)node->number;

        case 'u':
                if (!find_facts_ready())
                        return false;
                return find_facts->owner == (positive)node->number;

        case 'g':
                if (!find_facts_ready())
                        return false;
                return find_facts->group == (positive)node->number;

        case 'U':
                if (!find_facts_ready())
                        return false;
                return !file_user_name(find_facts->owner, name, FILE_NAME_MAX);

        case 'G':
                if (!find_facts_ready())
                        return false;
                return !file_group_name(find_facts->group, name, FILE_NAME_MAX);

        case 'k':
                if (!find_facts_ready())
                        return false;
                return find_holds_count(node->comparison, find_facts->hard_links,
                                        node->number);

        case 'i':
                if (!find_facts_ready())
                        return false;
                return find_holds_count(node->comparison, (b64)find_facts->inode,
                                        node->number);

        case 'T':
                if (!find_facts_ready())
                        return false;
                return find_holds_count(node->comparison,
                                        find_age(node->unit, node->extra), node->number);

        case 'w':
                if (!find_facts_ready())
                        return false;

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
                // Removed by the directory it sits in and its own name, as
                // rm does, so a path that has grown past what the walk can
                // spell is not what gets unlinked. A kind the directory
                // entry did not give is asked of the kernel first, because
                // a directory removed as a file is refused and a file
                // removed as a directory is too.
                if (!(find_facts->mode & MODE_FORMAT) && !find_facts_ready())
                        return false;

                bipolar gone = system_remove_at(
                    find_parent, find_entry,
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

static fn find_walk(string_address path, string_address name, positive depth, bool named,
                    bipolar parent, string_address entry, p8 type)
{
        file_facts facts;
        bool follow = find_follow || (find_follow_named && named);

        if (find_quit)
                return;

        memory_fill(address_of facts, 0, sizeof(facts));
        facts.mode = file_mode_from_type(type);

        find_path = path;
        find_name = name;
        find_facts = address_of facts;
        find_depth = depth;
        find_parent = parent;
        find_entry = entry;
        find_facts_known = false;
        find_facts_follow = follow;
        find_pruned = false;

        /* Roots have no dirent hint. Unknown types and followed links also
           need the kernel's answer before descent can be decided. */
        if ((named || !facts.mode || (follow && type == DT_LNK)) &&
            !find_facts_ready())
        {
                return;
        }

        if (named)
                find_device = file_device_key(facts.device_major, facts.device_minor);

        bool directory = (facts.mode & MODE_FORMAT) == MODE_DIRECTORY;
        bool wanted = depth >= find_minimum && depth <= find_maximum;

        if (!find_deepest && wanted)
                find_true(find_root);

        bool descend = directory && depth < find_maximum && !find_pruned && !find_quit;

        if (descend && find_one_system &&
            (!find_facts_ready() ||
             file_device_key(facts.device_major, facts.device_minor) != find_device))
                descend = false;

        bool facts_known_here = find_facts_known;

        if (descend)
        {
                file_walk walk;

                if (!file_walk_open(address_of walk, parent, entry))
                {
                        string_format(file_fail, "find: '%s': %s\n", path,
                                      file_reason(walk.handle));
                        find_status = 1;
                }
                else
                {
                        struct linux_dirent64 address_to entry;

                        while (!find_quit && (entry = file_walk_next(address_of walk)))
                        {
                                if (file_is_dot(entry->d_name))
                                        continue;

                                // Every level spends a descriptor and a frame
                                // with a getdents block in it, which is what
                                // a tree this deep would run out of; the
                                // first entry found past the ceiling is
                                // what says the walk is not complete.
                                if (depth >= FILE_MAX_DEPTH)
                                {
                                        string_format(file_fail,
                                                      "find: '%s' is nested too deep\n",
                                                      path);
                                        find_status = 1;
                                        break;
                                }

                                p8 below[FILE_PATH_MAX];
                                p8 held[FILE_NAME_MAX];

                                string_copy_max_end(held, entry->d_name,
                                                    FILE_NAME_MAX - 1);

                                if (!file_path_join(below, path, held))
                                {
                                        file_too_long((string_address) "find",
                                                      (string_address) "cannot access",
                                                      path, held);
                                        find_status = 1;
                                        continue;
                                }

                                find_walk(below, held, depth + 1, false,
                                          walk.handle, held, entry->d_type);
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
                find_parent = parent;
                find_entry = entry;
                find_facts_known = facts_known_here;
                find_facts_follow = follow;

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
        find_maximum = positive_max;
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
                find_walk((string_address) ".", (string_address) ".", 0, true,
                          AT_FDCWD, (string_address) ".", 0);
        else
                for (positive i = roots_first; i < roots_last && !find_quit; i++)
                {
                        string_address root = program_argument((b32)i);
                        p8 name[FILE_PATH_MAX];

                        path_tail_copy(name, FILE_PATH_MAX, root);
                        find_walk(root, name, 0, true, AT_FDCWD, root, 0);
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

/*
        A stat format, walked once: everything up to the next % goes out as it
        stands and the letter after it is handed to the tool's own specifier.
        stat and stat -f differ only in what a letter means and in what they
        read it from, so both of those come in and neither walker is written
        twice.
*/
static fn stat_percent_walk(string_address format, string_address path,
                            address_any facts,
                            fn(address_to one)(p8 letter, string_address path,
                                               address_any facts))
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
                        one(string_get(mark + 1), path, facts);
                        step = mark + 2;
                        continue;
                }

                log("%", 1);
                break;
        }

        log("\n", 1);
}

static fn statfs_one_specifier(p8 letter, string_address path, address_any given)
{
        file_mount_facts address_to facts = given;

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

static fn stat_one_specifier(p8 letter, string_address path, address_any given)
{
        file_facts address_to facts = given;
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
                file_account_label(facts->owner, false, true, text);
                return log(text, 0);

        case 'G':
                file_account_label(facts->group, true, true, text);
                return log(text, 0);

        case 'o':
                return positive_to_string(log, facts->blocksize);

        case 'd':
                return positive_to_string(log, file_device(facts->device_major,
                                                           facts->device_minor));

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

        file_account_label(facts->owner, false, true, text);
        string_to_field(log, text, 8, ' ', false);

        log(")   Gid: (", 0);
        positive_to_padded(log, facts->group, 5, ' ', 0);
        log("/", 1);

        file_account_label(facts->group, true, true, text);
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
                return file_missing((string_address) "stat");

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
                                stat_percent_walk(format, path, address_of facts,
                                                  statfs_one_specifier);
                        else
                                statfs_readable(path, address_of facts);

                        continue;
                }

                file_facts facts;
                bipolar looked = file_look_code(AT_FDCWD, path,
                                                stat_follow ? 0 : AT_SYMLINK_NOFOLLOW,
                                                address_of facts);

                if (looked < 0)
                {
                        string_format(file_fail, "stat: cannot statx '%s': %s\n",
                                      path, file_reason(looked));
                        stat_status = 1;
                        continue;
                }

                if (format)
                        stat_percent_walk(format, path, address_of facts,
                                          stat_one_specifier);
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

        p64 device = file_device_key(facts->device_major, facts->device_minor);
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
        bipolar looked = file_look_code(AT_FDCWD, path,
                                        du_follow ? 0 : AT_SYMLINK_NOFOLLOW,
                                        address_of facts);

        if (looked < 0)
        {
                string_format(file_fail, "du: cannot access '%s': %s\n", path,
                              file_reason(looked));
                du_status = 1;
                du_was_directory = false;
                return 0;
        }

        du_was_directory = (facts.mode & MODE_FORMAT) == MODE_DIRECTORY;

        if (named)
                du_device = file_device_key(facts.device_major, facts.device_minor);
        else if (du_one_system &&
                 file_device_key(facts.device_major, facts.device_minor) != du_device)
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
        file_walk walk;

        if (file_walk_open(address_of walk, AT_FDCWD, path))
        {
                struct linux_dirent64 address_to entry;

                while ((entry = file_walk_next(address_of walk)))
                {
                        if (file_is_dot(entry->d_name))
                                continue;

                        // Out of depth is answered by the first entry there
                        // is, before an exclusion could hide it: a tree this
                        // deep has not been measured and saying so is the
                        // whole of what is left to do here.
                        if (depth == 0)
                        {
                                file_fail("du: tree is nested too deep\n", 0);
                                du_depth_broken = true;
                                du_status = 1;
                                break;
                        }

                        p8 under[FILE_PATH_MAX];

                        if (!file_path_join(under, path, entry->d_name))
                        {
                                file_too_long((string_address) "du",
                                              (string_address) "cannot access", path,
                                              entry->d_name);
                                du_status = 1;
                                continue;
                        }

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
        else if (depth > 0)
        {
                // A directory that will not open at the bottom of the walk is
                // not complained about, because nothing was going to be read
                // out of it either way.
                string_format(file_fail, "du: cannot read directory '%s'\n", path);
                du_status = 1;
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

static const file_supersede du_supersedes[] = {
    {(string_address) "bkm", address_of du_unit_option},
    {null, null},
};

static bool du_exclude_seen(p8 letter, string_address value)
{
        if (letter != 'e' || !value)
                return true;

        if (!shell_array_room(du_excludes, du_exclude_room, du_exclude_have + 1))
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
            .supersedes = du_supersedes,
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

        The mounted filesystems come from the shared mountinfo table, because
        the kernel is the only thing that knows what this namespace can see.
        A filesystem with no blocks at all is one of the kernel's own
        bookkeeping mounts and is left out unless -a asks for it, the way df
        has always left it out.

        Each filesystem is measured once and its facts stay beside the parsed
        record while widths and rows are produced. Path operands use statx's
        mount ID, so one kernel answer replaces a search by filesystem traits.
*/
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

typedef struct
{
        file_mount_facts facts;
        bool eligible;
        bool shown;
        bool measured;
} df_sample;

static df_sample address_to df_samples;
static positive df_sample_room;

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
        // An operand that could not be measured is a failure df answers
        // with, after the table for the rest.
        bool df_failed = false;
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

        storage_mount_table mounts;

        if (!storage_mount_table_load(address_of mounts, null))
        {
                file_fail("df: cannot read mount table\n", 0);
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

        if (!array_store_reserve(df_samples, df_sample_room, 0, mounts.count,
                                 32))
        {
                storage_mount_table_release(address_of mounts);
                file_fail("df: out of memory\n", 0);
                return 1;
        }

        memory_fill(df_samples, 0, mounts.count * sizeof(*df_samples));

        for (positive at = 0; at < mounts.count; at++)
        {
                storage_mount address_to mount = mounts.entry + at;
                df_sample address_to sample = df_samples + at;
                file_mount_facts address_to facts = address_of sample->facts;
                string_address where = mount->target;
                string_address type = mount->type;

                /* Asking an autofs mount for its facts would mount it. */
                bipolar answered = string_compare(type, "autofs") == 0
                                       ? -ERROR_ACCESS
                                       : system_call_2(syscall(statfs),
                                                       (positive)where,
                                                       (positive)facts);

                if (answered < 0 && answered != -ERROR_ACCESS)
                {
                        string_format(file_fail, "df: %s: %s\n", where,
                                      file_reason(answered));
                        df_failed = true;
                        continue;
                }

                sample->measured = answered >= 0;

                if (!sample->measured && !df_all)
                        continue;

                if (!facts->blocks && !df_all)
                        continue;

                sample->eligible = true;
                sample->shown = !filtering;
        }

        /* statx supplies the mount ID directly. Each operand is therefore one
           syscall and one exact table match, including stacked/bind mounts. */
        if (filtering)
                for (positive i = first; i < count; i++)
                {
                        string_address path = program_argument((b32)i);
                        file_facts wanted;
                        bipolar answered;

                        memory_fill(address_of wanted, 0, sizeof(wanted));
                        answered = system_stat_at(AT_FDCWD, path,
                                                  AT_NO_AUTOMOUNT,
                                                  STATX_WANTED,
                                                  address_of wanted);

                        if (answered < 0)
                        {
                                string_format(file_fail, "df: %s: %s\n", path,
                                              file_reason(answered));
                                df_failed = true;
                                continue;
                        }

                        /* The last record is the visible top of a stacked
                           mount, just as storage_mount_find_target chooses. */
                        for (positive at = mounts.count; at; at--)
                                if (mounts.entry[at - 1].id == wanted.mount_id)
                                {
                                        df_samples[at - 1].shown =
                                            df_samples[at - 1].eligible;
                                        break;
                                }
                }

        for (positive at = 0; at < mounts.count; at++)
        {
                storage_mount address_to mount = mounts.entry + at;
                df_sample address_to sample = df_samples + at;
                file_mount_facts address_to facts = address_of sample->facts;
                string_address device = mount->source;
                string_address type = mount->type;
                p8 text[64];

                if (!sample->shown)
                        continue;

                if (string_length(device) > df_device_width)
                        df_device_width = string_length(device);

                if (sample->measured && string_length(type) > df_type_width)
                        df_type_width = string_length(type);

                if (!sample->measured)
                        continue;

                p64 total, used, spare, size;

                df_reading(facts, address_of total, address_of used,
                           address_of spare, address_of size);
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

        string_to_field(log, (string_address) "Filesystem", df_device_width,
                        ' ', true);
        log(" ", 1);

        if (df_types)
        {
                string_to_field(log, (string_address) "Type", df_type_width,
                                ' ', true);
                log(" ", 1);
        }

        df_column(blocks_heading, df_blocks_width);
        df_column(used_heading, df_used_width);
        df_column(free_heading, df_free_width);
        log(full_heading, 0);
        log(" Mounted on\n", 0);

        for (positive at = 0; at < mounts.count; at++)
                if (df_samples[at].shown)
                        df_row(mounts.entry[at].source,
                               mounts.entry[at].type,
                               mounts.entry[at].target,
                               address_of df_samples[at].facts,
                               df_samples[at].measured);

        storage_mount_table_release(address_of mounts);
        log_flush();

        return df_failed ? 1 : 0;
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
static positive chmod_umask;
static bool chmod_surprising;

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

static fn chmod_one(bipolar directory, string_address name, string_address shown)
{
        file_facts facts;
        // A name on the command line is followed, because Linux has no mode
        // on a symlink of its own to change and chmod has always meant the
        // thing pointed at. A link met under -R is not: the walk refuses to
        // descend into one, and it must refuse to change through one too,
        // or chmod -R 000 over a tree with a link to /etc in it changes /etc.
        bool operand = directory == AT_FDCWD;
        bipolar looked = file_look_code(directory, name,
                                        operand ? 0 : AT_SYMLINK_NOFOLLOW,
                                        address_of facts);

        if (looked < 0)
        {
                if (!chmod_quiet)
                        string_format(file_fail, "chmod: cannot access '%s': %s\n",
                                      shown, file_reason(looked));

                chmod_status = 1;
                return;
        }

        if (!operand && (facts.mode & MODE_FORMAT) == MODE_LINK)
                return;

        bool directory_mode = (facts.mode & MODE_FORMAT) == MODE_DIRECTORY;
        positive wanted = chmod_reference_mode & 07777;
        positive naive = wanted;

        if (!chmod_referenced &&
            (!file_mode_masked(chmod_specification, facts.mode, directory_mode,
                               chmod_umask, address_of wanted) ||
             !file_mode_of(chmod_specification, facts.mode, directory_mode,
                           address_of naive)))
        {
                if (!chmod_quiet)
                        string_format(file_fail, "chmod: invalid mode: %s\n",
                                      chmod_specification);

                chmod_status = 1;
                return;
        }

        bipolar done = system_change_mode_at(directory, name, wanted);

        if (done < 0)
        {
                if (!chmod_quiet)
                        string_format(file_fail, "chmod: changing permissions of '%s': %s\n",
                                      shown, file_reason(done));

                chmod_status = 1;
                return;
        }

        chmod_said(shown, facts.mode, wanted | (facts.mode & MODE_FORMAT));

        // A "-w" under a umask of 022 takes write away from the owner alone
        // and leaves the group and others as they were. The mode was set as
        // asked, and the reference chmod then says what it did rather than
        // what the mode looked like it asked for, and answers 1 -- but only
        // for a mode given as that kind of word, which is the one that reads
        // like an option and surprises.
        if (chmod_surprising && (wanted & ~naive))
        {
                p8 set[12];
                p8 expected[12];

                file_mode_letters(set, wanted);
                file_mode_letters(expected, naive);
                string_format(file_fail, "chmod: %s: new permissions are %s, not %s\n",
                              shown, set + 1, expected + 1);
                chmod_status = 1;
        }
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
            .allowed = (string_address) "RcfvrwxXstugoa",
            .valued = (string_address) "e",
            .optional = (string_address) "rwxXstugoa",
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
                bipolar looked = file_look_code(AT_FDCWD, like, 0, address_of facts);

                if (looked < 0)
                {
                        string_format(file_fail,
                                      "chmod: failed to get attributes of '%s': %s\n",
                                      like, file_reason(looked));
                        return 1;
                }

                chmod_referenced = true;
                chmod_reference_mode = facts.mode;
        }

        /*
                "chmod -w file" is a mode and not an option, and the reference
                chmod reads any word that begins with a minus and a mode
                letter as the whole mode. The letters are taken as options
                whose value is the rest of the word, so the mode is put back
                together from the letter and what followed it.
        */
        static p8 chmod_taken[4];
        string_address minus_mode = null;

        for (string_address letter = (string_address) "rwxXstugoa"; string_get(letter); letter++)
        {
                if (!(taking.flags & FILE_FLAG(string_get(letter))))
                        continue;

                string_address rest = file_option_value(address_of taking, string_get(letter));

                if (rest)
                        minus_mode = rest - 2;
                else
                {
                        chmod_taken[0] = '-';
                        chmod_taken[1] = string_get(letter);
                        chmod_taken[2] = end;
                        minus_mode = chmod_taken;
                }

                break;
        }

        if (minus_mode && chmod_referenced)
        {
                string_format(file_fail, "chmod: invalid mode: '%s'\n", minus_mode);
                return 1;
        }

        chmod_surprising = minus_mode != null;

        if (minus_mode)
        {
                if (first >= count)
                        return file_missing((string_address) "chmod");

                chmod_specification = minus_mode;
        }
        else if (first >= count || (!chmod_referenced && first + 1 >= count))
                return file_missing((string_address) "chmod");
        else if (!chmod_referenced)
                chmod_specification = program_argument((b32)first++);

        chmod_umask = file_umask();

        file_change_paths(first, count, (taking.flags & FILE_FLAG('R')) != 0,
                          (string_address) "chmod", address_of chmod_status,
                          chmod_one);

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

static const file_supersede chown_supersedes[] = {
    {(string_address) "dh", address_of chown_dereference_option},
    {null, null},
};

// Who a file will belong to, said the way chown says it: the user alone when
// only a user was named, and user:group when a group was.
static fn chown_who(positive user, positive group, p8 address_to into)
{
        if (chown_groups_only)
        {
                file_account_label(group, true, true, into);
                return;
        }

        file_account_label(user, false, true, into);

        if (chown_group < 0)
                return;

        positive length = string_length(into);

        into[length++] = ':';
        file_account_label(group, true, true, into + length);
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

        bipolar done = system_change_owner_at(
            directory, name, chown_user, chown_group, through);

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
        file_change_paths(first, count, (chown_flags & FILE_FLAG('R')) != 0,
                          chown_program, address_of chown_status, chown_one);
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
            .supersedes = chown_supersedes,
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
                bipolar looked = file_look_code(AT_FDCWD, like, 0, address_of facts);

                if (looked < 0)
                {
                        string_format(file_fail,
                                      "%s: failed to get attributes of '%s': %s\n",
                                      program, like, file_reason(looked));
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

static const file_supersede ln_supersedes[] = {
    {(string_address) "fi", address_of ln_collision_option},
    {(string_address) "LP", address_of ln_dereference_option},
    {null, null},
};

// realpath's, and named here because ln is written before it.
static bool realpath_relative(string_address from, string_address path,
                              p8 address_to into);

// -r says where the target is from where the link will sit rather than from
// here, which is the only spelling of a symbolic link that survives the whole
// tree being moved somewhere else.
static bool ln_relative_text(string_address target, string_address name,
                             p8 address_to into)
{
        p8 there[FILE_PATH_MAX];
        p8 head[FILE_PATH_MAX];
        p8 above[FILE_PATH_MAX];

        // The link's own last component is left alone, because it may be a
        // link that -f is about to replace; the directory it sits in is
        // followed all the way, because the text written into the link is
        // read from where the directory really is and not from what it was
        // called on the command line.
        path_head_copy(head, FILE_PATH_MAX, name);

        if (!file_resolve(target, there, true) ||
            !file_resolve(head, above, true))
                return false;

        return realpath_relative(above, there, into);
}

static bool ln_make(string_address target, string_address name)
{
        p8 relative[FILE_PATH_MAX];

        if (ln_relative && ln_symbolic)
        {
                if (!ln_relative_text(target, name, relative))
                {
                        string_format(file_fail,
                                      "ln: cannot make relative target '%s': File name too long\n",
                                      target);
                        return false;
                }

                target = relative;
        }

        if (ln_ask && file_exists(AT_FDCWD, name) &&
            !file_ask((string_address) "ln", (string_address) "replace", name))
                return false;

        /*
                A hard link needs its source to be there before the
                destination is given up: unlinking first and linking second
                left "ln -f missing keep" with neither, and "ln -f a a" with
                nothing at all. The source is looked at first, as the
                reference ln looks, and a destination that is the source is
                refused rather than removed.
        */
        if (!ln_symbolic && (ln_force || ln_ask))
        {
                file_facts source;
                file_facts destination;
                bipolar looked = file_look_code(AT_FDCWD, target,
                                                ln_through ? 0 : AT_SYMLINK_NOFOLLOW,
                                                address_of source);

                if (looked < 0)
                {
                        string_format(file_fail,
                                      "ln: failed to access '%s': %s\n", target,
                                      file_reason(looked));
                        return false;
                }

                if (file_look(AT_FDCWD, name, AT_SYMLINK_NOFOLLOW,
                              address_of destination) &&
                    file_same_identity(address_of source,
                                       address_of destination))
                {
                        string_format(file_fail,
                                      "ln: '%s' and '%s' are the same file\n",
                                      target, name);
                        return false;
                }
        }

        if (ln_force || ln_ask)
                system_remove_at(AT_FDCWD, name, 0);

        bipolar done;

        if (ln_symbolic)
                done = system_symbolic_link_at(target, AT_FDCWD, name);
        else
                done = system_link_at(AT_FDCWD, target, AT_FDCWD, name,
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
            .supersedes = ln_supersedes,
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
                return file_missing((string_address) "ln");

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

                if (!file_path_join(name, last, tail))
                {
                        file_too_long((string_address) "ln",
                                      (string_address) "failed to create link", last,
                                      tail);
                        status = 1;
                        continue;
                }

                if (!ln_make(target, name))
                        status = 1;
        }

        log_flush();

        return status;
}

// link / unlink ------------------------------------------------------
/* The single-purpose POSIX interfaces are deliberately narrower than ln and
   rm: no collision policy, directory traversal or symlink dereference. */
static string_address file_simple_operand_list[3];
static positive file_simple_operand_count;

static b32 address_to file_operand_list;
static positive file_operand_count;
static positive file_operand_room;
static bool file_operand_failed;

static fn file_simple_operand(b32 index)
{
        if (file_simple_operand_count < 3)
                file_simple_operand_list[file_simple_operand_count] =
                    program_argument(index);
        file_simple_operand_count++;
}

static bool file_simple_operands(string_address program, positive wanted)
{
        file_simple_operand_count = 0;
        file_taking taking = {
            .program = program, .allowed = (string_address)"",
            .valued = (string_address)"", .operand = file_simple_operand,
        };

        if (!file_take(address_of taking))
                return false;
        if (file_simple_operand_count == wanted)
                return true;

        string_format(file_fail, "%s: %s\n", program,
                      file_simple_operand_count < wanted
                          ? (string_address)"missing operand"
                          : (string_address)"too many operands");
        return false;
}

static fn file_operand(b32 index)
{
        if (file_operand_failed)
                return;

        if (!shell_array_room(file_operand_list, file_operand_room, file_operand_count + 1))
        {
                file_operand_failed = true;
                return;
        }

        file_operand_list[file_operand_count++] = index;
}

static fn file_operands_begin()
{
        file_operand_count = 0;
        file_operand_failed = false;
}

static string_address file_operand_at(positive index)
{
        return program_argument(file_operand_list[index]);
}

static b32 file_link()
{
        if (!file_simple_operands((string_address)"link", 2))
                return 1;

        string_address source = file_simple_operand_list[0];
        string_address target = file_simple_operand_list[1];
        bipolar answer = system_link_at(AT_FDCWD, source, AT_FDCWD, target, 0);

        if (answer < 0)
        {
                string_format(file_fail,
                              "link: cannot create link '%s' to '%s': %s\n",
                              target, source, file_reason(answer));
                return 1;
        }
        return 0;
}

static b32 file_unlink()
{
        if (!file_simple_operands((string_address)"unlink", 1))
                return 1;

        string_address path = file_simple_operand_list[0];
        bipolar answer = system_remove_at(AT_FDCWD,
                                       path, 0);

        if (answer < 0)
        {
                string_format(file_fail, "unlink: cannot unlink '%s': %s\n",
                              path, file_reason(answer));
                return 1;
        }
        return 0;
}

// namei ---------------------------------------------------------------

/* namei reports the walk rather than only its final answer.  The statx and
   readlink operations remain the same shared metadata path used by stat,
   find and readlink; this layer only remembers the component rows so owner
   and group columns can be aligned without walking the filesystem twice. */
#define NAMEI_LINK_LIMIT 20
typedef struct
{
        file_facts facts;
        string_address reason;
        positive name_at;
        positive target_at;
        positive depth;
        bool known;
        bool mountpoint;
} namei_row;

static namei_row address_to namei_rows;
static positive namei_row_count;
static positive namei_row_room;
static p8 address_to namei_text;
static positive namei_text_used;
static positive namei_text_room;
static string_address namei_operand;
static bool namei_no_symlinks;
static bool namei_mounts;

static const file_long namei_longs[] = {
    {(string_address)"long", 'l'},
    {(string_address)"modes", 'm'},
    {(string_address)"owners", 'o'},
    {(string_address)"mountpoints", 'x'},
    {(string_address)"nosymlinks", 'n'},
    {(string_address)"vertical", 'v'},
    {(string_address)"help", 'h'},
    {(string_address)"version", 'V'},
    {null, 0},
};

static bool namei_save(string_address text, positive length,
                       positive address_to offset)
{
        if (length == positive_max ||
            !shell_array_room(namei_text, namei_text_room,
                              namei_text_used + length + 1))
        {
                file_fail("namei: out of memory while recording path\n", 0);
                return false;
        }
        address_to offset = namei_text_used;
        memory_copy_apart_end(namei_text + namei_text_used, text, length);
        namei_text_used += length + 1;
        return true;
}

static bool namei_is_mountpoint(string_address path,
                                file_facts address_to facts)
{
        if (!namei_mounts ||
            (facts->mode & MODE_FORMAT) != MODE_DIRECTORY)
                return false;

        p8 parent_path[FILE_PATH_MAX];
        if (!file_path_join(parent_path, path, (string_address)".."))
                return false;

        file_facts parent;
        if (!file_look(AT_FDCWD, parent_path, 0, address_of parent))
                return false;

        return facts->mount_id != parent.mount_id ||
               (facts->inode == parent.inode &&
                facts->device_major == parent.device_major &&
                facts->device_minor == parent.device_minor);
}

static namei_row address_to namei_add(string_address name, positive length,
                                      positive depth)
{
        if (!shell_array_room(namei_rows, namei_row_room,
                              namei_row_count + 1))
        {
                file_fail("namei: out of memory while recording path\n", 0);
                return null;
        }

        positive saved;
        if (!namei_save(name, length, address_of saved))
                return null;

        namei_row address_to row = namei_rows + namei_row_count++;
        memory_fill(row, 0, sizeof(*row));
        row->name_at = saved;
        row->target_at = positive_max;
        row->depth = depth;
        return row;
}

static bool namei_join_component(p8 address_to into, string_address directory,
                                 string_address component, positive length)
{
        positive head = string_length(directory);
        positive slash = head && directory[head - 1] != '/';

        if (head >= FILE_PATH_MAX || length >= FILE_PATH_MAX - head ||
            slash >= FILE_PATH_MAX - head - length)
                return false;

        memory_copy_apart(into, directory, head);
        if (slash)
                into[head++] = '/';
        memory_copy_apart_end(into + head, component, length);
        return true;
}

/* Resolve one written sequence from base.  Symlink text is recursively
   walked at one deeper display level, while the caller's remaining suffix
   resumes at its original level.  current is always the actual absolute
   directory/object reached, so . and .. after a link keep kernel semantics. */
static bool namei_walk(string_address path, string_address base,
                       positive depth, positive address_to hops,
                       p8 address_to resolved)
{
        positive length = string_length(path);
        bool absolute = length && path[0] == '/';
        bool trailing = length > 1 && path[length - 1] == '/';
        p8 current[FILE_PATH_MAX];

        if (absolute)
        {
                string_copy_end(current, "/");
                namei_row address_to root = namei_add("/", 1, depth);
                if (!root)
                        return false;
                bipolar told = file_look_code(AT_FDCWD, "/",
                                              AT_SYMLINK_NOFOLLOW,
                                              address_of root->facts);
                if (told < 0)
                {
                        root->reason = file_reason(told);
                        return false;
                }
                root->known = true;
                root->mountpoint = namei_mounts;
        }
        else
                string_copy_max_end(current, base, FILE_PATH_MAX - 1);

        positive at = 0;
        namei_row address_to last = null;
        while (at < length)
        {
                while (at < length && path[at] == '/')
                        at++;
                if (at == length)
                        break;

                positive start = at;
                while (at < length && path[at] != '/')
                        at++;
                positive part = at - start;
                p8 candidate[FILE_PATH_MAX];
                if (!namei_join_component(candidate, current,
                                          path + start, part))
                {
                        string_format(file_fail, "namei: %s: %s\n",
                                      namei_operand,
                                      file_reason(-ERROR_NAME_TOO_LONG));
                        return false;
                }

                namei_row address_to row = namei_add(path + start, part,
                                                     depth);
                if (!row)
                        return false;
                last = row;
                bipolar told = file_look_code(AT_FDCWD, candidate,
                                              AT_SYMLINK_NOFOLLOW,
                                              address_of row->facts);
                if (told < 0)
                {
                        row->reason = file_reason(told);
                        return false;
                }
                row->known = true;
                row->mountpoint = namei_is_mountpoint(candidate,
                                                      address_of row->facts);

                if ((row->facts.mode & MODE_FORMAT) == MODE_LINK)
                {
                        p8 target[FILE_PATH_MAX];
                        bipolar target_length = file_link_text(
                            candidate, target, sizeof(target));
                        if (target_length < 0)
                        {
                                row->reason = file_reason(target_length);
                                return false;
                        }
                        if (!namei_save(target, (positive)target_length,
                                        address_of row->target_at))
                                return false;

                        if (!namei_no_symlinks)
                        {
                                if ((address_to hops)++ >= NAMEI_LINK_LIMIT)
                                {
                                        string_format(file_fail,
                                                      "namei: %s: exceeded limit of symlinks\n",
                                                      namei_operand);
                                        return false;
                                }

                                p8 followed[FILE_PATH_MAX];
                                if (!namei_walk(target, current, depth + 1,
                                                hops, followed))
                                        return false;
                                string_copy_max_end(current, followed,
                                                    FILE_PATH_MAX - 1);
                                continue;
                        }
                }

                if (part == 1 && path[start] == '.')
                        continue;
                if (part == 2 && path[start] == '.' && path[start + 1] == '.')
                {
                        p8 actual[FILE_PATH_MAX];
                        if (!file_real(candidate, actual))
                                return false;
                        string_copy_max_end(current, actual, FILE_PATH_MAX - 1);
                }
                else
                        string_copy_max_end(current, candidate,
                                            FILE_PATH_MAX - 1);
        }

        if (trailing && last)
        {
                file_facts through;
                if (!file_look(AT_FDCWD, current, 0, address_of through) ||
                    (through.mode & MODE_FORMAT) != MODE_DIRECTORY)
                        return false;
        }

        string_copy_max_end(resolved, current, FILE_PATH_MAX - 1);
        return true;
}

static fn namei_put_padding(positive count)
{
        writer_fill(log, count, ' ');
}

static fn namei_show(bool modes, bool owners, bool vertical)
{
        positive user_width = 0;
        positive group_width = 0;
        p8 label[FILE_NAME_MAX];

        if (owners)
                for (positive i = 0; i < namei_row_count; i++)
                        if (namei_rows[i].known)
                        {
                                file_account_label(namei_rows[i].facts.owner,
                                                   false, true, label);
                                user_width = max(user_width,
                                                 string_length(label));
                                file_account_label(namei_rows[i].facts.group,
                                                   true, true, label);
                                group_width = max(group_width,
                                                  string_length(label));
                        }

        for (positive i = 0; i < namei_row_count; i++)
        {
                namei_row address_to row = namei_rows + i;
                if (!vertical)
                        namei_put_padding(1 + row->depth * 2);

                positive metadata = modes ? 10 : 1;
                if (owners)
                        metadata += 1 + user_width + 1 + group_width;

                if (!row->known)
                        namei_put_padding(metadata);
                else
                {
                        if (modes)
                        {
                                p8 letters[11];
                                file_mode_letters(letters, row->facts.mode);
                                if (row->mountpoint)
                                        letters[0] = 'D';
                                log(letters, 10);
                        }
                        else
                        {
                                p8 kind = row->mountpoint
                                    ? 'D' : file_kind_letter(row->facts.mode);
                                log(address_of kind, 1);
                        }

                        if (owners)
                        {
                                log(" ", 1);
                                file_account_label(row->facts.owner, false,
                                                   true, label);
                                string_to_field(log, label, user_width, ' ',
                                                false);
                                log(" ", 1);
                                file_account_label(row->facts.group, true,
                                                   true, label);
                                string_to_field(log, label, group_width, ' ',
                                                false);
                        }
                }

                namei_put_padding(row->known ? 1 : 2);
                if (vertical)
                        namei_put_padding(row->depth * 2);
                log(namei_text + row->name_at, 0);
                if (row->target_at != positive_max)
                {
                        log(" -> ", 4);
                        log(namei_text + row->target_at, 0);
                }
                if (row->reason)
                {
                        log(" - ", 3);
                        log(row->reason, 0);
                }
                log("\n", 1);
        }
}

static b32 file_namei()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address)"namei",
            .allowed = (string_address)"lmoxnvhV",
            .valued = (string_address)"",
            .longs = namei_longs,
            .operand = file_operand,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;
        if (taking.flags & FILE_FLAG('h'))
        {
                string_format(log, "Usage: namei [options] pathname...\n");
                log_flush();
                return 0;
        }
        if (taking.flags & FILE_FLAG('V'))
        {
                string_format(log, "namei from dawning-kit\n");
                log_flush();
                return 0;
        }
        if (!file_operand_count)
        {
                string_format(file_fail, "namei: pathname argument is missing\n");
                return 1;
        }

        namei_row_count = 0;
        namei_text_used = 0;

        positive flags = taking.flags;
        bool long_form = (flags & FILE_FLAG('l')) != 0;
        bool modes = long_form || (flags & FILE_FLAG('m')) != 0;
        bool owners = long_form || (flags & FILE_FLAG('o')) != 0;
        bool vertical = long_form || (flags & FILE_FLAG('v')) != 0;
        namei_no_symlinks = (flags & FILE_FLAG('n')) != 0;
        namei_mounts = (flags & FILE_FLAG('x')) != 0;
        b32 status = 0;

        for (positive i = 0; i < file_operand_count; i++)
        {
                namei_row_count = 0;
                namei_text_used = 0;
                namei_operand = file_operand_at(i);
                string_format(log, "f: %s\n", namei_operand);

                positive hops = 0;
                p8 resolved[FILE_PATH_MAX];
                string_address cwd = working_directory_get();
                if (!cwd || !namei_walk(namei_operand, cwd, 0,
                                         address_of hops, resolved))
                        status = 1;
                namei_show(modes, owners, vertical);
        }
        log_flush();
        return status;
}

// whereis ------------------------------------------------------------
/*
        The directory stream is the same getdents64 walker used by ls/find.
        whereis only supplies the small policy layer: which directories to
        visit, and which conventional source/manual suffixes are namesakes.
        Keeping an identity beside the resolved spelling avoids rescanning a
        directory mentioned through both PATH and the built-in list.
*/
enum
{
        WHEREIS_BINARY,
        WHEREIS_MANUAL,
        WHEREIS_SOURCE,
        WHEREIS_KINDS
};

typedef struct
{
        p8 path[FILE_PATH_MAX];
        file_facts facts;
        p8 kind;
} whereis_directory;

typedef struct
{
        positive directory;
        p8 name[256];
} whereis_match;

static whereis_directory address_to whereis_directories;
static positive whereis_directory_count;
static positive whereis_directory_room;
static whereis_match address_to whereis_matches;
static positive whereis_match_count;
static positive whereis_match_room;

static bool whereis_add_directory(positive kind, string_address path)
{
        p8 resolved[FILE_PATH_MAX];
        file_facts facts;

        if (!path || !file_real(path, resolved) ||
            !file_look(AT_FDCWD, resolved, 0, address_of facts) ||
            (facts.mode & MODE_FORMAT) != MODE_DIRECTORY)
                return true; // Search-list holes and races are not errors.

        for (positive i = 0; i < whereis_directory_count; i++)
                if (whereis_directories[i].kind == kind &&
                    file_same_identity(address_of whereis_directories[i].facts,
                                       address_of facts))
                        return true;

        if (!shell_array_room(whereis_directories, whereis_directory_room,
                              whereis_directory_count + 1))
        {
                string_format(file_fail,
                              "whereis: out of memory while recording search paths\n");
                return false;
        }

        whereis_directory address_to directory =
            whereis_directories + whereis_directory_count++;
        string_copy_max_end(directory->path, resolved, FILE_PATH_MAX - 1);
        directory->facts = facts;
        directory->kind = (p8)kind;
        return true;
}

static bool whereis_add_environment(positive kind, string_address value)
{
        if (!value)
                return true;

        path_walk walk = {.at = value};
        while (path_walk_next(address_of walk))
        {
                p8 directory[FILE_PATH_MAX];

                if (!walk.length)
                        string_copy_max_end(directory, (string_address)".",
                                            FILE_PATH_MAX - 1);
                else
                {
                        if (walk.length >= FILE_PATH_MAX)
                                continue;
                        memory_copy_apart_end(directory, walk.segment,
                                             walk.length);
                }

                if (!whereis_add_directory(kind, directory))
                        return false;
        }
        return true;
}

static bool whereis_add_children(positive kind, string_address root)
{
        file_walk walk;
        if (!file_walk_open(address_of walk, AT_FDCWD, root))
                return true;

        struct linux_dirent64 address_to entry;
        bool okay = true;
        while ((entry = file_walk_next(address_of walk)))
        {
                p8 path[FILE_PATH_MAX];

                if (file_is_dot(entry->d_name) ||
                    !file_path_join(path, root, entry->d_name))
                        continue;
                if (!whereis_add_directory(kind, path))
                {
                        okay = false;
                        break;
                }
        }
        file_walk_close(address_of walk);
        return okay;
}

static bool whereis_add_defaults(positive kind)
{
        static const string_address binary[] = {
            (string_address)"/usr/bin",       (string_address)"/usr/lib",
            (string_address)"/usr/lib32",     (string_address)"/etc",
            (string_address)"/usr/local/bin", (string_address)"/usr/local/sbin",
            (string_address)"/usr/local/etc", (string_address)"/usr/local/lib",
            (string_address)"/usr/local/games", (string_address)"/usr/include",
            (string_address)"/usr/local",     (string_address)"/usr/share",
            null};

        if (kind == WHEREIS_BINARY)
        {
                for (positive i = 0; binary[i]; i++)
                        if (!whereis_add_directory(kind, binary[i]))
                                return false;
                return whereis_add_environment(kind,
                                               file_environment((string_address)"PATH"));
        }

        if (kind == WHEREIS_MANUAL)
        {
                if (!whereis_add_children(kind, (string_address)"/usr/share/man") ||
                    !whereis_add_directory(kind, (string_address)"/usr/share/info"))
                        return false;
                return whereis_add_environment(
                    kind, file_environment((string_address)"MANPATH"));
        }

        return whereis_add_children(kind, (string_address)"/usr/src");
}

static bool whereis_compression(string_address suffix)
{
        static const string_address names[] = {
            (string_address)"gz",   (string_address)"bz2",
            (string_address)"xz",   (string_address)"zst",
            (string_address)"lz",   (string_address)"lzma",
            (string_address)"lzo",  (string_address)"Z", null};

        for (positive i = 0; names[i]; i++)
                if (string_equals(suffix, names[i]))
                        return true;
        return false;
}

/* One conventional suffix belongs to a source or manual name.  Manuals may
   additionally carry one compression suffix.  s.NAME is SCCS source syntax. */
static bool whereis_suffix_match(positive kind, string_address query,
                                 string_address candidate)
{
        if (kind == WHEREIS_SOURCE && candidate[0] == 's' &&
            candidate[1] == '.')
                candidate += 2;

        positive wanted = string_length(query);
        positive have = string_length(candidate);
        if (have < wanted || string_compare_max(candidate, query, wanted))
                return false;
        if (have == wanted)
                return true;
        if (candidate[wanted] != '.')
                return false;

        string_address suffix = candidate + wanted + 1;
        string_address second = string_first_of_or_end(suffix, '.');
        if (!string_get(second))
                return true;

        return kind == WHEREIS_MANUAL && whereis_compression(second + 1) &&
               !string_first_of(second + 1, '.');
}

static bool whereis_name_matches(positive kind, string_address query,
                                 string_address candidate, bool glob)
{
        if (glob)
                return shell_match(query, candidate);
        if (kind == WHEREIS_BINARY)
                return string_equals(query, candidate);
        return whereis_suffix_match(kind, query, candidate);
}

static bool whereis_scan(positive kind, string_address query, bool glob)
{
        for (positive directory_at = 0;
             directory_at < whereis_directory_count; directory_at++)
        {
                whereis_directory address_to directory =
                    whereis_directories + directory_at;
                if (directory->kind != kind)
                        continue;

                file_walk walk;
                if (!file_walk_open(address_of walk, AT_FDCWD,
                                    directory->path))
                        continue;

                struct linux_dirent64 address_to entry;
                while ((entry = file_walk_next(address_of walk)))
                {
                        if (file_is_dot(entry->d_name) ||
                            !whereis_name_matches(kind, query, entry->d_name,
                                                  glob))
                                continue;
                        if (!shell_array_room(whereis_matches,
                                              whereis_match_room,
                                              whereis_match_count + 1))
                        {
                                file_walk_close(address_of walk);
                                string_format(file_fail,
                                              "whereis: out of memory while recording matches\n");
                                return false;
                        }

                        whereis_match address_to match =
                            whereis_matches + whereis_match_count++;
                        match->directory = directory_at;
                        string_copy_max_end(match->name, entry->d_name, 255);
                }
                file_walk_close(address_of walk);

                /* Matches stay in the kernel directory order, as util-linux does. */
        }
        return true;
}

static bool whereis_long(string_address word, string_address name)
{
        return word[0] == '-' && word[1] == '-' &&
               string_equals(word + 2, name);
}

static b32 file_whereis()
{
        positive count = (positive)program_argument_count();
        bool selected[WHEREIS_KINDS] = {false, false, false};
        bool custom[WHEREIS_KINDS] = {false, false, false};
        bool unusual = false;
        bool glob = false;
        bool list = false;
        bool names = false;
        bipolar collecting = -1;
        b32 status = 0;

        whereis_directory_count = 0;
        whereis_match_count = 0;
        file_operands_begin();

        for (positive i = 1; i < count; i++)
        {
                string_address word = program_argument((b32)i);

                if (names)
                {
                        file_operand((b32)i);
                        continue;
                }
                if (collecting >= 0 && word[0] != '-')
                {
                        if (!whereis_add_directory((positive)collecting, word))
                                return 1;
                        continue;
                }
                collecting = -1;

                if (string_equals(word, (string_address)"-f") ||
                    string_equals(word, (string_address)"--"))
                {
                        names = true;
                        continue;
                }
                if (string_equals(word, (string_address)"-h") ||
                    whereis_long(word, (string_address)"help"))
                {
                        string_format(
                            log,
                            "Usage: whereis [options] NAME...\n"
                            "  -b, -m, -s       search binaries, manuals, sources\n"
                            "  -B, -M, -S DIR... -f  set category search paths\n"
                            "  -u unusual  -g glob  -l list paths\n");
                        return 0;
                }
                if (string_equals(word, (string_address)"-V") ||
                    whereis_long(word, (string_address)"version"))
                {
                        string_format(log, "whereis from dawning-kit\n");
                        return 0;
                }
                if (word[0] == '-' && word[1])
                {
                        for (positive at = 1; word[at]; at++)
                        {
                                p8 option = word[at];
                                if (option == 'b')
                                        selected[WHEREIS_BINARY] = true;
                                else if (option == 'm')
                                        selected[WHEREIS_MANUAL] = true;
                                else if (option == 's')
                                        selected[WHEREIS_SOURCE] = true;
                                else if (option == 'u')
                                        unusual = true;
                                else if (option == 'g')
                                        glob = true;
                                else if (option == 'l')
                                        list = true;
                                else if (option == 'B' || option == 'M' ||
                                         option == 'S')
                                {
                                        collecting = option == 'B'
                                                         ? WHEREIS_BINARY
                                                         : option == 'M'
                                                               ? WHEREIS_MANUAL
                                                               : WHEREIS_SOURCE;
                                        custom[collecting] = true;
                                        if (word[at + 1])
                                        {
                                                if (!whereis_add_directory(
                                                        (positive)collecting,
                                                        word + at + 1))
                                                        return 1;
                                                collecting = -1;
                                        }
                                        break;
                                }
                                else
                                {
                                        string_format(file_fail,
                                                      "whereis: invalid option -- '%c'\n",
                                                      option);
                                        return 1;
                                }
                        }
                }
                else
                        file_operand((b32)i);
        }

        if (file_operand_failed)
        {
                string_format(file_fail, "whereis: out of memory\n");
                return 1;
        }

        if (!selected[0] && !selected[1] && !selected[2])
                selected[0] = selected[1] = selected[2] = true;

        for (positive kind = 0; kind < WHEREIS_KINDS; kind++)
                if ((selected[kind] && !custom[kind]) || list)
                        if (!whereis_add_defaults(kind))
                                return 1;

        if (list)
        {
                static const string_address label[] = {
                    (string_address)"bin", (string_address)"man",
                    (string_address)"src"};
                for (positive kind = 0; kind < WHEREIS_KINDS; kind++)
                        for (positive i = 0; i < whereis_directory_count; i++)
                                if (whereis_directories[i].kind == kind)
                                        string_format(log, "%s: %s\n",
                                                      label[kind],
                                                      whereis_directories[i].path);
        }

        if (!file_operand_count)
        {
                if (list)
                {
                        log_flush();
                        return 0;
                }
                string_format(file_fail, "whereis: missing name\n");
                return 1;
        }

        for (positive operand = 0; operand < file_operand_count; operand++)
        {
                string_address query =
                    file_last_component(file_operand_at(operand));
                whereis_match_count = 0;

                for (positive kind = 0; kind < WHEREIS_KINDS; kind++)
                        if (selected[kind] && !whereis_scan(kind, query, glob))
                                return 1;

                if (unusual && whereis_match_count <= 1)
                        continue;

                string_format(log, "%s:", query);
                for (positive i = 0; i < whereis_match_count; i++)
                {
                        whereis_match address_to match = whereis_matches + i;
                        string_format(log, " %s/%s",
                                      whereis_directories[match->directory].path,
                                      match->name);
                }
                string_format(log, "\n");
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

static const file_supersede readlink_supersedes[] = {
    {(string_address) "fem", address_of readlink_canonical_option},
    {null, null},
};

static b32 file_readlink()
{
        readlink_canonical_option = 0;

        file_taking taking = {
            .program = (string_address) "readlink",
            .allowed = (string_address) "fneqsvmz",
            .valued = (string_address) "",
            .longs = readlink_longs,
            .supersedes = readlink_supersedes,
        };

        if (!file_take(address_of taking))
                return 1;

        positive first = taking.first;
        positive count = (positive)program_argument_count();
        positive flags = taking.flags;

        if (first >= count)
                return file_missing((string_address) "readlink");

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
                        bool valid = file_real(path, answer);

                        if (valid && readlink_canonical_option != 'm')
                        {
                                p8 above[FILE_PATH_MAX];

                                path_head_copy(above, FILE_PATH_MAX, answer);

                                // -f wants the parent to be real, -e wants
                                // the whole path to be, -m wants neither.
                                valid = file_is_directory_through(above) &&
                                        (readlink_canonical_option != 'e' ||
                                         file_exists(AT_FDCWD, answer));
                        }

                        if (!valid)
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
        positive stop = string_length(name);

        // Trailing separators are not part of the basename. Preserve one
        // when the complete operand is a run of separators, because the
        // basename of root is root.
        while (stop > 1 && name[stop - 1] == '/')
                stop--;

        positive start = 0;
        p8 address_to slash = memory_last_of(name, '/', stop);

        if (slash)
        {
                start = (positive)(slash - name) + 1;

                if (start == stop)
                        start--;
        }

        positive length = stop - start;

        if (suffix)
        {
                positive cut = string_length(suffix);

                // A name that is nothing but its suffix keeps it: stripping
                // would leave an empty line where a name was asked for.
                if (cut > 0 && cut < length)
                {
                        if (!memory_compare(name + start + length - cut, suffix, cut))
                                length -= cut;
                }
        }

        log(name + start, length);
        log(zero ? "\0" : "\n", 1);
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
                return file_missing((string_address) "basename");

        if (!many && index + 1 < count)
                suffix = program_argument((b32)(index + 1));

        if (!many && index + 2 < count)
        {
                file_fail("basename: extra operand\n", 0);
                return 1;
        }

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

static fn dirname_one(string_address name, bool zero)
{
        positive stop = string_length(name);

        // First discard separators after the basename, then the basename
        // itself, then separators between it and the directory. Keeping one
        // separator makes every spelling of root answer "/".
        while (stop && name[stop - 1] == '/')
                stop--;

        if (!stop && !name[0])
        {
                log(".", 1);
                log(zero ? "\0" : "\n", 1);
                return;
        }

        if (!stop)
        {
                log("/", 1);
                log(zero ? "\0" : "\n", 1);
                return;
        }

        while (stop && name[stop - 1] != '/')
                stop--;

        if (!stop)
        {
                log(".", 1);
                log(zero ? "\0" : "\n", 1);
                return;
        }

        while (stop > 1 && name[stop - 1] == '/')
                stop--;

        log(name, stop);
        log(zero ? "\0" : "\n", 1);
}

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
                return file_missing((string_address) "dirname");

        while (first < count)
                dirname_one(program_argument((b32)first++),
                            (taking.flags & FILE_FLAG('z')) != 0);

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

static const file_supersede realpath_supersedes[] = {
    {(string_address) "Eem", address_of realpath_missing_option},
    {(string_address) "LP", address_of realpath_walk_option},
    {null, null},
};

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
static bool realpath_relative(string_address from, string_address path,
                              p8 address_to into)
{
        positive from_length = string_length(from);
        positive path_length = string_length(path);
        positive same = memory_common_prefix(
            from, path, from_length < path_length ? from_length : path_length);
        string_address slash = memory_last_of(from, '/', same);
        positive mark = slash ? (positive)(slash - from) + 1 : 0;

        // A whole component or none of it: /usr/lib and /usr/libexec share
        // five letters and no directory below the first.
        positive from_mark = mark;
        positive path_mark = mark;

        if (string_is(from + same, end) &&
            (string_is(path + same, '/') || string_is(path + same, end)))
        {
                from_mark = same;
                path_mark = same + (string_is(path + same, '/') ? 1 : 0);
        }
        else if (string_is(path + same, end) && string_is(from + same, '/'))
        {
                from_mark = same + 1;
                path_mark = same;
        }

        positive length = 0;
        string_address step = from + from_mark;

        while (string_get(step))
        {
                while (string_is(step, '/'))
                        step++;

                if (string_is(step, end))
                        break;

                while (string_get(step) && !string_is(step, '/'))
                        step++;

                positive need = 2 + (length != 0);

                if (length > FILE_PATH_MAX - 1 - need)
                        return false;

                if (length)
                        into[length++] = '/';

                into[length++] = '.';
                into[length++] = '.';
        }

        if (string_get(path + path_mark))
        {
                positive rest = string_length(path + path_mark);
                positive separator = length != 0;

                if (length > FILE_PATH_MAX - 1 - separator ||
                    rest > FILE_PATH_MAX - 1 - length - separator)
                        return false;

                if (length)
                        into[length++] = '/';

                memory_copy_apart(into + length, path + path_mark, rest);
                length += rest;
        }

        if (!length)
                into[length++] = '.';

        into[length] = end;
        return true;
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
            .supersedes = realpath_supersedes,
        };

        if (!file_take(address_of taking))
                return 1;

        positive first = taking.first;
        positive count = (positive)program_argument_count();

        if (first >= count)
                return file_missing((string_address) "realpath");

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

        if ((base && !string_get(base)) || (against && !string_get(against)))
        {
                file_fail("realpath: relative directory is empty\n", 0);
                return 1;
        }

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

                        if (!realpath_relative(against, answer, relative))
                        {
                                if (!quiet)
                                        string_format(file_fail,
                                                      "realpath: %s: File name too long\n",
                                                      path);

                                status = 1;
                                continue;
                        }

                        file_written(relative, zero);
                }
                else
                        file_written(answer, zero);
        }

        log_flush();

        return status;
}

// pathchk ----------------------------------------------------------
#define PATHCHK_POSIX_PATH 256
#define PATHCHK_POSIX_NAME 14

static const file_long pathchk_longs[] = {
    {(string_address) "portability", 'Q'},
    {null, 0},
};

static COLD bool pathchk_bad(string_address path, string_address why)
{
        string_format(file_fail, "pathchk: %s: '%s'\n", why, path);
        return false;
}

static bool pathchk_portable_chars(string_address path, positive length)
{
        static b8 portable[STRING_SET_BYTES];
        static bool ready;

        if (!ready)
        {
                string_set_add(portable,
                               (string_address) "/ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                                "abcdefghijklmnopqrstuvwxyz"
                                                "0123456789._-");
                ready = true;
        }

        return string_span(path, portable) == length;
}

static bool pathchk_one(string_address path, bool basic, bool extra)
{
        positive length = string_length(path);

        if ((basic || extra) && !length)
                return pathchk_bad(path, (string_address) "empty file name");

        positive at = 0;
        positive longest = 0;

        while (at < length)
        {
                at += memory_span_byte(path + at, '/', length - at);

                if (at >= length)
                        break;

                string_address stop = string_first_of_or_end(path + at, '/');
                positive component = (positive)(stop - path) - at;

                if (extra && string_is(path + at, '-'))
                        return pathchk_bad(path,
                                           (string_address) "leading '-' in a component");

                if (component > longest)
                        longest = component;

                at += component;
        }

        if (basic && !pathchk_portable_chars(path, length))
                return pathchk_bad(path, (string_address) "non-portable character");

        if (basic)
        {
                if (length >= PATHCHK_POSIX_PATH)
                        return pathchk_bad(path,
                                           (string_address) "portable path limit exceeded");

                if (longest > PATHCHK_POSIX_NAME)
                        return pathchk_bad(path,
                                           (string_address) "portable component limit exceeded");

                return true;
        }

        file_facts facts;
        bipolar looked = system_stat_at(
            AT_FDCWD, path, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT,
            STATX_BASIC, address_of facts);

        if (!looked)
                return true;

        if (looked != -ERROR_NO_ENTRY || !length)
                return pathchk_bad(path, file_reason(looked));

        if (length >= FILE_PATH_MAX)
                return pathchk_bad(path, (string_address) "path limit exceeded");

        // Linux promises at least fourteen bytes in every component. Only a
        // longer one needs the mount-specific f_namelen walk.
        if (longest <= PATHCHK_POSIX_NAME)
                return true;

        p8 prefix[FILE_PATH_MAX];
        positive filled = 0;
        positive name_max = PATHCHK_POSIX_NAME;
        file_mount_facts mount;
        string_address base = string_is(path, '/') ? (string_address) "/"
                                                   : (string_address) ".";
        bipolar mounted = system_call_2(syscall(statfs), (positive)base,
                                        (positive)address_of mount);

        if (mounted < 0)
                return pathchk_bad(path, file_reason(mounted));

        if (mount.name_length > 0)
                name_max = (positive)mount.name_length;

        at = 0;

        if (string_is(path, '/'))
                prefix[filled++] = '/';

        while (at < length)
        {
                at += memory_span_byte(path + at, '/', length - at);

                if (at >= length)
                        break;

                string_address stop = string_first_of_or_end(path + at, '/');
                positive component = (positive)(stop - path) - at;

                if (component > name_max)
                        return pathchk_bad(path,
                                           (string_address) "component limit exceeded");

                if (filled && prefix[filled - 1] != '/')
                        prefix[filled++] = '/';

                memory_copy_apart(prefix + filled, path + at, component);
                filled += component;
                prefix[filled] = end;

                mounted = system_call_2(syscall(statfs), (positive)prefix,
                                        (positive)address_of mount);

                if (!mounted && mount.name_length > 0)
                        name_max = (positive)mount.name_length;
                else if (mounted < 0 && mounted != -ERROR_NO_ENTRY)
                        return pathchk_bad(path, file_reason(mounted));

                at += component;
        }

        return true;
}

static b32 file_pathchk()
{
        file_taking taking = {
            .program = (string_address) "pathchk",
            .allowed = (string_address) "pP",
            .valued = (string_address) "",
            .longs = pathchk_longs,
        };

        if (!file_take(address_of taking))
                return 1;

        positive count = (positive)program_argument_count();
        positive first = taking.first;

        if (first >= count)
                return file_missing((string_address) "pathchk");

        bool basic = (taking.flags &
                      (FILE_FLAG('p') | FILE_FLAG('Q'))) != 0;
        bool extra = (taking.flags &
                      (FILE_FLAG('P') | FILE_FLAG('Q'))) != 0;
        b32 status = 0;

        while (first < count)
                if (!pathchk_one(program_argument((b32)first++), basic, extra))
                        status = 1;

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

        // -m is read against a=rwx the way the reference mkdir reads it,
        // with a clause that names no class filtered through the umask.
        if (given_mode &&
            !file_mode_masked(file_option_value(address_of taking, 'm'), 0777, true,
                              file_umask(), address_of mode))
        {
                file_fail("mkdir: bad mode\n", 0);
                return 1;
        }

        if (index >= count)
                return file_missing((string_address) "mkdir");

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
                                system_change_mode_at(AT_FDCWD, path, mode);

                        continue;
                }

                bipolar made = system_make_directory_at(AT_FDCWD, path, mode);

                if (made < 0)
                {
                        string_format(file_fail, "mkdir: cannot create directory '%s': %s\n",
                                      path, file_reason(made));
                        status = 1;
                }
                else if (given_mode)
                        // mkdirat applies the umask; -m names the mode after
                        // it, as the -p branch above already does.
                        system_change_mode_at(AT_FDCWD, path, mode);
        }

        log_flush();

        return status;
}

// mkfifo / mknod ----------------------------------------------------
/*
        Both interfaces are the same operation at the kernel floor.  Their
        explicit mode starts from a=rw; an omitted `who` is filtered through
        the process umask, while a named class is not.  The creation syscall
        applies the umask once more, so an explicit mode is restored after a
        successful creation exactly as GNU does.
*/
static const file_long file_node_longs[] = {
    {(string_address) "context", 'C'},
    {(string_address) "mode", 'm'},
    {null, 0},
};

static bool file_node_mode(string_address specification,
                           positive address_to mode)
{
        return file_mode_masked(specification, 0666, false, file_umask(), mode) &&
               !(address_to mode & ~0777);
}

static b32 file_make_node(string_address program, string_address path,
                          positive kind, positive device, positive mode,
                          bool given_mode)
{
        bipolar made = system_call_4(syscall(mknodat), AT_FDCWD,
                                     (positive)path, kind | mode, device);

        if (made < 0)
        {
                string_format(file_fail, "%s: cannot create '%s': %s\n",
                              program, path, file_reason(made));
                return 1;
        }

        if (given_mode &&
            system_change_mode_at(AT_FDCWD, path, mode) < 0)
        {
                string_format(file_fail,
                              "%s: cannot set permissions of '%s'\n",
                              program, path);
                return 1;
        }

        return 0;
}

static bool file_node_options(string_address program, file_taking address_to taking,
                              positive address_to mode, bool address_to given)
{
        file_operands_begin();
        taking->program = program;
        taking->allowed = (string_address) "mZ";
        taking->valued = (string_address) "m";
        taking->optional = (string_address) "C";
        taking->longs = file_node_longs;
        taking->operand = file_operand;

        if (!file_take(taking) || file_operand_failed)
                return false;

        address_to given = (taking->flags & FILE_FLAG('m')) != 0;
        address_to mode = 0666;

        if (address_to given &&
            !file_node_mode(file_option_value(taking, 'm'), mode))
        {
                string_format(file_fail, "%s: invalid mode\n", program);
                return false;
        }

        // This image has neither SELinux nor SMACK. GNU treats -Z and a bare
        // --context as no-ops in that case, and only warns for an explicit
        // context value while leaving the creation and status untouched.
        if (file_option_value(taking, 'C'))
                string_format(file_fail,
                              "%s: warning: ignoring --context; it requires "
                              "an SELinux/SMACK-enabled kernel\n",
                              program);

        return true;
}

static b32 file_mkfifo()
{
        file_taking taking = {0};
        positive mode;
        bool given_mode;

        if (!file_node_options((string_address) "mkfifo", address_of taking,
                               address_of mode, address_of given_mode))
                return 1;

        if (!file_operand_count)
                return file_missing((string_address) "mkfifo");

        b32 status = 0;

        for (positive i = 0; i < file_operand_count; i++)
                status |= file_make_node((string_address) "mkfifo",
                                         file_operand_at(i), MODE_PIPE, 0,
                                         mode, given_mode);

        log_flush();
        return status;
}

static bool file_device_number(string_address text, p32 address_to value)
{
        text += string_span(text, string_set_blanks);

        if (string_is(text, '+'))
                text++;
        else if (string_is(text, '-'))
                return false;

        positive used;
        positive made;

        if (string_is(text, '0') &&
            (string_is(text + 1, 'x') || string_is(text + 1, 'X')))
        {
                positive digits;

                made = string_digits_hexadecimal_max(text + 2, p32_max,
                                                     address_of digits);

                if (!digits)
                        return false;

                used = digits + 2;
        }
        else if (string_is(text, '0'))
                made = string_digits_octal_max(text, p32_max, address_of used);
        else
                made = string_digits_max(text, p32_max, address_of used);

        if (!used || string_get(text + used) || made > p32_max)
                return false;

        address_to value = (p32)made;
        return true;
}

static CONST positive file_device(p32 major, p32 minor)
{
        return ((positive)minor & 0xff) |
               (((positive)major & 0xfff) << 8) |
               (((positive)minor & ~0xffu) << 12) |
               (((positive)major & ~0xfffu) << 32);
}

static b32 file_mknod()
{
        file_taking taking = {0};
        positive mode;
        bool given_mode;

        if (!file_node_options((string_address) "mknod", address_of taking,
                               address_of mode, address_of given_mode))
                return 1;

        positive expected = file_operand_count > 1 &&
                                    string_is(file_operand_at(1), 'p')
                                ? 2
                                : 4;

        if (file_operand_count != expected)
        {
                file_fail(!file_operand_count
                              ? (string_address) "mknod: missing operand\n"
                              : file_operand_count < expected
                                    ? (string_address) "mknod: missing operand\n"
                                    : (string_address) "mknod: extra operand\n",
                          0);
                return 1;
        }

        string_address path = file_operand_at(0);
        p8 type = string_get(file_operand_at(1));
        positive kind;
        positive device = 0;

        if (type == 'p')
                kind = MODE_PIPE;
        else if (type == 'b' || type == 'c' || type == 'u')
        {
                p32 major, minor;

                if (!file_device_number(file_operand_at(2), address_of major))
                {
                        string_format(file_fail,
                                      "mknod: invalid major device number '%s'\n",
                                      file_operand_at(2));
                        return 1;
                }

                if (!file_device_number(file_operand_at(3), address_of minor))
                {
                        string_format(file_fail,
                                      "mknod: invalid minor device number '%s'\n",
                                      file_operand_at(3));
                        return 1;
                }

                kind = type == 'b' ? MODE_BLOCK : MODE_CHARACTER;
                device = file_device(major, minor);
        }
        else
        {
                string_format(file_fail, "mknod: invalid device type '%s'\n",
                              file_operand_at(1));
                return 1;
        }

        b32 status = file_make_node((string_address) "mknod", path, kind,
                                    device, mode, given_mode);
        log_flush();
        return status;
}

// sync -------------------------------------------------------------
#define FILE_F_GETFL 3
#define FILE_F_SETFL 4

static const file_long sync_longs[] = {
    {(string_address) "data", 'd'},
    {(string_address) "file-system", 'f'},
    {null, 0},
};

static bool file_sync_one(string_address path, p8 mode)
{
        bipolar opened = system_open_at(AT_FDCWD,
                                       path,
                                       FILE_READ | O_NONBLOCK);
        bipolar read_error = opened;

        if (opened < 0)
                opened = system_open_at(AT_FDCWD,
                                       path, 01 | O_NONBLOCK);

        if (opened < 0)
        {
                string_format(file_fail, "sync: error opening '%s': %s\n",
                              path, file_reason(read_error));
                return false;
        }

        bipolar flags = system_call_3(syscall(fcntl), (positive)opened,
                                      FILE_F_GETFL, 0);
        bool good = flags >= 0 &&
                    system_call_3(syscall(fcntl), (positive)opened,
                                  FILE_F_SETFL,
                                  (positive)flags & ~O_NONBLOCK) >= 0;

        if (!good)
                string_format(file_fail,
                              "sync: couldn't reset non-blocking mode '%s'\n",
                              path);
        else
        {
                bipolar synced = mode == 'd'
                                      ? system_call_1(syscall(fdatasync),
                                                      (positive)opened)
                                  : mode == 'f'
                                      ? system_call_1(syscall(syncfs),
                                                      (positive)opened)
                                      : system_call_1(syscall(fsync),
                                                      (positive)opened);

                if (synced < 0)
                {
                        string_format(file_fail, "sync: error syncing '%s': %s\n",
                                      path, file_reason(synced));
                        good = false;
                }
        }

        bipolar closed = system_close(opened);

        if (closed < 0)
        {
                string_format(file_fail, "sync: failed to close '%s': %s\n",
                              path, file_reason(closed));
                good = false;
        }

        return good;
}

static b32 file_sync()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address) "sync",
            .allowed = (string_address) "df",
            .valued = (string_address) "",
            .longs = sync_longs,
            .operand = file_operand,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;

        bool data = (taking.flags & FILE_FLAG('d')) != 0;
        bool filesystem = (taking.flags & FILE_FLAG('f')) != 0;

        if (data && filesystem)
        {
                file_fail("sync: cannot specify both --data and --file-system\n",
                          0);
                return 1;
        }

        if (data && !file_operand_count)
        {
                file_fail("sync: --data needs at least one argument\n", 0);
                return 1;
        }

        if (!file_operand_count)
                system_call(syscall(sync));
        else
        {
                p8 mode = data ? 'd' : filesystem ? 'f' : 0;
                b32 status = 0;

                for (positive i = 0; i < file_operand_count; i++)
                        if (!file_sync_one(file_operand_at(i), mode))
                                status = 1;

                log_flush();
                return status;
        }

        return 0;
}

// split ------------------------------------------------------------
/*
        split's byte path stays in the kernel for regular files: one
        copy_file_range per ordinary output piece, sendfile when two mounted
        filesystems cannot range-copy, and the shared file_transfer buffer
        only when neither kernel interface accepts the pair.  The record path
        uses the same buffer and memory_first_of scanner already used by the
        text tools; it does not grow a second reader or output layer.

        Line-byte packing (-C) scans mapped regular input with the shared
        vector first-of primitive, while indeterminate streams use the shared
        text arena.  Plain -n distribution keeps known-size input in the
        kernel-copy path.  Its l/, r/ and K/N forms are distinct scheduling
        contracts and are rejected until implemented rather than guessed.
*/
#define SPLIT_SUFFIX_MAX 32

typedef struct
{
        string_address prefix;
        string_address additional;
        positive prefix_length;
        positive additional_length;
        positive suffix_length;
        positive number;
        p8 radix;
        bool suffix_fixed;
        bool need_advance;
        bool verbose;
        bool protect_input;
        file_facts input;
        bipolar handle;
        p8 suffix[SPLIT_SUFFIX_MAX];
        p8 name[FILE_PATH_MAX];
} split_output;

static PURE p8 file_size_power(p8 suffix, bool every_lower);

static const file_long split_longs[] = {
    {(string_address) "additional-suffix", 'S'},
    {(string_address) "bytes", 'b'},
    {(string_address) "hex-suffixes", 'x'},
    {(string_address) "line-bytes", 'C'},
    {(string_address) "lines", 'l'},
    {(string_address) "numeric-suffixes", 'd'},
    {(string_address) "number", 'n'},
    {(string_address) "separator", 't'},
    {(string_address) "suffix-length", 'a'},
    {(string_address) "verbose", 'v'},
    {null, 0},
};

static bool split_size(string_address text, positive address_to out)
{
        string_address at = text;
        positive value;

        if (!string_digits_checked(address_of at, 10, address_of value))
                return false;

        positive multiple = 1;
        p8 suffix = string_get(at);

        if (suffix == 'b' && !string_get(at + 1))
        {
                multiple = 512;
                at++;
        }
        else if (suffix == 'B' && !string_get(at + 1))
                at++;
        else if (suffix)
        {
                positive power = file_size_power(suffix, true);

                if (!power || power > 8)
                        return false;

                at++;
                positive base = 1024;

                if (byte_to_upper(string_get(at)) == 'B' &&
                    !string_get(at + 1))
                {
                        base = 1000;
                        at++;
                }
                else if (string_is(at, 'i') &&
                         byte_to_upper(string_get(at + 1)) == 'B' &&
                         !string_get(at + 2))
                        at += 2;
                else if (string_get(at))
                        return false;

                while (power--)
                {
                        if (multiple > positive_max / base)
                                return false;
                        multiple *= base;
                }
        }

        if (string_get(at) || !value || value > positive_max / multiple)
                return false;

        address_to out = value * multiple;
        return true;
}

/* The default alphabetic sequence remains lexically ordered when it grows:
   .. yz, zaaa .. zyzz, zzaaaa ... .  An explicit -a instead uses every name
   of its fixed width and reports exhaustion after zz. */
static bool split_alpha_advance(split_output address_to output)
{
        positive at = output->suffix_length;

        while (at && output->suffix[at - 1] == 'z')
        {
                output->suffix[at - 1] = 'a';
                at--;
        }

        if (!at)
                return false;

        positive changed = at - 1;

        if (!output->suffix_fixed && output->suffix[changed] == 'y')
        {
                bool leading_z = true;

                for (positive i = 0; i < changed; i++)
                        if (output->suffix[i] != 'z')
                                leading_z = false;

                if (leading_z)
                {
                        if (output->suffix_length + 2 > SPLIT_SUFFIX_MAX)
                                return false;

                        output->suffix[changed] = 'z';
                        memory_fill(output->suffix + changed + 1, 'a',
                                    output->suffix_length - changed + 1);
                        output->suffix_length += 2;
                        return true;
                }
        }

        output->suffix[changed]++;
        return true;
}

static bool split_output_advance(split_output address_to output)
{
        if (!output->need_advance)
                return true;

        output->need_advance = false;

        if (!output->radix)
                return split_alpha_advance(output);

        if (output->number == positive_max)
                return false;

        output->number++;
        return true;
}

static bool split_output_name(split_output address_to output)
{
        if (!split_output_advance(output))
        {
                file_fail("split: output file suffixes exhausted\n", 0);
                return false;
        }

        p8 digits[SPLIT_SUFFIX_MAX];
        string_address suffix = output->suffix;
        positive suffix_length = output->suffix_length;

        if (output->radix)
        {
                positive length = positive_into_base(
                    digits, output->number, output->radix, false);

                if (length > suffix_length)
                {
                        if (output->suffix_fixed || length > SPLIT_SUFFIX_MAX)
                        {
                                file_fail("split: output file suffixes exhausted\n", 0);
                                return false;
                        }
                        suffix_length = output->suffix_length = length;
                }

                positive padding = suffix_length - length;
                memory_fill(output->suffix, '0', padding);
                memory_copy_apart(output->suffix + padding, digits, length);
        }

        if (output->prefix_length >= FILE_PATH_MAX ||
            suffix_length >= FILE_PATH_MAX - output->prefix_length ||
            output->additional_length >=
                FILE_PATH_MAX - output->prefix_length - suffix_length)
        {
                file_fail("split: output file name is too long\n", 0);
                return false;
        }

        positive used = output->prefix_length;
        memory_copy_apart(output->name, output->prefix, used);
        memory_copy_apart(output->name + used, suffix, suffix_length);
        used += suffix_length;
        memory_copy_apart_end(output->name + used, output->additional,
                              output->additional_length);
        return true;
}

static bool split_same_input(split_output address_to output)
{
        if (!output->protect_input)
                return false;

        file_facts existing;

        if (!file_look_at(output->name, address_of existing))
                return false;

        return existing.inode == output->input.inode &&
               existing.device_major == output->input.device_major &&
               existing.device_minor == output->input.device_minor;
}

static bool split_output_open(split_output address_to output)
{
        if (output->handle >= 0)
                return true;
        if (!split_output_name(output))
                return false;
        if (split_same_input(output))
        {
                string_format(file_fail,
                              "split: '%s' would overwrite input; aborting\n",
                              output->name);
                return false;
        }

        output->handle = system_open_at_mode(AT_FDCWD, output->name,
                                             FILE_WRITE, 0666);

        if (output->handle < 0)
        {
                string_format(file_fail, "split: cannot open '%s': %s\n",
                              output->name, file_reason(output->handle));
                return false;
        }

        if (output->verbose)
                string_format(log, "creating file '%s'\n", output->name);

        return true;
}

static bool split_output_write(split_output address_to output,
                               address_any bytes, positive length)
{
        if (!length)
                return true;
        if (!split_output_open(output))
                return false;
        if (system_write_all((positive)output->handle, bytes, length) != length)
        {
                string_format(file_fail, "split: write error on '%s'\n",
                              output->name);
                return false;
        }
        return true;
}

static bool split_output_close(split_output address_to output)
{
        if (output->handle < 0)
                return true;

        bipolar closed = system_close(output->handle);
        output->handle = -1;
        output->need_advance = true;

        if (closed < 0)
        {
                string_format(file_fail, "split: closing '%s': %s\n",
                              output->name, file_reason(closed));
                return false;
        }
        return true;
}

/* Exactly one known-size byte piece.  The capability booleans persist across
   output files so an EXDEV or unsupported result is paid only once. */
static bool split_copy_piece(bipolar in, bipolar out, positive length,
                             bool address_to range_copy,
                             bool address_to send_copy)
{
        while (length && address_to range_copy)
        {
                positive ask = length > FILE_KERNEL_COPY_SIZE
                                   ? FILE_KERNEL_COPY_SIZE : length;
                bipolar copied = file_copy_range_once(in, null, out, null, ask);

                if (copied > 0)
                {
                        length -= (positive)copied;
                        continue;
                }
                if (copied == -4)
                        continue;
                if (copied < 0 && file_copy_range_fallback(copied))
                {
                        address_to range_copy = false;
                        break;
                }
                return false;
        }

        while (length && address_to send_copy)
        {
                positive ask = length > FILE_KERNEL_COPY_SIZE
                                   ? FILE_KERNEL_COPY_SIZE : length;
                bipolar copied = file_send_range_once(in, null, out, ask);

                if (copied > 0)
                {
                        length -= (positive)copied;
                        continue;
                }
                if (copied == -4)
                        continue;
                if (copied < 0 && file_copy_range_fallback(copied))
                {
                        address_to send_copy = false;
                        break;
                }
                return false;
        }

        while (length)
        {
                positive ask = length < sizeof(file_transfer)
                                   ? length : sizeof(file_transfer);
                bipolar taken = system_read_retry((positive)in, file_transfer,
                                                   ask);

                if (taken <= 0)
                        return false;
                if (system_write_all((positive)out, file_transfer,
                                     (positive)taken) != (positive)taken)
                        return false;
                length -= (positive)taken;
        }

        return true;
}

static bool split_regular_bytes(bipolar in, p64 length, positive piece,
                                split_output address_to output)
{
        bool range_copy = true;
        bool send_copy = true;

        while (length)
        {
                positive here = length < (p64)piece ? (positive)length : piece;

                if (!split_output_open(output) ||
                    !split_copy_piece(in, output->handle, here,
                                      address_of range_copy,
                                      address_of send_copy))
                {
                        file_fail("split: read or write error\n", 0);
                        return false;
                }
                if (!split_output_close(output))
                        return false;
                length -= here;
        }

        return true;
}

static bool split_stream_bytes(bipolar in, positive piece,
                               split_output address_to output)
{
        positive filled = 0;

        while (1)
        {
                positive ask = piece - filled;

                if (ask > sizeof(file_transfer))
                        ask = sizeof(file_transfer);

                bipolar taken = system_read_retry((positive)in, file_transfer,
                                                   ask);

                if (taken < 0)
                {
                        file_fail("split: read error\n", 0);
                        return false;
                }
                if (!taken)
                        return split_output_close(output);
                if (!split_output_write(output, file_transfer,
                                        (positive)taken))
                        return false;

                filled += (positive)taken;

                if (filled == piece)
                {
                        if (!split_output_close(output))
                                return false;
                        filled = 0;
                }
        }
}

static bool split_stream_lines(bipolar in, positive lines, p8 separator,
                               split_output address_to output)
{
        positive in_piece = 0;

        while (1)
        {
                bipolar taken = system_read_retry((positive)in, file_transfer,
                                                   sizeof(file_transfer));

                if (taken < 0)
                {
                        file_fail("split: read error\n", 0);
                        return false;
                }
                if (!taken)
                        return split_output_close(output);

                p8 address_to pending = file_transfer;
                p8 address_to finish = file_transfer + (positive)taken;

                while (pending < finish)
                {
                        positive needed = lines - in_piece;
                        positive records = memory_count(
                            pending, (positive)(finish - pending), separator);

                        /* The usual large-piece case has no boundary in this
                           refill.  memory_count is a vector-width pass, then
                           the whole block is one write; do not call the
                           first-of scanner once per short input record. */
                        if (records < needed)
                        {
                                if (!split_output_write(
                                        output, pending,
                                        (positive)(finish - pending)))
                                        return false;
                                in_piece += records;
                                break;
                        }

                        p8 address_to scan = pending;

                        for (positive found_count = 0;
                             found_count < needed; found_count++)
                        {
                                p8 address_to found = memory_first_of(
                                    scan, separator,
                                    (positive)(finish - scan));
                                scan = found + 1; /* records proved it exists */
                        }

                        if (!split_output_write(output, pending,
                                                (positive)(scan - pending)) ||
                            !split_output_close(output))
                                return false;
                        pending = scan;
                        in_piece = 0;
                }
        }
}

/* Pack whole records while one fits, then cut an overlong record into exact
   size pieces.  This is also the mmap hot path, so a normal short record is
   one vector search and one buffered output write. */
static bool split_line_bytes_memory(p8 address_to input, positive length,
                                    positive piece, p8 separator,
                                    split_output address_to output)
{
        positive at = 0;
        positive used = 0;

        while (at < length)
        {
                p8 address_to found = memory_first_of(input + at, separator,
                                                      length - at);
                positive stop = found ? (positive)(found - input) + 1 : length;
                positive record = stop - at;

                if (record <= piece)
                {
                        if (used && record > piece - used)
                        {
                                if (!split_output_close(output))
                                        return false;
                                used = 0;
                        }
                        if (!split_output_write(output, input + at, record))
                                return false;
                        used += record;
                        if (used == piece)
                        {
                                if (!split_output_close(output))
                                        return false;
                                used = 0;
                        }
                }
                else
                {
                        if (used)
                        {
                                if (!split_output_close(output))
                                        return false;
                                used = 0;
                        }

                        positive left = record;
                        positive record_at = at;
                        while (left >= piece)
                        {
                                if (!split_output_write(output,
                                                        input + record_at,
                                                        piece) ||
                                    !split_output_close(output))
                                        return false;
                                record_at += piece;
                                left -= piece;
                        }
                        if (left)
                        {
                                if (!split_output_write(output,
                                                        input + record_at,
                                                        left))
                                        return false;
                                used = left;
                        }
                }
                at = stop;
        }

        return split_output_close(output);
}

static bool split_line_bytes(bipolar in, file_facts address_to facts,
                             bool regular, positive piece, p8 separator,
                             split_output address_to output)
{
        if (regular)
        {
                if (!facts->size)
                        return true;

                bipolar mapped = system_call_6(
                    syscall(mmap), 0, (positive)facts->size,
                    FILE_PROTECT_READ, FILE_MAP_PRIVATE, (positive)in, 0);
                if (mapped < 0)
                {
                        string_format(file_fail, "split: cannot map input: %s\n",
                                      file_reason(mapped));
                        return false;
                }

                bool answer = split_line_bytes_memory(
                    (p8 address_to)(positive)mapped, (positive)facts->size,
                    piece, separator, output);
                system_call_2(syscall(munmap), (positive)mapped,
                              (positive)facts->size);
                return answer;
        }

        text_arena_used = 0;
        positive length;
        bool read_failed;
        p8 address_to input = text_arena_read_all(
            (positive)in, FILE_TRANSFER_SIZE, address_of length,
            address_of read_failed);

        if (!input)
        {
                file_fail(read_failed ? (string_address)"split: read error\n"
                                      : (string_address)"split: input too large\n",
                          0);
                text_arena_used = 0;
                return false;
        }

        bool answer = split_line_bytes_memory(input, length, piece, separator,
                                              output);
        text_arena_used = 0;
        return answer;
}

static bool split_chunks(string_address text, positive address_to chunks)
{
        string_address at = text;
        positive value;

        if (!string_digits_checked(address_of at, 10, address_of value) ||
            string_get(at) || !value)
                return false;
        address_to chunks = value;
        return true;
}

static bool split_distribute_regular(bipolar in, p64 length, positive chunks,
                                     split_output address_to output)
{
        bool range_copy = true;
        bool send_copy = true;
        p64 ordinary = length / chunks;
        positive extra = (positive)(length % chunks);

        for (positive i = 0; i < chunks; i++)
        {
                positive here = (positive)ordinary + (positive)(i < extra);

                if (!split_output_open(output) ||
                    (here && !split_copy_piece(in, output->handle, here,
                                               address_of range_copy,
                                               address_of send_copy)) ||
                    !split_output_close(output))
                {
                        file_fail("split: read or write error\n", 0);
                        return false;
                }
        }
        return true;
}

static bool split_distribute_stream(bipolar in, positive chunks,
                                    split_output address_to output)
{
        text_arena_used = 0;
        positive length;
        bool read_failed;
        p8 address_to input = text_arena_read_all(
            (positive)in, FILE_TRANSFER_SIZE, address_of length,
            address_of read_failed);

        if (!input)
        {
                file_fail(read_failed ? (string_address)"split: read error\n"
                                      : (string_address)"split: input too large\n",
                          0);
                text_arena_used = 0;
                return false;
        }

        positive ordinary = length / chunks;
        positive extra = length % chunks;
        positive at = 0;
        bool answer = true;

        for (positive i = 0; i < chunks; i++)
        {
                positive here = ordinary + (positive)(i < extra);
                if (!split_output_open(output) ||
                    (here && !split_output_write(output, input + at, here)) ||
                    !split_output_close(output))
                {
                        answer = false;
                        break;
                }
                at += here;
        }
        text_arena_used = 0;
        return answer;
}

static bool split_separator(string_address text, p8 address_to separator)
{
        if (string_get(text) && !string_get(text + 1))
        {
                address_to separator = string_get(text);
                return true;
        }
        if (string_is(text, '\\') && string_is(text + 1, '0') &&
            !string_get(text + 2))
        {
                address_to separator = 0;
                return true;
        }
        return false;
}

static b32 file_split()
{
        p8 suffix_kind = 0;
        file_supersede supersedes[] = {
            {(string_address)"dx", address_of suffix_kind},
            {null, null},
        };

        file_operands_begin();
        file_taking taking = {
            .program = (string_address)"split",
            .allowed = (string_address)"abCdlntx",
            .valued = (string_address)"abClnSt",
            /* Only the long spellings take an optional FROM.  In `-d7 -b3`,
               coreutils reads 7 as the old -7 line count and diagnoses the
               line/byte mode conflict; it is not a suffix start. */
            .long_optional = (string_address)"dx",
            .longs = split_longs,
            .digits = 'l',
            .operand = file_operand,
            .supersedes = supersedes,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;

        /* file_take rejects the combined -d7 spelling; with -b this is the
           same deliberate refusal and status as GNU's mode-conflict answer. */
        if (file_operand_count > 2)
        {
                file_fail("split: extra operand\n", 0);
                return 1;
        }

        bool bytes = (taking.flags & FILE_FLAG('b')) != 0;
        bool lines = (taking.flags & FILE_FLAG('l')) != 0;
        bool line_bytes = (taking.flags & FILE_FLAG('C')) != 0;
        bool distribute = (taking.flags & FILE_FLAG('n')) != 0;

        if ((positive)bytes + (positive)lines + (positive)line_bytes +
                (positive)distribute >
            1)
        {
                file_fail("split: cannot split in more than one way\n", 0);
                return 1;
        }

        p8 mode = bytes ? 'b' : line_bytes ? 'C' : distribute ? 'n' : 'l';

        positive piece = 1000;
        string_address measure = file_option_value(address_of taking, mode);

        if (measure && mode != 'n' &&
            !split_size(measure, address_of piece))
        {
                string_format(file_fail, "split: invalid number of bytes: '%s'\n",
                              measure);
                return 1;
        }

        positive chunks = 0;
        if (mode == 'n' && !split_chunks(measure, address_of chunks))
        {
                string_format(file_fail,
                              "split: unsupported number of chunks: '%s'\n",
                              measure);
                return 1;
        }

        positive suffix_length = 2;
        string_address width = file_option_value(address_of taking, 'a');

        if (width)
        {
                string_address at = width;

                if (!string_digits_checked(address_of at, 10,
                                           address_of suffix_length) ||
                    string_get(at) || !suffix_length ||
                    suffix_length > SPLIT_SUFFIX_MAX)
                {
                        string_format(file_fail,
                                      "split: invalid suffix length: '%s'\n",
                                      width);
                        return 1;
                }
        }

        p8 separator = '\n';
        string_address separator_text = file_option_value(address_of taking, 't');

        if (separator_text && !split_separator(separator_text,
                                               address_of separator))
        {
                file_fail("split: multi-character separator\n", 0);
                return 1;
        }

        string_address input_name = file_operand_count
                                        ? file_operand_at(0)
                                        : (string_address)"-";
        string_address prefix = file_operand_count > 1
                                    ? file_operand_at(1)
                                    : (string_address)"x";
        string_address additional = file_option_value(address_of taking, 'S');

        if (!additional)
                additional = (string_address)"";

        bipolar in = string_is(input_name, '-') && !string_get(input_name + 1)
                         ? 0
                         : system_open_at(AT_FDCWD, input_name, FILE_READ);

        if (in < 0)
        {
                string_format(file_fail, "split: cannot open '%s': %s\n",
                              input_name, file_reason(in));
                return 1;
        }

        split_output output = {
            .prefix = prefix,
            .additional = additional,
            .prefix_length = string_length(prefix),
            .additional_length = string_length(additional),
            .suffix_length = suffix_length,
            .radix = suffix_kind == 'd' ? 10 : suffix_kind == 'x' ? 16 : 0,
            .suffix_fixed = width != null,
            .verbose = (taking.flags & FILE_FLAG('v')) != 0,
            .handle = -1,
        };
        memory_fill(output.suffix, output.radix ? '0' : 'a', suffix_length);

        string_address first_suffix = suffix_kind
                                          ? file_option_value(address_of taking,
                                                              suffix_kind)
                                          : null;

        if (first_suffix)
        {
                string_address at = first_suffix;

                if (!string_digits_checked(address_of at, output.radix,
                                           address_of output.number) ||
                    string_get(at))
                {
                        string_format(file_fail,
                                      "split: invalid suffix start: '%s'\n",
                                      first_suffix);
                        if (in != 0)
                                system_close(in);
                        return 1;
                }
        }

        file_facts facts;
        bool looked = file_look(in, (string_address)"", AT_EMPTY_PATH,
                                address_of facts);

        if (looked && (facts.mode & MODE_FORMAT) == MODE_FILE)
        {
                output.protect_input = true;
                output.input = facts;
        }

        bool complete;

        bool regular = looked && (facts.mode & MODE_FORMAT) == MODE_FILE;

        if (mode == 'b' && output.protect_input && facts.size)
                complete = split_regular_bytes(in, facts.size, piece,
                                               address_of output);
        else if (mode == 'b')
                complete = split_stream_bytes(in, piece, address_of output);
        else if (mode == 'C')
                complete = split_line_bytes(in, address_of facts, regular,
                                            piece, separator,
                                            address_of output);
        else if (mode == 'n' && regular)
                complete = split_distribute_regular(in, facts.size, chunks,
                                                    address_of output);
        else if (mode == 'n')
                complete = split_distribute_stream(in, chunks,
                                                   address_of output);
        else
                complete = split_stream_lines(in, piece, separator,
                                              address_of output);

        if (output.handle >= 0)
                split_output_close(address_of output);
        if (in != 0)
                system_close(in);
        log_flush();
        return complete ? 0 : 1;
}

// csplit -----------------------------------------------------------
/*
        csplit is necessarily a look-ahead utility: a negative regular-
        expression offset can put the cut before the line which proved the
        match.  The input therefore uses text.c's existing grow-in-place arena
        reader, and pattern matching calls its existing BRE VM.  Sections are
        still written through the same system_write_all path as split; there
        is no second file reader, regex engine, or buffered writer here.
*/
#define CSPLIT_REGEX_POLICY 5 /* dot-newline | basic-repeat */

enum
{
        CSPLIT_LINE,
        CSPLIT_REGEX,
};

enum
{
        CSPLIT_EXECUTED,
        CSPLIT_NOT_FOUND,
        CSPLIT_FAILED,
};

typedef struct
{
        p8 kind;
        bool discard;
        bipolar offset;
        positive line_step;
        positive line_target;
        positive search_line;
        p8 expression[FILE_PATH_MAX];
} csplit_pattern;

typedef struct
{
        p8 address_to input;
        positive length;
        positive lines;
        positive cursor;
        positive cursor_line;
        positive next_search_line;
        string_address input_name;
        string_address prefix;
        positive prefix_length;
        positive digits;
        positive made;
        bool keep;
        bool quiet;
        bool elide;
        bool suppress_matched;
        bool suppress_final;
        bool protect_input;
        file_facts input_facts;
        p8 name[FILE_PATH_MAX];
} csplit_state;

static const file_long csplit_longs[] = {
    {(string_address)"digits", 'n'},
    {(string_address)"elide-empty-files", 'z'},
    {(string_address)"keep-files", 'k'},
    {(string_address)"prefix", 'f'},
    {(string_address)"quiet", 's'},
    {(string_address)"silent", 's'},
    {(string_address)"suppress-matched", 'M'},
    {null, 0},
};

static bool csplit_name(csplit_state address_to state, positive number)
{
        p8 suffix[32];
        positive length = positive_into_base(suffix, number, 10, false);
        positive width = length > state->digits ? length : state->digits;

        if (state->prefix_length >= FILE_PATH_MAX ||
            width >= FILE_PATH_MAX - state->prefix_length)
        {
                file_fail("csplit: output file name is too long\n", 0);
                return false;
        }

        memory_copy_apart(state->name, state->prefix, state->prefix_length);
        memory_fill(state->name + state->prefix_length, '0', width - length);
        memory_copy_end(state->name + state->prefix_length + width - length,
                        suffix, length);
        return true;
}

static bool csplit_same_input(csplit_state address_to state)
{
        if (!state->protect_input)
                return false;

        file_facts existing;

        if (!file_look_at(state->name, address_of existing))
                return false;

        return existing.inode == state->input_facts.inode &&
               existing.device_major == state->input_facts.device_major &&
               existing.device_minor == state->input_facts.device_minor;
}

static bool csplit_section(csplit_state address_to state, positive from,
                           positive to, bool emit)
{
        if (!emit)
                return true;
        if (to < from)
                return false;

        positive length = to - from;

        if (!length && state->elide)
                return true;
        if (!csplit_name(state, state->made))
                return false;
        if (csplit_same_input(state))
        {
                string_format(file_fail,
                              "csplit: '%s' would overwrite input; aborting\n",
                              state->name);
                return false;
        }

        bipolar out = system_open_at_mode(AT_FDCWD, state->name,
                                          FILE_WRITE, 0666);

        if (out < 0)
        {
                string_format(file_fail, "csplit: cannot open '%s': %s\n",
                              state->name, file_reason(out));
                return false;
        }

        state->made++;
        bool written = !length ||
                       system_write_all((positive)out, state->input + from,
                                        length) == length;
        bipolar closed = system_close(out);

        if (!written || closed < 0)
        {
                string_format(file_fail, "csplit: write error on '%s'\n",
                              state->name);
                return false;
        }

        if (!state->quiet)
        {
                positive_to_string(log, length);
                log("\n", 1);
        }

        return true;
}

static fn csplit_cleanup(csplit_state address_to state)
{
        if (state->keep)
                return;

        for (positive i = 0; i < state->made; i++)
                if (csplit_name(state, i))
                        system_remove_at(AT_FDCWD, state->name, 0);
}

static bool csplit_line_offset(csplit_state address_to state,
                               positive wanted, bool allow_end,
                               positive address_to offset)
{
        if (wanted < state->cursor_line ||
            wanted > state->lines + (positive)allow_end)
                return false;

        positive at = state->cursor;
        positive line = state->cursor_line;

        while (line < wanted)
        {
                p8 address_to found = memory_first_of(
                    state->input + at, '\n', state->length - at);

                if (found)
                        at = (positive)(found - state->input) + 1;
                else
                        at = state->length;
                line++;
        }

        if (at == state->length && !allow_end)
                return false;

        address_to offset = at;
        return true;
}

static bool csplit_signed(string_address text, bipolar address_to value)
{
        bool negative = string_is(text, '-');

        if (negative || string_is(text, '+'))
                text++;

        string_address at = text;
        positive magnitude;

        if (!string_digits_checked(address_of at, 10, address_of magnitude) ||
            string_get(at) ||
            magnitude > (positive)bipolar_max + (positive)negative)
                return false;

        address_to value = bipolar_from_magnitude(magnitude, negative);
        return true;
}

static bool csplit_parse_regex(string_address word,
                               csplit_pattern address_to pattern)
{
        p8 delimiter = string_get(word);
        positive source = 1;
        positive used = 0;

        if (delimiter != '/' && delimiter != '%')
                return false;

        while (string_get(word + source) &&
               string_get(word + source) != delimiter)
        {
                if (used + 2 >= sizeof(pattern->expression))
                        return false;

                p8 byte = string_get(word + source++);
                pattern->expression[used++] = byte;

                if (byte == '\\' && string_get(word + source))
                        pattern->expression[used++] = string_get(word + source++);
        }

        if (!string_is(word + source, delimiter) || !used)
                return false;

        pattern->expression[used] = end;
        pattern->kind = CSPLIT_REGEX;
        pattern->discard = delimiter == '%';
        source++;

        string_address offset = word + source;

        if (!string_get(offset))
                pattern->offset = 0;
        else if (!csplit_signed(offset, address_of pattern->offset))
                return false;

        return true;
}

static bool csplit_parse_line(string_address word,
                              csplit_pattern address_to pattern)
{
        string_address at = word;
        positive line;

        if (!string_digits_checked(address_of at, 10, address_of line) ||
            string_get(at) || !line)
                return false;

        pattern->kind = CSPLIT_LINE;
        pattern->discard = false;
        pattern->line_step = line;
        pattern->line_target = line;
        return true;
}

static bool csplit_repeat(string_address word, bool address_to forever,
                          positive address_to count)
{
        if (!string_is(word, '{'))
                return false;

        if (string_is(word + 1, '*') && string_is(word + 2, '}') &&
            !string_get(word + 3))
        {
                address_to forever = true;
                address_to count = 0;
                return true;
        }

        string_address at = word + 1;
        positive got;

        if (!string_digits_checked(address_of at, 10, address_of got) ||
            !string_is(at, '}') || string_get(at + 1))
                return false;

        address_to forever = false;
        address_to count = got;
        return true;
}

static b32 csplit_execute_line(csplit_state address_to state,
                               csplit_pattern address_to pattern,
                               bool repeated)
{
        positive target = pattern->line_target;

        if (repeated)
        {
                if (target > positive_max - pattern->line_step)
                        return CSPLIT_NOT_FOUND;
                target += pattern->line_step;
        }

        positive boundary;

        if (!csplit_line_offset(state, target, false, address_of boundary))
                return CSPLIT_NOT_FOUND;
        if (!csplit_section(state, state->cursor, boundary, true))
                return CSPLIT_FAILED;

        pattern->line_target = target;
        state->cursor = boundary;
        state->cursor_line = target;

        if (state->next_search_line < target)
                state->next_search_line = target;

        return CSPLIT_EXECUTED;
}

static bool csplit_find_regex(csplit_state address_to state,
                              csplit_pattern address_to pattern,
                              positive address_to matched_line,
                              positive address_to matched_at,
                              positive address_to matched_after)
{
        positive line = pattern->search_line;
        positive at;

        /* search_line is never behind cursor_line. */
        if (!csplit_line_offset(state, line, false, address_of at))
                return false;

        while (at < state->length)
        {
                p8 address_to newline = memory_first_of(
                    state->input + at, '\n', state->length - at);
                positive stop = newline ? (positive)(newline - state->input)
                                        : state->length;

                if (regex_search(state->input + at, stop - at, 0))
                {
                        address_to matched_line = line;
                        address_to matched_at = at;
                        address_to matched_after = newline ? stop + 1 : stop;
                        return true;
                }

                at = newline ? stop + 1 : stop;
                line++;
        }

        return false;
}

static b32 csplit_execute_regex(csplit_state address_to state,
                                csplit_pattern address_to pattern)
{
        positive matched_line;
        positive matched_at;
        positive matched_after;

        if (!csplit_find_regex(state, pattern, address_of matched_line,
                               address_of matched_at,
                               address_of matched_after))
                return CSPLIT_NOT_FOUND;

        positive target;

        if (pattern->offset < 0)
        {
                positive back = (positive)(-(pattern->offset + 1)) + 1;

                if (back >= matched_line)
                        return CSPLIT_NOT_FOUND;
                target = matched_line - back;
        }
        else
        {
                positive ahead = (positive)pattern->offset;

                if (matched_line > positive_max - ahead)
                        return CSPLIT_NOT_FOUND;
                target = matched_line + ahead;
        }

        positive boundary;

        if (!csplit_line_offset(state, target, true, address_of boundary))
                return CSPLIT_NOT_FOUND;

        if (!csplit_section(state, state->cursor, boundary,
                            !pattern->discard))
                return CSPLIT_FAILED;

        if (state->suppress_matched)
        {
                /* Offsets and suppression describe two different target
                   lines. Refuse that ambiguous combination until the exact
                   GNU ordering is represented. */
                if (pattern->offset)
                {
                        file_fail("csplit: --suppress-matched with an offset is unsupported\n",
                                  0);
                        return CSPLIT_FAILED;
                }
                state->cursor = matched_after;
                state->cursor_line = matched_line + 1;
        }
        else
        {
                state->cursor = boundary;
                state->cursor_line = target;
        }

        pattern->search_line = matched_line + 1;
        if (pattern->search_line < state->cursor_line)
                pattern->search_line = state->cursor_line;
        state->next_search_line = pattern->search_line;
        return CSPLIT_EXECUTED;
}

static b32 csplit_execute(csplit_state address_to state,
                          csplit_pattern address_to pattern, bool repeated)
{
        return pattern->kind == CSPLIT_LINE
                   ? csplit_execute_line(state, pattern, repeated)
                   : csplit_execute_regex(state, pattern);
}

static b32 file_csplit()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address)"csplit",
            .allowed = (string_address)"fknsz",
            .valued = (string_address)"fn",
            .longs = csplit_longs,
            .operand = file_operand,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;
        if (file_operand_count < 2)
        {
                file_fail("csplit: missing operand\n", 0);
                return 1;
        }

        positive digits = 2;
        string_address digit_text = file_option_value(address_of taking, 'n');

        if (digit_text)
        {
                string_address at = digit_text;

                if (!string_digits_checked(address_of at, 10,
                                           address_of digits) ||
                    string_get(at) || !digits || digits > 32)
                {
                        string_format(file_fail, "csplit: invalid number: '%s'\n",
                                      digit_text);
                        return 1;
                }
        }

        string_address input_name = file_operand_at(0);
        bipolar in = string_is(input_name, '-') && !string_get(input_name + 1)
                         ? 0
                         : system_open_at(AT_FDCWD, input_name, FILE_READ);

        if (in < 0)
        {
                string_format(file_fail, "csplit: cannot open '%s': %s\n",
                              input_name, file_reason(in));
                return 1;
        }

        file_facts facts;
        bool looked = file_look(in, (string_address)"", AT_EMPTY_PATH,
                                address_of facts);

        text_arena_used = 0;
        positive length;
        bool read_failed;
        p8 address_to input = text_arena_read_all(
            (positive)in, FILE_TRANSFER_SIZE, address_of length,
            address_of read_failed);

        if (in != 0)
                system_close(in);

        if (!input)
        {
                file_fail(read_failed ? "csplit: read error\n"
                                      : "csplit: input too large\n", 0);
                text_arena_used = 0;
                return 1;
        }

        csplit_state state = {
            .input = input,
            .length = length,
            .lines = memory_count(input, length, '\n') +
                     (positive)(length && input[length - 1] != '\n'),
            .cursor = 0,
            .cursor_line = 1,
            .next_search_line = 1,
            .input_name = input_name,
            .prefix = file_option_value(address_of taking, 'f'),
            .digits = digits,
            .keep = (taking.flags & FILE_FLAG('k')) != 0,
            .quiet = (taking.flags & FILE_FLAG('s')) != 0,
            .elide = (taking.flags & FILE_FLAG('z')) != 0,
            .suppress_matched = (taking.flags & FILE_FLAG('M')) != 0,
        };

        if (!state.prefix)
                state.prefix = (string_address)"xx";
        state.prefix_length = string_length(state.prefix);

        if (looked && (facts.mode & MODE_FORMAT) == MODE_FILE)
        {
                state.protect_input = true;
                state.input_facts = facts;
        }

        csplit_pattern pattern;
        memory_fill(address_of pattern, 0, sizeof(pattern));
        bool have_pattern = false;
        bool failed = false;

        for (positive i = 1; i < file_operand_count && !failed; i++)
        {
                string_address word = file_operand_at(i);
                bool forever;
                positive repeats;

                if (csplit_repeat(word, address_of forever,
                                  address_of repeats))
                {
                        if (!have_pattern)
                        {
                                file_fail("csplit: repeat with no previous pattern\n", 0);
                                failed = true;
                                break;
                        }

                        positive repetition = 0;

                        while (forever || repetition < repeats)
                        {
                                b32 done = csplit_execute(address_of state,
                                                         address_of pattern,
                                                         true);

                                if (done == CSPLIT_EXECUTED)
                                {
                                        repetition++;
                                        continue;
                                }
                                if (done == CSPLIT_NOT_FOUND && forever)
                                {
                                        /* A repeated %pattern% consumes the
                                           unmatched tail as part of the
                                           suppressed search. */
                                        if (pattern.discard)
                                                state.suppress_final = true;
                                        break;
                                }

                                if (done == CSPLIT_NOT_FOUND)
                                {
                                        if (!csplit_section(
                                                address_of state, state.cursor,
                                                length, !pattern.discard))
                                        {
                                                failed = true;
                                                break;
                                        }
                                        string_format(file_fail,
                                                      "csplit: '%s': match not found on repetition %u\n",
                                                      word, repetition + 1);
                                }
                                failed = true;
                                break;
                        }
                        continue;
                }

                memory_fill(address_of pattern, 0, sizeof(pattern));

                if (string_is(word, '/') || string_is(word, '%'))
                {
                        if (!csplit_parse_regex(word, address_of pattern))
                        {
                                string_format(file_fail,
                                              "csplit: invalid pattern: '%s'\n", word);
                                failed = true;
                                break;
                        }
                        if (state.suppress_matched && pattern.offset)
                        {
                                file_fail("csplit: --suppress-matched with an offset is unsupported\n",
                                          0);
                                failed = true;
                                break;
                        }
                        if (!regex_compile(pattern.expression, false, false,
                                           false, CSPLIT_REGEX_POLICY))
                        {
                                string_format(file_fail,
                                              "csplit: invalid regular expression: '%s'\n",
                                              pattern.expression);
                                failed = true;
                                break;
                        }
                        pattern.search_line = state.next_search_line;

                        if (pattern.search_line < state.cursor_line)
                                pattern.search_line = state.cursor_line;
                }
                else if (!csplit_parse_line(word, address_of pattern))
                {
                        string_format(file_fail, "csplit: invalid pattern: '%s'\n",
                                      word);
                        failed = true;
                        break;
                }

                have_pattern = true;
                b32 done = csplit_execute(address_of state, address_of pattern,
                                          false);

                if (done != CSPLIT_EXECUTED)
                {
                        if (done == CSPLIT_NOT_FOUND)
                        {
                                if (!csplit_section(
                                        address_of state, state.cursor, length,
                                        !pattern.discard))
                                {
                                        failed = true;
                                        break;
                                }
                                string_format(file_fail,
                                              "csplit: '%s': match not found\n", word);
                        }
                        failed = true;
                }
        }

        if (!failed && !state.suppress_final &&
            !csplit_section(address_of state, state.cursor, length, true))
                failed = true;

        if (failed)
                csplit_cleanup(address_of state);

        log_flush();
        text_arena_used = 0;
        return failed ? 1 : 0;
}

// truncate ---------------------------------------------------------

enum
{
        TRUNCATE_ABSOLUTE,
        TRUNCATE_RELATIVE,
        TRUNCATE_AT_MOST,
        TRUNCATE_AT_LEAST,
        TRUNCATE_ROUND_DOWN,
        TRUNCATE_ROUND_UP,
};

static const file_long truncate_longs[] = {
    {(string_address) "no-create", 'c'},
    {(string_address) "io-blocks", 'o'},
    {(string_address) "reference", 'r'},
    {(string_address) "size", 's'},
    {null, 0},
};

/* The exponent is shared by dd, truncate and util-linux's strtosize. Their
   surrounding grammars deliberately are not: callers keep their own accepted
   case, range and trailing-unit rules. A direct ASCII table keeps this cold
   parser smaller and branchless instead of spelling three switches. */
static PURE p8 file_size_power(p8 suffix, bool every_lower)
{
        static const p8 powers['z' - 'A' + 1] = {
            ['K' - 'A'] = 1, ['M' - 'A'] = 2, ['G' - 'A'] = 3,
            ['T' - 'A'] = 4, ['P' - 'A'] = 5, ['E' - 'A'] = 6,
            ['Z' - 'A'] = 7, ['Y' - 'A'] = 8, ['R' - 'A'] = 9,
            ['Q' - 'A'] = 10,
            ['k' - 'A'] = 1, ['m' - 'A'] = 2, ['g' - 'A'] = 3,
            ['t' - 'A'] = 4, ['p' - 'A'] = 5, ['e' - 'A'] = 6,
            ['z' - 'A'] = 7, ['y' - 'A'] = 8, ['r' - 'A'] = 9,
            ['q' - 'A'] = 10,
        };

        if (suffix < 'A' || suffix > 'z')
                return 0;

        p8 power = powers[suffix - 'A'];

        return suffix >= 'a' && !every_lower && power > 4
                   ? 0
                   : power;
}

/* GNU's SIZE grammar here is deliberately narrower than dd's: an integer,
   optionally followed by K..Q, with bare suffixes meaning one. A trailing B
   selects powers of 1000; no B or iB selects powers of 1024. */
static bool truncate_size(string_address text, b64 address_to out,
                          p8 address_to relation)
{
        while (byte_is_space(string_get(text)))
                text++;

        p8 mode = TRUNCATE_ABSOLUTE;

        if (string_is(text, '<'))
                mode = TRUNCATE_AT_MOST;
        else if (string_is(text, '>'))
                mode = TRUNCATE_AT_LEAST;
        else if (string_is(text, '/'))
                mode = TRUNCATE_ROUND_DOWN;
        else if (string_is(text, '%'))
                mode = TRUNCATE_ROUND_UP;

        if (mode != TRUNCATE_ABSOLUTE)
        {
                text++;

                while (byte_is_space(string_get(text)))
                        text++;
        }

        bool negative = string_is(text, '-');

        if (negative || string_is(text, '+'))
        {
                if (mode != TRUNCATE_ABSOLUTE)
                        return false;

                mode = TRUNCATE_RELATIVE;
                text++;
        }

        // Nineteen digits always fit; twenty are either past the unsigned
        // range or past the signed one every answer is checked against
        // below, so a run that reaches twenty is refused before it can wrap.
        positive digits;
        p64 magnitude = string_digits_max(text, 20, address_of digits);

        if (digits == 20)
                return false;

        text += digits;

        positive power = file_size_power(string_get(text), false);

        if (!digits && !power)
                return false;

        if (power)
        {
                if (!digits)
                        magnitude = 1;

                text++;
                p64 base = 1024;

                if (string_is(text, 'B'))
                {
                        base = 1000;
                        text++;
                }
                else if (string_is(text, 'i') && string_is(text + 1, 'B'))
                        text += 2;

                while (power--)
                {
                        if (magnitude > (p64)b64_max / base)
                                return false;

                        magnitude *= base;
                }
        }

        if (string_get(text) || magnitude > (p64)b64_max + (p64)negative)
                return false;

        if ((mode == TRUNCATE_ROUND_DOWN || mode == TRUNCATE_ROUND_UP) &&
            !magnitude)
                return false;

        address_to out = negative
                             ? (magnitude == (p64)b64_max + 1
                                    ? b64_min
                                    : -(b64)magnitude)
                             : (b64)magnitude;
        address_to relation = mode;
        return true;
}

static bool truncate_current_size(string_address path, bipolar handle,
                                  file_facts address_to facts,
                                  b64 address_to out)
{
        bipolar size;

        if ((facts->mode & MODE_FORMAT) == MODE_FILE)
                size = facts->size > (p64)b64_max ? -ERROR_INVALID
                                                  : (b64)facts->size;
        else
                size = system_seek(handle, 0, FILE_SEEK_END);

        if (size < 0)
        {
                string_format(file_fail,
                              "truncate: cannot get the size of '%s': %s\n",
                              path, file_reason(size));
                return false;
        }

        address_to out = size;
        return true;
}

static bool truncate_one(string_address path, b64 size, b64 reference,
                         p8 relation, bool no_create, bool blocks)
{
        positive flags = (FILE_WRITE & ~O_TRUNC) | O_NONBLOCK;

        if (no_create)
                flags &= ~O_CREAT;

        bipolar handle = system_open_at_mode(AT_FDCWD,
                                       path, flags, 0666);

        if (handle < 0)
        {
                if (no_create && handle == -ERROR_NO_ENTRY)
                        return true;

                string_format(file_fail, "truncate: cannot open '%s' for writing: %s\n",
                              path, file_reason(handle));
                return false;
        }

        file_facts facts;
        bool need_facts = blocks || (relation && reference < 0);

        if (need_facts &&
            !file_look(handle, (string_address) "", AT_EMPTY_PATH,
                       address_of facts))
        {
                file_complain((string_address) "truncate", (string_address) "cannot stat",
                              path);
                system_close(handle);
                return false;
        }

        if (blocks)
        {
                p64 block = facts.blocksize;

                if (!block || size > b64_max / (b64)block ||
                    size < b64_min / (b64)block)
                {
                        file_complain((string_address) "truncate", (string_address) "size overflow",
                                      path);
                        system_close(handle);
                        return false;
                }

                size *= (b64)block;
        }

        b64 current = reference;

        if (relation && reference < 0 &&
            !truncate_current_size(path, handle, address_of facts,
                                   address_of current))
        {
                system_close(handle);
                return false;
        }

        b64 wanted = size;
        bool overflow = false;

        if (relation == TRUNCATE_AT_MOST)
                wanted = current < size ? current : size;
        else if (relation == TRUNCATE_AT_LEAST)
                wanted = current > size ? current : size;
        else if (relation == TRUNCATE_ROUND_DOWN)
                wanted = current - current % size;
        else if (relation == TRUNCATE_ROUND_UP)
        {
                b64 spare = current % size;
                b64 add = spare ? size - spare : 0;

                overflow = current > b64_max - add;
                wanted = overflow ? 0 : current + add;
        }
        else if (relation == TRUNCATE_RELATIVE)
        {
                /* A file size is nonnegative, so adding a negative signed
                   SIZE cannot cross INT64_MIN. Only extension can overflow. */
                overflow = size > 0 && current > b64_max - size;
                wanted = overflow ? 0 : current + size;
        }

        if (overflow)
        {
                file_complain((string_address) "truncate", (string_address) "size overflow",
                              path);
                system_close(handle);
                return false;
        }

        if (wanted < 0)
                wanted = 0;

        bipolar done = system_truncate_handle(handle, wanted);
        bipolar closed = system_close(handle);

        if (done < 0)
        {
                string_format(file_fail, "truncate: failed to truncate '%s': %s\n",
                              path, file_reason(done));
                return false;
        }

        if (closed < 0)
        {
                string_format(file_fail, "truncate: failed to close '%s': %s\n",
                              path, file_reason(closed));
                return false;
        }

        return true;
}

static b32 file_truncate()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address) "truncate",
            .allowed = (string_address) "cors",
            .valued = (string_address) "rs",
            .longs = truncate_longs,
            .operand = file_operand,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;

        string_address size_text = file_option_value(address_of taking, 's');
        string_address reference_path = file_option_value(address_of taking, 'r');
        bool blocks = (taking.flags & FILE_FLAG('o')) != 0;
        bool no_create = (taking.flags & FILE_FLAG('c')) != 0;
        b64 size = 0;
        p8 relation = TRUNCATE_ABSOLUTE;

        if (!reference_path && !size_text)
        {
                file_fail("truncate: you must specify either --size or --reference\n", 0);
                return 1;
        }

        if (size_text && !truncate_size(size_text, address_of size,
                                        address_of relation))
        {
                string_format(file_fail, "truncate: Invalid number: '%s'\n", size_text);
                return 1;
        }

        if (reference_path && size_text && relation == TRUNCATE_ABSOLUTE)
        {
                file_fail("truncate: --size must be relative with --reference\n", 0);
                return 1;
        }

        if (blocks && !size_text)
        {
                file_fail("truncate: --io-blocks requires --size\n", 0);
                return 1;
        }

        if (!file_operand_count)
        {
                file_fail("truncate: missing file operand\n", 0);
                return 1;
        }

        b64 reference = -1;

        if (reference_path)
        {
                file_facts facts;

                if (!file_look_at(reference_path, address_of facts))
                {
                        string_format(file_fail, "truncate: cannot stat '%s'\n",
                                      reference_path);
                        return 1;
                }

                bipolar handle = -1;

                if ((facts.mode & MODE_FORMAT) != MODE_FILE)
                        handle = system_open_at(AT_FDCWD,
                                               reference_path,
                                               FILE_READ);

                if (handle < 0 && (facts.mode & MODE_FORMAT) != MODE_FILE)
                {
                        string_format(file_fail,
                                      "truncate: cannot get the size of '%s': %s\n",
                                      reference_path, file_reason(handle));
                        return 1;
                }

                bool known = truncate_current_size(reference_path, handle,
                                                   address_of facts,
                                                   address_of reference);

                if (handle >= 0)
                        system_close(handle);

                if (!known)
                        return 1;

                if (!size_text)
                        size = reference;
        }

        b32 status = 0;

        for (positive i = 0; i < file_operand_count; i++)
                if (!truncate_one(file_operand_at(i), size, reference,
                                  relation, no_create, blocks))
                        status = 1;

        log_flush();
        return status;
}

// shred ------------------------------------------------------------
/*
        This is deliberately a regular-file utility.  Pipes can block before
        their kind is known, devices have media-specific erase contracts, and
        accepting either as though ordinary writes were a secure erase would
        be worse than refusing them.

        The transfer buffer, checked write loop, size grammar, statx shape,
        truncate and unlink paths are the same ones used by cp/split/truncate.
        Random streams are seeded directly from Linux's CSPRNG. fdatasync
        after every pass preserves pass order at the filesystem boundary; it cannot make
        copy-on-write filesystems, snapshots, flash translation layers,
        mirrors or backups overwrite their older physical copies.
*/
static const file_long shred_longs[] = {
    {(string_address) "exact", 'x'},
    {(string_address) "force", 'f'},
    {(string_address) "iterations", 'n'},
    {(string_address) "random-source", 'R'},
    {(string_address) "remove", 'u'},
    {(string_address) "size", 's'},
    {(string_address) "verbose", 'v'},
    {(string_address) "zero", 'z'},
    {null, 0},
};

static bool shred_number(string_address text, positive address_to number)
{
        string_address at = text;
        positive value;

        if (!string_digits_checked(address_of at, 10, address_of value) ||
            string_get(at))
                return false;

        address_to number = value;
        return true;
}

static bool shred_size(string_address text, positive address_to size)
{
        if (string_is(text, '0') && !string_get(text + 1))
        {
                address_to size = 0;
                return true;
        }

        return split_size(text, size);
}

typedef struct
{
        p64 words[4];
} file_random_state;

typedef p64 shred_random_word_type
    __attribute__((aligned(1), may_alias));

static bool file_random_seed(file_random_state address_to state)
{
        positive filled = 0;

        while (filled < sizeof(state->words))
        {
                bipolar got = system_call_3(syscall(getrandom),
                                             (positive)((p8 address_to)state +
                                                        filled),
                                             sizeof(state->words) - filled, 0);

                if (got == -4)
                        continue;
                if (got <= 0)
                        return false;

                filled += (positive)got;
        }

        return state->words[0] || state->words[1] || state->words[2] ||
               state->words[3];
}

static inline INLINE p64 file_random_rotate(p64 value, positive shift)
{
        return (value << shift) | (value >> (64 - shift));
}

/* xoshiro256** expands one kernel seed at register speed.  Erasure needs an
   unpredictable starting stream, not one entropy syscall per output block;
   the latter measured five times slower than GNU on the 64 MiB hot path. */
static inline INLINE p64 file_random_word(file_random_state address_to state)
{
        p64 result = file_random_rotate(state->words[1] * 5, 7) * 9;
        p64 shifted = state->words[1] << 17;

        state->words[2] ^= state->words[0];
        state->words[3] ^= state->words[1];
        state->words[1] ^= state->words[2];
        state->words[0] ^= state->words[3];
        state->words[2] ^= shifted;
        state->words[3] = file_random_rotate(state->words[3], 45);
        return result;
}

static fn shred_random_fill(file_random_state address_to state,
                            p8 address_to bytes, positive length)
{
        positive words = length / sizeof(p64);
        shred_random_word_type address_to output =
            (shred_random_word_type address_to)bytes;

        for (positive i = 0; i < words; i++)
                output[i] = file_random_word(state);

        positive filled = words * sizeof(p64);

        if (filled < length)
        {
                p64 final = file_random_word(state);

                for (positive i = 0; filled + i < length; i++)
                        bytes[filled + i] = (p8)(final >> (i * 8));
        }
}

static bool shred_sync(bipolar handle, string_address path, bool data)
{
        bipolar done;

        do
                done = system_call_1(data ? syscall(fdatasync) : syscall(fsync),
                                     (positive)handle);
        while (done == -4);

        if (done >= 0)
                return true;

        string_format(file_fail, "shred: '%s': sync failed: %s\n", path,
                      file_reason(done));
        return false;
}

static bool shred_pass(bipolar handle, string_address path, positive length,
                       bool zero, file_random_state address_to random)
{
        if (system_seek(handle, 0, FILE_SEEK_SET) < 0)
        {
                string_format(file_fail, "shred: '%s': seek failed\n", path);
                return false;
        }

        if (zero)
                memory_fill(file_transfer, 0, sizeof(file_transfer));

        positive left = length;

        while (left)
        {
                positive chunk = left < sizeof(file_transfer)
                                     ? left : sizeof(file_transfer);

                if (!zero)
                        shred_random_fill(random, file_transfer, chunk);

                if (system_write_all((positive)handle, file_transfer, chunk) !=
                    chunk)
                {
                        string_format(file_fail, "shred: '%s': write failed\n",
                                      path);
                        return false;
                }

                left -= chunk;
        }

        return !length || shred_sync(handle, path, true);
}

static bipolar shred_open(string_address path, bool force)
{
        positive flags = (FILE_WRITE & ~(O_TRUNC | FILE_CREATE)) | O_NONBLOCK;
        bipolar handle = system_open_at(AT_FDCWD, path, flags);

        if (handle >= 0 || !force ||
            (handle != -ERROR_ACCESS && handle != -ERROR_NOT_PERMITTED))
                return handle;

        file_facts facts;

        if (!file_look_at(path, address_of facts) ||
            (facts.mode & MODE_FORMAT) != MODE_FILE ||
            system_change_mode_at(AT_FDCWD, path, 0200) < 0)
                return handle;

        return system_open_at(AT_FDCWD, path, flags);
}

static bool shred_one(string_address path, positive iterations,
                      bool size_given, positive requested, bool exact,
                      bool zero, bool remove, bool force, bool verbose)
{
        bipolar handle = shred_open(path, force);

        if (handle < 0)
        {
                string_format(file_fail, "shred: '%s': cannot open: %s\n", path,
                              file_reason(handle));
                return false;
        }

        file_facts facts;
        bool good = file_look(handle, (string_address) "", AT_EMPTY_PATH,
                              address_of facts);

        if (!good || (facts.mode & MODE_FORMAT) != MODE_FILE)
        {
                string_format(file_fail,
                              "shred: '%s': refusing non-regular file\n", path);
                system_close(handle);
                return false;
        }

        positive length;

        if (size_given)
                length = requested;
        else if (facts.size > positive_max)
        {
                string_format(file_fail, "shred: '%s': file is too large\n", path);
                system_close(handle);
                return false;
        }
        else
                length = (positive)facts.size;

        if (!size_given && !exact && length)
        {
                positive block = facts.blocksize ? facts.blocksize : FILE_BLOCK;
                positive spare = length % block;

                if (spare)
                {
                        positive add = block - spare;

                        if (length > positive_max - add)
                        {
                                string_format(file_fail,
                                              "shred: '%s': size overflow\n", path);
                                system_close(handle);
                                return false;
                        }

                        length += add;
                }
        }

        if (verbose)
                string_format(file_fail,
                              "shred: '%s': caution: storage layers may retain old copies\n",
                              path);

        file_random_state random;

        if (iterations && good && !file_random_seed(address_of random))
        {
                string_format(file_fail,
                              "shred: '%s': kernel randomness unavailable\n", path);
                good = false;
        }

        for (positive pass = 0; pass < iterations && good; pass++)
        {
                if (verbose)
                        string_format(file_fail,
                                      "shred: '%s': pass %u/%u (random)\n",
                                      path, pass + 1, iterations);

                good = shred_pass(handle, path, length, false,
                                  address_of random);
        }

        if (zero && good)
        {
                if (verbose)
                        string_format(file_fail, "shred: '%s': pass (zero)\n",
                                      path);

                good = shred_pass(handle, path, length, true, null);
        }

        if (remove && good)
        {
                bipolar shortened = system_truncate_handle(handle, 0);

                if (shortened < 0)
                {
                        string_format(file_fail,
                                      "shred: '%s': cannot truncate before removal: %s\n",
                                      path, file_reason(shortened));
                        good = false;
                }
                else
                        good = shred_sync(handle, path, false);
        }

        bipolar closed = system_close(handle);

        if (closed < 0)
        {
                string_format(file_fail, "shred: '%s': close failed: %s\n", path,
                              file_reason(closed));
                good = false;
        }

        if (remove && good)
        {
                file_facts named;

                if (!file_look_at(path, address_of named) ||
                    !file_same_identity(address_of facts, address_of named))
                {
                        string_format(file_fail,
                                      "shred: '%s': name changed; refusing removal\n",
                                      path);
                        good = false;
                }
                else
                {
                        bipolar gone = system_remove_at(AT_FDCWD, path, 0);

                        if (gone < 0)
                        {
                                string_format(file_fail,
                                              "shred: '%s': cannot remove: %s\n",
                                              path, file_reason(gone));
                                good = false;
                        }
                        else if (verbose)
                                string_format(file_fail, "shred: '%s': removed\n",
                                              path);
                }
        }

        return good;
}

static b32 file_shred()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address) "shred",
            .allowed = (string_address) "fnsuvxz",
            .valued = (string_address) "nRs",
            .long_optional = (string_address) "u",
            .longs = shred_longs,
            .operand = file_operand,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;
        if (!file_operand_count)
                return file_missing((string_address) "shred");

        if (file_option_value(address_of taking, 'R'))
        {
                file_fail("shred: --random-source is unsupported; kernel randomness is mandatory\n",
                          0);
                return 1;
        }

        string_address remove_how = file_option_value(address_of taking, 'u');

        if (remove_how && !string_equals(remove_how, (string_address) "unlink"))
        {
                file_fail("shred: filename wiping modes are unsupported; use --remove=unlink\n",
                          0);
                return 1;
        }

        positive iterations = 3;
        string_address iteration_text = file_option_value(address_of taking, 'n');

        if (iteration_text &&
            !shred_number(iteration_text, address_of iterations))
        {
                string_format(file_fail, "shred: invalid number of passes: '%s'\n",
                              iteration_text);
                return 1;
        }

        positive size = 0;
        string_address size_text = file_option_value(address_of taking, 's');

        if (size_text && !shred_size(size_text, address_of size))
        {
                string_format(file_fail, "shred: invalid size: '%s'\n", size_text);
                return 1;
        }

        positive flags = taking.flags;
        bool remove = (flags & FILE_FLAG('u')) != 0;
        b32 status = 0;

        if (remove && !remove_how)
                file_fail("shred: warning: -u uses unlink removal without filename wiping\n",
                          0);

        for (positive i = 0; i < file_operand_count; i++)
                if (!shred_one(file_operand_at(i), iterations, size_text != null,
                               size, (flags & FILE_FLAG('x')) != 0,
                               (flags & FILE_FLAG('z')) != 0, remove,
                               (flags & FILE_FLAG('f')) != 0,
                               (flags & FILE_FLAG('v')) != 0))
                        status = 1;

        log_flush();
        return status;
}

// shuf -------------------------------------------------------------
/* One arena load, one compact record table, and a partial Fisher-Yates walk.
   -n therefore pays for only the selections it emits.  Every bounded choice
   uses rejection sampling: modulo bias is small enough to hide in ordinary
   tests and still wrong enough not to put in a sampling utility. */
typedef struct
{
        string_address text;
        positive length;
} shuf_record;

typedef struct
{
        bipolar handle;
        positive used;
        string_address name;
        bool opened;
} shuf_output;

static const file_long shuf_longs[] = {
    {(string_address) "echo", 'e'},
    {(string_address) "head-count", 'n'},
    {(string_address) "input-range", 'i'},
    {(string_address) "output", 'o'},
    {(string_address) "random-source", 'R'},
    {(string_address) "repeat", 'r'},
    {(string_address) "zero-terminated", 'z'},
    {null, 0},
};

static bool shuf_range(string_address text, positive address_to low,
                       positive address_to high)
{
        string_address at = text;
        positive first;
        positive last;

        if (!string_digits_checked(address_of at, 10, address_of first) ||
            !string_is(at, '-'))
                return false;

        at++;

        if (!string_digits_checked(address_of at, 10, address_of last) ||
            string_get(at) || last < first)
                return false;

        address_to low = first;
        address_to high = last;
        return true;
}

static positive shuf_uniform(file_random_state address_to random,
                             positive bound)
{
        if (bound < 2)
                return 0;

        p64 width = (p64)bound;
        p64 threshold = (0 - width) % width;
        p64 value;

        do
                value = file_random_word(random);
        while (value < threshold);

        return (positive)(value % width);
}

static bool shuf_output_flush(shuf_output address_to output)
{
        if (!output->used)
                return true;

        if (system_write_all((positive)output->handle, file_transfer,
                             output->used) != output->used)
        {
                string_format(file_fail, "shuf: write error%s%s\n",
                              output->name ? (string_address) " on "
                                           : (string_address) "",
                              output->name ? output->name
                                           : (string_address) "");
                return false;
        }

        output->used = 0;
        return true;
}

static bool shuf_output_send(shuf_output address_to output,
                             string_address bytes, positive length)
{
        while (length)
        {
                if (!output->used && length >= sizeof(file_transfer))
                {
                        if (system_write_all((positive)output->handle, bytes,
                                             length) != length)
                        {
                                string_format(file_fail,
                                              "shuf: write error%s%s\n",
                                              output->name
                                                  ? (string_address) " on "
                                                  : (string_address) "",
                                              output->name
                                                  ? output->name
                                                  : (string_address) "");
                                return false;
                        }

                        return true;
                }

                positive room = sizeof(file_transfer) - output->used;
                positive copied = length < room ? length : room;

                memory_copy_apart(file_transfer + output->used, bytes, copied);
                output->used += copied;
                bytes += copied;
                length -= copied;

                if (output->used == sizeof(file_transfer) &&
                    !shuf_output_flush(output))
                        return false;
        }

        return true;
}

static bool shuf_output_record(shuf_output address_to output,
                               shuf_record address_to record, p8 delimiter)
{
        return shuf_output_send(output, record->text, record->length) &&
               shuf_output_send(output, address_of delimiter, 1);
}

static bool shuf_output_number(shuf_output address_to output, positive number,
                               p8 delimiter)
{
        p8 text[32];
        positive length = positive_into_base(text, number, 10, false);

        return shuf_output_send(output, text, length) &&
               shuf_output_send(output, address_of delimiter, 1);
}

static shuf_record address_to shuf_file_records(string_address name,
                                                p8 delimiter,
                                                positive address_to count)
{
        bipolar handle = !name || (string_is(name, '-') && !string_get(name + 1))
                             ? 0
                             : system_open_at(AT_FDCWD, name, FILE_READ);

        if (handle < 0)
        {
                string_format(file_fail, "shuf: cannot open '%s': %s\n", name,
                              file_reason(handle));
                return null;
        }

        positive length;
        bool read_failed;
        p8 address_to input = text_arena_read_all(
            (positive)handle, FILE_TRANSFER_SIZE, address_of length,
            address_of read_failed);

        if (handle != 0)
                system_close(handle);

        if (!input)
        {
                file_fail(read_failed ? (string_address) "shuf: read error\n"
                                      : (string_address) "shuf: input too large\n",
                          0);
                return null;
        }

        positive records = memory_count(input, length, delimiter) +
                           (positive)(length && input[length - 1] != delimiter);
        shuf_record address_to table = records
            ? (shuf_record address_to)text_arena_take(records * sizeof(*table))
            : (shuf_record address_to)input;

        if (!table)
                return null;

        positive at = 0;

        for (positive i = 0; i < records; i++)
        {
                p8 address_to found = memory_first_of(input + at, delimiter,
                                                      length - at);
                positive stop = found ? (positive)(found - input) : length;

                table[i].text = input + at;
                table[i].length = stop - at;
                at = found ? stop + 1 : stop;
        }

        address_to count = records;
        return table;
}

static shuf_record address_to shuf_echo_records(positive address_to count)
{
        positive records = file_operand_count;
        shuf_record address_to table = records
            ? (shuf_record address_to)text_arena_take(records * sizeof(*table))
            : (shuf_record address_to)(positive)1;

        if (!table)
                return null;

        for (positive i = 0; i < records; i++)
        {
                table[i].text = file_operand_at(i);
                table[i].length = string_length(table[i].text);
        }

        address_to count = records;
        return table;
}

static bool shuf_emit_records(shuf_output address_to output,
                              shuf_record address_to records, positive count,
                              positive wanted, bool limited, bool repeat,
                              p8 delimiter, file_random_state address_to random)
{
        if (repeat)
        {
                positive made = 0;

                while (!limited || made < wanted)
                {
                        positive chosen = shuf_uniform(random, count);

                        if (!shuf_output_record(output, records + chosen,
                                                delimiter))
                                return false;
                        made++;
                }

                return true;
        }

        positive take = limited && wanted < count ? wanted : count;

        for (positive made = 0; made < take; made++)
        {
                positive chosen = made + shuf_uniform(random, count - made);
                shuf_record held = records[made];

                records[made] = records[chosen];
                records[chosen] = held;

                if (!shuf_output_record(output, records + made, delimiter))
                        return false;
        }

        return true;
}

static bool shuf_set_add(positive address_to set, positive mask,
                         positive value)
{
        positive stored = value + 1;
        positive slot = (value * 11400714819323198485u) & mask;

        while (set[slot])
        {
                if (set[slot] == stored)
                        return false;
                slot = (slot + 1) & mask;
        }

        set[slot] = stored;
        return true;
}

/* Floyd's selection avoids constructing a billion-element range to answer
   `-i 1-1000000000 -n 5`.  Its set is uniformly sampled; the final small
   Fisher-Yates pass makes the order uniform too. */
static bool shuf_emit_sparse_range(shuf_output address_to output, positive low,
                                   positive count, positive take, p8 delimiter,
                                   file_random_state address_to random)
{
        if (take > positive_max / 2 ||
            take > positive_max / sizeof(positive))
                return false;

        positive wanted = take * 2;
        positive capacity = 1;

        while (capacity < wanted)
        {
                if (capacity > positive_max / 2)
                        return false;
                capacity *= 2;
        }

        if (capacity > positive_max / sizeof(positive))
                return false;

        positive address_to selected =
            (positive address_to)text_arena_take(take * sizeof(positive));
        positive address_to set = (positive address_to)text_arena_take(
            capacity * sizeof(positive));

        if (!selected || !set)
                return false;

        memory_fill(set, 0, capacity * sizeof(positive));

        positive first = count - take;

        for (positive made = 0; made < take; made++)
        {
                positive last = first + made;
                positive chosen = shuf_uniform(random, last + 1);

                if (!shuf_set_add(set, capacity - 1, chosen))
                {
                        chosen = last;
                        shuf_set_add(set, capacity - 1, chosen);
                }

                selected[made] = chosen;
        }

        for (positive left = take; left > 1; left--)
        {
                positive chosen = shuf_uniform(random, left);
                positive held = selected[left - 1];

                selected[left - 1] = selected[chosen];
                selected[chosen] = held;
        }

        for (positive made = 0; made < take; made++)
                if (!shuf_output_number(output, low + selected[made],
                                        delimiter))
                        return false;

        return true;
}

static bool shuf_emit_range(shuf_output address_to output, positive low,
                            positive count, positive wanted, bool limited,
                            bool repeat, p8 delimiter,
                            file_random_state address_to random)
{
        if (repeat)
        {
                positive made = 0;

                while (!limited || made < wanted)
                {
                        positive chosen = shuf_uniform(random, count);

                        if (!shuf_output_number(output, low + chosen, delimiter))
                                return false;
                        made++;
                }

                return true;
        }

        positive take = limited && wanted < count ? wanted : count;

        if (!take)
                return true;
        if (take <= count / 4)
                return shuf_emit_sparse_range(output, low, count, take,
                                              delimiter, random);
        if (count > positive_max / sizeof(positive))
                return false;

        positive address_to numbers =
            (positive address_to)text_arena_take(count * sizeof(positive));

        if (!numbers)
                return false;

        for (positive i = 0; i < count; i++)
                numbers[i] = low + i;

        for (positive made = 0; made < take; made++)
        {
                positive chosen = made + shuf_uniform(random, count - made);
                positive held = numbers[made];

                numbers[made] = numbers[chosen];
                numbers[chosen] = held;

                if (!shuf_output_number(output, numbers[made], delimiter))
                        return false;
        }

        return true;
}

static b32 file_shuf()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address) "shuf",
            .allowed = (string_address) "einorz",
            .valued = (string_address) "inoR",
            .longs = shuf_longs,
            .operand = file_operand,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;
        if (file_option_value(address_of taking, 'R'))
        {
                file_fail("shuf: --random-source is unsupported; kernel-seeded randomness is mandatory\n",
                          0);
                return 1;
        }

        positive flags = taking.flags;
        bool echo = (flags & FILE_FLAG('e')) != 0;
        bool repeat = (flags & FILE_FLAG('r')) != 0;
        string_address range_text = file_option_value(address_of taking, 'i');

        if (echo && range_text)
        {
                file_fail("shuf: cannot combine --echo and --input-range\n", 0);
                return 1;
        }
        if (range_text && file_operand_count)
        {
                file_fail("shuf: extra operand with --input-range\n", 0);
                return 1;
        }
        if (!echo && !range_text && file_operand_count > 1)
        {
                file_fail("shuf: extra operand\n", 0);
                return 1;
        }

        positive wanted = 0;
        string_address count_text = file_option_value(address_of taking, 'n');
        bool limited = count_text != null;

        if (limited && !shred_number(count_text, address_of wanted))
        {
                string_format(file_fail, "shuf: invalid line count: '%s'\n",
                              count_text);
                return 1;
        }

        positive low = 0;
        positive high = 0;
        positive count = 0;
        shuf_record address_to records = null;

        text_arena_used = 0;

        if (range_text)
        {
                if (!shuf_range(range_text, address_of low, address_of high) ||
                    high - low == positive_max)
                {
                        string_format(file_fail, "shuf: invalid input range: '%s'\n",
                                      range_text);
                        text_arena_used = 0;
                        return 1;
                }

                count = high - low + 1;
        }
        else if (echo)
                records = shuf_echo_records(address_of count);
        else
                records = shuf_file_records(file_operand_count
                                                 ? file_operand_at(0)
                                                 : null,
                                             (flags & FILE_FLAG('z')) ? '\0'
                                                                      : '\n',
                                             address_of count);

        if (!range_text && !records)
        {
                text_arena_used = 0;
                return 1;
        }

        if (repeat && !count && (!limited || wanted))
        {
                file_fail("shuf: no lines to repeat\n", 0);
                text_arena_used = 0;
                return 1;
        }

        bool need_random = count > 1 && (!limited || wanted);
        file_random_state random;

        if (need_random && !file_random_seed(address_of random))
        {
                file_fail("shuf: kernel randomness unavailable\n", 0);
                text_arena_used = 0;
                return 1;
        }

        string_address output_name = file_option_value(address_of taking, 'o');
        shuf_output output = {.handle = 1, .name = output_name};

        if (output_name)
        {
                output.handle = system_open_at_mode(AT_FDCWD, output_name,
                                                    FILE_WRITE, 0666);
                output.opened = output.handle >= 0;

                if (!output.opened)
                {
                        string_format(file_fail, "shuf: cannot open '%s': %s\n",
                                      output_name, file_reason(output.handle));
                        text_arena_used = 0;
                        return 1;
                }
        }

        p8 delimiter = (flags & FILE_FLAG('z')) ? '\0' : '\n';
        bool good = range_text
                        ? shuf_emit_range(address_of output, low, count, wanted,
                                          limited, repeat, delimiter,
                                          address_of random)
                        : shuf_emit_records(address_of output, records, count,
                                            wanted, limited, repeat, delimiter,
                                            address_of random);

        if (good)
                good = shuf_output_flush(address_of output);

        if (output.opened && system_close(output.handle) < 0)
        {
                string_format(file_fail, "shuf: close failed on '%s'\n",
                              output_name);
                good = false;
        }

        log_flush();
        text_arena_used = 0;
        return good ? 0 : 1;
}

// dircolors -------------------------------------------------------
/*
        The database is intentionally a compact useful default, not a frozen
        copy of GNU's distribution list.  FILE parsing below is the compatible
        surface: all of the core kind/mode keywords and arbitrary suffix rows
        are translated into the same LS_COLORS colon table that ls parses.
        Keeping the policy data small saves every installed shell a few KiB;
        sites wanting the exhaustive extension list can pass their ordinary
        dircolors file unchanged.
*/
static const string_address dircolors_database =
    "# Moonwater compact dircolors database.\n"
    "# The parser accepts GNU core keywords and arbitrary suffix rules.\n"
    "TERM ansi\n"
    "TERM *color*\n"
    "TERM con[0-9]*x[0-9]*\n"
    "TERM console\n"
    "TERM cygwin\n"
    "TERM gnome*\n"
    "TERM hurd\n"
    "TERM konsole*\n"
    "TERM linux\n"
    "TERM putty\n"
    "TERM rxvt*\n"
    "TERM screen*\n"
    "TERM st*\n"
    "TERM tmux*\n"
    "TERM vt100\n"
    "TERM xterm*\n"
    "COLORTERM ?*\n"
    "RESET 0\n"
    "DIR 01;34\n"
    "LINK 01;36\n"
    "MULTIHARDLINK 00\n"
    "FIFO 40;33\n"
    "SOCK 01;35\n"
    "DOOR 01;35\n"
    "BLK 40;33;01\n"
    "CHR 40;33;01\n"
    "ORPHAN 40;31;01\n"
    "MISSING 00\n"
    "SETUID 37;41\n"
    "SETGID 30;43\n"
    "CAPABILITY 00\n"
    "STICKY_OTHER_WRITABLE 30;42\n"
    "OTHER_WRITABLE 34;42\n"
    "STICKY 37;44\n"
    "EXEC 01;32\n"
    ".tar 01;31\n"
    ".tgz 01;31\n"
    ".gz 01;31\n"
    ".bz2 01;31\n"
    ".xz 01;31\n"
    ".zst 01;31\n"
    ".zip 01;31\n"
    ".7z 01;31\n"
    ".rar 01;31\n"
    ".deb 01;31\n"
    ".rpm 01;31\n"
    ".jpg 01;35\n"
    ".jpeg 01;35\n"
    ".gif 01;35\n"
    ".png 01;35\n"
    ".svg 01;35\n"
    ".webp 01;35\n"
    ".mp4 01;35\n"
    ".mkv 01;35\n"
    ".mp3 00;36\n"
    ".flac 00;36\n"
    ".ogg 00;36\n"
    ".wav 00;36\n"
    "*~ 00;90\n"
    ".bak 00;90\n"
    ".old 00;90\n"
    ".orig 00;90\n"
    ".rej 00;90\n"
    ".swp 00;90\n"
    ".tmp 00;90\n";

typedef struct
{
        string_address name;
        string_address key;
} dircolors_keyword;

static const dircolors_keyword dircolors_keywords[] = {
    {(string_address) "RESET", (string_address) "rs"},
    {(string_address) "NORMAL", (string_address) "no"},
    {(string_address) "FILE", (string_address) "fi"},
    {(string_address) "DIR", (string_address) "di"},
    {(string_address) "LINK", (string_address) "ln"},
    {(string_address) "MULTIHARDLINK", (string_address) "mh"},
    {(string_address) "FIFO", (string_address) "pi"},
    {(string_address) "SOCK", (string_address) "so"},
    {(string_address) "DOOR", (string_address) "do"},
    {(string_address) "BLK", (string_address) "bd"},
    {(string_address) "CHR", (string_address) "cd"},
    {(string_address) "ORPHAN", (string_address) "or"},
    {(string_address) "MISSING", (string_address) "mi"},
    {(string_address) "SETUID", (string_address) "su"},
    {(string_address) "SETGID", (string_address) "sg"},
    {(string_address) "CAPABILITY", (string_address) "ca"},
    {(string_address) "STICKY_OTHER_WRITABLE", (string_address) "tw"},
    {(string_address) "OTHER_WRITABLE", (string_address) "ow"},
    {(string_address) "STICKY", (string_address) "st"},
    {(string_address) "EXEC", (string_address) "ex"},
    {(string_address) "LEFTCODE", (string_address) "lc"},
    {(string_address) "RIGHTCODE", (string_address) "rc"},
    {(string_address) "ENDCODE", (string_address) "ec"},
    {null, null},
};

static const file_long dircolors_longs[] = {
    {(string_address) "bourne-shell", 'b'},
    {(string_address) "sh", 'b'},
    {(string_address) "c-shell", 'c'},
    {(string_address) "csh", 'c'},
    {(string_address) "print-database", 'p'},
    {(string_address) "print-ls-colors", 'L'},
    {null, 0},
};

static p8 dircolors_shell_option;
static const file_supersede dircolors_supersedes[] = {
    {(string_address) "bc", address_of dircolors_shell_option},
    {null, null},
};

typedef struct
{
        p8 address_to text;
        positive used;
        positive room;
} dircolors_builder;

static bool dircolors_word_is(string_address text, positive length,
                              string_address word)
{
        if (length != string_length(word))
                return false;

        for (positive i = 0; i < length; i++)
                if (byte_to_upper(string_get(text + i)) != string_get(word + i))
                        return false;

        return true;
}

static string_address dircolors_key(string_address word, positive length)
{
        for (positive i = 0; dircolors_keywords[i].name; i++)
                if (dircolors_word_is(word, length,
                                      dircolors_keywords[i].name))
                        return dircolors_keywords[i].key;

        return null;
}

static bool dircolors_add(dircolors_builder address_to builder,
                          string_address text, positive length)
{
        /* One byte always remains for the table terminator. */
        if (builder->used >= builder->room ||
            length >= builder->room - builder->used)
                return false;

        memory_copy_apart(builder->text + builder->used, text, length);
        builder->used += length;
        return true;
}

static bool dircolors_add_entry(dircolors_builder address_to builder,
                                string_address key, positive key_length,
                                bool extension, string_address value,
                                positive value_length)
{
        if ((memory_first_of(key, ':', key_length) ||
             memory_first_of(value, ':', value_length)))
        {
                file_fail("dircolors: ':' in keys or values is unsupported by the shared LS_COLORS grammar\n",
                          0);
                return false;
        }

        positive prefix = extension && string_is(key, '.') ? 1 : 0;

        if (key_length > positive_max - value_length - 2 - prefix ||
            builder->used >= builder->room ||
            key_length + value_length + 2 + prefix >=
                builder->room - builder->used)
        {
                file_fail("dircolors: translated table is too large\n", 0);
                return false;
        }

        return (!extension || !string_is(key, '.') ||
                dircolors_add(builder, (string_address) "*", 1)) &&
               dircolors_add(builder, key, key_length) &&
               dircolors_add(builder, (string_address) "=", 1) &&
               dircolors_add(builder, value, value_length) &&
               dircolors_add(builder, (string_address) ":", 1);
}

/* Translate once, then immediately pass the result through file_color_next
   and ls_color_parse.  This is only the line-oriented front end to the one
   colour-table engine, not a parallel lookup structure. */
static string_address dircolors_parse(string_address input, positive length,
                                      string_address name)
{
        if (memory_first_of(input, 0, length))
        {
                string_format(file_fail, "dircolors: %s: embedded NUL byte\n",
                              name);
                return null;
        }

        positive room = length <= (positive_max - 64) / 2
                            ? length + length / 2 + 64 : 0;
        dircolors_builder builder = {
            .text = room ? (p8 address_to)text_arena_take(room) : null,
            .room = room,
        };

        if (!builder.text)
        {
                file_fail("dircolors: configuration is too large\n", 0);
                return null;
        }

        string_address term = file_environment((string_address) "TERM");
        string_address colorterm = file_environment((string_address) "COLORTERM");
        bool gated = false;
        bool gate_matches = false;
        positive line_number = 0;

        for (positive at = 0; at < length;)
        {
                positive stop = at;

                while (stop < length && !string_is(input + stop, '\n'))
                        stop++;

                line_number++;
                positive first = at;

                while (first < stop && byte_is_space(string_get(input + first)))
                        first++;

                positive finish = stop;

                while (finish > first &&
                       byte_is_space(string_get(input + finish - 1)))
                        finish--;

                at = stop < length ? stop + 1 : stop;

                if (first == finish || string_is(input + first, '#'))
                        continue;

                positive key_end = first;

                while (key_end < finish &&
                       !byte_is_space(string_get(input + key_end)))
                        key_end++;

                positive value = key_end;

                while (value < finish &&
                       byte_is_space(string_get(input + value)))
                        value++;

                for (positive i = value; i < finish; i++)
                        if (string_is(input + i, '#') &&
                            (i == value ||
                             byte_is_space(string_get(input + i - 1))))
                        {
                                finish = i;

                                while (finish > value &&
                                       byte_is_space(
                                           string_get(input + finish - 1)))
                                        finish--;
                                break;
                        }

                if (value == finish)
                {
                        string_format(file_fail,
                                      "dircolors: %s:%u: missing second token\n",
                                      name, line_number);
                        return null;
                }

                positive key_length = key_end - first;
                positive value_length = finish - value;
                bool term_gate = dircolors_word_is(input + first, key_length,
                                                   (string_address) "TERM");
                bool color_gate = dircolors_word_is(
                    input + first, key_length, (string_address) "COLORTERM");

                if (term_gate || color_gate)
                {
                        if (value_length >= FILE_PATH_MAX)
                        {
                                string_format(file_fail,
                                              "dircolors: %s:%u: terminal pattern is too long\n",
                                              name, line_number);
                                return null;
                        }

                        p8 pattern[FILE_PATH_MAX];
                        memory_copy_apart(pattern, input + value, value_length);
                        pattern[value_length] = end;
                        string_address against = term_gate ? term : colorterm;

                        gated = true;
                        gate_matches = gate_matches ||
                                       (against && shell_match(pattern, against));
                        continue;
                }

                string_address short_key =
                    dircolors_key(input + first, key_length);
                bool extension = string_is(input + first, '.') ||
                                 string_is(input + first, '*');

                /* GNU ignores unknown historical directives.  Keeping that
                   behavior lets one shared file serve old and new systems;
                   recognized rows are never accepted partially. */
                if (!short_key && !extension)
                        continue;

                string_address output_key = short_key ? short_key : input + first;
                positive output_key_length = short_key ? 2 : key_length;

                if (!dircolors_add_entry(address_of builder, output_key,
                                         output_key_length, extension,
                                         input + value, value_length))
                        return null;
        }

        if (gated && !gate_matches)
                builder.used = 0;

        builder.text[builder.used] = end;

        if (!file_color_table_valid(builder.text, false))
        {
                file_fail("dircolors: configuration cannot be represented by LS_COLORS\n",
                          0);
                return null;
        }

        ls_colors = builder.text;
        ls_color_parse();
        return builder.text;
}

static fn dircolors_shell_quote(string_address table, bool csh)
{
        log(csh ? (string_address) "setenv LS_COLORS '"
                : (string_address) "LS_COLORS='",
            0);

        for (positive i = 0; string_get(table + i); i++)
        {
                p8 character = string_get(table + i);

                if (character == '\'')
                        log("'\\''", 4);
                else
                        log(address_of character, 1);
        }

        log(csh ? (string_address) "'\n"
                : (string_address) "';\nexport LS_COLORS\n",
            0);
}

static fn dircolors_print_table(string_address table)
{
        file_color_entry entry;

        while (file_color_next(address_of table, address_of entry))
        {
                if (!entry.assigned || !entry.key.length)
                        continue;

                file_color_sgr(log, entry.value);
                log(entry.key.text, entry.key.length);
                log("\t", 1);
                log(entry.value.text, entry.value.length);
                log("\033[0m\n", 5);
        }
}

static b32 file_dircolors()
{
        file_operands_begin();
        dircolors_shell_option = 0;
        file_taking taking = {
            .program = (string_address) "dircolors",
            .allowed = (string_address) "bcp",
            .valued = (string_address) "",
            .longs = dircolors_longs,
            .operand = file_operand,
            .supersedes = dircolors_supersedes,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;
        if (file_operand_count > 1)
        {
                file_fail("dircolors: extra operand\n", 0);
                return 1;
        }

        positive flags = taking.flags;
        bool print_database = (flags & FILE_FLAG('p')) != 0;
        bool print_table = (flags & FILE_FLAG('L')) != 0;

        if (print_database && (file_operand_count || dircolors_shell_option ||
                               print_table))
        {
                file_fail("dircolors: --print-database cannot be combined with a file or another output mode\n",
                          0);
                return 1;
        }
        if (print_table && dircolors_shell_option)
        {
                file_fail("dircolors: --print-ls-colors cannot select a shell syntax\n",
                          0);
                return 1;
        }
        if (print_database)
        {
                log(dircolors_database, 0);
                log_flush();
                return 0;
        }

        text_arena_used = 0;
        string_address input = dircolors_database;
        positive length = string_length(dircolors_database);
        string_address name = (string_address) "built-in database";
        bipolar handle = -1;

        if (file_operand_count)
        {
                name = file_operand_at(0);
                handle = string_is(name, '-') && !string_get(name + 1)
                             ? 0 : system_open_at(AT_FDCWD, name, FILE_READ);

                if (handle < 0)
                {
                        string_format(file_fail, "dircolors: '%s': %s\n", name,
                                      file_reason(handle));
                        return 1;
                }

                bool read_failed;
                input = text_arena_read_all((positive)handle,
                                            FILE_TRANSFER_SIZE,
                                            address_of length,
                                            address_of read_failed);

                if (handle != 0)
                        system_close(handle);

                if (!input)
                {
                        string_format(file_fail,
                                      read_failed
                                          ? (string_address) "dircolors: cannot read '%s'\n"
                                          : (string_address) "dircolors: '%s' is too large\n",
                                      name);
                        text_arena_used = 0;
                        return 1;
                }
        }

        string_address table = dircolors_parse(input, length, name);

        if (!table)
        {
                text_arena_used = 0;
                return 1;
        }

        if (print_table)
                dircolors_print_table(table);
        else
        {
                bool csh = dircolors_shell_option == 'c';

                if (!dircolors_shell_option)
                {
                        string_address shell =
                            file_environment((string_address) "SHELL");
                        positive shell_length = shell ? string_length(shell) : 0;

                        csh = shell_length >= 3 &&
                              !string_compare_max(shell + shell_length - 3,
                                                  (string_address) "csh", 3);
                }

                dircolors_shell_quote(table, csh);
        }

        log_flush();
        text_arena_used = 0;
        return 0;
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
                return file_missing((string_address) "rmdir");

        b32 status = 0;

        while (first < count)
        {
                string_address path = program_argument((b32)first++);
                bipolar gone = system_remove_at(AT_FDCWD, path,
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

                        bipolar kept = system_remove_at(AT_FDCWD, above,
                                                        AT_REMOVEDIR);

                        // A parent that stays is a failure like the first
                        // name's: said, and counted in the status.
                        if (kept < 0)
                        {
                                string_format(file_fail,
                                              "rmdir: failed to remove '%s': %s\n",
                                              above, file_reason(kept));
                                status = 1;
                                break;
                        }

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

/* -f is independent; only -i and -n supersede one another. */
static const file_supersede cp_supersedes[] = {
    {(string_address) "in", address_of cp_collision_option},
    {(string_address) "HLPda", address_of cp_dereference_option},
    {null, null},
};

// 0 copies a symbolic link as itself, 1 copies what it points at, 2 does
// that only for the links named on the command line.
static positive cp_dereference;

// The umask as it stood when cp began: what a new file gets is the source's
// mode with the umask taken out, and the kernel applies it to the creation
// itself, but a directory's final mode is set afterwards by hand.
static positive cp_umask;

/*
        Everything -p carries over, and everything a move across devices
        carries over whether asked or not: owner, times and mode, each tried
        and none insisted on, because an owner that cannot be given is not a
        reason to leave the copy unmade.
*/
static fn file_keep(string_address destination, file_facts address_to facts)
{
        p64 times[4];

        file_times_of(facts, times);

        system_change_owner_at(AT_FDCWD, destination, facts->owner,
                               facts->group, AT_SYMLINK_NOFOLLOW);

        system_update_times_at(AT_FDCWD, destination, times,
                               AT_SYMLINK_NOFOLLOW);

        // A symbolic link has no mode of its own, and fchmodat has no way to
        // stop at one: the chmod would land on whatever it points at.
        if ((facts->mode & MODE_FORMAT) == MODE_LINK)
                return;

        system_change_mode_at(AT_FDCWD, destination, facts->mode & 07777);
}

static fn cp_keep(string_address destination, file_facts address_to facts)
{
        if (cp_preserve)
                file_keep(destination, facts);
}

// A pipe, a socket or a device node is made again with the source's kind
// and numbers rather than read, because reading a pipe waits for a writer
// that is never coming and reading a device copies whatever it produces.
static bool file_make_alike(string_address program, string_address destination,
                            file_facts address_to facts)
{
        bipolar made = system_call_4(
            syscall(mknodat), AT_FDCWD, (positive)destination,
            (facts->mode & MODE_FORMAT) | (facts->mode & 07777),
            file_device(facts->rdev_major, facts->rdev_minor));

        if (made < 0)
                string_format(file_fail, "%s: cannot create special file '%s': %s\n",
                              program, destination, file_reason(made));

        return made == 0;
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
                system_remove_at(AT_FDCWD, destination, 0);

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

                done = system_symbolic_link_at(source, AT_FDCWD, destination);
        }
        else
                done = system_link_at(AT_FDCWD, source, AT_FDCWD, destination, 0);

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
        bipolar looked = file_look_code(AT_FDCWD, source,
                                        follow ? 0 : AT_SYMLINK_NOFOLLOW,
                                        address_of facts);

        if (looked < 0)
        {
                string_format(file_fail, "cp: cannot stat '%s': %s\n", source,
                              file_reason(looked));
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

                // Both sides followed all the way: a destination spelled
                // through a link into the source is still inside it.
                if (file_resolve(source, from, true) &&
                    file_resolve(destination, to, true) && realpath_under(from, to))
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

                system_remove_at(AT_FDCWD, destination, 0);

                if (system_symbolic_link_at(target, AT_FDCWD, destination) < 0)
                {
                        string_format(file_fail, "cp: cannot create link '%s'\n", destination);
                        cp_status = 1;
                        return false;
                }

                cp_keep(destination, address_of facts);
                cp_said(source, destination);
                return true;
        }

        // -r copies a tree, and a pipe or a device in a tree is part of its
        // shape rather than a stream to drain; whatever stood at the
        // destination is taken away first, as the reference cp does for a
        // source that is not a regular file.
        if (cp_recursive && kind != MODE_DIRECTORY && kind != MODE_FILE)
        {
                system_remove_at(AT_FDCWD, destination, 0);

                if (!file_make_alike((string_address) "cp", destination, address_of facts))
                {
                        cp_status = 1;
                        return false;
                }

                cp_keep(destination, address_of facts);
                cp_said(source, destination);
                return true;
        }

        if (kind != MODE_DIRECTORY)
        {
                // The open creates a file with the source's mode under the
                // umask, and a destination that was already there keeps the
                // mode it had: both are what the reference cp leaves, and -p
                // is what asks for the source's mode whole.
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

                        system_remove_at(AT_FDCWD, destination, 0);

                        if (!file_copy_contents(AT_FDCWD, source, AT_FDCWD, destination,
                                                facts.mode & 07777))
                        {
                                string_format(file_fail, "cp: cannot copy '%s'\n", source);
                                cp_status = 1;
                                return false;
                        }
                }

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

        // The directory is made with the owner able to write into it whatever
        // the source allowed, so a read-only tree can still be filled, and is
        // given its final mode once everything is inside.
        bipolar made = system_make_directory_at(
            AT_FDCWD, destination, (facts.mode & 07777) | 0700);

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
                string_format(file_fail, "cp: cannot read directory '%s': %s\n", source,
                              file_reason(walk.handle));
                cp_status = 1;
                return false;
        }

        cp_said(source, destination);

        bool complete = true;
        positive skipped = 0;
        p8 from[FILE_PATH_MAX];
        p8 to[FILE_PATH_MAX];

        while (file_walk_pair(address_of walk, (string_address) "cp", source,
                              destination, from, to, address_of skipped))
        {
                if (!cp_one(from, to, depth - 1, false))
                        complete = false;
        }

        file_walk_close(address_of walk);

        if (skipped)
        {
                cp_status = 1;
                complete = false;
        }

        // A directory that was already there keeps its mode, as a file does;
        // one made here gets the source's under the umask, unless -p wants
        // the source's whole.
        if (cp_preserve)
                cp_keep(destination, address_of facts);
        else if (made == 0)
                system_change_mode_at(AT_FDCWD, destination,
                                      facts.mode & 07777 & ~cp_umask);

        return complete;
}

// cp_one carries the walk depth and whether the name was written on the
// command line; the pair walker cp shares with mv carries neither, and every
// pair it hands over is a named one at full depth.
static fn cp_pair(string_address source, string_address destination)
{
        cp_one(source, destination, FILE_MAX_DEPTH, true);
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
            .supersedes = cp_supersedes,
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
        cp_umask = file_umask();

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

        if (!file_source_destination((string_address) "cp", first, count, into,
                                     (flags & FILE_FLAG('T')) != 0, cp_pair))
                return 1;

        return cp_status;
}

// install ---------------------------------------------------------
/*
        install is the package-build copy: unlike cp, its final mode is an
        explicit property of the destination and defaults to executable.
        The bytes still go through file_copy_contents, so the common case is
        the same in-kernel copy rather than a second userspace copy loop.

        This is the surface used by ordinary make install rules: -D creates
        the leading path, -d creates directory trees, -m chooses the mode,
        -o/-g choose ownership, -p keeps timestamps, and -t/-T select the two
        destination forms.  Unsupported transforming operations such as
        --strip are rejected instead of silently claiming to have happened.
*/
static positive install_mode;
static bipolar install_owner;
static bipolar install_group;
static bool install_parents;
static bool install_preserve;
static bool install_loud;
static b32 install_status;

static const file_long install_longs[] = {
    {(string_address) "create-leading-directories", 'D'},
    {(string_address) "directory", 'd'},
    {(string_address) "group", 'g'},
    {(string_address) "mode", 'm'},
    {(string_address) "owner", 'o'},
    {(string_address) "preserve-timestamps", 'p'},
    {(string_address) "no-target-directory", 'T'},
    {(string_address) "target-directory", 't'},
    {(string_address) "verbose", 'v'},
    {null, 0},
};

static bool install_identity(string_address text, bool group,
                             bipolar address_to identity)
{
        positive number;
        bipolar found;

        if (string_digits_exact(text, address_of number) && number <= p32_max)
                found = (bipolar)number;
        else
                found = group ? file_group_id(text) : file_user_id(text);

        if (found < 0)
        {
                string_format(file_fail, "install: invalid %s '%s'\n",
                              group ? "group" : "user", text);
                return false;
        }

        address_to identity = found;
        return true;
}

static bool install_leading(string_address destination)
{
        p8 parent[FILE_PATH_MAX];

        path_head_copy(parent, FILE_PATH_MAX, destination);
        if (file_make_parents(parent, 0755))
                return true;

        string_format(file_fail,
                      "install: cannot create leading directories for '%s'\n",
                      destination);
        return false;
}

static bool install_attributes(string_address destination,
                               file_facts address_to source)
{
        if ((install_owner >= 0 || install_group >= 0) &&
            system_change_owner_at(AT_FDCWD, destination, install_owner,
                                   install_group, 0) < 0)
        {
                string_format(file_fail,
                              "install: cannot change ownership of '%s'\n",
                              destination);
                return false;
        }

        if (system_change_mode_at(AT_FDCWD, destination, install_mode) < 0)
        {
                string_format(file_fail, "install: cannot change mode of '%s'\n",
                              destination);
                return false;
        }

        if (install_preserve && source)
        {
                p64 times[4];

                file_times_of(source, times);
                if (system_update_times_at(AT_FDCWD, destination, times, 0) < 0)
                {
                        string_format(file_fail,
                                      "install: cannot preserve times of '%s'\n",
                                      destination);
                        return false;
                }
        }

        return true;
}

static fn install_pair(string_address source, string_address destination)
{
        file_facts from;
        file_facts to;
        bipolar looked = file_look_code(AT_FDCWD, source, 0, address_of from);

        if (looked < 0 || (from.mode & MODE_FORMAT) != MODE_FILE)
        {
                string_format(file_fail, "install: cannot stat '%s': %s\n",
                              source,
                              looked < 0 ? file_reason(looked)
                                         : (string_address)"Not a regular file");
                install_status = 1;
                return;
        }

        if (file_look_at(destination, address_of to) &&
            file_same_identity(address_of from, address_of to))
        {
                string_format(file_fail,
                              "install: '%s' and '%s' are the same file\n",
                              source, destination);
                install_status = 1;
                return;
        }

        if (install_parents && !install_leading(destination))
        {
                install_status = 1;
                return;
        }

        bipolar removed = system_remove_at(AT_FDCWD, destination, 0);

        if (removed < 0 && removed != -ERROR_NO_ENTRY)
        {
                string_format(file_fail, "install: cannot remove '%s': %s\n",
                              destination, file_reason(removed));
                install_status = 1;
                return;
        }

        /* O_EXCL makes the unlink/create boundary fail closed if another
           process races a symlink into the destination. It also gives
           install its usual inode-breaking behavior for hard links. */
        if (!file_copy_contents_open(AT_FDCWD, source, AT_FDCWD, destination,
                                     install_mode,
                                     FILE_WRITE | FILE_EXCLUSIVE))
        {
                string_format(file_fail, "install: cannot copy '%s' to '%s'\n",
                              source, destination);
                install_status = 1;
                return;
        }

        if (!install_attributes(destination, address_of from))
        {
                install_status = 1;
                return;
        }

        if (install_loud)
                string_format(log, "'%s' -> '%s'\n", source, destination);
}

static b32 file_install()
{
        positive count = (positive)program_argument_count();
        file_taking taking = {
            .program = (string_address) "install",
            .allowed = (string_address) "DcdgmopTtv",
            .valued = (string_address) "gmot",
            .longs = install_longs,
        };

        install_mode = 0755;
        install_owner = -1;
        install_group = -1;
        install_status = 0;

        if (!file_take(address_of taking))
                return 1;

        string_address mode = file_option_value(address_of taking, 'm');
        string_address owner = file_option_value(address_of taking, 'o');
        string_address group = file_option_value(address_of taking, 'g');
        string_address into = file_option_value(address_of taking, 't');
        positive flags = taking.flags;
        bool directories = (flags & FILE_FLAG('d')) != 0;

        if ((mode && !file_mode_of(mode, 0, false, address_of install_mode)) ||
            (owner && !install_identity(owner, false, address_of install_owner)) ||
            (group && !install_identity(group, true, address_of install_group)))
                return 1;

        install_parents = (flags & FILE_FLAG('D')) != 0;
        install_preserve = (flags & FILE_FLAG('p')) != 0;
        install_loud = (flags & FILE_FLAG('v')) != 0;

        if (directories)
        {
                if (into || (flags & (FILE_FLAG('D') | FILE_FLAG('T'))))
                        return file_missing((string_address) "install");
                if (taking.first >= count)
                        return file_missing((string_address) "install");

                for (positive at = taking.first; at < count; at++)
                {
                        string_address path = program_argument((b32)at);

                        if (!file_make_parents(path, 0755) ||
                            !install_attributes(path, null))
                        {
                                string_format(file_fail,
                                              "install: cannot create directory '%s'\n",
                                              path);
                                install_status = 1;
                        }
                        else if (install_loud)
                                string_format(log, "install: creating directory '%s'\n",
                                              path);
                }

                log_flush();
                return install_status;
        }

        if (install_parents && (into || count - taking.first != 2))
        {
                file_fail("install: -D requires one source and one destination\n", 0);
                return 1;
        }

        if (!file_source_destination((string_address) "install", taking.first,
                                     count, into,
                                     (flags & FILE_FLAG('T')) != 0,
                                     install_pair))
                return 1;

        return install_status;
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

static const file_supersede mv_supersedes[] = {
    {(string_address) "fin", address_of mv_collision_option},
    {null, null},
};

static bool mv_across(string_address source, string_address destination, positive depth);

// Whether the walk across devices has already said what went wrong, so the
// caller does not put the rename's own refusal over the top of it.
static bool mv_across_said;

static bool mv_across_directory(string_address source, string_address destination,
                                positive depth)
{
        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, source))
        {
                string_format(file_fail, "mv: cannot read directory '%s': %s\n", source,
                              file_reason(walk.handle));
                mv_across_said = true;
                return false;
        }

        bool complete = true;
        positive skipped = 0;
        p8 from[FILE_PATH_MAX];
        p8 to[FILE_PATH_MAX];

        while (file_walk_pair(address_of walk, (string_address) "mv", source,
                              destination, from, to, address_of skipped))
        {
                if (!mv_across(from, to, depth - 1))
                        complete = false;
        }

        file_walk_close(address_of walk);

        if (skipped)
                complete = false;

        if (complete)
                system_remove_at(AT_FDCWD, source, AT_REMOVEDIR);

        return complete;
}

// A rename that crosses a mount point is not a rename at all, so the bytes
// have to be carried over and the original taken away afterwards. What is
// carried is everything a rename would have kept: the mode whole and not
// under the umask, the times, the owner where that is allowed, and a pipe
// or a device as the thing it is rather than what could be read out of it.
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
                // Filled first and given its mode last, so a directory the
                // owner may not write is still moved with what is in it;
                // the mode is set even when something inside could not be
                // taken away, because the copy is what is left standing.
                bipolar made = system_make_directory_at(
                    AT_FDCWD, destination, (facts.mode & 07777) | 0700);

                if (made < 0 && made != -ERROR_EXISTS)
                {
                        string_format(file_fail, "mv: cannot create directory '%s': %s\n",
                                      destination, file_reason(made));
                        mv_across_said = true;
                        return false;
                }

                bool complete = mv_across_directory(source, destination, depth);

                file_keep(destination, address_of facts);
                return complete;
        }

        if (kind == MODE_LINK)
        {
                p8 target[FILE_PATH_MAX];

                if (file_link_text(source, target, FILE_PATH_MAX) < 0)
                        return false;

                system_remove_at(AT_FDCWD, destination, 0);

                if (system_symbolic_link_at(target, AT_FDCWD, destination) < 0)
                        return false;
        }
        else if (kind != MODE_FILE)
        {
                system_remove_at(AT_FDCWD, destination, 0);

                if (!file_make_alike((string_address) "mv", destination, address_of facts))
                {
                        mv_across_said = true;
                        return false;
                }
        }
        else if (!file_copy_contents(AT_FDCWD, source, AT_FDCWD, destination,
                                     facts.mode & 07777))
                return false;

        file_keep(destination, address_of facts);

        bipolar gone = system_remove_at(AT_FDCWD, source, 0);

        if (gone < 0)
        {
                string_format(file_fail, "mv: cannot remove '%s': %s\n", source,
                              file_reason(gone));
                mv_across_said = true;
        }

        return gone == 0;
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

        bipolar done = system_rename_at(
            AT_FDCWD, source, AT_FDCWD, destination, 0);

        if (done == 0)
        {
                if (mv_loud)
                        string_format(log, "renamed '%s' -> '%s'\n", source, destination);

                return;
        }

        if (done == -ERROR_CROSS_DEVICE)
        {
                mv_across_said = false;

                if (mv_across(source, destination, FILE_MAX_DEPTH))
                {
                        if (mv_loud)
                                string_format(log, "renamed '%s' -> '%s'\n", source,
                                              destination);

                        return;
                }

                if (mv_across_said)
                {
                        mv_status = 1;
                        return;
                }
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
            .supersedes = mv_supersedes,
        };

        if (!file_take(address_of taking))
                return 1;

        positive first = taking.first;

        mv_ask = mv_collision_option == 'i';
        mv_never_clobber = mv_collision_option == 'n';
        mv_loud = (taking.flags & FILE_FLAG('v')) != 0;

        string_address into = file_option_value(address_of taking, 't');

        if (!file_source_destination((string_address) "mv", first, count, into,
                                     (taking.flags & FILE_FLAG('T')) != 0, mv_one))
                return 1;

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

static const file_supersede rm_supersedes[] = {
    {(string_address) "fi", address_of rm_collision_option},
    {null, null},
};

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

                system_seek(directory, 0, FILE_SEEK_SET);

                struct linux_dirent64 address_to entry;
                positive removed = 0;
                positive seen = 0;

                while ((entry = file_walk_next(address_of walk)))
                {
                        if (file_is_dot(entry->d_name))
                                continue;

                        seen++;

                        p8 below[FILE_PATH_MAX];

                        if (!file_path_join(below, shown, entry->d_name))
                        {
                                file_too_long((string_address) "rm",
                                              (string_address) "cannot remove", shown,
                                              entry->d_name);
                                rm_status = 1;
                                complete = false;
                                continue;
                        }

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
        bipolar tried = -ERROR_NO_ENTRY;

        if (!rm_careful)
        {
                tried = system_remove_at(directory, name, 0);

                if (tried == 0)
                        return true;
        }
        else if ((tried = file_look_code(directory, name, AT_SYMLINK_NOFOLLOW,
                                         address_of facts)) < 0)
        {
                // -f forgives only a name that is not there; a name that is
                // there and will not be looked at is still a failure.
                if (!rm_force || tried != -ERROR_NO_ENTRY)
                {
                        string_format(file_fail, "rm: cannot remove '%s': %s\n", shown,
                                      file_reason(tried));
                        rm_status = 1;
                }

                return false;
        }
        else if ((facts.mode & MODE_FORMAT) != MODE_DIRECTORY)
        {
                if (rm_ask && !file_ask((string_address) "rm", rm_wording(address_of facts),
                                        shown))
                        return false;

                tried = system_remove_at(directory, name, 0);

                if (tried == 0)
                {
                        rm_said(shown, false);
                        return true;
                }
        }

        /*
                Not a directory, and the unlink refused: the kernel's reason
                is the one to give, and -f forgives only a name that is not
                there. A file that could not be removed under -rf was passed
                over in silence, its directory then refused as not empty in
                the same silence, and rm answered 0 with the tree still there.
        */
        if (!file_is_directory(directory, name))
        {
                if (!rm_force || tried != -ERROR_NO_ENTRY)
                {
                        string_format(file_fail, "rm: cannot remove '%s': %s\n", shown,
                                      file_reason(tried));
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

                bipolar inside = system_open_at(directory, name,
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

                system_close(inside);
        }

        if (rm_ask && !rm_recursive &&
            !file_ask((string_address) "rm", (string_address) "remove directory", shown))
                return false;

        bipolar gone = system_remove_at(directory, name,
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
            .supersedes = rm_supersedes,
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

                return file_missing((string_address) "rm");
        }

        while (first < count)
        {
                string_address path = program_argument((b32)first++);
                file_facts facts;
                bipolar looked = file_look_code(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW,
                                                address_of facts);

                if (looked < 0)
                {
                        // -f forgives a name that is not there and nothing
                        // else: a name that cannot be looked at is reported
                        // with the kernel's reason, as the reference rm does.
                        if (!rm_force || looked != -ERROR_NO_ENTRY)
                        {
                                string_format(file_fail, "rm: cannot remove '%s': %s\n",
                                              path, file_reason(looked));
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

        address_to out = clock_days_from_civil(year, field[2], field[3]) * 86400 +
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
                // The fraction a written date carries is kept, so that what
                // stat printed for one file can be given to another whole.
                positive fraction;

                if (!file_moment_read_exact(written, given ? seconds : file_now(),
                                            address_of seconds, address_of fraction))
                {
                        string_format(file_fail, "touch: invalid date format '%s'\n", written);
                        return 1;
                }

                nanoseconds = (p32)fraction;
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

                        bipolar made = system_open_at_mode(AT_FDCWD,
                                                     path, FILE_WRITE & ~O_TRUNC,
                                                     0666);

                        if (made < 0)
                        {
                                string_format(file_fail, "touch: cannot touch '%s': %s\n",
                                              path, file_reason(made));
                                status = 1;
                                continue;
                        }

                        system_close(made);
                }

                bipolar done = system_update_times_at(
                    AT_FDCWD, path, times,
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

                while (byte_is_digit(string_get(text)))
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
                return file_missing((string_address) "sleep");

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

// stty -------------------------------------------------------------
/*
        Only the one query scripts can portably use without parsing a
        platform-specific mode dump: stty size writes rows then columns.

        The query belongs on standard input.  In particular, stdout is a pipe
        in set -- $(stty size), while stdin is still the controlling terminal.
*/
typedef struct
{
        p16 rows, columns, x_pixels, y_pixels;
} file_window_size;

#define FILE_TIOCGWINSZ 0x5413

static b32 file_stty()
{
        if (program_argument_count() != 2 ||
            !string_equals(program_argument(1), "size"))
        {
                file_fail("stty: only 'size' is supported\n", 0);
                return 1;
        }

        file_window_size size = {0, 0, 0, 0};
        bipolar answer = system_control(0, FILE_TIOCGWINSZ,
                                       address_of size);

        if (answer < 0)
        {
                string_format(file_fail, "stty: standard input: %s\n",
                              file_reason(answer));
                return 1;
        }

        positive_to_string(log, size.rows);
        log(" ", 1);
        positive_to_string(log, size.columns);
        log("\n", 1);
        return 0;
}

// tty --------------------------------------------------------------
static const file_long tty_longs[] = {
    {(string_address) "quiet", 's'},
    {(string_address) "silent", 's'},
    {null, 0},
};

static b32 file_tty()
{
        file_simple_operand_count = 0;
        file_taking taking = {
            .program = (string_address) "tty",
            .allowed = (string_address) "s",
            .valued = (string_address) "",
            .longs = tty_longs,
            .operand = file_simple_operand,
        };

        if (!file_take(address_of taking))
                return 2;

        if (file_simple_operand_count)
        {
                string_format(file_fail, "tty: extra operand '%s'\n",
                              file_simple_operand_list[0]);
                return 2;
        }

        if (taking.flags & FILE_FLAG('s'))
                return stream_is_terminal(0) ? 0 : 1;

        p8 path[FILE_PATH_MAX];
        bipolar length = file_input_terminal_name(path, sizeof(path));

        if (length == -ENOTTY)
        {
                file_line((string_address) "not a tty");
                log_flush();
                return log_failed() ? 3 : 1;
        }

        if (length < 0)
        {
                string_format(file_fail, "tty: ttyname error: %s\n",
                              file_reason(length));
                return 4;
        }

        file_line(path);
        log_flush();
        return log_failed() ? 3 : 0;
}

// seq ------------------------------------------------------------
/*
        seq LAST, seq FIRST LAST, seq FIRST INCREMENT LAST.

        Decimal operands stay decimal.  Keeping an integer coefficient and a
        power-of-ten scale is both smaller than bringing a floating-point
        parser into every utility and, more importantly, means .1 added three
        times ends at .3 exactly.  The whole-number path below remains
        separate: it owns the complete signed 64-bit range without making an
        integer pay for decimal scaling.
*/
static fn seq_write(writer write, bipolar value, positive width)
{
        if (value < 0)
        {
                write("-", 1);
                positive_to_padded(write, (positive)0 - (positive)value,
                                   width ? width - 1 : 0, '0', 0);
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

// seq's native-width contract still has to distinguish an exact number from
// a numeric prefix, and refuse overflow instead of wrapping it into a
// plausible value. Compose the floor digit/span/compare primitives here;
// string_bipolar deliberately wraps for arithmetic callers.
static bool seq_number(string_address text, bipolar address_to value)
{
        positive at = 0;
        bool minus = false;

        if (string_is(text, '-') || string_is(text, '+'))
        {
                minus = string_is(text, '-');
                at++;
        }

        string_address digits = text + at;
        positive length = string_length(digits);

        if (!length)
                return false;

        positive zeros = memory_span_byte(digits, '0', length);
        positive significant = length - zeros;
        string_address limit = minus ? (string_address) "9223372036854775808"
                                     : (string_address) "9223372036854775807";

        if (significant > 19 ||
            (significant == 19 &&
             memory_compare(digits + zeros, limit, 19) > 0))
                return false;

        positive magnitude;

        if (!string_digits_exact(digits, address_of magnitude))
                return false;

        address_to value = bipolar_from_magnitude(magnitude, minus);

        return true;
}

typedef struct
{
        bipolar coefficient;
        positive scale;
        positive shown;
        bool negative_zero;
} seq_decimal;

static positive seq_power_ten(positive power)
{
        positive answer = 1;

        while (power--)
                answer *= 10;

        return answer;
}

/*
        Read the decimal grammar accepted by seq: a sign, digits with one
        optional point, and an optional decimal exponent.  Trailing fractional
        zeroes are removed from the arithmetic coefficient but retained in
        shown, because `seq 1.00 .5 2` promises two places in its output.

        Eighteen fractional places keep every scale and rescale inside one
        native register.  Inputs outside that exact range are rejected rather
        than silently rounded through binary floating point.
*/
static bool seq_decimal_number(string_address text, seq_decimal address_to out)
{
        positive at = 0;
        bool minus = false;

        if (string_is(text, '-') || string_is(text, '+'))
        {
                minus = string_is(text, '-');
                at++;
        }

        positive mantissa = at;
        positive point = positive_max;
        positive digits = 0;
        positive fractional = 0;

        while (text[at] && text[at] != 'e' && text[at] != 'E')
        {
                if (text[at] == '.')
                {
                        if (point != positive_max)
                                return false;

                        point = at;
                }
                else if (text[at] >= '0' && text[at] <= '9')
                {
                        digits++;

                        if (point != positive_max)
                                fractional++;
                }
                else
                        return false;

                at++;
        }

        if (!digits)
                return false;

        bipolar exponent = 0;

        if (text[at])
        {
                at++;
                bool exponent_minus = false;

                if (text[at] == '-' || text[at] == '+')
                {
                        exponent_minus = text[at] == '-';
                        at++;
                }

                if (text[at] < '0' || text[at] > '9')
                        return false;

                positive magnitude = 0;

                while (text[at] >= '0' && text[at] <= '9')
                {
                        positive digit = (positive)(text[at++] - '0');

                        // More than this cannot fit the exact decimal floor.
                        if (magnitude > 100000)
                                return false;

                        magnitude = magnitude * 10 + digit;
                }

                if (text[at])
                        return false;

                exponent = exponent_minus ? -(bipolar)magnitude
                                          : (bipolar)magnitude;
        }

        bipolar effective = (bipolar)fractional - exponent;
        positive shown = effective > 0 ? (positive)effective : 0;

        if (shown > 18)
                return false;

        // Find how many rightmost coefficient zeroes can cancel the scale.
        // The exponent was included in at; find the mantissa end directly.
        positive finish = mantissa;

        while (text[finish] && text[finish] != 'e' && text[finish] != 'E')
                finish++;

        positive removable = effective > 0 ? (positive)effective : 0;
        positive trim = 0;
        positive scan = finish;

        while (scan > mantissa && trim < removable)
        {
                scan--;

                if (text[scan] == '.')
                        continue;

                if (text[scan] != '0')
                        break;

                trim++;
        }

        positive limit = minus ? (positive)bipolar_max + 1
                               : (positive)bipolar_max;
        positive coefficient = 0;
        positive kept = digits - trim;
        positive seen = 0;

        for (positive i = mantissa; i < finish && seen < kept; i++)
        {
                if (text[i] == '.')
                        continue;

                positive digit = (positive)(text[i] - '0');

                if (coefficient > (limit - digit) / 10)
                        return false;

                coefficient = coefficient * 10 + digit;
                seen++;
        }

        effective -= (bipolar)trim;

        if (effective < 0)
        {
                positive grow = (positive)-effective;

                if (grow > 18)
                {
                        if (coefficient)
                                return false;

                        out->coefficient = 0;
                        out->scale = 0;
                        out->shown = shown;
                        out->negative_zero = minus;
                        return true;
                }

                positive multiplier = seq_power_ten(grow);

                if (coefficient > limit / multiplier)
                        return false;

                coefficient *= multiplier;
                effective = 0;
        }

        out->coefficient = bipolar_from_magnitude(coefficient, minus);
        out->scale = (positive)effective;
        out->shown = shown;
        out->negative_zero = minus && coefficient == 0;
        return true;
}

static bool seq_decimal_rescale(seq_decimal address_to number, positive scale)
{
        if (number->scale == scale)
                return true;

        positive multiplier = seq_power_ten(scale - number->scale);
        positive magnitude = (positive)number->coefficient;
        positive limit = number->coefficient < 0
                             ? (positive)bipolar_max + 1
                             : (positive)bipolar_max;

        if (number->coefficient < 0)
                magnitude = (positive)0 - magnitude;

        if (magnitude > limit / multiplier)
                return false;

        magnitude *= multiplier;
        number->coefficient = bipolar_from_magnitude(
            magnitude, number->coefficient < 0);
        number->scale = scale;
        return true;
}

static positive seq_decimal_width(bipolar value, positive scale,
                                  positive precision, bool negative_zero)
{
        positive magnitude = (positive)value;

        if (value < 0)
                magnitude = (positive)0 - magnitude;

        if (scale > precision)
                magnitude /= seq_power_ten(scale - precision);

        magnitude /= seq_power_ten(scale < precision ? scale : precision);

        return positive_digits(magnitude) + (precision ? precision + 1 : 0) +
               (value < 0 || negative_zero);
}

static fn seq_decimal_write(writer write, bipolar value, positive scale,
                            positive precision, positive width,
                            bool negative_zero)
{
        bool minus = value < 0 || negative_zero;
        positive magnitude = (positive)value;

        if (value < 0)
                magnitude = (positive)0 - magnitude;

        // Values emitted by the sequence have no significant digits below
        // its chosen precision.  The division is therefore exact.
        if (scale > precision)
                magnitude /= seq_power_ten(scale - precision);

        positive stored_precision = scale < precision ? scale : precision;
        positive divisor = seq_power_ten(stored_precision);
        positive whole = magnitude / divisor;
        positive fraction = magnitude % divisor;
        positive suffix = precision ? precision + 1 : 0;
        positive whole_width = width > suffix ? width - suffix : 0;

        if (minus)
        {
                write("-", 1);

                if (whole_width)
                        whole_width--;
        }

        positive_to_padded(write, whole, whole_width, '0', 0);

        if (precision)
        {
                write(".", 1);

                if (stored_precision)
                        positive_to_padded(write, fraction, stored_precision,
                                           '0', 0);

                if (precision > stored_precision)
                        writer_fill(write, precision - stored_precision, '0');
        }
}

typedef struct
{
        string_address text;
        positive directive;
        positive after;
        positive width;
        positive precision;
        positive flags;
} seq_format;

static bool seq_format_read(string_address text, seq_format address_to format)
{
        bool found = false;

        memory_fill(format, 0, sizeof(*format));
        format->text = text;

        for (positive at = 0; text[at]; at++)
        {
                if (text[at] != '%')
                        continue;

                if (text[at + 1] == '%')
                {
                        at++;
                        continue;
                }

                if (found)
                        return false;

                found = true;
                format->directive = at++;

                string_address flags_at = text + at;

                format->flags = conversion_flags_take(address_of flags_at);
                at = (positive)(flags_at - text);

                while (text[at] >= '0' && text[at] <= '9')
                {
                        if (format->width > 100000)
                                return false;

                        format->width = format->width * 10 +
                                        (positive)(text[at++] - '0');
                }

                format->precision = 6;

                if (text[at] == '.')
                {
                        at++;
                        format->precision = 0;

                        while (text[at] >= '0' && text[at] <= '9')
                        {
                                if (format->precision > 100000)
                                        return false;

                                format->precision = format->precision * 10 +
                                                    (positive)(text[at++] - '0');
                        }
                }

                // coreutils accepts an explicit long-double length here even
                // though seq supplies that type itself.
                if (text[at] == 'L')
                        at++;

                if (text[at] != 'f' && text[at] != 'F')
                        return false;

                format->after = at + 1;
        }

        return found;
}

static fn seq_format_literal(writer write, string_address text, positive length)
{
        positive start = 0;

        for (positive at = 0; at < length; at++)
                if (text[at] == '%' && at + 1 < length && text[at + 1] == '%')
                {
                        write(text + start, at - start + 1);
                        at++;
                        start = at + 1;
                }

        if (start < length)
                write(text + start, length - start);
}

static fn seq_format_write(writer write, seq_format address_to format,
                           bipolar value, positive scale, bool negative_zero)
{
        seq_format_literal(write, format->text, format->directive);

        bool minus = value < 0 || negative_zero;
        positive magnitude = (positive)value;

        if (value < 0)
                magnitude = (positive)0 - magnitude;

        positive precision = format->precision;
        positive stored = scale;

        if (stored > precision)
        {
                positive divisor = seq_power_ten(stored - precision);
                positive rounded = magnitude / divisor;
                positive remainder = magnitude % divisor;
                positive half = divisor / 2;

                if (remainder > half || (remainder == half && (rounded & 1)))
                        rounded++;

                magnitude = rounded;
                stored = precision;
        }

        positive divisor = seq_power_ten(stored);
        positive whole = magnitude / divisor;
        positive fraction = magnitude % divisor;
        positive sign = minus ||
                        (format->flags & (CONVERSION_FLAG_PLUS |
                                          CONVERSION_FLAG_SPACE));
        positive body = positive_digits(whole) + sign +
                        (precision ||
                                 (format->flags & CONVERSION_FLAG_ALTERNATE)
                             ? precision + 1 : 0);
        positive padding = format->width > body ? format->width - body : 0;

        if (!(format->flags & CONVERSION_FLAG_LEFT) &&
            !(format->flags & CONVERSION_FLAG_ZERO))
                writer_fill(write, padding, ' ');

        if (minus)
                write("-", 1);
        else if (format->flags & CONVERSION_FLAG_PLUS)
                write("+", 1);
        else if (format->flags & CONVERSION_FLAG_SPACE)
                write(" ", 1);

        if (!(format->flags & CONVERSION_FLAG_LEFT) &&
            (format->flags & CONVERSION_FLAG_ZERO))
                writer_fill(write, padding, '0');

        positive_to_string(write, whole);

        if (precision || (format->flags & CONVERSION_FLAG_ALTERNATE))
        {
                write(".", 1);

                if (stored)
                        positive_to_padded(write, fraction, stored, '0', 0);

                if (precision > stored)
                        writer_fill(write, precision - stored, '0');
        }

        if (format->flags & CONVERSION_FLAG_LEFT)
                writer_fill(write, padding, ' ');

        string_address suffix = format->text + format->after;
        seq_format_literal(write, suffix, string_length(suffix));
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

        positive count = (positive)program_argument_count();
        positive index = taking.first;
        bool pad = (taking.flags & FILE_FLAG('w')) != 0;
        string_address separator = file_option_value(address_of taking, 's');
        string_address format_text = file_option_value(address_of taking, 'f');

        if (!separator)
                separator = (string_address) "\n";

        positive given = count - index;

        if (given < 1 || given > 3)
        {
                file_fail("seq: needs one, two or three numbers\n", 0);
                return 1;
        }

        if (pad && format_text)
        {
                file_fail("seq: a format cannot be combined with equal width\n", 0);
                return 1;
        }

        seq_format format;

        if (format_text && !seq_format_read(format_text, address_of format))
        {
                file_fail("seq: format needs exactly one %f conversion\n", 0);
                return 1;
        }

        bool decimal = format_text != null;

        for (positive i = 0; i < given && !decimal; i++)
        {
                string_address word = program_argument((b32)(index + i));

                decimal = string_first_of_or_end(word, '.') != word + string_length(word) ||
                          string_first_of_or_end(word, 'e') != word + string_length(word) ||
                          string_first_of_or_end(word, 'E') != word + string_length(word);
        }

        if (decimal)
        {
                seq_decimal number[3];

                for (positive i = 0; i < given; i++)
                        if (!seq_decimal_number(program_argument((b32)(index + i)),
                                                address_of number[i]))
                        {
                                string_format(file_fail, "seq: invalid number: %s\n",
                                              program_argument((b32)(index + i)));
                                return 1;
                        }

                seq_decimal first = given == 1
                                        ? (seq_decimal){1, 0, 0, false}
                                        : number[0];
                seq_decimal step = given == 3
                                       ? number[1]
                                       : (seq_decimal){1, 0, 0, false};
                seq_decimal last = number[given - 1];
                positive precision = first.shown > step.shown
                                         ? first.shown
                                         : step.shown;
                positive scale = first.scale;

                if (step.scale > scale)
                        scale = step.scale;

                if (last.scale > scale)
                        scale = last.scale;

                if (!seq_decimal_rescale(address_of first, scale) ||
                    !seq_decimal_rescale(address_of step, scale) ||
                    !seq_decimal_rescale(address_of last, scale))
                {
                        file_fail("seq: decimal range is too large\n", 0);
                        return 1;
                }

                if (step.coefficient == 0)
                {
                        file_fail("seq: increment must not be zero\n", 0);
                        return 1;
                }

                positive width = 0;

                if (pad)
                {
                        width = seq_decimal_width(first.coefficient, scale,
                                                  precision,
                                                  first.negative_zero);
                        positive last_width = seq_decimal_width(
                            last.coefficient, scale, precision,
                            last.negative_zero);

                        if (last_width > width)
                                width = last_width;
                }

                bipolar value = first.coefficient;
                bool written = false;

                while (step.coefficient > 0 ? value <= last.coefficient
                                            : value >= last.coefficient)
                {
                        if (written)
                                log(separator, 0);

                        bool negative_zero = !written && first.negative_zero;

                        if (format_text)
                                seq_format_write(log, address_of format, value,
                                                 scale, negative_zero);
                        else
                                seq_decimal_write(log, value, scale, precision,
                                                  width, negative_zero);

                        written = true;

                        if (value == last.coefficient ||
                            (step.coefficient > 0 &&
                             value > bipolar_max - step.coefficient) ||
                            (step.coefficient < 0 &&
                             value < bipolar_min - step.coefficient))
                                break;

                        value += step.coefficient;
                }

                if (written)
                        log("\n", 1);

                log_flush();
                return 0;
        }

        bipolar number[3];

        for (positive i = 0; i < given; i++)
                if (!seq_number(program_argument((b32)(index + i)),
                                address_of number[i]))
                {
                        string_format(file_fail, "seq: invalid number: %s\n",
                                      program_argument((b32)(index + i)));
                        return 1;
                }

        bipolar first = given == 1 ? 1 : number[0];
        bipolar step = given == 3 ? number[1] : 1;
        bipolar last = number[given - 1];

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

                if (value == last ||
                    (step > 0 && value > bipolar_max - step) ||
                    (step < 0 && value < bipolar_min - step))
                        break;

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

                        used = (positive)(memory_copy_apart_end(
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
                if (system_write_all(standard_output_descriptor, output, filled) != filled)
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

static fn env_drop(string_address name)
{
        positive length = string_length(name);
        positive keep = 0;

        for (positive i = 0; i < env_have; i++)
                if (!environment_key_is(env_list[i], name, length))
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
                        if (environment_key_is(env_list[i], entry, length))
                        {
                                env_list[i] = entry;
                                return true;
                        }
                }
        }

        if (!shell_array_room(env_list, env_room, env_have + 2))
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

        if (!shell_array_room(env_dropped, env_dropped_room, env_drops + 1))
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
        if (!shell_array_room(env_split_store, env_split_room, length + 1) ||
            !shell_array_room(env_words, env_words_room, given + length / 2 + 2))
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

        if (!shell_array_room(env_words, env_words_room, have + count - index + 1))
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

        if (!shell_array_room(env_list, env_room, env_have + 1))
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

        if (where && system_change_directory(where) < 0)
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

// printenv -------------------------------------------------------
static const file_long printenv_longs[] = {
    {(string_address) "null", '0'},
    {null, 0},
};

static b32 file_printenv()
{
        file_taking taking = {
            .program = (string_address) "printenv",
            .allowed = (string_address) "0",
            .valued = (string_address) "",
            .longs = printenv_longs,
        };

        // GNU reserves 2 for syntax and 1 for a name that is not present.
        if (!file_take(address_of taking))
                return 2;

        positive count = (positive)program_argument_count();
        positive first = taking.first;
        bool zero = (taking.flags & FILE_FLAG('0')) != 0;

        if (first == count)
        {
                string_address address_to environment = file_environment_all();

                for (positive i = 0; environment && environment[i]; i++)
                        file_written(environment[i], zero);

                log_flush();
                return 0;
        }

        b32 status = 0;

        while (first < count)
        {
                string_address name = program_argument((b32)first++);
                string_address value = string_first_of(name, '=')
                                           ? null
                                           : file_environment(name);

                if (value)
                        file_written(value, zero);
                else
                        status = 1;
        }

        log_flush();
        return status;
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

        file_account_label(value, group, names, text);
        file_written(text, zero);
}

/*
        The primary group first and then every group whose member list names
        the user. The kernel hands the supplementary groups back in its own
        order and does not promise the primary one is among them, so the group
        actually in effect belongs at the front either way.
*/
/* Reusable numeric-ID vector for id and the util-linux process applets. */
static p32 address_to file_id_scratch;
static positive file_id_scratch_room;

static bool id_group_add(positive value, positive address_to have)
{
        for (positive i = 0; i < address_to have; i++)
                if (file_id_scratch[i] == (p32)value)
                        return true;

        if (!shell_array_room(file_id_scratch, file_id_scratch_room, address_to have + 1))
                return false;

        file_id_scratch[address_to have] = (p32)value;
        address_to have = address_to have + 1;
        return true;
}

static bool id_groups_named(string_address name, positive primary,
                            positive address_to have)
{
        p8 address_to text = file_account_text(FILE_ACCOUNT_GROUP);
        positive wanted = string_length(name);
        positive at = 0;

        address_to have = 0;

        if (!id_group_add(primary, have))
                return false;

        file_account_record record;

        // name:password:gid:member,member -- the gid is the record's second
        // field, and the members are whatever follows the colon after it.
        while (file_account_next(text, address_of at, 2, address_of record))
        {
                positive taken = 0;
                positive value = string_digits_max(record.value, record.value_length,
                                                   address_of taken);
                string_address members = record.value + record.value_length;

                if (!record.has_value || !record.value_length ||
                    taken != record.value_length || !string_is(members, ':') ||
                    value == primary)
                        continue;

                members++;

                positive stop = (positive)(string_first_of_or_end(members, '\n') -
                                           members);
                positive i = 0;

                while (i < stop)
                {
                        positive from = i;

                        while (i < stop && members[i] != ',')
                                i++;

                        if (i - from == wanted &&
                            !memory_compare(members + from, name, wanted))
                        {
                                if (!id_group_add(value, have))
                                        return false;

                                break;
                        }

                        if (i < stop)
                                i++;
                }
        }

        return true;
}

static bool id_groups_process(positive real, positive effective,
                              positive address_to have)
{
        bipolar groups = system_call_2(syscall(getgroups), 0, 0);

        address_to have = 0;

        if (groups < 0 ||
            !shell_array_room(file_id_scratch, file_id_scratch_room, (positive)groups + 2))
                return false;

        if (groups &&
            system_call_2(syscall(getgroups), (positive)groups,
                          (positive)(file_id_scratch + 2)) != groups)
                return false;

        if (!id_group_add(real, have) ||
            (effective != real && !id_group_add(effective, have)))
                return false;

        for (positive i = 0; i < (positive)groups; i++)
                if (!id_group_add(file_id_scratch[i + 2], have))
                        return false;

        return true;
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

                        file_account_label(members[i], true, names, text);

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
        if (first < count)
        {
                b32 status = 0;

                while (first < count)
                {
                        string_address who = program_argument((b32)first++);
                        bipolar user = file_user_id(who);
                        bipolar group = file_account_id(
                            file_account_text(FILE_ACCOUNT_USER), who, 3);

                        if (user < 0 || group < 0)
                        {
                                string_format(file_fail, "id: '%s': no such user\n", who);
                                status = 1;
                                continue;
                        }

                        positive have;

                        if (!id_groups_named(who, (positive)group,
                                             address_of have))
                        {
                                file_fail("id: group list is too large\n", 0);
                                status = 1;
                                continue;
                        }

                        id_written((positive)user, (positive)group,
                                   file_id_scratch, have, flags, names, zero);
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

        positive have;

        if (!id_groups_process(group, group, address_of have))
        {
                file_fail("id: failed to get groups for the current process\n", 0);
                return 1;
        }

        id_written(user, group, file_id_scratch, have, flags, names, zero);

        log_flush();

        return 0;
}

// groups ----------------------------------------------------------
static bool groups_written(positive have)
{
        bool known = true;

        for (positive i = 0; i < have; i++)
        {
                p8 text[FILE_NAME_MAX];

                if (i)
                        log(" ", 1);

                if (!file_account_label(file_id_scratch[i], true, true, text))
                {
                        string_format(file_fail,
                                      "groups: cannot find name for group ID %u\n",
                                      (positive)file_id_scratch[i]);
                        known = false;
                }

                log(text, 0);
        }

        log("\n", 1);
        return known;
}

static b32 file_groups()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address) "groups",
            .allowed = (string_address) "",
            .valued = (string_address) "",
            .operand = file_operand,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;

        b32 status = 0;
        positive have;

        if (!file_operand_count)
        {
                positive real = (positive)system_call(syscall(getgid));
                positive effective = (positive)system_call(syscall(getegid));

                if (!id_groups_process(real, effective, address_of have))
                {
                        file_fail("groups: failed to get groups for the current process\n",
                                  0);
                        return 1;
                }

                status = groups_written(have) ? 0 : 1;
        }
        else
                for (positive i = 0; i < file_operand_count; i++)
                {
                        string_address who = file_operand_at(i);
                        bipolar user = file_user_id(who);
                        bipolar group = file_account_id(
                            file_account_text(FILE_ACCOUNT_USER), who, 3);

                        if (user < 0 || group < 0)
                        {
                                string_format(file_fail,
                                              "groups: '%s': no such user\n", who);
                                status = 1;
                                continue;
                        }

                        if (!id_groups_named(who, (positive)group,
                                             address_of have))
                        {
                                file_fail("groups: group list is too large\n", 0);
                                status = 1;
                                continue;
                        }

                        log(who, 0);
                        log(" : ", 3);

                        if (!groups_written(have))
                                status = 1;
                }

        log_flush();
        return status;
}

// whoami ---------------------------------------------------------
static b32 file_whoami()
{
        file_simple_operand_count = 0;
        file_taking taking = {
            .program = (string_address) "whoami",
            .allowed = (string_address) "",
            .valued = (string_address) "",
            .operand = file_simple_operand,
        };

        if (!file_take(address_of taking))
                return 1;

        if (file_simple_operand_count)
        {
                string_format(file_fail, "whoami: extra operand '%s'\n",
                              file_simple_operand_list[0]);
                return 1;
        }

        positive user = (positive)system_call(syscall(geteuid));
        p8 name[FILE_NAME_MAX];

        if (!file_user_name(user, name, FILE_NAME_MAX))
        {
                string_format(file_fail,
                              "whoami: cannot find name for user ID %u\n", user);
                return 1;
        }

        file_line(name);
        log_flush();
        return 0;
}

// logname --------------------------------------------------------
typedef struct
{
        p16 type;
        p16 padding;
        p32 process;
        p8 line[32];
        p8 identity[4];
        p8 user[32];
        p8 host[256];
        p16 termination;
        p16 exit;
        b32 session;
        b32 seconds;
        b32 microseconds;
        p32 address[4];
        p8 reserved[20];
} file_utmp;

_Static_assert(sizeof(file_utmp) == 384, "Linux utmp record is 384 bytes");
_Static_assert(__builtin_offsetof(file_utmp, line) == 8,
               "Linux utmp line offset");
_Static_assert(__builtin_offsetof(file_utmp, user) == 44,
               "Linux utmp user offset");

static bool file_logname_utmp(string_address tty, p8 address_to name)
{
        // GNU consults utmp only for the traditional /dev/tty namespace.
        if (string_compare_max(tty, (string_address) "/dev/tty", 8))
                return false;

        bipolar handle = system_open_at(AT_FDCWD,
                                       "/var/run/utmp",
                                       FILE_READ);

        if (handle < 0)
                return false;

        string_address wanted = tty + 5;
        positive wanted_length = string_length(wanted);
        file_utmp record;

        while (true)
        {
                positive filled = 0;

                while (filled < sizeof(record))
                {
                        bipolar got = system_read_retry(
                            (positive)handle, (p8 address_to)address_of record + filled,
                            sizeof(record) - filled);

                        if (got <= 0)
                        {
                                system_close(handle);
                                return false;
                        }

                        filled += (positive)got;
                }

                positive line_length =
                    string_length_max(record.line, sizeof(record.line));
                positive user_length =
                    string_length_max(record.user, sizeof(record.user));

                if (record.type == 7 && user_length &&
                    line_length == wanted_length &&
                    !memory_compare(record.line, wanted, wanted_length))
                {
                        memory_copy_apart_end(name, record.user, user_length);
                        system_close(handle);
                        return true;
                }
        }
}

static b32 file_logname()
{
        file_simple_operand_count = 0;
        file_taking taking = {
            .program = (string_address) "logname",
            .allowed = (string_address) "",
            .valued = (string_address) "",
            .operand = file_simple_operand,
        };

        if (!file_take(address_of taking))
                return 1;

        if (file_simple_operand_count)
        {
                string_format(file_fail, "logname: extra operand '%s'\n",
                              file_simple_operand_list[0]);
                return 1;
        }

        p8 loginuid[32];
        positive user = positive_max;
        p8 name[65];
        bool found = file_slurp((string_address) "/proc/self/loginuid", loginuid,
                                sizeof(loginuid)) > 0 &&
                     string_digits_exact(loginuid, address_of user) &&
                     user < p32_max &&
                     file_user_name(user, name, sizeof(name)) &&
                     string_length(name) < 64;

        if (!found)
        {
                p8 tty[FILE_PATH_MAX];

                if (file_input_terminal_name(tty, sizeof(tty)) >= 0)
                {
                        found = file_logname_utmp(tty, name);

                        if (!found)
                        {
                                file_facts facts;

                                found = file_look_at(tty, address_of facts) &&
                                        file_user_name(facts.owner, name,
                                                       sizeof(name)) &&
                                        string_length(name) < 64;
                        }
                }
        }

        if (!found)
        {
                file_fail("logname: no login name\n", 0);
                return 1;
        }

        file_line(name);
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

        /*
                Every field uname answers with, in the order -a writes them.

                part_of_all is the five the kernel actually keeps, which is
                what -a takes. beside_all says whether asking for a field by
                its own letter still writes it when -a was given too: -p and
                -i are not fields the kernel keeps, so on their own they
                answer unknown and beside -a they are left out, which is what
                -a means by "except omit -p and -i if unknown" and is why -a
                is not simply all the others.
        */
        struct
        {
                p8 letter;
                string_address text;
                bool part_of_all;
                bool beside_all;
        } fields[] = {
            {'s', facts.system, true, true},
            {'n', facts.node, true, true},
            {'r', facts.release, true, true},
            {'v', facts.version, true, true},
            {'m', facts.machine, true, true},
            {'p', (string_address) "unknown", false, false},
            {'i', (string_address) "unknown", false, false},
            {'o', (string_address) "Moonwater", false, true},
        };

        bool all = (flags & FILE_FLAG('a')) != 0;
        positive written = 0;

        // Nothing asked for at all is the kernel name, which is the first
        // field and the only one that answers to having been asked nothing.
        if (!flags)
                flags = FILE_FLAG('s');

        for (positive i = 0; i < array_count(fields); i++)
        {
                bool asked = (flags & FILE_FLAG(fields[i].letter)) != 0;
                bool wanted = all ? fields[i].part_of_all ||
                                        (asked && fields[i].beside_all)
                                  : asked;

                if (!wanted)
                        continue;

                if (written++)
                        log(" ", 1);

                log(fields[i].text, 0);
        }

        log("\n", 1);
        log_flush();

        return 0;
}

// nproc -----------------------------------------------------------
/* Both nproc number grammars are saturating decimal. --ignore accepts a
   leading plus but no trailing space; OpenMP accepts trailing space and a
   comma-delimited nesting tail, but not a plus. */
static bool nproc_decimal(string_address text, bool plus, bool trailing,
                          bool comma, positive address_to value)
{
        if (!text)
                return false;

        while (byte_is_space(string_get(text)))
                text++;

        if (plus && string_is(text, '+'))
                text++;

        if (!byte_is_digit(string_get(text)))
                return false;

        positive number = 0;

        while (byte_is_digit(string_get(text)))
        {
                positive digit = (positive)(string_get(text++) - '0');

                if (number > (positive_max - digit) / 10)
                        number = positive_max;
                else
                        number = number * 10 + digit;
        }

        if (trailing)
                while (byte_is_space(string_get(text)))
                        text++;

        if (string_get(text) && !(comma && string_is(text, ',')))
                return false;

        address_to value = number;
        return true;
}

static positive nproc_cpu_list(string_address path)
{
        p8 text[FILE_PATH_MAX];

        if (file_slurp(path, text, sizeof(text)) <= 0)
                return 0;

        positive at = 0;
        positive total = 0;

        while (text[at])
        {
                while (byte_is_space(text[at]))
                        at++;

                positive used = 0;
                positive first = string_digits(text + at, address_of used);

                if (!used)
                        return total;

                at += used;

                positive last = first;

                if (text[at] == '-')
                {
                        at++;

                        last = string_digits(text + at, address_of used);

                        if (!used)
                                return 0;

                        at += used;
                }

                if (last < first || total > positive_max - (last - first + 1))
                        return 0;

                total += last - first + 1;

                if (text[at] != ',')
                {
                        while (byte_is_space(text[at]))
                                at++;

                        return text[at] ? 0 : total;
                }

                at++;
        }

        return total;
}

#define NPROC_AFFINITY_WORDS 1024
static positive nproc_affinity_words[NPROC_AFFINITY_WORDS];

static positive nproc_affinity_count()
{
        bipolar used = system_call_3(syscall(sched_getaffinity), 0,
                                     sizeof(nproc_affinity_words),
                                     (positive)nproc_affinity_words);

        if (used <= 0 || (positive)used > sizeof(nproc_affinity_words))
                return 0;

        positive count = 0;
        positive whole = (positive)used / sizeof(positive);
        positive spare = (positive)used % sizeof(positive);

        for (positive i = 0; i < whole; i++)
                count += bits_counted(nproc_affinity_words[i]);

        p8 address_to tail = (p8 address_to)(nproc_affinity_words + whole);

        for (positive i = 0; i < spare; i++)
                count += bits_counted(tail[i]);

        return count;
}

static bool nproc_cgroup_mount(p8 address_to into)
{
        if (file_exists(AT_FDCWD,
                        (string_address) "/sys/fs/cgroup/cgroup.controllers"))
        {
                string_copy_max_end(into, (string_address) "/sys/fs/cgroup",
                                    FILE_PATH_MAX - 1);
                return true;
        }

        storage_mount_table table;

        if (!storage_mount_table_load(address_of table, null))
                return false;

        bool found = false;

        for (positive at = 0; at < table.count; at++)
                if (!string_compare(table.entry[at].type,
                                    (string_address) "cgroup2"))
                {
                        string_copy_max_end(into, table.entry[at].target,
                                            FILE_PATH_MAX - 1);
                        found = true;
                        break;
                }

        storage_mount_table_release(address_of table);
        return found;
}

static positive nproc_cgroup_quota()
{
        bipolar policy = system_call_1(syscall(sched_getscheduler), 0);

        // Realtime and deadline scheduling do not honor CFS CPU quotas.
        if (policy < 0 || policy == 1 || policy == 2 || policy == 6)
                return positive_max;

        p8 mount[FILE_PATH_MAX];

        if (!nproc_cgroup_mount(mount))
                return positive_max;

        p8 record[FILE_PATH_MAX];
        bipolar length = file_slurp((string_address) "/proc/self/cgroup", record,
                                    sizeof(record));

        if (length <= 0)
                return positive_max;

        string_address found = null;

        for (positive at = 0; at < (positive)length;)
        {
                if (record[at] == '0' && record[at + 1] == ':' &&
                    record[at + 2] == ':' && record[at + 3] == '/')
                {
                        found = record + at + 3;
                        break;
                }

                while (record[at] && record[at] != '\n')
                        at++;

                if (record[at])
                        at++;
        }

        if (!found)
                return positive_max;

        p8 group[FILE_PATH_MAX];
        positive group_length = 0;

        while (found[group_length] && found[group_length] != '\n' &&
               group_length + 1 < sizeof(group))
        {
                group[group_length] = string_get(found + group_length);
                group_length++;
        }

        group[group_length] = end;

        if (!group_length)
                return positive_max;

        positive lowest = positive_max;

        for (;;)
        {
                p8 directory[FILE_PATH_MAX];
                p8 path[FILE_PATH_MAX];
                p8 limit[128];

                path_join(directory, sizeof(directory), mount, group);
                path_join(path, sizeof(path), directory,
                          (string_address) "cpu.max");

                if (file_slurp(path, limit, sizeof(limit)) > 0 &&
                    byte_is_digit(limit[0]))
                {
                        positive used = 0;
                        positive quota = string_digits(limit, address_of used);
                        positive at = used;

                        while (byte_is_space(limit[at]))
                                at++;

                        positive period_used = 0;
                        positive period =
                            string_digits(limit + at, address_of period_used);

                        if (used && period_used && period)
                        {
                                positive cpus = quota / period;
                                positive remainder = quota % period;

                                if (remainder >= period / 2 + (period & 1))
                                        cpus++;

                                if (!cpus)
                                        cpus = 1;

                                if (cpus < lowest)
                                        lowest = cpus;

                                if (lowest == 1)
                                        return 1;
                        }
                }

                if (group[0] == '/' && !group[1])
                        break;

                p8 address_to slash = (p8 address_to)string_last_of(group, '/');

                if (!slash)
                        break;

                if (slash == group)
                        group[1] = end;
                else
                        address_to slash = end;
        }

        return lowest;
}

static positive nproc_available(bool all)
{
        if (!all)
        {
                positive count = nproc_affinity_count();

                if (count)
                        return count;
        }

        positive count = nproc_cpu_list(all
                                            ? (string_address)
                                                  "/sys/devices/system/cpu/possible"
                                            : (string_address)
                                                  "/sys/devices/system/cpu/online");

        if (!count && all)
                count = nproc_cpu_list(
                    (string_address) "/sys/devices/system/cpu/present");

        if (!count)
                count = nproc_affinity_count();

        return count ? count : 1;
}

static positive nproc_omp(string_address name)
{
        positive value = 0;

        if (!nproc_decimal(file_environment(name), false, true, true,
                           address_of value))
                return 0;

        return value;
}

static const file_long nproc_longs[] = {
    {(string_address) "all", 'a'},
    {(string_address) "ignore", 'i'},
    {null, 0},
};

static b32 file_nproc()
{
        file_simple_operand_count = 0;
        file_taking taking = {
            .program = (string_address) "nproc",
            .allowed = (string_address) "",
            .valued = (string_address) "i",
            .longs = nproc_longs,
            .operand = file_simple_operand,
        };

        if (!file_take(address_of taking))
                return 1;

        if (file_simple_operand_count)
        {
                string_format(file_fail, "nproc: extra operand '%s'\n",
                              file_simple_operand_list[0]);
                return 1;
        }

        positive ignore = 0;
        string_address ignored = file_option_value(address_of taking, 'i');

        if (ignored &&
            !nproc_decimal(ignored, true, false, false, address_of ignore))
        {
                string_format(file_fail, "nproc: invalid number: '%s'\n", ignored);
                return 1;
        }

        bool all = (taking.flags & FILE_FLAG('a')) != 0;
        positive count;

        if (all)
                count = nproc_available(true);
        else
        {
                positive threads = nproc_omp((string_address) "OMP_NUM_THREADS");
                positive limit = nproc_omp((string_address) "OMP_THREAD_LIMIT");

                if (!limit)
                        limit = positive_max;

                if (threads)
                        count = threads < limit ? threads : limit;
                else if (limit == 1)
                        count = 1;
                else
                {
                        positive quota = nproc_cgroup_quota();
                        count = nproc_available(false);

                        if (quota < count)
                                count = quota;

                        if (limit < count)
                                count = limit;
                }
        }

        if (!count || ignore >= count)
                count = 1;
        else
                count -= ignore;

        positive_to_string(log, count);
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

static string_address mktemp_letters =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
static string_address mktemp_template;
static bool mktemp_extra_template;

static fn mktemp_operand(b32 index)
{
        if (mktemp_template)
                mktemp_extra_template = true;
        else
                mktemp_template = program_argument(index);
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

// --tmpdir is the one option here spelled without a letter, and T is a letter
// mktemp has not got: it is left out of `allowed` so -T is still a mistake.
static const file_long mktemp_longs[] = {
    {(string_address) "directory", 'd'},
    {(string_address) "dry-run", 'u'},
    {(string_address) "quiet", 'q'},
    {(string_address) "suffix", 'S'},
    {(string_address) "tmpdir", 'T'},
    {null, 0},
};

static b32 file_mktemp()
{
        mktemp_template = null;
        mktemp_extra_template = false;

        file_taking taking = {
            .program = (string_address) "mktemp",
            .allowed = (string_address) "Sdpqtu",
            .valued = (string_address) "Sp",
            .optional = (string_address) "T",
            .longs = mktemp_longs,
            .operand = mktemp_operand,
        };

        if (!file_take(address_of taking))
                return 1;

        bool directory = (taking.flags & FILE_FLAG('d')) != 0;
        bool dry = (taking.flags & FILE_FLAG('u')) != 0;
        bool quiet = (taking.flags & FILE_FLAG('q')) != 0;
        bool rooted = (taking.flags & (FILE_FLAG('p') | FILE_FLAG('t') |
                                       FILE_FLAG('T'))) != 0;
        string_address base = file_option_value(address_of taking, 'T');
        string_address suffix = file_option_value(address_of taking, 'S');
        string_address template = mktemp_template;
        p8 path[FILE_PATH_MAX];
        positive length = 0;
        positive marks_at;
        positive marks = 0;

        if (!base)
                base = file_option_value(address_of taking, 'p');

        if (mktemp_extra_template)
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

                memory_copy_apart(path, base, length);

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

        if (suffix && (string_first_of(suffix, '/') || !length ||
                       path[length - 1] != 'X'))
        {
                file_fail(string_first_of(suffix, '/')
                              ? (string_address)
                                    "mktemp: suffix may not contain a slash\n"
                              : (string_address)
                                    "mktemp: with --suffix, template must end in X\n",
                          0);
                return 1;
        }

        // Only the template's own bytes are looked at for the run: a
        // directory with an X in its name is not a place to put randomness.
        positive template_at = length - added;

        while (marks_at > template_at && path[marks_at - 1] != 'X')
                marks_at--;

        while (marks_at > template_at && path[marks_at - 1] == 'X')
        {
                marks_at--;
                marks++;
        }

        if (suffix)
        {
                positive suffix_length = string_length(suffix);

                if (suffix_length > FILE_PATH_MAX - 1 - length)
                {
                        file_fail("mktemp: template too long\n", 0);
                        return 1;
                }

                memory_copy_apart_end(path + length, suffix, suffix_length);
                length += suffix_length;
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
                        answer = system_make_directory_at(AT_FDCWD, path, 0700);
                else
                {
                        answer = system_open_at_mode(
                                               AT_FDCWD,
                                               path,
                                               FILE_WRITE | FILE_EXCLUSIVE, 0600);

                        if (answer >= 0)
                                system_close(answer);
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
/* date and strftime used to carry separate calendar-format state machines.
   Keep one engine: the stack covers ordinary command lines and an exceptional
   width grows through the shared byte store until the bounded formatter fits. */
static bool date_shape(writer write, b64 when, string_address format)
{
        time_t stamp = (time_t)when;
        tm broken;
        p8 fixed[512];
        positive length;

        if (!gmtime_r(address_of stamp, address_of broken))
                return false;

        length = clock_format_extended(fixed, sizeof(fixed), format,
                                       address_of broken);
        if (length || !string_get(format))
        {
                if (length)
                        write(fixed, length);
                return true;
        }

        byte_store grown = {0};
        positive wanted = sizeof(fixed) * 2;

        while (wanted)
        {
                if (!byte_store_reserve(address_of grown, wanted, wanted))
                        break;

                length = clock_format_extended(grown.bytes, grown.room,
                                               format, address_of broken);
                if (length)
                {
                        write(grown.bytes, length);
                        byte_store_release(address_of grown);
                        return true;
                }

                if (wanted > positive_max / 2)
                        break;
                wanted *= 2;
        }

        byte_store_release(address_of grown);
        return false;
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
                bipolar looked = file_look_code(AT_FDCWD, of_file, 0, address_of facts);

                if (looked < 0)
                {
                        string_format(file_fail, "date: %s: %s\n", of_file,
                                      file_reason(looked));
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

        if (!format)
                format = iso   ? iso
                         : rfc ? (string_address) "%a, %d %b %Y %H:%M:%S %z"
                               : (string_address) "%a %b %e %H:%M:%S %Z %Y";

        if (!date_shape(log, when, format))
        {
                file_fail("date: formatted value is too large\n", 0);
                return 1;
        }

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

/*
        The word table and the item buffer are sized once, before the mark
        that every batch is reset to, for the fullest batch the input can
        make: the command's own words, and one word for every byte a batch
        holds, since an item can be empty.  Nothing taken after the mark is
        pointed at across a reset, which is what lets the bytes of a batch
        that has run be given back rather than kept until the input ends.
*/
static positive xargs_mark;

static bool xargs_add(string_address text, positive length)
{
        if (length == positive_max || xargs_used > positive_max - length - 1 ||
            xargs_word_count + 2 > xargs_word_room)
                return false;

        p8 address_to made = (p8 address_to)text_arena_take(length + 1);

        if (!made)
                return false;

        memory_copy_end(made, text, length);
        xargs_words[xargs_word_count++] = made;
        xargs_used += length + 1;

        return true;
}

// An item longer than a whole batch is one the kernel would refuse as a
// single argument anyway, and is refused here with the same words.
static fn xargs_item_put(p8 letter)
{
        if (xargs_item_length + 2 > xargs_item_room)
        {
                xargs_item_broken = true;
                return;
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

        if (system_pipe(ends,
                          XARGS_O_CLOEXEC) < 0)
                return XARGS_EXEC_SYSTEM;

        bipolar child = system_fork();

        if (child == 0)
        {
                system_close(ends[0]);
                bipolar answer = file_exec_path_try(words);

                system_write_all((positive)ends[1], address_of answer,
                                 sizeof(answer));
                exit(answer == -ERROR_ACCESS ? 126 : 127);
        }

        system_close(ends[1]);


        if (child < 0)
        {
                system_close(ends[0]);
                return XARGS_EXEC_SYSTEM;
        }

        bipolar exec_error = 0;
        bipolar got = system_read_retry((positive)ends[0], address_of exec_error,
                                        sizeof(exec_error));

        system_close(ends[0]);
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
                memory_copy_apart(words, xargs_words,
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
        text_arena_used = xargs_mark;
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

        memory_copy_apart(xargs_template, xargs_words,
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

                        memory_copy_apart(made + used, scan, kept);
                        used += kept;

                        if (!hit)
                                break;

                        memory_copy_apart(made + used, item, item_length);
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
                file_fail("xargs: argument line too long\n", 0);
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
                xargs_reset();
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

        positive words = (index < count ? count - index : 1) + XARGS_BATCH_BYTES + 3;

        if (!array_arena_reserve(xargs_words, xargs_word_room, 0, words, words,
                                 text_arena_grow))
                return 1;

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

        xargs_item = (p8 address_to)text_arena_take(XARGS_BATCH_BYTES + 1);
        xargs_buffer = (p8 address_to)text_arena_take(XARGS_READ_BYTES);

        if (!xargs_item || !xargs_buffer)
                return 1;

        xargs_item_room = XARGS_BATCH_BYTES + 1;
        xargs_mark = text_arena_used;

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
                        if (byte_is_blank(letter))
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
