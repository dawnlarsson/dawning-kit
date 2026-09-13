/*
        Shared C floor above library.c's architecture floor.

        library.c stays assembly and declarations only.  This file owns the
        small structural mechanisms that are genuinely common to libc, the
        shell, utilities, networking and Canvas: layouts, indexed grammars
        and inline state transitions.  Keeping them here prevents subsystem
        files from growing private micro-libraries while still letting the
        compiler erase every unused or constant branch from the amalgamated
        build.
*/
#ifndef STANDARD_MODERN_C_LIBRARY_COMMON
#define STANDARD_MODERN_C_LIBRARY_COMMON

/* C varargs adapters shared by the standard compatibility families. */
#define var_list_entry(name, returned, parameters, last, call)               \
        static returned name parameters                                      \
        {                                                                    \
                var_args _variadic_list;                                     \
                var_list(_variadic_list, last);                              \
                returned _variadic_answer = (call);                          \
                var_list_end(_variadic_list);                                \
                return _variadic_answer;                                     \
        }

//      One digit for a base that is folded. Anything that is not a digit of
//      that base answers the base itself, which no digit of it can be.
static inline INLINE positive digit_known(p8 character, positive base)
{
        p32 narrow = (p32)character - 48;

        if (base <= 10)
                return narrow < (p32)base ? narrow : base;
        if (narrow <= 9)
                return narrow;

        narrow = (p32)(character | 32) - 97;
        return narrow <= (p32)base - 11 ? narrow + 10 : base;
}

/* Checked base-2..36 digit runs: overflow or no digits leaves both outputs
   untouched; success advances the cursor and writes the unsigned value. */
static inline bool string_digits_checked(string_address address_to text,
                                         positive base,
                                         positive address_to value)
{
        if (base < 2 || base > 36)
                return false;

        string_address at = address_to text;
        positive got = 0;
        bool any = false;

        while (1)
        {
                positive digit = digit_known(string_get(at), base);

                if (digit >= base)
                        break;

                positive scaled;

                if (__builtin_mul_overflow(got, base, address_of scaled) ||
                    __builtin_add_overflow(scaled, digit, address_of got))
                        return false;

                at++;
                any = true;
        }

        if (!any)
                return false;

        address_to text = at;
        address_to value = got;
        return true;
}

/* A whole numeric word with the checked scanner's range contract. Keep
   string_digits_exact's wrapping spelling test for callers that want it. */
static inline bool string_digits_checked_exact(string_address text,
                                                positive base,
                                                positive address_to value)
{
        positive got;
        if (!text || !string_digits_checked(address_of text, base, address_of got) ||
            string_get(text))
                return false;
        if (value)
                address_to value = got;
        return true;
}

/* GNU ld repairs an A53 ADRP/load pair split by a 4 KiB boundary with a whole
   veneer page.  Large functions which have actually hit that layout use one
   shared, architecture-scoped alignment spelling. */
#if ARM64
#define ARM64_ERRATUM_ALIGN __attribute__((aligned(64)))
#else
#define ARM64_ERRATUM_ALIGN
#endif

/* Typed, unaligned loads and same-width bit casts.  __builtin_memcpy is the
   compiler's one spelling that is both alias-safe and architecture-safe. */
#define memory_load_unaligned(type, source)                                  \
        ({ type _memory_loaded;                                              \
           __builtin_memcpy(address_of _memory_loaded, (source),             \
                            sizeof(_memory_loaded));                          \
           _memory_loaded; })

#define memory_cast(type, value)                                             \
        ({ __auto_type _memory_from = (value); type _memory_to;              \
           _Static_assert(sizeof(_memory_to) == sizeof(_memory_from),         \
                          "memory_cast changes width");                     \
           __builtin_memcpy(address_of _memory_to, address_of _memory_from,  \
                            sizeof(_memory_to));                              \
           _memory_to; })

/* Compile-time keys for the little-endian machine floor shared by x86-64,
   AArch64 and RV64.  Pair them with memory_load_unaligned for short grammar
   words; no general string comparator should survive for two or four bytes. */
#define byte_word_2(a, b) ((p16)(p8)(a) | ((p16)(p8)(b) << 8))
#define byte_word_4(a, b, c, d)                                              \
        ((p32)byte_word_2(a, b) | ((p32)byte_word_2(c, d) << 16))

#if RISCV64
/* Baseline RV64 has no unaligned word load: spelling the bytes lets GCC keep
   them independent instead of synthesizing a packed integer. */
#define memory_is_2(source, a, b)                                            \
        ({ __auto_type _memory_source = (source);                            \
           _memory_source[0] == (p8)(a) && _memory_source[1] == (p8)(b); })
#define memory_is_4(source, a, b, c, d)                                      \
        ({ __auto_type _memory_source = (source);                            \
           _memory_source[0] == (p8)(a) && _memory_source[1] == (p8)(b) &&  \
           _memory_source[2] == (p8)(c) && _memory_source[3] == (p8)(d); })
#else
#define memory_is_2(source, a, b)                                            \
        (memory_load_unaligned(p16, (source)) == byte_word_2(a, b))
#define memory_is_4(source, a, b, c, d)                                      \
        (memory_load_unaligned(p32, (source)) == byte_word_4(a, b, c, d))
#endif
#define memory_is_5(source, a, b, c, d, e)                                   \
        ({ __auto_type _memory_source = (source);                            \
           memory_is_4(_memory_source, a, b, c, d) &&                        \
               _memory_source[4] == (p8)(e); })

/* Compile-time array shape, never a separately maintained count. */
#define array_count(array) (sizeof(array) / sizeof((array)[0]))

/* Exact byte spans may contain NUL. Reuse the bounded architecture scan and
   return the bound on a miss, including an empty span at a null address. */
static inline INLINE PURE positive memory_span_without_byte(
    address_any block, p8 byte, positive size)
{
        p8 address_to found = memory_first_of(block, byte, size);
        return found ? (positive)(found - (p8 address_to)block) : size;
}

/* One Unicode scalar, with no terminator and no partial writes on failure.
   Fixed-width byte stores also work at unaligned addresses on the RV floor. */
static inline INLINE positive memory_utf8_encode(
    p8 address_to into, positive room, positive scalar)
{
        positive size = scalar < 0x80 ? 1 : scalar < 0x800 ? 2
                                    : scalar < 0x10000 ? 3 : 4;

        if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff) ||
            size > room)
                return 0;

        for (positive at = size - 1; at; at--)
        {
                into[at] = (p8)(0x80 | (scalar & 0x3f));
                scalar >>= 6;
        }
        into[0] = (p8)scalar | (size == 1 ? 0 : (p8)(0xffu << (8 - size)));
        return size;
}

typedef struct {
        p32 value, left, least;
} memory_utf8_state;

/* One incremental scalar: 0 needs another byte, 1 completed a scalar, and
   -1 rejected a sequence. The caller owns replacement/replay policy; a byte
   interrupting a sequence is consumed here, not silently processed twice. */
static inline INLINE b32 memory_utf8_feed(memory_utf8_state address_to state, p8 byte)
{
        if (state->left)
        {
                if ((byte & 0xc0) != 0x80)
                {
                        state->left = 0;
                        return -1;
                }
                state->value = (state->value << 6) | (byte & 0x3f);
                if (--state->left)
                        return 0;
                return state->value < state->least || state->value > 0x10ffff ||
                       (state->value >= 0xd800 && state->value <= 0xdfff) ? -1 : 1;
        }
        if (byte < 0x80)
        {
                state->value = byte;
                return 1;
        }
        if (byte < 0xc0 || byte >= 0xf8)
                return -1;
        state->left = byte < 0xe0 ? 1 : byte < 0xf0 ? 2 : 3;
        state->least = state->left == 1 ? 0x80 : state->left == 2 ? 0x800 : 0x10000;
        state->value = byte & (0x3f >> state->left);
        return 0;
}

/* The negative half of a signed range has one extra magnitude. Keeping that
   conversion unsigned until the minimum case is selected avoids overflowing
   the signed type in every parser that accepts the full native range. */
static inline INLINE CONST bipolar bipolar_from_magnitude(positive magnitude,
                                                          bool negative)
{
        return negative
            ? magnitude == (positive)bipolar_max + 1
                  ? bipolar_min
                  : -(bipolar)magnitude
            : (bipolar)magnitude;
}

/* One stable bottom-up merge machine for indexes, pointers and full records.
   The comparator and element type remain visible at every expansion, while
   exhausted runs fall through to the architecture's bulk copy floor. */
#define array_merge_sort(array, spare, count, order)                         \
        ({ __auto_type _merge_origin = (array);                             \
           __auto_type _merge_from = _merge_origin;                         \
           __auto_type _merge_into = (spare);                               \
           positive _merge_count = (count);                                 \
           for (positive _merge_width = 1; _merge_width < _merge_count;) {  \
                   for (positive _merge_base = 0; _merge_base < _merge_count;\
                        _merge_base += _merge_width * 2) {                    \
                           positive _merge_middle =                          \
                               min(_merge_base + _merge_width, _merge_count);\
                           positive _merge_stop =                            \
                               min(_merge_middle + _merge_width, _merge_count);\
                           positive _merge_left = _merge_base;               \
                           positive _merge_right = _merge_middle;            \
                           positive _merge_out = _merge_base;                \
                           while (_merge_left < _merge_middle &&             \
                                  _merge_right < _merge_stop)                \
                                   _merge_into[_merge_out++] =               \
                                       order(_merge_from[_merge_left],        \
                                             _merge_from[_merge_right]) <= 0 \
                                           ? _merge_from[_merge_left++]       \
                                           : _merge_from[_merge_right++];     \
                           positive _merge_tail =                            \
                               _merge_left < _merge_middle                   \
                                   ? _merge_middle - _merge_left             \
                                   : _merge_stop - _merge_right;             \
                           __auto_type _merge_rest =                         \
                               _merge_left < _merge_middle                   \
                                   ? _merge_from + _merge_left               \
                                   : _merge_from + _merge_right;             \
                           if (_merge_tail)                                  \
                                   memory_copy_apart(                        \
                                       _merge_into + _merge_out, _merge_rest,\
                                       _merge_tail * sizeof(_merge_from[0])); \
                   }                                                        \
                   __auto_type _merge_swap = _merge_from;                   \
                   _merge_from = _merge_into;                               \
                   _merge_into = _merge_swap;                               \
                   if (_merge_width > _merge_count / 2)                     \
                           break;                                           \
                   _merge_width *= 2;                                       \
           }                                                                \
           _merge_from; })

/* Linux raw errors occupy [-4095, -1]; zero succeeds. Allocations that reject
   a null address check it separately. Evaluate the raw result once. */
#define system_failed(result) ((positive)(result) >= (positive)-4095)

/* Cleanup paths neither need nor want close(2)'s errno translation. */
#define system_close(handle)                                                 \
        system_call_1(syscall(close), (positive)(handle))

/* Raw openat has two real call shapes: three arguments when the mode is
   ignored, four when creation consumes it.  These fronts keep the syscall
   number and ABI casts at the same floor without turning the three-argument
   form into an extra register move. */
