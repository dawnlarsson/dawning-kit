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
enum { REGEX_FIRST, REGEX_LONGEST, REGEX_EXACT_LONGEST, REGEX_CAPTURES = 4 };
static bool regex_find(p8 mode, string_address text, positive length, positive from);

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
#define ERROR_LOOP 40
#define ERROR_EXEC_FORMAT 8
#define ERROR_NOT_EMPTY 39
#define ERROR_INPUT_OUTPUT 5
#define ERROR_AGAIN 11
#define ERROR_NO_MEMORY 12
#define ERROR_BUSY 16
#define ERROR_NO_DEVICE 19
#define ERROR_SYSTEM_FILES 23
#define ERROR_PROCESS_FILES 24
#define ERROR_TEXT_BUSY 26
#define ERROR_FILE_TOO_LARGE 27
#define ERROR_NO_SPACE 28
#define ERROR_READ_ONLY 30
#define ERROR_TOO_MANY_LINKS 31
#define ERROR_BROKEN_PIPE 32
#define ERROR_OUT_OF_RANGE 34
#define ERROR_TOO_MANY_LEVELS 40
#define ERROR_NOT_SUPPORTED 95
#define ERROR_PROTOCOL_TYPE 91
#define ERROR_NOT_CONNECTED 107
#define ERROR_CONNECTION_REFUSED 111
#define ERROR_OVER_QUOTA 122

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
                string_address step = specification;
                positive value;
                if (!string_digits_checked(address_of step, 8, address_of value) ||
                    string_get(step) || value > 07777)
                        return false;

                // Fewer than five digits mention only the special bits they
                // set; five or more mention all of them.
                positive mentioned = step - specification < 5 ? value & 06000 : 06000;

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
#define FILE_RESOLVE_DIRECTORIES 1
#define FILE_RESOLVE_FINAL_MISSING 2
#define FILE_RESOLVE_MISSING_TAIL 4
#define FILE_RESOLVE_UNRESOLVED 8

static bool file_resolve_as(string_address path, p8 address_to into,
                            bool follow, p8 policy)
{
        p8 rest[FILE_PATH_MAX];
        p8 link[FILE_PATH_MAX];
        positive at = 0;
        positive length = 0;
        positive hops = 0;
        bool missing_walk = false;

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
                {
                        /* A final dot says the preceding component itself
                           must be a directory.  An interior dot adds no such
                           constraint to a missing -s path. */
                        if (missing_walk)
                        {
                                string_address after = rest + at;
                                after += string_span_of_set(after, "/");
                                if (!*after)
                                        return false;
                        }
                        continue;
                }

                if (piece == 2 && rest[start] == '.' && rest[start + 1] == '.')
                {
                        if (missing_walk)
                                return false;

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

                file_facts facts;
                bipolar looked = 0;
                bipolar seen = 0;
                bool need_directory =
                    (policy & FILE_RESOLVE_DIRECTORIES) && rest[at];

                /* Strict walks need the component's type anyway. Reuse that
                   lookup and read link text only for an actual symlink.
                   Lexical -s/-L walks still validate through the referent;
                   unconstrained callers retain their readlink-only path. */
                if (need_directory)
                {
                        looked = file_look_code(
                            AT_FDCWD, into, follow ? AT_SYMLINK_NOFOLLOW : 0,
                            address_of facts);
                        if (follow && looked == 0 &&
                            (facts.mode & MODE_FORMAT) == MODE_LINK)
                                seen = system_read_link_at(
                                    AT_FDCWD, into, link, FILE_PATH_MAX - 1);
                }
                else if (follow)
                        seen = system_read_link_at(AT_FDCWD, into, link,
                                                   FILE_PATH_MAX - 1);

                if (seen <= 0)
                {
                        if (need_directory)
                        {
                                bool directory = looked == 0 &&
                                                 (facts.mode & MODE_FORMAT) ==
                                                     MODE_DIRECTORY;

                                if (!directory)
                                {
                                        if (looked != -ERROR_NO_ENTRY)
                                                return false;

                                        string_address after = rest + at;
                                        after += string_span_of_set(after,
                                                                    "/");
                                        bool final = !*after;
                                        if (!(policy &
                                              FILE_RESOLVE_MISSING_TAIL) &&
                                            (!(policy &
                                               FILE_RESOLVE_FINAL_MISSING) ||
                                             !final))
                                                return false;
                                        missing_walk = true;
                                }
                                else
                                        missing_walk = false;
                        }
                        continue;
                }

                if (++hops > 40)
                {
                        /* -m treats the link at the resolution ceiling like
                           an absent component and preserves its spelling. */
                        if (policy & FILE_RESOLVE_UNRESOLVED)
                                continue;
                        return false;
                }

                link[seen] = end;

                bool separator = rest[at] != end;
                positive fill = (positive)seen + separator;

                if (fill >= FILE_PATH_MAX)
                        return false;

                positive left = string_length_max(rest + at, FILE_PATH_MAX - fill);

                // What follows the link has to fit behind it whole; a tail
                // cut to fit is some other path.
                if (left >= FILE_PATH_MAX - fill)
                        return false;

                /* The unread tail can move in either direction. Preserve it
                   with the overlap-safe floor, then prepend the link once. */
                memory_copy(rest + fill, rest + at, left + 1);
                memory_copy_apart(rest, link, (positive)seen);
                if (separator)
                        rest[seen] = '/';

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

bool file_resolve(string_address path, p8 address_to into, bool follow)
{
        return file_resolve_as(path, into, follow, 0);
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

static bool file_moment_read_from(string_address text, b64 now, positive fraction,
                                   b64 address_to out,
                                   positive address_to nanoseconds)
{
        positive at = 0;

        address_to nanoseconds = fraction;

        at += string_span(text + at, string_set_blanks);

        if (string_is(text + at, '@'))
        {
                address_to nanoseconds = 0;
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
                                        address_to nanoseconds = 0;
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

                                address_to nanoseconds = 0;

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
                                                address_to nanoseconds = 0;
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
                                address_to nanoseconds = 0;
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
                address_to nanoseconds = 0;
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
                        address_to nanoseconds = 0;
                }
        }

        address_to out = days * 86400 + (b64)hour * 3600 + (b64)minute * 60 +
                         (b64)second + shift - zone;

        return true;
}

bool file_moment_read_exact(string_address text, b64 now, b64 address_to out,
                            positive address_to nanoseconds)
{
        return file_moment_read_from(text, now, 0, out, nanoseconds);
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
        bipolar error;
        positive have;
        positive at;
        p8 block[FILE_BLOCK];
} file_walk;

bool file_walk_open(file_walk address_to walk, bipolar directory, string_address path)
{
        walk->handle = system_open_at(directory, path,
                                     FILE_READ | O_DIRECTORY);
        walk->error = walk->handle < 0 ? walk->handle : 0;
        walk->have = 0;
        walk->at = 0;

        return walk->handle >= 0;
}

/* Callers keep their chosen buffer size and error policy. Kernel getdents
   records are trusted here; storage's defensive input walk is separate. */
static inline INLINE struct linux_dirent64 address_to file_directory_next(
    bipolar handle, p8 address_to block, positive capacity,
    positive address_to have, positive address_to at, bipolar address_to error)
{
        if (address_to at >= address_to have)
        {
                bipolar taken = system_read_directory(handle, block, capacity);

                if (taken <= 0)
                {
                        if (taken < 0)
                                address_to error = taken;
                        return null;
                }

                address_to have = (positive)taken;
                address_to at = 0;
        }

        struct linux_dirent64 address_to entry =
            (struct linux_dirent64 address_to)(block + address_to at);

        address_to at += entry->d_reclen;

        return entry;
}

struct linux_dirent64 address_to file_walk_next(file_walk address_to walk)
{
        return file_directory_next(walk->handle, walk->block, sizeof(walk->block),
                                   address_of walk->have, address_of walk->at,
                                   address_of walk->error);
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
                        string_format(log_error, "%s: %s '%s/%s': %s\n", program, (string_address) "cannot copy", source, entry->d_name, file_reason(-ERROR_NAME_TOO_LONG));
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

/*
        Whether a directory is visited before or after what is under it.

        chmod walks a tree from the top: a directory is changed and then read,
        because the mode it is given is what says whether it can be read at
        all. chown and chgrp walk it from the bottom, which is what the
        reference's own -v listing shows, and this is where the two differ.
*/
static bool file_change_after_contents;

static fn file_change_walk_as(bipolar directory, string_address name,
                              string_address shown, positive depth,
                              string_address program, b32 address_to status,
                              file_visit visit, bool report_walk_errors)
{
        bool here = file_is_directory(directory, name);

        if (!here || !file_change_after_contents)
                visit(directory, name, shown);

        if (!here)
                return;

        if (depth == 0)
        {
                string_format(log_error, "%s: '%s' is nested too deep\n",
                              program, shown);
                address_to status = 1;
                return;
        }

        file_walk walk;

        if (!file_walk_open(address_of walk, directory, name))
        {
                if (report_walk_errors)
                {
                        string_format(log_error,
                                      "%s: cannot open directory '%s': %s\n",
                                      program, shown, file_reason(walk.error));
                        address_to status = 1;
                }

                if (file_change_after_contents)
                        visit(directory, name, shown);

                return;
        }

        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                p8 below[FILE_PATH_MAX];

                if (!file_path_join(below, shown, entry->d_name))
                {
                        string_format(log_error, "%s: %s '%s/%s': %s\n", program, (string_address) "cannot access", shown, entry->d_name, file_reason(-ERROR_NAME_TOO_LONG));
                        address_to status = 1;
                        continue;
                }

                file_change_walk_as(walk.handle, entry->d_name, below,
                                    depth - 1, program, status, visit,
                                    report_walk_errors);
        }

        if (report_walk_errors && walk.error < 0)
        {
                string_format(log_error,
                              "%s: cannot read directory '%s': %s\n",
                              program, shown, file_reason(walk.error));
                address_to status = 1;
        }

        file_walk_close(address_of walk);

        if (file_change_after_contents)
                visit(directory, name, shown);
}

// The operand list those three read, which is the same list every time: each
// name is visited, and under -R so is everything under it.
static fn file_change_paths(positive first, positive count, bool recursive,
                            string_address program, b32 address_to status,
                            file_visit visit)
{
        bool ended = false;

        while (first < count)
        {
                string_address path = program_argument((b32)first++);

                //      A -- among the operands is the end of the options and
                //      not a name: the reference's getopt reads the whole
                //      line, so chmod 0600 -- -dash changes -dash. Only the
                //      first one is the marker; a second is a file called --.
                if (!ended && string_is(path, '-') && string_is(path + 1, '-') &&
                    !string_get(path + 2))
                {
                        ended = true;
                        continue;
                }

                if (recursive)
                        file_change_walk_as(AT_FDCWD, path, path, FILE_MAX_DEPTH,
                                            program, status, visit, false);
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

/*
        A word chosen from a list, and the complaint that names every word
        the option would have taken. Rows that answer alike are written on
        one line, the way the reference writes its synonyms.
*/
typedef struct
{
        string_address word;
        p8 answer;
        bool alone;
} file_word;

/*
        A word is matched by any beginning of it that no other word shares.

        The reference reads these with argmatch, which takes a prefix: --time=at
        is atime, --sort=si is size, and --time=m is modification because every
        word it begins answers alike. A prefix that begins words with different
        answers -- --sort=n, or the empty word, which begins them all -- is
        ambiguous rather than unknown, and says so.
*/
static bool file_word_begins(string_address value, string_address word)
{
        while (string_get(value))
        {
                if (string_get(value) != string_get(word))
                        return false;

                value++;
                word++;
        }

        return true;
}

static b32 file_word_among(string_address program, string_address option,
                           string_address value, const file_word address_to words,
                           positive count)
{
        b32 answer = -1;
        bool ambiguous = false;

        for (positive i = 0; i < count; i++)
        {
                if (!string_compare(value, words[i].word))
                        return words[i].answer;

                if (!file_word_begins(value, words[i].word))
                        continue;

                if (answer < 0)
                        answer = words[i].answer;
                else if (answer != words[i].answer)
                        ambiguous = true;
        }

        if (answer >= 0 && !ambiguous)
                return answer;

        string_format(log_error, "%s: %s argument '%s' for '%s'\nValid arguments are:\n",
                      program, ambiguous ? "ambiguous" : "invalid", value, option);

        for (positive i = 0; i < count; i++)
        {
                if (i && !words[i].alone && words[i].answer == words[i - 1].answer)
                {
                        string_format(log_error, ", '%s'", words[i].word);
                        continue;
                }

                if (i)
                        log_error("\n", 1);
                string_format(log_error, "  - '%s'", words[i].word);
        }

        log_error("\n", 1);
        string_format(log_error, "Try '%s --help' for more information.\n", program);
        return -1;
}

// -t names one directory; a second one is a question with two answers.
static bool file_one_target;
static bool file_two_targets;

static fn file_targets_begin()
{
        file_one_target = false;
        file_two_targets = false;
}

static bool file_target_seen(p8 letter, string_address value)
{
        if (letter != 't' || !value)
                return true;

        file_two_targets |= file_one_target;
        file_one_target = true;

        return true;
}

static bool file_targets_told(string_address program)
{
        if (!file_two_targets)
                return true;

        string_format(log_error, "%s: multiple target directories specified\n", program);
        return false;
}

// What every tool says when it was given nothing to work on, and the status
// each of them answers with.
static COLD b32 file_missing(string_address program)
{
        string_format(log_error, "%s: missing operand\n", program);

        return 1;
}

/*
        The question -i asks before something is destroyed.

        Written to the standard error and answered from the standard input,
        because that is where a person is when a script is not. Anything that
        does not begin with a y is a no, and so is an input that has ended --
        which is what makes the tools safe to run with no input at all.
*/
static bool file_answer_is_yes()
{
        p8 answer[2];
        bipolar got = system_read_once(0, answer, 1);

        if (got != 1)
                return false;

        bool yes = answer[0] == 'y' || answer[0] == 'Y';

        while (answer[0] != '\n' && system_read_once(0, answer, 1) == 1)
                ;

        return yes;
}

bool file_ask(string_address program, string_address question, string_address subject)
{
        string_format(log_error, "%s: %s '%s'? ", program, question, subject);

        return file_answer_is_yes();
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

/* Help wins over version; callers retain their writer and flush/status policy. */
static bool file_meta(file_taking address_to taking, string_address syntax,
                       writer output)
{
        if (!(taking->flags & (FILE_FLAG('h') | FILE_FLAG('V'))))
                return false;
        string_format(output, taking->flags & FILE_FLAG('h')
                                  ? "Usage: %s %s\n" : "%s from dawning-kit\n",
                      taking->program, syntax);
        return true;
}

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

// Said the way getopt says it, which is what every script that matches on a
// diagnostic has learned to expect: the letter after two dashes for a short
// option, the whole word for a long one.
static bool file_option_needs(file_taking address_to taking, string_address word)
{
        if (string_is(word, '-') && string_is(word + 1, '-'))
                string_format(log_error, "%s: option '%s' requires an argument\n",
                              taking->program, word);
        else
                string_format(log_error, "%s: option requires an argument -- '%s'\n",
                              taking->program, word + 1);

        /*
                Both families say where to look next here as well, in the
                same words they use for an option that is not there at all:
                coreutils out of usage() and util-linux out of errtryhelp().
                An option given without its value was a line short of the
                reference in every program that has one.
        */
        string_format(log_error, "Try '%s --help' for more information.\n",
                      taking->program);

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
        argument_cursor cursor = {.argc = (positive)program_argument_count(),
                                  .argv = program_argument_list(), .at = index};
        for (;;)
        {
                /* Legacy numeric operands/counts belong to utility policy,
                   before tokenization can split their digits into options. */
                if ((!cursor.letters || !*cursor.letters) &&
                    !cursor.operands_only && cursor.at < cursor.argc)
                {
                        string_address word = cursor.argv[cursor.at];
                        if (word[0] == '-' && word[1] != '-')
                        {
                                if (taking->numbers && (byte_is_digit(word[1]) || word[1] == '.'))
                                        break;
                                if (taking->digits && byte_is_digit(word[1]))
                                {
                                        positive bit = file_letter_bit(taking->digits);
                                        taking->flags |= (positive)1 << bit;
                                        taking->value[bit] = word + 1;
                                        taking->last = taking->digits;
                                        cursor.at++;
                                        cursor.letters = null;
                                        continue;
                                }
                        }
                }
                b32 option = argument_next(&cursor);
                if (option == ARGUMENT_END)
                        break;
                if (option == ARGUMENT_OPERAND)
                {
                        if (!taking->operand)
                        {
                                cursor.at--;
                                break;
                        }
                        taking->operand((b32)(cursor.at - 1));
                        continue;
                }
                bool long_option = option == ARGUMENT_LONG;
                p8 letter = long_option
                    ? file_long_letter(taking, cursor.word + 2, cursor.name_length)
                    : (p8)option;
                p8 named[3] = {'-', letter, end};
                string_address shown = long_option ? cursor.word : named;
                if (!letter || (!long_option && !string_first_of(taking->allowed, letter)))
                {
                        if (long_option)
                                string_format(log_error, "%s: unrecognized option '%s'\n",
                                              taking->program, shown);
                        else
                                string_format(log_error, "%s: invalid option -- '%s'\n",
                                              taking->program, named + 1);
                        /*
                                Both families say where to look next, in the
                                same words: coreutils out of usage() and
                                util-linux out of errtryhelp(). Twenty-one of
                                twenty-one programs were a line short of the
                                reference here, and nothing noticed because
                                no grammar fed a letter that is not there.
                        */
                        string_format(log_error,
                                      "Try '%s --help' for more information.\n",
                                      taking->program);
                        return false;
                }
                positive bit = file_letter_bit(letter);
                bool optional = file_option_among(taking->optional, letter) ||
                    (long_option && file_option_among(taking->long_optional, letter));
                bool valued = file_option_among(taking->valued, letter);
                if (cursor.attached && !optional && !valued)
                {
                        p8 name[FILE_NAME_MAX];

                        string_copy_max_end(name, cursor.word,
                                            min(cursor.name_length + 2, FILE_NAME_MAX - 1));
                        string_format(log_error, "%s: option '%s' doesn't allow an argument\n",
                                      taking->program, name);
                        //      Same line, and for the same reason, as the one
                        //      an unknown option gets: both families send the
                        //      reader on to --help and this path was a line
                        //      short of them.
                        string_format(log_error,
                                      "Try '%s --help' for more information.\n",
                                      taking->program);
                        return false;
                }
                taking->flags |= (positive)1 << bit;
                /* Short options supersede before missing-value errors; long
                   options do so only after their values have been accepted. */
                if (!long_option)
                        file_option_supersede(taking, letter);
                if (optional || valued)
                {
                        taking->last = letter;
                        string_address value = argument_value(&cursor, !optional);
                        if (value)
                                taking->value[bit] = value;
                        else if (!optional)
                                return file_option_needs(taking, shown);
                        else
                        {
                                taking->bare |= (positive)1 << bit;
                                if (!file_option_among(taking->sticky_optional, letter))
                                        taking->value[bit] = null;
                        }
                }
                if (long_option)
                        file_option_supersede(taking, letter);
                if (taking->seen && !taking->seen(letter,
                    long_option || optional || valued ? taking->value[bit] : null))
                        return false;
        }
        taking->first = cursor.at;
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

/* Fixture paths never cross a real/effective uid or gid boundary. */
static string_address file_environment_override(string_address name,
                                         string_address fallback)
{
        positive user = (positive)system_call(syscall(getuid));
        positive effective_user = (positive)system_call(syscall(geteuid));
        positive group = (positive)system_call(syscall(getgid));
        positive effective_group = (positive)system_call(syscall(getegid));

        if (user == effective_user && group == effective_group)
        {
                string_address value = file_environment(name);
                if (value && string_get(value))
                        return value;
        }
        return fallback;
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
        static const p8 accepted[] = {
            ERROR_NO_ENTRY, ERROR_NO_PROCESS, ERROR_BAD_DESCRIPTOR,
            ERROR_NOT_PERMITTED, ERROR_ACCESS, ERROR_EXISTS,
            ERROR_NOT_DIRECTORY, ERROR_IS_DIRECTORY, ERROR_NOT_EMPTY,
            ERROR_INVALID, ERROR_NOT_TERMINAL, ERROR_CROSS_DEVICE,
            ERROR_ILLEGAL_SEEK, ERROR_NAME_TOO_LONG, ERROR_INPUT_OUTPUT,
            ERROR_NO_DEVICE_ADDRESS, ERROR_ARGUMENT_LIST, ERROR_AGAIN,
            ERROR_NO_MEMORY, ERROR_BUSY, ERROR_NO_DEVICE,
            ERROR_SYSTEM_FILES, ERROR_PROCESS_FILES, ERROR_TEXT_BUSY,
            ERROR_FILE_TOO_LARGE, ERROR_NO_SPACE, ERROR_READ_ONLY,
            ERROR_TOO_MANY_LINKS, ERROR_BROKEN_PIPE, ERROR_OUT_OF_RANGE,
            ERROR_NO_SYSTEM_CALL, ERROR_TOO_MANY_LEVELS, ERROR_NOT_SUPPORTED,
            ERROR_NOT_CONNECTED, ERROR_OVER_QUOTA,
            /* A datagram socket asked to reach something that is not a
               listener says which of those two it was; logger, write and
               wall all report it. */
            ERROR_PROTOCOL_TYPE, ERROR_CONNECTION_REFUSED,
        };
        positive magnitude = code < 0 ? (positive)0 - (positive)code
                                      : (positive)code;

        return magnitude <= p8_max &&
               memory_first_of((address_any)accepted, (p8)magnitude,
                               sizeof(accepted))
                   ? system_error_message((bipolar)magnitude)
                   : (string_address)"Error";
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

/* cp and split share the kernel-copy cascade. Sparse extents supply explicit
   source/destination offsets; streams use the current descriptor positions.
   Capability bits persist across pieces so each unavailable floor is tried once. */
static inline INLINE bool file_copy_stream(
    bipolar in, bipolar out, p64 length, bool bounded,
    bool address_to range_copy, bool address_to send_copy,
    p64 address_to offsets)
{
        for (positive stage = 0; stage < 2; stage++)
        {
                bool address_to enabled = stage ? send_copy : range_copy;
                if (stage && (!bounded || length) && *enabled && offsets &&
                    system_seek(out, offsets[1], FILE_SEEK_SET) < 0)
                        return false;
                while ((!bounded || length) && *enabled)
                {
                        positive ask = !bounded || length > FILE_KERNEL_COPY_SIZE
                            ? FILE_KERNEL_COPY_SIZE : (positive)length;
                        bipolar copied = stage
                            ? file_send_range_once(in, offsets, out, ask)
                            : file_copy_range_once(in, offsets, out,
                                                   offsets ? offsets + 1 : null, ask);
                        if (copied > 0)
                        {
                                if (stage && offsets)
                                        offsets[1] += (positive)copied;
                                if (bounded)
                                        length -= (positive)copied;
                                continue;
                        }
                        if (!copied)
                                return !bounded;
                        if (copied == -4)
                                continue;
                        if (!file_copy_range_fallback(copied))
                                return false;

                        *enabled = false;
                }
        }

        if ((!bounded || length) && offsets &&
            (system_seek(in, offsets[0], FILE_SEEK_SET) < 0 ||
             system_seek(out, offsets[1], FILE_SEEK_SET) < 0))
                return false;

        while (!bounded || length)
        {
                positive ask = !bounded || length > sizeof(file_transfer)
                                   ? sizeof(file_transfer) : (positive)length;
                bipolar taken = system_read_retry((positive)in, file_transfer,
                                                   ask);

                if (taken < 0)
                        return false;
                if (!taken)
                        return !bounded;
                if (system_write_all((positive)out, file_transfer,
                                     (positive)taken) != (positive)taken)
                        return false;

                if (bounded)
                        length -= (positive)taken;
        }

        return true;
}

/* Copy one known data extent. A capability miss switches every later extent
   to the buffered path, but never copies the holes between them. */
static bool file_copy_extent(bipolar in, bipolar out, p64 start,
                             positive length, bool address_to range_copy,
                             bool address_to send_copy)
{
        p64 offsets[2] = {start, start};
        return file_copy_stream(in, out, length, true, range_copy, send_copy,
                                offsets);
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
                complete = file_copy_stream(in, out, 0, false,
                                            address_of range_copy,
                                            address_of send_copy, null);
        }

        system_close(in);

        if (system_close(out) < 0)
                complete = false;

        return complete;
}

bool file_copy_contents(bipolar from_directory, string_address from,
                        bipolar to_directory, string_address to, positive mode)
{
        return file_copy_contents_open(from_directory, from, to_directory, to,
                                       mode, FILE_WRITE);
}

/*
        Every component of a path, made in turn.

        `told` is called with each component this actually created, which is
        what mkdir -v reports -- a component that was already there is not a
        creation and is not named. On failure the component that could not be
        made is copied into `failed`, because that is the name the reference
        quotes and not the whole path it was given.
*/
/*
        What the reference says about a component that is already there.

        mkdir answers EEXIST and the walk then asks what the name is: a
        directory is the component already made and nothing to report, a name
        that can be looked at and is not a directory is Not a directory, and
        a name that cannot be looked at at all -- a symbolic link pointing at
        nothing -- keeps the EEXIST the kernel gave, because there is
        something there whatever it points at.
*/
static bipolar file_exists_as(string_address work)
{
        file_facts facts;

        if (file_look(AT_FDCWD, work, 0, address_of facts))
                return (facts.mode & MODE_FORMAT) == MODE_DIRECTORY
                           ? 0
                           : -ERROR_NOT_DIRECTORY;

        return -ERROR_EXISTS;
}

static bipolar file_make_parents_walk(string_address path, positive mode,
                                      fn(address_to told)(string_address),
                                      p8 address_to failed, bool address_to created)
{
        p8 work[FILE_PATH_MAX];
        positive length = string_length(path);

        if (created)
                address_to created = false;

        if (length >= FILE_PATH_MAX)
                return -ERROR_NAME_TOO_LONG;

        memory_copy_apart_end(work, path, length);

        //      A trailing run of slashes names the same directory as the
        //      name without them, so it is not a component of its own: the
        //      whole path, slashes and all, is what the last step makes and
        //      what -v then names.
        positive components = length;

        while (components > 1 && work[components - 1] == '/')
                components--;

        for (positive i = 1; i < components; i++)
        {
                if (work[i] != '/')
                        continue;

                work[i] = end;

                bipolar made = system_make_directory_at(AT_FDCWD, work, mode);
                bipolar already = made == -ERROR_EXISTS ? file_exists_as(work) : 0;

                if (made < 0 && (made != -ERROR_EXISTS || already))
                {
                        if (failed)
                                string_copy(failed, work);

                        work[i] = '/';

                        return made == -ERROR_EXISTS ? already : made;
                }

                //      The reference walks into each component it has made
                //      or found before making the next one, so a directory
                //      it cannot search is named here rather than the child
                //      that could not be reached through it.
                if (system_access_at(AT_FDCWD, work, 1) < 0)
                {
                        if (failed)
                                string_copy(failed, work);

                        work[i] = '/';

                        return -ERROR_ACCESS;
                }

                if (!made && told)
                        told(work);

                work[i] = '/';
        }

        bipolar made = system_make_directory_at(AT_FDCWD, work, mode);

        if (!made)
        {
                if (created)
                        address_to created = true;

                if (told)
                        told(work);

                return 0;
        }

        //      The last component is the one that was asked for, and the
        //      reference reports the kernel's own word about it: a name that
        //      is there and is not a directory is File exists here, where the
        //      same name in the middle of a path is Not a directory.
        if (made == -ERROR_EXISTS && !file_exists_as(work))
                return 0;

        if (failed)
                string_copy(failed, work);

        return made;
}



bool file_make_parents(string_address path, positive mode)
{
        return file_make_parents_walk(path, mode, null, null, null) == 0;
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
                return string_report(
                    log_error, false,
                    "%s: cannot combine --target-directory and --no-target-directory\n",
                    program);

        if (first >= count)
        {
                string_format(log_error, "%s: missing file operand\n", program);
                return false;
        }

        if (!into && first + 1 >= count)
        {
                string_format(log_error, "%s: missing destination file operand after '%s'\n",
                              program, program_argument((b32)first));
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
                        return string_report(log_error, false, "%s: extra operand '%s'\n", program,
                                      program_argument((b32)(first + 1)));

                pair(program_argument((b32)first), last);
                log_flush();

                return true;
        }

        //      The reference stats the target and quotes what the kernel
        //      said about it: not there is one answer and there but not a
        //      directory is another.
        if (!file_is_directory_through(last))
        {
                file_facts facts;
                bipolar looked = file_look_code(AT_FDCWD, last, 0, address_of facts);

                return string_report(log_error, false, "%s: target '%s': %s\n", program,
                                     last,
                                     file_reason(looked < 0 ? looked
                                                            : -ERROR_NOT_DIRECTORY));
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
                        string_format(log_error, "%s: %s '%s/%s': %s\n", program, (string_address) "cannot create", last, tail, file_reason(-ERROR_NAME_TOO_LONG));
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
        ls [OPTION]... [FILE]...

        There is an ls builtin in the shell as well. This is the one with the
        flags, and the builtin should call it rather than grow a second copy:
        a listing is not the shell's business, and a program can be replaced
        on its own.

        One name per line when the output is not a terminal and columns when
        it is, with every format, order, time, quoting style and indicator
        the reference ls answers to, decided the way it decides them: the last
        of a set of options that answer the same question is the one that
        counts, and the questions are kept apart (what to show, what order,
        which time, how to spell a name, what to follow).

        Times are UTC. Nothing in this tree reads /usr/share/zoneinfo, and a
        listing that quietly used the wrong zone would be worse than one that
        says which zone it used.
*/
#define LS_MAX_ENTRIES 8192
#define LS_ARENA (1 << 20)
#define LS_PATTERNS 64
#define LS_LISTED 4096

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
        b64 accessed;
        p32 accessed_fraction;
        b64 changed;
        p32 changed_fraction;
        b64 created;
        p32 created_fraction;
        p64 inode;
        p64 blocks;
        p32 rdev_major;
        p32 rdev_minor;
        bool known;
        bool created_known;
        bool points_at_directory;
        bool quoted;
} ls_entry;

static ls_entry ls_entries[LS_MAX_ENTRIES];
static positive ls_sorted[LS_MAX_ENTRIES];
static positive ls_sort_spare[LS_MAX_ENTRIES];
static positive ls_count;
static p8 ls_arena[LS_ARENA];
static positive ls_used;

// What was asked for, one letter per question.
static p8 ls_format;      // l long, 1 one per line, C down columns, x across, m commas
static p8 ls_sorting;     // n name, t time, S size, v version, X extension, w width, U none
static p8 ls_time_key;    // m modified, a accessed, c changed, b born
static p8 ls_time_style;  // d default, f full-iso, l long-iso, i iso, + a format of its own
static string_address ls_time_format_old;
static string_address ls_time_format_recent;
static p8 ls_quoting;     // L literal, s shell, S shell-always, e shell-escape,
                          // E shell-escape-always, c C, b escape, o locale
static bool ls_hide_controls;
static p8 ls_indicator;   // 0 none, / slash, f file-type, F classify
static p8 ls_dereference; // N never, D command-line links to directories, H command line, L always
static bool ls_hidden;
static bool ls_almost;
static bool ls_recursive;
static bool ls_reversed;
static bool ls_inode;
static bool ls_blocks;
static bool ls_numeric;
static bool ls_as_itself;
static bool ls_group_directories;
static bool ls_ignore_backups;
static bool ls_kibibytes;
static bool ls_owner_shown;
static bool ls_group_shown;
static bool ls_author;
static bool ls_context;
static bool ls_dired;
static bool ls_hyperlink;
static p8 ls_eol;
static positive ls_width;
static positive ls_tabsize;

// Sizes in the long listing, and the blocks -s and the total count in: each
// is a unit to divide by with a suffix to write after, or the human spelling.
static positive ls_size_unit;
static bool ls_size_human;
static bool ls_size_si;
static p8 ls_size_suffix[8];
static positive ls_block_unit;
static bool ls_block_human;
static bool ls_block_si;
static p8 ls_block_suffix[8];

static string_address ls_ignore_patterns[LS_PATTERNS];
static positive ls_ignore_count;
static string_address ls_hide_patterns[LS_PATTERNS];
static positive ls_hide_count;

static bool ls_some_quoted;
static bool ls_coloring;
static bool ls_color_started;
static bool ls_terminal;
static string_address ls_colors;

static b32 ls_status;
static bool ls_written;
static bool ls_broken;
static b64 ls_now;
static string_address ls_program;

// --dired needs to know where every name landed in the output, so every
// byte the listing writes goes through one counter.
static positive ls_out_bytes;
static positive ls_dired_marks[2 * LS_MAX_ENTRIES];
static positive ls_dired_count;
static positive ls_subdired_marks[2 * LS_LISTED];
static positive ls_subdired_count;

// The directories -R has listed, by identity, so a link back into one is
// named as already listed rather than walked again.
static p64 ls_listed_device[LS_LISTED];
static p64 ls_listed_inode[LS_LISTED];
static positive ls_listed_count;

static p8 ls_host[FILE_NAME_MAX];
static p8 ls_cwd[FILE_PATH_MAX];

static p8 ls_format_option;
static p8 ls_sort_option;
static p8 ls_time_option;
static p8 ls_quote_option;
static p8 ls_indicator_option;
static p8 ls_hidden_option;
static p8 ls_deref_option;
static p8 ls_size_option;
static p8 ls_control_option;
//      --zero read while --format=WORD stood: whether the word was the long
//      one is not known until the word is read, so the question waits.
static bool ls_zero_after_word;

static const file_supersede ls_supersedes[] = {
    {(string_address) "CxmlgonJMD", address_of ls_format_option},
    {(string_address) "tSUvX3f", address_of ls_sort_option},
    {(string_address) "cu4", address_of ls_time_option},
    //      --zero says how a name is spelled and whether control bytes are
    //      shown, so it stands in those two rows and a later -Q or -q takes
    //      it back, which is what the reference does.
    {(string_address) "NQbz6", address_of ls_quote_option},
    {(string_address) "FpjEY", address_of ls_indicator_option},
    {(string_address) "aAf", address_of ls_hidden_option},
    {(string_address) "HLV", address_of ls_deref_option},
    {(string_address) "hP7", address_of ls_size_option},
    {(string_address) "q26", address_of ls_control_option},
    {null, null},
};

static bool date_shape(writer write, b64 when, string_address format);
bool shell_match(string_address pattern, string_address text);

static fn ls_out(address_any text, positive length)
{
        if (!length)
                length = string_length((string_address)text);

        log(text, length);
        ls_out_bytes += length;
}

static positive ls_counted;

static fn ls_count_bytes(address_any text, positive length)
{
        ls_counted += length ? length : string_length((string_address)text);
}

static fn ls_limit(string_address why)
{
        if (!ls_broken)
        {
                log_error(ls_program, 0);
                log_error(": ", 2);
                log_error(why, 0);
                log_error("\n", 1);
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

// ---- Names as the reference spells them --------------------------------

/*
        The bytes a shell would have to quote: the empty name, whitespace and
        control bytes, the shell's own punctuation, a hash or tilde in front,
        and a brace standing alone. Under the escaping styles a byte outside
        ASCII is spelled in octal and so needs quoting too.
*/
static bool ls_shell_needs_quotes(string_address name, positive length, bool escaping)
{
        if (!length)
                return true;

        for (positive at = 0; at < length; at++)
        {
                p8 byte = string_get(name + at);

                if (byte < 32 || byte == 127)
                        return true;
                if (byte >= 128)
                {
                        if (escaping)
                                return true;
                        continue;
                }
                if (string_first_of((string_address) " !\"$&'()*;<=>?[^`|\\", byte))
                        return true;
                if ((byte == '#' || byte == '~') && at == 0)
                        return true;
                if ((byte == '{' || byte == '}') && length == 1)
                        return true;
        }

        return false;
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

static positive ls_escape_letter(p8 byte, p8 address_to into)
{
        return ls_escape_byte(byte, into, true);
}

static bool ls_byte_unprintable(p8 byte)
{
        return byte < 32 || byte >= 127;
}

// The shell styles: quotes only when needed or always, and the escaping
// variants that spell an unprintable byte as $'\ooo' between quoted runs.
static fn ls_quote_shell(writer write, string_address name, positive length, bool always,
                         bool escaping)
{
        if (!always && !ls_shell_needs_quotes(name, length, escaping))
        {
                write(name, length);
                return;
        }

        bool quote_inside = false;
        bool double_unsafe = false;
        bool unprintable = false;

        for (positive at = 0; at < length; at++)
        {
                p8 byte = string_get(name + at);

                if (byte == '\'')
                        quote_inside = true;
                if (byte == '"' || byte == '$' || byte == '`' || byte == '\\' || byte == '!')
                        double_unsafe = true;
                if (ls_byte_unprintable(byte))
                        unprintable = true;
        }

        if (quote_inside && !double_unsafe && !(escaping && unprintable))
        {
                write("\"", 1);
                write(name, length);
                write("\"", 1);
                return;
        }

        write("'", 1);

        bool open = true;

        for (positive at = 0; at < length; at++)
        {
                p8 byte = string_get(name + at);

                if (byte == '\'')
                {
                        if (!open)
                        {
                                write("'", 1);
                                open = true;
                        }
                        write("'\\''", 4);
                        continue;
                }

                if (escaping && ls_byte_unprintable(byte))
                {
                        if (open)
                                write("'", 1);
                        write("$'", 2);

                        while (at < length && ls_byte_unprintable(string_get(name + at)))
                        {
                                p8 spelled[4];

                                write(spelled, ls_escape_letter(string_get(name + at), spelled));
                                at++;
                        }
                        at--;
                        write("'", 1);
                        open = false;
                        continue;
                }

                if (!open)
                {
                        write("'", 1);
                        open = true;
                }
                write(name + at, 1);
        }

        if (open)
                write("'", 1);
}

// The C styles: a quoted string a C compiler would read back, the same
// without its quotes and with spaces escaped, and the locale style that in
// the C locale is the C string in single quotes.
static fn ls_quote_c(writer write, string_address name, positive length, p8 style)
{
        p8 quote = style == 'c' ? '"' : style == 'o' ? '\'' : 0;

        if (quote)
                write(address_of quote, 1);

        for (positive at = 0; at < length; at++)
        {
                p8 byte = string_get(name + at);
                p8 spelled[4];

                if (byte == '\\')
                        write("\\\\", 2);
                else if (quote && byte == quote)
                {
                        write("\\", 1);
                        write(address_of quote, 1);
                }
                else if (style == 'b' && byte == ' ')
                        write("\\ ", 2);
                else if (ls_byte_unprintable(byte))
                        write(spelled, ls_escape_letter(byte, spelled));
                else
                        write(name + at, 1);
        }

        if (quote)
                write(address_of quote, 1);
}

/*
        A name inside a diagnostic's quotes.

        The reference does not write a name into a message as it stands: a
        byte that is not printable, a backslash and the quote itself are all
        spelled out, so that one line stays one line whatever the name holds.
        mkdir writes the locale style, which in the C locale is a C string
        between single quotes, and that is what this renders -- the body of
        it, because the quotes are already in every format string that asks.

        A name with nothing to escape is handed back as it came, so a name
        longer than this buffer is still written whole.
*/
#define FILE_SHOWN_MAX (FILE_PATH_MAX * 2)

static p8 file_shown_store[FILE_SHOWN_MAX];

static string_address file_shown_c(string_address name)
{
        positive length = string_length(name);
        positive plain = 0;

        while (plain < length)
        {
                p8 byte = string_get(name + plain);

                if (byte == '\\' || byte == '\'' || ls_byte_unprintable(byte))
                        break;

                plain++;
        }

        if (plain == length)
                return name;

        positive used = 0;

        for (positive at = 0; at < length && used < FILE_SHOWN_MAX - 5; at++)
        {
                p8 byte = string_get(name + at);

                if (byte == '\\' || byte == '\'')
                {
                        file_shown_store[used++] = '\\';
                        file_shown_store[used++] = byte;
                }
                else if (ls_byte_unprintable(byte))
                {
                        p8 spelled[4];
                        positive wide = ls_escape_letter(byte, spelled);

                        for (positive i = 0; i < wide; i++)
                                file_shown_store[used++] = spelled[i];
                }
                else
                        file_shown_store[used++] = byte;
        }

        file_shown_store[used] = end;

        return file_shown_store;
}

static fn ls_quote_literal(writer write, string_address name, positive length)
{
        if (!ls_hide_controls)
        {
                write(name, length);
                return;
        }

        for (positive at = 0; at < length; at++)
        {
                p8 byte = string_get(name + at);

                if (ls_byte_unprintable(byte))
                        write("?", 1);
                else
                        write(name + at, 1);
        }
}

static fn ls_quote(writer write, string_address name)
{
        positive length = string_length(name);

        switch (ls_quoting)
        {
        case 's':
                return ls_quote_shell(write, name, length, false, false);
        case 'S':
                return ls_quote_shell(write, name, length, true, false);
        case 'e':
                return ls_quote_shell(write, name, length, false, true);
        case 'E':
                return ls_quote_shell(write, name, length, true, true);
        case 'c':
        case 'b':
        case 'o':
                return ls_quote_c(write, name, length, ls_quoting);
        }

        ls_quote_literal(write, name, length);
}

// Whether the reference would put this name in quotes under a style that
// quotes only when it must; other names in the same listing then get a
// space in front so the columns still line up.
static bool ls_name_quoted(string_address name)
{
        if (ls_quoting != 's' && ls_quoting != 'e')
                return false;

        return ls_shell_needs_quotes(name, string_length(name), ls_quoting == 'e');
}

static bool ls_aligns_quotes()
{
        return (ls_format == 'l' || ((ls_format == 'C' || ls_format == 'x') && ls_width)) &&
               (ls_quoting == 's' || ls_quoting == 'e');
}

static positive ls_quoted_width(ls_entry address_to entry)
{
        ls_counted = 0;
        ls_quote(ls_count_bytes, ls_arena + entry->name);

        if (ls_aligns_quotes() && ls_some_quoted && !entry->quoted)
                ls_counted++;

        return ls_counted;
}

// ---- Sizes and blocks ----------------------------------------------------

// The reference's power of ten spelling: below a thousand the number, then
// the greatest unit not exceeding it, one decimal below ten, and every
// rounding upward.
static fn ls_human_1000(writer write, p64 value)
{
        static const p8 units[] = "kMGTPEZY";
        positive unit = 0;
        p64 divisor = 1;

        if (value < 1000)
                return positive_to_string(write, value);

        while (unit + 1 < sizeof(units) - 1 && value / divisor >= 1000000)
        {
                divisor *= 1000;
                unit++;
        }

        divisor *= 1000;

        p64 whole = value / divisor;
        p64 rest = value % divisor;
        p8 text[16];
        positive length;

        if (whole < 10)
        {
                p64 tenths = (rest * 10 + divisor - 1) / divisor;

                if (tenths == 10)
                {
                        whole++;
                        tenths = 0;
                }

                if (whole < 10)
                {
                        length = positive_into_string(text, whole);
                        text[length++] = '.';
                        text[length++] = (p8)('0' + tenths);
                        text[length++] = units[unit];
                        return write(text, length);
                }
        }
        else
                whole += rest != 0;

        if (whole >= 1000 && unit + 1 < sizeof(units) - 1)
        {
                p64 next = whole / 1000 + (whole % 1000 != 0);

                length = positive_into_string(text, next);
                text[length++] = units[unit + 1];
                return write(text, length);
        }

        length = positive_into_string(text, whole);
        text[length++] = units[unit];
        write(text, length);
}

static fn ls_scaled(writer write, p64 value, positive unit, bool human, bool si,
                    string_address suffix)
{
        if (human)
                return si ? ls_human_1000(write, value) : positive_to_human_1024(write, value);

        positive_to_string(write, unit > 1 ? value / unit + (value % unit != 0) : value);
        write(suffix, string_length(suffix));
}

static positive ls_scaled_width(p64 value, positive unit, bool human, bool si,
                                string_address suffix)
{
        ls_counted = 0;
        ls_scaled(ls_count_bytes, value, unit, human, si, suffix);
        return ls_counted;
}

/*
        A --block-size argument: a number, a unit letter, or both, with B for
        powers of a thousand and iB or nothing for powers of 1024; a leading
        apostrophe asks for digit grouping, which the C locale has none of.
        The suffix written after each number is the unit as it was spelled.
*/
static bool ls_block_size_read(string_address text, positive address_to unit,
                               bool address_to human, bool address_to si,
                               p8 address_to suffix)
{
        static const p8 letters[] = "KMGTPEZYRQ";
        string_address at = text;
        positive number = 1;
        bool numeric = false;

        address_to human = false;
        address_to si = false;
        suffix[0] = end;

        if (string_is(at, '\''))
                at++;

        if (byte_is_digit(string_get(at)))
        {
                if (!string_digits_checked(address_of at, 10, address_of number) || !number)
                        return false;
                numeric = true;
        }

        if (!string_get(at))
        {
                if (!numeric)
                        return false;
                address_to unit = number;
                return true;
        }

        string_address letter = string_first_of((string_address)letters, string_get(at));

        if (!letter || string_get(at) == end)
                return false;

        positive power = (positive)(letter - (string_address)letters) + 1;
        positive base = 1024;
        positive suffix_length = 0;

        suffix[suffix_length++] = string_get(at);
        at++;

        if (string_is(at, 'B'))
        {
                base = 1000;
                suffix[0] = suffix[0] == 'K' ? 'k' : suffix[0];
                suffix[suffix_length++] = 'B';
                at++;
        }
        else if (string_is(at, 'i') && string_is(at + 1, 'B'))
        {
                suffix[suffix_length++] = 'i';
                suffix[suffix_length++] = 'B';
                at += 2;
        }

        if (string_get(at))
                return false;

        suffix[suffix_length] = end;

        positive scale = 1;

        for (positive i = 0; i < power; i++)
        {
                if (scale > positive_max / base)
                        return false;
                scale *= base;
        }

        address_to unit = number * scale;
        return true;
}

// ---- Order ---------------------------------------------------------------

// gnulib's filevercmp: a file suffix is set aside first, digit runs are
// compared as numbers, a tilde sorts before everything and punctuation
// after letters, so b.txt~ comes before b.txt and v2 before v10.
static b32 ls_version_order_byte(p8 byte)
{
        if (byte_is_digit(byte))
                return 0;
        if (byte_is_alpha(byte))
                return byte;
        if (byte == '~')
                return -1;
        return byte + 256;
}

static b32 ls_version_run(string_address left, positive left_length, string_address right,
                          positive right_length)
{
        positive l = 0;
        positive r = 0;

        while (l < left_length || r < right_length)
        {
                b32 first_difference = 0;

                while ((l < left_length && !byte_is_digit(string_get(left + l))) ||
                       (r < right_length && !byte_is_digit(string_get(right + r))))
                {
                        b32 lc = l == left_length ? 0 : ls_version_order_byte(string_get(left + l));
                        b32 rc = r == right_length ? 0 : ls_version_order_byte(string_get(right + r));

                        if (lc != rc)
                                return lc - rc;
                        l++;
                        r++;
                }

                while (l < left_length && string_is(left + l, '0'))
                        l++;
                while (r < right_length && string_is(right + r, '0'))
                        r++;

                while (l < left_length && byte_is_digit(string_get(left + l)) &&
                       r < right_length && byte_is_digit(string_get(right + r)))
                {
                        if (!first_difference)
                                first_difference = (b32)string_get(left + l) -
                                                   (b32)string_get(right + r);
                        l++;
                        r++;
                }

                if (l < left_length && byte_is_digit(string_get(left + l)))
                        return 1;
                if (r < right_length && byte_is_digit(string_get(right + r)))
                        return -1;
                if (first_difference)
                        return first_difference;
        }

        return 0;
}

static positive ls_version_prefix(string_address name, positive length)
{
        positive prefix = 0;

        for (positive i = 0;;)
        {
                if (i == length)
                        return prefix;
                i++;
                prefix = i;
                while (i + 1 < length && string_is(name + i, '.') &&
                       (byte_is_alpha(string_get(name + i + 1)) || string_is(name + i + 1, '~')))
                        for (i += 2; i < length && (byte_is_alnum(string_get(name + i)) ||
                                                    string_is(name + i, '~'));
                             i++)
                                ;
        }
}

static b32 ls_version_compare(string_address left, string_address right)
{
        positive left_length = string_length(left);
        positive right_length = string_length(right);

        if (!left_length)
                return right_length ? -1 : 0;
        if (!right_length)
                return 1;

        if (string_is(left, '.'))
        {
                if (!string_is(right, '.'))
                        return -1;

                bool left_dot = left_length == 1;
                bool right_dot = right_length == 1;

                if (left_dot)
                        return right_dot ? 0 : -1;
                if (right_dot)
                        return 1;

                bool left_dots = string_is(left + 1, '.') && left_length == 2;
                bool right_dots = string_is(right + 1, '.') && right_length == 2;

                if (left_dots)
                        return right_dots ? 0 : -1;
                if (right_dots)
                        return 1;
        }
        else if (string_is(right, '.'))
                return 1;

        positive left_prefix = ls_version_prefix(left, left_length);
        positive right_prefix = ls_version_prefix(right, right_length);
        bool one_pass = left_prefix == left_length && right_prefix == right_length;
        b32 answer = ls_version_run(left, left_prefix, right, right_prefix);

        if (answer || one_pass)
                return answer;

        return ls_version_run(left, left_length, right, right_length);
}

static string_address ls_extension(string_address name)
{
        string_address dot = string_last_of(name, '.');

        return dot ? dot : (string_address) "";
}

static fn ls_entry_time(ls_entry address_to entry, b64 address_to seconds,
                        p32 address_to fraction)
{
        switch (ls_time_key)
        {
        case 'a':
                address_to seconds = entry->accessed;
                address_to fraction = entry->accessed_fraction;
                return;
        case 'c':
                address_to seconds = entry->changed;
                address_to fraction = entry->changed_fraction;
                return;
        case 'b':
                address_to seconds = entry->created;
                address_to fraction = entry->created_fraction;
                return;
        }

        address_to seconds = entry->modified;
        address_to fraction = entry->modified_fraction;
}

static bool ls_is_directory_like(ls_entry address_to entry)
{
        return (entry->mode & MODE_FORMAT) == MODE_DIRECTORY || entry->points_at_directory;
}

static PURE HOT bipolar ls_order(ls_entry address_to left, ls_entry address_to right)
{
        if (ls_group_directories && ls_sorting != 'U')
        {
                bool left_directory = ls_is_directory_like(left);
                bool right_directory = ls_is_directory_like(right);

                if (left_directory != right_directory)
                        return left_directory ? -1 : 1;
        }

        string_address left_name = ls_arena + left->name;
        string_address right_name = ls_arena + right->name;
        bipolar answer = 0;

        switch (ls_sorting)
        {
        case 't':
        {
                b64 left_seconds, right_seconds;
                p32 left_fraction, right_fraction;

                ls_entry_time(left, address_of left_seconds, address_of left_fraction);
                ls_entry_time(right, address_of right_seconds, address_of right_fraction);

                if (left_seconds != right_seconds)
                        answer = left_seconds > right_seconds ? -1 : 1;
                else if (left_fraction != right_fraction)
                        answer = left_fraction > right_fraction ? -1 : 1;
                break;
        }
        case 'S':
                if (left->size != right->size)
                        answer = left->size > right->size ? -1 : 1;
                break;
        case 'v':
                answer = ls_version_compare(left_name, right_name);
                break;
        case 'X':
                answer = string_compare(ls_extension(left_name), ls_extension(right_name));
                break;
        case 'w':
        {
                positive left_width = ls_quoted_width(left);
                positive right_width = ls_quoted_width(right);

                if (left_width != right_width)
                        answer = left_width < right_width ? -1 : 1;
                break;
        }
        }

        if (!answer)
                answer = string_compare(left_name, right_name);

        return ls_reversed ? -answer : answer;
}

#define ls_index_order(left, right) \
        ls_order(ls_entries + (left), ls_entries + (right))

/* Bottom-up merge sort keeps comparison count at n log n on the full 8192
   entry surface. Only eight-byte indexes move. -U leaves the directory's
   own order, reversed by -r as the reference reverses it. */
static fn ls_sort()
{
        for (positive i = 0; i < ls_count; i++)
                ls_sorted[i] = i;

        if (ls_count < 2)
                return;

        if (ls_sorting == 'U')
        {
                if (ls_reversed)
                        for (positive i = 0; i < ls_count; i++)
                                ls_sorted[i] = ls_count - 1 - i;
                return;
        }

        positive address_to from = array_merge_sort(
            ls_sorted, ls_sort_spare, ls_count, ls_index_order);

        if (from != ls_sorted)
                memory_copy_apart(ls_sorted, from,
                                  ls_count * sizeof(positive));
}

// A character or block device has no size worth a column; the reference ls
// prints its major and minor numbers there instead.
static bool ls_is_device(ls_entry address_to entry)
{
        positive kind = entry->mode & MODE_FORMAT;

        return entry->known && (kind == MODE_CHARACTER || kind == MODE_BLOCK);
}

// The letter after a name that says what it is: a slash for -p, the kinds
// for --file-type, and the executable's star only for -F.
static p8 ls_mark(positive mode)
{
        p8 mark = file_kind_of(mode)->mark;

        if (!ls_indicator)
                return 0;

        if (ls_indicator == '/')
                return mark == '/' ? mark : 0;

        if (ls_indicator == 'F' && (mode & MODE_FORMAT) == MODE_FILE && (mode & 0111))
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

static bool ls_full_path(p8 address_to full, string_address directory, string_address name)
{
        if (directory)
                return file_path_join(full, directory, name);

        string_copy_max_end(full, name, FILE_PATH_MAX - 1);
        return true;
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

                // A link whose path would not fit whole cannot be followed,
                // and is coloured as the orphan it might as well be.
                if (!ls_full_path(full, directory, name) || !file_look_at(full, address_of through))
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

// ---- Hyperlinks ----------------------------------------------------------

static fn ls_url_bytes(string_address text)
{
        for (positive at = 0; string_get(text + at); at++)
        {
                p8 byte = string_get(text + at);

                if (byte_is_alnum(byte) || string_first_of((string_address) "-._~/", byte))
                {
                        ls_out(text + at, 1);
                        continue;
                }

                p8 escaped[3] = {'%', (p8)"0123456789ABCDEF"[byte >> 4],
                                 (p8)"0123456789ABCDEF"[byte & 15]};

                ls_out(escaped, 3);
        }
}

// The file: URL a terminal turns into a link: this host, the absolute path,
// every byte outside the unreserved set spelled in percent form.
static fn ls_hyperlink_open(string_address directory, string_address name)
{
        p8 full[FILE_PATH_MAX];

        ls_out("\033]8;;file://", 0);
        ls_out(ls_host, 0);

        if (!ls_full_path(full, directory, name))
                string_copy_max_end(full, name, FILE_PATH_MAX - 1);

        if (!string_is(full, '/'))
        {
                ls_url_bytes(ls_cwd);
                if (!(string_is(ls_cwd, '/') && !string_get(ls_cwd + 1)))
                        ls_out("/", 1);
        }

        ls_url_bytes(full);
        ls_out("\033\\", 2);
}

static fn ls_hyperlink_close()
{
        ls_out("\033]8;;\033\\", 0);
}

// ---- Writing a name ------------------------------------------------------

static fn ls_dired_mark(positive begin, positive stop)
{
        if (!ls_dired)
                return;

        if (ls_dired_count + 2 > array_count(ls_dired_marks))
        {
                ls_limit((string_address) "too many names for --dired");
                return;
        }

        ls_dired_marks[ls_dired_count++] = begin;
        ls_dired_marks[ls_dired_count++] = stop;
}

/*
        One name, coloured, quoted and linked as asked. start_column is where
        on the line it begins: a coloured name that crosses the width of the
        line is followed by the terminal's erase-to-end, as the reference
        writes it, so a background colour does not bleed into the wrap.
*/
static fn ls_name_say(string_address directory, ls_entry address_to entry,
                      string_address name, positive start_column)
{
        file_color_span color = {null, 0};
        file_color_span reset = {null, 0};

        if (ls_aligns_quotes() && ls_some_quoted && !entry->quoted)
                ls_out(" ", 1);

        if (ls_coloring)
        {
                color = ls_name_color(directory, entry, name);

                if (color.text && !color.length)
                        color.text = null;
        }

        if (color.text)
        {
                reset = ls_color_of(LS_COLOR_RS, (string_address) "0");

                if (!ls_color_started)
                {
                        file_color_sgr(ls_out, reset);
                        ls_color_started = true;
                }

                file_color_sgr(ls_out, color);
        }

        if (ls_hyperlink)
                ls_hyperlink_open(directory, name);

        positive begin = ls_out_bytes;

        ls_quote(ls_out, name);
        ls_dired_mark(begin, ls_out_bytes);

        if (ls_hyperlink)
                ls_hyperlink_close();

        if (color.text)
        {
                file_color_sgr(ls_out, reset);

                positive width = ls_out_bytes - begin;

                if (ls_width && width &&
                    start_column / ls_width != (start_column + width - 1) / ls_width)
                        ls_out("\033[K", 3);
        }
}

// ---- Times ---------------------------------------------------------------

static bool ls_recent(b64 seconds)
{
        return seconds <= ls_now + 3600 && seconds > ls_now - 15778476;
}

static fn ls_time_say(ls_entry address_to entry)
{
        b64 seconds;
        p32 fraction;

        ls_entry_time(entry, address_of seconds, address_of fraction);

        switch (ls_time_style)
        {
        case 'f':
                return file_stamp(ls_out, seconds, fraction);
        case 'l':
        {
                b64 year;
                positive month, day, hour, minute, second;

                file_split_moment(seconds, address_of year, address_of month, address_of day,
                                  address_of hour, address_of minute, address_of second);
                positive_to_string(ls_out, (positive)year);
                ls_out("-", 1);
                file_two(ls_out, month);
                ls_out("-", 1);
                file_two(ls_out, day);
                ls_out(" ", 1);
                file_two(ls_out, hour);
                ls_out(":", 1);
                file_two(ls_out, minute);
                return;
        }
        case 'i':
        {
                b64 year;
                positive month, day, hour, minute, second;

                file_split_moment(seconds, address_of year, address_of month, address_of day,
                                  address_of hour, address_of minute, address_of second);

                if (ls_recent(seconds))
                {
                        file_two(ls_out, month);
                        ls_out("-", 1);
                        file_two(ls_out, day);
                        ls_out(" ", 1);
                        file_two(ls_out, hour);
                        ls_out(":", 1);
                        file_two(ls_out, minute);
                        return;
                }

                positive_to_string(ls_out, (positive)year);
                ls_out("-", 1);
                file_two(ls_out, month);
                ls_out("-", 1);
                file_two(ls_out, day);
                ls_out(" ", 1);
                return;
        }
        case '+':
                date_shape(ls_out, seconds,
                           ls_recent(seconds) ? ls_time_format_recent : ls_time_format_old);
                return;
        }

        file_stamp_short(ls_out, seconds, ls_now);
}

// ---- The listing ---------------------------------------------------------

static positive ls_indent(positive from, positive to)
{
        while (from < to)
        {
                if (ls_tabsize && to / ls_tabsize > (from + 1) / ls_tabsize)
                {
                        ls_out("\t", 1);
                        from += ls_tabsize - from % ls_tabsize;
                }
                else
                {
                        ls_out(" ", 1);
                        from++;
                }
        }

        return to;
}

static positive ls_inode_width;
static positive ls_block_width;

// What a name takes up in a column, frills included: the inode and block
// columns in front, the quoted name, and the mark after it.
static positive ls_frilled_width(ls_entry address_to entry)
{
        positive width = ls_quoted_width(entry);

        if (ls_inode)
                width += 1 + (ls_format == 'm' ? positive_digits(entry->inode) : ls_inode_width);
        if (ls_blocks)
                width += 1 + (ls_format == 'm'
                                  ? ls_scaled_width(entry->blocks * 512, ls_block_unit,
                                                    ls_block_human, ls_block_si, ls_block_suffix)
                                  : ls_block_width);
        if (ls_context)
                width += 2;
        if (ls_indicator && (entry->known || (entry->mode & MODE_FORMAT)) && ls_mark(entry->mode))
                width++;

        return width;
}

static fn ls_frills_before(ls_entry address_to entry)
{
        if (ls_inode)
        {
                if (entry->known)
                        positive_to_padded(ls_out, entry->inode,
                                           ls_format == 'm' ? 0 : ls_inode_width, ' ', 0);
                else
                        string_to_field(ls_out, (string_address) "?",
                                        ls_format == 'm' ? 1 : ls_inode_width, ' ', false);
                ls_out(" ", 1);
        }

        if (ls_blocks)
        {
                positive width = ls_format == 'm' ? 0 : ls_block_width;

                if (entry->known)
                {
                        positive have = ls_scaled_width(entry->blocks * 512, ls_block_unit,
                                                        ls_block_human, ls_block_si,
                                                        ls_block_suffix);

                        writer_fill(ls_out, width > have ? width - have : 0, ' ');
                        ls_scaled(ls_out, entry->blocks * 512, ls_block_unit, ls_block_human,
                                  ls_block_si, ls_block_suffix);
                }
                else
                        string_to_field(ls_out, (string_address) "?", width ? width : 1, ' ', false);
                ls_out(" ", 1);
        }

        if (ls_context && ls_format != 'l')
                ls_out("? ", 2);
}

static fn ls_mark_after(string_address directory, ls_entry address_to entry, string_address name)
{
        bool link = entry->known && (entry->mode & MODE_FORMAT) == MODE_LINK;
        p8 mark = ls_indicator && (entry->known || (entry->mode & MODE_FORMAT))
                      ? ls_mark(entry->mode)
                      : 0;

        // In the long form the arrow is written and the mark goes on what the
        // link points at; on a line of its own the link is the only thing
        // there is to mark.
        if (mark && !(ls_format == 'l' && link))
                ls_out(address_of mark, 1);

        if (ls_format == 'l' && link)
        {
                p8 where[FILE_PATH_MAX];
                p8 full[FILE_PATH_MAX];

                if (!ls_full_path(full, directory, name))
                {
                        string_format(log_error, "%s: cannot read symbolic link '%s/%s': %s\n",
                                      ls_program, directory, name,
                                      file_reason(-ERROR_NAME_TOO_LONG));
                        ls_status = 1;
                }
                else if (file_link_text(full, where, FILE_PATH_MAX) >= 0)
                {
                        file_facts through;

                        ls_out(" -> ", 4);
                        ls_quote(ls_out, (string_address)where);

                        if (ls_indicator && ls_indicator != '/' &&
                            file_look_at(full, address_of through))
                        {
                                p8 there = ls_mark(through.mode);

                                if (there)
                                        ls_out(address_of there, 1);
                        }
                }
        }
}

static fn ls_print_total()
{
        p64 blocks = 0;

        for (positive i = 0; i < ls_count; i++)
                blocks += ls_entries[i].blocks;

        if (ls_dired)
                ls_out("  ", 2);

        ls_out("total ", 6);
        ls_scaled(ls_out, blocks * 512, ls_block_unit, ls_block_human, ls_block_si,
                  ls_block_suffix);
        ls_out(address_of ls_eol, 1);
}

static fn ls_print_long(string_address directory)
{
        positive link_width = 1;
        positive size_width = 1;
        positive owner_width = 1;
        positive group_width = 1;
        positive major_width = 0;
        positive minor_width = 0;

        // An entry the kernel would not describe is a "?" in every column,
        // which is one character wide and so counts for nothing here.
        for (positive i = 0; i < ls_count; i++)
        {
                ls_entry address_to entry = address_of ls_entries[i];

                if (!entry->known)
                        continue;

                link_width = max(link_width, positive_digits(entry->links));

                if (ls_is_device(entry))
                {
                        major_width = max(major_width, positive_digits(entry->rdev_major));
                        minor_width = max(minor_width, positive_digits(entry->rdev_minor));
                }
                else
                        size_width = max(size_width,
                                         ls_scaled_width(entry->size, ls_size_unit, ls_size_human,
                                                         ls_size_si, ls_size_suffix));

                p8 name[FILE_NAME_MAX];

                file_account_label(entry->owner, false, !ls_numeric, name);
                owner_width = max(owner_width, string_length(name));

                file_account_label(entry->group, true, !ls_numeric, name);
                group_width = max(group_width, string_length(name));
        }

        // The device column is "major, minor", and the size column is wide
        // enough for whichever of the two spellings is the wider.
        positive device_width = major_width + 2 + minor_width;

        if (major_width && device_width > size_width)
                size_width = device_width;

        for (positive k = 0; k < ls_count; k++)
        {
                ls_entry address_to entry = address_of ls_entries[ls_sorted[k]];
                string_address name = ls_arena + entry->name;
                positive line_start = ls_out_bytes;

                if (ls_dired)
                        ls_out("  ", 2);

                ls_frills_before(entry);

                p8 letters[12];
                p8 who[FILE_NAME_MAX];

                if (!entry->known)
                {
                        // What the reference ls prints for an entry it could
                        // not ask about: the kind the directory gave, a
                        // question mark for every bit and every column, and
                        // the time column held at its width.
                        letters[0] = (entry->mode & MODE_FORMAT)
                                         ? file_kind_letter(entry->mode)
                                         : '?';
                        memory_fill(letters + 1, '?', 9);
                        ls_out(letters, 10);
                        ls_out(" ", 1);
                        string_to_field(ls_out, (string_address) "?", link_width, ' ', false);
                        ls_out(" ", 1);
                        if (ls_owner_shown)
                        {
                                string_to_field(ls_out, (string_address) "?", owner_width, ' ', true);
                                ls_out(" ", 1);
                        }
                        if (ls_group_shown)
                        {
                                string_to_field(ls_out, (string_address) "?", group_width, ' ', true);
                                ls_out(" ", 1);
                        }
                        if (ls_author)
                        {
                                string_to_field(ls_out, (string_address) "?", owner_width, ' ', true);
                                ls_out(" ", 1);
                        }
                        if (ls_context)
                                ls_out("? ", 2);
                        string_to_field(ls_out, (string_address) "?", size_width, ' ', false);
                        ls_out(" ", 1);
                        string_to_field(ls_out, (string_address) "?", 12, ' ', false);
                        ls_out(" ", 1);
                }
                else
                {
                        file_mode_letters(letters, entry->mode);
                        ls_out(letters, 10);
                        ls_out(" ", 1);
                        positive_to_padded(ls_out, entry->links, link_width, ' ', 0);
                        ls_out(" ", 1);

                        if (ls_owner_shown)
                        {
                                file_account_label(entry->owner, false, !ls_numeric, who);
                                string_to_field(ls_out, who, owner_width, ' ', true);
                                ls_out(" ", 1);
                        }

                        if (ls_group_shown)
                        {
                                file_account_label(entry->group, true, !ls_numeric, who);
                                string_to_field(ls_out, who, group_width, ' ', true);
                                ls_out(" ", 1);
                        }

                        if (ls_author)
                        {
                                file_account_label(entry->owner, false, !ls_numeric, who);
                                string_to_field(ls_out, who, owner_width, ' ', true);
                                ls_out(" ", 1);
                        }

                        if (ls_context)
                                ls_out("? ", 2);

                        if (ls_is_device(entry))
                        {
                                // The major number takes whatever the size
                                // column has over the device spelling, so
                                // the comma lines up down the listing.
                                positive_to_padded(ls_out, entry->rdev_major,
                                                   major_width + size_width - device_width,
                                                   ' ', 0);
                                ls_out(", ", 2);
                                positive_to_padded(ls_out, entry->rdev_minor, minor_width,
                                                   ' ', 0);
                        }
                        else
                        {
                                positive have = ls_scaled_width(entry->size, ls_size_unit,
                                                                ls_size_human, ls_size_si,
                                                                ls_size_suffix);

                                writer_fill(ls_out, size_width > have ? size_width - have : 0, ' ');
                                ls_scaled(ls_out, entry->size, ls_size_unit, ls_size_human,
                                          ls_size_si, ls_size_suffix);
                        }

                        ls_out(" ", 1);
                        ls_time_say(entry);
                        ls_out(" ", 1);
                }

                ls_name_say(directory, entry, name, ls_out_bytes - line_start);
                ls_mark_after(directory, entry, name);
                ls_out(address_of ls_eol, 1);
        }
}

/*
        The reference's column layout: for every possible count of columns
        the widths are grown entry by entry, a count stays possible while its
        line would fit, and the largest count still possible is the one used.
        Down the columns for -C, across the rows for -x, with tabs filling the
        gaps where they can.
*/
static positive ls_column_widths[LS_MAX_ENTRIES];

static fn ls_print_columns(string_address directory, bool across)
{
        positive width_limit = ls_width ? ls_width : positive_max;
        positive most = ls_width
            ? ls_width / 3 + (ls_width % 3 != 0) : ls_count;

        if (most > ls_count)
                most = ls_count;
        if (!most)
                most = 1;

        for (positive i = 0; i < ls_count; i++)
                ls_sort_spare[i] = ls_frilled_width(address_of ls_entries[ls_sorted[i]]);

        positive columns = 1;

        // Each candidate count is tried on its own; the widths array is
        // reused, so the search runs from the most columns downward and
        // stops at the first count whose line fits.
        for (positive candidate = most; candidate >= 1; candidate--)
        {
                positive line = candidate * 3;
                bool fits = true;

                for (positive column = 0; column < candidate; column++)
                        ls_column_widths[column] = 3;

                positive rows = (ls_count + candidate - 1) / candidate;

                for (positive index = 0; index < ls_count && fits; index++)
                {
                        positive column = across ? index % candidate : index / rows;
                        positive real = ls_sort_spare[index] + (column == candidate - 1 ? 0 : 2);

                        if (ls_column_widths[column] < real)
                        {
                                line += real - ls_column_widths[column];
                                ls_column_widths[column] = real;
                                fits = line < width_limit;
                        }
                }

                if (fits || candidate == 1)
                {
                        columns = candidate;
                        break;
                }
        }

        positive rows = ls_count / columns + (ls_count % columns != 0);

        if (across)
        {
                positive position = 0;

                for (positive index = 0; index < ls_count; index++)
                {
                        positive column = index % columns;
                        ls_entry address_to entry = address_of ls_entries[ls_sorted[index]];

                        if (column == 0)
                        {
                                if (index)
                                        ls_out(address_of ls_eol, 1);
                                position = 0;
                        }

                        ls_frills_before(entry);
                        ls_name_say(directory, entry, ls_arena + entry->name, position);
                        ls_mark_after(directory, entry, ls_arena + entry->name);

                        if (index + 1 < ls_count && column + 1 < columns)
                                position = ls_indent(position + ls_sort_spare[index],
                                                     position + ls_column_widths[column]);
                }

                if (ls_count)
                        ls_out(address_of ls_eol, 1);
                return;
        }

        for (positive row = 0; row < rows; row++)
        {
                positive position = 0;
                positive column = 0;

                for (positive index = row; index < ls_count; index += rows, column++)
                {
                        ls_entry address_to entry = address_of ls_entries[ls_sorted[index]];

                        ls_frills_before(entry);
                        ls_name_say(directory, entry, ls_arena + entry->name, position);
                        ls_mark_after(directory, entry, ls_arena + entry->name);

                        if (index + rows < ls_count)
                                position = ls_indent(position + ls_sort_spare[index],
                                                     position + ls_column_widths[column]);
                }

                ls_out(address_of ls_eol, 1);
        }
}

static fn ls_print_commas(string_address directory)
{
        positive position = 0;

        for (positive index = 0; index < ls_count; index++)
        {
                ls_entry address_to entry = address_of ls_entries[ls_sorted[index]];
                positive width = ls_frilled_width(entry);

                if (index)
                {
                        if (ls_width && position + width + 2 > ls_width)
                        {
                                ls_out(",", 1);
                                ls_out(address_of ls_eol, 1);
                                position = 0;
                        }
                        else
                        {
                                ls_out(", ", 2);
                                position += 2;
                        }
                }

                ls_frills_before(entry);
                ls_name_say(directory, entry, ls_arena + entry->name, position);
                ls_mark_after(directory, entry, ls_arena + entry->name);
                position += width;
        }

        ls_out(address_of ls_eol, 1);
}

static fn ls_print_lines(string_address directory)
{
        for (positive k = 0; k < ls_count; k++)
        {
                ls_entry address_to entry = address_of ls_entries[ls_sorted[k]];

                ls_frills_before(entry);
                ls_name_say(directory, entry, ls_arena + entry->name, 0);
                ls_mark_after(directory, entry, ls_arena + entry->name);
                ls_out(address_of ls_eol, 1);
        }
}

static fn ls_print(string_address directory)
{
        ls_some_quoted = false;
        ls_inode_width = 1;
        ls_block_width = 1;

        for (positive i = 0; i < ls_count; i++)
        {
                ls_entry address_to entry = address_of ls_entries[i];

                entry->quoted = ls_name_quoted(ls_arena + entry->name);
                ls_some_quoted |= entry->quoted;

                if (!entry->known)
                        continue;
                if (ls_inode)
                        ls_inode_width = max(ls_inode_width, positive_digits(entry->inode));
                if (ls_blocks)
                        ls_block_width = max(ls_block_width,
                                             ls_scaled_width(entry->blocks * 512, ls_block_unit,
                                                             ls_block_human, ls_block_si,
                                                             ls_block_suffix));
        }

        if (directory && (ls_format == 'l' || ls_blocks))
                ls_print_total();

        switch (ls_format)
        {
        case 'l':
                return ls_print_long(directory);
        case 'C':
                return ls_print_columns(directory, false);
        case 'x':
                return ls_print_columns(directory, true);
        case 'm':
                return ls_print_commas(directory);
        }

        ls_print_lines(directory);
}

// ---- Gathering entries ---------------------------------------------------

static bool ls_pattern_hidden(string_address name)
{
        for (positive i = 0; i < ls_ignore_count; i++)
                if (shell_match(ls_ignore_patterns[i], name))
                        return true;

        if (ls_hidden || ls_almost)
                return false;

        for (positive i = 0; i < ls_hide_count; i++)
                if (shell_match(ls_hide_patterns[i], name))
                        return true;

        return false;
}

static fn ls_fill(ls_entry address_to entry, file_facts address_to facts)
{
        entry->known = true;
        entry->mode = facts->mode;
        entry->links = facts->hard_links;
        entry->owner = facts->owner;
        entry->group = facts->group;
        entry->size = facts->size;
        entry->modified = facts->modified.seconds;
        entry->modified_fraction = facts->modified.nanoseconds;
        entry->accessed = facts->accessed.seconds;
        entry->accessed_fraction = facts->accessed.nanoseconds;
        entry->changed = facts->changed.seconds;
        entry->changed_fraction = facts->changed.nanoseconds;
        entry->created_known = (facts->mask & STATX_BIRTH) != 0;
        entry->created = entry->created_known ? facts->created.seconds : 0;
        entry->created_fraction = entry->created_known ? facts->created.nanoseconds : 0;
        entry->inode = facts->inode;
        entry->blocks = facts->blocks;
        entry->rdev_major = facts->rdev_major;
        entry->rdev_minor = facts->rdev_minor;
}

/*
        One entry into the listing. An operand has no directory above it and
        its dirent kind is unknown; an entry read out of a directory brings
        both, and when the kernel will not describe it the kind the directory
        gave is what the listing has to go on.

        The reference ls asks the kernel about an entry only when a column or
        an order wants the answer, and a plain listing of a directory whose
        entries cannot be looked at prints their names and says nothing. The
        moment -l, -i, -s, -t, -S, -F or colour is asked for, the failure is
        reported, the entry is printed as unknown, and the status says so; a
        failed operand is a failed operand, and answers 2.
*/
static bool ls_add(bipolar directory, string_address path, string_address shown,
                   p8 type, string_address under, file_facts address_to given)
{
        if (ls_count >= LS_MAX_ENTRIES)
        {
                ls_limit((string_address) "directory has too many entries");
                return false;
        }

        file_facts facts;
        ls_entry address_to entry = address_of ls_entries[ls_count];
        bipolar looked = 0;

        memory_fill(entry, 0, sizeof(ls_entry));

        if (given)
                facts = *given;
        else
        {
                looked = file_look_code(directory, path,
                                        ls_dereference == 'L' ? 0 : AT_SYMLINK_NOFOLLOW,
                                        address_of facts);

                if (looked < 0 && !under)
                {
                        string_format(log_error, "%s: cannot access '%s': %s\n",
                                      ls_program, shown, file_reason(looked));
                        ls_status = 2;
                        return true;
                }
        }

        if (!ls_keep(shown, address_of entry->name))
                return false;

        if (looked == 0)
        {
                ls_fill(entry, address_of facts);

                // A link's target decides which group it sorts with and what
                // mark or colour it gets, when any of those was asked for.
                if ((facts.mode & MODE_FORMAT) == MODE_LINK &&
                    (ls_group_directories || ls_indicator || ls_coloring || ls_format == 'l'))
                {
                        file_facts through;
                        p8 full[FILE_PATH_MAX];

                        if (ls_full_path(full, under, path) && file_look_at(full, address_of through))
                                entry->points_at_directory =
                                    (through.mode & MODE_FORMAT) == MODE_DIRECTORY;
                }
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

                if (ls_format == 'l' || ls_inode || ls_blocks || ls_sorting == 't' ||
                    ls_sorting == 'S' || ls_dereference == 'L' ||
                    ((ls_indicator || ls_coloring) && !format) ||
                    (ls_indicator == 'F' && format == MODE_FILE) ||
                    (ls_coloring && (format == MODE_FILE || format == MODE_DIRECTORY)))
                {
                        p8 full[FILE_PATH_MAX];

                        string_format(log_error, "%s: cannot access '%s': %s\n",
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

static bool ls_already_listed(file_facts address_to facts)
{
        p64 device = file_device_key(facts->device_major, facts->device_minor);

        for (positive i = 0; i < ls_listed_count; i++)
                if (ls_listed_device[i] == device && ls_listed_inode[i] == facts->inode)
                        return true;

        if (ls_listed_count < LS_LISTED)
        {
                ls_listed_device[ls_listed_count] = device;
                ls_listed_inode[ls_listed_count] = facts->inode;
                ls_listed_count++;
        }

        return false;
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
                ls_entry address_to entry = address_of ls_entries[ls_sorted[k]];

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
                        string_format(log_error, "%s: '%s/%s' is nested too deep\n",
                                      ls_program, path, name);
                        ls_status = 1;
                        continue;
                }

                if (!file_path_join(below, path, name))
                {
                        string_format(log_error, "%s: %s '%s/%s': %s\n", ls_program, (string_address) "cannot open directory", path, name, file_reason(-ERROR_NAME_TOO_LONG));
                        ls_status = 1;
                        continue;
                }

                ls_directory(below, true, depth - 1, false);
        }
}

// A directory that will not open is answered with 2 when it was named on
// the command line and 1 when -R met it on the way down, as the reference
// ls answers; one already listed under -R is named and passed over.
static fn ls_directory(string_address path, bool heading, positive depth,
                       bool named)
{
        file_walk walk;
        file_facts identity;

        if (ls_recursive && file_look_at(path, address_of identity) &&
            ls_already_listed(address_of identity))
        {
                string_format(log_error, "%s: %s: not listing already-listed directory\n",
                              ls_program, path);
                ls_status = 2;
                return;
        }

        if (!file_walk_open(address_of walk, AT_FDCWD, path))
        {
                string_format(log_error, "%s: cannot open directory '%s': %s\n",
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

                if (ls_ignore_backups && entry->d_name[0] &&
                    entry->d_name[string_length(entry->d_name) - 1] == '~')
                        continue;

                if (ls_pattern_hidden(entry->d_name))
                        continue;

                if (!ls_add(walk.handle, entry->d_name, entry->d_name,
                            entry->d_type, path, null))
                        break;
        }

        file_walk_close(address_of walk);

        if (ls_broken)
                return;

        ls_sort();

        if (heading)
        {
                if (ls_written)
                        ls_out(address_of ls_eol, 1);

                if (ls_dired)
                        ls_out("  ", 2);

                if (ls_hyperlink)
                        ls_hyperlink_open(null, path);

                positive begin = ls_out_bytes;

                ls_quote(ls_out, path);

                if (ls_dired && ls_subdired_count + 2 <= array_count(ls_subdired_marks))
                {
                        ls_subdired_marks[ls_subdired_count++] = begin;
                        ls_subdired_marks[ls_subdired_count++] = ls_out_bytes;
                }

                if (ls_hyperlink)
                        ls_hyperlink_close();

                ls_out(":", 1);
                ls_out(address_of ls_eol, 1);
        }

        ls_written = true;

        ls_print(path);

        if (ls_recursive)
                ls_below(path, depth);
}

// ---- Options -------------------------------------------------------------

static const file_long ls_longs[] = {
    {(string_address) "all", 'a'},
    {(string_address) "almost-all", 'A'},
    {(string_address) "author", '8'},
    {(string_address) "escape", 'b'},
    {(string_address) "block-size", '7'},
    {(string_address) "ignore-backups", 'B'},
    {(string_address) "color", 'K'},
    {(string_address) "directory", 'd'},
    {(string_address) "dired", 'D'},
    {(string_address) "classify", 'E'},
    {(string_address) "file-type", 'j'},
    {(string_address) "format", 'J'},
    {(string_address) "full-time", 'M'},
    {(string_address) "group-directories-first", 'O'},
    {(string_address) "no-group", 'G'},
    {(string_address) "human-readable", 'h'},
    {(string_address) "si", 'P'},
    {(string_address) "dereference-command-line", 'H'},
    {(string_address) "dereference-command-line-symlink-to-dir", 'V'},
    {(string_address) "hide", 'W'},
    {(string_address) "hyperlink", 'y'},
    {(string_address) "indicator-style", 'Y'},
    {(string_address) "inode", 'i'},
    {(string_address) "ignore", 'I'},
    {(string_address) "kibibytes", 'k'},
    {(string_address) "dereference", 'L'},
    {(string_address) "numeric-uid-gid", 'n'},
    {(string_address) "literal", 'N'},
    {(string_address) "hide-control-chars", 'q'},
    {(string_address) "show-control-chars", '2'},
    {(string_address) "quote-name", 'Q'},
    {(string_address) "quoting-style", 'z'},
    {(string_address) "reverse", 'r'},
    {(string_address) "recursive", 'R'},
    {(string_address) "size", 's'},
    {(string_address) "sort", '3'},
    {(string_address) "time", '4'},
    {(string_address) "time-style", '5'},
    {(string_address) "tabsize", 'T'},
    {(string_address) "width", 'w'},
    {(string_address) "context", 'Z'},
    {(string_address) "zero", '6'},
    {null, 0},
};

// -I and --hide are the two options ls takes more than once.
typedef struct
{
        string_address word;
        p8 answer;
        bool alone;
} ls_word;

// One word among several spellings, or a complaint listing them the way the
// reference lists them: each answer once, its synonyms beside it.
static b32 ls_word_among(string_address option, string_address value,
                         const ls_word address_to words, positive count)
{
        b32 answer = -1;
        bool ambiguous = false;

        for (positive i = 0; i < count; i++)
        {
                if (!string_compare(value, words[i].word))
                        return words[i].answer;

                if (!file_word_begins(value, words[i].word))
                        continue;

                if (answer < 0)
                        answer = words[i].answer;
                else if (answer != words[i].answer)
                        ambiguous = true;
        }

        if (answer >= 0 && !ambiguous)
                return answer;

        string_format(log_error, "%s: %s argument '%s' for '%s'\nValid arguments are:\n",
                      ls_program, ambiguous ? "ambiguous" : "invalid", value, option);

        for (positive i = 0; i < count; i++)
        {
                if (i && !words[i].alone && words[i].answer == words[i - 1].answer)
                {
                        string_format(log_error, ", '%s'", words[i].word);
                        continue;
                }

                if (i)
                        log_error("\n", 1);
                string_format(log_error, "  - '%s'", words[i].word);
        }

        log_error("\n", 1);
        return -1;
}

static const ls_word ls_when_words[] = {
    {"always", 'a'}, {"yes", 'a'}, {"force", 'a'},
    {"never", 'n'}, {"no", 'n'}, {"none", 'n'},
    {"auto", 't'}, {"tty", 't'}, {"if-tty", 't'}};
static const ls_word ls_format_words[] = {
    {"verbose", 'l'}, {"long", 'l'}, {"commas", 'm'}, {"horizontal", 'x'}, {"across", 'x'},
    {"vertical", 'C'}, {"single-column", '1'}};
static const ls_word ls_sort_words[] = {
    {"none", 'U'}, {"size", 'S'}, {"time", 't'}, {"version", 'v'}, {"extension", 'X'},
    {"name", 'n'}, {"width", 'w'}};
static const ls_word ls_time_words[] = {
    {"atime", 'a'}, {"access", 'a'}, {"use", 'a'}, {"ctime", 'c'}, {"status", 'c'},
    {"mtime", 'm'}, {"modification", 'm'}, {"birth", 'b'}, {"creation", 'b'}};
static const ls_word ls_quoting_words[] = {
    {"literal", 'L'}, {"shell", 's'}, {"shell-always", 'S'}, {"shell-escape", 'e'},
    {"shell-escape-always", 'E'}, {"c", 'c'}, {"c-maybe", 'c', true}, {"escape", 'b'},
    {"locale", 'o'}, {"clocale", 'o', true}};
static const ls_word ls_indicator_words[] = {
    {"none", 'N'}, {"slash", '/'}, {"file-type", 'f'}, {"classify", 'F'}};

/*
        Every option argument is read where the option is, not where the
        answer needs it.

        The reference is a getopt loop and refuses a word it does not know as
        it reaches it, so of two bad words the first one written is the one
        reported -- and a word for something this listing will not print is
        refused all the same. Only --time-style is left to its own place: the
        reference reads that one where it writes a time, so a plain listing
        takes a style it would otherwise refuse.
*/
static b32 ls_option_status;

static bool ls_option_word(p8 letter, string_address value)
{
        if (!value)
                return true;

        switch (letter)
        {
        case 'J':
                return ls_word_among((string_address) "--format", value,
                                     ls_format_words, array_count(ls_format_words)) >= 0;
        case '3':
                return ls_word_among((string_address) "--sort", value,
                                     ls_sort_words, array_count(ls_sort_words)) >= 0;
        case '4':
                return ls_word_among((string_address) "--time", value,
                                     ls_time_words, array_count(ls_time_words)) >= 0;
        case 'z':
                return ls_word_among((string_address) "--quoting-style", value,
                                     ls_quoting_words, array_count(ls_quoting_words)) >= 0;
        case 'Y':
                return ls_word_among((string_address) "--indicator-style", value,
                                     ls_indicator_words, array_count(ls_indicator_words)) >= 0;
        case 'K':
                return ls_word_among((string_address) "--color", value,
                                     ls_when_words, array_count(ls_when_words)) >= 0;
        case 'y':
                return ls_word_among((string_address) "--hyperlink", value,
                                     ls_when_words, array_count(ls_when_words)) >= 0;
        default:
                return true;
        }
}

static bool ls_option_seen(p8 letter, string_address value)
{
        /*
                -1 and --zero each ask for one name per line, and each of
                them has no effect after a long listing was asked for. The
                question is answered where the option is read, so a format
                written after them wins and one written before does not.
        */
        if (letter == '1' || letter == '6')
        {
                p8 chosen = ls_format_option;

                if (chosen == 'J')
                        chosen = 'l'; // decided by its word, checked below

                if (!chosen || !string_first_of((string_address) "lgonMD", chosen))
                        ls_format_option = '1';
                else if (ls_format_option == 'J')
                        ls_zero_after_word = true;

                return true;
        }

        if (!ls_option_word(letter, value))
        {
                ls_option_status = 1;
                return false;
        }

        if ((letter != 'I' && letter != 'W') || !value)
                return true;

        string_address address_to table = letter == 'I' ? ls_ignore_patterns : ls_hide_patterns;
        positive address_to have = letter == 'I' ? address_of ls_ignore_count
                                                 : address_of ls_hide_count;

        if (address_to have >= LS_PATTERNS)
        {
                string_format(log_error, "%s: too many patterns to ignore\n", ls_program);
                return false;
        }

        table[(address_to have)++] = value;
        return true;
}

static bool ls_when_active(p8 when)
{
        return when == 'a' || (when == 't' && ls_terminal);
}

static bool ls_count_option(string_address value, string_address what,
                            positive address_to into)
{
        string_address at = value;
        positive parsed;

        if (!value || !string_digits_checked(address_of at, 10, address_of parsed) ||
            string_get(at) || !string_get(value))
        {
                string_format(log_error, "%s: invalid %s: '%s'\n", ls_program, what,
                              value ? value : (string_address) "");
                return false;
        }

        address_to into = parsed;
        return true;
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

static fn ls_dired_finish()
{
        if (!ls_dired)
                return;

        if (ls_dired_count)
        {
                ls_out("//DIRED//", 0);
                for (positive i = 0; i < ls_dired_count; i++)
                {
                        ls_out(" ", 1);
                        positive_to_string(ls_out, ls_dired_marks[i]);
                }
                ls_out("\n", 1);
        }

        if (ls_subdired_count)
        {
                ls_out("//SUBDIRED//", 0);
                for (positive i = 0; i < ls_subdired_count; i++)
                {
                        ls_out(" ", 1);
                        positive_to_string(ls_out, ls_subdired_marks[i]);
                }
                ls_out("\n", 1);
        }

        string_address style = ls_quoting == 's'   ? "shell"
                               : ls_quoting == 'S' ? "shell-always"
                               : ls_quoting == 'e' ? "shell-escape"
                               : ls_quoting == 'E' ? "shell-escape-always"
                               : ls_quoting == 'c' ? "c"
                               : ls_quoting == 'b' ? "escape"
                               : ls_quoting == 'o' ? "locale"
                                                   : "literal";

        string_format(ls_out, "//DIRED-OPTIONS// --quoting-style=%s\n", style);
}

/*
        What an operand is, under the following policy in force: a directory
        to list, or an entry of its own. The reference follows a command line
        link to a directory when nothing asked about the link itself, follows
        every command line link under -H and every link under -L, and asks
        the kernel about the link alone otherwise; a link to nothing is an
        entry when following was only a convenience and a failure when it was
        asked for.
*/
static bool ls_operand(string_address path, file_facts address_to facts, bool address_to directory)
{
        bipolar looked;

        if (ls_dereference == 'N')
                looked = file_look_code(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, facts);
        else
        {
                looked = file_look_code(AT_FDCWD, path, 0, facts);

                if (looked < 0 && ls_dereference == 'D')
                        looked = file_look_code(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, facts);
                else if (looked == 0 && ls_dereference == 'D' &&
                         (facts->mode & MODE_FORMAT) != MODE_DIRECTORY)
                        looked = file_look_code(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, facts);
        }

        if (looked < 0)
        {
                string_format(log_error, "%s: cannot access '%s': %s\n", ls_program, path,
                              file_reason(looked));
                ls_status = 2;
                return false;
        }

        address_to directory = (facts->mode & MODE_FORMAT) == MODE_DIRECTORY && !ls_as_itself;
        return true;
}

static b32 file_ls_as(string_address program, p8 default_format, p8 default_quoting)
{
        positive count = (positive)program_argument_count();

        ls_program = program;
        ls_format_option = 0;
        ls_sort_option = 0;
        ls_time_option = 0;
        ls_quote_option = 0;
        ls_indicator_option = 0;
        ls_hidden_option = 0;
        ls_deref_option = 0;
        ls_size_option = 0;
        ls_control_option = 0;
        ls_zero_after_word = false;
        ls_option_status = 0;
        ls_ignore_count = 0;
        ls_hide_count = 0;
        ls_status = 0;
        ls_written = false;
        ls_broken = false;
        ls_out_bytes = 0;
        ls_dired_count = 0;
        ls_subdired_count = 0;
        ls_listed_count = 0;
        ls_color_started = false;

        file_taking taking = {
            .program = program,
            .allowed = (string_address) "aAbBcCdDfFgGhHiIkKlLmnNopqQrRsStTuUvwxXZ1",
            .valued = (string_address) "IwT7JWYz345",
            .optional = (string_address) "",
            .long_optional = (string_address) "KEy",
            .longs = ls_longs,
            .seen = ls_option_seen,
            .supersedes = ls_supersedes,
        };

        if (!file_take(address_of taking))
                return ls_option_status ? ls_option_status : 2;

        positive flags = taking.flags;
        positive first = taking.first;

        ls_now = file_now();
        ls_terminal = stream_is_terminal(1);

        // The format: one name per line unless a terminal is watching, and
        // the last word on it wins; -g, -o, -n, --full-time and --dired are
        // all ways of asking for the long one.
        ls_format = default_format ? default_format : ls_terminal ? 'C' : '1';
        if (ls_format_option == 'J')
        {
                b32 word = ls_word_among((string_address) "--format",
                                         file_option_value(address_of taking, 'J'),
                                         ls_format_words, array_count(ls_format_words));
                if (word < 0)
                        return 1;
                ls_format = (p8)word;
        }
        else if (ls_format_option && string_first_of((string_address) "lgonMD", ls_format_option))
                ls_format = 'l';
        else if (ls_format_option)
                ls_format = ls_format_option;

        //      --zero came after a --format=WORD that turned out not to be
        //      the long one, so it has its say after all.
        if (ls_zero_after_word && ls_format != 'l')
                ls_format = '1';

        ls_owner_shown = !(flags & FILE_FLAG('g'));
        ls_group_shown = !(flags & (FILE_FLAG('o') | FILE_FLAG('G')));
        ls_author = (flags & FILE_FLAG('8')) != 0;
        ls_numeric = (flags & FILE_FLAG('n')) != 0;
        //      --dired is a long listing's own annotation: asked for
        //      beside any other format it is dropped, and asked for beside
        //      --zero, which the long listing survives, the two cannot both
        //      be answered.
        ls_dired = (flags & FILE_FLAG('D')) != 0 && ls_format == 'l';

        if (ls_dired && (flags & FILE_FLAG('6')))
                return string_report(log_error, 2, "%s: --dired and --zero are incompatible\n",
                                     program);

        ls_context = (flags & FILE_FLAG('Z')) != 0;
        ls_inode = (flags & FILE_FLAG('i')) != 0;
        ls_blocks = (flags & FILE_FLAG('s')) != 0;
        ls_recursive = (flags & FILE_FLAG('R')) != 0;
        ls_as_itself = (flags & FILE_FLAG('d')) != 0;
        ls_reversed = (flags & FILE_FLAG('r')) != 0;
        ls_group_directories = (flags & FILE_FLAG('O')) != 0;
        ls_ignore_backups = (flags & FILE_FLAG('B')) != 0;
        ls_kibibytes = (flags & FILE_FLAG('k')) != 0;
        ls_hyperlink = false;
        ls_eol = (flags & FILE_FLAG('6')) ? 0 : '\n';

        // Which entries: -f is -a with no sorting, and a later -A narrows it.
        ls_hidden = ls_hidden_option == 'a' || ls_hidden_option == 'f';
        ls_almost = ls_hidden_option == 'A';

        // The order.
        ls_sorting = 'n';
        if (ls_sort_option == '3')
        {
                b32 word = ls_word_among((string_address) "--sort",
                                         file_option_value(address_of taking, '3'),
                                         ls_sort_words, array_count(ls_sort_words));
                if (word < 0)
                        return 1;
                ls_sorting = (p8)word;
        }
        else if (ls_sort_option == 'f')
                ls_sorting = 'U';
        else if (ls_sort_option)
                ls_sorting = ls_sort_option;

        // Which time.
        ls_time_key = 'm';
        if (ls_time_option == '4')
        {
                b32 word = ls_word_among((string_address) "--time",
                                         file_option_value(address_of taking, '4'),
                                         ls_time_words, array_count(ls_time_words));
                if (word < 0)
                        return 1;
                ls_time_key = (p8)word;
        }
        else if (ls_time_option == 'c')
                ls_time_key = 'c';
        else if (ls_time_option == 'u')
                ls_time_key = 'a';

        /*
                -u and -c say which time is meant, and where no long listing
                and no sort was asked for they say the order as well: the
                reference puts the newest of that time first. With -l the
                order stays by name and the time is only shown, and with -lt
                it is -t that orders it. So this is the case where neither
                was named.
        */
        if (ls_time_key != 'm' && !ls_sort_option && ls_format != 'l')
                ls_sorting = 't';

        /*
                How a time is written. The reference reads the style only
                where it is going to write one, so a listing that shows no
                time takes a style it would otherwise refuse -- `ls
                --time-style=bogus` is a plain listing and says nothing,
                while `ls -l --time-style=bogus` is the error.
        */
        ls_time_style = 'd';
        if ((flags & FILE_FLAG('5')) && ls_format == 'l')
        {
                string_address style = file_option_value(address_of taking, '5');

                if (string_is(style, 'p') && string_is(style + 1, 'o') &&
                    string_is(style + 2, 's') && string_is(style + 3, 'i') &&
                    string_is(style + 4, 'x') && string_is(style + 5, '-'))
                        style = (string_address) "locale";

                if (string_is(style, '+'))
                {
                        string_address newline = string_first_of(style + 1, '\n');

                        ls_time_style = '+';
                        ls_time_format_old = style + 1;
                        ls_time_format_recent = style + 1;

                        if (newline)
                        {
                                // Two formats on either side of the newline:
                                // the first for old files, the second for
                                // recent ones. The word is cut in place.
                                address_to newline = end;
                                ls_time_format_recent = newline + 1;
                        }
                }
                else if (!string_compare(style, "full-iso"))
                        ls_time_style = 'f';
                else if (!string_compare(style, "long-iso"))
                        ls_time_style = 'l';
                else if (!string_compare(style, "iso"))
                        ls_time_style = 'i';
                else if (string_compare(style, "locale"))
                {
                        string_format(log_error,
                                      "%s: invalid argument '%s' for 'time style'\n"
                                      "Valid arguments are:\n"
                                      "  - [posix-]full-iso\n"
                                      "  - [posix-]long-iso\n"
                                      "  - [posix-]iso\n"
                                      "  - [posix-]locale\n"
                                      "  - +FORMAT (e.g., +%%H:%%M) for a 'date'-style format\n",
                                      program, style);
                        return 2;
                }
        }
        if (flags & FILE_FLAG('M'))
                ls_time_style = 'f';

        // How a name is spelled.
        ls_quoting = default_quoting ? default_quoting : ls_terminal ? 'e' : 'L';
        if (ls_quote_option == 'z')
        {
                b32 word = ls_word_among((string_address) "--quoting-style",
                                         file_option_value(address_of taking, 'z'),
                                         ls_quoting_words, array_count(ls_quoting_words));
                if (word < 0)
                        return 1;
                ls_quoting = (p8)word;
        }
        else if (ls_quote_option == 'N')
                ls_quoting = 'L';
        else if (ls_quote_option == 'Q')
                ls_quoting = 'c';
        else if (ls_quote_option == 'b')
                ls_quoting = 'b';
        if (ls_quote_option == '6')
                ls_quoting = 'L';

        ls_hide_controls = ls_control_option ? ls_control_option == 'q'
                                             : ls_terminal && !(flags & FILE_FLAG('6'));

        // The letter after a name.
        ls_indicator = 0;
        if (ls_indicator_option == 'Y')
        {
                b32 word = ls_word_among((string_address) "--indicator-style",
                                         file_option_value(address_of taking, 'Y'),
                                         ls_indicator_words, array_count(ls_indicator_words));
                if (word < 0)
                        return 1;
                ls_indicator = word == 'N' ? 0 : (p8)word;
        }
        else if (ls_indicator_option == 'E')
        {
                string_address when_text = file_option_value(address_of taking, 'E');
                b32 when = when_text ? ls_word_among((string_address) "--classify", when_text,
                                                     ls_when_words, array_count(ls_when_words))
                                     : 'a';

                if (when < 0)
                        return 2;
                if (ls_when_active((p8)when))
                        ls_indicator = 'F';
        }
        else if (ls_indicator_option == 'p')
                ls_indicator = '/';
        else if (ls_indicator_option == 'j')
                ls_indicator = 'f';
        else if (ls_indicator_option == 'F')
                ls_indicator = 'F';

        // What is followed.
        ls_dereference = ls_deref_option == 'L'   ? 'L'
                         : ls_deref_option == 'H' ? 'H'
                         : ls_deref_option == 'V' ? 'D'
                         : (ls_as_itself || ls_indicator == 'F' || ls_format == 'l') ? 'N'
                                                                                     : 'D';

        // Sizes: -h, --si and --block-size answer for the long listing, and
        // for the blocks column too unless -k holds that at a kibibyte.
        ls_size_unit = 1;
        ls_size_human = false;
        ls_size_si = false;
        ls_size_suffix[0] = end;
        ls_block_unit = 1024;
        ls_block_human = false;
        ls_block_si = false;
        ls_block_suffix[0] = end;

        if (ls_size_option == 'h' || ls_size_option == 'P')
        {
                ls_size_human = ls_block_human = true;
                ls_size_si = ls_block_si = ls_size_option == 'P';
        }
        else if (ls_size_option == '7')
        {
                string_address given = file_option_value(address_of taking, '7');

                if (!ls_block_size_read(given, address_of ls_size_unit, address_of ls_size_human,
                                        address_of ls_size_si, ls_size_suffix))
                {
                        string_format(log_error, "%s: invalid --block-size argument '%s'\n",
                                      program, given);
                        return 2;
                }

                ls_block_unit = ls_size_unit;
                ls_block_human = ls_size_human;
                ls_block_si = ls_size_si;
                memory_copy_apart(ls_block_suffix, ls_size_suffix, sizeof(ls_block_suffix));
        }

        if (ls_kibibytes)
        {
                ls_block_unit = 1024;
                ls_block_human = false;
                ls_block_si = false;
                ls_block_suffix[0] = end;
        }

        // The line.
        ls_width = ls_column_limit();
        if (flags & FILE_FLAG('w'))
        {
                if (!ls_count_option(file_option_value(address_of taking, 'w'),
                                     (string_address) "line width", address_of ls_width))
                        return 2;
        }
        ls_tabsize = 8;
        if (flags & FILE_FLAG('T'))
        {
                if (!ls_count_option(file_option_value(address_of taking, 'T'),
                                     (string_address) "tab size", address_of ls_tabsize))
                        return 2;
        }

        // Colour.
        ls_coloring = false;
        ls_colors = file_environment((string_address) "LS_COLORS");

        if (ls_colors && string_get(ls_colors) &&
            !file_color_table_valid(ls_colors, false))
        {
                string_format(log_error,
                              "%s: unparsable value for LS_COLORS environment variable\n",
                              program);
                ls_colors = null;
        }

        if (flags & FILE_FLAG('K'))
        {
                string_address when_text = file_option_value(address_of taking, 'K');
                b32 when = when_text ? ls_word_among((string_address) "--color", when_text,
                                                     ls_when_words, array_count(ls_when_words))
                                     : 'a';

                if (when < 0)
                        return 1;

                ls_coloring = ls_colors && string_get(ls_colors) && ls_when_active((p8)when);
        }

        //      A run of names with nothing between them but a zero byte is
        //      not a place for colour, whichever order the two were asked in.
        if (flags & FILE_FLAG('6'))
                ls_coloring = false;

        if (ls_coloring)
                ls_color_parse();

        if (flags & FILE_FLAG('y'))
        {
                string_address when_text = file_option_value(address_of taking, 'y');
                b32 when = when_text ? ls_word_among((string_address) "--hyperlink", when_text,
                                                     ls_when_words, array_count(ls_when_words))
                                     : 'a';

                if (when < 0)
                        return 2;

                ls_hyperlink = ls_when_active((p8)when);

                if (ls_hyperlink)
                {
                        file_machine machine;

                        memory_fill(address_of machine, 0, sizeof(machine));
                        system_call_1(syscall(uname), (positive)address_of machine);
                        string_copy_max_end(ls_host, machine.node, FILE_NAME_MAX - 1);

                        if (system_call_2(syscall(getcwd), (positive)ls_cwd, FILE_PATH_MAX) < 0)
                                ls_cwd[0] = end;
                }
        }

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
                                   (string_address) ".", 0, null, null))
                        {
                                ls_sort();
                                ls_print(null);
                        }
                }
                else
                        ls_directory((string_address) ".", ls_recursive,
                                     FILE_MAX_DEPTH, true);

                ls_dired_finish();
                log_flush();
                return ls_status;
        }

        // Everything that is not a directory is listed first, together, and
        // then each directory in turn -- which is the order the system's own
        // ls uses and the only one where a mixed set of operands reads. The
        // column widths of the first group count the directories too, as
        // the reference's do.
        ls_count = 0;
        ls_used = 0;

        positive directories = 0;
        positive given = count - first;

        for (positive i = first; i < count; i++)
        {
                string_address path = program_argument((b32)i);
                file_facts facts;
                bool directory;

                if (!ls_operand(path, address_of facts, address_of directory))
                        continue;

                if (directory)
                        directories++;

                if (!ls_add(AT_FDCWD, path, path, 0, null, address_of facts))
                        break;

                ls_entries[ls_count - 1].points_at_directory |= directory;
        }

        if (ls_broken)
        {
                log_flush();
                return ls_status;
        }

        // The directories come out of the group once the widths are known;
        // their order is the sort's, however they were typed.
        ls_sort();

        positive order[LS_MAX_ENTRIES];
        positive have = 0;
        positive files = 0;

        for (positive k = 0; k < ls_count; k++)
        {
                positive index = ls_sorted[k];
                ls_entry address_to entry = address_of ls_entries[index];

                if ((entry->mode & MODE_FORMAT) == MODE_DIRECTORY && !ls_as_itself &&
                    file_is_directory_through(ls_arena + entry->name))
                        order[have++] = index;
                else if ((entry->mode & MODE_FORMAT) != MODE_DIRECTORY || ls_as_itself)
                        ls_sorted[files++] = index;
                else
                        order[have++] = index;
        }

        if (files)
        {
                positive whole = ls_count;

                // Widths are computed over the whole group and printing runs
                // over the first `files` sorted entries only.
                ls_count = files;
                ls_print(null);
                ls_count = whole;
                ls_written = true;
        }

        bool headings = given > 1 || ls_recursive;

        // The directory names are copied out before the listing buffers are
        // reused for the first of them.
        p8 names[LS_ARENA / 4];
        positive kept = 0;

        for (positive i = 0; i < have; i++)
        {
                string_address name = ls_arena + ls_entries[order[i]].name;
                positive length = string_length(name);

                if (kept + length + 1 > sizeof(names))
                {
                        ls_limit((string_address) "too many directory operands");
                        log_flush();
                        return ls_status;
                }

                memory_copy_apart(names + kept, name, length + 1);
                kept += length + 1;
        }

        positive at = 0;

        for (positive i = 0; i < have; i++)
        {
                string_address name = names + at;

                at += string_length(name) + 1;
                ls_directory(name, headings, FILE_MAX_DEPTH, true);
        }

        ls_dired_finish();
        log_flush();

        return ls_status;
}

static b32 file_ls()
{
        return file_ls_as((string_address) "ls", 0, 0);
}

/* GNU dir is the shared ls engine with -C and -b selected by default. */
static b32 file_dir()
{
        return file_ls_as((string_address) "dir", 'C', 'b');
}

static b32 file_vdir()
{
        return file_ls_as((string_address) "vdir", 'l', 'b');
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
//      Why a command could not be run, in the words the reference uses.
//      Only what an exec can answer with is here; anything else keeps the
//      commonest reading, which is the one a missing command gives.
static string_address file_exec_refusal(bipolar answer)
{
        switch (-answer)
        {
        case ERROR_ACCESS:
                return "Permission denied";
        case ERROR_NOT_DIRECTORY:
                return "Not a directory";
        case ERROR_IS_DIRECTORY:
                return "Is a directory";
        case ERROR_ARGUMENT_LIST:
                return "Argument list too long";
        case ERROR_NAME_TOO_LONG:
                return "File name too long";
        case ERROR_NO_MEMORY:
                return "Cannot allocate memory";
        case ERROR_LOOP:
                return "Too many levels of symbolic links";
        case ERROR_EXEC_FORMAT:
                return "Exec format error";
        }

        return "No such file or directory";
}

/*
        A name inside a diagnostic, quoted the way the reference quotes it:
        single quotes around it, and a backslash in front of a backslash or
        an apostrophe so the quoting cannot be walked out of. A control byte
        is written as the escape it is known by, or as its octal, so a name
        carrying a newline stays on one line.
*/
static fn file_quoted_name(string_address name)
{
        log_error("'", 1);

        for (string_address at = name; at[0]; at++)
        {
                p8 byte = (p8)at[0];
                string_address escape = null;

                switch (byte)
                {
                case '\\': escape = "\\\\"; break;
                case '\'': escape = "\\'"; break;
                case '\a': escape = "\\a"; break;
                case '\b': escape = "\\b"; break;
                case '\f': escape = "\\f"; break;
                case '\n': escape = "\\n"; break;
                case '\r': escape = "\\r"; break;
                case '\t': escape = "\\t"; break;
                case '\v': escape = "\\v"; break;
                }

                if (escape)
                {
                        log_error(escape, 2);
                        continue;
                }

                if (byte < ' ' || byte == 127)
                {
                        p8 octal[4] = {'\\', (p8)('0' + ((byte >> 6) & 7)),
                                       (p8)('0' + ((byte >> 3) & 7)),
                                       (p8)('0' + (byte & 7))};

                        log_error(octal, 4);
                        continue;
                }

                log_error(at, 1);
        }

        log_error("'", 1);
}

static fn file_exec_path(string_address address_to words)
{
        bipolar answer = file_exec_path_try(words);

        /*
                The child says why, because only the child knows: back in the
                parent all that survives is an exit status, and a command
                that merely exits 127 of its own accord looks the same as one
                that was never there. find's -exec reports each command it
                could not run and carries on, which is what the reference
                does.
        */
        log_error("find: ", 6);
        file_quoted_name(words[0]);
        string_format(log_error, ": %s\n", file_exec_refusal(answer));
        log_flush();

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

/* Saturating options consume the complete overflowing run and retain syntax. */
static bool file_decimal_read(string_address address_to text, bool saturate,
                              positive address_to value)
{
        if (string_digits_checked(text, 10, value))
                return true;
        if (!saturate || !byte_is_digit(string_get(address_to text)))
                return false;
        address_to text += string_span_of_set(address_to text, "0123456789");
        address_to value = positive_max;
        return true;
}

static bool file_signed_decimal(string_address text, bipolar address_to value)
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

static bool file_unsigned_decimal(string_address text,
                                   positive address_to number)
{
        string_address at = text;
        positive value;

        if (!string_digits_checked(address_of at, 10, address_of value) ||
            string_get(at))
                return false;

        address_to number = value;
        return true;
}


// nice -------------------------------------------------------------
#define NICE_PROCESS 0

static bool nice_adjustment(string_address text, bipolar address_to value)
{
        text += string_span(text, string_set_blanks);
        bool negative = string_is(text, '-');
        if (negative || string_is(text, '+'))
                text++;
        positive magnitude;
        if (!file_decimal_read(address_of text, true, address_of magnitude) ||
            string_get(text))
                return false;
        address_to value = bipolar_from_magnitude(min(magnitude, (positive)39),
                                                   negative);
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
                                log_error("nice: option requires an argument -- 'n'\n", 0);
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
                                        return string_report(log_error, 125, "nice: option needs an argument: --adjustment\n");

                                first++;
                                continue;
                        }
                }

                if (string_is(word, '-') && !string_is(word + 1, end))
                {
                        //      A word beginning with two dashes is a long
                        //      option the program does not have, and is said
                        //      whole; one dash is a letter, and is said as
                        //      the letter.
                        if (string_is(word + 1, '-'))
                                string_format(log_error,
                                    "nice: unrecognized option '%s'\n", word);
                        else
                        {
                                p8 named[2] = {string_get(word + 1), end};

                                string_format(log_error,
                                    "nice: invalid option -- '%s'\n", named);
                        }

                        string_format(log_error,
                            "Try 'nice --help' for more information.\n");
                        return 125;
                }

                break;
        }

        bipolar adjustment = 10;

        if (given)
        {
                if (!nice_adjustment(given, address_of adjustment))
                        return string_report(log_error, 125, "nice: invalid adjustment '%s'\n",
                                      given);
        }

        if (first >= count)
        {
                if (given)
                        return string_report(log_error, 125, "nice: a command must be given with an adjustment\n");

                bipolar current;

                if (!nice_current(address_of current))
                        return string_report(log_error, 125, "nice: cannot get niceness\n");

                bipolar_to_string(log, current);
                log("\n", 1);
                log_flush();
                return 0;
        }

        bipolar current;

        if (!nice_current(address_of current))
                return string_report(log_error, 125, "nice: cannot get niceness\n");

        bipolar wanted = current + adjustment;

        if (wanted < -20)
                wanted = -20;
        else if (wanted > 19)
                wanted = 19;

        bipolar changed = system_call_3(syscall(setpriority), NICE_PROCESS, 0,
                                        (positive)wanted);

        if (changed < 0)
        {
                string_format(log_error, "nice: cannot set niceness: %s\n",
                              file_reason(changed));

                if (changed != -ERROR_ACCESS && changed != -ERROR_NOT_PERMITTED)
                        return 125;
        }

        string_address address_to words = program_argument_list() + first;

        log_flush();

        bipolar answer = file_exec_path_try(words);

        string_format(log_error, "nice: '%s': %s\n", words[0],
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
        p8 mode;
        b32 unit;
        p8 comparison;
        b32 left;
        b32 right;
        string_address text;
        b64 number;
        b64 extra;
        bipolar output;
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

// The directories entered while following links, by identity, one per
// level: a link back into one of them is the cycle it is, reported and left
// alone, rather than a tree walked to the frame ceiling.
typedef struct
{
        p64 device;
        p64 inode;
} find_ancestor;

static find_ancestor find_ancestors[FILE_MAX_DEPTH + 1];

static string_address find_path;
static string_address find_root_path;
static bool find_daystart;

// The three below are written where the walk's own state is in scope, and
// the parser above them needs to name them.
#define FIND_REGEX_POLICY 5

static bipolar find_output_open(string_address path);
static fn find_printf_walk(string_address format, bipolar handle);
static bool find_regex_holds(find_node address_to node, string_address text);
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

                string_format(log_error, "find: '%s': %s\n", find_path,
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
        else if (test->unit == 'w')
                divisor = 2;
        else if (test->unit == 'k')
                divisor = 1024;
        else if (test->unit == 'M')
                divisor = 1024 * 1024;
        else if (test->unit == 'G')
                divisor = 1024 * 1024 * 1024;

        // find rounds up: a file of one byte is one block, and "-size 1" is
        // meant to find it.
        p64 units = facts->size / divisor + (facts->size % divisor != 0);

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
                log_error("find: out of memory while reading expression\n", 0);
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
                string_format(log_error, "find: missing argument to %s\n", word);
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
        p8 address_to stop = string_copy_max_end(into, text, FILE_PATH_MAX - 1);
        memory_to_lower_ascii(into, (positive)(stop - into));
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

/* Decode a predicate once. Walk options take effect while parsing, even in
   a branch that will not be evaluated; value grammar stays with its opcode. */
#define FIND_SETS_DEEPEST 1
#define FIND_SETS_ONE_SYSTEM 2
#define FIND_SETS_FOLLOW 4
#define FIND_SETS_ACTION 8
#define FIND_TAKES_VALUE 16
#define FIND_SETS_DAYSTART 32

static const struct
{
        string_address name;
        p8 kind;
        p8 sets;
} find_predicates[] = {
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
    {(string_address) "-quit", 'q', FIND_SETS_ACTION},
    {(string_address) "-empty", 'y', 0},
    {(string_address) "-nouser", 'U', 0},
    {(string_address) "-nogroup", 'G', 0},
    {"-exec", 'x', FIND_SETS_ACTION},
    {"-maxdepth", '>', FIND_TAKES_VALUE},
    {"-mindepth", '<', FIND_TAKES_VALUE},
    {"-name", 'n', FIND_TAKES_VALUE},
    {"-iname", 'N', FIND_TAKES_VALUE},
    {"-path", 'p', FIND_TAKES_VALUE},
    {"-wholename", 'p', FIND_TAKES_VALUE},
    {"-ipath", 'P', FIND_TAKES_VALUE},
    {"-lname", 'L', FIND_TAKES_VALUE},
    {"-type", 't', FIND_TAKES_VALUE},
    {"-perm", 'm', FIND_TAKES_VALUE},
    {"-size", 'z', FIND_TAKES_VALUE},
    {"-links", 'k', FIND_TAKES_VALUE},
    {"-inum", 'i', FIND_TAKES_VALUE},
    {"-mtime", 'T', FIND_TAKES_VALUE},
    {"-atime", 'T', FIND_TAKES_VALUE},
    {"-ctime", 'T', FIND_TAKES_VALUE},
    {"-mmin", 'T', FIND_TAKES_VALUE},
    {"-amin", 'T', FIND_TAKES_VALUE},
    {"-cmin", 'T', FIND_TAKES_VALUE},
    {"-user", 'u', FIND_TAKES_VALUE},
    {"-uid", 'u', FIND_TAKES_VALUE},
    {"-group", 'g', FIND_TAKES_VALUE},
    {"-gid", 'g', FIND_TAKES_VALUE},
    {"-newer", 'w', FIND_TAKES_VALUE},
    {"-newermt", 'w', FIND_TAKES_VALUE},
    {"-anewer", 'w', FIND_TAKES_VALUE},
    {"-cnewer", 'w', FIND_TAKES_VALUE},
    {(string_address) "-daystart", 'v', FIND_SETS_DAYSTART},
    {(string_address) "-noleaf", 'v', 0},
    {(string_address) "-warn", 'v', 0},
    {(string_address) "-nowarn", 'v', 0},
    {(string_address) "-ignore_readdir_race", 'v', 0},
    {(string_address) "-noignore_readdir_race", 'v', 0},
    {(string_address) "-readable", 'A', 0},
    {(string_address) "-writable", 'A', 0},
    {(string_address) "-executable", 'A', 0},
    {"-execdir", 'x', FIND_SETS_ACTION},
    {"-ok", 'x', FIND_SETS_ACTION},
    {"-okdir", 'x', FIND_SETS_ACTION},
    {"-regex", 'R', FIND_TAKES_VALUE},
    {"-iregex", 'R', FIND_TAKES_VALUE},
    {"-regextype", 'v', FIND_TAKES_VALUE},
    {"-samefile", 'S', FIND_TAKES_VALUE},
    {"-xtype", 'Y', FIND_TAKES_VALUE},
    {"-used", 'B', FIND_TAKES_VALUE},
    {"-ilname", 'L', FIND_TAKES_VALUE},
    {"-iwholename", 'P', FIND_TAKES_VALUE},
    {"-printf", 'l', FIND_SETS_ACTION | FIND_TAKES_VALUE},
    {"-fprintf", 'l', FIND_SETS_ACTION | FIND_TAKES_VALUE},
    {"-fprint", 'e', FIND_SETS_ACTION | FIND_TAKES_VALUE},
    {"-fprint0", 'e', FIND_SETS_ACTION | FIND_TAKES_VALUE},
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
                        log_error("find: expected ')'\n", 0);
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

        // -newerXY is a family rather than a word: X says which of this
        // file's times to take and Y which of the other's.
        if (!string_compare_max(word, "-newer", 6) && string_length(word) == 8 &&
            string_first_of((string_address) "aBcm", word[6]) &&
            string_first_of((string_address) "aBcmt", word[7]))
        {
                string_address named = find_value(word);
                b32 index = find_make('W');

                if (!named || index < 0 || find_bad)
                        return -1;

                find_node address_to made = find_nodes + index;

                made->comparison = word[6];
                made->mode = word[7];

                if (word[7] == 't')
                {
                        if (!file_moment_read(named, find_moment, address_of made->number))
                        {
                                string_format(log_error, "find: invalid date '%s'\n", named);
                                find_bad = true;
                                return -1;
                        }
                }
                else
                {
                        file_facts facts;
                        bipolar looked = file_look_code(AT_FDCWD, named, 0, address_of facts);

                        if (looked < 0)
                        {
                                string_format(log_error, "find: '%s': %s\n", named,
                                              file_reason(looked));
                                find_bad = true;
                                return -1;
                        }

                        file_moment address_to when =
                            word[7] == 'a'   ? address_of facts.accessed
                            : word[7] == 'B' ? address_of facts.created
                            : word[7] == 'c' ? address_of facts.changed
                                             : address_of facts.modified;

                        made->number = when->seconds;
                        made->extra = when->nanoseconds;
                }

                return index;
        }

        positive selected = string_table_find(
            word, find_predicates, sizeof(find_predicates[0]),
            array_count(find_predicates));
        if (selected == array_count(find_predicates))
        {
                //      findutils quotes the predicate the way it quotes
                //      everything else, with a backquote in front and an
                //      apostrophe behind.
                string_format(log_error, "find: unknown predicate `%s'\n", word);
                find_bad = true;
                return -1;
        }

        p8 sets = find_predicates[selected].sets;
        string_address value = sets & FIND_TAKES_VALUE ? find_value(word) : null;
        if ((sets & FIND_TAKES_VALUE) && !value)
                return -1;
        b32 index = find_make(find_predicates[selected].kind);
        if (index < 0)
                return -1;
        find_node address_to node = find_nodes + index;

        find_deepest |= (sets & FIND_SETS_DEEPEST) != 0;
        if (sets & FIND_SETS_DAYSTART)
        {
                find_daystart = true;
                find_moment = file_now() - file_now() % CLOCK_SECONDS_PER_DAY +
                              CLOCK_SECONDS_PER_DAY;
        }
        find_one_system |= (sets & FIND_SETS_ONE_SYSTEM) != 0;
        find_follow |= (sets & FIND_SETS_FOLLOW) != 0;
        find_has_action |= (sets & FIND_SETS_ACTION) != 0;

        switch (node->kind)
        {
        case 'R':
                node->text = value;
                node->comparison = find_is(word, "-iregex") ? 'i' : 0;
                if (!regex_compile(value, false, node->comparison == 'i', false,
                                   FIND_REGEX_POLICY))
                {
                        string_format(log_error,
                                      "find: invalid regular expression '%s'\n", value);
                        goto bad;
                }
                break;

        case 'A':
                node->number = word[1] == 'r' ? 'r' : word[1] == 'w' ? 'w' : 'x';
                break;

        case 'Y':
                node->number = string_get(value);
                break;

        case 'S':
        {
                file_facts facts;
                bipolar looked = file_look_code(AT_FDCWD, value, 0, address_of facts);

                if (looked < 0)
                {
                        string_format(log_error, "find: '%s': %s\n", value,
                                      file_reason(looked));
                        goto bad;
                }

                node->number = (b64)facts.inode;
                node->extra = (b64)file_device_key(facts.device_major,
                                                   facts.device_minor);
                break;
        }

        case 'l':
                // -fprintf names the file first and the format second.
                if (find_is(word, "-fprintf"))
                {
                        node->output = find_output_open(value);
                        value = find_value(word);

                        if (!value || find_bad)
                                return -1;
                }
                else
                        node->output = -1;

                node->text = value;
                break;

        case 'e':
                node->comparison = find_is(word, "-fprint0") ? '0' : 0;
                node->output = find_output_open(value);
                break;

        case '>':
        case '<':
                if (!file_unsigned_decimal(value, node->kind == '>'
                                                      ? address_of find_maximum
                                                      : address_of find_minimum))
                {
                        string_format(log_error, "find: invalid depth '%s'\n", value);
                        goto bad;
                }
                node->kind = 'v';
                break;

        case 'v':
                // -regextype names a dialect; only the one below is here,
                // and a name for it is taken and passed over.
                break;

        case 'x':
                node->mode = find_is(word, "-execdir")  ? 'd'
                             : find_is(word, "-ok")     ? 'o'
                             : find_is(word, "-okdir")  ? 'O'
                                                        : 0;
                node->number = (b64)find_at;

                /*
                        A semicolon always closes the command. A plus closes
                        it only where {} stands immediately in front, and is
                        an ordinary word anywhere else -- which is how
                        `-exec echo + ;` prints a plus for every entry, and
                        why `-exec echo +` runs off the end looking for a
                        terminator it never finds.
                */
                while (find_at < find_count)
                {
                        if (find_is(find_word(), ";"))
                                break;

                        if (find_is(find_word(), "+") &&
                            (b64)find_at > node->number &&
                            find_is(program_argument((b32)(find_at - 1)), "{}"))
                                break;

                        find_at++;
                }
                if (find_at >= find_count)
                {
                        string_format(log_error,
                                      "find: missing argument to `%s'\n", word);
                        goto bad;
                }
                node->extra = (b64)find_at;
                node->comparison = find_is(find_word(), "+") ? '+' : ';';
                find_at++;
                /*
                        Nothing at all between the option and its semicolon.
                        The reference names the terminator as the argument it
                        was handed where a command belonged. This used to
                        build an empty command and run it once per entry,
                        which on two hundred files took sixteen seconds to do
                        nothing.
                */
                if (node->extra == node->number)
                {
                        string_format(log_error,
                            "find: invalid argument `;' to `%s'\n", word);
                        goto bad;
                }
                if (node->comparison == '+')
                {
                        node->extra--;
                        if (!array_store_reserve(
                                find_batches, find_batch_room, find_batch_have,
                                find_batch_have + 1, 2))
                        {
                                shell_memory_failed = true;
                                log_error("find: out of memory while reading -exec\n", 0);
                                goto bad;
                        }
                        find_batches[find_batch_have] = (find_batch){.node = index};
                        node->unit = (b32)find_batch_have++;
                }
                break;

        case 'n':
        case 'N':
        case 'p':
        case 'P':
        case 'L':
                // Fold invariant patterns once, not on every visited entry.
                if (node->kind == 'N' || node->kind == 'P')
                        find_lowered(value, (p8 address_to)value);
                node->text = value;
                find_pattern_prepare(node);
                break;

        case 't':
                node->number = string_get(value);
                break;

        case 'm':
        {
                node->comparison = ' ';
                if (string_is(value, '-') || string_is(value, '/'))
                        node->comparison = string_get(value++);
                positive mode;
                if (!file_mode_of(value, 0, false, address_of mode))
                {
                        string_format(log_error, "find: invalid mode %s\n", value);
                        goto bad;
                }
                node->number = (b64)mode;
                break;
        }
        case 'z':
        case 'k':
        case 'i':
        case 'B':
        case 'T':
        {
                positive number;
                value = find_marked(value, address_of node->comparison);
                string_address after = value;
                if (!string_digits_checked(address_of after, 10, address_of number) ||
                    number > (positive)b64_max)
                        goto bad_number;
                if (node->kind == 'z')
                {
                        node->unit = string_get(after) ? string_get(after++) : 'b';
                        if (node->unit != 'b' && node->unit != 'c' &&
                            node->unit != 'w' && node->unit != 'k' &&
                            node->unit != 'M' && node->unit != 'G')
                                goto bad_number;
                }
                if (string_get(after))
                        goto bad_number;
                node->number = (b64)number;
                if (node->kind == 'T')
                {
                        node->unit = word[1];
                        node->extra = word[2] == 't' ? 86400 : 60;
                }
                break;
        }
        case 'g':
        case 'u':
        {
                bool group = node->kind == 'g';
                positive number;
                bipolar who = string_digits_exact(value, address_of number)
                                  ? (bipolar)number
                                  : (group ? file_group_id(value) : file_user_id(value));
                if (who < 0)
                {
                        string_format(log_error, "find: '%s' is not the name of a known %s\n",
                                      value, group ? "group" : "user");
                        goto bad;
                }
                node->number = (b64)who;
                break;
        }
        case 'w':
                node->comparison = find_is(word, "-anewer")   ? 'a'
                                   : find_is(word, "-cnewer") ? 'c'
                                                              : 'm';
                if (find_is(word, "-newermt"))
                {
                        if (!file_moment_read(value, find_moment, address_of node->number))
                        {
                                string_format(log_error, "find: invalid date '%s'\n", value);
                                goto bad;
                        }
                }
                else
                {
                        file_facts facts;
                        bipolar looked = file_look_code(AT_FDCWD, value, 0,
                                                        address_of facts);
                        if (looked < 0)
                        {
                                string_format(log_error, "find: '%s': %s\n", value,
                                              file_reason(looked));
                                goto bad;
                        }
                        node->number = facts.modified.seconds;
                        node->extra = facts.modified.nanoseconds;
                }
                break;
        }
        return index;

bad_number:
        string_format(log_error, "find: invalid number '%s' for %s\n", value, word);
bad:
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
                         find_is(word, (string_address) ",") ||
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

static b32 find_parse_or();

static b32 find_parse_comma()
{
        b32 left = find_parse_or();

        while (!find_bad && find_is(find_word(), (string_address) ","))
        {
                find_at++;

                b32 right = find_parse_or();

                if (find_bad || right < 0)
                {
                        find_bad = true;
                        return -1;
                }

                b32 node = find_make(',');

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
                log_error("find: out of memory while building -exec arguments\n", 0);
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

/* Literal substitution shared by find -exec and xargs -I. A null output
   measures the required bytes including NUL; zero means overflow. Writers
   use the same immutable inputs after reserving that measured size. */
static positive file_replace_literal(string_address word, string_address marker,
                                      positive mark, string_address replacement,
                                      positive replacement_length, p8 address_to into)
{
        positive length = 0;

        for (;;)
        {
                string_address hit = mark ? string_find(word, marker) : null;
                positive kept = hit ? (positive)(hit - word) : string_length(word);

                if (kept > positive_max - length)
                        return 0;
                if (into)
                        memory_copy_apart(into + length, word, kept);
                length += kept;
                if (!hit)
                        break;
                if (replacement_length > positive_max - length)
                        return 0;
                if (into)
                        memory_copy_apart(into + length, replacement, replacement_length);
                length += replacement_length;
                word = hit + mark;
        }

        if (length == positive_max)
                return 0;
        if (into)
                into[length] = end;
        return length + 1;
}

// The word -exec puts in place of {}: the path the walk built, or the name
// beside a dot for the -execdir shapes, which name a file in its own
// directory rather than from where find was started.
static string_address find_exec_subject(find_node address_to node, p8 address_to into)
{
        if (node->mode != 'd' && node->mode != 'O')
                return find_path;

        into[0] = '.';
        into[1] = '/';
        path_tail_copy(into + 2, FILE_PATH_MAX - 2, find_path);

        return (string_address)into;
}

// What -ok and -okdir ask before they run: the command, an ellipsis, the
// name, and a question mark. Anything but a yes is a no.
static bool find_exec_asked(find_node address_to node, string_address subject)
{
        string_format(log_error, "< %s ... %s > ? ",
                      program_argument((b32)node->number), subject);

        p8 answer[2];
        bipolar got = system_read_once(0, answer, 1);

        if (got != 1)
                return false;

        bool yes = answer[0] == 'y' || answer[0] == 'Y';

        while (answer[0] != '\n' && system_read_once(0, answer, 1) == 1)
                ;

        return yes;
}

static bool find_exec_once(find_node address_to node)
{
        positive used = 0;
        positive have = 0;
        p8 beside[FILE_PATH_MAX];
        string_address subject = find_exec_subject(node, beside);
        positive path_length = string_length(subject);
        positive words = (positive)(node->extra - node->number);

        if ((node->mode == 'o' || node->mode == 'O') &&
            !find_exec_asked(node, subject))
                return false;

        if (!shell_array_room(find_exec_words, find_exec_word_room, words + 1))
        {
                log_error("find: out of memory while building -exec arguments\n", 0);
                find_status = 1;
                return false;
        }

        positive needed = 0;

        for (b32 i = (b32)node->number; i < (b32)node->extra; i++)
        {
                positive length = file_replace_literal(program_argument(i), "{}", 2,
                                                        subject, path_length, null);

                if (!length || length > positive_max - needed)
                {
                        log_error("find: -exec arguments are too large\n", 0);
                        find_status = 1;
                        return false;
                }
                needed += length;
        }

        if (!shell_array_room(find_exec_text, find_exec_text_room, needed ? needed : 1))
        {
                log_error("find: out of memory while expanding -exec arguments\n", 0);
                find_status = 1;
                return false;
        }

        for (b32 i = (b32)node->number; i < (b32)node->extra; i++)
        {
                find_exec_words[have++] = find_exec_text + used;
                used += file_replace_literal(program_argument(i), "{}", 2,
                                              subject, path_length,
                                              find_exec_text + used);
        }

        find_exec_words[have] = null;

        return file_run(find_exec_words) == 0;
}

/*
        -printf, which is a format language of its own.

        Every directive answers with one field and nothing else, the way
        stat's -c does, and the ones that name a time are handed to date's
        own formatter so there is a single calendar here. A width or a
        precision in front of a directive is applied to whatever it wrote,
        which is why every field is rendered into a buffer first.
*/
static p8 find_field[FILE_PATH_MAX];
static positive find_field_used;

static fn find_field_write(address_any text, positive length)
{
        string_address from = text;

        if (!length)
                length = string_length(from);

        for (positive i = 0; i < length && find_field_used + 1 < sizeof(find_field); i++)
                find_field[find_field_used++] = from[i];

        find_field[find_field_used] = end;
}

static file_moment address_to find_moment_of(p8 letter)
{
        return letter == 'A'   ? address_of find_facts->accessed
               : letter == 'C' ? address_of find_facts->changed
               : letter == 'B' ? address_of find_facts->created
                               : address_of find_facts->modified;
}

// The path below the starting point, which is what %P is: the walk's own
// path with the root and the slash after it taken off.
static string_address find_below_root()
{
        positive length = string_length(find_root_path);
        string_address path = find_path;

        if (!string_compare_max(path, find_root_path, length))
        {
                path += length;

                while (string_is(path, '/'))
                        path++;
        }

        return path;
}

static bool find_printf_one(p8 letter, string_address format, positive address_to at)
{
        p8 name[FILE_PATH_MAX];

        switch (letter)
        {
        case 'p':
                find_field_write(find_path, 0);
                return true;
        case 'f':
                path_tail_copy(name, FILE_PATH_MAX, find_path);
                find_field_write(name, 0);
                return true;
        case 'h':
                path_head_copy(name, FILE_PATH_MAX, find_path);
                find_field_write(name, 0);
                return true;
        case 'H':
                find_field_write(find_root_path, 0);
                return true;
        case 'P':
                find_field_write(find_below_root(), 0);
                return true;
        case 'd':
                positive_to_string(find_field_write, find_depth);
                return true;
        case 'l':
                if (find_facts_ready() &&
                    (find_facts->mode & MODE_FORMAT) == MODE_LINK &&
                    file_link_text(find_path, name, FILE_PATH_MAX) >= 0)
                        find_field_write(name, 0);
                return true;
        case 'y':
        case 'Y':
        {
                file_facts through;
                positive mode = find_facts_ready() ? find_facts->mode : 0;

                if (letter == 'Y' && (mode & MODE_FORMAT) == MODE_LINK)
                {
                        if (!file_look_at(find_path, address_of through))
                        {
                                find_field_write("N", 1);
                                return true;
                        }

                        mode = through.mode;
                }

                p8 kind = !mode                            ? '?'
                          : (mode & MODE_FORMAT) == MODE_FILE ? 'f'
                                                              : file_kind_letter(mode);

                find_field_write(address_of kind, 1);
                return true;
        }
        }

        if (!find_facts_ready())
                return true;

        switch (letter)
        {
        case 's':
                positive_to_string(find_field_write, find_facts->size);
                return true;
        case 'b':
                positive_to_string(find_field_write, find_facts->blocks);
                return true;
        case 'k':
                positive_to_string(find_field_write,
                                   find_facts->blocks / 2 + (find_facts->blocks % 2 != 0));
                return true;
        case 'S':
        {
                // How sparse a file is: the room it takes over the room its
                // length would need, which is above one for a file with
                // holes and below one for a small file in a whole block.
                p64 room = find_facts->blocks * 512;
                p64 length = find_facts->size;

                if (!length)
                {
                        find_field_write(room ? "inf" : "0", 0);
                        return true;
                }

                positive_to_string(find_field_write, room / length);
                find_field_write(".", 1);
                positive_to_padded(find_field_write, room * 10 / length % 10, 1, '0', 0);
                return true;
        }
        case 'i':
                positive_to_string(find_field_write, find_facts->inode);
                return true;
        case 'n':
                positive_to_string(find_field_write, find_facts->hard_links);
                return true;
        case 'D':
                positive_to_string(find_field_write,
                                   file_device_key(find_facts->device_major,
                                                   find_facts->device_minor));
                return true;
        case 'm':
                find_field_write(name, positive_into_base(name, find_facts->mode & 07777, 8, false));
                return true;
        case 'M':
                file_mode_letters(name, find_facts->mode);
                find_field_write(name, 10);
                return true;
        case 'u':
                file_account_label(find_facts->owner, false, true, name);
                find_field_write(name, 0);
                return true;
        case 'U':
                positive_to_string(find_field_write, find_facts->owner);
                return true;
        case 'g':
                file_account_label(find_facts->group, true, true, name);
                find_field_write(name, 0);
                return true;
        case 'G':
                positive_to_string(find_field_write, find_facts->group);
                return true;
        case 'a':
        case 'c':
        case 't':
        {
                file_moment address_to when =
                    letter == 'a'   ? address_of find_facts->accessed
                    : letter == 'c' ? address_of find_facts->changed
                                    : address_of find_facts->modified;

                date_shape(find_field_write, when->seconds,
                           (string_address) "%a %b %e %H:%M:%S.");
                positive_to_padded(find_field_write, when->nanoseconds, 9, '0', 0);
                find_field_write("0 ", 2);
                date_shape(find_field_write, when->seconds, (string_address) "%Y");
                return true;
        }
        case 'A':
        case 'C':
        case 'T':
        {
                file_moment address_to when = find_moment_of(letter);
                p8 which = string_get(format + address_to at);

                if (!which)
                        return true;

                address_to at += 1;

                if (which == '@')
                {
                        bipolar_to_string(find_field_write, when->seconds);
                        find_field_write(".", 1);
                        positive_to_padded(find_field_write, when->nanoseconds, 9, '0', 0);
                        find_field_write("0", 1);
                        return true;
                }

                if (which == '+')
                {
                        date_shape(find_field_write, when->seconds,
                                   (string_address) "%Y-%m-%d+%H:%M:%S.");
                        positive_to_padded(find_field_write, when->nanoseconds, 9, '0', 0);
                        find_field_write("0", 1);
                        return true;
                }

                // The letters that end in seconds carry the fraction with
                // them, which is the one place find's clock is finer than
                // the calendar's own.
                if (which == 'S' || which == 'T' || which == 'X')
                {
                        date_shape(find_field_write, when->seconds,
                                   which == 'S' ? (string_address) "%S"
                                                : (string_address) "%H:%M:%S");
                        find_field_write(".", 1);
                        positive_to_padded(find_field_write, when->nanoseconds, 9, '0', 0);
                        find_field_write("0", 1);
                        return true;
                }

                p8 shape[3] = {'%', which, end};

                date_shape(find_field_write, when->seconds, shape);
                return true;
        }
        }

        return false;
}

static fn find_printf_walk(string_address format, bipolar handle)
{
        p8 line[FILE_PATH_MAX * 2];
        positive used = 0;

        for (positive at = 0; string_get(format + at) && used + 1 < sizeof(line);)
        {
                p8 byte = string_get(format + at++);

                if (byte == '\\')
                {
                        p8 next = string_get(format + at);
                        p8 named = next == 'n'    ? '\n'
                                   : next == 't'  ? '\t'
                                   : next == 'r'  ? '\r'
                                   : next == 'b'  ? '\b'
                                   : next == 'f'  ? '\f'
                                   : next == 'v'  ? '\v'
                                   : next == 'a'  ? 7
                                   : next == '\\' ? '\\'
                                   : next == '0'  ? 0
                                                  : 0xff;

                        if (next && named != 0xff)
                        {
                                line[used++] = named;
                                at++;
                                continue;
                        }

                        if (byte_is_digit(next))
                        {
                                positive number = 0;
                                positive digits = 0;

                                while (digits < 3 && byte_is_digit(string_get(format + at)))
                                {
                                        number = number * 8 +
                                                 (positive)(string_get(format + at) - '0');
                                        at++;
                                        digits++;
                                }

                                line[used++] = (p8)number;
                                continue;
                        }

                        line[used++] = byte;
                        continue;
                }

                if (byte != '%')
                {
                        line[used++] = byte;
                        continue;
                }

                if (string_is(format + at, '%'))
                {
                        line[used++] = '%';
                        at++;
                        continue;
                }

                // The flags, width and precision in front of the directive,
                // read the way a printf reads them.
                positive begin = at;
                bool left = false;

                while (string_first_of((string_address) "-+ #0", string_get(format + at)) &&
                       string_get(format + at))
                {
                        left |= string_is(format + at, '-');
                        at++;
                }

                positive width = 0;
                positive precision = positive_max;

                while (byte_is_digit(string_get(format + at)))
                        width = width * 10 + (positive)(string_get(format + at++) - '0');

                if (string_is(format + at, '.'))
                {
                        at++;
                        precision = 0;

                        while (byte_is_digit(string_get(format + at)))
                                precision = precision * 10 +
                                            (positive)(string_get(format + at++) - '0');
                }

                p8 letter = string_get(format + at);

                if (!letter)
                {
                        // A directive with nothing after it is written out
                        // as it stands, which is what the reference does.
                        for (positive i = begin - 1; i < at && used + 1 < sizeof(line); i++)
                                line[used++] = string_get(format + i);
                        break;
                }

                at++;

                if (letter == '{')
                {
                        while (string_get(format + at) && !string_is(format + at, '}'))
                                at++;
                        if (string_get(format + at))
                                at++;
                        continue;
                }

                find_field_used = 0;
                find_field[0] = end;

                if (!find_printf_one(letter, format, address_of at))
                {
                        // An unknown directive is written back as it stands.
                        for (positive i = begin - 1; i < at && used + 1 < sizeof(line); i++)
                                line[used++] = string_get(format + i);
                        continue;
                }

                positive length = find_field_used;

                if (precision != positive_max && precision < length)
                        length = precision;

                positive pad = width > length ? width - length : 0;

                if (!left)
                        while (pad-- && used + 1 < sizeof(line))
                                line[used++] = ' ';

                for (positive i = 0; i < length && used + 1 < sizeof(line); i++)
                        line[used++] = find_field[i];

                if (left)
                        while (pad-- && used + 1 < sizeof(line))
                                line[used++] = ' ';
        }

        if (handle >= 0)
                system_write_all((positive)handle, line, used);
        else
                log(line, used);
}

// -fprint and friends write into a file rather than onto the output, and
// the file is made once however many entries reach it.
static bipolar find_output_open(string_address path)
{
        bipolar handle = system_open_at_mode(AT_FDCWD, path, FILE_WRITE, 0666);

        if (handle < 0)
        {
                string_format(log_error, "find: '%s': %s\n", path, file_reason(handle));
                find_bad = true;
        }

        return handle;
}

// -readable, -writable and -executable ask the kernel the question the
// caller would have to ask by trying.
static bool find_access_holds(p8 which)
{
        positive mode = which == 'r' ? 4 : which == 'w' ? 2 : 1;

        return system_access_at(AT_FDCWD, find_path, mode) == 0;
}

// -regex and -iregex match the whole path, which is the one thing the
// shared matcher has to be told: a find is a match only when it covers
// every byte of the name.
static bool find_regex_holds(find_node address_to node, string_address text)
{
        if (!regex_compile(node->text, false, node->comparison == 'i', false,
                           FIND_REGEX_POLICY))
                return false;

        positive length = string_length(text);

        return regex_find(REGEX_EXACT_LONGEST, text, length, 0);
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

        case ',':
                find_true(node->left);
                return find_true(node->right);

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
                        return !node->number ||
                               ((positive)find_facts->mode & (positive)node->number) != 0;

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
        case 'W':
        {
                if (!find_facts_ready())
                        return false;

                p8 which = node->kind == 'W' ? node->comparison
                           : node->comparison ? node->comparison
                                              : 'm';
                file_moment address_to mine =
                    which == 'a'   ? address_of find_facts->accessed
                    : which == 'c' ? address_of find_facts->changed
                    : which == 'B' ? address_of find_facts->created
                                   : address_of find_facts->modified;

                if (mine->seconds != node->number)
                        return mine->seconds > node->number;

                return (b64)mine->nanoseconds > node->extra;
        }

        case 'd':
                file_line(find_path);
                return true;

        case '0':
                log(find_path, 0);
                log("\0", 1);
                return true;

        case 'R':
                return find_regex_holds(node, find_path);

        case 'A':
                return find_access_holds((p8)node->number);

        case 'S':
                if (!find_facts_ready())
                        return false;
                return (b64)find_facts->inode == node->number &&
                       (b64)file_device_key(find_facts->device_major,
                                            find_facts->device_minor) == node->extra;

        case 'Y':
        {
                file_facts through;
                positive mode = 0;

                if (!find_facts_ready())
                        return false;

                // -xtype asks what a link points at, and what a name that is
                // not a link is; a link to nothing is a link.
                if ((find_facts->mode & MODE_FORMAT) == MODE_LINK)
                        mode = file_look_at(find_path, address_of through)
                                   ? through.mode
                                   : find_facts->mode;
                else
                        mode = find_facts->mode;

                return find_type_holds((p8)node->number, mode);
        }

        case 'B':
                if (!find_facts_ready())
                        return false;
                return find_holds_count(node->comparison,
                                        (find_facts->accessed.seconds -
                                         find_facts->changed.seconds) /
                                            CLOCK_SECONDS_PER_DAY,
                                        node->number);

        case 'l':
                find_printf_walk(node->text, node->output);
                return true;

        case 'e':
                if (node->output >= 0)
                {
                        system_write_all((positive)node->output, find_path,
                                         string_length(find_path));
                        system_write_all((positive)node->output,
                                         node->comparison == '0' ? "" : "\n",
                                         node->comparison == '0' ? 0 : 1);

                        if (node->comparison == '0')
                                system_write_all((positive)node->output, "", 1);
                }
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
                        string_format(log_error, "find: cannot delete '%s': %s\n", find_path,
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

        if (directory && follow && depth <= FILE_MAX_DEPTH)
        {
                if (!find_facts_ready())
                        return;

                p64 device = file_device_key(facts.device_major, facts.device_minor);

                for (positive above = 0; above < depth; above++)
                        if (find_ancestors[above].device == device &&
                            find_ancestors[above].inode == facts.inode)
                        {
                                string_format(log_error,
                                              "find: File system loop detected; the following "
                                              "directory is part of the cycle: '%s'\n",
                                              path);
                                find_status = 1;
                                return;
                        }

                find_ancestors[depth].device = device;
                find_ancestors[depth].inode = facts.inode;
        }

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
                        string_format(log_error, "find: '%s': %s\n", path,
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
                                        string_format(log_error,
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
                                        string_format(log_error, "%s: %s '%s/%s': %s\n", (string_address) "find", (string_address) "cannot access", path, held, file_reason(-ERROR_NAME_TOO_LONG));
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
        find_daystart = false;
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
                else if (!string_compare_max(word, "-O", 2))
                {
                        // The optimisation level names how the reference
                        // orders its own tests, which is not an answer.
                }
                else if (find_is(word, (string_address) "-D"))
                {
                        if (index + 1 >= count)
                        {
                                log_error("find: -D needs a list of debug options\n", 0);
                                return 1;
                        }

                        index++;
                }
                else if (string_equals(word, (string_address) "--"))
                {
                        //      Two dashes close the leading options and are
                        //      not a root: the roots begin after them. Later
                        //      in the line the word is a predicate that does
                        //      not exist, which is what the reference calls
                        //      it, so this is only recognised here.
                        index++;
                        break;
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
        find_root = find_parse_comma();

        if (find_bad)
                return 1;

        if (find_at < count)
                return string_report(log_error, 1, "find: paths must precede expression: %s\n",
                              program_argument((b32)find_at));

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
        {
                find_root_path = (string_address) ".";
                find_walk((string_address) ".", (string_address) ".", 0, true,
                          AT_FDCWD, (string_address) ".", 0);
        }
        else
                for (positive i = roots_first; i < roots_last && !find_quit; i++)
                {
                        string_address root = program_argument((b32)i);
                        p8 name[FILE_PATH_MAX];

                        find_root_path = root;
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

/* A regular file with nothing in it has its own spelling in coreutils, and
   both the default layout and %F use it. */
static RETURNS_NONNULL string_address file_kind_told(
    file_facts address_to facts)
{
        if ((facts->mode & MODE_FORMAT) == MODE_FILE && !facts->size)
                return (string_address) "regular empty file";

        return file_kind_name(facts->mode);
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
                return log(file_kind_told(facts), 0);

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
                return bipolar_to_string(log, facts->accessed.seconds);

        case 'Y':
                return bipolar_to_string(log, facts->modified.seconds);

        case 'Z':
                return bipolar_to_string(log, facts->changed.seconds);

        case 'W':
                return bipolar_to_string(log, (facts->mask & STATX_BIRTH)
                                            ? facts->created.seconds
                                            : (b64)0);

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
        log(file_kind_told(facts), 0);

        log("\nDevice: ", 0);
        positive_to_string(log, facts->device_major);
        log(",", 1);
        positive_to_string(log, facts->device_minor);
        log("\tInode: ", 0);
        positive_into_string(text, facts->inode);
        string_to_field(log, text, 10, ' ', true);
        log("  Links: ", 0);
        positive_to_string(log, facts->hard_links);

        positive kind = facts->mode & MODE_FORMAT;

        if (kind == MODE_CHARACTER || kind == MODE_BLOCK)
        {
                log("     Device type: ", 0);
                positive_to_string(log, facts->rdev_major);
                log(",", 1);
                positive_to_string(log, facts->rdev_minor);
        }

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
                return string_report(log_error, 1, "%s: missing operand\n", (string_address) "stat");

        while (index < count)
        {
                string_address path = program_argument((b32)index++);

                if (stat_file_system)
                {
                        //      A dash is the standard input, and a stream has
                        //      no file system to describe; the reference says
                        //      so rather than looking for a file called -.
                        if (string_is(path, '-') && !string_get(path + 1))
                        {
                                log_error("stat: using '-' to denote standard input "
                                          "does not work in file system mode\n", 0);
                                stat_status = 1;
                                continue;
                        }

                        file_mount_facts facts;
                        bipolar done = system_call_2(syscall(statfs), (positive)path,
                                                     (positive)address_of facts);

                        if (done < 0)
                        {
                                string_format(log_error,
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
                // A lone dash is standard input, whatever it is open on.
                bool standard = string_is(path, '-') && !string_get(path + 1);
                bipolar looked = standard
                    ? file_look_code(0, (string_address) "", AT_EMPTY_PATH, address_of facts)
                    : file_look_code(AT_FDCWD, path,
                                     stat_follow ? 0 : AT_SYMLINK_NOFOLLOW,
                                     address_of facts);

                if (looked < 0)
                {
                        string_format(log_error, "stat: cannot statx '%s': %s\n",
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

        if (!made || system_failed(made))
        {
                shell_memory_failed = true;
                log_error("du: out of memory while tracking hard links\n", 0);
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

        log_error("du: hard-link table is unexpectedly full\n", 0);
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
                string_format(log_error, "du: cannot access '%s': %s\n", path,
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
                                log_error("du: tree is nested too deep\n", 0);
                                du_depth_broken = true;
                                du_status = 1;
                                break;
                        }

                        p8 under[FILE_PATH_MAX];

                        if (!file_path_join(under, path, entry->d_name))
                        {
                                string_format(log_error, "%s: %s '%s/%s': %s\n", (string_address) "du", (string_address) "cannot access", path, entry->d_name, file_reason(-ERROR_NAME_TOO_LONG));
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
                string_format(log_error, "du: cannot read directory '%s': %s\n", path,
                              file_reason(walk.handle));
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
                return string_report(log_error, false, "du: out of memory while reading exclude patterns\n");

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
                return string_report(log_error, 1, "du: summarizing conflicts with --all or --max-depth\n");

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
                        return string_report(log_error, 1, "du: invalid maximum depth\n");

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

typedef struct
{
        string_address heading;
        positive width;
} df_amount_column;

typedef struct
{
        file_mount_facts facts;
        bipolar reason;
        bool eligible;
        bool shown;
        bool measured;
        bool queried;
} df_sample;

static df_sample address_to df_samples;
static positive df_sample_room;
static positive address_to df_order;
static positive df_order_room;

static fn df_measure(storage_mount address_to mount, df_sample address_to sample)
{
        sample->queried = true;
        file_mount_facts address_to facts = address_of sample->facts;
        // Autofs queries can mount a filesystem as a side effect.
        bipolar answer = string_equals(mount->type, "autofs")
                             ? -ERROR_ACCESS
                             : system_call_2(syscall(statfs),
                                             (positive)mount->target,
                                             (positive)facts);
        if (answer < 0 && answer != -ERROR_ACCESS)
        {
                sample->reason = answer;
                return;
        }
        sample->measured = answer >= 0;
        sample->eligible = df_all || (sample->measured && facts->blocks);
}

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
                 file_mount_facts address_to facts, bool measured,
                 df_amount_column address_to columns)
{
        p64 values[3], size;
        p8 text[64];
        string_address dash = (string_address) "-";

        df_reading(facts, values, values + 1, values + 2,
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
                if (measured)
                        df_amount(text, values[column], size);
                df_column(measured ? text : dash, columns[column].width);
        }

        p64 wanted = values[1] + values[2];

        // A filesystem with nothing in it to fill has no proportion full, and
        // saying nought percent would be an answer where there is none.
        if (!measured || !wanted)
                df_column(dash, df_full_width);
        else
        {
                positive_to_padded(log,
                                   (positive)((values[1] * 100 + wanted - 1) / wanted),
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
            //      -v is accepted and does nothing, which is all the
            //      reference does with it too.
            .allowed = (string_address) "ahikPTv",
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
                return string_report(log_error, 1, "df: cannot read mount table\n");

        df_amount_column columns[] = {
            {df_inodes ? (string_address) "Inodes"
             : df_human ? (string_address) "Size"
             : df_posix ? (string_address) "1024-blocks"
                        : (string_address) "1K-blocks", 5},
            {df_inodes ? (string_address) "IUsed" : (string_address) "Used", 5},
            {df_inodes ? (string_address) "IFree"
             : df_human ? (string_address) "Avail"
                        : (string_address) "Available", 5},
        };
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

        for (positive column = 0; column < array_count(columns); column++)
                if (string_length(columns[column].heading) > columns[column].width)
                        columns[column].width = string_length(columns[column].heading);

        df_full_width = string_length(full_heading);

        bool filtering = first < count;
        positive showing = 0;
        positive ordered = 0;

        if (!array_store_reserve(df_samples, df_sample_room, 0, mounts.count,
                                 32) ||
            (filtering && !array_store_reserve(df_order, df_order_room, 0,
                                               count - first, 8)))
        {
                storage_mount_table_release(address_of mounts);
                return string_report(log_error, 1, "df: out of memory\n");
        }

        memory_fill(df_samples, 0, mounts.count * sizeof(*df_samples));

        for (positive at = 0; !filtering && at < mounts.count; at++)
        {
                storage_mount address_to mount = mounts.entry + at;
                df_sample address_to sample = df_samples + at;
                df_measure(mount, sample);
                if (sample->reason)
                {
                        /* A mount that will not answer statfs is left out of
                           the plain table without a word; only -a, or naming
                           it, makes the failure worth reporting. */
                        if (df_all)
                        {
                                string_format(log_error, "df: %s: %s\n", mount->target,
                                              file_reason(sample->reason));
                                df_failed = true;
                        }

                        continue;
                }

                sample->shown = sample->eligible;
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
                                string_format(log_error, "df: %s: %s\n", path,
                                              file_reason(answered));
                                df_failed = true;
                                continue;
                        }

                        /* The last record is the visible top of a stacked
                           mount, just as storage_mount_find_target chooses. */
                        for (positive at = mounts.count; at; at--)
                                if (mounts.entry[at - 1].id == wanted.mount_id)
                                {
                                        df_sample address_to named =
                                            df_samples + at - 1;

                                        if (!named->queried)
                                                df_measure(mounts.entry + at - 1,
                                                            named);

                                        if (named->reason)
                                        {
                                                string_format(
                                                    log_error, "df: %s: %s\n",
                                                    path,
                                                    file_reason(named->reason));
                                                df_failed = true;
                                        }
                                        else
                                        {
                                                named->shown = named->eligible;
                                                if (named->eligible)
                                                        df_order[ordered++] = at - 1;
                                        }

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

                showing++;

                if (string_length(device) > df_device_width)
                        df_device_width = string_length(device);

                if (sample->measured && string_length(type) > df_type_width)
                        df_type_width = string_length(type);

                if (!sample->measured)
                        continue;

                p64 values[3], size;

                df_reading(facts, values, values + 1, values + 2, address_of size);
                for (positive column = 0; column < array_count(columns); column++)
                {
                        positive length = df_amount(text, values[column], size);

                        if (length > columns[column].width)
                                columns[column].width = length;
                }
        }

        /* Named operands that all failed leave nothing to head, and the
           reference prints no lone heading over an empty table. */
        if (filtering && !showing)
        {
                storage_mount_table_release(address_of mounts);
                log_flush();
                return df_failed ? 1 : 0;
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

        for (positive column = 0; column < array_count(columns); column++)
                df_column(columns[column].heading, columns[column].width);
        log(full_heading, 0);
        log(" Mounted on\n", 0);

        for (positive row = 0; row < (filtering ? ordered : mounts.count); row++)
        {
                positive at = filtering ? df_order[row] : row;
                if (df_samples[at].shown)
                        df_row(mounts.entry[at].source,
                               mounts.entry[at].type,
                               mounts.entry[at].target,
                               address_of df_samples[at].facts,
                               df_samples[at].measured, columns);
        }

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
static p8 chmod_dereference_option;
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
        // thing pointed at -- unless -h was asked for, which aims at the
        // link itself. A link met under -R is not followed either: the walk
        // refuses to descend into one, and it must refuse to change through
        // one too, or chmod -R 000 over a tree with a link to /etc in it
        // changes /etc.
        bool operand = directory == AT_FDCWD;
        bool through = operand && chmod_dereference_option != 'h';
        bipolar looked = file_look_code(directory, name,
                                        through ? 0 : AT_SYMLINK_NOFOLLOW,
                                        address_of facts);

        if (looked < 0)
        {
                //      A symbolic link pointing at nothing was reached and
                //      followed, and the reference says that rather than
                //      that the name could not be found.
                if (through && looked == -ERROR_NO_ENTRY &&
                    file_look(directory, name, AT_SYMLINK_NOFOLLOW, address_of facts) &&
                    (facts.mode & MODE_FORMAT) == MODE_LINK)
                {
                        if (!chmod_quiet)
                                string_format(log_error,
                                              "chmod: cannot operate on dangling symlink '%s'\n",
                                              shown);

                        chmod_status = 1;
                        return;
                }

                // -v says what it could not do on the output stream as well,
                // because it reports on every file it was handed and not
                // only on the ones it changed.
                if (chmod_loud)
                        string_format(log, "'%s' could not be accessed\n", shown);

                if (!chmod_quiet)
                        string_format(log_error, "chmod: cannot access '%s': %s\n",
                                      shown, file_reason(looked));

                chmod_status = 1;
                return;
        }

        // Linux keeps no mode on a symlink, so a link that is not followed
        // is left exactly as it was -- and -v says so in as many words.
        if (!through && (facts.mode & MODE_FORMAT) == MODE_LINK)
        {
                if (chmod_loud)
                        string_format(log, "neither symbolic link '%s' nor referent "
                                           "has been changed\n", shown);

                return;
        }

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
                        string_format(log_error, "chmod: invalid mode: '%s'\n",
                                      chmod_specification);

                chmod_status = 1;
                return;
        }

        bipolar done = system_change_mode_at(directory, name, wanted);

        if (done < 0)
        {
                if (!chmod_quiet)
                        string_format(log_error, "chmod: changing permissions of '%s': %s\n",
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
                string_format(log_error, "chmod: %s: new permissions are %s, not %s\n",
                              shown, set + 1, expected + 1);
                chmod_status = 1;
        }
}

static p8 chmod_loudness_option;
static p8 chmod_traverse_option;

static const file_supersede chmod_supersedes[] = {
    {(string_address) "dh", address_of chmod_dereference_option},
    //      How much to say -- every mode, only the ones that moved -- and
    //      which links a -R walk goes through. Each row's last letter wins.
    {(string_address) "cv", address_of chmod_loudness_option},
    {(string_address) "HLP", address_of chmod_traverse_option},
    {null, null},
};

static const file_long chmod_longs[] = {
    {(string_address) "changes", 'c'},
    {(string_address) "dereference", 'd'},
    {(string_address) "no-dereference", 'h'},
    {(string_address) "no-preserve-root", 'N'},
    {(string_address) "preserve-root", 'N'},
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
        chmod_dereference_option = 'd';
        chmod_loudness_option = 0;
        chmod_traverse_option = 0;

        //      -H, -L and -P say which symbolic links a -R walk goes
        //      through. This walk goes through none of them, which is what
        //      -H (the default) and -P both ask for; -L is taken and does
        //      not change the walk, and the ledger records that.
        file_taking taking = {
            .program = (string_address) "chmod",
            .allowed = (string_address) "HLPRcfhvrwxXstugoaN",
            .valued = (string_address) "e",
            .optional = (string_address) "rwxXstugoa",
            .longs = chmod_longs,
            .supersedes = chmod_supersedes,
        };

        if (!file_take(address_of taking))
                return 1;

        positive first = taking.first;

        chmod_loud = chmod_loudness_option == 'v';
        chmod_changes = chmod_loudness_option == 'c';
        chmod_quiet = (taking.flags & FILE_FLAG('f')) != 0;

        //      A recursive walk told to follow links has to be told which
        //      ones, and this is asked before --reference's file is looked
        //      at, the way the reference asks it.
        if ((taking.flags & FILE_FLAG('R')) && (taking.flags & FILE_FLAG('d')) &&
            chmod_traverse_option != 'H' && chmod_traverse_option != 'L')
                return string_report(log_error, 1,
                                     "chmod: -R --dereference requires either -H or -L\n");

        string_address like = file_option_value(address_of taking, 'e');

        // --reference says the mode without spelling it, and takes the place
        // of the mode operand rather than standing beside it.
        if (like)
        {
                file_facts facts;
                bipolar looked = file_look_code(AT_FDCWD, like, 0, address_of facts);

                if (looked < 0)
                        return string_report(log_error, 1,
                                      "chmod: failed to get attributes of '%s': %s\n",
                                      like, file_reason(looked));

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
                return string_report(log_error, 1,
                                     "chmod: cannot combine mode and --reference options\n");

        chmod_surprising = minus_mode != null;

        if (minus_mode)
        {
                if (first >= count)
                        return string_report(log_error, 1, "%s: missing operand\n", (string_address) "chmod");

                chmod_specification = minus_mode;
        }
        else if (first >= count || (!chmod_referenced && first + 1 >= count))
                return string_report(log_error, 1, "%s: missing operand\n", (string_address) "chmod");
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
//      The spec exactly as it was written, which is what the reference puts
//      after "to" when it says what it could not do -- the name it was given
//      and not the name that number happens to have in the database.
static string_address chown_spec;
//      --from: change only where the owner is already this one. -1 in either
//      half is "whatever it is", so --from=:group tests the group alone.
static bipolar chown_from_user = -1;
static bipolar chown_from_group = -1;

static p8 chown_traverse_option;

static p8 chown_loudness_option;

static const file_supersede chown_supersedes[] = {
    {(string_address) "dh", address_of chown_dereference_option},
    {(string_address) "HLP", address_of chown_traverse_option},
    //      How much to say: every change, only the changes, or nothing.
    //      The last of the two is the one that answers.
    {(string_address) "cv", address_of chown_loudness_option},
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
                //      A spec that names neither half asked for nothing, and
                //      the reference says so without naming what was kept --
                //      in chown's words, whichever of the two was called.
                if (chown_user < 0 && chown_group < 0)
                {
                        string_format(log, "ownership of '%s' retained\n", shown);
                        return;
                }

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
        bipolar looked = file_look_code(directory, name, through, address_of facts);
        bool known = looked == 0;

        // A name that is not there is not an ownership that would not
        // change: the reference says it could not look at it.
        if (looked == -ERROR_NO_ENTRY)
        {
                if (chown_loud)
                        string_format(log, chown_groups_only
                                               ? "failed to change group of '%s' to %s\n"
                                               : "failed to change ownership of '%s' to %s\n",
                                      shown, chown_spec);

                if (!chown_quiet)
                        string_format(log_error, "%s: cannot access '%s': %s\n",
                                      chown_program, shown, file_reason(looked));

                chown_status = 1;
                return;
        }

        //      --from names the ownership a file must already have. One that
        //      has another is left alone, and -v calls that a retention
        //      rather than a change.
        if (known && ((chown_from_user >= 0 && facts.owner != (positive)chown_from_user) ||
                      (chown_from_group >= 0 && facts.group != (positive)chown_from_group)))
        {
                chown_said(shown, address_of facts, false);
                return;
        }

        bipolar done = system_change_owner_at(
            directory, name, chown_user, chown_group, through);

        if (done < 0)
        {
                if (chown_loud)
                {
                        p8 before[FILE_PATH_MAX];

                        if (known)
                        {
                                chown_who(facts.owner, facts.group, before);
                                string_format(log, chown_groups_only
                                                       ? "failed to change group of '%s' from %s to %s\n"
                                                       : "failed to change ownership of '%s' from %s to %s\n",
                                              shown, before, chown_spec);
                        }
                        else
                                string_format(log, chown_groups_only
                                                       ? "failed to change group of '%s' to %s\n"
                                                       : "failed to change ownership of '%s' to %s\n",
                                              shown, chown_spec);
                }

                if (!chown_quiet)
                        string_format(log_error,
                                      chown_groups_only
                                          ? "%s: changing group of '%s': %s\n"
                                          : "%s: changing ownership of '%s': %s\n",
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
    {(string_address) "from", 'F'},
    {(string_address) "no-preserve-root", 'N'},
    {(string_address) "preserve-root", 'N'},
    {(string_address) "dereference", 'd'},
    {(string_address) "no-dereference", 'h'},
    {(string_address) "quiet", 'f'},
    {(string_address) "recursive", 'R'},
    {(string_address) "reference", 'e'},
    {(string_address) "silent", 'f'},
    {(string_address) "verbose", 'v'},
    {null, 0},
};

/*
        A USER[:GROUP] spec, read the way chown reads its operand: an empty
        half means "leave this one alone", "user:" is a spec this image
        cannot complete, and a half that names nobody is refused with the
        whole spec quoted, which is how the reference quotes it.
*/
static bool chown_spec_read(string_address who, bipolar address_to user,
                            bipolar address_to group)
{
        p8 name[FILE_NAME_MAX];
        positive length = 0;

        while (string_get(who + length) && !string_is(who + length, ':') &&
               !string_is(who + length, '.') && length + 1 < FILE_NAME_MAX)
        {
                name[length] = string_get(who + length);
                length++;
        }

        name[length] = end;

        string_address rest = null;

        if (string_is(who + length, ':') || string_is(who + length, '.'))
                rest = who + length + 1;

        //      "user:" names a group by that user's own login group, which
        //      needs a database this image has not got. A colon with nothing
        //      on either side of it names nobody in particular, which is
        //      what --from= means and is not a refusal.
        if (length && rest && !string_get(rest))
                return string_report(log_error, false, "%s: invalid spec: '%s'\n",
                                     chown_program, who);

        if (length > 0)
        {
                positive number;

                address_to user = string_digits_exact(name, address_of number)
                                      ? (bipolar)number
                                      : file_user_id(name);

                if (address_to user < 0)
                        return string_report(log_error, false, "%s: invalid user: '%s'\n",
                                             chown_program, who);
        }

        if (rest && string_get(rest))
        {
                positive number;

                address_to group = string_digits_exact(rest, address_of number)
                                       ? (bipolar)number
                                       : file_group_id(rest);

                if (address_to group < 0)
                        return string_report(log_error, false, "%s: invalid group: '%s'\n",
                                             chown_program, who);
        }

        return true;
}

static fn chown_paths(positive first, positive count)
{
        file_change_after_contents = true;
        file_change_paths(first, count, (chown_flags & FILE_FLAG('R')) != 0,
                          chown_program, address_of chown_status, chown_one);
        file_change_after_contents = false;
}

static b32 file_chown_common(string_address program, bool groups_only)
{
        positive count = (positive)program_argument_count();
        chown_user = -1;
        chown_group = -1;
        chown_from_user = -1;
        chown_from_group = -1;
        chown_status = 0;
        chown_dereference_option = 'd';
        chown_traverse_option = 0;
        chown_loudness_option = 0;
        chown_program = program;
        chown_groups_only = groups_only;
        chown_spec = (string_address) "";

        file_taking taking = {
            .program = program,
            .allowed = (string_address) "HLPRcfhvN",
            .valued = (string_address) "eF",
            .longs = chown_longs,
            .supersedes = chown_supersedes,
        };

        if (!file_take(address_of taking))
                return 1;

        //      --from's spec is read first: the reference is a getopt loop
        //      and reads the word where the option is, before it asks
        //      anything about the walk it was told to make.
        string_address from = file_option_value(address_of taking, 'F');

        if (from && !chown_spec_read(from, address_of chown_from_user,
                                     address_of chown_from_group))
                return 1;

        //      A recursive walk that was told to follow links has to be
        //      told which ones, and the last of -H, -L and -P is the one
        //      that answers: -P, or none at all, leaves the question open
        //      and the reference refuses the pair.
        if ((taking.flags & FILE_FLAG('R')) && (taking.flags & FILE_FLAG('d')) &&
            chown_traverse_option != 'H' && chown_traverse_option != 'L')
                return string_report(log_error, 1,
                                     "%s: -R --dereference requires either -H or -L\n",
                                     program);

        positive first = taking.first;

        chown_flags = taking.flags;
        chown_loud = chown_loudness_option == 'v';
        chown_changes = chown_loudness_option == 'c';
        chown_quiet = (taking.flags & FILE_FLAG('f')) != 0;

        string_address like = file_option_value(address_of taking, 'e');

        //      The operands are counted before the reference file is looked
        //      at: a line with nothing to change is a missing operand
        //      whatever --reference named, and one that is a spec and no
        //      file names the spec it stopped after.
        if (first >= count || (!like && first + 1 >= count))
        {
                if (first >= count)
                        return string_report(log_error, 1, "%s: missing operand\n", program);

                return string_report(log_error, 1, "%s: missing operand after '%s'\n",
                                     program, program_argument((b32)(count - 1)));
        }

        if (like)
        {
                file_facts facts;
                bipolar looked = file_look_code(AT_FDCWD, like, 0, address_of facts);

                if (looked < 0)
                        return string_report(log_error, 1,
                                      "%s: failed to get attributes of '%s': %s\n",
                                      program, like, file_reason(looked));

                if (!groups_only)
                        chown_user = (bipolar)facts.owner;

                chown_group = (bipolar)facts.group;
        }

        static p8 chown_reference_spec[FILE_PATH_MAX];

        if (like)
        {
                //      There is no written spec behind --reference, so the
                //      one -v quotes is the reference file's own ownership.
                chown_who(chown_user < 0 ? 0 : (positive)chown_user,
                          chown_group < 0 ? 0 : (positive)chown_group,
                          chown_reference_spec);
                chown_spec = chown_reference_spec;
                chown_paths(first, count);

                return chown_status;
        }

        string_address who = program_argument((b32)first++);

        chown_spec = who;

        if (groups_only)
        {
                positive number;

                // An empty group is no group: nothing changes, and the
                // reference chgrp answers 0 to it.
                chown_group = !string_get(who)              ? -1
                              : string_digits_exact(who, address_of number)
                                  ? (bipolar)number
                                  : file_group_id(who);

                if (chown_group < 0 && string_get(who))
                {
                        string_format(log_error, "%s: invalid group: '%s'\n", program, who);
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

        // "user:" names a group by the user's own login group, which needs a
        // password database this one has not got; the reference refuses a
        // spec it cannot complete rather than changing only the user. A
        // lone colon names neither half and asks for nothing.
        if (group && !string_get(group) && length)
        {
                string_format(log_error, "%s: invalid spec: '%s'\n", program, who);
                return 1;
        }

        if (length > 0)
        {
                positive number;

                chown_user = string_digits_exact(user, address_of number)
                                 ? (bipolar)number
                                 : file_user_id(user);

                if (chown_user < 0)
                {
                        string_format(log_error, "%s: invalid user: '%s'\n", program, who);
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
                        string_format(log_error, "%s: invalid group: '%s'\n", program, who);
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

// The backup a destination gets before it is written over, shared by cp,
// mv, ln and install and defined where the copying is.
static bool file_backup_made(string_address program, string_address destination);
static bool file_backup_taken(file_taking address_to taking, string_address program);
static bool file_targets_told(string_address program);

// ln ------------------------------------------------------------
// ln [-s] [-f] TARGET [NAME], and ln [-s] [-f] TARGET... DIRECTORY.
static bool ln_symbolic;
static bool ln_directories;
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
                        return string_report(log_error, false,
                                      "ln: cannot make relative target '%s': File name too long\n",
                                      target);

                target = relative;
        }

        // A directory has one name and a hard link would give it two; the
        // reference looks at what it was asked for before it tries, so a
        // name that is not there is named rather than the link that failed.
        if (!ln_symbolic)
        {
                file_facts source;
                bipolar looked = file_look_code(AT_FDCWD, target,
                                                ln_through ? 0 : AT_SYMLINK_NOFOLLOW,
                                                address_of source);

                if (looked < 0)
                {
                        string_format(log_error, "ln: failed to access '%s': %s\n", target,
                                      file_reason(looked));
                        return false;
                }

                //      -d, -F and --directory ask for the link to be
                //      attempted anyway, and the kernel is then the one that
                //      refuses it -- with its own words and not these.
                if (!ln_directories &&
                    (source.mode & MODE_FORMAT) == MODE_DIRECTORY)
                {
                        string_format(log_error, "ln: %s: hard link not allowed for directory\n",
                                      target);
                        return false;
                }
        }

        if (ln_ask && file_exists(AT_FDCWD, name) &&
            !file_ask((string_address) "ln", (string_address) "replace", name))
                return false;

        if (!file_backup_made((string_address) "ln", name))
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
                        return string_report(log_error, false,
                                      "ln: failed to access '%s': %s\n", target,
                                      file_reason(looked));

                if (file_look(AT_FDCWD, name, AT_SYMLINK_NOFOLLOW,
                              address_of destination) &&
                    file_same_identity(address_of source,
                                       address_of destination))
                        return string_report(log_error, false,
                                      "ln: '%s' and '%s' are the same file\n",
                                      target, name);
        }

        if (ln_force || ln_ask)
                system_remove_at(AT_FDCWD, name, 0);

        bipolar done;

        if (ln_symbolic)
                done = system_symbolic_link_at(target, AT_FDCWD, name);
        else
                done = system_link_at(AT_FDCWD, target, AT_FDCWD, name,
                                      ln_through ? AT_SYMLINK_FOLLOW : 0);

        //      A hard link that could not be made names both ends; a
        //      symbolic one names only the name it was to be given, which is
        //      how the reference writes each of them.
        if (done < 0)
                return ln_symbolic
                    ? string_report(log_error, false,
                                    "ln: failed to create symbolic link '%s': %s\n",
                                    name, file_reason(done))
                    : string_report(log_error, false,
                                    "ln: failed to create hard link '%s' => '%s': %s\n",
                                    name, target, file_reason(done));

        if (ln_loud)
                string_format(log, ln_symbolic ? "'%s' -> '%s'\n" : "'%s' => '%s'\n",
                              name, target);

        return true;
}

static const file_long ln_longs[] = {
    {(string_address) "backup", 'B'},
    {(string_address) "directory", 'd'},
    {(string_address) "force", 'f'},
    {(string_address) "suffix", 'S'},
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

        file_targets_begin();

        file_taking taking = {
            .program = (string_address) "ln",
            //      -d, -F and --directory ask for a hard link to a
            //      directory, which the kernel gives only to a privileged
            //      caller; taken and left to the link call to refuse.
            .allowed = (string_address) "bdfFiLnPrsStTv",
            .valued = (string_address) "tS",
            .long_optional = (string_address) "B",
            .longs = ln_longs,
            .seen = file_target_seen,
            .supersedes = ln_supersedes,
        };

        if (!file_take(address_of taking))
                return 1;

        if (!file_backup_taken(address_of taking, (string_address) "ln"))
                return 1;

        if (!file_targets_told((string_address) "ln"))
                return 1;

        positive flags = taking.flags;
        positive first = taking.first;

        ln_symbolic = (flags & FILE_FLAG('s')) != 0;
        ln_directories = (flags & (FILE_FLAG('d') | FILE_FLAG('F'))) != 0;
        ln_force = ln_collision_option == 'f';
        ln_ask = ln_collision_option == 'i';
        ln_loud = (flags & FILE_FLAG('v')) != 0;
        ln_relative = (flags & FILE_FLAG('r')) != 0;

        if (ln_relative && !ln_symbolic)
        {
                log_error("ln: cannot do --relative without --symbolic\n", 0);
                return 1;
        }

        // -L makes a hard link to what a symbolic target points at rather
        // than to the link, which is the one thing -L and -P are about.
        ln_through = ln_dereference_option == 'L';

        if (first >= count)
        {
                log_error("ln: missing file operand\n", 0);
                return 1;
        }

        string_address into = file_option_value(address_of taking, 't');
        bool alone = (flags & FILE_FLAG('T')) != 0;

        if (into && alone)
                return string_report(log_error, 1, "ln: cannot combine --target-directory and --no-target-directory\n");

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
                        file_facts facts;
                        bipolar looked = file_look_code(AT_FDCWD, last,
                                                        through ? 0 : AT_SYMLINK_NOFOLLOW,
                                                        address_of facts);

                        return string_report(log_error, 1, "ln: target '%s': %s\n", last,
                                             file_reason(looked < 0 ? looked
                                                                    : -ERROR_NOT_DIRECTORY));
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
                        string_format(log_error, "%s: %s '%s/%s': %s\n", (string_address) "ln", (string_address) "failed to create link", last, tail, file_reason(-ERROR_NAME_TOO_LONG));
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

        if (file_simple_operand_count > wanted)
                string_format(log_error, "%s: extra operand '%s'\n", program,
                              file_simple_operand_list[wanted]);
        else if (file_simple_operand_count)
                string_format(log_error, "%s: missing operand after '%s'\n", program,
                              file_simple_operand_list[file_simple_operand_count - 1]);
        else
                string_format(log_error, "%s: missing operand\n", program);

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
                return string_report(log_error, 1,
                              "link: cannot create link '%s' to '%s': %s\n",
                              target, source, file_reason(answer));
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
                return string_report(log_error, 1, "unlink: cannot unlink '%s': %s\n",
                              path, file_reason(answer));
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
                return string_report(log_error, false, "namei: out of memory while recording path\n");
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
                log_error("namei: out of memory while recording path\n", 0);
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
                        return string_report(log_error, false, "namei: %s: %s\n",
                                      namei_operand,
                                      file_reason(-ERROR_NAME_TOO_LONG));

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
                                        return string_report(log_error, false,
                                                      "namei: %s: exceeded limit of symlinks\n",
                                                      namei_operand);

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
                        writer_fill(log, 1 + row->depth * 2, ' ');

                positive metadata = modes ? 10 : 1;
                if (owners)
                        metadata += 1 + user_width + 1 + group_width;

                if (!row->known)
                        writer_fill(log, metadata, ' ');
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

                writer_fill(log, row->known ? 1 : 2, ' ');
                if (vertical)
                        writer_fill(log, row->depth * 2, ' ');
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
        if (file_meta(address_of taking, "[options] pathname...", log))
        {
                log_flush();
                return 0;
        }
        if (!file_operand_count)
                return string_report(log_error, 1, "namei: pathname argument is missing\n");

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

                // An empty pathname names nothing; the reference says
                // nothing about it and answers 1.
                if (!string_get(namei_operand))
                {
                        status = 1;
                        continue;
                }

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

//      Every directory of one kind leaves the list, so that the ones -B, -M
//      or -S names go on the end of it. The list's order is the order the
//      search walks and the order -l prints, which is why it is kept.
static fn whereis_forget_kind(positive kind)
{
        positive kept = 0;

        for (positive i = 0; i < whereis_directory_count; i++)
                if (whereis_directories[i].kind != kind)
                        whereis_directories[kept++] = whereis_directories[i];

        whereis_directory_count = kept;
}

static bool whereis_add_directory(positive kind, string_address path)
{
        p8 resolved[FILE_PATH_MAX];
        file_facts facts;

        //      The reference asks whether it may read the directory before
        //      it asks what the directory is, and passes over one it may not.
        if (!path || system_access_at(AT_FDCWD, path, 4) < 0 ||
            !file_real(path, resolved) ||
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
                return string_report(log_error, false,
                              "whereis: out of memory while recording search paths\n");

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
            (string_address)"lzo",  (string_address)"Z"};

        return string_table_find(suffix, names, sizeof(names[0]),
                                  array_count(names)) < array_count(names);
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

static bool whereis_scan(positive want, string_address query, bool glob)
{
        for (positive directory_at = 0;
             directory_at < whereis_directory_count; directory_at++)
        {
                whereis_directory address_to directory =
                    whereis_directories + directory_at;
                positive kind = directory->kind;

                if (!(want & ((positive)1 << kind)))
                        continue;

                file_walk walk;
                if (!file_walk_open(address_of walk, AT_FDCWD,
                                    directory->path))
                        continue;

                struct linux_dirent64 address_to entry;
                while ((entry = file_walk_next(address_of walk)))
                {
                        //      . and .. are entries like any other here: the
                        //      reference reads the directory and compares
                        //      names, so whereis '' answers with every dot.
                        if (!whereis_name_matches(kind, query, entry->d_name,
                                                  glob))
                                continue;
                        if (!shell_array_room(whereis_matches,
                                              whereis_match_room,
                                              whereis_match_count + 1))
                        {
                                file_walk_close(address_of walk);
                                return string_report(log_error, false,
                                              "whereis: out of memory while recording matches\n");
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

/*
        whereis reads its line the way util-linux reads it, which is not the
        way anything else here does.

        There is no option parser: the words are walked once, a word that is
        not an option is a name and is looked up where the walk reaches it, so
        the categories asked for so far are the ones that name is looked for
        in and a category named after it belongs to the next name. -B, -M and
        -S each take every following word that is not an option, which is why
        they must be written apart from their value and why the walk, not a
        parser, has to consume them; the reference insists a -f follows and
        says so at the end of the run, after everything else it was going to
        do.

        The directories are one list in one order -- the built-in binaries and
        PATH, then the manuals and MANPATH, then the sources -- and a search
        walks that list rather than each category in turn. -B empties the
        binaries out of it and puts the named ones on the end, so what was
        first is then last, and both the search and -l say so.
*/
#define WHEREIS_WANT(kind) ((positive)1 << (kind))
#define WHEREIS_WANT_ALL (WHEREIS_WANT(WHEREIS_BINARY) | \
                          WHEREIS_WANT(WHEREIS_MANUAL) | \
                          WHEREIS_WANT(WHEREIS_SOURCE))

static COLD b32 whereis_bad_usage()
{
        log_error("whereis: bad usage\n", 0);
        log_error("Try 'whereis --help' for more information.\n", 0);

        return 1;
}

static bool whereis_lookup(string_address name, positive want, bool glob,
                           bool unusual)
{
        string_address query = file_last_component(name);

        whereis_match_count = 0;

        if (!whereis_scan(want, query, glob))
                return false;

        if (unusual && whereis_match_count <= 1)
                return true;

        string_format(log, "%s:", query);

        for (positive i = 0; i < whereis_match_count; i++)
        {
                whereis_match address_to match = whereis_matches + i;

                string_format(log, " %s/%s",
                              whereis_directories[match->directory].path,
                              match->name);
        }

        string_format(log, "\n");

        return true;
}

static b32 file_whereis()
{
        positive count = (positive)program_argument_count();
        positive want = WHEREIS_WANT_ALL;
        bool resetable = false;
        bool unusual = false;
        bool glob = false;
        bool missing_f = false;

        whereis_directory_count = 0;
        whereis_match_count = 0;

        if (count <= 1)
        {
                log_error("whereis: not enough arguments\n", 0);
                log_error("Try 'whereis --help' for more information.\n", 0);

                return 1;
        }

        if (whereis_long(program_argument(1), (string_address) "help"))
        {
                string_format(log,
                              "Usage: whereis [options] NAME...\n"
                              "  -b, -m, -s       search binaries, manuals, sources\n"
                              "  -B, -M, -S DIR... -f  set category search paths\n"
                              "  -u unusual  -g glob  -l list paths\n");
                log_flush();
                return 0;
        }

        if (whereis_long(program_argument(1), (string_address) "version"))
        {
                string_format(log, "whereis from dawning-kit\n");
                log_flush();
                return 0;
        }

        for (positive kind = 0; kind < WHEREIS_KINDS; kind++)
                if (!whereis_add_defaults(kind))
                        return 1;

        for (positive i = 1; i < count; i++)
        {
                string_address word = program_argument((b32)i);
                positive was = i;

                if (word[0] != '-')
                {
                        if (!whereis_lookup(word, want, glob, unusual))
                                return 1;

                        resetable = true;
                        continue;
                }

                for (positive at = 1; word[at]; at++)
                {
                        p8 option = word[at];

                        if (option == 'f')
                                missing_f = false;
                        else if (option == 'u')
                        {
                                unusual = true;
                                missing_f = false;
                        }
                        else if (option == 'B' || option == 'M' || option == 'S')
                        {
                                if (word[at + 1])
                                        return whereis_bad_usage();

                                positive kind = option == 'B' ? WHEREIS_BINARY
                                                : option == 'M' ? WHEREIS_MANUAL
                                                                : WHEREIS_SOURCE;

                                whereis_forget_kind(kind);

                                for (positive next = ++i; next < count; next++)
                                {
                                        string_address dir =
                                            program_argument((b32)next);

                                        if (dir[0] == '-')
                                                break;

                                        if (!whereis_add_directory(kind, dir))
                                                return 1;

                                        i = next;
                                }

                                missing_f = true;
                        }
                        else if (option == 'b' || option == 'm' || option == 's')
                        {
                                positive kind = option == 'b' ? WHEREIS_BINARY
                                                : option == 'm' ? WHEREIS_MANUAL
                                                                : WHEREIS_SOURCE;

                                if (resetable)
                                {
                                        want = WHEREIS_WANT_ALL;
                                        resetable = false;
                                }

                                want = want == WHEREIS_WANT_ALL
                                           ? WHEREIS_WANT(kind)
                                           : want | WHEREIS_WANT(kind);
                                missing_f = false;
                        }
                        else if (option == 'l')
                        {
                                static const string_address label[] = {
                                    (string_address) "bin",
                                    (string_address) "man",
                                    (string_address) "src"};

                                for (positive at_dir = 0;
                                     at_dir < whereis_directory_count; at_dir++)
                                        string_format(log, "%s: %s\n",
                                                      label[whereis_directories[at_dir].kind],
                                                      whereis_directories[at_dir].path);
                        }
                        else if (option == 'g')
                                glob = true;
                        else if (option == 'V')
                        {
                                string_format(log, "whereis from dawning-kit\n");
                                log_flush();
                                return 0;
                        }
                        else if (option == 'h')
                        {
                                string_format(log,
                                              "Usage: whereis [options] NAME...\n"
                                              "  -b, -m, -s       search binaries, manuals, sources\n"
                                              "  -B, -M, -S DIR... -f  set category search paths\n"
                                              "  -u unusual  -g glob  -l list paths\n");
                                log_flush();
                                return 0;
                        }
                        else
                                return whereis_bad_usage();

                        //      A letter that ate words of its own ends the
                        //      cluster it was written in.
                        if (was < i)
                                break;
                }
        }

        log_flush();

        //      Said once, at the end, after every name has been answered.
        if (missing_f)
                return string_report(log_error, 1, "whereis: option -f is missing\n");

        return 0;
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
                return string_report(log_error, 1, "%s: missing operand\n", (string_address) "readlink");

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
                        p8 policy = readlink_canonical_option == 'm'
                                        ? FILE_RESOLVE_UNRESOLVED
                                    : readlink_canonical_option == 'e'
                                        ? FILE_RESOLVE_DIRECTORIES
                                        : FILE_RESOLVE_DIRECTORIES |
                                              FILE_RESOLVE_FINAL_MISSING;
                        bool valid = file_resolve_as(path, answer, true,
                                                     policy);

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
                                        string_format(log_error,
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
                                        string_format(log_error, "readlink: %s: %s\n", path,
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
                return string_report(log_error, 1, "%s: missing operand\n", (string_address) "basename");

        if (!many && index + 1 < count)
                suffix = program_argument((b32)(index + 1));

        if (!many && index + 2 < count)
        {
                string_format(log_error, "basename: extra operand '%s'\n",
                              program_argument((b32)(index + 2)));
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
                return string_report(log_error, 1, "%s: missing operand\n", (string_address) "dirname");

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
                return string_report(log_error, 1, "%s: missing operand\n", (string_address) "realpath");

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
                return string_report(log_error, 1, "realpath: relative directory is empty\n");

        // Both directories are made canonical before anything is said
        // relative to them, or /tmp/./x would not look like /tmp/x.
        if (base && file_real(base, base_real))
                base = base_real;

        if (!against)
                against = base;
        else if (file_real(against, against_real))
                against = against_real;

        p8 policy = allow_missing ? FILE_RESOLVE_UNRESOLVED
                    : FILE_RESOLVE_DIRECTORIES |
                          (written_name ? FILE_RESOLVE_MISSING_TAIL
                                        : FILE_RESOLVE_FINAL_MISSING);

        while (first < count)
        {
                string_address path = program_argument((b32)first++);
                p8 answer[FILE_PATH_MAX];
                p8 scratch[FILE_PATH_MAX];
                string_address source = path;
                string_address reason = (string_address) "Invalid argument";

                /*
                        -L takes the .. out of the name before any link in it
                        is followed, so link/.. is where the name was written
                        and not where the link went. That is two passes: the
                        lexical one, and then the real one over what it left.
                */
                if (logical && !written_name)
                {
                        if (!file_resolve_as(path, scratch, false, policy))
                                goto failed;
                        source = scratch;
                }

                if (!file_resolve_as(source, answer, !written_name, policy))
                        goto failed;

                /* -s preserves the spelling of links, but it does not hide
                   kernel traversal failures.  Default -E alone tolerates
                   ENOENT; -e still requires the complete referent. */
                if (written_name && !allow_missing)
                {
                        file_facts facts;
                        bipolar looked = file_look_code(AT_FDCWD, path, 0,
                                                        address_of facts);

                        if (looked < 0 &&
                            (realpath_missing_option == 'e' ||
                             looked != -ERROR_NO_ENTRY))
                        {
                                reason = file_reason(looked);
                                goto failed;
                        }
                }

                path_head_copy(scratch, FILE_PATH_MAX, answer);

                if (!written_name &&
                    ((!allow_missing && !file_is_directory_through(scratch)) ||
                     (realpath_missing_option == 'e' &&
                      !file_exists(AT_FDCWD, answer))))
                {
                        reason = (string_address) "No such file or directory";
                        goto failed;
                }

                // --relative-base names where the shorthand stops being worth
                // it: a path outside that directory is said in full.
                if (against && (!base || realpath_under(base, answer)))
                {
                        if (!realpath_relative(against, answer, scratch))
                        {
                                reason = (string_address) "File name too long";
                                goto failed;
                        }

                        file_written(scratch, zero);
                }
                else
                        file_written(answer, zero);
                continue;

failed:
                if (!quiet)
                        string_format(log_error, "realpath: %s: %s\n", path, reason);
                status = 1;
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

// The reference's shapes: a stat failure is "name: reason", the checks name
// the rule first and the file after it.
static COLD bool pathchk_bad(string_address path, string_address why)
{
        string_format(log_error, "pathchk: %s: %s\n", path, why);
        return false;
}

static COLD bool pathchk_rule(string_address why, string_address path)
{
        string_format(log_error, "pathchk: %s '%s'\n", why, path);
        return false;
}

static COLD bool pathchk_limit(positive limit, positive length, string_address what,
                               string_address text, positive text_length)
{
        p8 shown[FILE_PATH_MAX];

        string_copy_max_end(shown, text, min(text_length, FILE_PATH_MAX - 1));
        string_format(log_error, "pathchk: limit %p exceeded by length %p of %s '%s'\n",
                      limit, length, what, shown);
        return false;
}

static b8 address_to pathchk_portable_set()
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

        return portable;
}

static bool pathchk_portable_chars(string_address path, positive length)
{
        return string_span(path, pathchk_portable_set()) == length;
}

static bool pathchk_one(string_address path, bool basic, bool extra)
{
        positive length = string_length(path);

        if ((basic || extra) && !length)
        {
                log_error("pathchk: empty file name\n", 0);
                return false;
        }

        positive at = 0;
        positive longest = 0;
        positive longest_at = 0;

        while (at < length)
        {
                at += memory_span_byte(path + at, '/', length - at);

                if (at >= length)
                        break;

                string_address stop = string_first_of_or_end(path + at, '/');
                positive component = (positive)(stop - path) - at;

                if (extra && string_is(path + at, '-'))
                        return pathchk_rule(
                            (string_address) "leading '-' in a component of file name", path);

                if (component > longest)
                {
                        longest = component;
                        longest_at = at;
                }

                at += component;
        }

        if (basic && !pathchk_portable_chars(path, length))
        {
                p8 offender[2] = {path[string_span(path, pathchk_portable_set())], end};

                string_format(log_error,
                              "pathchk: non-portable character '%s' in file name '%s'\n",
                              offender, path);
                return false;
        }

        if (basic)
        {
                if (length >= PATHCHK_POSIX_PATH)
                        return pathchk_limit(PATHCHK_POSIX_PATH - 1, length,
                                             (string_address) "file name", path, length);

                if (longest > PATHCHK_POSIX_NAME)
                        return pathchk_limit(PATHCHK_POSIX_NAME, longest,
                                             (string_address) "file name component",
                                             path + longest_at, longest);

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
                        return pathchk_limit(name_max, component,
                                             (string_address) "file name component",
                                             path + at, component);

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
                return string_report(log_error, 1, "%s: missing operand\n", (string_address) "pathchk");

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
// -Z asks for the default label and --context=VALUE for a named one; a
// kernel with no labels at all ignores the second and says so.
/*
        --context on a kernel with no labels at all.

        Written as the option is read rather than after the whole line has
        been taken, because that is where the reference writes it: mkdir
        --context=x -dash warns and then complains about -d, and a later bare
        --context does not take back the warning an earlier --context=x
        earned. Once per run, whatever the option was spelled or repeated.
*/
static string_address file_context_program;
static bool file_context_said;

static bool file_context_seen(p8 letter, string_address value)
{
        if ((letter == 'Z' || letter == 'C') && value && !file_context_said)
        {
                file_context_said = true;
                string_format(log_error,
                              "%s: warning: ignoring --context; it requires an "
                              "SELinux/SMACK-enabled kernel\n",
                              file_context_program);
        }

        return true;
}

static const file_long mkdir_longs[] = {
    {(string_address) "context", 'Z'},
    {(string_address) "mode", 'm'},
    {(string_address) "parents", 'p'},
    {(string_address) "verbose", 'v'},
    {null, 0},
};

static fn mkdir_told(string_address path)
{
        string_format(log, "mkdir: created directory '%s'\n", file_shown_c(path));
}

static b32 file_mkdir()
{
        positive count = (positive)program_argument_count();
        file_taking taking = {
            .program = (string_address) "mkdir",
            .allowed = (string_address) "mpvZ",
            .valued = (string_address) "m",
            .long_optional = (string_address) "Z",
            .longs = mkdir_longs,
            .seen = file_context_seen,
        };

        file_context_program = (string_address) "mkdir";
        file_context_said = false;

        if (!file_take(address_of taking))
                return 1;

        positive index = taking.first;
        positive mode = 0777;
        bool parents = (taking.flags & FILE_FLAG('p')) != 0;
        bool given_mode = (taking.flags & FILE_FLAG('m')) != 0;
        bool loud = (taking.flags & FILE_FLAG('v')) != 0;

        //      An operand is asked for before -m is read, the way the
        //      reference asks: mkdir -m nonsense with nothing to make is a
        //      missing operand and not an invalid mode.
        if (index >= count)
                return string_report(log_error, 1, "%s: missing operand\n", (string_address) "mkdir");

        //      -m is read against a=rwx the way the reference mkdir reads
        //      it: the base is all nine bits and not the umask-filtered set,
        //      so mkdir -m u=rwx is 0777 and not 0755. Only a clause that
        //      names no class is filtered through the umask, which is what
        //      keeps -m -w at 0577 under a mask of 022.
        if (given_mode &&
            (!string_get(file_option_value(address_of taking, 'm')) ||
             !file_mode_masked(file_option_value(address_of taking, 'm'),
                               0777, true, file_umask(),
                               address_of mode)))
        {
                string_format(log_error, "mkdir: invalid mode '%s'\n",
                              file_option_value(address_of taking, 'm'));
                return 1;
        }

        b32 status = 0;

        while (index < count)
        {
                string_address path = program_argument((b32)index++);

                if (parents)
                {
                        //      The parents are made with the default, and
                        //      only the directory that was named gets the
                        //      mode asked for. -v names each component this
                        //      made and none that was already there, and a
                        //      failure names the component that failed.
                        p8 failed[FILE_PATH_MAX];
                        bool made_it = false;

                        failed[0] = end;

                        bipolar made = file_make_parents_walk(
                            path, 0777, loud ? mkdir_told : null, failed,
                            address_of made_it);

                        if (made < 0)
                        {
                                //      A name too long for the walk's buffer
                                //      never became a component, so the whole
                                //      operand is what failed and what the
                                //      reference names.
                                string_format(log_error,
                                              "mkdir: cannot create directory '%s': %s\n",
                                              file_shown_c(string_get(failed) ? failed : path),
                                              file_reason(made));
                                status = 1;
                                continue;
                        }

                        //      -p over a directory that was already there
                        //      leaves it as it was; -m names the mode of what
                        //      this call makes.
                        if (given_mode && made_it)
                                system_change_mode_at(AT_FDCWD, path, mode);

                        continue;
                }

                bipolar made = system_make_directory_at(AT_FDCWD, path, mode);

                if (made < 0)
                {
                        string_format(log_error, "mkdir: cannot create directory '%s': %s\n",
                                      file_shown_c(path), file_reason(made));
                        status = 1;
                }
                else if (loud)
                        string_format(log, "mkdir: created directory '%s'\n",
                                      file_shown_c(path));

                if (made >= 0 && given_mode)
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

//      A mode outside the nine permission bits is read here and refused by
//      the caller, which is where the reference's own sentence about it goes.
static bool file_node_mode(string_address specification,
                           positive address_to mode)
{
        return file_mode_masked(specification, 0666, false, file_umask(), mode);
}

static b32 file_make_node(string_address program, string_address path,
                          positive kind, positive device, positive mode,
                          bool given_mode)
{
        bipolar made = system_call_4(syscall(mknodat), AT_FDCWD,
                                     (positive)path, kind | mode, device);

        if (made < 0)
        {
                string_format(log_error,
                              string_is(program + 2, 'f')
                                  ? "%s: cannot create fifo '%s': %s\n"
                                  : "%s: %s: %s\n",
                              program, path, file_reason(made));
                return 1;
        }

        if (given_mode &&
            system_change_mode_at(AT_FDCWD, path, mode) < 0)
                return string_report(log_error, 1,
                              "%s: cannot set permissions of '%s'\n",
                              program, path);

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
        //      This image has neither SELinux nor SMACK. -Z and a bare
        //      --context are no-ops; a named context is ignored with a
        //      warning, written where the option is read.
        taking->seen = file_context_seen;
        file_context_program = program;
        file_context_said = false;

        if (!file_take(taking) || file_operand_failed)
                return false;

        address_to given = (taking->flags & FILE_FLAG('m')) != 0;
        address_to mode = 0666;

        return true;
}

/*
        -m, read after the operands have been counted.

        The reference asks for its operands first -- mkfifo -m1777 with
        nothing to make is a missing operand, not an invalid mode -- and then
        refuses a mode carrying anything but the nine permission bits, which
        is a sentence of its own and not the invalid-mode one.
*/
static bool file_node_mode_taken(string_address program, file_taking address_to taking,
                                 positive address_to mode, bool given)
{
        if (!given)
                return true;

        if (!file_node_mode(file_option_value(taking, 'm'), mode))
                return string_report(log_error, false, "%s: invalid mode\n", program);

        if (address_to mode & ~(positive)0777)
                return string_report(log_error, false,
                                     "%s: mode must specify only file permission bits\n",
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
                return string_report(log_error, 1, "%s: missing operand\n", (string_address) "mkfifo");

        if (!file_node_mode_taken((string_address) "mkfifo", address_of taking,
                                  address_of mode, given_mode))
                return 1;

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

        /*
                The operand count, counted the way the reference counts it:
                the name, the type, and for everything but a pipe a major and
                a minor. Each complaint names the operand it is about -- the
                last one written when something is missing, the first spare
                one when there are too many -- and a type that wants numbers
                says so in a line of its own.
        */
        if (!file_operand_count)
                return string_report(log_error, 1, "%s: missing operand\n", (string_address) "mknod");

        if (file_operand_count == 1)
                return string_report(log_error, 1, "mknod: missing operand after '%s'\n",
                                     file_operand_at(0));

        //      Only the first letter of the type is read, so that the
        //      mnemonic spellings the reference allows -- character, block,
        //      pipe -- are the letters they begin with.
        bool pipe = string_is(file_operand_at(1), 'p');

        if (pipe)
        {
                if (file_operand_count > 2)
                {
                        string_format(log_error, "mknod: extra operand '%s'\n",
                                      file_operand_at(2));
                        log_error("Fifos do not have major and minor device numbers.\n", 0);
                        return 1;
                }
        }
        else if (file_operand_count < 4)
        {
                string_format(log_error, "mknod: missing operand after '%s'\n",
                              file_operand_at(file_operand_count - 1));

                //      The line about what a special file needs is written
                //      only when nothing but the name and the type were
                //      given; a line with a major and no minor has said it.
                if (file_operand_count == 2)
                        log_error("Special files require major and minor device numbers.\n", 0);

                return 1;
        }
        else if (file_operand_count > 4)
                return string_report(log_error, 1, "mknod: extra operand '%s'\n",
                                     file_operand_at(4));

        if (!file_node_mode_taken((string_address) "mknod", address_of taking,
                                  address_of mode, given_mode))
                return 1;

        string_address path = file_operand_at(0);
        p8 type = string_get(file_operand_at(1));
        positive kind;
        positive device = 0;

        if (pipe)
                kind = MODE_PIPE;
        else if (type == 'b' || type == 'c' || type == 'u')
        {
                p32 major, minor;

                if (!file_device_number(file_operand_at(2), address_of major))
                        return string_report(log_error, 1,
                                      "mknod: invalid major device number '%s'\n",
                                      file_operand_at(2));

                if (!file_device_number(file_operand_at(3), address_of minor))
                        return string_report(log_error, 1,
                                      "mknod: invalid minor device number '%s'\n",
                                      file_operand_at(3));

                kind = type == 'b' ? MODE_BLOCK : MODE_CHARACTER;
                device = file_device(major, minor);

                //      The reference hands the device number to a library
                //      that refuses one wider than the syscall's argument
                //      rather than letting the kernel see a truncated one,
                //      and answers with that refusal's reason.
                if (device != (positive)(p32)device)
                        return string_report(log_error, 1, "mknod: %s: %s\n", path,
                                             file_reason(-ERROR_INVALID));
        }
        else
                return string_report(log_error, 1, "mknod: invalid device type '%s'\n",
                              file_operand_at(1));


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
                return string_report(log_error, false, "sync: error opening '%s': %s\n",
                              path, file_reason(read_error));

        bipolar flags = system_call_3(syscall(fcntl), (positive)opened,
                                      FILE_F_GETFL, 0);
        bool good = flags >= 0 &&
                    system_call_3(syscall(fcntl), (positive)opened,
                                  FILE_F_SETFL,
                                  (positive)flags & ~O_NONBLOCK) >= 0;

        if (!good)
                string_format(log_error,
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
                        string_format(log_error, "sync: error syncing '%s': %s\n",
                                      path, file_reason(synced));
                        good = false;
                }
        }

        bipolar closed = system_close(opened);

        if (closed < 0)
        {
                string_format(log_error, "sync: failed to close '%s': %s\n",
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
                return string_report(log_error, 1, "sync: cannot specify both --data and --file-system\n");

        if (data && !file_operand_count)
                return string_report(log_error, 1, "sync: --data needs at least one argument\n");

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
                return string_report(log_error, false, "split: output file suffixes exhausted\n");

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
                                return string_report(log_error, false, "split: output file suffixes exhausted\n");
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
                return string_report(log_error, false, "split: output file name is too long\n");

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
                return string_report(log_error, false,
                              "split: '%s' would overwrite input; aborting\n",
                              output->name);

        output->handle = system_open_at_mode(AT_FDCWD, output->name,
                                             FILE_WRITE, 0666);

        if (output->handle < 0)
                return string_report(log_error, false, "split: cannot open '%s': %s\n",
                              output->name, file_reason(output->handle));

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
                return string_report(log_error, false, "split: write error on '%s'\n",
                              output->name);
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
                return string_report(log_error, false, "split: closing '%s': %s\n",
                              output->name, file_reason(closed));
        return true;
}

static bool split_fixed(bipolar in, p64 length, positive measure,
                         bool distribute, split_output address_to output,
                         p8 address_to bytes)
{
        bool range_copy = true;
        bool send_copy = true;
        positive chunks = distribute ? measure
                            : length ? (positive)((length - 1) / measure) + 1 : 0;
        p64 ordinary = distribute ? length / measure : measure;
        positive extra = distribute ? (positive)(length % measure) : 0;

        for (positive i = 0; i < chunks; i++)
        {
                positive here = (positive)min(length, ordinary + (i < extra));

                if (!split_output_open(output) ||
                    (here && !(bytes ? split_output_write(output, bytes, here)
                                : file_copy_stream(in, output->handle, here, true,
                                                   address_of range_copy,
                                                   address_of send_copy, null))) ||
                    !split_output_close(output))
                {
                        if (!bytes)
                                log_error("split: read or write error\n", 0);
                        return false;
                }
                length -= here;
                if (bytes)
                        bytes += here;
        }

        return true;
}

static bool split_stream(bipolar in, positive piece, p8 separator, bool lines,
                          split_output address_to output)
{
        positive in_piece = 0;

        while (1)
        {
                bipolar taken = system_read_retry((positive)in, file_transfer,
                                                   sizeof(file_transfer));

                if (taken < 0)
                        return string_report(log_error, false, "split: read error\n");
                if (!taken)
                        return split_output_close(output);

                p8 address_to pending = file_transfer;
                p8 address_to finish = file_transfer + (positive)taken;
                positive records = lines
                    ? memory_count(pending, (positive)taken, separator)
                    : (positive)taken;

                while (pending < finish)
                {
                        positive needed = piece - in_piece;

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
                        if (lines)
                        {
                                for (positive found_count = 0;
                                     found_count < needed; found_count++)
                                        scan = (p8 address_to)memory_first_of(
                                            scan, separator,
                                            (positive)(finish - scan)) + 1;
                        }
                        else
                                scan += needed;

                        if (!split_output_write(output, pending,
                                                (positive)(scan - pending)) ||
                            !split_output_close(output))
                                return false;
                        pending = scan;
                        records -= needed;
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

                if (used && record > piece - used)
                {
                        if (!split_output_close(output))
                                return false;
                        used = 0;
                }
                while (record)
                {
                        positive here = min(record, piece - used);
                        if (!split_output_write(output, input + at, here))
                                return false;
                        used += here;
                        at += here;
                        record -= here;
                        if (used == piece)
                        {
                                if (!split_output_close(output))
                                        return false;
                                used = 0;
                        }
                }
        }

        return split_output_close(output);
}

static bool split_materialized(bipolar in, file_facts address_to facts,
                                bool regular, positive piece, p8 separator,
                                bool distribute, split_output address_to output)
{
        if (regular)
        {
                if (!facts->size)
                        return true;

                bipolar mapped = system_call_6(
                    syscall(mmap), 0, (positive)facts->size,
                    FILE_PROTECT_READ, FILE_MAP_PRIVATE, (positive)in, 0);
                if (mapped < 0)
                        return string_report(log_error, false, "split: cannot map input: %s\n",
                                      file_reason(mapped));

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
                log_error(read_failed ? (string_address)"split: read error\n"
                                      : (string_address)"split: input too large\n",
                          0);
                text_arena_used = 0;
                return false;
        }

        bool answer = distribute
            ? split_fixed(in, length, piece, true, output, input)
            : split_line_bytes_memory(input, length, piece, separator, output);
        text_arena_used = 0;
        return answer;
}

static bool split_chunks(string_address text, positive address_to chunks)
{
        positive value;

        if (!file_unsigned_decimal(text, address_of value) || !value)
                return false;
        address_to chunks = value;
        return true;
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
                return string_report(log_error, 1, "split: extra operand\n");

        bool bytes = (taking.flags & FILE_FLAG('b')) != 0;
        bool lines = (taking.flags & FILE_FLAG('l')) != 0;
        bool line_bytes = (taking.flags & FILE_FLAG('C')) != 0;
        bool distribute = (taking.flags & FILE_FLAG('n')) != 0;

        if ((positive)bytes + (positive)lines + (positive)line_bytes +
                (positive)distribute >
            1)
                return string_report(log_error, 1, "split: cannot split in more than one way\n");

        p8 mode = bytes ? 'b' : line_bytes ? 'C' : distribute ? 'n' : 'l';

        positive piece = 1000;
        string_address measure = file_option_value(address_of taking, mode);

        if (measure && mode != 'n' &&
            !split_size(measure, address_of piece))
                return string_report(log_error, 1, "split: invalid number of bytes: '%s'\n",
                              measure);

        positive chunks = 0;
        if (mode == 'n' && !split_chunks(measure, address_of chunks))
                return string_report(log_error, 1,
                              "split: unsupported number of chunks: '%s'\n",
                              measure);

        positive suffix_length = 2;
        string_address width = file_option_value(address_of taking, 'a');

        if (width)
        {
                string_address at = width;

                if (!string_digits_checked(address_of at, 10,
                                           address_of suffix_length) ||
                    string_get(at) || !suffix_length ||
                    suffix_length > SPLIT_SUFFIX_MAX)
                        return string_report(log_error, 1,
                                      "split: invalid suffix length: '%s'\n",
                                      width);
        }

        p8 separator = '\n';
        string_address separator_text = file_option_value(address_of taking, 't');

        if (separator_text && !split_separator(separator_text,
                                               address_of separator))
                return string_report(log_error, 1, "split: multi-character separator\n");

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
                string_format(log_error, "split: cannot open '%s' for reading: %s\n",
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
                        string_format(log_error,
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

        if (regular && (mode == 'n' || (mode == 'b' && facts.size)))
                complete = split_fixed(in, facts.size,
                                        mode == 'n' ? chunks : piece,
                                        mode == 'n', address_of output, null);
        else if (mode == 'C' || mode == 'n')
                complete = split_materialized(in, address_of facts, regular,
                                               mode == 'n' ? chunks : piece,
                                               separator, mode == 'n',
                                               address_of output);
        else
                complete = split_stream(in, piece, separator, mode == 'l',
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
                return string_report(log_error, false, "csplit: output file name is too long\n");

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
                return string_report(log_error, false,
                              "csplit: '%s' would overwrite input; aborting\n",
                              state->name);

        bipolar out = system_open_at_mode(AT_FDCWD, state->name,
                                          FILE_WRITE, 0666);

        if (out < 0)
                return string_report(log_error, false, "csplit: cannot open '%s': %s\n",
                              state->name, file_reason(out));

        state->made++;
        bool written = !length ||
                       system_write_all((positive)out, state->input + from,
                                        length) == length;
        bipolar closed = system_close(out);

        if (!written || closed < 0)
                return string_report(log_error, false, "csplit: write error on '%s'\n",
                              state->name);

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
        else if (!file_signed_decimal(offset, address_of pattern->offset))
                return false;

        return true;
}

static bool csplit_parse_line(string_address word,
                              csplit_pattern address_to pattern)
{
        positive line;

        if (!file_unsigned_decimal(word, address_of line) || !line)
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

                if (regex_find(REGEX_FIRST, state->input + at, stop - at, 0))
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
                        log_error("csplit: --suppress-matched with an offset is unsupported\n",
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
                return string_report(log_error, 1, "csplit: missing operand\n");

        positive digits = 2;
        string_address digit_text = file_option_value(address_of taking, 'n');

        if (digit_text)
        {
                string_address at = digit_text;

                if (!string_digits_checked(address_of at, 10,
                                           address_of digits) ||
                    string_get(at) || !digits || digits > 32)
                        return string_report(log_error, 1, "csplit: invalid number: '%s'\n",
                                      digit_text);
        }

        string_address input_name = file_operand_at(0);
        bipolar in = string_is(input_name, '-') && !string_get(input_name + 1)
                         ? 0
                         : system_open_at(AT_FDCWD, input_name, FILE_READ);

        if (in < 0)
                return string_report(log_error, 1, "csplit: cannot open '%s': %s\n",
                              input_name, file_reason(in));

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
                log_error(read_failed ? "csplit: read error\n"
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
                bool forever = false;
                positive repeats = 1;
                bool repeated = csplit_repeat(word, address_of forever,
                                               address_of repeats);

                if (repeated)
                {
                        if (!have_pattern)
                        {
                                log_error("csplit: repeat with no previous pattern\n", 0);
                                failed = true;
                                break;
                        }
                }
                else
                {
                        memory_fill(address_of pattern, 0, sizeof(pattern));

                        if (string_is(word, '/') || string_is(word, '%'))
                        {
                                if (!csplit_parse_regex(word, address_of pattern))
                                {
                                        string_format(log_error,
                                                      "csplit: '%s': invalid pattern\n", word);
                                        failed = true;
                                        break;
                                }
                                if (state.suppress_matched && pattern.offset)
                                {
                                        log_error("csplit: --suppress-matched with an offset is unsupported\n",
                                                  0);
                                        failed = true;
                                        break;
                                }
                                if (!regex_compile(pattern.expression, false, false,
                                                   false, CSPLIT_REGEX_POLICY))
                                {
                                        string_format(log_error,
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
                                string_format(log_error, "csplit: '%s': invalid pattern\n",
                                              word);
                                failed = true;
                                break;
                        }
                        have_pattern = true;
                }

                for (positive repetition = 0; forever || repetition < repeats;
                     repetition++)
                {
                        b32 done = csplit_execute(address_of state,
                                                  address_of pattern, repeated);

                        if (done == CSPLIT_EXECUTED)
                                continue;
                        if (done == CSPLIT_NOT_FOUND && forever)
                        {
                                /* A repeated %pattern% consumes the unmatched
                                   tail as part of the suppressed search. */
                                if (pattern.discard)
                                        state.suppress_final = true;
                                break;
                        }

                        if (done == CSPLIT_NOT_FOUND)
                        {
                                if (!csplit_section(address_of state, state.cursor,
                                                     length, !pattern.discard))
                                {
                                        failed = true;
                                        break;
                                }
                                string_format(log_error,
                                              repeated ? "csplit: '%s': match not found on repetition %p\n"
                                                       : "csplit: '%s': match not found\n",
                                              word, repetition + 1);
                        }
                        failed = true;
                        break;
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
/*
        A size, read where its option is written.

        The reference reads every -s as the getopt loop reaches it, and the
        modifier it carries stays behind for the next one: -s +4 --size=4 is
        still relative, which is why it may stand beside --reference, and a
        second plus or minus over a modifier already standing is the one thing
        it calls multiple relative modifiers. The number itself is the last
        one written.
*/
static bool truncate_two_modifiers;
//      A number whose digits are a number but whose value is past what a
//      size can hold: the reference names the number and then the reason.
static bool truncate_too_large;

static bool truncate_size(string_address text, b64 address_to out,
                          p8 address_to relation)
{
        while (byte_is_space(string_get(text)))
                text++;

        p8 mode = address_to relation;
        p8 lead = string_get(text);

        truncate_two_modifiers = false;
        truncate_too_large = false;

        if (lead == '<' || lead == '>' || lead == '/' || lead == '%')
        {
                mode = lead == '<'   ? TRUNCATE_AT_MOST
                       : lead == '>' ? TRUNCATE_AT_LEAST
                       : lead == '/' ? TRUNCATE_ROUND_DOWN
                                     : TRUNCATE_ROUND_UP;
                text++;

                while (byte_is_space(string_get(text)))
                        text++;
        }

        bool negative = string_is(text, '-');

        if (negative || string_is(text, '+'))
        {
                if (mode != TRUNCATE_ABSOLUTE)
                {
                        truncate_two_modifiers = true;
                        return false;
                }

                mode = TRUNCATE_RELATIVE;
                text++;
        }

        // Nineteen digits always fit; twenty are either past the unsigned
        // range or past the signed one every answer is checked against
        // below, so a run that reaches twenty is refused before it can wrap.
        positive digits;
        p64 magnitude = string_digits_max(text, 20, address_of digits);

        if (digits == 20)
        {
                truncate_too_large = true;
                return false;
        }

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
                        {
                                truncate_too_large = true;
                                return false;
                        }

                        magnitude *= base;
                }
        }

        if (string_get(text))
                return false;

        if (magnitude > (p64)b64_max + (p64)negative)
        {
                truncate_too_large = true;
                return false;
        }

        //      A rounding step of nothing is read as the number it is; the
        //      caller says it is a division by zero, which is what the
        //      reference calls it rather than an invalid number.
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
                return string_report(log_error, false,
                              "truncate: cannot get the size of '%s': %s\n",
                              path, file_reason(size));

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

                return string_report(log_error, false, "truncate: cannot open '%s' for writing: %s\n",
                              path, file_reason(handle));
        }

        file_facts facts;
        bool need_facts = blocks || (relation && reference < 0);

        if (need_facts &&
            !file_look(handle, (string_address) "", AT_EMPTY_PATH,
                       address_of facts))
        {
                string_report(log_error, false, "%s: %s: %s\n", (string_address) "truncate", path, (string_address) "cannot stat");
                system_close(handle);
                return false;
        }

        if (blocks)
        {
                p64 block = facts.blocksize;

                if (!block || size > b64_max / (b64)block ||
                    size < b64_min / (b64)block)
                {
                        string_report(log_error, false, "%s: %s: %s\n", (string_address) "truncate", path, (string_address) "size overflow");
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
                string_report(log_error, false, "%s: %s: %s\n", (string_address) "truncate", path, (string_address) "size overflow");
                system_close(handle);
                return false;
        }

        if (wanted < 0)
                wanted = 0;

        bipolar done = system_truncate_handle(handle, wanted);
        bipolar closed = system_close(handle);

        if (done < 0)
                return string_report(log_error, false, "truncate: failed to truncate '%s': %s\n",
                              path, file_reason(done));

        if (closed < 0)
                return string_report(log_error, false, "truncate: failed to close '%s': %s\n",
                              path, file_reason(closed));

        return true;
}

static b64 truncate_asked;
static p8 truncate_relation;
static bool truncate_given;

static bool truncate_option_seen(p8 letter, string_address value)
{
        if (letter != 's' || !value)
                return true;

        if (!truncate_size(value, address_of truncate_asked,
                           address_of truncate_relation))
        {
                if (truncate_two_modifiers)
                        return string_report(log_error, false,
                                             "truncate: multiple relative modifiers specified\n");

                if (truncate_too_large)
                        return string_report(log_error, false,
                                             "truncate: Invalid number: '%s': "
                                             "Value too large for defined data type\n",
                                             value);

                return string_report(log_error, false,
                                     "truncate: Invalid number: '%s'\n", value);
        }

        //      A rounding step of nothing is a division by nothing, and the
        //      reference says which of the two it is.
        if ((truncate_relation == TRUNCATE_ROUND_DOWN ||
             truncate_relation == TRUNCATE_ROUND_UP) && !truncate_asked)
                return string_report(log_error, false, "truncate: division by zero\n");

        truncate_given = true;

        return true;
}

static b32 file_truncate()
{
        file_operands_begin();

        truncate_asked = 0;
        truncate_relation = TRUNCATE_ABSOLUTE;
        truncate_given = false;

        file_taking taking = {
            .program = (string_address) "truncate",
            .allowed = (string_address) "cors",
            .valued = (string_address) "rs",
            .longs = truncate_longs,
            .operand = file_operand,
            .seen = truncate_option_seen,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;

        string_address reference_path = file_option_value(address_of taking, 'r');
        bool blocks = (taking.flags & FILE_FLAG('o')) != 0;
        bool no_create = (taking.flags & FILE_FLAG('c')) != 0;
        b64 size = truncate_asked;
        p8 relation = truncate_relation;
        bool size_text = truncate_given;

        if (!reference_path && !size_text)
        {
                log_error("truncate: you must specify either '--size' or '--reference'\n", 0);
                return 1;
        }

        if (reference_path && size_text && relation == TRUNCATE_ABSOLUTE)
        {
                log_error("truncate: you must specify a relative '--size'"
                          " with '--reference'\n", 0);
                return 1;
        }

        if (blocks && !size_text)
                return string_report(log_error, 1,
                                     "truncate: '--io-blocks' was specified but '--size' was not\n");

        if (!file_operand_count)
                return string_report(log_error, 1, "truncate: missing file operand\n");

        b64 reference = -1;

        if (reference_path)
        {
                file_facts facts;
                bipolar looked = file_look_code(AT_FDCWD, reference_path, 0,
                                                address_of facts);

                if (looked < 0)
                        return string_report(log_error, 1, "truncate: cannot stat '%s': %s\n",
                                      reference_path, file_reason(looked));

                bipolar handle = -1;

                if ((facts.mode & MODE_FORMAT) != MODE_FILE)
                        handle = system_open_at(AT_FDCWD,
                                               reference_path,
                                               FILE_READ);

                if (handle < 0 && (facts.mode & MODE_FORMAT) != MODE_FILE)
                        return string_report(log_error, 1,
                                      "truncate: cannot get the size of '%s': %s\n",
                                      reference_path, file_reason(handle));

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

// hardlink ---------------------------------------------------------
/*
        hardlink's walk is file_change_walk_as, shared with chmod/chown/chgrp.
        Candidate ordering is the library's stable merge sorter.  Only files
        with matching device, size and requested metadata are mapped; the
        tuned in-memory hash makes unlike contents cheap, and memory_compare
        remains the final proof so a hash collision can never replace data.

        Replacement uses linkat plus renameat2(RENAME_EXCHANGE).  The file
        displaced by the exchange remains under the temporary name until its
        inode is checked against the one whose contents were proved.  A name
        changed by a racing process is exchanged back, never overwritten.
*/
#define HARDLINK_RENAME_EXCHANGE 2
#define HARDLINK_RENAME_NOREPLACE 1

typedef struct
{
        positive path_at;
        positive path_hash;
        file_facts facts;
        positive hash;
        bool hashed;
        bool valid;
        bool listed;
} hardlink_file;

static hardlink_file address_to hardlink_files;
static positive hardlink_file_count;
static positive hardlink_file_room;
static p8 address_to hardlink_paths;
static positive hardlink_path_used;
static positive hardlink_path_room;
static positive address_to hardlink_order;
static positive address_to hardlink_spare;
static positive hardlink_order_room;
static positive hardlink_spare_room;
static p64 hardlink_minimum;
static p64 hardlink_maximum;
static bool hardlink_ignore_mode;
static bool hardlink_ignore_owner;
static bool hardlink_ignore_time;
static bool hardlink_respect_name;
static bool hardlink_sort_hash;
static bool hardlink_sort_links;
static b32 hardlink_status;
static positive hardlink_linked;
static p64 hardlink_saved;
static positive hardlink_temp_number;
static positive hardlink_process;
static positive address_to hardlink_seen;
static positive hardlink_seen_room;
static positive hardlink_io_size;
static p8 address_to hardlink_read_one;
static p8 address_to hardlink_read_two;

static string_address hardlink_path(hardlink_file address_to file)
{
        return hardlink_paths + file->path_at;
}

static bool hardlink_moment_same(file_moment address_to one,
                                 file_moment address_to two)
{
        return one->seconds == two->seconds &&
               one->nanoseconds == two->nanoseconds;
}

static bool hardlink_metadata_same(hardlink_file address_to one,
                                   hardlink_file address_to two)
{
        if (one->facts.device_major != two->facts.device_major ||
            one->facts.device_minor != two->facts.device_minor ||
            one->facts.size != two->facts.size)
                return false;
        if (!hardlink_ignore_mode &&
            (one->facts.mode & 07777) != (two->facts.mode & 07777))
                return false;
        if (!hardlink_ignore_owner &&
            (one->facts.owner != two->facts.owner ||
             one->facts.group != two->facts.group))
                return false;
        if (!hardlink_ignore_time &&
            !hardlink_moment_same(address_of one->facts.modified,
                                  address_of two->facts.modified))
                return false;
        if (hardlink_respect_name &&
            !string_equals(file_last_component(hardlink_path(one)),
                           file_last_component(hardlink_path(two))))
                return false;
        return true;
}

/* Strict stability is separate from user-selected equivalence.  Even -c may
   not use bytes observed before a concurrent size/mode/owner/time change. */
static bool hardlink_snapshot_same(file_facts address_to old,
                                   file_facts address_to now)
{
        return file_same_identity(old, now) && old->size == now->size &&
               old->mode == now->mode && old->owner == now->owner &&
               old->group == now->group &&
               hardlink_moment_same(address_of old->changed,
                                    address_of now->changed) &&
               hardlink_moment_same(address_of old->modified,
                                    address_of now->modified);
}

/* A load below one half makes repeated or overlapping input trees O(n).
   Offsets, not pointers, survive growth of the shared path arena. */
static bool hardlink_seen_prepare(positive wanted)
{
        if (wanted <= hardlink_seen_room / 2)
                return true;

        positive larger = hardlink_seen_room ? hardlink_seen_room : 256;

        while (wanted > larger / 2)
        {
                if (larger > positive_max / 2)
                        return false;
                larger *= 2;
        }
        if (larger > positive_max / sizeof(positive))
                return false;

        positive address_to table =
            (positive address_to)text_arena_take(larger * sizeof(positive));
        if (!table)
                return false;
        memory_fill(table, 0, larger * sizeof(positive));

        for (positive i = 0; i < hardlink_file_count; i++)
        {
                positive slot = hardlink_files[i].path_hash & (larger - 1);
                while (table[slot])
                        slot = (slot + 1) & (larger - 1);
                table[slot] = i + 1;
        }

        hardlink_seen = table;
        hardlink_seen_room = larger;
        return true;
}

static fn hardlink_visit(bipolar directory, string_address name,
                         string_address shown)
{
        file_facts facts;
        bipolar looked = file_look_code(directory, name, AT_SYMLINK_NOFOLLOW,
                                        address_of facts);

        if (looked < 0)
        {
                string_format(log_error, "hardlink: cannot stat '%s': %s\n",
                              shown, file_reason(looked));
                hardlink_status = 1;
                return;
        }
        if ((facts.mode & MODE_FORMAT) != MODE_FILE ||
            facts.size < hardlink_minimum || facts.size > hardlink_maximum)
                return;

        positive2 named = string_hash_33_length(shown);
        if (!hardlink_seen_prepare(hardlink_file_count + 1))
        {
                log_error("hardlink: out of memory while indexing paths\n", 0);
                hardlink_status = 1;
                return;
        }

        positive slot = named.x & (hardlink_seen_room - 1);
        while (hardlink_seen[slot])
        {
                hardlink_file address_to have =
                    hardlink_files + hardlink_seen[slot] - 1;
                if (have->path_hash == named.x &&
                    string_equals(hardlink_path(have), shown))
                        return;
                slot = (slot + 1) & (hardlink_seen_room - 1);
        }

        positive length = named.y;

        if (!shell_array_room(hardlink_files, hardlink_file_room,
                              hardlink_file_count + 1) ||
            !shell_array_room(hardlink_paths, hardlink_path_room,
                              hardlink_path_used + length + 1))
        {
                log_error("hardlink: out of memory while walking files\n", 0);
                hardlink_status = 1;
                return;
        }

        hardlink_file address_to file = hardlink_files + hardlink_file_count++;
        memory_fill(file, 0, sizeof(*file));
        file->path_at = hardlink_path_used;
        file->path_hash = named.x;
        file->facts = facts;
        file->valid = true;
        memory_copy_apart_end(hardlink_paths + hardlink_path_used, shown,
                              length);
        hardlink_path_used += length + 1;
        hardlink_seen[slot] = hardlink_file_count;
}

static b32 hardlink_index_compare(positive left_at, positive right_at)
{
        hardlink_file address_to left = hardlink_files + left_at;
        hardlink_file address_to right = hardlink_files + right_at;

#define HARDLINK_COMPARE(field)                                             \
        do {                                                                \
                if (left->facts.field != right->facts.field)                \
                        return left->facts.field < right->facts.field ? -1 : 1; \
        } while (0)
        HARDLINK_COMPARE(device_major);
        HARDLINK_COMPARE(device_minor);
        HARDLINK_COMPARE(size);
        if (!hardlink_ignore_mode)
                HARDLINK_COMPARE(mode);
        if (!hardlink_ignore_owner)
        {
                HARDLINK_COMPARE(owner);
                HARDLINK_COMPARE(group);
        }
        if (!hardlink_ignore_time)
        {
                HARDLINK_COMPARE(modified.seconds);
                HARDLINK_COMPARE(modified.nanoseconds);
        }
#undef HARDLINK_COMPARE

        if (hardlink_respect_name)
        {
                b32 named = string_compare(
                    file_last_component(hardlink_path(left)),
                    file_last_component(hardlink_path(right)));
                if (named)
                        return named;
        }
        if (hardlink_sort_hash && left->hash != right->hash)
                return left->hash < right->hash ? -1 : 1;
        if (hardlink_sort_links &&
            left->facts.hard_links != right->facts.hard_links)
                return left->facts.hard_links > right->facts.hard_links
                           ? -1 : 1;
        return 0;
}

/* Exact positional reads and writes share offset/short-transfer handling.
   Probe reads that preserve a short prefix intentionally use storage_read. */
static bipolar file_transfer_exact(positive operation, bipolar handle,
                                   p8 address_to buffer, positive length,
                                   p64 offset)
{
        positive have = 0;

        while (have < length)
        {
                bipolar got = system_call_4(
                    operation, (positive)handle,
                    (positive)(buffer + have), length - have,
                    (positive)(offset + have));

                if (got == -4)
                        continue;
                if (got <= 0)
                        return got ? got : -ERROR_INPUT_OUTPUT;
                have += (positive)got;
        }
        return (bipolar)have;
}

static bool hardlink_hash_one(hardlink_file address_to file)
{
        string_address path = hardlink_path(file);
        bipolar handle = system_open_at(AT_FDCWD, path,
                                        FILE_READ | O_CLOEXEC);
        if (handle < 0)
        {
                string_format(log_error, "hardlink: cannot read '%s': %s\n",
                              path, file_reason(handle));
                hardlink_status = 1;
                file->valid = false;
                return false;
        }

        file_facts before;
        file_facts after;
        bool stable = file_look(handle, (string_address)"", AT_EMPTY_PATH,
                                address_of before) &&
                      hardlink_snapshot_same(address_of file->facts,
                                             address_of before);
        bipolar read_error = 0;
        p64 offset = 0;
        positive hash = 5381;

        while (stable && offset < before.size)
        {
                p64 left = before.size - offset;
                positive take = left < hardlink_io_size
                                    ? (positive)left : hardlink_io_size;
                bipolar got = file_transfer_exact(syscall(pread64), handle,
                                                   hardlink_read_one, take, offset);
                if (got < 0)
                {
                        read_error = got;
                        stable = false;
                        break;
                }
                hash = hash * 33 +
                       memory_hash_33(hardlink_read_one, take);
                offset += take;
        }
        if (stable)
                file->hash = hash;

        stable = stable &&
                 file_look(handle, (string_address)"", AT_EMPTY_PATH,
                           address_of after) &&
                 hardlink_snapshot_same(address_of before, address_of after);
        system_close(handle);

        file->valid = stable;
        file->hashed = stable;
        if (stable)
                file->facts = after;
        else
        {
                if (read_error)
                        string_format(log_error,
                                      "hardlink: short or failed read of '%s': %s\n",
                                      path, file_reason(read_error));
                else
                        string_format(log_error,
                                      "hardlink: '%s' changed while reading\n",
                                      path);
                hardlink_status = 1;
        }
        return stable;
}

static bool hardlink_equal(hardlink_file address_to one,
                           hardlink_file address_to two)
{
        string_address one_path = hardlink_path(one);
        string_address two_path = hardlink_path(two);
        bipolar one_handle = system_open_at(
            AT_FDCWD, one_path, FILE_READ | O_CLOEXEC);
        bipolar two_handle = system_open_at(
            AT_FDCWD, two_path, FILE_READ | O_CLOEXEC);

        if (one_handle < 0 || two_handle < 0)
        {
                string_format(log_error,
                              "hardlink: cannot compare '%s' and '%s': %s\n",
                              one_path, two_path,
                              file_reason(one_handle < 0 ? one_handle
                                                         : two_handle));
                hardlink_status = 1;
                if (one_handle < 0)
                        one->valid = false;
                if (two_handle < 0)
                        two->valid = false;
                if (one_handle >= 0)
                        system_close(one_handle);
                if (two_handle >= 0)
                        system_close(two_handle);
                return false;
        }

        file_facts one_before;
        file_facts two_before;
        file_facts one_after;
        file_facts two_after;
        bool one_stable = file_look(one_handle, (string_address)"",
                                    AT_EMPTY_PATH, address_of one_before) &&
                          hardlink_snapshot_same(address_of one->facts,
                                                 address_of one_before);
        bool two_stable = file_look(two_handle, (string_address)"",
                                    AT_EMPTY_PATH, address_of two_before) &&
                          hardlink_snapshot_same(address_of two->facts,
                                                 address_of two_before);
        bool stable = one_stable && two_stable;
        bool same = stable && one_before.size == two_before.size;
        bipolar read_error = 0;
        p64 offset = 0;

        while (same && offset < one_before.size)
        {
                p64 left = one_before.size - offset;
                positive take = left < hardlink_io_size
                                    ? (positive)left : hardlink_io_size;
                bipolar got = file_transfer_exact(syscall(pread64), one_handle,
                                                   hardlink_read_one, take, offset);
                if (got >= 0)
                        got = file_transfer_exact(syscall(pread64), two_handle,
                                                  hardlink_read_two, take, offset);
                if (got < 0)
                {
                        read_error = got;
                        stable = false;
                        same = false;
                        break;
                }
                same = !memory_compare(hardlink_read_one, hardlink_read_two,
                                       take);
                offset += take;
        }

        one_stable = one_stable &&
                     file_look(one_handle, (string_address)"", AT_EMPTY_PATH,
                               address_of one_after) &&
                     hardlink_snapshot_same(address_of one_before,
                                            address_of one_after);
        two_stable = two_stable &&
                     file_look(two_handle, (string_address)"", AT_EMPTY_PATH,
                               address_of two_after) &&
                     hardlink_snapshot_same(address_of two_before,
                                            address_of two_after);
        stable = stable && one_stable && two_stable;
        system_close(one_handle);
        system_close(two_handle);

        if (!one_stable)
                one->valid = false;
        if (!two_stable)
                two->valid = false;
        if (!stable)
        {
                string_format(log_error,
                              read_error
                                  ? (string_address)"hardlink: short or failed read while comparing '%s' and '%s': %s\n"
                                  : (string_address)"hardlink: file changed while comparing '%s' and '%s': %s\n",
                              one_path, two_path,
                              read_error ? file_reason(read_error)
                                         : (string_address)"comparison abandoned");
                hardlink_status = 1;
        }
        return same && stable;
}

static bool hardlink_temporary(string_address destination, p8 address_to into)
{
        static const string_address middle =
            (string_address)".moonwater-hardlink-";
        positive destination_length = string_length(destination);
        positive middle_length = string_length(middle);

        if (destination_length + middle_length + 42 >= FILE_PATH_MAX)
                return false;

        memory_copy_apart(into, destination, destination_length);
        memory_copy_apart(into + destination_length, middle, middle_length);
        positive used = destination_length + middle_length;
        used += positive_into_string(into + used, hardlink_process);
        into[used++] = '-';
        positive_into_string(into + used, hardlink_temp_number++);
        return true;
}

/* linkat necessarily changes ctime and nlink.  Everything else about the
   linked inode must still be the exact object whose bytes were proved. */
static bool hardlink_link_same(file_facts address_to old,
                               file_facts address_to now)
{
        return file_same_identity(old, now) && old->size == now->size &&
               old->mode == now->mode && old->owner == now->owner &&
               old->group == now->group &&
               hardlink_moment_same(address_of old->modified,
                                    address_of now->modified);
}

/* Never unlink the public exchange name after merely looking at it: a
   concurrent rename could put a different object there between statx and
   unlinkat.  First move that dentry, without replacement, to a fresh name
   which no destination-side racer has used, prove it again, and only then
   remove it.  Any ambiguity is preserved under the reported name. */
static bool hardlink_cleanup(string_address temporary,
                             string_address destination,
                             file_facts address_to expected)
{
        p8 cleanup[FILE_PATH_MAX];
        bipolar moved = -ERROR_EXISTS;

        for (positive tries = 0; tries < 128 && moved == -ERROR_EXISTS; tries++)
        {
                if (!hardlink_temporary(destination, cleanup))
                        return string_report(log_error, false,
                                      "hardlink: displaced object retained as '%s'; cleanup path is too long\n",
                                      temporary);
                moved = system_rename_at(AT_FDCWD, temporary, AT_FDCWD,
                                         cleanup, HARDLINK_RENAME_NOREPLACE);
        }
        if (moved < 0)
                return string_report(log_error, false,
                              "hardlink: displaced object retained as '%s': %s\n",
                              temporary, file_reason(moved));

        file_facts first;
        file_facts final;
        if (!file_look_link(cleanup, address_of first) ||
            !hardlink_link_same(expected, address_of first) ||
            !file_look_link(cleanup, address_of final) ||
            !hardlink_snapshot_same(address_of first, address_of final))
                return string_report(log_error, false,
                              "hardlink: cleanup race; displaced object retained as '%s'\n",
                              cleanup);

        bipolar removed = system_remove_at(AT_FDCWD, cleanup, 0);
        if (removed < 0)
                return string_report(log_error, false,
                              "hardlink: displaced object retained as '%s': %s\n",
                              cleanup, file_reason(removed));
        return true;
}

static fn hardlink_refresh(hardlink_file address_to file,
                           file_facts address_to expected)
{
        file_facts now;
        if (file_look_link(hardlink_path(file), address_of now) &&
            hardlink_link_same(expected, address_of now))
                file->facts = now;
        else
                file->valid = false;
}

static bool hardlink_replace(hardlink_file address_to keep,
                             hardlink_file address_to duplicate)
{
        p8 temporary[FILE_PATH_MAX];
        string_address keep_path = hardlink_path(keep);
        string_address duplicate_path = hardlink_path(duplicate);
        file_facts keep_current;
        file_facts duplicate_current;

        if (!file_look_link(keep_path, address_of keep_current) ||
            !hardlink_snapshot_same(address_of keep->facts,
                                    address_of keep_current) ||
            !file_look_link(duplicate_path, address_of duplicate_current) ||
            !hardlink_snapshot_same(address_of duplicate->facts,
                                    address_of duplicate_current))
                return string_report(log_error, false,
                              "hardlink: '%s' or '%s' changed before replacement\n",
                              keep_path, duplicate_path);

        bipolar linked = -ERROR_EXISTS;

        for (positive tries = 0; tries < 128 && linked == -ERROR_EXISTS; tries++)
        {
                if (!hardlink_temporary(duplicate_path, temporary))
                        return string_report(log_error, false, "hardlink: temporary path is too long\n");
                linked = system_link_at(AT_FDCWD, keep_path, AT_FDCWD,
                                        temporary, 0);
        }
        if (linked < 0)
                return string_report(log_error, false, "hardlink: cannot link '%s': %s\n",
                              duplicate_path, file_reason(linked));

        file_facts made;
        if (!file_look_link(temporary, address_of made) ||
            !hardlink_link_same(address_of keep_current, address_of made))
        {
                string_format(log_error,
                              "hardlink: temporary link for '%s' could not be proved\n",
                              duplicate_path);
                keep->valid = false;
                return false;
        }

        /* Linking changed the retained inode's ctime.  Keep the strict
           snapshot current even when a later exchange cannot proceed. */
        keep->facts = made;

        file_facts current;
        if (!file_look_link(duplicate_path, address_of current) ||
            !hardlink_snapshot_same(address_of duplicate_current,
                                    address_of current))
        {
                hardlink_cleanup(temporary, duplicate_path, address_of made);
                hardlink_refresh(keep, address_of made);
                return string_report(log_error, false,
                              "hardlink: '%s' changed before exchange\n",
                              duplicate_path);
        }

        bipolar exchanged = system_rename_at(
            AT_FDCWD, temporary, AT_FDCWD, duplicate_path,
            HARDLINK_RENAME_EXCHANGE);
        if (exchanged < 0)
        {
                hardlink_cleanup(temporary, duplicate_path, address_of made);
                hardlink_refresh(keep, address_of made);
                return string_report(log_error, false, "hardlink: cannot replace '%s': %s\n",
                              duplicate_path, file_reason(exchanged));
        }

        file_facts linked_current;
        if (!file_look_link(duplicate_path, address_of linked_current) ||
            !hardlink_link_same(address_of made, address_of linked_current))
        {
                string_format(log_error,
                              "hardlink: destination race at '%s'; displaced object retained as '%s'\n",
                              duplicate_path, temporary);
                keep->valid = false;
                return false;
        }
        /* RENAME_EXCHANGE changes ctime on the retained inode as well. */
        keep->facts = linked_current;

        file_facts displaced;
        if (!file_look_link(temporary, address_of displaced) ||
            !hardlink_link_same(address_of duplicate_current,
                                address_of displaced))
                return string_report(log_error, false,
                              "hardlink: race while replacing '%s'; unexpected displaced object retained as '%s'\n",
                              duplicate_path, temporary);

        return hardlink_cleanup(temporary, duplicate_path,
                                address_of displaced);
}

static fn hardlink_list_one(hardlink_file address_to file, p8 delimiter)
{
        positive_to_padded(log, file->hash, 20, '0', 0);
        log("\t", 1);
        log(hardlink_path(file), 0);
        log(address_of delimiter, 1);
}

static fn hardlink_list_pair(hardlink_file address_to keep,
                             hardlink_file address_to duplicate,
                             p8 delimiter)
{
        if (!keep->listed)
        {
                hardlink_list_one(keep, delimiter);
                keep->listed = true;
        }
        if (!duplicate->listed)
        {
                hardlink_list_one(duplicate, delimiter);
                duplicate->listed = true;
        }
}

static const file_long hardlink_longs[] = {
    {(string_address)"content", 'c'},
    {(string_address)"io-size", 'b'},
    {(string_address)"respect-name", 'f'},
    {(string_address)"list-duplicates", 'l'},
    {(string_address)"dry-run", 'n'},
    {(string_address)"ignore-owner", 'o'},
    {(string_address)"ignore-mode", 'p'},
    {(string_address)"quiet", 'q'},
    {(string_address)"reflink", 'R'},
    {(string_address)"minimum-size", 's'},
    {(string_address)"maximum-size", 'S'},
    {(string_address)"ignore-time", 't'},
    {(string_address)"verbose", 'v'},
    {(string_address)"zero", 'z'},
    {(string_address)"help", 'h'},
    {(string_address)"version", 'V'},
    {null, 0},
};

static bool hardlink_size(string_address text, p64 address_to size)
{
        b64 parsed;
        p8 relation;
        if (!truncate_size(text, address_of parsed, address_of relation) ||
            relation != TRUNCATE_ABSOLUTE || parsed < 0)
                return false;
        address_to size = (p64)parsed;
        return true;
}

static b32 file_hardlink()
{
        file_operands_begin();
        file_taking taking = {
            .program = (string_address)"hardlink",
            .allowed = (string_address)"bcflnopqSstvzhV",
            .valued = (string_address)"bsS",
            .long_optional = (string_address)"R",
            .longs = hardlink_longs,
            .operand = file_operand,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;
        if (file_meta(address_of taking, "[options] FILE|DIRECTORY...\n"
                      "  -c content only  -n dry-run  -l list  -q quiet\n"
                      "  -s MIN  -S MAX  -f respect name", log))
                return 0;
        if (!file_operand_count)
                return string_report(log_error, 1, "%s: missing operand\n", (string_address)"hardlink");

        if (taking.flags & FILE_FLAG('R'))
        {
                string_address reflink = file_option_value(address_of taking, 'R');
                if ((taking.bare & FILE_FLAG('R')) ||
                    !reflink || !string_equals(reflink, (string_address)"never"))
                        return string_report(log_error, 1, "hardlink: reflink auto/always is unsupported; use --reflink=never\n");
        }

        string_address io_size = file_option_value(address_of taking, 'b');
        p64 parsed_io_size = FILE_TRANSFER_SIZE;
        if (io_size && (!hardlink_size(io_size, address_of parsed_io_size) ||
                        !parsed_io_size || parsed_io_size > positive_max))
                return string_report(log_error, 1, "hardlink: invalid I/O size: '%s'\n",
                              io_size);

        hardlink_minimum = 1;
        hardlink_maximum = (p64)-1;
        string_address minimum = file_option_value(address_of taking, 's');
        string_address maximum = file_option_value(address_of taking, 'S');
        if ((minimum && !hardlink_size(minimum, address_of hardlink_minimum)) ||
            (maximum && !hardlink_size(maximum, address_of hardlink_maximum)))
                return string_report(log_error, 1, "hardlink: invalid minimum or maximum size\n");

        bool content = (taking.flags & FILE_FLAG('c')) != 0;
        hardlink_ignore_mode = content || (taking.flags & FILE_FLAG('p'));
        hardlink_ignore_owner = content || (taking.flags & FILE_FLAG('o'));
        hardlink_ignore_time = content || (taking.flags & FILE_FLAG('t'));
        hardlink_respect_name = (taking.flags & FILE_FLAG('f')) != 0;
        bool dry = (taking.flags & FILE_FLAG('n')) != 0;
        bool listing = (taking.flags & FILE_FLAG('l')) != 0;
        bool quiet = (taking.flags & FILE_FLAG('q')) != 0;
        bool verbose = (taking.flags & FILE_FLAG('v')) != 0;
        p8 delimiter = (taking.flags & FILE_FLAG('z')) ? 0 : '\n';

        hardlink_file_count = 0;
        hardlink_path_used = 0;
        hardlink_status = 0;
        hardlink_linked = 0;
        hardlink_saved = 0;
        hardlink_temp_number = 0;
        hardlink_process = (positive)system_call(syscall(getpid));
        hardlink_io_size = (positive)parsed_io_size;
        if (hardlink_seen && hardlink_seen_room)
                memory_fill(hardlink_seen, 0,
                            hardlink_seen_room * sizeof(positive));

        for (positive i = 0; i < file_operand_count; i++)
        {
                string_address path = file_operand_at(i);
                file_change_walk_as(AT_FDCWD, path, path, FILE_MAX_DEPTH,
                                    (string_address)"hardlink",
                                    address_of hardlink_status, hardlink_visit,
                                    true);
        }

        if (hardlink_file_count > 1)
        {
                hardlink_read_one =
                    (p8 address_to)text_arena_take(hardlink_io_size);
                hardlink_read_two =
                    (p8 address_to)text_arena_take(hardlink_io_size);
                if (!hardlink_read_one || !hardlink_read_two ||
                    !shell_array_room(hardlink_order, hardlink_order_room,
                                      hardlink_file_count) ||
                    !shell_array_room(hardlink_spare, hardlink_spare_room,
                                      hardlink_file_count))
                        return string_report(log_error, 1, "hardlink: out of memory while sorting files\n");
                for (positive i = 0; i < hardlink_file_count; i++)
                        hardlink_order[i] = i;

                hardlink_sort_hash = false;
                positive address_to sorted = array_merge_sort(
                    hardlink_order, hardlink_spare, hardlink_file_count,
                    hardlink_index_compare);
                if (sorted != hardlink_order)
                        memory_copy_apart(hardlink_order, sorted,
                                          hardlink_file_count * sizeof(positive));

                for (positive first = 0; first < hardlink_file_count;)
                {
                        positive after = first + 1;
                        while (after < hardlink_file_count &&
                               !hardlink_index_compare(hardlink_order[first],
                                                       hardlink_order[after]))
                                after++;
                        if (after - first > 1)
                                for (positive i = first; i < after; i++)
                                        hardlink_hash_one(
                                            hardlink_files + hardlink_order[i]);
                        first = after;
                }

                hardlink_sort_hash = true;
                sorted = array_merge_sort(hardlink_order, hardlink_spare,
                                          hardlink_file_count,
                                          hardlink_index_compare);
                if (sorted != hardlink_order)
                        memory_copy_apart(hardlink_order, sorted,
                                          hardlink_file_count * sizeof(positive));

                /* Preserve established link groups.  This is a secondary
                   ordering only: reset it before finding the metadata/hash
                   boundaries so different nlink counts remain candidates. */
                hardlink_sort_links = true;
                sorted = array_merge_sort(hardlink_order, hardlink_spare,
                                          hardlink_file_count,
                                          hardlink_index_compare);
                if (sorted != hardlink_order)
                        memory_copy_apart(hardlink_order, sorted,
                                          hardlink_file_count * sizeof(positive));
                hardlink_sort_links = false;

                for (positive first = 0; first < hardlink_file_count;)
                {
                        positive after = first + 1;
                        while (after < hardlink_file_count &&
                               !hardlink_index_compare(hardlink_order[first],
                                                       hardlink_order[after]))
                                after++;

                        for (positive i = first + 1; i < after; i++)
                        {
                                hardlink_file address_to duplicate =
                                    hardlink_files + hardlink_order[i];
                                if (!duplicate->valid || !duplicate->hashed)
                                        continue;

                                hardlink_file address_to keep = null;
                                for (positive candidate = first; candidate < i;
                                     candidate++)
                                {
                                        hardlink_file address_to possible =
                                            hardlink_files +
                                            hardlink_order[candidate];
                                        if (possible->valid && possible->hashed &&
                                            hardlink_metadata_same(possible,
                                                                   duplicate) &&
                                            possible->hash == duplicate->hash &&
                                            (file_same_identity(
                                                 address_of possible->facts,
                                                 address_of duplicate->facts) ||
                                             hardlink_equal(possible,
                                                            duplicate)))
                                        {
                                                keep = possible;
                                                break;
                                        }
                                }
                                if (!keep)
                                        continue;

                                if (listing)
                                {
                                        hardlink_list_pair(keep, duplicate,
                                                           delimiter);
                                        continue;
                                }
                                if (file_same_identity(address_of keep->facts,
                                                       address_of duplicate->facts))
                                        continue;

                                bool changed = dry || hardlink_replace(keep,
                                                                       duplicate);
                                if (!changed)
                                {
                                        hardlink_status = 1;
                                        duplicate->valid = false;
                                        continue;
                                }
                                hardlink_linked++;
                                hardlink_saved += duplicate->facts.size;
                                if (verbose && !quiet)
                                        string_format(log,
                                                      "Linking %s to %s (-%b B)\n",
                                                      hardlink_path(keep),
                                                      hardlink_path(duplicate),
                                                      duplicate->facts.size);
                        }
                        first = after;
                }
        }

        if (!quiet && !listing)
                string_format(log,
                              "Mode:                     %s\n"
                              "Method:                   checked-read\n"
                              "Files:                    %b\n"
                              "Linked:                   %b files\n"
                              "Saved:                    %b B\n",
                              dry ? (string_address)"dry-run"
                                  : (string_address)"real",
                              hardlink_file_count, hardlink_linked,
                              hardlink_saved);
        log_flush();
        return hardlink_status;
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

        return string_report(log_error, false, "shred: '%s': sync failed: %s\n", path,
                      file_reason(done));
}

static bool shred_pass(bipolar handle, string_address path, positive length,
                       bool zero, file_random_state address_to random)
{
        if (system_seek(handle, 0, FILE_SEEK_SET) < 0)
                return string_report(log_error, false, "shred: '%s': seek failed\n", path);

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
                        return string_report(log_error, false, "shred: '%s': write failed\n",
                                      path);

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
                return string_report(log_error, false,
                                     "shred: %s: failed to open for writing: %s\n", path,
                                     file_reason(handle));

        file_facts facts;
        bool good = file_look(handle, (string_address) "", AT_EMPTY_PATH,
                              address_of facts);

        if (!good || (facts.mode & MODE_FORMAT) != MODE_FILE)
        {
                string_format(log_error,
                              "shred: '%s': refusing non-regular file\n", path);
                system_close(handle);
                return false;
        }

        positive length;

        if (size_given)
                length = requested;
        else if (facts.size > positive_max)
        {
                string_format(log_error, "shred: '%s': file is too large\n", path);
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
                                string_format(log_error,
                                              "shred: '%s': size overflow\n", path);
                                system_close(handle);
                                return false;
                        }

                        length += add;
                }
        }

        file_random_state random;

        if (iterations && good && !file_random_seed(address_of random))
        {
                string_format(log_error,
                              "shred: '%s': kernel randomness unavailable\n", path);
                good = false;
        }

        //      The zero pass is one of the passes and is counted with
        //      them, which is how the reference numbers them: -n1 -z is
        //      pass 1/2 random and pass 2/2 of zeroes.
        positive passes = iterations + (zero ? 1 : 0);

        for (positive pass = 0; pass < iterations && good; pass++)
        {
                if (verbose)
                        string_format(log_error,
                                      "shred: %s: pass %p/%p (random)...\n",
                                      path, pass + 1, passes);

                good = shred_pass(handle, path, length, false,
                                  address_of random);
        }

        if (zero && good)
        {
                if (verbose)
                        string_format(log_error, "shred: %s: pass %p/%p (000000)...\n",
                                      path, passes, passes);

                good = shred_pass(handle, path, length, true, null);
        }

        if (remove && good)
        {
                bipolar shortened = system_truncate_handle(handle, 0);

                if (shortened < 0)
                {
                        string_format(log_error,
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
                string_format(log_error, "shred: '%s': close failed: %s\n", path,
                              file_reason(closed));
                good = false;
        }

        if (remove && good)
        {
                file_facts named;

                if (!file_look_at(path, address_of named) ||
                    !file_same_identity(address_of facts, address_of named))
                {
                        string_format(log_error,
                                      "shred: '%s': name changed; refusing removal\n",
                                      path);
                        good = false;
                }
                else
                {
                        bipolar gone = system_remove_at(AT_FDCWD, path, 0);

                        if (gone < 0)
                        {
                                string_format(log_error,
                                              "shred: '%s': cannot remove: %s\n",
                                              path, file_reason(gone));
                                good = false;
                        }
                        else if (verbose)
                                string_format(log_error, "shred: %s: removed\n",
                                              path);
                }
        }

        return good;
}

/*
        Every word an option carries is read where the option is written.

        The reference is a getopt loop: a pass count that is not a count, a
        size that is not a size and a --remove that names no removal it knows
        are each reported as that option is reached, so of two bad words the
        first one written is the one reported. What this shred will not do --
        wipe a name before unlinking it, take randomness from a file -- is
        said afterwards, because the reference has nothing to say there and
        the order can only be ours.
*/
static const file_word shred_removals[] = {
    {(string_address) "unlink", 'u', false},
    {(string_address) "wipe", 'w', false},
    {(string_address) "wipesync", 's', false},
};

static positive shred_iterations;
static positive shred_asked_size;
static p8 shred_removal;

static bool shred_option_seen(p8 letter, string_address value)
{
        if (letter == 'n' && value &&
            !file_unsigned_decimal(value, address_of shred_iterations))
                return string_report(log_error, false,
                                     "shred: invalid number of passes: '%s'\n", value);

        if (letter == 's' && value && !shred_size(value, address_of shred_asked_size))
                return string_report(log_error, false,
                                     "shred: invalid file size: '%s'\n", value);

        if (letter == 'u' && value)
        {
                b32 which = file_word_among((string_address) "shred",
                                            (string_address) "--remove", value,
                                            shred_removals,
                                            array_count(shred_removals));

                if (which < 0)
                        return false;

                shred_removal = (p8)which;
        }

        return true;
}

static b32 file_shred()
{
        file_operands_begin();

        shred_iterations = 3;
        shred_asked_size = 0;
        shred_removal = 'u';

        file_taking taking = {
            .program = (string_address) "shred",
            .allowed = (string_address) "fnsuvxz",
            .valued = (string_address) "nRs",
            .long_optional = (string_address) "u",
            .longs = shred_longs,
            .operand = file_operand,
            .seen = shred_option_seen,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;
        if (!file_operand_count)
                return string_report(log_error, 1, "%s: missing file operand\n", (string_address) "shred");

        if (file_option_value(address_of taking, 'R'))
                return string_report(log_error, 1, "shred: --random-source is unsupported; kernel randomness is mandatory\n");

        if (shred_removal != 'u')
                return string_report(log_error, 1, "shred: filename wiping modes are unsupported; use --remove=unlink\n");

        positive iterations = shred_iterations;
        positive size = shred_asked_size;
        string_address size_text = file_option_value(address_of taking, 's');

        positive flags = taking.flags;
        bool remove = (flags & FILE_FLAG('u')) != 0;
        b32 status = 0;

        //      -u on its own asks the reference for a wiping removal, and
        //      this one only unlinks; --remove=unlink asked for what it does.
        if (remove && !file_option_value(address_of taking, 'u'))
                log_error("shred: warning: -u uses unlink removal without filename wiping\n",
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
        return buffered_flush((positive)output->handle, file_transfer,
                               address_of output->used) || string_report(log_error, false, "shuf: write error%s%s\n", output->name ? (string_address)" on " : (string_address)"", output->name ? output->name : (string_address)"");
}

static bool shuf_output_send(shuf_output address_to output,
                             string_address bytes, positive length)
{
        return buffered_write((positive)output->handle, file_transfer,
                               sizeof(file_transfer), address_of output->used,
                               bytes, length) || string_report(log_error, false, "shuf: write error%s%s\n", output->name ? (string_address)" on " : (string_address)"", output->name ? output->name : (string_address)"");
}

static bool shuf_output_record(shuf_output address_to output,
                               shuf_record address_to record, p8 delimiter)
{
        if (record->length < sizeof(file_transfer))
        {
                p8 address_to bytes = buffered_reserve(
                    (positive)output->handle, file_transfer, sizeof(file_transfer),
                    address_of output->used, record->length + 1);
                if (!bytes)
                        return string_report(log_error, false, "shuf: write error%s%s\n", output->name ? (string_address)" on " : (string_address)"", output->name ? output->name : (string_address)"");
                memory_copy_apart(bytes, record->text, record->length);
                bytes[record->length] = delimiter;
                return true;
        }
        return shuf_output_send(output, record->text, record->length) &&
               (buffered_write_byte((positive)output->handle, file_transfer,
                                     sizeof(file_transfer), address_of output->used,
                                     delimiter) || string_report(log_error, false, "shuf: write error%s%s\n", output->name ? (string_address)" on " : (string_address)"", output->name ? output->name : (string_address)""));
}

static bool shuf_output_number(shuf_output address_to output, positive number,
                               p8 delimiter)
{
        p8 text[32];
        positive length = positive_into_base(text, number, 10, false);
        text[length++] = delimiter;
        return shuf_output_send(output, text, length);
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
                string_format(log_error, "shuf: %s: %s\n", name,
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
                log_error(read_failed ? (string_address) "shuf: read error\n"
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

/*
        -n and -i are read as they are written, not once at the end.

        Every -n is a ceiling and the smallest of them holds -- shuf -n5 -n2
        writes two lines and so does shuf -n2 -n5 -- so one value per letter
        cannot say what was asked for. Both are also refused where they are
        written: a second -i is a second answer to one question, and a count
        or a range that is not one is refused even where a later option would
        have replaced it.
*/
static positive shuf_wanted;
static bool shuf_limited;
static bool shuf_ranged;

static bool shuf_seen(p8 letter, string_address value)
{
        if (letter == 'n' && value)
        {
                positive lines;

                if (!file_unsigned_decimal(value, address_of lines))
                        return string_report(log_error, false,
                                             "shuf: invalid line count: '%s'\n", value);

                if (!shuf_limited || lines < shuf_wanted)
                        shuf_wanted = lines;

                shuf_limited = true;
        }

        if (letter == 'i' && value)
        {
                positive low;
                positive high;

                //      A second -i is refused before the word it carries is
                //      read, the way the reference refuses it: two ranges is
                //      the complaint whether or not the second one parses.
                if (shuf_ranged)
                        return string_report(log_error, false,
                                             "shuf: multiple -i options specified\n");

                if (!shuf_range(value, address_of low, address_of high) ||
                    high - low == positive_max)
                        return string_report(log_error, false,
                                             "shuf: invalid input range: '%s'\n", value);

                shuf_ranged = true;
        }

        return true;
}

static b32 file_shuf()
{
        file_operands_begin();
        shuf_wanted = 0;
        shuf_limited = false;
        shuf_ranged = false;
        file_taking taking = {
            .program = (string_address) "shuf",
            .allowed = (string_address) "einorz",
            .valued = (string_address) "inoR",
            .longs = shuf_longs,
            .operand = file_operand,
            .seen = shuf_seen,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;
        if (file_option_value(address_of taking, 'R'))
                return string_report(log_error, 1, "shuf: --random-source is unsupported; kernel-seeded randomness is mandatory\n");

        positive flags = taking.flags;
        bool echo = (flags & FILE_FLAG('e')) != 0;
        bool repeat = (flags & FILE_FLAG('r')) != 0;
        string_address range_text = file_option_value(address_of taking, 'i');

        if (echo && range_text)
                return string_report(log_error, 1, "shuf: cannot combine -e and -i options\n");
        if (range_text && file_operand_count)
                return string_report(log_error, 1, "shuf: extra operand '%s'\n",
                                     file_operand_at(0));
        if (!echo && !range_text && file_operand_count > 1)
        {
                string_format(log_error, "shuf: extra operand '%s'\n",
                              file_operand_at(1));
                return 1;
        }

        positive wanted = shuf_wanted;
        bool limited = shuf_limited;

        //      No lines are wanted, so no input is read: a file that is not
        //      there or cannot be read is never opened, and the answer is a
        //      successful nothing.
        if (limited && !wanted)
                return 0;

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
                        string_format(log_error, "shuf: invalid input range: '%s'\n",
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
                log_error("shuf: no lines to repeat\n", 0);
                text_arena_used = 0;
                return 1;
        }

        bool need_random = count > 1 && (!limited || wanted);
        file_random_state random;

        if (need_random && !file_random_seed(address_of random))
        {
                log_error("shuf: kernel randomness unavailable\n", 0);
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
                        string_format(log_error, "shuf: %s: %s\n",
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
                string_format(log_error, "shuf: close failed on '%s'\n",
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
        //      The reference writes a colon inside a key or a value with a
        //      backslash in front of it. This reader has no escape -- the
        //      one scanner that splits an LS_COLORS table cuts at every
        //      colon there is, for ls as well as here -- so such a table is
        //      refused rather than written and then misread.
        if ((memory_first_of(key, ':', key_length) ||
             memory_first_of(value, ':', value_length)))
                return string_report(log_error, false, "dircolors: ':' in keys or values is unsupported by the shared LS_COLORS grammar\n");

        positive prefix = extension && string_is(key, '.') ? 1 : 0;

        if (key_length > positive_max - value_length - 2 - prefix ||
            builder->used >= builder->room ||
            key_length + value_length + 2 + prefix >=
                builder->room - builder->used)
                return string_report(log_error, false, "dircolors: translated table is too large\n");

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
                string_format(log_error, "dircolors: %s: embedded NUL byte\n",
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
                log_error("dircolors: configuration is too large\n", 0);
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
                        string_format(log_error,
                                      "dircolors: %s:%p: missing second token\n",
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
                                string_format(log_error,
                                              "dircolors: %s:%p: terminal pattern is too long\n",
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
                log_error("dircolors: configuration cannot be represented by LS_COLORS\n",
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

//      Every dircolors refusal is its own sentence and then the line that
//      sends the reader on, which is one shape written once.
static b32 dircolors_refused(string_address sentence)
{
        string_format(log_error, "dircolors: %s", sentence);
        log_error("Try 'dircolors --help' for more information.\n", 0);
        return 1;
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

        positive flags = taking.flags;
        bool print_database = (flags & FILE_FLAG('p')) != 0;
        bool print_table = (flags & FILE_FLAG('L')) != 0;

        //      The reference refuses these in this order and with these
        //      words, and sends the reader on to --help after each.
        if ((print_database || print_table) && dircolors_shell_option)
                return dircolors_refused((string_address)
                    "the options to output non shell syntax,\n"
                    "and to select a shell syntax are mutually exclusive\n");

        if (print_database && print_table)
                return dircolors_refused((string_address)
                    "options --print-database and --print-ls-colors are "
                    "mutually exclusive\n");

        if (file_operand_count > 1)
        {
                string_format(log_error, "dircolors: extra operand '%s'\n",
                              file_operand_at(1));
                log_error("Try 'dircolors --help' for more information.\n", 0);
                return 1;
        }

        if (print_database && file_operand_count)
        {
                string_format(log_error, "dircolors: extra operand '%s'\n",
                              file_operand_at(0));
                log_error("file operands cannot be combined with "
                          "--print-database (-p)\n", 0);
                log_error("Try 'dircolors --help' for more information.\n", 0);
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
                        string_format(log_error, "dircolors: %s: %s\n", name,
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
                        string_format(log_error,
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
static const file_long rmdir_longs[] = {
    {(string_address) "ignore-fail-on-non-empty", 'I'},
    {(string_address) "parents", 'p'},
    {(string_address) "verbose", 'v'},
    {null, 0},
};

static b32 file_rmdir()
{
        positive count = (positive)program_argument_count();
        file_taking taking = {
            .program = (string_address) "rmdir",
            .allowed = (string_address) "pvI",
            .valued = (string_address) "",
            .longs = rmdir_longs,
        };

        if (!file_take(address_of taking))
                return 1;

        positive flags = taking.flags;
        positive first = taking.first;

        if (first >= count)
                return string_report(log_error, 1, "%s: missing operand\n", (string_address) "rmdir");

        b32 status = 0;

        while (first < count)
        {
                string_address path = program_argument((b32)first++);
                p8 parent[FILE_PATH_MAX];
                p8 above[FILE_PATH_MAX];

                bool named = true;

                while (1)
                {
                        if (flags & FILE_FLAG('v'))
                                string_format(log, "rmdir: removing directory, '%s'\n", path);

                        bipolar gone = system_remove_at(AT_FDCWD, path,
                                                         AT_REMOVEDIR);

                        if (gone < 0)
                        {
                                if ((flags & FILE_FLAG('I')) && gone == -ERROR_NOT_EMPTY)
                                        break;

                                // A name that ends in a slash and is a link
                                // named a directory that was never followed.
                                positive length = string_length(path);

                                //      The operand is reported as a name and
                                //      a parent walked up to as a directory,
                                //      which is how the reference's two
                                //      messages differ.
                                string_format(log_error,
                                              named ? "rmdir: failed to remove '%s': %s\n"
                                                    : "rmdir: failed to remove directory '%s': %s\n",
                                              path,
                                              gone == -ERROR_NOT_DIRECTORY && length &&
                                                      path[length - 1] == '/'
                                                  ? (string_address) "Symbolic link not followed"
                                                  : file_reason(gone));
                                status = 1;
                                break;
                        }

                        if (!(flags & FILE_FLAG('p')))
                                break;

                        //      A parent is what is left when the last
                        //      component is cut off, and there is one for as
                        //      long as a slash is left: the reference walks
                        //      up to '.' and to '/' and asks the kernel about
                        //      them too.
                        string_copy_max_end(parent, path, FILE_PATH_MAX - 1);

                        positive cut = string_length(parent);

                        while (cut && parent[cut - 1] == '/')
                                cut--;

                        while (cut && parent[cut - 1] != '/')
                                cut--;

                        if (!cut)
                                break;

                        while (cut > 1 && parent[cut - 1] == '/')
                                cut--;

                        memory_copy_apart_end(above, parent, cut);
                        path = above;
                        named = false;
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
static bool cp_attributes_only;
static bool cp_replace;
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
static bool mv_across_said;
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
/*
        -b and --backup: what happens to what was already there.

        none    nothing is kept
        simple  one backup, under the suffix -S names, ~ by default
        numbered  name.~1~, ~2~ and so on, however many are already there
        existing  numbered when a numbered one is already there, simple when
                  it is not, which is what -b means on its own

        The name is moved rather than copied, so a backup costs nothing and
        the file that was there keeps its inode.
*/
static p8 file_backup_kind;
static string_address file_backup_suffix;

static bool file_backup_control(string_address program, string_address word)
{
        if (!word || !string_compare(word, "existing") || !string_compare(word, "nil"))
                file_backup_kind = 'e';
        else if (!string_compare(word, "none") || !string_compare(word, "off"))
                file_backup_kind = 0;
        else if (!string_compare(word, "simple") || !string_compare(word, "never"))
                file_backup_kind = 's';
        else if (!string_compare(word, "numbered") || !string_compare(word, "t"))
                file_backup_kind = 'n';
        else
        {
                string_format(log_error,
                              "%s: invalid argument '%s' for 'backup type'\n"
                              "Valid arguments are:\n"
                              "  - 'none', 'off'\n"
                              "  - 'simple', 'never'\n"
                              "  - 'existing', 'nil'\n"
                              "  - 'numbered', 't'\n",
                              program, word);
                return false;
        }

        return true;
}

// The numbered backups already beside a name say whether the next one is
// numbered too, and which number it takes.
static positive file_backup_number(string_address destination)
{
        p8 candidate[FILE_PATH_MAX];
        positive at = 1;

        for (;;)
        {
                if (string_length(destination) + 16 >= FILE_PATH_MAX)
                        return 0;

                positive length = string_length(destination);

                memory_copy_apart(candidate, destination, length);
                candidate[length++] = '.';
                candidate[length++] = '~';
                length += positive_into_string(candidate + length, at);
                candidate[length++] = '~';
                candidate[length] = end;

                if (!file_exists(AT_FDCWD, candidate))
                        return at;

                at++;
        }
}

static bool file_backup_made(string_address program, string_address destination)
{
        p8 kept[FILE_PATH_MAX];
        positive length = string_length(destination);

        if (!file_backup_kind || !file_exists(AT_FDCWD, destination))
                return true;

        p8 kind = file_backup_kind;

        if (kind == 'e')
                kind = file_backup_number(destination) > 1 ? 'n' : 's';

        if (kind == 'n')
        {
                positive at = file_backup_number(destination);
                positive used = length;

                if (length + 16 >= FILE_PATH_MAX)
                        return true;

                memory_copy_apart(kept, destination, length);
                kept[used++] = '.';
                kept[used++] = '~';
                used += positive_into_string(kept + used, at);
                kept[used++] = '~';
                kept[used] = end;
        }
        else
        {
                positive suffix = string_length(file_backup_suffix);

                if (length + suffix >= FILE_PATH_MAX)
                        return true;

                memory_copy_apart(kept, destination, length);
                memory_copy_apart_end(kept + length, file_backup_suffix, suffix);
        }

        bipolar moved = system_rename_at(AT_FDCWD, destination, AT_FDCWD, kept, 0);

        if (moved < 0)
        {
                string_format(log_error, "%s: cannot backup '%s': %s\n", program,
                              destination, file_reason(moved));
                return false;
        }

        return true;
}

// Every program that takes -b reads it the same way, so the reading is
// here rather than four times over.
static bool file_backup_taken(file_taking address_to taking, string_address program)
{
        file_backup_kind = 0;
        (void)program;
        file_backup_suffix = file_option_value(taking, 'S');

        if (!file_backup_suffix)
        {
                file_backup_suffix = file_environment((string_address) "SIMPLE_BACKUP_SUFFIX");

                if (!file_backup_suffix || !string_get(file_backup_suffix))
                        file_backup_suffix = (string_address) "~";
        }

        if (taking->flags & FILE_FLAG('b'))
                file_backup_kind = 'e';

        if (taking->flags & FILE_FLAG('B'))
        {
                string_address control = file_option_value(taking, 'B');

                if (!control)
                        control = file_environment((string_address) "VERSION_CONTROL");

                if (!file_backup_control(program, control))
                        return false;
        }

        return true;
}

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
                string_format(log_error, "%s: cannot create special file '%s': %s\n",
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
                        return string_report(log_error, false,
                                      "cp: %s: can make relative symbolic links only in current directory\n",
                                      destination);

                done = system_symbolic_link_at(source, AT_FDCWD, destination);
        }
        else
                done = system_link_at(AT_FDCWD, source, AT_FDCWD, destination, 0);

        if (done < 0)
                return string_report(log_error, false, "cp: cannot create link '%s': %s\n",
                              destination, file_reason(done));

        if (cp_loud)
                string_format(log, "'%s' -> '%s'\n", source, destination);

        return true;
}

static bool file_move_remove(string_address source, positive flags)
{
        bipolar gone = system_remove_at(AT_FDCWD, source, flags);
        if (gone < 0)
        {
                string_format(log_error, "mv: cannot remove '%s': %s\n", source,
                              file_reason(gone));
                mv_across_said = true;
        }
        return gone == 0;
}

/* cp and cross-device mv copy the same object graph. Only source removal,
   dereferencing, overwrite policy and metadata policy differ. */
static bool file_copy_one(string_address source, string_address destination,
                           positive depth, bool named, bool moving)
{
        file_facts facts;
        file_facts there;
        string_address program = moving ? (string_address)"mv" : (string_address)"cp";
        bool follow = !moving &&
                      (cp_dereference == 1 || (cp_dereference == 2 && named));
        bipolar looked = file_look_code(AT_FDCWD, source,
                                        follow ? 0 : AT_SYMLINK_NOFOLLOW,
                                        address_of facts);

        if (looked < 0)
        {
                if (!moving)
                        string_format(log_error, "cp: cannot stat '%s': %s\n", source,
                                      file_reason(looked));
                return false;
        }
        if (moving && !depth)
                return false;

        positive kind = facts.mode & MODE_FORMAT;

        bool destination_exists = !moving && (kind == MODE_LINK && !follow
                                      ? file_look_link(destination, address_of there)
                                      : file_look_at(destination, address_of there));

        if (destination_exists && file_same_identity(address_of facts, address_of there))
                return string_report(log_error, false, "cp: '%s' and '%s' are the same file\n", source,
                              destination);

        // A destination that is a link to nothing would be written through,
        // making a file wherever the link points; the reference refuses that
        // unless -f says to replace what is in the way.
        if (!moving && !destination_exists && !cp_force && kind != MODE_DIRECTORY &&
            file_look_link(destination, address_of there) &&
            (there.mode & MODE_FORMAT) == MODE_LINK)
        {
                string_format(log_error, "cp: not writing through dangling symlink '%s'\n",
                              destination);
                return false;
        }

        if (!moving && kind == MODE_DIRECTORY)
        {
                p8 from[FILE_PATH_MAX];
                p8 to[FILE_PATH_MAX];

                // Both sides followed all the way: a destination spelled
                // through a link into the source is still inside it.
                if (file_resolve(source, from, true) &&
                    file_resolve(destination, to, true) && realpath_under(from, to))
                        return string_report(log_error, false,
                                      "cp: cannot copy a directory, '%s', into itself, '%s'\n",
                                      source, destination);
        }

        if (!moving && kind != MODE_DIRECTORY &&
            !cp_allowed(destination, address_of facts))
                return true;

        if (!moving && (cp_hard || cp_symbolic) && kind != MODE_DIRECTORY)
                return cp_linked(source, destination);

        if (kind == MODE_LINK)
        {
                p8 target[FILE_PATH_MAX];

                if (file_link_text(source, target, FILE_PATH_MAX) < 0)
                        return false;

                system_remove_at(AT_FDCWD, destination, 0);

                if (system_symbolic_link_at(target, AT_FDCWD, destination) < 0)
                {
                        if (!moving)
                                string_format(log_error, "cp: cannot create link '%s'\n", destination);
                        return false;
                }

                goto copied;
        }

        // -r copies a tree, and a pipe or a device in a tree is part of its
        // shape rather than a stream to drain; whatever stood at the
        // destination is taken away first, as the reference cp does for a
        // source that is not a regular file.
        if ((moving || cp_recursive) && kind != MODE_DIRECTORY && kind != MODE_FILE)
        {
                system_remove_at(AT_FDCWD, destination, 0);

                if (!file_make_alike(program, destination, address_of facts))
                {
                        mv_across_said |= moving;
                        return false;
                }

                goto copied;
        }

        if (!moving && cp_attributes_only && kind != MODE_DIRECTORY)
        {
                bipolar made = system_open_at_mode(AT_FDCWD, destination,
                                                   FILE_WRITE & ~O_TRUNC,
                                                   facts.mode & 07777);

                if (made < 0)
                {
                        string_format(log_error, "cp: cannot create regular file '%s': %s\n",
                                      destination, file_reason(made));
                        return false;
                }

                system_close(made);
                goto copied;
        }

        if (kind != MODE_DIRECTORY)
        {
                // The open creates a file with the source's mode under the
                // umask, and a destination that was already there keeps the
                // mode it had: both are what the reference cp leaves, and -p
                // is what asks for the source's mode whole.
                bool copied = file_copy_contents(
                    AT_FDCWD, source, AT_FDCWD, destination, facts.mode & 07777);
                if (!copied && !moving && cp_force)
                {
                        // -f retries by replacing an unwritable destination.
                        system_remove_at(AT_FDCWD, destination, 0);
                        copied = file_copy_contents(
                            AT_FDCWD, source, AT_FDCWD, destination, facts.mode & 07777);
                }
                if (!copied)
                {
                        if (!moving)
                        {
                                // The copy says only that it failed; the open
                                // says why, and a probe that succeeds is
                                // taken away again before anyone sees it.
                                bipolar probe = system_open_at_mode(
                                    AT_FDCWD, destination, FILE_WRITE, facts.mode & 07777);

                                if (probe >= 0)
                                {
                                        system_close(probe);
                                        string_format(log_error, "cp: cannot copy '%s'\n", source);
                                }
                                else
                                        string_format(log_error,
                                                      "cp: cannot create regular file '%s': %s\n",
                                                      destination, file_reason(probe));
                        }
                        return false;
                }

                goto copied;
        }

        if (!moving && !cp_recursive)
                return string_report(log_error, false, "cp: -r not specified; omitting directory '%s'\n",
                              source);

        if (depth == 0)
                return string_report(log_error, false, "cp: '%s' is nested too deep\n", source);

        // The directory is made with the owner able to write into it whatever
        // the source allowed, so a read-only tree can still be filled, and is
        // given its final mode once everything is inside.
        bipolar made = system_make_directory_at(
            AT_FDCWD, destination, (facts.mode & 07777) | 0700);

        if (made < 0 && made != -ERROR_EXISTS)
        {
                string_format(log_error, "%s: cannot create directory '%s': %s\n",
                              program, destination, file_reason(made));
                mv_across_said |= moving;
                return false;
        }

        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, source))
        {
                string_format(log_error, "%s: cannot read directory '%s': %s\n",
                              program, source, file_reason(walk.handle));
                if (moving)
                {
                        mv_across_said = true;
                        file_keep(destination, address_of facts);
                }
                return false;
        }

        if (!moving && cp_loud)
                string_format(log, "'%s' -> '%s'\n", source, destination);

        bool complete = true;
        positive skipped = 0;
        p8 from[FILE_PATH_MAX];
        p8 to[FILE_PATH_MAX];

        while (file_walk_pair(address_of walk, program, source,
                              destination, from, to, address_of skipped))
        {
                if (!file_copy_one(from, to, depth - 1, false, moving))
                        complete = false;
        }

        if (walk.error < 0)
        {
                string_format(log_error, "%s: cannot read directory '%s': %s\n",
                              program, source, file_reason(walk.error));
                mv_across_said |= moving;
                complete = false;
        }
        file_walk_close(address_of walk);

        complete &= !skipped;
        if (moving && complete)
                complete = file_move_remove(source, AT_REMOVEDIR);

        // A directory that was already there keeps its mode, as a file does;
        // one made here gets the source's under the umask, unless -p wants
        // the source's whole.
        if (moving || cp_preserve)
                file_keep(destination, address_of facts);
        else if (made == 0)
                system_change_mode_at(AT_FDCWD, destination,
                                      facts.mode & 07777 & ~cp_umask);

        return complete;

copied:
        if (moving || cp_preserve)
                file_keep(destination, address_of facts);
        if (!moving)
        {
                if (cp_loud)
                        string_format(log, "'%s' -> '%s'\n", source, destination);
                return true;
        }

        return file_move_remove(source, 0);
}

// file_copy_one carries the walk depth and whether the name was written on the
// command line; the pair walker cp shares with mv carries neither, and every
// pair it hands over is a named one at full depth.
static fn cp_pair(string_address source, string_address destination)
{
        if (!file_backup_made((string_address) "cp", destination))
        {
                cp_status = 1;
                return;
        }

        if (cp_replace)
                system_remove_at(AT_FDCWD, destination, 0);

        if (!file_copy_one(source, destination, FILE_MAX_DEPTH, true, false))
                cp_status = 1;
}

static const file_word cp_preserve_words[] = {
    {"mode", 'm'}, {"timestamps", 't'}, {"ownership", 'o'}, {"links", 'l'},
    {"context", 'c'}, {"xattr", 'x'}, {"all", 'a'}};
static const file_word cp_sparse_words[] = {
    {"never", 'n'}, {"auto", 'a'}, {"always", 'A'}};
static const file_word cp_reflink_words[] = {
    {"auto", 'a'}, {"always", 'A'}, {"never", 'n'}};

// Each of the four takes a comma-separated list, and every word in it has
// to be one the option knows.
static bool cp_words_read(string_address option, string_address value,
                          const file_word address_to words, positive count,
                          bool address_to context)
{
        p8 one[64];

        if (!value)
                return true;

        for (positive at = 0; value[at];)
        {
                positive length = 0;

                while (value[at] && value[at] != ',' && length + 1 < sizeof(one))
                        one[length++] = value[at++];

                one[length] = end;

                if (value[at] == ',')
                        at++;

                b32 answer = file_word_among((string_address) "cp", option, one, words, count);

                if (answer < 0)
                        return false;

                if (context)
                        address_to context |= answer == 'c';
        }

        return true;
}

static const file_long cp_longs[] = {
    {(string_address) "archive", 'a'},
    {(string_address) "attributes-only", 'A'},
    {(string_address) "backup", 'B'},
    {(string_address) "context", 'Z'},
    {(string_address) "copy-contents", 'C'},
    {(string_address) "debug", 'v'},
    {(string_address) "keep-directory-symlink", 'K'},
    {(string_address) "no-preserve", 'N'},
    {(string_address) "one-file-system", 'x'},
    {(string_address) "parents", 'e'},
    {(string_address) "reflink", 'k'},
    {(string_address) "remove-destination", 'D'},
    {(string_address) "sparse", 'z'},
    {(string_address) "strip-trailing-slashes", 'w'},
    {(string_address) "suffix", 'S'},
    {(string_address) "update", 'u'},
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

        file_targets_begin();

        file_taking taking = {
            .program = (string_address) "cp",
            .allowed = (string_address) "aAbCdDefHiklLnNpPrRsStTuvwxzZ",
            .valued = (string_address) "tSNz",
            .long_optional = (string_address) "BkupZ",
            .longs = cp_longs,
            .seen = file_target_seen,
            .supersedes = cp_supersedes,
        };

        if (!file_take(address_of taking))
                return 1;

        if (!file_backup_taken(address_of taking, (string_address) "cp"))
                return 1;

        if (!file_targets_told((string_address) "cp"))
                return 1;

        bool wants_context = false;

        if (!cp_words_read((string_address) "--preserve",
                           file_option_value(address_of taking, 'p'),
                           cp_preserve_words, array_count(cp_preserve_words),
                           address_of wants_context) ||
            !cp_words_read((string_address) "--no-preserve",
                           file_option_value(address_of taking, 'N'),
                           cp_preserve_words, array_count(cp_preserve_words), null) ||
            !cp_words_read((string_address) "--sparse",
                           file_option_value(address_of taking, 'z'),
                           cp_sparse_words, array_count(cp_sparse_words), null) ||
            !cp_words_read((string_address) "--reflink",
                           file_option_value(address_of taking, 'k'),
                           cp_reflink_words, array_count(cp_reflink_words), null))
                return 1;

        //      This image has no SELinux. -Z and a bare --context are
        //      no-ops there, and a named context is warned about and
        //      otherwise ignored; cp says "SELinux-enabled" where mkdir and
        //      mknod say "SELinux/SMACK-enabled". The reference warns after
        //      it has read the option values and before it weighs them
        //      against one another, so this sits between the two.
        if (file_option_value(address_of taking, 'Z'))
                log_error("cp: warning: ignoring --context; it requires an "
                          "SELinux-enabled kernel\n", 0);

        // A label asked for by name is one this kernel cannot give.
        if (wants_context)
        {
                log_error("cp: cannot preserve security context without an "
                          "SELinux-enabled kernel\n", 0);
                return 1;
        }

        //      A copy is one kind of link or the other, never both.
        if ((taking.flags & FILE_FLAG('l')) && (taking.flags & FILE_FLAG('s')))
        {
                log_error("cp: cannot make both hard and symbolic links\n", 0);
                log_error("Try 'cp --help' for more information.\n", 0);
                return 1;
        }

        cp_attributes_only = (taking.flags & FILE_FLAG('A')) != 0;
        cp_replace = (taking.flags & FILE_FLAG('D')) != 0;

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
    {(string_address) "backup", 'B'},
    {(string_address) "compare", 'C'},
    {(string_address) "create-leading-directories", 'D'},
    {(string_address) "suffix", 'S'},
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
                return string_report(log_error, false, "install: invalid %s '%s'\n",
                              group ? "group" : "user", text);

        address_to identity = found;
        return true;
}

static bool install_leading(string_address destination)
{
        p8 parent[FILE_PATH_MAX];

        path_head_copy(parent, FILE_PATH_MAX, destination);
        if (file_make_parents(parent, 0755))
                return true;

        return string_report(log_error, false,
                      "install: cannot create leading directories for '%s'\n",
                      destination);
}

static bool install_attributes(string_address destination,
                               file_facts address_to source)
{
        bipolar owned = install_owner >= 0 || install_group >= 0
                            ? system_change_owner_at(AT_FDCWD, destination, install_owner,
                                                     install_group, 0)
                            : 0;

        if (owned < 0)
        {
                string_format(log_error,
                              "install: cannot change ownership of '%s': %s\n",
                              destination, file_reason(owned));
                return false;
        }

        if (system_change_mode_at(AT_FDCWD, destination, install_mode) < 0)
                return string_report(log_error, false, "install: cannot change mode of '%s'\n",
                              destination);

        if (install_preserve && source)
        {
                p64 times[4];

                file_times_of(source, times);
                if (system_update_times_at(AT_FDCWD, destination, times, 0) < 0)
                        return string_report(log_error, false,
                                      "install: cannot preserve times of '%s'\n",
                                      destination);
        }

        return true;
}

static fn install_pair(string_address source, string_address destination)
{
        file_facts from;
        file_facts to;
        bipolar looked = file_look_code(AT_FDCWD, source, 0, address_of from);

        if (looked == 0 && (from.mode & MODE_FORMAT) == MODE_DIRECTORY)
        {
                string_format(log_error, "install: omitting directory '%s'\n", source);
                install_status = 1;
                return;
        }

        if (looked < 0 || (from.mode & MODE_FORMAT) != MODE_FILE)
        {
                string_format(log_error, "install: cannot stat '%s': %s\n",
                              source,
                              looked < 0 ? file_reason(looked)
                                         : (string_address)"Not a regular file");
                install_status = 1;
                return;
        }

        // The destination is the name, not what a link there points at: a
        // link is taken away and a file written in its place.
        if (file_look_link(destination, address_of to) &&
            file_same_identity(address_of from, address_of to))
        {
                string_format(log_error,
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

        if (!file_backup_made((string_address) "install", destination))
        {
                install_status = 1;
                return;
        }

        bipolar removed = system_remove_at(AT_FDCWD, destination, 0);

        if (removed < 0 && removed != -ERROR_NO_ENTRY)
        {
                string_format(log_error, "install: cannot remove '%s': %s\n",
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
                // The copy says only that it failed; asking the kernel again
                // for the destination is what names the reason, and a probe
                // that succeeds is taken away before anyone sees it.
                bipolar probe = system_open_at_mode(AT_FDCWD, destination,
                                                    FILE_WRITE | FILE_EXCLUSIVE, install_mode);

                if (probe >= 0)
                {
                        system_close(probe);
                        system_remove_at(AT_FDCWD, destination, 0);
                        string_format(log_error, "install: cannot copy '%s' to '%s'\n",
                                      source, destination);
                }
                else
                        string_format(log_error,
                                      "install: cannot create regular file '%s': %s\n",
                                      destination, file_reason(probe));

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
        file_targets_begin();

        file_taking taking = {
            .program = (string_address) "install",
            .allowed = (string_address) "bCDcdgmopSTtv",
            .valued = (string_address) "gmotS",
            .long_optional = (string_address) "B",
            .longs = install_longs,
            .seen = file_target_seen,
        };

        if (!file_targets_told((string_address) "install"))
                return 1;

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
                        return string_report(log_error, 1, "%s: missing operand\n", (string_address) "install");
                if (taking.first >= count)
                        return string_report(log_error, 1, "%s: missing operand\n", (string_address) "install");

                for (positive at = taking.first; at < count; at++)
                {
                        string_address path = program_argument((b32)at);

                        // Each level is named as it is made, which is what
                        // -v is for; a level already there is passed over.
                        if (install_loud)
                                for (positive cut = 0; path[cut]; cut++)
                                {
                                        if (path[cut] != '/' && path[cut + 1])
                                                continue;

                                        p8 step[FILE_PATH_MAX];
                                        positive length = path[cut] == '/' ? cut : cut + 1;

                                        if (!length || length >= FILE_PATH_MAX)
                                                continue;

                                        memory_copy_apart(step, path, length);
                                        step[length] = end;

                                        if (!file_exists(AT_FDCWD, step))
                                                string_format(log,
                                                              "install: creating directory '%s'\n",
                                                              step);
                                }

                        if (!file_make_parents(path, 0755) ||
                            !install_attributes(path, null))
                        {
                                string_format(log_error,
                                              "install: cannot create directory '%s'\n",
                                              path);
                                install_status = 1;
                        }
                }

                log_flush();
                return install_status;
        }

        if (install_parents && (into || count - taking.first != 2))
                return string_report(log_error, 1, "install: -D requires one source and one destination\n");

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
static bool mv_newer_only;
static b32 mv_status;
static bool mv_ask;
static bool mv_never_clobber;
static bool mv_loud;
static p8 mv_collision_option;

static const file_supersede mv_supersedes[] = {
    {(string_address) "fin", address_of mv_collision_option},
    {null, null},
};

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

        if (mv_newer_only)
        {
                file_facts from;
                file_facts to;

                if (file_look_at(source, address_of from) &&
                    file_look_at(destination, address_of to) &&
                    from.modified.seconds <= to.modified.seconds)
                        return;
        }

        if (!file_backup_made((string_address) "mv", destination))
        {
                mv_status = 1;
                return;
        }

        file_facts from;
        file_facts to;
        file_facts through;

        // The two names for one file, or a link named as the source that
        // points at the destination: renaming either would lose the file.
        // A link named as the destination is only what the rename replaces.
        if (file_look_link(source, address_of from) &&
            file_look_link(destination, address_of to) &&
            (file_same_identity(address_of from, address_of to) ||
             ((from.mode & MODE_FORMAT) == MODE_LINK &&
              file_look_at(source, address_of through) &&
              file_same_identity(address_of through, address_of to))))
        {
                string_format(log_error, "mv: '%s' and '%s' are the same file\n", source,
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

                if (file_copy_one(source, destination, FILE_MAX_DEPTH, true, true))
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

        if (done == -ERROR_INVALID)
        {
                p8 outer[FILE_PATH_MAX];
                p8 inner[FILE_PATH_MAX];

                if (file_resolve(source, outer, true) &&
                    file_resolve(destination, inner, true) && realpath_under(outer, inner))
                {
                        string_format(log_error,
                                      "mv: cannot move '%s' to a subdirectory of itself, '%s'\n",
                                      source, destination);
                        mv_status = 1;
                        return;
                }
        }

        string_format(log_error, "mv: cannot move '%s' to '%s': %s\n", source,
                      destination, file_reason(done));
        mv_status = 1;
}

static const file_long mv_longs[] = {
    {(string_address) "backup", 'B'},
    {(string_address) "context", 'Z'},
    {(string_address) "debug", 'v'},
    {(string_address) "force", 'f'},
    {(string_address) "no-copy", 'c'},
    {(string_address) "strip-trailing-slashes", 'w'},
    {(string_address) "suffix", 'S'},
    {(string_address) "update", 'u'},
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

        file_targets_begin();

        file_taking taking = {
            .program = (string_address) "mv",
            .allowed = (string_address) "bcfinSTtuvwZ",
            .valued = (string_address) "tS",
            //      mv's --context takes no value at all, unlike cp's and
            //      mkdir's, so Z is not among the ones that may carry one.
            .long_optional = (string_address) "Bu",
            .longs = mv_longs,
            .seen = file_target_seen,
            .supersedes = mv_supersedes,
        };

        if (!file_take(address_of taking))
                return 1;

        if (!file_backup_taken(address_of taking, (string_address) "mv"))
                return 1;

        if (!file_targets_told((string_address) "mv"))
                return 1;

        mv_newer_only = (taking.flags & FILE_FLAG('u')) != 0;

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
static bool rm_ask_once;
static bool rm_preserve_all;
static bool rm_loud;
static bool rm_one_system;
static bool rm_careful;
static bool rm_preserve_root;
static file_facts rm_root;
static p32 rm_device_major;
static p32 rm_device_minor;
static b32 rm_status;
static p8 rm_collision_option;

static p8 rm_prompt_option;

static const file_supersede rm_supersedes[] = {
    {(string_address) "fiI", address_of rm_collision_option},
    {(string_address) "fiIW", address_of rm_prompt_option},
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
                walk.error = 0;
                walk.have = 0;
                walk.at = 0;

                bipolar sought = system_seek(directory, 0, FILE_SEEK_SET);

                if (sought < 0)
                {
                        string_format(log_error, "rm: cannot read '%s': %s\n", shown,
                                      file_reason(sought));
                        rm_status = 1;
                        return false;
                }

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
                                string_format(log_error, "%s: %s '%s/%s': %s\n", (string_address) "rm", (string_address) "cannot remove", shown, entry->d_name, file_reason(-ERROR_NAME_TOO_LONG));
                                rm_status = 1;
                                complete = false;
                                continue;
                        }

                        if (rm_tree(directory, entry->d_name, below, depth))
                                removed++;
                        else
                                complete = false;
                }

                if (walk.error < 0)
                {
                        string_format(log_error, "rm: cannot read '%s': %s\n", shown,
                                      file_reason(walk.error));
                        rm_status = 1;
                        return false;
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
                        string_format(log_error, "rm: cannot remove '%s': %s\n", shown,
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
                        if (rm_loud)
                                string_format(log, "removed '%s'\n", shown);
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
                        string_format(log_error, "rm: cannot remove '%s': %s\n", shown,
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
                string_format(log_error,
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
                        string_format(log_error, "rm: '%s' is nested too deep\n", shown);
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
                        if (!rm_force || inside != -ERROR_NO_ENTRY)
                        {
                                string_format(log_error, "rm: cannot read '%s': %s\n", shown,
                                              file_reason(inside));
                                rm_status = 1;
                        }

                        return false;
                }

                // Check the opened object as well as the operand: a name
                // can change between the initial lookup and this open.
                if (rm_preserve_root)
                {
                        bipolar looked = file_look_code(inside, "", AT_EMPTY_PATH,
                                                        address_of facts);
                        if (looked < 0 || file_same_identity(address_of facts,
                                                             address_of rm_root))
                        {
                                string_format(log_error,
                                              "rm: refusing to read '%s': %s\n", shown,
                                              looked < 0 ? file_reason(looked)
                                                         : (string_address) "preserved root directory");
                                rm_status = 1;
                                system_close(inside);
                                return false;
                        }
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
                if (!rm_force || gone != -ERROR_NO_ENTRY)
                {
                        string_format(log_error, "rm: cannot remove '%s': %s\n", shown,
                                      file_reason(gone));
                        rm_status = 1;
                }

                return false;
        }

        if (rm_loud)
                string_format(log, "removed directory '%s'\n", shown);

        return complete;
}

static const file_long rm_longs[] = {
    {(string_address) "dir", 'd'},
    {(string_address) "force", 'f'},
    //      Its own letter, not -i's, because the two are not the same
    //      option: -i and -f each say what to do about a name that is not
    //      there as well as whether to ask, and --interactive says only
    //      the second.
    {(string_address) "interactive", 'W'},
    {(string_address) "one-file-system", 'o'},
    {(string_address) "no-preserve-root", 'N'},
    {(string_address) "preserve-root", 'P'},
    {(string_address) "recursive", 'R'},
    {(string_address) "verbose", 'v'},
    {null, 0},
};

/*
        The three prompting policies, and which option last chose one.

        -f never asks and forgives a name that is not there, -i asks about
        every name, -I asks once about the whole batch, and --interactive=WHEN
        chooses one of the three without saying anything about a missing name.
        Whichever was written last is the one that answers, which is why the
        letters are read out of a supersede row rather than out of the flags.
*/
static const file_word rm_whens[] = {
    {(string_address) "never", 'f', false},
    {(string_address) "no", 'f', false},
    {(string_address) "none", 'f', false},
    {(string_address) "once", 'I', true},
    {(string_address) "always", 'i', true},
    {(string_address) "yes", 'i', false},
};

static b32 file_rm()
{
        positive count = (positive)program_argument_count();
        rm_status = 0;
        rm_collision_option = 0;

        rm_prompt_option = 0;

        file_taking taking = {
            .program = (string_address) "rm",
            .allowed = (string_address) "dfiIrRv",
            .valued = (string_address) "",
            .long_optional = (string_address) "WP",
            .longs = rm_longs,
            .supersedes = rm_supersedes,
        };

        if (!file_take(address_of taking))
                return 1;

        positive flags = taking.flags;
        positive first = taking.first;

        /*
                --preserve-root takes one word and only one. The plain
                spelling is the default this tool already keeps; =all adds
                the refusal to walk off the device an argument's parent is
                on, which is checked with the arguments below.
        */
        rm_preserve_all = false;

        if (flags & FILE_FLAG('P'))
        {
                string_address which = file_option_value(address_of taking, 'P');

                if (which && string_compare(which, (string_address) "all"))
                        return string_report(log_error, 1,
                                             "rm: unrecognized --preserve-root argument: '%s'\n",
                                             which);

                rm_preserve_all = which != null;
        }

        //      --interactive=WHEN names one of the three policies; a bare
        //      --interactive is the one -i asks for.
        p8 prompting = rm_prompt_option;

        if (prompting == 'W')
        {
                string_address when = file_option_value(address_of taking, 'W');

                if (!when)
                        prompting = 'i';
                else
                {
                        b32 chosen = file_word_among((string_address) "rm",
                                                     (string_address) "--interactive",
                                                     when, rm_whens,
                                                     sizeof(rm_whens) / sizeof(rm_whens[0]));

                        if (chosen < 0)
                                return 1;

                        prompting = (p8)chosen;
                }
        }

        rm_force = rm_collision_option == 'f';
        rm_ask = prompting == 'i';
        rm_ask_once = prompting == 'I';
        rm_loud = (flags & FILE_FLAG('v')) != 0;
        rm_one_system = (flags & FILE_FLAG('o')) != 0;
        rm_empty_directories = (flags & FILE_FLAG('d')) != 0;
        rm_recursive = (flags & (FILE_FLAG('r') | FILE_FLAG('R'))) != 0;
        rm_careful = rm_ask || rm_loud || rm_one_system;
        rm_preserve_root = rm_recursive && !(flags & FILE_FLAG('N'));

        if (first >= count)
        {
                if (rm_force)
                        return 0;

                return string_report(log_error, 1, "%s: missing operand\n", (string_address) "rm");
        }

        /*
                -I and --interactive=once ask once about the batch instead of
                once about each name: over three names, or any name at all
                under -r. A no here is not a failure, it is the batch not
                being taken.
        */
        if (rm_ask_once)
        {
                positive named = count - first;

                if (named > 3 || (rm_recursive && named))
                {
                        string_format(log_error, rm_recursive
                                          ? "rm: remove %p argument%s recursively? "
                                          : "rm: remove %p argument%s? ",
                                      named, named == 1 ? "" : "s");

                        if (!file_answer_is_yes())
                                return 0;
                }
        }

        if (rm_preserve_root)
        {
                bipolar looked = file_look_code(AT_FDCWD, "/", 0, address_of rm_root);
                if (looked < 0)
                        return string_report(log_error, 1, "rm: cannot preserve '/': %s\n",
                                      file_reason(looked));
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
                                string_format(log_error, "rm: cannot remove '%s': %s\n",
                                              path, file_reason(looked));
                                rm_status = 1;
                        }

                        continue;
                }

                bool here = (facts.mode & MODE_FORMAT) == MODE_DIRECTORY;

                if (here && rm_preserve_root &&
                    file_same_identity(address_of facts, address_of rm_root))
                {
                        log_error("rm: it is dangerous to operate recursively on '/'\n", 0);
                        log_error("rm: use --no-preserve-root to override this failsafe\n", 0);
                        rm_status = 1;
                        continue;
                }

                /*
                        =all keeps a named directory that is a mount point
                        whole: it is on a different device from the directory
                        it hangs under, and taking it would take a filesystem
                        rather than a tree.
                */
                if (here && rm_preserve_all)
                {
                        p8 above[FILE_PATH_MAX];
                        file_facts parent;

                        if (file_path_join(above, path, "..") &&
                            file_look_at(above, address_of parent) &&
                            (parent.device_major != facts.device_major ||
                             parent.device_minor != facts.device_minor))
                        {
                                string_format(log_error,
                                              "rm: skipping '%s', since it's on a different device\n",
                                              path);
                                log_error("rm: and --preserve-root=all is in effect\n", 0);
                                rm_status = 1;
                                continue;
                        }
                }

                if (here && !rm_recursive && !rm_empty_directories)
                {
                        string_format(log_error, "rm: cannot remove '%s': Is a directory\n",
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
                                     address_of wide) != digits + 3 || wide != 2 ||
                    string_get(text + digits + 3) || fraction > 60)
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

        if (field[2] < 1 || field[2] > 12 || field[3] < 1 ||
            field[3] > (b64)file_month_days(year, field[2]) ||
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

/*
        The words --time answers to, and the stamp -t carries.

        The reference reads both where the option is written: -t is parsed by
        the getopt loop, so of a bad -t and a bad --time the first one on the
        line is what is reported, and a -r whose file is not there is not
        reached until both have been read.
*/
static const file_word touch_which_words[] = {
    {(string_address) "atime", 'a', false},
    {(string_address) "access", 'a', false},
    {(string_address) "use", 'a', true},
    {(string_address) "mtime", 'm', false},
    {(string_address) "modify", 'm', false},
};

static b64 touch_stamp_seconds;
static bool touch_stamp_given;
static p8 touch_which_letter;

static bool touch_option_seen(p8 letter, string_address value)
{
        if (letter == 'T' && value)
        {
                b32 which = file_word_among((string_address) "touch",
                                            (string_address) "--time", value,
                                            touch_which_words,
                                            array_count(touch_which_words));

                if (which < 0)
                        return false;

                touch_which_letter = (p8)which;
        }

        if (letter == 't' && value)
        {
                if (!touch_stamp(value, file_now(), address_of touch_stamp_seconds))
                        return string_report(log_error, false,
                                             "touch: invalid date format '%s'\n", value);

                touch_stamp_given = true;
        }

        return true;
}

static b32 file_touch()
{
        positive count = (positive)program_argument_count();

        touch_stamp_given = false;
        touch_which_letter = 0;

        file_taking taking = {
            .program = (string_address) "touch",
            .allowed = (string_address) "acdfhmrt",
            .valued = (string_address) "drtT",
            .longs = touch_longs,
            .seen = touch_option_seen,
        };

        if (!file_take(address_of taking))
                return 1;

        positive flags = taking.flags;
        positive first = taking.first;

        bool access = (flags & FILE_FLAG('a')) != 0;
        bool modify = (flags & FILE_FLAG('m')) != 0;
        bool no_create = (flags & FILE_FLAG('c')) != 0;
        bool through = (flags & FILE_FLAG('h')) == 0;
        p64 times[4] = {0, UTIME_NOW, 0, UTIME_NOW};

        if (touch_which_letter == 'a')
                access = true;
        else if (touch_which_letter == 'm')
                modify = true;

        string_address from = file_option_value(address_of taking, 'r');

        //      -t carries a time of its own, so beside a -r or a -d it is one
        //      source too many. -r and -d together are not: the reference
        //      takes the file's times and lets the date move them.
        if (touch_stamp_given && (from || file_option_value(address_of taking, 'd')))
                return string_report(log_error, 1,
                                     "touch: cannot specify times from more than one source\n");

        if (from)
        {
                file_facts facts;
                bipolar looked = file_look_code(AT_FDCWD, from, 0, address_of facts);

                if (looked < 0)
                {
                        string_format(log_error,
                                      "touch: failed to get attributes of '%s': %s\n", from,
                                      file_reason(looked));
                        return 1;
                }

                file_times_of(address_of facts, times);
        }

        string_address written = file_option_value(address_of taking, 'd');

        if (written)
        {
                // Relative dates start from each reference timestamp,
                // including its own fraction; absolute dates replace it.
                b64 now = from ? 0 : file_now();
                for (positive at = 0; at < 4; at += 2)
                {
                        b64 seconds;
                        positive fraction;
                        if (!file_moment_read_from(written,
                                                   from ? (b64)times[at] : now,
                                                   from ? times[at + 1] : 0,
                                                   address_of seconds,
                                                   address_of fraction))
                                return string_report(log_error, 1, "touch: invalid date format '%s'\n", written);
                        times[at] = (p64)seconds;
                        times[at + 1] = fraction;
                }
        }

        if (touch_stamp_given)
        {
                times[0] = times[2] = (p64)touch_stamp_seconds;
                times[1] = times[3] = 0;
        }

        if (first >= count)
                return string_report(log_error, 1, "touch: missing file operand\n");

        if (!access && !modify)
        {
                access = true;
                modify = true;
        }

        if (!access)
                times[1] = UTIME_OMIT;
        if (!modify)
                times[3] = UTIME_OMIT;

        b32 status = 0;

        while (first < count)
        {
                string_address path = program_argument((b32)first++);

                // A lone dash is standard output, already open, whatever
                // name it has.
                if (string_is(path, '-') && !string_get(path + 1))
                {
                        bipolar done = system_update_times_at(1, null, times, 0);

                        if (done < 0)
                        {
                                string_format(log_error, "touch: cannot touch '%s': %s\n",
                                              path, file_reason(done));
                                status = 1;
                        }

                        continue;
                }

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
                                string_format(log_error, "touch: cannot touch '%s': %s\n",
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
                        string_format(log_error, "touch: setting times of '%s': %s\n",
                                      path, file_reason(done));
                        status = 1;
                }
        }

        log_flush();

        return status;
}

// Durations: sleep, timeout, replay and util-linux waits share checked
// decimal/scientific seconds. Keep 19 significant digits plus the next digit:
// only the twentieth can still contribute to a native-word nanosecond count.
static bool file_duration_read(string_address text, bool units,
                                positive address_to nanoseconds)
{
        string_address at = text;
        positive made = 0;
        positive fraction = 0;
        positive dropped = 0;
        positive next = 0;
        bool point = false;
        bool any = false;

        while (byte_is_space(string_get(at)))
                at++;
        if (string_is(at, '+'))
                at++;

        while (byte_is_digit(string_get(at)) ||
               (!point && string_is(at, '.')))
        {
                if (string_is(at, '.'))
                {
                        point = true;
                        at++;
                        continue;
                }

                positive digit = string_get(at++) - '0';
                any = true;
                if (point)
                        fraction++;
                if (!dropped && made < 1000000000000000000ul)
                        made = made * 10 + digit;
                else
                {
                        if (!dropped)
                                next = digit;
                        dropped++;
                }
        }
        if (!any)
                return false;

        bipolar exponent = 0;
        if (string_is(at, 'e') || string_is(at, 'E'))
        {
                bool negative = false;
                positive magnitude = 0;

                at++;
                if (string_is(at, '+') || string_is(at, '-'))
                        negative = string_get(at++) == '-';
                if (!byte_is_digit(string_get(at)))
                        return false;
                while (byte_is_digit(string_get(at)))
                {
                        if (magnitude < 1000000)
                                magnitude = magnitude * 10 +
                                            string_get(at) - '0';
                        at++;
                }
                exponent = negative ? -(bipolar)magnitude
                                    : (bipolar)magnitude;
        }
        positive multiplier = 1;
        if (units && string_get(at) && string_first_of("smhd", string_get(at)))
        {
                p8 suffix = string_get(at++);
                multiplier = suffix == 'm' ? 60 : suffix == 'h' ? 3600
                             : suffix == 'd' ? 86400 : 1;
        }
        if (string_get(at))
                return false;

        bipolar scale = 9 + exponent - (bipolar)fraction +
                        (bipolar)dropped;
        if (!made)
        {
                address_to nanoseconds = 0;
                return true;
        }
        while (scale > 0)
        {
                positive digit = dropped ? next : 0;
                if (made > (positive_max - digit) / 10)
                        return false;
                made = made * 10 + digit;
                dropped = 0;
                scale--;
        }
        while (scale < 0 && made)
        {
                made /= 10;
                scale++;
        }
        if (made > positive_max / multiplier)
                return false;
        address_to nanoseconds = made * multiplier;
        return true;
}

static bool sleep_read(string_address text, p64 address_to seconds,
                       p64 address_to nanoseconds)
{
        positive total;
        if (!file_duration_read(text, true, address_of total))
                return false;
        address_to seconds = total / 1000000000;
        address_to nanoseconds = total % 1000000000;
        return true;
}

static b32 file_sleep()
{
        positive count = (positive)program_argument_count();

        if (count < 2)
                return string_report(log_error, 1, "%s: missing operand\n", (string_address) "sleep");

        bool intervals_only = false;

        for (positive i = 1; i < count; i++)
        {
                p64 wanted[2] = {0, 0};
                string_address word = program_argument((b32)i);

                //      Two dashes close the options: they are not an
                //      interval themselves, and every word after them is
                //      one however it begins.
                if (!intervals_only && string_equals(word, "--"))
                {
                        intervals_only = true;
                        continue;
                }

                // sleep takes no options, so a word that begins with a dash
                // is one the option reader refuses rather than an interval.
                if (!intervals_only && string_is(word, '-') && string_get(word + 1))
                {
                        //      Two dashes name the whole word, one names
                        //      the letter, and either way the reference says
                        //      where to look next.
                        if (string_is(word + 1, '-'))
                                string_format(log_error,
                                    "sleep: unrecognized option '%s'\n", word);
                        else
                        {
                                p8 named[2] = {string_get(word + 1), end};

                                string_format(log_error,
                                    "sleep: invalid option -- '%s'\n", named);
                        }

                        string_format(log_error,
                            "Try 'sleep --help' for more information.\n");
                        return 1;
                }

                if (!sleep_read(word, address_of wanted[0],
                                address_of wanted[1]))
                {
                        string_format(log_error, "sleep: invalid time interval '%s'\n",
                                      program_argument((b32)i));
                        return 1;
                }

                // A signal that arrives partway through leaves the remainder
                // in the second timespec, and the sleep goes on from there.
                p64 left[2] = {wanted[0], wanted[1]};

                bipolar slept;
                do
                        slept = system_call_2(syscall(nanosleep),
                                               (positive)left, (positive)left);
                while (slept == -4);
                if (slept < 0)
                        return string_report(log_error, 1, "sleep: %s\n", file_reason(slept));
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
                return string_report(log_error, 1, "stty: only 'size' is supported\n");

        file_window_size size = {0, 0, 0, 0};
        bipolar answer = system_control(0, FILE_TIOCGWINSZ,
                                       address_of size);

        if (answer < 0)
                return string_report(log_error, 1, "stty: standard input: %s\n",
                              file_reason(answer));

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
                return string_report(log_error, 2, "tty: extra operand '%s'\n",
                              file_simple_operand_list[0]);

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
                return string_report(log_error, 4, "tty: ttyname error: %s\n",
                              file_reason(length));

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
        times ends at .3 exactly. Scale zero owns the complete signed 64-bit
        range, so integers use the same parser, iterator and formatter.
*/
typedef struct
{
        bipolar coefficient;
        positive scale;
        positive shown;
        positive whole_width;
        bool negative_zero;
} seq_decimal;

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
        out->whole_width = max((positive)1,
            (point == positive_max ? finish : point) - mantissa) +
                (exponent > 0 ? (positive)exponent : 0);

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

                positive multiplier = positive_power_ten(grow);

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

        positive multiplier = positive_power_ten(scale - number->scale);
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

static positive seq_decimal_width(seq_decimal address_to number,
                                  positive scale, positive precision)
{
        positive magnitude = (positive)number->coefficient;

        if (number->coefficient < 0)
                magnitude = (positive)0 - magnitude;

        if (scale > precision)
                magnitude /= positive_power_ten(scale - precision);

        magnitude /= positive_power_ten(scale < precision ? scale : precision);

        return max(positive_digits(magnitude), number->whole_width) +
               (precision ? precision + 1 : 0) +
               (number->coefficient < 0 || number->negative_zero);
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

// Which of the three ways a format can be wrong it was: a second
// conversion, a conversion this one cannot render, or a % at the end.
enum
{
        SEQ_FORMAT_GOOD,
        SEQ_FORMAT_MANY,
        SEQ_FORMAT_UNKNOWN,
        SEQ_FORMAT_ENDS,
        // A conversion the reference knows and this one cannot compute.
        SEQ_FORMAT_FLOAT
};

static p8 seq_format_wrong;
static p8 seq_format_letter;

static bool seq_format_read(string_address text, seq_format address_to format)
{
        bool found = false;

        seq_format_wrong = SEQ_FORMAT_GOOD;
        seq_format_letter = 0;

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
                {
                        seq_format_wrong = SEQ_FORMAT_MANY;
                        return false;
                }

                found = true;
                format->directive = at++;

                string_address field = text + at;
                conversion_spec parsed = conversion_spec_take_max(&field, positive_max);
                // The former pre-digit limit of 100000 admits one final digit.
                if (parsed.stars || parsed.overflow || parsed.field[0] > 1000009 ||
                    parsed.field[1] > 1000009)
                {
                        seq_format_wrong = SEQ_FORMAT_UNKNOWN;
                        seq_format_letter = string_get(text + at);
                        return false;
                }
                format->flags = parsed.flags;
                format->width = parsed.field[0];
                format->precision = parsed.fields == 2 ? parsed.field[1] : 6;
                at = (positive)(field - text);

                // coreutils accepts an explicit long-double length here even
                // though seq supplies that type itself.
                if (text[at] == 'L')
                        at++;

                if (!text[at])
                {
                        seq_format_wrong = SEQ_FORMAT_ENDS;
                        return false;
                }

                //      The conversions the reference knows and this one
                //      cannot compute -- there is no floating point in this
                //      file -- are read as conversions all the same, so that
                //      what is said about them is said in the reference's
                //      order and is about them rather than about the letter.
                if (string_first_of((string_address) "eEgGaA", text[at]))
                {
                        seq_format_wrong = SEQ_FORMAT_FLOAT;
                        seq_format_letter = text[at];
                        return false;
                }

                if (text[at] != 'f' && text[at] != 'F')
                {
                        seq_format_wrong = SEQ_FORMAT_UNKNOWN;
                        seq_format_letter = text[at];
                        return false;
                }

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

static positive seq_format_literal_into(p8 address_to into,
                                         string_address text, positive length)
{
        positive used = 0;
        for (positive at = 0; at < length; at++)
        {
                into[used++] = text[at];
                if (text[at] == '%' && at + 1 < length && text[at + 1] == '%')
                        at++;
        }
        return used;
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

        //      Too few or too many numbers, said the way the reference says
        //      it: nothing at all is a missing operand, and a fourth is the
        //      extra one, named.
        if (given < 1)
                return string_report(log_error, 1, "seq: missing operand\n");

        if (given > 3)
                return string_report(log_error, 1, "seq: extra operand '%s'\n",
                                     program_argument((b32)(index + 3)));

        seq_format format = {.text = "", .flags = CONVERSION_FLAG_ZERO};

        //      A format is read before it is weighed against -w, because the
        //      reference reports a format it cannot read whatever else was
        //      asked -- but a conversion it can read and this one cannot
        //      compute is weighed against -w first, as the reference does.
        bool readable = !format_text || seq_format_read(format_text, address_of format);

        if (!readable && seq_format_wrong != SEQ_FORMAT_FLOAT)
        {
                p8 named[2] = {seq_format_letter, end};

                if (seq_format_wrong == SEQ_FORMAT_MANY)
                        string_format(log_error,
                                      "seq: format '%s' has too many %% directives\n",
                                      format_text);
                else if (seq_format_wrong == SEQ_FORMAT_ENDS)
                        string_format(log_error, "seq: format '%s' ends in %%\n", format_text);
                else
                        string_format(log_error,
                                      "seq: format '%s' has unknown %%%s directive\n",
                                      format_text, named);

                return 1;
        }

        if (pad && format_text)
        {
                log_error("seq: format string may not be specified"
                          " when printing equal width strings\n", 0);
                return 1;
        }

        if (!readable)
        {
                p8 named[2] = {seq_format_letter, end};

                return string_report(log_error, 1,
                                     "seq: format '%s' asks for the %%%s conversion, which needs "
                                     "floating point this seq has not got\n",
                                     format_text, named);
        }

        seq_decimal number[3];

        for (positive i = 0; i < given; i++)
                if (!seq_decimal_number(program_argument((b32)(index + i)),
                                         address_of number[i]))
                {
                        string_address text = program_argument((b32)(index + i));
                        string_address at = text;

                        if (string_is(at, '+') || string_is(at, '-'))
                                at++;

                        //      A word that spells a number no decimal holds
                        //      is named for what it spells, which is what the
                        //      reference calls it.
                        if ((string_is(at, 'n') || string_is(at, 'N')) &&
                            (string_is(at + 1, 'a') || string_is(at + 1, 'A')) &&
                            (string_is(at + 2, 'n') || string_is(at + 2, 'N')) &&
                            !string_get(at + 3))
                                return string_report(log_error, 1,
                                                     "seq: invalid 'not-a-number' argument: '%s'\n",
                                                     text);

                        string_format(log_error, "seq: invalid floating point argument: '%s'\n",
                                      text);
                        return 1;
                }

        seq_decimal first = given == 1 ? (seq_decimal){1} : number[0];
        seq_decimal step = given == 3 ? number[1] : (seq_decimal){1};
        seq_decimal last = number[given - 1];
        positive precision = max(first.shown, step.shown);
        positive scale = max(first.scale, max(step.scale, last.scale));

        if (!seq_decimal_rescale(address_of first, scale) ||
            !seq_decimal_rescale(address_of step, scale) ||
            !seq_decimal_rescale(address_of last, scale))
                return string_report(log_error, 1, "seq: decimal range is too large\n");

        if (!step.coefficient)
        {
                string_format(log_error, "seq: invalid Zero increment value: '%s'\n",
                              program_argument((b32)(index + 1)));
                return 1;
        }

        if (!format_text)
                format.precision = precision;
        if (pad)
                format.width = max(
                    seq_decimal_width(address_of first, scale, precision),
                    seq_decimal_width(address_of last, scale, precision));

        bipolar value = first.coefficient;
        bool written = false;
        positive separator_length = string_length(separator);
        string_address suffix = format.text + format.after;
        positive suffix_length = string_length(suffix);
        positive stride = step.coefficient < 0
                              ? (positive)0 - (positive)step.coefficient
                              : (positive)step.coefficient;

        while (step.coefficient > 0 ? value <= last.coefficient
                                    : value >= last.coefficient)
        {
                if (written)
                        log(separator, 0);

                positive magnitude = value < 0 ? (positive)0 - (positive)value
                                               : (positive)value;
                bool negative_zero = !written && first.negative_zero;
                fixed_decimal field = fixed_decimal_prepare(
                    magnitude, scale, value < 0 || negative_zero,
                    format.width, format.precision, format.flags);
                positive records = 1;
                positive length = field.length + field.zeroes + field.padding;
                if (format.directive + suffix_length + separator_length <=
                        sizeof(file_transfer) &&
                    length <= sizeof(file_transfer) - format.directive -
                                  suffix_length - separator_length)
                {
                        positive prefix = seq_format_literal_into(
                            file_transfer, format.text, format.directive);
                        fixed_decimal_into(file_transfer + prefix,
                                           sizeof(file_transfer) - prefix,
                                           address_of field);
                        length += prefix;
                        length += seq_format_literal_into(file_transfer + length,
                                                           suffix, suffix_length);
                        memory_copy_apart(file_transfer + length, separator,
                                          separator_length);
                        length += separator_length;
                        if (format.precision >= scale && !negative_zero &&
                            stride <= (positive)bipolar_max)
                        {
                                records = sizeof(file_transfer) / length;
                                positive distance = step.coefficient > 0
                                    ? (positive)last.coefficient - (positive)value
                                    : (positive)value - (positive)last.coefficient;
                                if (distance / stride < records - 1)
                                        records = distance / stride + 1;
                                bool decreasing = (value < 0) !=
                                                  (step.coefficient < 0);
                                positive boundary = positive_power_ten(scale + 1);
                                while (boundary <= magnitude && boundary <= positive_max / 10)
                                        boundary *= 10;
                                positive minimum = boundary / 10;
                                if (minimum <= positive_power_ten(scale)) minimum = 0;
                                distance = decreasing ? magnitude - minimum
                                                      : boundary - 1 - magnitude;
                                if (distance / stride < records - 1)
                                        records = distance / stride + 1;
                                if (value < 0 && step.coefficient > 0 &&
                                    (magnitude - 1) / stride < records - 1)
                                        records = (magnitude - 1) / stride + 1;
                                positive offset = prefix +
                                    (field.left ? 0 : field.padding);
                                bipolar increment = decreasing ? -(bipolar)stride
                                                               : (bipolar)stride;
                                positive bytes = memory_decimal_series(
                                    file_transfer, records * length, length,
                                    offset + field.sign,
                                    offset + field.length, increment);
                                records = bytes / length;
                        }
                        log(file_transfer, records * length - separator_length);
                }
                else
                {
                        seq_format_literal(log, format.text, format.directive);
                        fixed_decimal_write(log, address_of field);
                        seq_format_literal(log, suffix, suffix_length);
                }
                written = true;

                value = (bipolar)((positive)value +
                                 (positive)step.coefficient * (records - 1));

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
                                return string_report(log_error, 1, "yes: arguments are too large\n");

                        length += space + have;
                }
        }

        positive mapped = (positive)memory(length);

        if (!mapped || system_failed(mapped))
                return string_report(log_error, 1, "yes: out of memory\n");

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
                return string_report(log_error, false, "env: environment is too large\n");

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
                return string_report(log_error, false, "env: unset list is too large\n");

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
                        return string_report(log_error, false, "env: -S here cuts at spaces and reads nothing else\n");
        }

        /*
                Reserve before storing pointers into the byte block: growing
                it after the first word would move the text underneath those
                pointers. At most every second byte begins a one-byte word.
        */
        if (!shell_array_room(env_split_store, env_split_room, length + 1) ||
            !shell_array_room(env_words, env_words_room, given + length / 2 + 2))
                return string_report(log_error, false, "env: split string is too large\n");

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
                log_error("env: the signal options are taken here and change nothing\n", 0);

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
                return string_report(log_error, 125, "env: argument list is too large\n");

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
                        return string_report(log_error, 125, "env: must specify command with --chdir (-C)\n");

                if (file_option_value(address_of taking, 'a'))
                        return string_report(log_error, 125, "env: must specify command with --argv0 (-a)\n");

                bool zero = (taking.flags & FILE_FLAG('0')) != 0;

                for (positive i = 0; i < env_have; i++)
                        file_written(env_list[i], zero);

                log_flush();
                return 0;
        }

        if (where && system_change_directory(where) < 0)
                return string_report(log_error, 125, "env: cannot change directory to %s\n", where);

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

        string_format(log_error, "env: '%s': %s\n", name,
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
                return string_report(log_error, 1, "id: --context (-Z) works only on an SELinux-enabled kernel\n");

        //      -u, -g and -G each say the answer is one thing; two of them
        //      say it is two, which the reference refuses before it looks
        //      anything up.
        positive chosen = ((flags & FILE_FLAG('u')) != 0) +
                          ((flags & FILE_FLAG('g')) != 0) +
                          ((flags & FILE_FLAG('G')) != 0);

        if (chosen > 1)
                return string_report(log_error, 1,
                                     "id: cannot print \"only\" of more than one choice\n");

        if ((names || real) && !one)
                return string_report(log_error, 1, "id: printing only names or real IDs requires -u, -g, or -G\n");

        if (zero && !one)
                return string_report(log_error, 1, "id: option --zero not permitted in default format\n");

        positive first = taking.first;
        positive count = (positive)program_argument_count();
        if (first < count)
        {
                b32 status = 0;

                while (first < count)
                {
                        string_address who = program_argument((b32)first++);
                        p8 named[FILE_NAME_MAX];
                        positive number;

                        // A number is the account that has it, by name from
                        // here on, so every field below is the same lookup.
                        if (string_digits_exact(who, address_of number) &&
                            number <= p32_max &&
                            file_user_name(number, named, FILE_NAME_MAX))
                                who = (string_address)named;

                        bipolar user = file_user_id(who);
                        bipolar group = file_account_id(
                            file_account_text(FILE_ACCOUNT_USER), who, 3);

                        if (user < 0 || group < 0)
                        {
                                string_format(log_error, "id: '%s': no such user\n",
                                              program_argument((b32)(first - 1)));
                                status = 1;
                                continue;
                        }

                        positive have;

                        if (!id_groups_named(who, (positive)group,
                                             address_of have))
                        {
                                log_error("id: group list is too large\n", 0);
                                status = 1;
                                continue;
                        }

                        id_written((positive)user, (positive)group,
                                   file_id_scratch, have, flags, names, zero);

                        //      A group list asked about more than one
                        //      account with -z ends each account with a
                        //      second zero byte: the list's own separator is
                        //      the zero byte too, so the reference closes the
                        //      list and then the account.
                        if (zero && (flags & FILE_FLAG('G')) &&
                            count - taking.first > 1)
                                log("", 1);
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
                return string_report(log_error, 1, "id: failed to get groups for the current process\n");

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
                        string_format(log_error,
                                      "groups: cannot find name for group ID %p\n",
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
                        return string_report(log_error, 1, "groups: failed to get groups for the current process\n");

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
                                string_format(log_error,
                                              "groups: '%s': no such user\n", who);
                                status = 1;
                                continue;
                        }

                        if (!id_groups_named(who, (positive)group,
                                             address_of have))
                        {
                                log_error("groups: group list is too large\n", 0);
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
                return string_report(log_error, 1, "whoami: extra operand '%s'\n",
                              file_simple_operand_list[0]);

        positive user = (positive)system_call(syscall(geteuid));
        p8 name[FILE_NAME_MAX];

        if (!file_user_name(user, name, FILE_NAME_MAX))
                return string_report(log_error, 1,
                              "whoami: cannot find name for user ID %p\n", user);

        file_line(name);
        log_flush();
        return 0;
}

// nologin --------------------------------------------------------
/*
        A login shell has one job here: refuse the session.  Keep the optional
        site message on the same bounded streaming path as the file tools so a
        large /etc/nologin.txt neither allocates nor gets truncated.  util-linux
        accepts -c for su compatibility but deliberately does not execute it.
*/
static const file_long nologin_longs[] = {
    {(string_address)"command", 'c'},
    {(string_address)"help", 'h'},
    {(string_address)"version", 'V'},
    {null, 0},
};

static b32 file_nologin()
{
        file_taking taking = {
            .program = (string_address)"nologin",
            .allowed = (string_address)"chV",
            .valued = (string_address)"c",
            .longs = nologin_longs,
        };

        if (!file_take(address_of taking))
                return 1;

        if (taking.flags & FILE_FLAG('h'))
                string_format(log,
                              "Usage: nologin [options]\n"
                              "  -c, --command COMMAND  ignored for su compatibility\n"
                              "  -h, --help             display this help\n"
                              "  -V, --version          display version\n");
        else if (taking.flags & FILE_FLAG('V'))
                string_format(log, "nologin from dawning-kit\n");
        else
        {
                bipolar handle = system_open_at(
                    AT_FDCWD, (string_address)"/etc/nologin.txt", FILE_READ);

                if (handle < 0)
                        log("This account is currently not available.\n", 41);
                else
                {
                        while (1)
                        {
                                bipolar got = system_read_retry(
                                    (positive)handle, file_transfer,
                                    sizeof(file_transfer));

                                if (got <= 0)
                                        break;
                                log((string_address)file_transfer,
                                    (positive)got);
                        }

                        system_close(handle);
                }
        }

        log_flush();
        return 1;
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
                return string_report(log_error, 1, "logname: extra operand '%s'\n",
                              file_simple_operand_list[0]);

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
                return string_report(log_error, 1, "logname: no login name\n");

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
                return string_report(log_error, 1, "hostname: cannot read system name\n");

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

        if (taking.first < (positive)program_argument_count())
        {
                string_format(log_error, "uname: extra operand '%s'\n",
                              program_argument((b32)taking.first));
                return 1;
        }

        positive flags = taking.flags;

        memory_fill(address_of facts, 0, sizeof(facts));

        if (system_call_1(syscall(uname), (positive)address_of facts) < 0)
                return string_report(log_error, 1, "uname: cannot read system name\n");

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

        positive number;
        if (!file_decimal_read(address_of text, true, address_of number))
                return false;

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

//      --ignore's number is read where the option is written, so a line
//      that also carries an operand or an option nobody has says what is
//      wrong with the number first, the way the reference's getopt does.
static positive nproc_ignore;

static bool nproc_option_seen(p8 letter, string_address value)
{
        if (letter != 'i' || !value)
                return true;

        if (!nproc_decimal(value, true, false, false, address_of nproc_ignore))
                return string_report(log_error, false, "nproc: invalid number: '%s'\n", value);

        return true;
}

static b32 file_nproc()
{
        file_simple_operand_count = 0;
        nproc_ignore = 0;

        file_taking taking = {
            .program = (string_address) "nproc",
            .allowed = (string_address) "",
            .valued = (string_address) "i",
            .longs = nproc_longs,
            .operand = file_simple_operand,
            .seen = nproc_option_seen,
        };

        if (!file_take(address_of taking))
                return 1;

        if (file_simple_operand_count)
                return string_report(log_error, 1, "nproc: extra operand '%s'\n",
                              file_simple_operand_list[0]);

        positive ignore = nproc_ignore;

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

//      -p and --tmpdir answer the same question, so the last one written is
//      the one that answers it; a bare --tmpdir is that answer too, and
//      means the environment's directory rather than a named one.
static p8 mktemp_where_option;

static const file_supersede mktemp_supersedes[] = {
    {(string_address) "pT", address_of mktemp_where_option},
    {null, null},
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
            .supersedes = mktemp_supersedes,
        };

        mktemp_where_option = 0;

        if (!file_take(address_of taking))
                return 1;

        bool directory = (taking.flags & FILE_FLAG('d')) != 0;
        bool dry = (taking.flags & FILE_FLAG('u')) != 0;
        bool quiet = (taking.flags & FILE_FLAG('q')) != 0;
        bool old_t = (taking.flags & FILE_FLAG('t')) != 0;
        bool named_where = (taking.flags & (FILE_FLAG('p') | FILE_FLAG('T'))) != 0;
        string_address base = named_where
                                  ? file_option_value(address_of taking,
                                                      mktemp_where_option)
                                  : null;
        string_address suffix = file_option_value(address_of taking, 'S');
        string_address template = mktemp_template;
        p8 path[FILE_PATH_MAX];
        p8 whole[FILE_PATH_MAX];
        positive length = 0;
        positive marks_at;
        positive marks = 0;

        if (mktemp_extra_template)
                return string_report(log_error, 1, "mktemp: too many templates\n");

        if (!template)
        {
                template = "tmp.XXXXXXXXXX";
                named_where = true;
        }

        /*
                The template and its suffix, and what the reference refuses
                about them, in the reference's order: a suffix asked for
                needs a template that ends in an X and may not name a
                directory of its own; then there have to be X's to fill in;
                then the tail after them, which is a suffix whether it was
                asked for or found, may not name a directory either.
        */
        positive template_length = string_length(template);

        if (suffix)
        {
                if (!template_length || template[template_length - 1] != 'X')
                        return string_report(log_error, 1,
                                             "mktemp: with --suffix, template '%s' must end in X\n",
                                             template);

                if (string_first_of(suffix, '/'))
                        return string_report(log_error, 1,
                                             "mktemp: invalid suffix '%s', contains directory separator\n",
                                             suffix);
        }

        positive run_end = template_length;

        while (run_end && template[run_end - 1] != 'X')
                run_end--;

        positive run_at = run_end;

        while (run_at && template[run_at - 1] == 'X')
        {
                run_at--;
                marks++;
        }

        if (marks < MKTEMP_LEAST)
                return string_report(log_error, 1, "mktemp: too few X's in template '%s'\n",
                                     template);

        positive suffix_length = suffix ? string_length(suffix) : 0;

        if (template_length + suffix_length >= FILE_PATH_MAX - 1)
                return string_report(log_error, 1, "mktemp: template too long\n");

        memory_copy_apart(whole, template, template_length);

        if (suffix)
                memory_copy_apart(whole + template_length, suffix, suffix_length);

        whole[template_length + suffix_length] = end;

        if (!suffix && string_first_of(whole + run_end, '/'))
                return string_report(log_error, 1,
                                     "mktemp: invalid suffix '%s', contains directory separator\n",
                                     whole + run_end);

        /*
                Where it goes. -t is the deprecated spelling and answers with
                the environment's directory whatever -p or --tmpdir said, and
                refuses a template that names a directory of its own; -p and
                --tmpdir name one, and either of them written bare or empty
                means the environment's directory too.
        */
        if (old_t)
        {
                if (string_first_of(whole, '/'))
                        return string_report(log_error, 1,
                                             "mktemp: invalid template, '%s', contains directory separator\n",
                                             whole);

                base = null;
                named_where = true;
        }
        else if (named_where && string_is(whole, '/'))
                return string_report(log_error, 1,
                                     "mktemp: invalid template, '%s'; with --tmpdir, it may not be absolute\n",
                                     whole);

        if (named_where && !string_is(whole, '/'))
        {
                if (!base || !string_get(base))
                        base = file_environment("TMPDIR");

                if (!base || !string_get(base))
                        base = "/tmp";

                length = string_length(base);

                if (length > FILE_PATH_MAX - 2)
                        return string_report(log_error, 1, "mktemp: template too long\n");

                memory_copy_apart(path, base, length);

                while (length > 1 && path[length - 1] == '/')
                        length--;

                path[length++] = '/';
        }

        positive whole_length = template_length + suffix_length;

        if (length + whole_length >= FILE_PATH_MAX)
                return string_report(log_error, 1, "mktemp: template too long\n");

        memory_copy_apart(path + length, whole, whole_length);
        path[length + whole_length] = end;

        marks_at = length + run_at;
        length += whole_length;

        // The template as the reference names it when nothing can be made:
        // directory, X's and suffix together, before any X was filled in.
        p8 shown[FILE_PATH_MAX];

        memory_copy_apart(shown, path, length + 1);

        for (positive attempt = 0; attempt < MKTEMP_ATTEMPTS; attempt++)
        {
                bipolar answer;

                mktemp_letters_into(path + marks_at, marks);

                /*
                        -u makes nothing, but it does not promise a name
                        either: the reference asks the same question the
                        creation would have asked -- is this name free -- and
                        a question it cannot ask is the same failure it would
                        have reported.
                */
                if (dry)
                {
                        file_facts standing;
                        bipolar looked = file_look_code(AT_FDCWD, path,
                                                        AT_SYMLINK_NOFOLLOW,
                                                        address_of standing);

                        if (!looked)
                                continue;

                        if (looked == -ERROR_NO_ENTRY)
                                break;

                        if (!quiet)
                                string_format(log_error,
                                              "mktemp: failed to create %s via template '%s': %s\n",
                                              directory ? "directory" : "file",
                                              shown, file_reason(looked));

                        return 1;
                }

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
                                string_format(log_error,
                                              "mktemp: failed to create %s via template '%s': %s\n",
                                              directory ? "directory" : "file",
                                              shown, file_reason(answer));

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
                string_format(log_error,
                              "mktemp: failed to create %s via template '%s': File exists\n",
                              directory ? "directory" : "file", shown);

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

/*
        The external kill is util-linux's, which is a different program from
        the shell builtin of the same name: it names signals in a table of
        its own, prints that table for -l and -L, and says what it could not
        do rather than answering with a number alone. The shared kill_name
        above is the builtin's spelling and four other programs read it, so
        the table here is this program's own and leaves that one alone.
*/
typedef struct
{
        p8 number;
        string_address name;
} kill_row;

static const kill_row kill_table[] = {
    {1, "HUP"},     {2, "INT"},     {3, "QUIT"},    {4, "ILL"},    {5, "TRAP"},
    {6, "ABRT"},    {6, "IOT"},     {7, "BUS"},     {8, "FPE"},    {9, "KILL"},
    {10, "USR1"},   {11, "SEGV"},   {12, "USR2"},   {13, "PIPE"},  {14, "ALRM"},
    {15, "TERM"},   {16, "STKFLT"}, {17, "CHLD"},   {17, "CLD"},   {18, "CONT"},
    {19, "STOP"},   {20, "TSTP"},   {21, "TTIN"},   {22, "TTOU"},  {23, "URG"},
    {24, "XCPU"},   {25, "XFSZ"},   {26, "VTALRM"}, {27, "PROF"},  {28, "WINCH"},
    {29, "IO"},     {29, "POLL"},   {30, "PWR"},    {31, "SYS"},   {34, "RTMIN"},
    {64, "RTMAX"},
};

// The names -l lists: the table without the two real-time ends, and the
// three shapes a real-time signal is written in rather than every number
// they stand for.
static fn kill_names_listed()
{
        for (positive i = 0; i < array_count(kill_table); i++)
        {
                if (kill_table[i].number == 34 || kill_table[i].number == 64)
                        continue;

                file_line(kill_table[i].name);
        }

        file_line((string_address) "RT<N>");
        file_line((string_address) "RTMIN+<N>");
        file_line((string_address) "RTMAX-<N>");
}

static fn kill_table_written(writer write)
{
        for (positive i = 0; i < array_count(kill_table); i++)
        {
                positive_to_padded(write, kill_table[i].number, 2, ' ', 0);
                write(" ", 1);
                string_to_field(write, kill_table[i].name, 8, ' ', true);
                write("\n", 1);
        }
}

// A number's name: the table for the ones that have one, and RTn above the
// real-time floor, which is how util-linux writes them for -l.
/*
        Which spelling a real-time signal gets.

        util-linux's kill calls signal 34 RT0 and 64 RT30; the shells call the
        same two RTMIN and RTMAX. Both are right for the program a caller
        thinks it is running, so the utility keeps its own and the shell
        builtin sets this while it borrows the utility's parser.
*/
static bool kill_shell_spelling;

static bool kill_number_named(positive number, p8 address_to into)
{
        if (!number)
        {
                string_copy(into, (string_address) "0");
                return true;
        }

        for (positive i = 0; i < array_count(kill_table); i++)
                if (kill_table[i].number == number && number < 32)
                {
                        string_copy(into, kill_table[i].name);
                        return true;
                }

        if (number >= KILL_LEAST_REAL && number <= KILL_MOST)
        {
                if (kill_shell_spelling)
                {
                        kill_name(number, into);
                        return true;
                }

                p8 address_to at = into;

                address_to at++ = 'R';
                address_to at++ = 'T';
                positive_into_string(at, number - KILL_LEAST_REAL);
                return true;
        }

        return false;
}

/*
        A real-time signal as a shell writes it: RTMIN, RTMAX, or an offset
        from either. This is the reading side of the spelling kill_name
        writes. util-linux calls the same signals RT0 to RT30 and knows none
        of these, so the caller says which vocabulary it is speaking.
*/
static bipolar kill_real_time_of(string_address word)
{
        positive offset;
        string_address rest;

        if (!string_compare_folded_max(word, "RTMIN", 5))
        {
                rest = word + 5;

                if (!rest[0])
                        return KILL_LEAST_REAL;

                if (rest[0] != '+' ||
                    !string_digits_exact(rest + 1, address_of offset) ||
                    KILL_LEAST_REAL + offset > KILL_MOST)
                        return -1;

                return (bipolar)(KILL_LEAST_REAL + offset);
        }

        if (!string_compare_folded_max(word, "RTMAX", 5))
        {
                rest = word + 5;

                if (!rest[0])
                        return KILL_MOST;

                if (rest[0] != '-' ||
                    !string_digits_exact(rest + 1, address_of offset) ||
                    offset > KILL_MOST - KILL_LEAST_REAL)
                        return -1;

                return (bipolar)(KILL_MOST - offset);
        }

        return -1;
}

// A signal as a word: a number, a name, SIG in front of one, or an RT
// spelling. Answers -1 for anything else.
static bipolar kill_signal_of(string_address word)
{
        positive number;

        if (string_digits_exact(word, address_of number))
                return number <= KILL_MOST ? (bipolar)number : -1;

        //      Bash reads SIG in front of a name and dash does not, so
        //      `kill -s SIGINT` is a signal in one shell and an error in the
        //      other. The utility reads it whichever shell is running it.
        if ((!kill_shell_spelling || shell_bash_compat) &&
            string_is(word, 'S') && string_is(word + 1, 'I') && string_is(word + 2, 'G'))
                word += 3;

        for (positive i = 0; i < array_count(kill_table); i++)
                if (!string_compare(word, kill_table[i].name))
                        return kill_table[i].number;

        //      Each vocabulary knows only its own real-time spelling: the
        //      shells answer RTMIN and refuse RT0, the utility the reverse.
        if (kill_shell_spelling)
                return kill_real_time_of(word);

        if (string_is(word, 'R') && string_is(word + 1, 'T') &&
            string_digits_exact(word + 2, address_of number) &&
            KILL_LEAST_REAL + number <= KILL_MOST)
                return (bipolar)(KILL_LEAST_REAL + number);

        return -1;
}

static b32 kill_list(positive count, positive index)
{
        p8 name[16];

        if (index >= count)
        {
                kill_names_listed();
                log_flush();
                return 0;
        }

        string_address word = program_argument((b32)index);
        positive number;

        if (string_digits_exact(word, address_of number))
        {
                // A status carries the signal that ended a process in its low
                // seven bits, which is what a caller of -l usually has.
                if (number > 128)
                        number -= 128;

                if (!kill_number_named(number, name))
                {
                        string_format(log_error, "kill: unknown signal: %s\n", word);
                        return 1;
                }

                file_line(name);
                log_flush();
                return 0;
        }

        bipolar found = kill_signal_of(word);

        if (found < 0 || found >= KILL_LEAST_REAL)
        {
                string_format(log_error, "kill: unknown signal: %s\n", word);
                return 1;
        }

        // A name given to -l is answered with the name, which is what
        // util-linux answers with.
        file_line(string_is(word, 'S') && string_is(word + 1, 'I') &&
                          string_is(word + 2, 'G')
                      ? word + 3
                      : word);
        log_flush();
        return 0;
}

#define KILL_PIDFD_OPEN 434

/*
        The three signal masks a process carries, read out of the one line
        each occupies in /proc/<pid>/status. -r asks whether a handler is
        there before signalling, and -d prints all three; both are the same
        record read the same way, so it is read once here.
*/
static bipolar kill_process_status(string_address pid, p8 address_to into,
                                   positive capacity)
{
        p8 path[FILE_PATH_MAX];
        positive at = 0;

        string_copy(path, (string_address) "/proc/");
        at = string_length(path);

        for (positive i = 0; string_get(pid + i) && at + 9 < sizeof(path); i++)
                path[at++] = string_get(pid + i);

        string_copy(path + at, (string_address) "/status");

        return file_slurp_once_at(AT_FDCWD, path, into, capacity);
}

// The hexadecimal mask written after one of the Sig* labels, or false when
// this record has no such line.
static bool kill_status_mask(string_address text, string_address label,
                             positive address_to mask)
{
        for (positive at = 0; text[at]; at++)
        {
                if (at && text[at - 1] != '\n')
                        continue;

                if (string_compare_max(text + at, label, string_length(label)))
                        continue;

                positive from = at + string_length(label);

                while (text[from] == ' ' || text[from] == '\t')
                        from++;

                positive value = 0;
                bool any = false;

                for (; byte_is_hexadecimal(text[from]); from++)
                {
                        p8 byte = text[from];
                        positive digit = byte_is_digit(byte) ? (positive)(byte - '0')
                                         : byte >= 'a'       ? (positive)(byte - 'a' + 10)
                                                             : (positive)(byte - 'A' + 10);

                        value = value * 16 + digit;
                        any = true;
                }

                if (!any)
                        return false;

                address_to mask = value;
                return true;
        }

        return false;
}

// "Blocked: HUP INT ", the way the utility writes one mask out.
static fn kill_mask_written(string_address label, positive mask)
{
        p8 name[16];

        string_format(log, "%s: ", label);

        for (positive i = 1; i <= KILL_MOST; i++)
                if (mask >> (i - 1) & 1)
                {
                        kill_number_named(i, name);
                        string_format(log, "%s ", name);
                }

        log("\n", 1);
}

static b32 kill_process_state(string_address pid)
{
        positive used;
        bipolar who = string_bipolar(pid, address_of used);

        if (!used || string_get(pid + used) || who < 0)
                return string_report(log_error, 1,
                                     "kill: invalid PID argument: '%s'\n", pid);

        p8 text[8192];
        bipolar got = kill_process_status(pid, text, sizeof(text));

        if (got < 0)
                return string_report(log_error, 1,
                                     "kill: failed to initialize procfs handler: %s\n",
                                     file_reason(got));

        static const struct
        {
                string_address label;
                string_address line;
        } wanted[] = {
            {(string_address) "SigBlk:", (string_address) "Blocked"},
            {(string_address) "SigIgn:", (string_address) "Ignored"},
            {(string_address) "SigCgt:", (string_address) "Caught"},
        };

        for (positive i = 0; i < array_count(wanted); i++)
        {
                positive mask = 0;

                kill_status_mask(text, wanted[i].label, address_of mask);
                kill_mask_written(wanted[i].line, mask);
        }

        log_flush();
        return 0;
}

// -r: a signal with no handler on the other side is not sent at all.
static bool kill_handled(string_address pid, bipolar number)
{
        p8 text[8192];
        positive mask = 0;

        if (number <= 0 || number > KILL_MOST)
                return false;

        if (kill_process_status(pid, text, sizeof(text)) < 0)
                return false;

        if (!kill_status_mask(text, (string_address) "SigCgt:", address_of mask))
                return false;

        return (mask >> (number - 1) & 1) != 0;
}

static b32 file_kill()
{
        positive count = (positive)program_argument_count();
        positive index = 1;
        bipolar number = 15;
        b32 answer = 0;
        bool print_only = false;
        bool loud = false;
        bool timed = false;
        bool needs_handler = false;
        bool show_state = false;
        string_address state_pid = null;
        positive milliseconds = 0;

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

                string_address name = argument + 1;
                bool longer = string_is(name, '-');
                string_address value = null;

                if (longer)
                {
                        name++;

                        string_address equals = string_first_of(name, '=');

                        if (equals)
                        {
                                // The word is cut where its value begins.
                                address_to equals = end;
                                value = equals + 1;
                        }
                }

                bool one = !longer && !string_get(name + 1);

                if ((one && string_is(name, 'p')) || (longer && !string_compare(name, "pid")))
                {
                        print_only = true;
                        index++;
                        continue;
                }

                if ((one && string_is(name, 'a')) || (longer && !string_compare(name, "all")))
                {
                        index++;
                        continue;
                }

                if (longer && !string_compare(name, "verbose"))
                {
                        loud = true;
                        index++;
                        continue;
                }

                if ((one && string_is(name, 'r')) ||
                    (longer && !string_compare(name, "require-handler")))
                {
                        needs_handler = true;
                        index++;
                        continue;
                }

                //      -d is a bare letter only: the reference reads -d1 as
                //      a signal called d1, and takes the process to look at
                //      from --show-process-state=PID or from the one operand.
                if ((one && string_is(name, 'd')) ||
                    (longer && !string_compare(name, "show-process-state")))
                {
                        show_state = true;
                        state_pid = value;
                        index++;
                        continue;
                }

                if ((one && string_is(name, 'L')) || (longer && !string_compare(name, "table")))
                {
                        kill_table_written(log);
                        log_flush();
                        return 0;
                }

                if ((one && string_is(name, 'l')) || (longer && !string_compare(name, "list")))
                {
                        if (value)
                        {
                                p8 named[16];
                                positive listed;

                                if (string_digits_exact(value, address_of listed) &&
                                    kill_number_named(listed, named))
                                {
                                        file_line(named);
                                        log_flush();
                                        return 0;
                                }

                                string_format(log_error, "kill: unknown signal: %s\n", value);
                                return 1;
                        }

                        return kill_list(count, index + 1);
                }

                if ((one && (string_is(name, 's') || string_is(name, 'q'))) ||
                    (longer && (!string_compare(name, "signal") ||
                                !string_compare(name, "queue"))))
                {
                        bool queued = string_is(name, 'q') || !string_compare(name, "queue");

                        if (!value)
                        {
                                if (index + 1 >= count)
                                {
                                        log_error("kill: not enough arguments\n", 0);
                                        return 2;
                                }

                                value = program_argument((b32)(index + 1));
                                index++;
                        }

                        index++;

                        if (queued)
                        {
                                positive queue;

                                if (!string_digits_exact(value, address_of queue))
                                {
                                        string_format(log_error,
                                                      "kill: invalid sigval argument: %s\n", value);
                                        return 1;
                                }

                                continue;
                        }

                        number = kill_signal_of(value);

                        if (number < 0)
                        {
                                string_format(log_error,
                                              "kill: unknown signal %s; valid signals:\n", value);
                                kill_table_written(log_error);
                                return 1;
                        }

                        continue;
                }

                if (longer && !string_compare(name, "timeout"))
                {
                        if (index + 2 >= count)
                        {
                                log_error("kill: not enough arguments\n", 0);
                                return 2;
                        }

                        string_address written = value ? value
                                                       : program_argument((b32)(index + 1));

                        if (!value)
                                index++;

                        if (!string_digits_exact(written, address_of milliseconds))
                        {
                                string_format(log_error, "kill: invalid timeout argument: %s\n",
                                              written);
                                return 1;
                        }

                        string_address wanted = program_argument((b32)(index + 1));

                        number = kill_signal_of(wanted);

                        if (number < 0)
                        {
                                string_format(log_error,
                                              "kill: unknown signal %s; valid signals:\n", wanted);
                                kill_table_written(log_error);
                                return 1;
                        }

                        timed = true;
                        index += 2;
                        continue;
                }

                // Anything else that begins with a dash is the signal
                // itself. What is quoted back is the word without the one
                // dash that made it an option, so a long-looking word keeps
                // the second one: -bogus-option is reported as -bogus-option.
                number = kill_signal_of(name);

                if (number < 0)
                {
                        string_format(log_error, "kill: invalid signal name or number: %s\n",
                                      argument + 1);
                        return 1;
                }

                index++;

                // One signal and no more, or a negative process group would be
                // read as a second one.
                break;
        }

        if (show_state)
        {
                //      --show-process-state=PID carries the process with it
                //      and the reference then looks at nothing else; a bare
                //      -d takes the one operand, and wants exactly one.
                if (state_pid)
                        return kill_process_state(state_pid);

                if (index >= count)
                        return string_report(log_error, 1, "kill: too few arguments\n");

                if (count - index > 1)
                        return string_report(log_error, 1, "kill: too many arguments\n");

                return kill_process_state(program_argument((b32)index));
        }

        if (index >= count)
        {
                log_error("kill: not enough arguments\n", 0);
                return 2;
        }

        while (index < count)
        {
                string_address word = program_argument((b32)index++);
                positive used;
                bipolar who = string_bipolar(word, address_of used);

                // A word that is not a number names a process, and this one
                // has no process table to look the name up in.
                if (!used || string_get(word + used))
                {
                        string_format(log_error, "kill: cannot find process \"%s\"\n", word);
                        answer = 1;
                        continue;
                }

                if (print_only)
                {
                        file_line(word);
                        continue;
                }

                //      -r looks first and says nothing: a process with no
                //      handler for this signal is left alone, and so is one
                //      that is not there to be asked.
                if (needs_handler && !kill_handled(word, number))
                {
                        answer = 1;
                        continue;
                }

                if (loud)
                {
                        string_format(log, "sending signal %p to pid %s\n",
                                      (positive)number, word);
                        log_flush();
                }

                if (timed)
                {
                        // The wait needs a handle on the process, and a
                        // process that is not there has none to give.
                        bipolar handle = system_call_2(KILL_PIDFD_OPEN, (positive)who, 0);

                        if (handle < 0)
                        {
                                string_format(log_error,
                                              "kill: failed to obtain a valid file descriptor "
                                              "for PID %s: %s\n",
                                              word, file_reason(handle));
                                answer = 1;
                                continue;
                        }

                        system_close(handle);
                }

                bipolar done = system_call_2(syscall(kill), (positive)who, (positive)number);

                if (done < 0)
                {
                        string_format(log_error, "kill: sending signal to %s failed: %s\n",
                                      word, file_reason(done));
                        answer = 1;
                }
        }

        log_flush();

        return answer;
}

// rename ----------------------------------------------------------

static const file_long rename_longs[] = {
    {(string_address)"verbose", 'v'},
    {(string_address)"symlink", 's'},
    {(string_address)"no-act", 'n'},
    {(string_address)"all", 'a'},
    {(string_address)"last", 'l'},
    {(string_address)"no-overwrite", 'o'},
    {(string_address)"interactive", 'i'},
    {(string_address)"help", 'h'},
    {(string_address)"version", 'V'},
    {null, 0},
};

/* Return zero for no occurrence, one for a complete new name and two when
   that name cannot fit in the kernel pathname boundary. */
static p8 rename_name(string_address source, string_address before,
                      string_address after, bool every, bool last, bool whole,
                      p8 address_to into)
{
        string_address slash = whole ? null : string_last_of(source, '/');
        positive prefix = slash ? (positive)(slash - source) + 1 : 0;
        string_address name = source + prefix;
        positive source_length = string_length(name);
        positive before_length = string_length(before);
        positive after_length = string_length(after);
        positive matches = 0;
        positive position = 0;

        if (!before_length)
        {
                matches = every ? source_length + 1 : 1;
                position = last ? source_length : 0;
        }
        else if (every)
        {
                positive at = 0;
                while (at <= source_length - min(source_length, before_length))
                {
                        p8 address_to found = (p8 address_to)memory_search(
                            (address_any)(name + at), source_length - at,
                            (address_any)before, before_length);
                        if (!found)
                                break;
                        matches++;
                        at = (positive)(found - name) + before_length;
                }
        }
        else
        {
                p8 address_to found = (p8 address_to)memory_search(
                    (address_any)name, source_length, (address_any)before,
                    before_length);
                while (found)
                {
                        matches = 1;
                        position = (positive)(found - name);
                        if (!last || position + 1 >= source_length)
                                break;
                        p8 address_to later = (p8 address_to)memory_search(
                            (address_any)(name + position + 1),
                            source_length - position - 1,
                            (address_any)before, before_length);
                        if (!later)
                                break;
                        found = later;
                }
        }

        if (!matches)
                return 0;
        if (matches > positive_max / max(after_length, (positive)1))
                return 2;
        positive removed = matches * before_length;
        positive added = matches * after_length;
        if (removed > source_length || added > positive_max - source_length + removed)
                return 2;
        positive length = prefix + source_length - removed + added;
        if (length >= FILE_PATH_MAX)
                return 2;

        positive made = prefix;
        memory_copy_apart(into, source, prefix);
        if (!every)
        {
                memory_copy_apart(into + made, name, position);
                made += position;
                memory_copy_apart(into + made, after, after_length);
                made += after_length;
                positive tail = position + before_length;
                memory_copy_apart(into + made, name + tail,
                                  source_length - tail);
                made += source_length - tail;
        }
        else if (!before_length)
        {
                for (positive at = 0; at <= source_length; at++)
                {
                        memory_copy_apart(into + made, after, after_length);
                        made += after_length;
                        if (at < source_length)
                                into[made++] = name[at];
                }
        }
        else
        {
                positive at = 0;
                while (at < source_length)
                {
                        p8 address_to found = (p8 address_to)memory_search(
                            (address_any)(name + at), source_length - at,
                            (address_any)before, before_length);
                        if (!found)
                                break;
                        positive place = (positive)(found - name);
                        memory_copy_apart(into + made, name + at, place - at);
                        made += place - at;
                        memory_copy_apart(into + made, after, after_length);
                        made += after_length;
                        at = place + before_length;
                }
                memory_copy_apart(into + made, name + at,
                                  source_length - at);
                made += source_length - at;
        }
        into[made] = end;
        return string_equals(source, into) ? 0 : 1;
}

static bool rename_ask(string_address destination)
{
        string_format(log, "rename: overwrite `%s'? ", destination);
        log_flush();

        p8 answer;
        bipolar got = system_read_once(0, address_of answer, 1);
        bool yes = got == 1 && (answer == 'y' || answer == 'Y');

        while (got == 1 && answer != '\n')
                got = system_read_once(0, address_of answer, 1);
        return yes;
}

/*
        Two pairs that cannot both be asked for, refused where the second of
        a pair is read.

        The reference keeps the first option of each pair it has seen and
        complains as soon as another from the same pair arrives, naming the
        two in the order they were written -- so -o ... -i is "--no-overwrite
        and --interactive" and -i ... -o is the same two the other way round,
        and whichever pair is completed first is the one reported.
*/
static p8 rename_all_last;
static p8 rename_ask_keep;

static string_address rename_spelled(p8 letter)
{
        return letter == 'a'   ? (string_address) "--all"
               : letter == 'l' ? (string_address) "--last"
               : letter == 'i' ? (string_address) "--interactive"
                               : (string_address) "--no-overwrite";
}

static bool rename_exclusive(p8 address_to kept, p8 letter)
{
        if (!address_to kept)
        {
                address_to kept = letter;
                return true;
        }

        if (address_to kept == letter)
                return true;

        string_format(log_error, "rename: options %s and %s cannot be combined\n",
                      rename_spelled(address_to kept), rename_spelled(letter));

        return false;
}

static bool rename_option_seen(p8 letter, string_address value)
{
        if (letter == 'a' || letter == 'l')
                return rename_exclusive(address_of rename_all_last, letter);

        if (letter == 'i' || letter == 'o')
                return rename_exclusive(address_of rename_ask_keep, letter);

        return true;
}

static b32 file_rename()
{
        file_operands_begin();
        rename_all_last = 0;
        rename_ask_keep = 0;

        file_taking taking = {
            .program = (string_address)"rename",
            .allowed = (string_address)"vsnaloihV",
            .valued = (string_address)"",
            .longs = rename_longs,
            .operand = file_operand,
            .seen = rename_option_seen,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;
        if (file_meta(address_of taking, "[options] SUBSTRING REPLACEMENT FILE...\n"
                      "  -n no-act  -v verbose  -a all  -l last\n"
                      "  -o no-overwrite  -i interactive", log))
                return 0;
        if (file_operand_count < 3)
                return string_report(log_error, 1, "rename: not enough arguments\n");
        bool symlinks = (taking.flags & FILE_FLAG('s')) != 0;

        string_address before = file_operand_at(0);
        string_address after = file_operand_at(1);
        bool no_act = (taking.flags & FILE_FLAG('n')) != 0;
        bool verbose = (taking.flags & FILE_FLAG('v')) != 0;
        bool no_overwrite = (taking.flags & FILE_FLAG('o')) != 0;
        bool interactive = !no_act && !no_overwrite &&
                           (taking.flags & FILE_FLAG('i')) != 0;
        positive renamed = 0;
        bool failed = false;

        for (positive at = 2; at < file_operand_count; at++)
        {
                string_address source = file_operand_at(at);
                p8 destination[FILE_PATH_MAX];
                p8 target[FILE_PATH_MAX];
                file_facts facts;
                bipolar looked = file_look_code(AT_FDCWD, source, AT_SYMLINK_NOFOLLOW,
                                                address_of facts);

                if (looked < 0)
                {
                        string_format(log_error, "rename: %s: not accessible: %s\n", source,
                                      file_reason(looked));
                        failed = true;
                        continue;
                }

                // -s rewrites what a link points at, the whole of it, and the
                // link is made again to say the new target.
                if (symlinks)
                {
                        if ((facts.mode & MODE_FORMAT) != MODE_LINK ||
                            file_link_text(source, target, FILE_PATH_MAX) < 0)
                        {
                                string_format(log_error, "rename: %s: not a symbolic link\n",
                                              source);
                                failed = true;
                                continue;
                        }

                        p8 made = rename_name(
                            (string_address)target, before, after,
                            (taking.flags & FILE_FLAG('a')) != 0,
                            (taking.flags & FILE_FLAG('l')) != 0, true, destination);

                        if (!made)
                                continue;
                        if (made == 2)
                        {
                                string_format(log_error,
                                              "rename: new name for '%s' is too long\n",
                                              source);
                                failed = true;
                                continue;
                        }

                        if (no_overwrite || interactive)
                        {
                                file_facts there;

                                if (file_look_link((string_address)destination, address_of there) &&
                                    (no_overwrite || !rename_ask((string_address)destination)))
                                        continue;
                        }

                        if (!no_act)
                        {
                                system_remove_at(AT_FDCWD, source, 0);

                                bipolar linked = system_symbolic_link_at(
                                    (string_address)destination, AT_FDCWD, source);

                                if (linked < 0)
                                {
                                        string_format(log_error,
                                                      "rename: %s: symlinking to %s failed: %s\n",
                                                      source, destination, file_reason(linked));
                                        failed = true;
                                        continue;
                                }
                        }

                        if (verbose)
                                string_format(log, "%s: `%s' -> `%s'\n", source, target,
                                              destination);
                        renamed++;
                        continue;
                }

                p8 made = rename_name(
                    source, before, after,
                    (taking.flags & FILE_FLAG('a')) != 0,
                    (taking.flags & FILE_FLAG('l')) != 0, false, destination);

                if (!made)
                        continue;
                if (made == 2)
                {
                        string_format(log_error,
                                      "rename: new name for '%s' is too long\n",
                                      source);
                        failed = true;
                        continue;
                }

                if (no_overwrite)
                {
                        file_facts facts;
                        if (file_look_at(destination, address_of facts))
                                continue;
                }
                else if (interactive)
                {
                        file_facts facts;
                        if (file_look_at(destination, address_of facts) &&
                            !rename_ask(destination))
                                continue;
                }

                if (!no_act)
                {
                        bipolar answer = system_rename_at(
                            AT_FDCWD, source, AT_FDCWD, destination,
                            no_overwrite ? HARDLINK_RENAME_NOREPLACE : 0);
                        if (answer < 0)
                        {
                                if (no_overwrite && answer == -ERROR_EXISTS)
                                        continue;
                                string_format(log_error,
                                              "rename: %s: rename to %s failed: %s\n",
                                              source, destination,
                                              file_reason(answer));
                                failed = true;
                                continue;
                        }
                }
                if (verbose)
                        string_format(log, "`%s' -> `%s'\n", source,
                                      destination);
                renamed++;
        }

        log_flush();

        // The reference's four answers: everything renamed, everything
        // failed, some of each, or nothing to rename at all.
        if (failed)
                return renamed ? 2 : 1;

        return renamed ? 0 : 4;
}

// cal -------------------------------------------------------------
/*
        Calendar arithmetic stays beside date's one civil-time engine.  The
        ordinary Gregorian path is clock_days_from_civil; the small Julian
        leaf exists only for util-linux's historical 1752 default.  Rendering
        is bounded to three fixed month blocks and writes through the shared
        log buffer, so a year does not allocate or build a second text engine.
*/
#define CAL_ROWS 8
#define CAL_NORMAL_WIDTH 20
#define CAL_JULIAN_WIDTH 27

typedef struct
{
        p8 row[CAL_ROWS][CAL_JULIAN_WIDTH];
} cal_month_block;

static const file_long cal_longs[] = {
    {(string_address)"one", '1'},
    {(string_address)"three", '3'},
    {(string_address)"months", 'n'},
    {(string_address)"span", 'S'},
    {(string_address)"sunday", 's'},
    {(string_address)"monday", 'm'},
    {(string_address)"julian", 'j'},
    {(string_address)"reform", 'R'},
    {(string_address)"iso", 'I'},
    {(string_address)"year", 'y'},
    {(string_address)"twelve", 'Y'},
    {(string_address)"week", 'w'},
    {(string_address)"vertical", 'v'},
    {(string_address)"columns", 'c'},
    {(string_address)"color", 'C'},
    {(string_address)"help", 'h'},
    {(string_address)"version", 'V'},
    {null, 0},
};

static bipolar cal_month_number(string_address text)
{
        positive value;
        if (string_digits_exact(text, address_of value))
                return value >= 1 && value <= 12 ? (bipolar)value : -1;

        positive length = string_length(text);
        bipolar named = file_name_among(text, length, file_month_names, 12);
        return named < 0 ? -1 : named + 1;
}

static bool cal_julian_leap(b64 year)
{
        return year % 4 == 0;
}

/* The British reform used by util-linux's default calendar: September 2,
   1752 was followed by September 14. */
static bool cal_gregorian_date(b64 year, positive month, positive day,
                               bool proleptic)
{
        return proleptic || year > 1752 ||
               (year == 1752 &&
                (month > 9 || (month == 9 && day >= 14)));
}

static positive cal_days_in_month(b64 year, positive month, bool proleptic)
{
        if (month != 2)
                return file_month_days(year, month);
        if (!cal_gregorian_date(year, month, 1, proleptic))
                return cal_julian_leap(year) ? 29 : 28;
        return file_month_days(year, month);
}

static positive cal_weekday(b64 year, positive month, positive day,
                            bool proleptic)
{
        b64 weekday;

        if (cal_gregorian_date(year, month, day, proleptic))
        {
                b64 days = clock_days_from_civil(year, month, day);
                weekday = (days + 4) % 7;
        }
        else
        {
                b64 adjust = (14 - (b64)month) / 12;
                b64 y = year + 4800 - adjust;
                b64 m = (b64)month + adjust * 12 - 3;
                b64 julian_day = (b64)day + (153 * m + 2) / 5 +
                                  365 * y + y / 4 - 32083;
                weekday = (julian_day + 1) % 7;
        }

        return (positive)(weekday < 0 ? weekday + 7 : weekday);
}

static positive cal_ordinal(b64 year, positive month, positive day,
                            bool proleptic)
{
        positive answer = day;
        for (positive before = 1; before < month; before++)
                answer += cal_days_in_month(year, before, proleptic);
        return answer;
}

static positive cal_year_into(p8 address_to into, b64 year)
{
        if (year < 0)
        {
                into[0] = '-';
                return 1 + positive_into_padded(into + 1, (positive)-year,
                                                3, '0');
        }
        return positive_into_padded(into, (positive)year, 4, '0');
}

static fn cal_center(p8 address_to row, positive width,
                     p8 address_to text, positive length)
{
        memory_fill(row, ' ', width);
        if (length > width)
                length = width;
        positive left = (width - length + 1) / 2;
        memory_copy_apart(row + left, text, length);
}

static fn cal_put_value(p8 address_to row, positive column,
                        positive cell, positive value)
{
        p8 digits[32];
        positive length = positive_into_string(digits, value);
        if (length > cell)
                length = cell;
        memory_copy_apart(row + column + cell - length, digits, length);
}

static fn cal_render_month(cal_month_block address_to block, b64 year,
                           positive month, bool with_year, bool monday,
                           bool julian, bool proleptic)
{
        positive width = julian ? CAL_JULIAN_WIDTH : CAL_NORMAL_WIDTH;
        positive cell = julian ? 3 : 2;
        positive step = cell + 1;
        p8 title[64];
        positive title_length = 0;
        string_address month_name = file_month_names[month - 1];
        positive month_length = string_length(month_name);

        memory_fill(block, ' ', sizeof(*block));
        memory_copy_apart(title, month_name, month_length);
        title[0] -= 'a' - 'A';
        title_length = month_length;
        if (with_year)
        {
                title[title_length++] = ' ';
                title_length += cal_year_into(title + title_length, year);
        }
        cal_center(block->row[0], width, title, title_length);

        static p8 sunday_normal[] = "Su Mo Tu We Th Fr Sa";
        static p8 monday_normal[] = "Mo Tu We Th Fr Sa Su";
        static p8 sunday_julian[] = "Sun Mon Tue Wed Thu Fri Sat";
        static p8 monday_julian[] = "Mon Tue Wed Thu Fri Sat Sun";
        memory_copy_apart(block->row[1],
                          julian ? (monday ? monday_julian : sunday_julian)
                                 : (monday ? monday_normal : sunday_normal),
                          width);

        positive weekday = cal_weekday(year, month, 1, proleptic);
        if (monday)
                weekday = (weekday + 6) % 7;
        positive week = 0;
        positive days = cal_days_in_month(year, month, proleptic);

        for (positive day = 1; day <= days; day++)
        {
                if (!proleptic && year == 1752 && month == 9 &&
                    day >= 3 && day <= 13)
                        continue;

                positive value = julian ? cal_ordinal(year, month, day,
                                                      proleptic)
                                         : day;
                cal_put_value(block->row[week + 2], weekday * step,
                              cell, value);
                if (++weekday == 7)
                {
                        weekday = 0;
                        week++;
                }
        }
}

static fn cal_month_at(b64 serial, b64 address_to year,
                       positive address_to month)
{
        b64 before = clock_floor_divide(serial, 12);
        address_to year = before + 1;
        address_to month = (positive)(serial - before * 12 + 1);
}

static fn cal_emit_group(b64 first, positive count, bool with_year,
                         bool monday, bool julian, bool proleptic,
                         positive separation)
{
        cal_month_block blocks[3];
        positive width = julian ? CAL_JULIAN_WIDTH : CAL_NORMAL_WIDTH;

        for (positive i = 0; i < count; i++)
        {
                b64 year;
                positive month;
                cal_month_at(first + i, address_of year, address_of month);
                cal_render_month(blocks + i, year, month, with_year, monday,
                                 julian, proleptic);
        }

        static p8 spaces[] = "   ";
        for (positive row = 0; row < CAL_ROWS; row++)
        {
                for (positive column = 0; column < count; column++)
                {
                        if (column)
                                log(spaces, separation);
                        log(blocks[column].row[row], width);
                }
                log("\n", 1);
        }
}

static fn cal_emit_year_title(b64 year, positive width)
{
        p8 title[32];
        p8 row[CAL_JULIAN_WIDTH * 3 + 6];
        positive length = cal_year_into(title, year);
        cal_center(row, width, title, length);
        log(row, width);
        log("\n\n", 2);
}

static b32 file_cal()
{
        file_operands_begin();
        p8 week_start = 0;
        const file_supersede supersedes[] = {
            {(string_address)"sm", address_of week_start},
            {null, null},
        };
        file_taking taking = {
            .program = (string_address)"cal",
            .allowed = (string_address)"13nSsmjRIyYwvcChV",
            .valued = (string_address)"nRc",
            .long_optional = (string_address)"wC",
            .longs = cal_longs,
            .operand = file_operand,
            .supersedes = supersedes,
        };

        if (!file_take(address_of taking) || file_operand_failed)
                return 1;
        if (file_meta(address_of taking, "[-1|-3|-y|-Y] [-n MONTHS] [-Ssmj] [[MONTH] YEAR]", log_error))
                return 0;
        if (taking.flags & (FILE_FLAG('w') | FILE_FLAG('v') |
                            FILE_FLAG('c')))
                return string_report(log_error, 1, "cal: week numbers, vertical layout and custom columns are unsupported\n");

        string_address color = file_option_value(address_of taking, 'C');
        if (taking.flags & FILE_FLAG('C'))
        {
                if ((taking.bare & FILE_FLAG('C')) || !color ||
                    string_compare(color, (string_address)"never"))
                        return string_report(log_error, 1, "cal: only --color=never is supported\n");
        }

        bool proleptic = false;
        string_address reform = file_option_value(address_of taking, 'R');
        if (taking.flags & FILE_FLAG('I'))
                proleptic = true;
        if (reform)
        {
                if (!string_compare(reform, (string_address)"gregorian") ||
                    !string_compare(reform, (string_address)"iso"))
                        proleptic = true;
                else if (!string_compare(reform, (string_address)"1752"))
                        proleptic = false;
                else
                        return string_report(log_error, 1, "cal: only the 1752 and Gregorian reforms are supported\n");
        }

        time_t now = (time_t)file_now();
        tm broken;
        if (!localtime_r(address_of now, address_of broken))
                return string_report(log_error, 1, "cal: cannot read the current calendar date\n");
        b64 year = (b64)broken.tm_year + 1900;
        positive month = (positive)broken.tm_mon + 1;
        positive selected_day = 1;
        bool year_only = false;

        if (file_operand_count > 3)
                return string_report(log_error, 1, "cal: too many operands\n");
        if (file_operand_count == 1)
        {
                string_address word = file_operand_at(0);
                positive number;
                bipolar named;
                if (string_digits_exact(word, address_of number))
                {
                        if (!number || number > 2147483646U)
                        {
                                log_error("cal: illegal year value: use positive integer\n", 0);
                                return 1;
                        }
                        year = (b64)number;
                        year_only = true;
                }
                else if ((named = cal_month_number(word)) > 0)
                        month = (positive)named;
                else
                {
                        b64 stamp;
                        positive nanoseconds;
                        if (!file_moment_read_exact(word, (b64)now,
                                                    address_of stamp,
                                                    address_of nanoseconds))
                                return string_report(log_error, 1,
                                              "cal: cannot parse date '%s'\n",
                                              word);
                        positive hour, minute, second;
                        file_split_moment(stamp, address_of year,
                                          address_of month,
                                          address_of selected_day,
                                          address_of hour, address_of minute,
                                          address_of second);
                }
        }
        else if (file_operand_count >= 2)
        {
                bipolar named = cal_month_number(
                    file_operand_at(file_operand_count == 2 ? 0 : 1));
                positive parsed_year;
                //      A number outside the twelve is an illegal value; a
                //      word is a name this does not know, and the reference
                //      says which of the two it met.
                if (named < 1)
                {
                        string_address written =
                            file_operand_at(file_operand_count == 2 ? 0 : 1);
                        positive value;

                        return string_digits_exact(written, address_of value)
                            ? string_report(log_error, 1,
                                            "cal: illegal month value: use 1-12\n")
                            : string_report(log_error, 1,
                                            "cal: unknown month name: %s\n", written);
                }
                if (!string_digits_exact(file_operand_at(file_operand_count - 1),
                                address_of parsed_year) || !parsed_year ||
                    parsed_year > 2147483646U)
                {
                        log_error("cal: illegal year value: use positive integer\n", 0);
                        return 1;
                }
                month = (positive)named;
                year = parsed_year;
                if (file_operand_count == 3 &&
                    (!string_digits_exact(file_operand_at(0), address_of selected_day) ||
                     !selected_day ||
                     selected_day > cal_days_in_month(year, month,
                                                       proleptic)))
                        return string_report(log_error, 1, "cal: illegal day value\n");
        }

        bool monday = week_start == 'm';
        bool julian = (taking.flags & FILE_FLAG('j')) != 0;
        bool three = (taking.flags & FILE_FLAG('3')) != 0;
        bool twelve = (taking.flags & FILE_FLAG('Y')) != 0;
        bool whole_year = (taking.flags & FILE_FLAG('y')) != 0;
        bool one = (taking.flags & FILE_FLAG('1')) != 0;
        bool span = (taking.flags & FILE_FLAG('S')) != 0;
        string_address months_text = file_option_value(address_of taking, 'n');
        positive months = 1;
        bool months_given = months_text != null;

        if (months_given && !string_digits_exact(months_text, address_of months))
                return string_report(log_error, 1, "cal: invalid month count\n");
        if (!months)
                months = 1;
        if ((p64)months > 25769803776ULL)
                return string_report(log_error, 1, "cal: requested calendar range is out of bounds\n");
        if ((whole_year && (three || months_given || one || twelve)) ||
            (three && (months_given || twelve)) ||
            (one && (three || months_given || twelve)))
                return string_report(log_error, 1, "cal: conflicting calendar range options are unsupported\n");

        b64 first = (year - 1) * 12 + (b64)month - 1;
        bool year_layout = whole_year ||
                           (year_only && !one && !three && !months_given);
        positive separation = 2;

        if (year_layout)
        {
                months = 12;
                first = (year - 1) * 12;
                separation = 3;
        }
        else if (three)
        {
                months = 3;
                first--;
        }
        else if (twelve)
        {
                months = 12;
                separation = 3;
        }

        if (span && months > 1)
                first -= (b64)(months / 2);

        b64 first_year;
        positive first_month;
        b64 last_year;
        positive last_month;
        cal_month_at(first, address_of first_year, address_of first_month);
        cal_month_at(first + (b64)months - 1,
                     address_of last_year, address_of last_month);
        if (first_year < 0 || last_year > 2147483647)
                return string_report(log_error, 1, "cal: requested calendar range is out of bounds\n");

        positive width = julian ? CAL_JULIAN_WIDTH : CAL_NORMAL_WIDTH;
        if (year_layout)
                cal_emit_year_title(year, width * 3 + separation * 2);

        for (positive shown = 0; shown < months; shown += 3)
        {
                positive across = months - shown < 3 ? months - shown : 3;
                cal_emit_group(first + (b64)shown, across, !year_layout, monday,
                               julian, proleptic, separation);
        }
        log_flush();
        return 0;
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
                        return string_report(log_error, 1,
                                      "date: invalid argument '%s' for '--iso-8601'\n",
                                      precision);
        }

        if (index < count)
        {
                string_address argument = program_argument((b32)index++);

                if (!string_is(argument, '+'))
                        return string_report(log_error, 1, "date: cannot set the date: %s\n",
                                      argument);

                format = argument + 1;
        }

        if (index < count)
                return string_report(log_error, 1, "date: too many operands\n");

        if (of_file)
        {
                file_facts facts;
                bipolar looked = file_look_code(AT_FDCWD, of_file, 0, address_of facts);

                if (looked < 0)
                        return string_report(log_error, 1, "date: %s: %s\n", of_file,
                                      file_reason(looked));

                when = (b64)facts.modified.seconds;
        }
        else if (given)
        {
                if (!file_moment_read(given, file_now(), address_of when))
                        return string_report(log_error, 1, "date: invalid date '%s'\n", given);
        }
        else
                when = file_now();

        if (!format)
                format = iso   ? iso
                         : rfc ? (string_address) "%a, %d %b %Y %H:%M:%S %z"
                               : (string_address) "%a %b %e %H:%M:%S %Z %Y";

        if (!date_shape(log, when, format))
                return string_report(log_error, 1, "date: formatted value is too large\n");

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
static p8 xargs_delimiter;
static bool xargs_delimited;
static positive xargs_most_bytes;
static bool xargs_exit_too_long;
static string_address xargs_slot_name;
static bool xargs_said_nul;
static bipolar xargs_input;
static b32 xargs_signal;
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

static bool xargs_ask;

static fn xargs_trace_words(string_address address_to words, positive count)
{
        for (positive i = 0; i < count; i++)
        {
                if (i)
                        log_error(" ", 1);

                ls_quote_shell(log_error, words[i], string_length(words[i]), false, false);
        }

        //      -p writes the same words and then waits on the terminal, so
        //      the line is left open for the question mark that follows.
        if (!xargs_ask)
                log_error("\n", 1);
}

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
#define XARGS_EXEC_TTY (-4099)
#define XARGS_O_CLOEXEC 02000000

/*
        -p asks the terminal before each command, and the terminal it asks is
        /dev/tty and never the input, which is where the items come from.
        A session with no controlling terminal has no /dev/tty to open, and
        the reference stops there -- after it has written the command it was
        about to run, which is why the question mark is written afterwards
        rather than with it.

        An answer that begins with y or Y runs the command; anything else,
        end of input included, skips it and goes on to the next batch.
*/
static bipolar xargs_terminal = -2;

static bool xargs_allowed(void)
{
        log_error("?...", 4);

        p8 answer[2];
        bipolar got = system_read_once((b32)xargs_terminal, answer, 1);

        if (got != 1)
                return false;

        bool yes = answer[0] == 'y' || answer[0] == 'Y';

        while (answer[0] != '\n' &&
               system_read_once((b32)xargs_terminal, answer, 1) == 1)
                ;

        return yes;
}

static bipolar xargs_execute(string_address address_to words,
                             positive word_count)
{
        b32 ends[2];
        positive status = 0;

        words[word_count] = null;

        if (xargs_trace || xargs_ask)
                xargs_trace_words(words, word_count);

        if (xargs_ask)
        {
                if (xargs_terminal == -2)
                        xargs_terminal = system_open_at(AT_FDCWD,
                                                       (string_address) "/dev/tty",
                                                       FILE_READ | XARGS_O_CLOEXEC);

                if (xargs_terminal < 0)
                {
                        string_format(log_error,
                                      "xargs: failed to open /dev/tty for reading: %s\n",
                                      file_reason(xargs_terminal));
                        return XARGS_EXEC_TTY;
                }

                if (!xargs_allowed())
                        return 0;
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
        {
                xargs_signal = (b32)(status & 0x7f);
                return XARGS_EXEC_SIGNAL;
        }

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
                        log_error("xargs: argument list too long\n", 0);
                        xargs_answer = 1;
                        xargs_done = true;
                        return false;
                }

                positive left = count / 2;

                return xargs_execute_range(first, left) &&
                       xargs_execute_range(first + left, count - left);
        }

        if (code == XARGS_EXEC_TTY)
        {
                xargs_answer = 1;
                xargs_done = true;
                return false;
        }

        if (code == XARGS_EXEC_SYSTEM)
        {
                log_error("xargs: cannot fork or execute\n", 0);
                xargs_answer = 125;
                xargs_done = true;
                return false;
        }

        if (code == XARGS_EXEC_SIGNAL)
        {
                string_format(log_error, "xargs: %s: terminated by signal %b\n",
                              command, xargs_signal);
                xargs_answer = 125;
                xargs_done = true;
                return false;
        }

        if (!code)
                return true;

        if (code < 0)
        {
                string_format(log_error, "xargs: failed to run command '%s': %s\n",
                              command, file_reason(code));
                xargs_answer = code == -ERROR_ACCESS ? 126 : 127;
                xargs_done = true;
                return false;
        }

        if (code == 255)
        {
                string_format(log_error, "xargs: %s: exited with status 255; aborting\n",
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
                positive length = file_replace_literal(word, xargs_replace, mark,
                                                        item, item_length, null);

                if (!length || length > positive_max - xargs_used ||
                    xargs_word_count + 2 > xargs_word_room)
                        return false;

                p8 address_to made = (p8 address_to)text_arena_take(length);

                if (!made)
                        return false;

                file_replace_literal(word, xargs_replace, mark, item, item_length, made);
                xargs_words[xargs_word_count++] = made;
                xargs_used += length;
        }

        return true;
}

static fn xargs_item_done()
{
        if (xargs_item_broken)
        {
                log_error("xargs: argument line too long\n", 0);
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
                        log_error("xargs: argument list too long\n", 0);
                        xargs_answer = 1;
                        xargs_done = true;
                        return;
                }

                xargs_run();
                xargs_reset();
                return;
        }

        if (xargs_prefix_bytes + xargs_item_length + 1 > xargs_most_bytes)
        {
                if (xargs_word_count > xargs_prefix_words)
                {
                        xargs_run();
                        xargs_reset();
                }

                log_error("xargs: argument line too long\n", 0);
                xargs_answer = 1;
                xargs_done = true;
                return;
        }

        if (xargs_word_count > xargs_prefix_words &&
            (xargs_item_length == positive_max ||
             xargs_used > positive_max - xargs_item_length - 1 ||
             xargs_used + xargs_item_length + 1 > xargs_most_bytes))
        {
                if (xargs_exit_too_long)
                {
                        log_error("xargs: argument list too long\n", 0);
                        xargs_answer = 1;
                        xargs_done = true;
                        return;
                }

                xargs_run();

                if (xargs_done)
                        return;

                xargs_reset();
        }

        if (!xargs_add(xargs_item, xargs_item_length))
        {
                log_error("xargs: argument list too long\n", 0);
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

static bool xargs_count_value(string_address value, p8 letter, positive address_to out)
{
        positive taken = 0;
        positive made = string_digits(value, address_of taken);
        p8 named[2] = {letter, end};

        if (!taken || string_get(value + taken))
        {
                string_format(log_error, "xargs: invalid number \"%s\" for -%s option\n",
                              value, named);
                return false;
        }

        if (!made && letter != 'P' && letter != 's')
        {
                string_format(log_error,
                              "xargs: value 0 for -%s option should be >= 1\n", named);
                return false;
        }

        address_to out = made;
        return true;
}

/*
        -d says the one byte that ends an item, written plainly or as an
        escape. Nothing else about an item is special then: no quotes, no
        backslashes, and a newline is a byte like any other.
*/
static bool xargs_delimiter_read(string_address text, p8 address_to into)
{
        positive length = string_length(text);

        if (length == 1)
        {
                address_to into = string_get(text);
                return true;
        }

        if (length >= 2 && string_is(text, '\\'))
        {
                p8 letter = string_get(text + 1);
                p8 named = letter == 'n'   ? '\n'
                           : letter == 't' ? '\t'
                           : letter == 'r' ? '\r'
                           : letter == 'b' ? '\b'
                           : letter == 'f' ? '\f'
                           : letter == 'v' ? '\v'
                           : letter == 'a' ? 7
                           : letter == '\\' ? '\\'
                                             : 0;

                if (named && length == 2)
                {
                        address_to into = named;
                        return true;
                }

                if (length == 2 && letter == '0')
                {
                        address_to into = 0;
                        return true;
                }

                string_address at = text + 1;
                positive number;

                if (string_is(at, 'x') || string_is(at, 'X'))
                {
                        at++;
                        if (string_digits_checked(address_of at, 16, address_of number) &&
                            !string_get(at) && number < 256)
                        {
                                address_to into = (p8)number;
                                return true;
                        }
                }
                else if (string_digits_checked(address_of at, 8, address_of number) &&
                         !string_get(at) && number < 256)
                {
                        address_to into = (p8)number;
                        return true;
                }
        }

        string_format(log_error,
                      "xargs: Invalid input delimiter specification %s: the delimiter must "
                      "be either a single character or an escape sequence starting with \\.\n",
                      text);
        return false;
}

static const file_long xargs_longs[] = {
    {(string_address) "arg-file", 'a'},
    {(string_address) "delimiter", 'd'},
    {(string_address) "eof", 'E'},
    {(string_address) "exit", 'x'},
    {(string_address) "interactive", 'p'},
    {(string_address) "max-args", 'n'},
    {(string_address) "max-chars", 's'},
    {(string_address) "max-lines", 'l'},
    {(string_address) "max-procs", 'P'},
    {(string_address) "no-run-if-empty", 'r'},
    {(string_address) "null", '0'},
    {(string_address) "process-slot-var", 'V'},
    {(string_address) "replace", 'I'},
    {(string_address) "verbose", 't'},
    {null, 0},
};

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
            .allowed = (string_address) "0aEdILilnPprstx",
            .valued = (string_address) "aEdILnPsV",
            .optional = (string_address) "il",
            .longs = xargs_longs,
        };

        if (!file_take(address_of taking))
                return 1;

        positive index = taking.first;

        xargs_null = (taking.flags & FILE_FLAG('0')) != 0;
        xargs_ask = (taking.flags & FILE_FLAG('p')) != 0;
        xargs_terminal = -2;
        xargs_trace = (taking.flags & FILE_FLAG('t')) != 0;
        xargs_needs_input = (taking.flags & FILE_FLAG('r')) != 0;
        xargs_ending = file_option_value(address_of taking, 'E');
        xargs_replace = file_option_value(address_of taking, 'I');
        xargs_slot_name = file_option_value(address_of taking, 'V');
        xargs_exit_too_long = (taking.flags & FILE_FLAG('x')) != 0;
        xargs_delimited = false;
        xargs_delimiter = 0;
        xargs_said_nul = false;
        xargs_signal = 0;
        xargs_most = 0;
        xargs_lines = 0;
        xargs_most_bytes = XARGS_BATCH_BYTES;
        xargs_input = 0;

        if ((taking.flags & FILE_FLAG('n')) &&
            !xargs_count_value(file_option_value(address_of taking, 'n'), 'n',
                               address_of xargs_most))
                return 1;

        if ((taking.flags & FILE_FLAG('L')) &&
            !xargs_count_value(file_option_value(address_of taking, 'L'), 'L',
                               address_of xargs_lines))
                return 1;

        if (taking.flags & FILE_FLAG('l'))
        {
                string_address written = file_option_value(address_of taking, 'l');

                xargs_lines = 1;

                if (written && !xargs_count_value(written, 'L', address_of xargs_lines))
                        return 1;
        }

        if (taking.flags & FILE_FLAG('P'))
        {
                positive parallel;

                if (!xargs_count_value(file_option_value(address_of taking, 'P'), 'P',
                                       address_of parallel))
                        return 1;
        }

        if (taking.flags & FILE_FLAG('s'))
        {
                if (!xargs_count_value(file_option_value(address_of taking, 's'), 's',
                                       address_of xargs_most_bytes))
                        return 1;

                if (xargs_most_bytes > XARGS_BATCH_BYTES)
                        xargs_most_bytes = XARGS_BATCH_BYTES;
        }

        if (taking.flags & FILE_FLAG('d'))
        {
                if (!xargs_delimiter_read(file_option_value(address_of taking, 'd'),
                                          address_of xargs_delimiter))
                        return 1;

                xargs_delimited = true;
                xargs_null = xargs_delimiter == 0;
        }

        if (!xargs_replace && (taking.flags & FILE_FLAG('i')))
        {
                xargs_replace = file_option_value(address_of taking, 'i');

                if (!xargs_replace)
                        xargs_replace = "{}";
        }

        string_address from = file_option_value(address_of taking, 'a');

        if (from)
        {
                xargs_input = system_open_at(AT_FDCWD, from, FILE_READ);

                if (xargs_input < 0)
                {
                        string_format(log_error, "xargs: Cannot open input file '%s': %s\n",
                                      from, file_reason(xargs_input));
                        return 1;
                }
        }

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
                        return string_report(log_error, 1, "xargs: command too long\n");
        }

        xargs_prefix_bytes = xargs_used;
        xargs_prefix_words = xargs_word_count;

        if (!xargs_keep_template())
                return string_report(log_error, 1, "xargs: command too long\n");

        xargs_item = (p8 address_to)text_arena_take(XARGS_BATCH_BYTES + 1);
        xargs_buffer = (p8 address_to)text_arena_take(XARGS_READ_BYTES);

        if (!xargs_item || !xargs_buffer)
                return 1;

        xargs_item_room = XARGS_BATCH_BYTES + 1;
        xargs_mark = text_arena_used;

        for (;;)
        {
                bipolar got = system_read_retry((positive)xargs_input, xargs_buffer,
                                                 XARGS_READ_BYTES);

                if (got < 0)
                {
                        log_error("xargs: read error\n", 0);
                        xargs_answer = 1;
                        break;
                }

                if (!got)
                        break;

                for (positive at = 0;
                     at < (positive)got && !xargs_done && !xargs_ended; at++)
                {
                        p8 letter = xargs_buffer[at];

                        if (!letter && !xargs_null && !xargs_said_nul)
                        {
                                log_error("xargs: WARNING: a NUL character occurred in the "
                                          "input.  It cannot be passed through in the "
                                          "argument list.  Did you mean to use the --null "
                                          "option?\n", 0);
                                xargs_said_nul = true;
                        }

                        if (xargs_delimited && !xargs_null)
                        {
                                if (letter != xargs_delimiter)
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

        if (xargs_input > 0)
                system_close(xargs_input);

        if (quote)
                return string_report(log_error, 1, "xargs: unmatched quote\n");

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