#define system_open_at(directory, path, flags)                               \
        system_call_3(syscall(openat), (positive)(bipolar)(directory),       \
                      (positive)(path), (positive)(flags))

#define system_open_at_mode(directory, path, flags, mode)                    \
        system_call_4(syscall(openat), (positive)(bipolar)(directory),       \
                      (positive)(path), (positive)(flags), (positive)(mode))

#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)
/* Output files are created exclusively unless replacement was requested.
   Replacement still refuses a final-component symlink atomically: applying
   O_NOFOLLOW in the same openat call keeps O_TRUNC from reaching its target. */
static bipolar system_open_output_at(bipolar directory, string_address path,
                                     bool replace, positive mode)
{
        positive flags = FILE_WRITE | O_CLOEXEC |
                         (replace ? O_NOFOLLOW : FILE_EXCLUSIVE);

        return system_open_at_mode(directory, path, flags, mode);
}
#endif

/* One-shot I/O stays visibly distinct from the EINTR-retrying helpers. */
#define system_read_once(handle, into, length)                               \
        system_call_3(syscall(read), (positive)(handle), (positive)(into),   \
                      (positive)(length))

#define system_write_once(handle, data, length)                              \
        system_call_3(syscall(write), (positive)(handle), (positive)(data),  \
                      (positive)(length))

/* The platform leaves the kernel's error convention at this boundary:
   Linux already returns negative errno, while Darwin's assembly leaf turns
   its carry-plus-positive-errno result into the same shape before retrying.
   Keep the declaration above shared slurp helpers so C99 builds never rely
   on an implicit declaration. */
#if defined(LINUX) || defined(MACOS)
bipolar system_read_retry(positive handle, address_any into, positive length);
#endif

#define system_seek(handle, offset, origin)                                  \
        system_call_3(syscall(lseek), (positive)(handle),                    \
                      (positive)(offset), (positive)(origin))

#define system_file_status(handle, into)                                     \
        system_call_2(syscall(fstat), (positive)(handle), (positive)(into))

#define system_read_directory(handle, into, length)                          \
        system_call_3(syscall(getdents64), (positive)(handle),               \
                      (positive)(into), (positive)(length))

#define system_read_link_at(directory, path, into, length)                   \
        system_call_4(syscall(readlinkat), (positive)(bipolar)(directory),   \
                      (positive)(path), (positive)(into),                    \
                      (positive)(length))

#define system_stat_at(directory, path, flags, mask, into)                   \
        system_call_5(syscall(statx), (positive)(bipolar)(directory),        \
                      (positive)(path), (positive)(flags), (positive)(mask), \
                      (positive)(into))

#define system_status_at(directory, path, into, flags)                       \
        system_call_4(syscall(newfstatat),                                   \
                      (positive)(bipolar)(directory), (positive)(path),      \
                      (positive)(into), (positive)(flags))

#define system_pipe(pair, flags)                                             \
        system_call_2(syscall(pipe2), (positive)(pair), (positive)(flags))

static inline INLINE positive system_nonce_stir(positive value)
{
        value ^= value >> 30;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27;
        value *= 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
}

#if defined(LINUX) && !defined(KERNEL_MODE)
/* Fill the whole request from getrandom, retrying interruption and preserving
   the first real kernel error.  Callers choose blocking, nonblocking or
   early-boot policy through the Linux flags rather than open-coding loops. */
static bipolar system_random_fill(address_any into, positive length,
                                  positive flags)
{
        positive used = 0;

        while (used < length)
        {
                bipolar got = system_call_3(
                    syscall(getrandom), (positive)((p8 address_to)into + used),
                    length - used, flags);

                if (got == -4)
                        continue;
                if (got <= 0)
                        return got ? got : -5;
                used += (positive)got;
        }
        return 0;
}

static inline INLINE positive system_nonce()
{
        positive value;

        if (!system_random_fill(address_of value, sizeof value, 1) ||
            !system_random_fill(address_of value, sizeof value, 4))
                return value;

        value = get_cpu_time() ^
                ((positive)system_call(syscall(getpid)) << 32) ^
                ((positive)address_of value >> 3);
        return system_nonce_stir(value);
}
#endif

#define system_duplicate(from, to, flags)                                    \
        system_call_3(syscall(dup3), (positive)(from), (positive)(to),       \
                      (positive)(flags))

/* Put a descriptor at the number a child will inherit. dup3 deliberately
   rejects fd == fd; that case still has work to do when pipe2 or open happened
   to allocate the destination number with O_CLOEXEC set. */
#define SYSTEM_F_SETFD 2
#define system_descriptor_install(from, to)                                  \
        ({ bipolar _install_from = (bipolar)(from);                          \
           bipolar _install_to = (bipolar)(to);                              \
           _install_from == _install_to                                      \
               ? system_call_3(syscall(fcntl), (positive)_install_from,      \
                               SYSTEM_F_SETFD, 0)                            \
               : system_duplicate(_install_from, _install_to, 0); })

/* Install and relinquish an owned source descriptor as one operation. */
#define system_descriptor_move(from, to)                                     \
        ({ bipolar _move_from = (bipolar)(from);                             \
           bipolar _move_to = (bipolar)(to);                                \
           bipolar _moved = system_descriptor_install(_move_from, _move_to); \
           if (_moved >= 0 && _move_from != _move_to)                       \
                   system_close(_move_from);                                \
           _moved; })

#define system_control(handle, request, argument)                            \
        system_call_3(syscall(ioctl), (positive)(handle),                    \
                      (positive)(request), (positive)(argument))

#define system_execute(path, arguments, environment)                        \
        system_call_3(syscall(execve), (positive)(path),                     \
                      (positive)(arguments), (positive)(environment))

#define system_mount(source, target, type, flags, data)                      \
        system_call_5(syscall(mount), (positive)(source),                    \
                      (positive)(target), (positive)(type),                  \
                      (positive)(flags), (positive)(data))

#define system_change_directory(path)                                       \
        system_call_1(syscall(chdir), (positive)(path))

#define system_make_directory_at(directory, path, mode)                     \
        system_call_3(syscall(mkdirat), (positive)(bipolar)(directory),      \
                      (positive)(path), (positive)(mode))

#define system_remove_at(directory, path, flags)                             \
        system_call_3(syscall(unlinkat), (positive)(bipolar)(directory),     \
                      (positive)(path), (positive)(flags))

#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)
/* Some private workspaces must have an exact owner mode even under a caller's
   restrictive umask.  The shell is single-threaded while builtins run, and
   its signal handlers only record state, so the process mask can be restored
   immediately around the one creation syscall. */
static COLD bipolar system_make_directory_exact_at(
    bipolar directory, string_address path, positive mode)
{
        bipolar mask = system_call_1(syscall(umask), 0);

        if (mask < 0)
                return mask;

        bipolar made = system_make_directory_at(directory, path, mode);
        bipolar restored = system_call_1(syscall(umask), (positive)mask);

        return made < 0 ? made : restored < 0 ? restored : made;
}

/* Return an owned parent descriptor and a bounded final component. Each
   intermediate directory is opened relative to the descriptor already held,
   so renaming or replacing a pathname cannot redirect the next operation.
   Absolute paths start at /; relative paths start at directory. Contained
   walks refuse dot-dot; callers reject empty final components. Raw errors
   survive descriptor cleanup. */
static COLD bipolar system_open_parent_walk(
    bipolar directory, string_address path, bool create, positive mode,
    p8 address_to leaf, positive room, bool contained,
    bool (*accept)(bipolar))
{
        if (!path || !string_get(path) || !leaf || !room)
                return -22;

        positive flags = O_PATH | O_DIRECTORY | O_CLOEXEC |
                         (contained ? O_NOFOLLOW : 0);
        bipolar held = system_open_at(directory, path[0] == '/' ? "/" : ".", flags);
        if (held < 0)
                return held;
        if (accept && !accept(held))
        {
                system_close(held);
                return -13;
        }

        while (true)
        {
                while (string_is(path, '/'))
                        path++;
                positive length = 0;
                while (string_get(path + length) && !string_is(path + length, '/'))
                {
                        if (length + 1 >= room)
                        {
                                system_close(held);
                                return -36;
                        }
                        length++;
                }
                bool dotdot = length == 2 && path[0] == '.' && path[1] == '.';

                if (!length || (contained && dotdot))
                {
                        system_close(held);
                        return -22;
                }
                bool dot = length == 1 && path[0] == '.';
                if (!string_get(path + length))
                {
                        if (dot)
                        {
                                system_close(held);
                                return -22;
                        }
                        memory_copy_apart(leaf, path, length);
                        leaf[length] = end;
                        return held;
                }
                if (!dot)
                {
                        memory_copy_apart(leaf, path, length);
                        leaf[length] = end;
                        bipolar next = system_open_at(held, leaf, flags);
                        if (next == -2 && create)
                        {
                                bipolar made = system_make_directory_at(held, leaf, mode);
                                next = made < 0 && made != -17
                                           ? made : system_open_at(held, leaf, flags);
                        }
                        if (next >= 0 && accept && !accept(next))
                        {
                                system_close(next);
                                next = -13;
                        }
                        system_close(held);
                        if (next < 0)
                                return next;
                        held = next;
                }
                path += length + 1;
        }
}

static COLD bipolar system_open_parent_nofollow(
    bipolar directory, string_address path, bool create, positive mode,
    p8 address_to leaf, positive room)
{
        return system_open_parent_walk(directory, path, create, mode, leaf,
                                       room, true, 0);
}

/* The caller supplies the ownership/mode policy while this shared walk keeps
   every accepted component pinned until its child has been opened. */
static COLD bipolar system_open_parent_nofollow_checked(
    bipolar directory, string_address path, bool create, positive mode,
    p8 address_to leaf, positive room, bool (*accept)(bipolar))
{
        return system_open_parent_walk(directory, path, create, mode, leaf,
                                       room, true, accept);
}

/* A command-line pathname may legitimately contain `..`; opening that
   component relative to the directory already held preserves its meaning
   without resolving any later operation through the original pathname. */
static COLD bipolar system_open_parent_pinned(
    bipolar directory, string_address path, p8 address_to leaf, positive room)
{
        return system_open_parent_walk(directory, path, false, 0, leaf, room,
                                       false, 0);
}

/* Make an unpredictable sibling name while preserving any directory prefix
   carried by path.  O_EXCL or RENAME_NOREPLACE must claim the result. */
static COLD bool system_temporary_name(
    string_address path, p8 address_to into, positive room,
    string_address marker, positive marker_length, positive value)
{
        string_address slash = string_last_of(path, '/');
        positive prefix = slash ? (positive)(slash - path) + 1 : 0;
        p8 number[24];

        if (!room || prefix > room || marker_length >= room - prefix)
        {
                if (room)
                        into[0] = end;
                return false;
        }

        memory_copy_apart(into, path, prefix);
        memory_copy_apart(into + prefix, marker, marker_length);
        positive length = positive_into_string(number, value);

        if (length >= room - prefix - marker_length)
        {
                into[0] = end;
                return false;
        }

        memory_copy_end(into + prefix + marker_length, number, length);
        return true;
}

#if defined(LINUX)
/* The statx fields needed to compare an open descriptor with a directory
   entry.  The surrounding bytes keep the kernel's fixed 256-byte ABI. */
typedef struct
{
        p8 before_mode[28];
        p16 mode;
        p16 spare;
        p64 inode;
        p8 before_device[96];
        p32 device_major;
        p32 device_minor;
        p8 remainder[112];
} system_path_identity;

_Static_assert(sizeof(system_path_identity) == 256,
               "statx writes 256 bytes");

static bipolar system_path_same_opened_at(
    bipolar handle, bipolar directory, string_address name)
{
        system_path_identity opened;
        system_path_identity named;
        bipolar looked = system_stat_at(
            handle, (string_address)"", 0x1000 | 0x800, 0x7ff,
            address_of opened);
        bipolar found = looked < 0 ? looked : system_stat_at(
            directory, name, 0x100 | 0x800, 0x7ff, address_of named);

        if (found < 0)
                return found;
        return opened.inode == named.inode &&
                       opened.device_major == named.device_major &&
                       opened.device_minor == named.device_minor &&
                       (opened.mode & 0170000) == (named.mode & 0170000)
                   ? 0 : -11;
}

/* Atomically detach a name, then prove it still names the open object.  On a
   mismatch the name is restored when possible and the unexpected object is
   never removed. */
static bipolar system_path_detach_opened_at(
    bipolar directory, string_address name, bipolar handle,
    p8 address_to temporary, positive room)
{
        bipolar moved = -17;
        positive nonce = system_nonce();

        for (positive attempt = 0; attempt < 128 && moved == -17; attempt++)
        {
                if (!system_temporary_name(
                        name, temporary, room,
                        (string_address)".moonwater-remove-", 18,
                        nonce + attempt))
                        return -22;
                moved = system_call_5(
                    syscall(renameat2), (positive)directory, (positive)name,
                    (positive)directory, (positive)temporary, 1);
        }
        if (moved < 0)
                return moved;

        bipolar same = system_path_same_opened_at(
            handle, directory, temporary);
        if (same < 0)
        {
                (void)system_call_5(
                    syscall(renameat2), (positive)directory,
                    (positive)temporary, (positive)directory,
                    (positive)name, 1);
                return same;
        }
        return 0;
}

static bipolar system_path_remove_opened_at(
    bipolar directory, string_address name, bipolar handle, positive flags)
{
        p8 temporary[256];
        bipolar detached = system_path_detach_opened_at(
            directory, name, handle, temporary, sizeof(temporary));
        if (detached < 0)
                return detached;

        bipolar removed = system_remove_at(directory, temporary, flags);
        if (removed < 0 &&
            system_path_same_opened_at(handle, directory, temporary) >= 0)
                /* A failed rmdir/unlink must not silently rename the object.
                   Restore only into the still-empty public name; a concurrent
                   claimant is preserved and the detached inode remains under
                   its private diagnostic name. */
                (void)system_call_5(
                    syscall(renameat2), (positive)directory,
                    (positive)temporary, (positive)directory,
                    (positive)name, 1);
        return removed;
}

#if !defined(KERNEL_MODE)
/* Link the inode behind an open descriptor.  Older kernels require a
   capability for AT_EMPTY_PATH; procfs exposes the same descriptor without
   weakening the identity binding. */
static bipolar system_path_link_opened_at(
    bipolar handle, bipolar directory, string_address name)
{
        bipolar linked = system_call_5(
            syscall(linkat), (positive)handle,
            (positive)(string_address)"", (positive)directory,
            (positive)name, 0x1000);
        if (linked != -2 && linked != -1 && linked != -95 &&
            linked != -38 && linked != -18 && linked != -22)
                return linked;

        p8 path[64];
        static const p8 prefix[] = "/proc/self/fd/";
        memory_copy_apart(path, prefix, sizeof(prefix) - 1);
        positive length = positive_into_string(
            path + sizeof(prefix) - 1, (positive)handle);
        path[sizeof(prefix) - 1 + length] = 0;
        return system_call_5(
            syscall(linkat), (positive)(bipolar)-100, (positive)path,
            (positive)directory, (positive)name, 0x400);
}

#define SYSTEM_PATH_ALIAS_LEAF ((string_address)"object")

/* Prepare a publisher-owned 0700 directory containing an exact hard link to
   object.  Renaming from its returned descriptor never trusts a chowned
   staging name.  On failure no destination name has been touched. */
static bipolar system_path_alias_opened_at(
    bipolar directory, string_address near, bipolar object,
    p8 address_to name, positive room)
{
        bipolar made = -17;
        positive nonce = system_nonce();

        for (positive attempt = 0; attempt < 128 && made == -17; attempt++)
        {
                if (!system_temporary_name(
                        near, name, room,
                        (string_address)".moonwater-publish-", 19,
                        nonce + attempt))
                        return -22;
                made = system_make_directory_at(directory, name, 0700);
        }
        if (made < 0)
                return made;

        bipolar handle = system_open_at(
            directory, name,
            FILE_READ | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        /* Without an fd there is no identity-safe cleanup: a parent writer
           could have exchanged the just-created name.  Retain the private
           0700 directory and report the open failure. */
        if (handle < 0)
                return handle;

        bipolar same = system_path_same_opened_at(handle, directory, name);
        bipolar linked = same < 0 ? same : system_path_link_opened_at(
            object, handle, SYSTEM_PATH_ALIAS_LEAF);
        if (linked < 0)
        {
                (void)system_path_remove_opened_at(
                    directory, name, handle, 0x200);
                system_close(handle);
                return linked;
        }
        return handle;
}
#endif
#endif

#define SYSTEM_PATH_LEAF_ROOM 256

typedef struct
{
        bipolar directory;
        bipolar handle;
        bool owns_directory;
        p8 leaf[SYSTEM_PATH_LEAF_ROOM];
} system_path_file;

static fn system_path_file_reset(system_path_file address_to file)
{
        file->directory = -1;
        file->handle = -1;
        file->owns_directory = false;
        file->leaf[0] = end;
}

static COLD bipolar system_path_file_open_in(
    system_path_file address_to file, bipolar directory, bool owns_directory,
    string_address leaf, bool output, bool replace, positive mode)
{
        positive length = string_length(leaf);

        system_path_file_reset(file);
        if (!length || length >= sizeof(file->leaf) ||
            (length == 1 && leaf[0] == '.') ||
            (length == 2 && leaf[0] == '.' && leaf[1] == '.'))
        {
                if (owns_directory)
                        system_close(directory);
                return -22;
        }

        memory_copy_end(file->leaf, leaf, length);
        file->directory = directory;
        file->owns_directory = owns_directory;
        file->handle = output
                           ? system_open_output_at(directory, file->leaf,
                                                   replace, mode)
                           : system_open_at(directory, file->leaf,
                                            FILE_READ | O_CLOEXEC);
        if (file->handle < 0)
        {
                bipolar failed = file->handle;

                if (owns_directory)
                        system_close(directory);
                system_path_file_reset(file);
                return failed;
        }

        return file->handle;
}

static COLD bipolar system_path_file_open(
    system_path_file address_to file, string_address path, bool output,
    bool replace, positive mode)
{
        p8 leaf[SYSTEM_PATH_LEAF_ROOM];
        bipolar directory = system_open_parent_pinned(AT_FDCWD, path, leaf,
                                                       sizeof(leaf));

        if (directory < 0)
        {
                system_path_file_reset(file);
                return directory;
        }
        return system_path_file_open_in(file, directory, true, leaf, output,
                                        replace, mode);
}

static COLD bipolar system_path_file_open_sibling(
    system_path_file address_to file, bipolar directory, string_address leaf,
    bool replace, positive mode)
{
        return system_path_file_open_in(file, directory, false, leaf, true,
                                        replace, mode);
}

static bipolar system_path_file_close_handle(system_path_file address_to file)
{
        bipolar closed = 0;

        if (file->handle >= 0)
        {
                closed = system_close(file->handle);
                file->handle = -1;
        }

        return closed;
}

static bipolar system_path_file_remove(system_path_file address_to file)
{
        if (file->directory < 0 || file->handle < 0 || !file->leaf[0])
                return -22;

#if defined(LINUX)
        return system_path_remove_opened_at(
            file->directory, file->leaf, file->handle, 0);
#else
        return system_remove_at(file->directory, file->leaf, 0);
#endif
}

static fn system_path_file_release(system_path_file address_to file)
{
        (void)system_path_file_close_handle(file);
        if (file->owns_directory && file->directory >= 0)
                system_close(file->directory);
        system_path_file_reset(file);
}

static bipolar system_path_file_finish(system_path_file address_to input,
                                       system_path_file address_to output,
                                       bool success, bool remove_input)
{
        bipolar output_closed = system_path_file_close_handle(output);
        bipolar input_removed = 0;

        if (success && output_closed >= 0 && remove_input)
                input_removed = system_path_file_remove(input);
        (void)system_path_file_close_handle(input);
        system_path_file_release(output);
        system_path_file_release(input);
        return output_closed < 0 ? output_closed : input_removed;
}
#endif

#define system_rename_at(from_directory, from, to_directory, to, flags)      \
        system_call_5(syscall(renameat2),                                    \
                      (positive)(bipolar)(from_directory), (positive)(from), \
                      (positive)(bipolar)(to_directory), (positive)(to),     \
                      (positive)(flags))

#define system_access_at(directory, path, mode)                              \
        system_call_3(syscall(faccessat), (positive)(bipolar)(directory),    \
                      (positive)(path), (positive)(mode))

#define system_change_mode_at(directory, path, mode)                         \
        system_call_3(syscall(fchmodat), (positive)(bipolar)(directory),     \
                      (positive)(path), (positive)(mode))

#define system_change_owner_at(directory, path, owner, group, flags)         \
        system_call_5(syscall(fchownat), (positive)(bipolar)(directory),     \
                      (positive)(path), (positive)(owner),                   \
                      (positive)(group), (positive)(flags))

#define system_link_at(from_directory, from, to_directory, to, flags)        \
        system_call_5(syscall(linkat),                                       \
                      (positive)(bipolar)(from_directory), (positive)(from), \
                      (positive)(bipolar)(to_directory), (positive)(to),     \
                      (positive)(flags))

#define system_symbolic_link_at(target, directory, path)                     \
        system_call_3(syscall(symlinkat), (positive)(target),                \
                      (positive)(bipolar)(directory), (positive)(path))

#define system_update_times_at(directory, path, times, flags)                \
        system_call_4(syscall(utimensat), (positive)(bipolar)(directory),    \
                      (positive)(path), (positive)(times), (positive)(flags))

#define system_truncate_handle(handle, length)                               \
        system_call_2(syscall(ftruncate), (positive)(handle),                \
                      (positive)(length))

#define system_signal_action(number, action, previous, set_bytes)            \
        system_call_4(syscall(rt_sigaction), (positive)(number),             \
                      (positive)(action), (positive)(previous),              \
                      (positive)(set_bytes))

/* Linux gives every signal disposition the same four-word record.  Keep that
   ABI shape at the syscall floor: callers choose the handler, flags and
   restorer without rebuilding the record in each subsystem. */
#if defined(LINUX) && !defined(KERNEL_MODE) && \
    !defined(STANDARD_NO_PLATFORM)
static inline INLINE bool system_signal_install(
    b32 number, positive disposition, positive flags, positive restorer,
    positive address_to previous)
{
        positive action[4] = {disposition, flags, restorer, 0};

        return system_signal_action(number, address_of action, previous, 8) >= 0;
}
#endif

#define system_signal_mask(how, set, previous, set_bytes)                    \
        system_call_4(syscall(rt_sigprocmask), (positive)(how),              \
                      (positive)(set), (positive)(previous),                 \
                      (positive)(set_bytes))

#define system_fork() system_call_2(syscall(clone), SIGCHLD, 0)

/* The common moving byte store.  Naming the three words once also names the
   only correct reserve/release argument order; subsystems keep semantic
   typedefs without rebuilding either operation around them. */
typedef struct
{
        p8 address_to bytes;
        positive room;
        positive used;
} byte_store;

/* A bounded byte sink borrows storage and never grows it. Empty spans do
   nothing, including for a null source. Return whether the complete span
   fitted; callers choose whether truncation is an error or display policy. */
static inline bool byte_store_append_span(byte_store address_to store,
                                           address_any data, positive length)
{
        if (store->used > store->room)
                return false;
        positive kept = min(length, store->room - store->used);
        if (kept)
                memory_copy_apart(store->bytes + store->used, data, kept);
        store->used += kept;
        return kept == length;
}

/* Atomic field variant: a short buffer retains its prior bytes and cursor. */
static inline bool byte_store_append_exact(byte_store address_to store,
                                            address_any data, positive length)
{
        if (store->used > store->room || length > store->room - store->used)
                return false;
        return byte_store_append_span(store, data, length);
}

/* Stable storage owns its mapping outside these mechanisms. Unlike a moving
   byte_store, every successful take preserves all earlier addresses. The
   owner chooses alignment and which marks may be rewound. */
typedef struct
{
        p8 address_to bytes;
        positive room;
        positive used;
} memory_arena;

static inline address_any memory_arena_take(memory_arena address_to arena,
                                             positive bytes, positive alignment)
{
        if (!alignment || (alignment & (alignment - 1)) ||
            bytes > positive_max - (alignment - 1))
                return null;
        bytes = (bytes + alignment - 1) & ~(alignment - 1);
        if (!arena->bytes || arena->used > arena->room ||
            bytes > arena->room - arena->used)
                return null;
        address_any at = arena->bytes + arena->used;
        arena->used += bytes;
        return at;
}

#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)
/* The allocator decides ownership and failure reporting. All stable arena
   vectors share checked growth and copy; their typed capacity check remains
   at each call site through array_arena_reserve. */
static COLD bool memory_vector_grow(
    address_any table, positive address_to room, positive used,
    positive wanted, positive unit, positive first,
    address_any (*take)(positive))
{
        if (wanted < used)
                return false;
        if (wanted <= address_to room)
                return true;
        positive larger = memory_growth(address_to room, wanted, first);
        if (!unit || !larger || larger > positive_max / unit)
                return false;
        address_any grown = take(larger * unit);
        if (!grown)
                return false;
        if (used)
                memory_copy_apart(grown,
                    address_to(address_any address_to)table, used * unit);
        address_to(address_any address_to)table = grown;
        address_to room = larger;
        return true;
}

/* Read into the newest arena allocation, beginning at mark with first bytes
   already reserved. Growth changes its capacity without moving its address;
   EOF keeps a NUL sentinel, failure rolls the entire allocation back. */
static p8 address_to memory_arena_read_tail(
    memory_arena address_to arena, positive handle, positive mark,
    positive first, positive alignment, positive address_to length,
    bool address_to read_failed)
{
        positive room = first, used = 0;
        p8 address_to bytes = arena->bytes + mark;
        while (true)
        {
                if (used == room)
                {
                        positive larger = room < positive_max
                            ? memory_growth(room, room + 1, first) : 0;
                        positive available = arena->room - mark;
                        if (larger > available)
                                larger = room < available ? available : 0;
                        if (!larger)
                                goto failed;
                        arena->used = mark;
                        bytes = memory_arena_take(arena, larger, alignment);
                        if (!bytes)
                                goto failed;
                        room = larger;
                }
                bipolar got = system_read_retry(handle, bytes + used, room - used);
                if (got < 0)
                {
                        if (read_failed)
                                address_to read_failed = true;
                        goto failed;
                }
                if (!got)
                        break;
                used += (positive)got;
        }
        bytes[used] = end;
        arena->used = mark + ((used + alignment) & ~(alignment - 1));
        address_to length = used;
        return bytes;
failed:
        arena->used = mark;
        return null;
}
#endif

/* Type-preserving fronts for the untyped allocation ABI.  The element width
   and all address casts live here rather than at every growing vector. */
#define array_store_reserve(array, room, used, wanted, step)                  \
        memory_reserve((address_any address_to)address_of (array),            \
                       address_of (room), (used), (wanted),                   \
                       sizeof((array)[0]), (step))

#define array_store_release(array, room, used)                               \
        memory_release((address_any address_to)address_of (array),            \
                       address_of (room), address_of (used),                  \
                       sizeof((array)[0]))

/* Allocator-backed stores have a different slow path from mmap-backed
   memory_reserve: realloc knows the old allocation's size, so the caller only
   carries its pointer and capacity. Keep the typed, overwhelmingly-hot check
   here and the allocation policy in allocator.c. */
#define memory_resize_reserve(held, room, wanted, first)                     \
        ({ __auto_type _held = (held); __auto_type _room = (room);           \
           positive _wanted = (wanted), _grown;                             \
           address_any _block = (address_any)address_to _held;              \
           bool _resize_ok = _wanted <= (positive)address_to _room;         \
           if (!_resize_ok &&                                               \
               (_block = memory_resize_growth(                              \
                    _block, (positive)address_to _room, _wanted, (first),    \
                    address_of _grown))) {                                  \
                   address_to _held = (__typeof__(address_to _held))_block; \
                   address_to _room = (__typeof__(address_to _room))_grown; \
                   _resize_ok = true;                                       \
           }                                                                \
           _resize_ok; })

/* Arena vectors cannot resize their last block.  Their hot path is only this
   typed capacity check; a subsystem supplies one cold grow/copy body for all
   its element widths. */
#define array_arena_reserve(array, room, used, wanted, first, grow)           \
        ({ positive _arena_used = (used);                                    \
           positive _arena_wanted = (wanted);                               \
           _arena_wanted >= _arena_used &&                                  \
               (_arena_wanted <= (room) ||                                  \
                (grow)((address_any address_to)address_of (array),            \
                       address_of (room), _arena_used, _arena_wanted,         \
                       sizeof((array)[0]), (first))); })

#define byte_store_reserve(store, wanted, step)                              \
        ({ __auto_type _byte_store = (store);                                \
           array_store_reserve(_byte_store->bytes, _byte_store->room,         \
                               _byte_store->used, (wanted), (step)); })

#define byte_store_release(store)                                            \
        ({ __auto_type _byte_store = (store);                                \
           array_store_release(_byte_store->bytes, _byte_store->room,         \
                               _byte_store->used); })

#ifndef KERNEL_MODE
/* A bounded input window, backed by a descriptor or a borrowed memory span.
   have is the end offset; at is the consumed prefix. Return available bytes
   (possibly short at EOF), or the raw read error. The caller owns framing. */
typedef struct
{
        bipolar fd;
        p8 address_to mem;
        positive mem_len, mem_at;
        p8 address_to buf;
        positive room, at, have;
        bool eof;
} byte_input;

static bipolar byte_input_need(byte_input address_to input, positive want)
{
        if (input->have - input->at >= want || input->eof)
                return (bipolar)(input->have - input->at);
        if (input->at)
        {
                input->have -= input->at;
                if (input->have)
                        memory_copy(input->buf, input->buf + input->at,
                                    input->have);
                input->at = 0;
        }
        while (input->have < want && !input->eof)
        {
                positive room = input->room - input->have;
                bipolar got;
                if (!room)
                        break;
                if (input->mem)
                {
                        positive take = min(room, input->mem_len - input->mem_at);
                        if (take)
                                memory_copy(input->buf + input->have,
                                            input->mem + input->mem_at, take);
                        input->mem_at += take;
                        got = (bipolar)take;
                        input->eof = input->mem_at == input->mem_len;
                }
                else
                        got = system_read_retry((positive)input->fd,
                                                input->buf + input->have, room);
                if (got < 0)
                        return got;
                input->have += (positive)got;
                input->eof |= !got;
        }
        return (bipolar)(input->have - input->at);
}

/* Read no more than maximum bytes and prove EOF with one bounded probe.  This
   is the shared shape for protocol records whose peer controls their size;
   -27 (EFBIG) distinguishes a complete maximum-sized value from overflow. */
static HOT bipolar file_store_read_limit(positive handle,
                                         byte_store address_to store,
                                         positive maximum)
{
        store->used = 0;
        if (maximum == positive_max)
                maximum--;

        while (store->used < maximum)
        {
                positive remaining = maximum - store->used;
                positive grow = remaining;

                if (grow > 4096)
                        grow = 4096;
                if (!byte_store_reserve(store, store->used + grow + 1, 4096))
                        return -12;

                positive take = store->room - store->used - 1;
                if (take > remaining)
                        take = remaining;

                bipolar got = system_read_retry(
                    handle, store->bytes + store->used, take);
                if (got < 0)
                        return got;
                if (!got)
                {
                        store->bytes[store->used] = end;
                        return 0;
                }
                store->used += (positive)got;
        }

        if (!byte_store_reserve(store, store->used + 1, 1))
                return -12;

        p8 extra;
        bipolar got = system_read_retry(handle, address_of extra, 1);
        if (got < 0)
                return got;
        store->bytes[store->used] = end;
        return got ? -27 : 0;
}

/* Read through EOF into reusable storage, leaving the descriptor open.
   Preserve raw read errors; -12 is ENOMEM at the Linux syscall boundary. */
static HOT bipolar file_store_read(positive handle, byte_store address_to store)
{
        return file_store_read_limit(handle, store, positive_max);
}

/* Procfs may return short reads before EOF; use the same loop as streams. */
static HOT bool file_store_slurp(string_address path,
                                 byte_store address_to store)
{
        bipolar handle = system_open_at(AT_FDCWD, path, FILE_READ | O_CLOEXEC);
        if (handle < 0)
                return false;
        bipolar result = file_store_read((positive)handle, store);
        system_close(handle);
        return result == 0;
}

/* One bounded proc/sys-style record: open, one EINTR-safe read, terminate,
   close. Use file_store_slurp when a short read is not the complete record. */
static HOT bipolar file_slurp_once_at(bipolar directory, string_address path,
                                      p8 address_to into,
                                      positive capacity)
{
        if (!capacity)
                return -1;

        bipolar handle = system_open_at(directory, path,
                                        FILE_READ | O_CLOEXEC);

        if (handle < 0)
                return handle;

        bipolar got = system_read_retry((positive)handle, into, capacity - 1);

        system_close(handle);
        if (got >= 0)
                into[got] = end;
        return got;
}
#endif // KERNEL_MODE

/* Bounded parsers keep their overflow bit instead of silently truncating.
   The array width is part of the expression and disappears at compile time. */
#define fixed_store_byte(bytes, used, failed, value)                         \
        do                                                                   \
        {                                                                    \
                __auto_type _fixed_bytes = address_of (bytes)[0];            \
                __auto_type _fixed_used = address_of (used);                 \
                __auto_type _fixed_failed = address_of (failed);             \
                p8 _fixed_value = (p8)(value);                               \
                if (*_fixed_used < sizeof(bytes) - 1)                        \
                        _fixed_bytes[(*_fixed_used)++] = _fixed_value;        \
                else                                                         \
                        *_fixed_failed = true;                               \
        } while (false)

/* Like string_table_find, with ASCII case folding and a null-name sentinel.
   The name pointer comes first; the remaining row shape belongs to callers. */
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)
static inline INLINE PURE positive string_table_find_ascii_case(
    string_address name, const address_any table, positive stride, positive count)
{
        for (positive i = 0; i < count; i++)
        {
                string_address entry = memory_load_unaligned(
                    string_address, (const p8 address_to)table + i * stride);
                if (!entry)
                        break;
                if (!string_compare_folded(name, entry))
                        return i;
        }
        return count;
}
#endif

/* Tables that turn a long name into one byte of grammar use this same row in
   both the shell and its utilities. */
typedef struct
{
        string_address name;
        p8 value;
} named_byte;

/* printf dialects and seq share the flag grammar. */
#define CONVERSION_FLAG_LEFT 1
#define CONVERSION_FLAG_PLUS 2
#define CONVERSION_FLAG_SPACE 4
#define CONVERSION_FLAG_ALTERNATE 8
#define CONVERSION_FLAG_ZERO 16

static const p8 conversion_flag_bytes['0' + 1] = {
    ['-'] = CONVERSION_FLAG_LEFT, ['+'] = CONVERSION_FLAG_PLUS,
    [' '] = CONVERSION_FLAG_SPACE, ['#'] = CONVERSION_FLAG_ALTERNATE,
    ['0'] = CONVERSION_FLAG_ZERO,
};

/* Lexical printf fields only: field[0] is width, field[1] is precision.
   Stars and overflow use the corresponding bit. Keep the wrapped unsigned
   value on overflow; each dialect decides its limits and resolves stars in
   field order. Length modifiers and the conversion remain at the cursor.

   The fields are thirty two bits and the record is sixteen bytes, which is
   what a pair of registers holds. At twenty four bits of value and thirty two
   bytes it did not, so the compiler built it on the stack: an eight byte
   store of the flags and then a sixteen byte read of the flags and the width
   together, which is a load no processor can forward from a narrower store.
   That one stall was 77% of snprintf's own time and 17.5 million interlocks
   a run; sixteen bytes leaves 6.9 million and takes 8.5% off a format-heavy
   workload.

   A width is an int everywhere it is finally used, so nothing downstream can
   accept more than this holds. What changes is which pathological spellings
   wrap: past four billion rather than past eighteen quintillion. Both are
   past what any consumer accepts, and every consumer now says so -- file.c
   and format.c already tested the overflow bit, and awk and the shell's
   printf were reading a wrapped width as a width and now do not. */
typedef struct
{
        p32 flags, field[2];
        p8 fields, stars, overflow;
} conversion_spec;

static inline INLINE conversion_spec conversion_spec_take_max(
    string_address address_to source, positive length)
{
        string_address at = *source;
        conversion_spec spec = {0};
        // A down-counter also accepts positive_max for terminated strings.
        while (length)
        {
                p8 byte = string_get(at);
                p8 flag = byte < array_count(conversion_flag_bytes)
                              ? conversion_flag_bytes[byte] : 0;
                if (!flag)
                        break;
                spec.flags |= flag;
                at++;
                length--;
        }
        for (p8 field = 0; field < 2; field++)
        {
                if (field)
                {
                        if (!length || string_get(at) != '.')
                                break;
                        at++;
                        length--;
                }
                spec.fields++;
                if (length && string_get(at) == '*')
                {
                        spec.stars |= 1u << field;
                        at++;
                        length--;
                        continue;
                }
                while (length)
                {
                        p32 digit = (p32)string_get(at) - '0';
                        if (digit > 9)
                                break;
                        p32 scaled;
                        bool overflow = __builtin_mul_overflow(spec.field[field],
                            (p32)10, &scaled);
                        overflow |= __builtin_add_overflow(scaled, (p32)digit,
                            &spec.field[field]);
                        spec.overflow |= (p8)overflow << field;
                        at++;
                        length--;
                }
        }
        *source = at;
        return spec;
}

/* Stream presentation keeps the legacy writer ABI while explicitly allowing
   batched padding. The assembly writer_fill/field entry points retain their
   callback protocol. Empty bounded bodies never request a C-string scan. */
static fn writer_fill_bulk(writer output, positive count, p8 byte)
{
        if (!count)
                return;
        p8 block[256];
        positive chunk = min(count, (positive)sizeof(block));
        memory_fill(block, byte, chunk);
        do
        {
                output(block, chunk);
                count -= chunk;
                chunk = min(count, (positive)sizeof(block));
        } while (count);
}

static fn writer_field_bulk(writer output, address_any data, positive length,
                             positive width, p8 pad, bool left)
{
        positive padding = width > length ? width - length : 0;
        if (!left)
                writer_fill_bulk(output, padding, pad);
        if (length)
                output(data, length);
        if (left)
                writer_fill_bulk(output, padding, pad);
}

static inline fn string_to_field_bulk(writer output, string_address text,
                                       positive width, p8 pad, bool left)
{
        writer_field_bulk(output, text, string_length(text), width, pad, left);
}

/* Exact fixed-point fields: the unsigned magnitude is scaled by 10^scale,
   scale <= 18, independently of the requested display precision. Preparation
   rounds ties to even once and retains a compact body; arbitrarily wide
   padding and trailing zeroes need no width-sized scratch allocation. */
typedef struct
{
        p8 bytes[40];
        positive length, zeroes, padding, sign;
        bool left, zero;
} fixed_decimal;

static inline positive positive_power_ten(positive power)
{
        positive value = 1;
        while (power--) value *= 10;
        return value;
}

static inline fixed_decimal fixed_decimal_prepare(
    positive magnitude, positive scale, bool minus, positive width,
    positive precision, positive flags)
{
        fixed_decimal field = {.left = (flags & CONVERSION_FLAG_LEFT) != 0,
                                .zero = (flags & CONVERSION_FLAG_ZERO) != 0};
        if (scale > precision)
        {
                positive divisor = positive_power_ten(scale - precision);
                positive remainder = magnitude % divisor;
                magnitude /= divisor;
                magnitude += remainder > divisor / 2 ||
                    (remainder == divisor / 2 && (magnitude & 1));
                scale = precision;
        }
        positive divisor = positive_power_ten(scale);
        p8 sign = minus ? '-' : flags & CONVERSION_FLAG_PLUS ? '+'
                                 : flags & CONVERSION_FLAG_SPACE ? ' ' : 0;
        if (sign) field.bytes[field.length++] = sign;
        field.sign = field.length;
        field.length += positive_into(field.bytes + field.length,
                                       magnitude / divisor);
        if (precision || (flags & CONVERSION_FLAG_ALTERNATE))
        {
                field.bytes[field.length++] = '.';
                if (scale)
                        field.length += positive_into_padded(
                            field.bytes + field.length, magnitude % divisor,
                            scale, '0');
                field.zeroes = precision - scale;
        }
        positive length = field.length + field.zeroes;
        field.padding = width > length ? width - length : 0;
        return field;
}

/* No terminator and no partial write on a short buffer. Width/precision must
   leave the complete field length representable in positive. */
static inline positive fixed_decimal_into(p8 address_to into, positive room,
                                          fixed_decimal address_to field)
{
        positive length = field->length + field->zeroes + field->padding;
        if (length > room) return 0;
        positive at = 0;
        if (!field->left)
        {
                if (field->zero && field->sign)
                        into[at++] = field->bytes[0];
                memory_fill(into + at, field->zero ? '0' : ' ', field->padding);
                at += field->padding;
        }
        positive skip = !field->left && field->zero ? field->sign : 0;
        memory_copy_apart(into + at, field->bytes + skip, field->length - skip);
        at += field->length - skip;
        memory_fill(into + at, '0', field->zeroes);
        if (field->left)
                memory_fill(into + at + field->zeroes, ' ', field->padding);
        return length;
}

static inline fn fixed_decimal_write(writer write,
                                      fixed_decimal address_to field)
{
        if (!field->left)
        {
                if (field->zero && field->sign) write(field->bytes, 1);
                writer_fill_bulk(write, field->padding, field->zero ? '0' : ' ');
        }
        positive skip = !field->left && field->zero ? field->sign : 0;
        write(field->bytes + skip, field->length - skip);
        writer_fill_bulk(write, field->zeroes, '0');
        if (field->left) writer_fill_bulk(write, field->padding, ' ');
}

/* printf and scanf assign different meanings to `l`, but recognize the same
   h, hh, l, ll, q, z, t, j and L byte state machine. */
#define CONVERSION_LENGTH_INT 0
#define CONVERSION_LENGTH_CHAR 1
#define CONVERSION_LENGTH_SHORT 2
#define CONVERSION_LENGTH_LONG 3
#define CONVERSION_LENGTH_LONG_LONG 4
#define CONVERSION_LENGTH_SIZE 5
#define CONVERSION_LENGTH_DIFFERENCE 6
#define CONVERSION_LENGTH_WIDEST 7
#define CONVERSION_LENGTH_WIDE_DECIMAL 8

static const p8 conversion_single_lengths[128] = {
    ['q'] = CONVERSION_LENGTH_LONG_LONG,
    ['z'] = CONVERSION_LENGTH_SIZE,
    ['t'] = CONVERSION_LENGTH_DIFFERENCE,
    ['j'] = CONVERSION_LENGTH_WIDEST,
    ['L'] = CONVERSION_LENGTH_WIDE_DECIMAL,
};

static inline INLINE positive conversion_length_take(
    string_address address_to source)
{
        string_address at = address_to source;
        p8 byte = string_get(at);
        positive length;

        if (byte == 'h' || byte == 'l')
        {
                at++;
                bool doubled = string_is(at, byte);

                at += doubled;
                length = byte == 'h'
                             ? (doubled ? CONVERSION_LENGTH_CHAR
                                        : CONVERSION_LENGTH_SHORT)
                             : (doubled ? CONVERSION_LENGTH_LONG_LONG
                                        : CONVERSION_LENGTH_LONG);
        }
        else
        {
                length = byte < 128 ? conversion_single_lengths[byte] : 0;
                at += length != 0;
        }

        address_to source = at;
        return length;
}

/* The seven byte escapes shared by shell quoting, printf, awk and tr.  Zero
   means "not one of them", so callers retain their own unknown-pair policy. */
static const p8 byte_simple_escapes[256] = {
    ['a'] = 7,  ['b'] = 8,  ['f'] = 12, ['n'] = '\n',
    ['r'] = '\r', ['t'] = '\t', ['v'] = 11,
};

#define byte_simple_escape(value) byte_simple_escapes[(p8)(value)]

enum {
        HEX_CONTROL = 1, HEX_TAB = 2, HEX_SPACE = 4,
        HEX_QUOTE = 8, HEX_SLASH = 16, HEX_HIGH = 32,
};

/* Long ordinary spans stay zero-copy; replacements cross the writer in bounded
   batches, not one callback per escaped byte. Policy 64 is JSON's spelling. */
static fn writer_escaped_bulk(writer output, address_any data, positive length,
                             p8 policy)
{
        p8 address_to bytes = data;
        while (length)
        {
                positive plain = memory_escape_index(bytes, length, policy);
                if (plain)
                        output(bytes, plain);
                bytes += plain;
                length -= plain;
                if (!length)
                        break;
                p8 escaped[256];
                positive2 chunk = memory_into_escaped(escaped, bytes, length,
                                                       sizeof(escaped), policy | 128);
                output(escaped, chunk.y);
                bytes += chunk.x;
                length -= chunk.x;
        }
}

static inline INLINE fn writer_hex_escaped(writer output, address_any data,
                                           positive length, p8 policy)
{
        // The only inline classification is a tiny literal fast path over the
        // ASM-owned table. All scanning and replacement machinery stays shared.
        p8 address_to bytes = data;
        if (length && length < 8)
        {
                p8 categories = escape_categories[bytes[0]];
                for (positive at = 1; at < length; at++)
                        categories |= escape_categories[bytes[at]];
                if (!(categories & policy))
                {
                        output(bytes, length);
                        return;
                }
                if (length == 1 && !(policy & 64))
                {
                        p8 escaped[4] = {'\\', 'x'};
                        memory_into_hex(escaped + 2, bytes, 1);
                        output(escaped, sizeof(escaped));
                        return;
                }
        }
        if (length && length <= 16)
        {
                p8 escaped[96];
                positive2 chunk = memory_into_escaped(escaped, bytes, length,
                                                       sizeof(escaped), policy);
                output(escaped, chunk.y);
                return;
        }
        writer_escaped_bulk(output, data, length, policy);
}

/* A pathname supplied by an archive or a directory entry may contain bytes
   that a terminal interprets as cursor movement, erased output, or a forged
   line.  Ordinary names retain their byte-for-byte output.  Once a control
   byte is present, backslashes are escaped as well so the visible \xNN form
   cannot be confused with literal input. */
static fn writer_terminal_name_span(writer output, string_address value,
                                    positive length)
{
        p8 unsafe = HEX_CONTROL | HEX_TAB | HEX_HIGH;

        if (memory_escape_index(value, length, unsafe) == length)
        {
                output(value, length);
                return;
        }

        writer_hex_escaped(output, value, length, unsafe | HEX_SLASH);
}

static fn writer_terminal_name(writer output, string_address value)
{
        writer_terminal_name_span(output, value, string_length(value));
}

/* The body of a pathname already delimited by single quotes in a diagnostic.
   Backslash and the delimiter are always escaped, while terminal controls and
   non-ASCII bytes use the same bounded streaming encoder as bare names. */
static fn writer_terminal_quoted_name_span(writer output,
                                           string_address value,
                                           positive length)
{
        p8 policy = HEX_CONTROL | HEX_TAB | HEX_HIGH | HEX_SLASH;

        while (length)
        {
                p8 address_to quote = memory_first_of(value, '\'', length);
                positive span = quote ? (positive)(quote - value) : length;

                writer_hex_escaped(output, value, span, policy);
                value += span;
                length -= span;
                if (!length)
                        break;
                output("\\'", 2);
                value++;
                length--;
        }
}

static fn writer_terminal_quoted_name(writer output, string_address value)
{
        writer_terminal_quoted_name_span(output, value, string_length(value));
}

typedef struct { p8 address_to bytes; positive length; } byte_span;

/* JSON keeps bounded byte fields intact; dialects choose short controls and key case. */
static fn writer_json_span(writer output, byte_span value, bool lower, bool short_escapes)
{
        if (!lower && !short_escapes)
        {
                if (value.length <= 20)
                {
                        p8 escaped[122]; escaped[0] = '"';
                        positive2 chunk = memory_into_escaped(escaped + 1, value.bytes, value.length,
                                                              sizeof(escaped) - 2, 64);
                        escaped[chunk.y + 1] = '"'; output(escaped, chunk.y + 2);
                }
                else
                {
                        output("\"", 1);
                        writer_hex_escaped(output, value.bytes, value.length, 64);
                        output("\"", 1);
                }
                return;
        }
        static const p8 short_escape[32] = {
            ['\b'] = 'b', ['\f'] = 'f', ['\n'] = 'n',
            ['\r'] = 'r', ['\t'] = 't',
        };
        output("\"", 1);
        positive at = 0;
        while (at < value.length)
        {
                positive plain = memory_escape_index(value.bytes + at,
                    lower ? min(value.length - at, 256) : value.length - at, 64);
                if (plain)
                {
                        if (lower)
                        {
                                p8 lowered[256];
                                plain = min(plain, sizeof(lowered));
                                memory_copy(lowered, value.bytes + at, plain);
                                memory_to_lower_ascii(lowered, plain);
                                output(lowered, plain);
                        }
                        else
                                output(value.bytes + at, plain);
                        at += plain;
                        continue;
                }
                p8 character = value.bytes[at++], escaped[6];
                positive length;
                if (short_escapes && character < 32 && short_escape[character])
                {
                        escaped[0] = '\\';
                        escaped[1] = short_escape[character];
                        length = 2;
                }
                else
                        length = memory_into_escaped(escaped, &character, 1,
                                                      sizeof(escaped), 64).y;
                output(escaped, length);
        }
        output("\"", 1);
}

static fn writer_json_string(writer output, string_address value)
{
        writer_json_span(output, (byte_span){value, string_length(value)}, false, false);
}

/* Borrowed byte fields keep their bounds through projection and presentation. */

enum {
        TABLE_RIGHT = 1, TABLE_LINES = 2, TABLE_WRAP = 4,
        TABLE_TRUNCATE = 8, TABLE_OVERFLOW = 16, TABLE_WIDTH_BYTES = 32,
        TABLE_CLIP_OUTPUT = 64,
};
enum { TABLE_STRING, TABLE_NUMBER, TABLE_BOOLEAN, TABLE_NULL_STRING, TABLE_NULL_NUMBER };
enum { TABLE_HEADING = positive_max, TABLE_NAME = positive_max - 1 };

typedef struct {
        byte_span text;
        positive minimum;
        p8 flags, escape, json;
} table_cell;

typedef struct table_view {
        writer output;
        address_any context;
        table_cell (*cell)(address_any context, positive row, positive column,
                           p8 address_to scratch);
        /* Composite fields can stream their pieces without a temporary string. */
        positive (*write)(writer output, address_any context, positive row, positive column);
        address_any order;
        positive order_size, count;
        string_address separator;
        bool pad_last, pad_empty, pairs, multipart, fixed_widths;
        positive pad_extra;
} table_view;

static inline INLINE positive table_index(const table_view address_to view, positive shown)
{
        if (!view->order) return shown;
        return view->order_size == 1 ? ((p8 address_to)view->order)[shown]
            : ((positive address_to)view->order)[shown];
}

static inline INLINE positive memory_hex_width(byte_span text, p8 policy)
{
        positive width = text.length, at = 0;
        while (policy && at < text.length)
        {
                at += memory_escape_index(text.bytes + at, text.length - at, policy);
                if (at < text.length) { width += 3; at++; }
        }
        return width;
}

/* The clipping bound is in emitted bytes, including a partial final escape. */
static inline INLINE fn writer_hex_span(writer output, byte_span text, p8 policy, positive limit)
{
        if (!policy)
        {
                if (text.length && limit) output(text.bytes, min(text.length, limit));
                return;
        }
        if (limit == positive_max)
                return writer_hex_escaped(output, text.bytes, text.length, policy);
        while (text.length && limit)
        {
                p8 escaped[256];
                positive2 made = memory_into_escaped(escaped, text.bytes, text.length,
                                                      sizeof(escaped), policy);
                positive kept = min(made.y, limit);
                if (kept) output(escaped, kept);
                text.bytes += made.x; text.length -= made.x; limit -= kept;
        }
}

static inline INLINE byte_span table_part(table_cell cell, positive width, positive part)
{
        byte_span text = cell.text;
        if (cell.flags & TABLE_TRUNCATE)
                text.length = part ? 0 : min(text.length, width);
        else if (cell.flags & TABLE_WRAP)
        {
                positive from = part * width;
                if (from < text.length)
                {
                        text.bytes += from;
                        text.length = min(width, text.length - from);
                }
                else text.length = 0;
        }
        else if (cell.flags & TABLE_LINES)
        {
                while (part && text.length)
                {
                        positive length = memory_span_without_byte(text.bytes, '\n', text.length);
                        positive step = min(length + 1, text.length);
                        text.bytes += step; text.length -= step; part--;
                }
                text.length = part ? 0 : memory_span_without_byte(text.bytes, '\n', text.length);
        }
        else if (part) text.length = 0;
        return text;
}

static inline INLINE positive table_cell_width(table_cell cell)
{
        if (cell.flags & TABLE_WIDTH_BYTES) return cell.text.length;
        positive width = 0;
        do {
                positive length = cell.flags & TABLE_LINES
                    ? memory_span_without_byte(cell.text.bytes, '\n', cell.text.length)
                    : cell.text.length;
                width = max(width, memory_hex_width((byte_span){cell.text.bytes, length}, cell.escape));
                if (length == cell.text.length) break;
                cell.text.bytes += length + 1; cell.text.length -= length + 1;
        } while (true);
        return width;
}

/* Widths are indexed by schema column, so repeated projections share a slot.
   all_columns measures hidden fields too, as column(1)'s extreme policy does. */
static inline INLINE fn table_measure(const table_view address_to view, positive rows,
    bool headings, bool declared, bool free_widths, positive all_columns,
    positive address_to widths, positive address_to second)
{
        positive fields = all_columns ? all_columns : view->count;
        for (positive shown = 0; shown < fields; shown++)
        {
                positive col = all_columns ? shown : table_index(view, shown);
                p8 scratch[96];
                table_cell heading = view->cell(view->context, TABLE_HEADING, col, scratch);
                widths[col] = max(declared ? heading.minimum : 0,
                                  headings ? table_cell_width(heading) : 0);
                if (second) second[col] = 0;
        }
        for (positive row = 0; row < rows; row++)
                for (positive shown = 0; shown < fields; shown++)
                {
                        positive col = all_columns ? shown : table_index(view, shown);
                        p8 scratch[96];
                        table_cell cell = view->cell(view->context, row, col, scratch);
                        positive length = table_cell_width(cell);
                        if (length > widths[col])
                        {
                                if (second) second[col] = widths[col];
                                widths[col] = length;
                        }
                        else if (second) second[col] = max(second[col], length);
                        if (!headings && !free_widths && length)
                                widths[col] = max(widths[col], cell.minimum);
                }
}

static inline INLINE fn table_row(const table_view address_to view, positive row,
                     positive address_to widths)
{
        positive parts = 1;
        for (positive shown = 0; widths && view->multipart && shown < view->count; shown++)
        {
                positive col = table_index(view, shown);
                p8 scratch[96];
                table_cell cell = view->cell(view->context, row, col, scratch);
                positive have = cell.flags & TABLE_LINES
                    ? memory_count(cell.text.bytes, cell.text.length, '\n') + 1
                    : (cell.flags & TABLE_WRAP) && widths[col] && cell.text.length
                        ? (cell.text.length + widths[col] - 1) / widths[col] : 1;
                parts = max(parts, have);
        }
        for (positive part = 0; part < parts; part++)
        {
                for (positive shown = 0; shown < view->count; shown++)
                {
                        positive col = table_index(view, shown);
                        bool last = shown + 1 == view->count;
                        p8 scratch[96], name_scratch[96];
                        table_cell cell = view->cell(view->context, row, col, scratch);
                        positive width = widths ? widths[col] : view->fixed_widths ? cell.minimum : 0;
                        byte_span text = widths && (parts > 1 ||
                            (cell.flags & (TABLE_WRAP | TABLE_TRUNCATE)))
                            ? table_part(cell, width, part) : cell.text;
                        positive length = widths || view->fixed_widths
                            ? memory_hex_width(text, cell.escape) : 0;
                        positive limit = (widths || view->fixed_widths) &&
                            (cell.flags & TABLE_CLIP_OUTPUT) && !last
                            ? width : positive_max;
                        length = min(length, limit);
                        positive pad = width > length ? width - length : 0;
                        if (last && !view->pad_last &&
                            (!(cell.flags & TABLE_RIGHT) || (!length && !view->pad_empty))) pad = 0;
                        if (last && view->pad_last && row != TABLE_HEADING)
                                pad = width + view->pad_extra > length ? width + view->pad_extra - length : 0;
                        if (view->pairs)
                        {
                                table_cell name = view->cell(view->context, TABLE_HEADING, col, name_scratch);
                                if (name.text.length) view->output(name.text.bytes, name.text.length);
                                view->output("=\"", 2);
                        }
                        if (cell.flags & TABLE_RIGHT) writer_fill_bulk(view->output, pad, ' ');
                        if (view->write) view->write(view->output, view->context, row, col);
                        else writer_hex_span(view->output, text, cell.escape, limit);
                        if (!(cell.flags & TABLE_RIGHT)) writer_fill_bulk(view->output, pad, ' ');
                        if (view->pairs) view->output("\"", 1);
                        if (!last)
                        {
                                if ((cell.flags & TABLE_OVERFLOW) && !(cell.flags & TABLE_RIGHT) && length > width)
                                {
                                        view->output("\n", 1);
                                        for (positive prior = 0; prior <= shown; prior++)
                                        {
                                                if (prior) view->output(view->separator, string_length(view->separator));
                                                writer_fill_bulk(view->output, widths[table_index(view, prior)], ' ');
                                        }
                                }
                                view->output(view->separator, string_length(view->separator));
                        }
                }
                view->output("\n", 1);
        }
}

static fn table_json_value(writer output, byte_span value, p8 kind, bool short_escapes)
{
        if ((kind == TABLE_NULL_STRING || kind == TABLE_NULL_NUMBER) && !value.length) output("null", 4);
        else if (kind == TABLE_BOOLEAN)
        {
                bool no = (value.length == 1 && value.bytes[0] == '0') ||
                    (value.length == 2 && memory_is_2(value.bytes, 'n', 'o'));
                output(no ? "false" : "true", no ? 5 : 4);
        }
        else if (kind == TABLE_NUMBER || kind == TABLE_NULL_NUMBER)
        {
                if (value.length) output(value.bytes, value.length);
        }
        else writer_json_span(output, value, false, short_escapes);
}

static inline INLINE fn table_json(const table_view address_to view, byte_span name,
                      positive rows, bool column_style)
{
        view->output("{\n   ", 5);
        writer_json_span(view->output, name, false, column_style);
        view->output(": [", 3);
        for (positive row = 0; row < rows; row++)
        {
                view->output(row ? ",{\n" : "\n      {\n", row ? 3 : 9);
                for (positive field = 0; field < view->count; field++)
                {
                        positive col = table_index(view, field);
                        p8 scratch[96];
                        if (field) view->output(",\n", 2);
                        view->output("         ", 9);
                        table_cell key = view->cell(view->context, TABLE_NAME, col, scratch);
                        writer_json_span(view->output, key.text, column_style, column_style);
                        view->output(": ", 2);
                        table_cell cell = view->cell(view->context, row, col, scratch);
                        table_json_value(view->output, cell.text, cell.json, column_style);
                }
                if (view->count || !column_style) view->output("\n", 1);
                view->output("      }", 7);
        }
        if (!rows) view->output("\n", 1);
        view->output("\n   ]\n}\n", 8);
}

/* Select byte indexes from a comma-separated list of named records.  Every
   schema keeps its name pointer first; stride lets tables retain the rest of
   their private shape.  The caller may seed a default prefix before an
   append-form list and chooses the few syntax policies that differ. */
#define NAME_LIST_CASE_SENSITIVE 1
#define NAME_LIST_UNIQUE 2
#define NAME_LIST_REJECT_TRAILING 4
typedef struct
{
        string_address at;
        positive length;
} name_list_error;

static COLD bool name_list_select(
    string_address text, const void address_to definitions, positive stride,
    positive definition_count, p8 address_to selected,
    positive address_to selected_count, positive maximum, p8 policy,
    name_list_error address_to error)
{
        if (error)
                address_to error = (name_list_error){text, 0};
        if (definition_count > 256 || stride < sizeof(string_address) ||
            address_to selected_count > maximum)
                return false;

        while (*text)
        {
                string_address comma = string_first_of_or_end(text, ',');
                positive length = (positive)(comma - text);
                positive found = definition_count;
                if (error)
                        address_to error = (name_list_error){text, length};

                for (positive i = 0; i < definition_count; i++)
                {
                        string_address name = memory_load_unaligned(
                            string_address,
                            (const p8 address_to)definitions + i * stride);

                        if (string_length(name) == length &&
                            !(policy & NAME_LIST_CASE_SENSITIVE
                                  ? memory_compare(name, text, length)
                                  : memory_compare_ascii_case(name, text,
                                                              length)))
                        {
                                found = i;
                                break;
                        }
                }

                if (found == definition_count)
                        return false;

                bool add = !(policy & NAME_LIST_UNIQUE) ||
                           !memory_first_of(selected, (p8)found,
                                            address_to selected_count);
                if (add)
                {
                        if (address_to selected_count == maximum)
                                return false;
                        selected[(address_to selected_count)++] = (p8)found;
                }

                if (!*comma)
                        break;
                text = comma + 1;
                if (!*text && (policy & NAME_LIST_REJECT_TRAILING))
                {
                        if (error)
                                address_to error = (name_list_error){text, 0};
                        return false;
                }
        }

        return address_to selected_count != 0;
}

enum { NAME_LIST_OK, NAME_LIST_EMPTY, NAME_LIST_UNKNOWN };

/* Column-list policy: retain repeats and trailing commas, reject an empty
   argument even with a seeded prefix, and report the first rejected span. */
static COLD b32 name_list_columns(
    string_address text, const void address_to definitions, positive stride,
    positive definition_count, p8 address_to selected,
    positive address_to selected_count, p8 address_to unknown, positive room)
{
        if (room)
                unknown[0] = 0;
        if (!string_get(text))
                return NAME_LIST_EMPTY;
        name_list_error error;
        if (name_list_select(text, definitions, stride, definition_count,
                             selected, selected_count, definition_count, 0,
                             address_of error))
                return NAME_LIST_OK;
        if (room)
        {
                positive kept = min(error.length, room - 1);
                memory_copy_apart(unknown, (address_any)error.at, kept);
                unknown[kept] = 0;
        }
        return NAME_LIST_UNKNOWN;
}

/* Regex, glob and tr share the [:name:] submachine.  A null limit means the
   surrounding string's terminator is the bound. */
static PURE inline INLINE string_address byte_class_end(string_address text,
                                                         string_address limit)
{
        if ((limit && limit - text < 2) || text[0] != '[' || text[1] != ':')
                return null;

        for (text += 2; (!limit || text + 1 < limit) && string_get(text); text++)
                if (text[0] == ':' && text[1] == ']')
                        return text + 2;

        return null;
}

static inline INLINE b32 byte_class_parse(string_address text, positive length,
                                          positive address_to used)
{
        if (length < 5)
                return -1;

        string_address past = byte_class_end(text, text + length);

        if (!past)
                return -1;

        b32 which = byte_class_index(text + 2, (positive)(past - text - 4));

        if (which >= 0)
                address_to used = (positive)(past - text);

        return which;
}

/* Environment vectors and env(1) use exactly the same NAME= key grammar. */
static inline INLINE bool environment_key_is(string_address entry,
                                              string_address name,
                                              positive length)
{
        string_address equals = string_first_of_or_end(entry, '=');

        return (positive)(equals - entry) == length && equals[0] == '=' &&
               !memory_compare(entry, name, length);
}

/* Linux errno text shared by libc and descriptor-oriented diagnostics.
   Numbering and wording match glibc; 41 and 58 are unassigned. A negative
   kernel result must be normalized by its caller. Unknown codes return null
   so each interface retains its own fallback and buffer policy. */
static inline CONST string_address system_error_message(bipolar code)
{
        static const char address_to const messages[] = {
                "Success",
                "Operation not permitted",
                "No such file or directory",
                "No such process",
                "Interrupted system call",
                "Input/output error",
                "No such device or address",
                "Argument list too long",
                "Exec format error",
                "Bad file descriptor",
                "No child processes",
                "Resource temporarily unavailable",
                "Cannot allocate memory",
                "Permission denied",
                "Bad address",
                "Block device required",
                "Device or resource busy",
                "File exists",
                "Invalid cross-device link",
                "No such device",
                "Not a directory",
                "Is a directory",
                "Invalid argument",
                "Too many open files in system",
                "Too many open files",
                "Inappropriate ioctl for device",
                "Text file busy",
                "File too large",
                "No space left on device",
                "Illegal seek",
                "Read-only file system",
                "Too many links",
                "Broken pipe",
                "Numerical argument out of domain",
                "Numerical result out of range",
                "Resource deadlock avoided",
                "File name too long",
                "No locks available",
                "Function not implemented",
                "Directory not empty",
                "Too many levels of symbolic links",
                null,
                "No message of desired type",
                "Identifier removed",
                "Channel number out of range",
                "Level 2 not synchronized",
                "Level 3 halted",
                "Level 3 reset",
                "Link number out of range",
                "Protocol driver not attached",
                "No CSI structure available",
                "Level 2 halted",
                "Invalid exchange",
                "Invalid request descriptor",
                "Exchange full",
                "No anode",
                "Invalid request code",
                "Invalid slot",
                null,
                "Bad font file format",
                "Device not a stream",
                "No data available",
                "Timer expired",
                "Out of streams resources",
                "Machine is not on the network",
                "Package not installed",
                "Object is remote",
                "Link has been severed",
                "Advertise error",
                "Srmount error",
                "Communication error on send",
                "Protocol error",
                "Multihop attempted",
                "RFS specific error",
                "Bad message",
                "Value too large for defined data type",
                "Name not unique on network",
                "File descriptor in bad state",
                "Remote address changed",
                "Can not access a needed shared library",
                "Accessing a corrupted shared library",
                ".lib section in a.out corrupted",
                "Attempting to link in too many shared libraries",
                "Cannot exec a shared library directly",
                "Invalid or incomplete multibyte or wide character",
                "Interrupted system call should be restarted",
                "Streams pipe error",
                "Too many users",
                "Socket operation on non-socket",
                "Destination address required",
                "Message too long",
                "Protocol wrong type for socket",
                "Protocol not available",
                "Protocol not supported",
                "Socket type not supported",
                "Operation not supported",
                "Protocol family not supported",
                "Address family not supported by protocol",
                "Address already in use",
                "Cannot assign requested address",
                "Network is down",
                "Network is unreachable",
                "Network dropped connection on reset",
                "Software caused connection abort",
                "Connection reset by peer",
                "No buffer space available",
                "Transport endpoint is already connected",
                "Transport endpoint is not connected",
                "Cannot send after transport endpoint shutdown",
                "Too many references: cannot splice",
                "Connection timed out",
                "Connection refused",
                "Host is down",
                "No route to host",
                "Operation already in progress",
                "Operation now in progress",
                "Stale file handle",
                "Structure needs cleaning",
                "Not a XENIX named type file",
                "No XENIX semaphores available",
                "Is a named type file",
                "Remote I/O error",
                "Disk quota exceeded",
                "No medium found",
                "Wrong medium type",
                "Operation canceled",
                "Required key not available",
                "Key has expired",
                "Key has been revoked",
                "Key was rejected by service",
                "Owner died",
                "State not recoverable",
                "Operation not possible due to RF-kill",
                "Memory page has hardware error",
        };
        return (positive)code < array_count(messages) ? (string_address)messages[code] : null;
}

/* argv token mechanics shared by utility policy adapters. Short options
   return one byte at a time; long names remain slices for each caller's
   exact/prefix lookup. The cursor borrows argv and never changes its bytes. */
typedef struct
{
        positive argc;
        string_address address_to argv;
        positive at;
        string_address letters;
        string_address word;
        bool operands_only;
        bool long_option;
        string_address attached;
        positive name_length;
} argument_cursor;

enum { ARGUMENT_END, ARGUMENT_OPERAND = 256, ARGUMENT_LONG,
       ARGUMENT_UNKNOWN = -1, ARGUMENT_MISSING = -2, ARGUMENT_UNEXPECTED = -3 };

static b32 argument_next(argument_cursor address_to cursor)
{
        for (;;)
        {
                if (cursor->letters && *cursor->letters)
                        return (p8)*cursor->letters++;
                cursor->letters = null;
                cursor->attached = null;
                cursor->long_option = false;
                if (cursor->at >= cursor->argc)
                        return ARGUMENT_END;
                cursor->word = cursor->argv[cursor->at++];
                if (cursor->operands_only || cursor->word[0] != '-' ||
                    !cursor->word[1])
                        return ARGUMENT_OPERAND;
                if (cursor->word[1] != '-')
                {
                        cursor->letters = cursor->word + 1;
                        continue;
                }
                if (!cursor->word[2])
                {
                        cursor->operands_only = true;
                        continue;
                }
                string_address name = cursor->word + 2;
                string_address mark = string_first_of(name, '=');
                cursor->long_option = true;
                cursor->name_length = mark ? (positive)(mark - name) : string_length(name);
                cursor->attached = mark ? mark + 1 : null;
                return ARGUMENT_LONG;
        }
}

/* Null means an absent optional value or a missing required one. An explicit
   empty value is a non-null pointer to NUL. Consume a short cluster only when
   its remaining bytes actually supply the argument. */
static string_address argument_value(argument_cursor address_to cursor,
                                      bool required)
{
        if (cursor->long_option)
        {
                if (cursor->attached)
                        return cursor->attached;
        }
        else if (cursor->letters && *cursor->letters)
        {
                string_address value = cursor->letters;
                cursor->letters = null;
                return value;
        }
        return required && cursor->at < cursor->argc ? cursor->argv[cursor->at++] : null;
}

/* One declaration owns spelling and argument policy. A zero letter groups
   short-only options in name; null name ends the table. */
enum {
        ARGUMENT_REQUIRED = 1, ARGUMENT_OPTIONAL = 2,
        ARGUMENT_LONG_ONLY = 4, ARGUMENT_LONG_OPTIONAL = 8,
        ARGUMENT_STICKY = 16,
};

/* A named row maps a long spelling to its option letter. With letter zero,
   name groups short-only letters with identical rules. A null name ends the
   table. Long-only aliases still expose their arity to legacy prescans. */
typedef struct
{
        string_address name;
        p8 letter;
        p8 mode;
        p16 selection;
} argument_option;

_Static_assert(sizeof(argument_option) == sizeof(named_byte),
               "option rules fit the former name/letter row");

/* A selection record contains byte fields. Each selected field receives the
   option letter; overlapping groups therefore retain independent answers. */
#define ARGUMENT_SELECT(type, field) (1u << __builtin_offsetof(type, field))

static fn argument_select(p8 address_to into, p16 selected, p8 letter)
{
        while (into && selected)
        {
                into[bits_first_set((b32)selected) - 1] = letter;
                selected &= selected - 1;
        }
}

static const argument_option address_to argument_option_short(
    const argument_option address_to options, p8 letter)
{
        for (; options && options->name; options++)
                if (options->letter == letter ||
                    (!options->letter && string_first_of(options->name, letter)))
                        return options;
        return null;
}

static p8 argument_option_mode(const argument_option address_to options,
                                p8 letter)
{
        const argument_option address_to option = argument_option_short(options, letter);
        return option ? option->mode : 0;
}

static const argument_option address_to argument_option_long(
    const argument_option address_to options, string_address name,
    positive length, bool prefix)
{
        const argument_option address_to candidate = null;
        if (!length)
                return null;
        for (; options && options->name; options++)
        {
                if (!options->letter || string_compare_max(options->name, name, length))
                        continue;
                if (!options->name[length])
                        return options;
                if (prefix)
                {
                        if (candidate)
                                return null;
                        candidate = options;
                }
        }
        return candidate;
}

typedef struct
{
        string_address value;
        p16 selection;
        p8 letter, mode;
        bool optional;
} argument_match;

/* Resolve and consume one option without diagnostics or application effects.
   On a missing value the match still identifies the option, so callers can
   preserve their pre-error state transitions. Explicit empty values are
   non-null; optional short and long spellings can have different arities. */
static b32 argument_option_take(argument_cursor address_to cursor,
    const argument_option address_to options, bool prefix,
    argument_match address_to match)
{
        b32 token = argument_next(cursor);
        *match = (argument_match){};
        if (token == ARGUMENT_END || token == ARGUMENT_OPERAND)
        {
                match->value = cursor->word;
                return token;
        }
        const argument_option address_to option = cursor->long_option
            ? argument_option_long(options, cursor->word + 2, cursor->name_length, prefix)
            : argument_option_short(options, (p8)token);
        match->letter = cursor->long_option ? (option ? option->letter : 0) : (p8)token;
        if (!option || (!cursor->long_option && (option->mode & ARGUMENT_LONG_ONLY)))
                return ARGUMENT_UNKNOWN;
        match->mode = option->mode;
        match->selection = option->selection;
        match->optional = (option->mode & ARGUMENT_OPTIONAL) ||
            (cursor->long_option && (option->mode & ARGUMENT_LONG_OPTIONAL));
        bool required = (option->mode & ARGUMENT_REQUIRED) && !match->optional;
        if (cursor->attached && !match->optional && !required)
                return ARGUMENT_UNEXPECTED;
        if (required || match->optional)
        {
                match->value = argument_value(cursor, required);
                if (required && !match->value)
                        return ARGUMENT_MISSING;
        }
        return match->letter;
}

/* Long-name policy stays with each option family; the ordered conflict scan
   only needs its decoded byte. This preserves the legacy scan's treatment
   of detached short values and its independently chosen diagnostic point. */
/*      Two options that cannot be given together, named in the order the
        command line wrote them: `-r -R` is "--read-only and --recursive" and
        `-R -r` the reverse.  The parsed flags say only that both were given,
        so the pair and its order are read back off the arguments. */
typedef struct
{
        p8 letter;
        string_address name;
} argument_exclusive_pair;

static COLD b32 argument_exclusive_refuse(
    writer diagnostic, string_address program, positive argc,
    string_address address_to argv,
    const argument_option address_to options, bool prefix,
    const argument_exclusive_pair address_to group,
    positive count)
{
        p8 seen[2] = {0, 0};
        positive have = 0;
        bool value_next = false;

        for (positive at = 1; at < argc && have < 2; at++)
        {
                string_address word = argv[at];

                if (value_next)
                {
                        value_next = false;
                        continue;
                }
                if (!word || word[0] != '-' || !word[1])
                        continue;

                if (word[1] == '-')
                {
                        if (!word[2])
                                break;

                        string_address equals =
                            string_first_of_or_end(word + 2, '=');
                        const argument_option address_to option = argument_option_long(
                            options, word + 2, (positive)(equals - (word + 2)), prefix);
                        p8 letter = option ? option->letter : 0;

                        for (positive i = 0; i < count && letter; i++)
                                if (letter == group[i].letter &&
                                    (!have || seen[0] != letter))
                                        seen[have++] = letter;
                        continue;
                }

                for (positive i = 1; word[i] && have < 2; i++)
                {
                        for (positive g = 0; g < count; g++)
                                if (word[i] == group[g].letter &&
                                    (!have || seen[0] != word[i]))
                                {
                                        seen[have++] = word[i];
                                        break;
                                }

                        if (argument_option_mode(options, word[i]) & ARGUMENT_REQUIRED)
                        {
                                value_next = !word[i + 1];
                                break;
                        }
                }
        }

        if (have < 2)
                return 0;

        string_address one = null;
        string_address two = null;

        for (positive i = 0; i < count; i++)
        {
                if (group[i].letter == seen[0])
                        one = group[i].name;
                if (group[i].letter == seen[1])
                        two = group[i].name;
        }

        return string_report(diagnostic, 1,
                      "%s: options --%s and --%s cannot be combined\n",
                      program, one, two);
}

#endif
