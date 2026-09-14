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

/* Typed, unaligned loads, stores and same-width bit casts.  __builtin_memcpy
   is the compiler's one spelling that is both alias-safe and
   architecture-safe. */
#define memory_load_unaligned(type, source)                                  \
        ({ type _memory_loaded;                                              \
           __builtin_memcpy(address_of _memory_loaded, (source),             \
                            sizeof(_memory_loaded));                          \
           _memory_loaded; })

#define memory_store_unaligned(type, destination, value)                     \
        ({ type _memory_stored = (value);                                    \
           __builtin_memcpy((destination), address_of _memory_stored,        \
                            sizeof(_memory_stored)); })

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

/* Word, prefix and suffix tests against a literal.  The length is the
   literal's own, so no site counts it by hand and none can count it wrong.
   memory_is_word and memory_has_suffix take an exact span; string_has_prefix
   stops at the text's NUL like string_compare_max does. */
#define memory_is_word(bytes, length, literal)                               \
        ((length) == sizeof(literal) - 1 &&                                  \
         !memory_compare((bytes), literal, sizeof(literal) - 1))
#define memory_has_suffix(bytes, length, literal)                            \
        ({ positive _suffix_have = (length);                                 \
           _suffix_have >= sizeof(literal) - 1 &&                            \
               !memory_compare((bytes) + _suffix_have - (sizeof(literal) - 1),\
                               literal, sizeof(literal) - 1); })
#define string_has_prefix(text, literal)                                     \
        (!string_compare_max((text), literal, sizeof(literal) - 1))

/* a - b, floored at zero: the saturating difference every width and room
   computation wants, spelled once. */
#define difference_or_zero(a, b)                                             \
        ({ __auto_type _difference_a = (a);                                  \
           __auto_type _difference_b = (b);                                  \
           _difference_a > _difference_b ? _difference_a - _difference_b : 0; })

/* The count of a NULL-ended pointer vector: argv, environ, a word list. */
static inline INLINE positive pointer_vector_count(
    string_address address_to vector)
{
        positive count = 0;

        while (vector[count])
                count++;

        return count;
}

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

/* Binary/decimal size dialects share the exponent alphabet while retaining
   their own accepted ranges, trailing-unit rules and overflow limits.  Some
   GNU grammars accept lower-case suffixes only through tera; every_lower
   selects the wider K..Q/k..q alphabet. */
static inline INLINE PURE p8 size_suffix_power(p8 suffix, bool every_lower)
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

/* Apply a decoded size exponent without letting an intermediate wrap.  The
   caller supplies its semantic ceiling (native size, signed file offset,
   protocol limit) and receives no partial result on failure. */
static inline INLINE bool size_scale_power_checked(
    p64 value, p64 base, p8 power, p64 maximum, p64 address_to scaled)
{
        if (!base)
                return false;
        while (power--)
        {
                if (value > maximum / base)
                        return false;
                value *= base;
        }
        address_to scaled = value;
        return true;
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

#if defined(LINUX) && !defined(KERNEL_MODE)
/* Linux exposes pollfd as an eight-byte kernel record on every supported
   architecture.  Keep its effective type, event spelling and ppoll's
   eight-byte kernel signal-set ABI in one floor shared by networking,
   terminals and process supervisors. */
typedef struct
{
        b32 descriptor;
        b16 events;
        b16 returned;
} system_poll_descriptor;

_Static_assert(sizeof(system_poll_descriptor) == 8,
               "Linux poll descriptors are eight bytes");

#define SYSTEM_POLL_READ  0x001
#define SYSTEM_POLL_WRITE 0x004
#define SYSTEM_POLL_ERROR 0x008
#define SYSTEM_POLL_HANGUP 0x010
#define SYSTEM_POLL_INVALID 0x020

static inline INLINE bipolar system_poll_wait(
    system_poll_descriptor address_to descriptors, positive count,
    timespec address_to limit, positive address_to signal_mask)
{
        return system_call_5(syscall(ppoll), (positive)descriptors, count,
                             (positive)limit, (positive)signal_mask, 8);
}

static inline INLINE bipolar descriptor_wait_readable(
    bipolar handle, timespec address_to limit,
    positive address_to signal_mask)
{
        system_poll_descriptor waited = {
            (b32)handle, SYSTEM_POLL_READ, 0};
        bipolar ready = system_poll_wait(
            address_of waited, 1, limit, signal_mask);

        /* ppoll reports an invalid descriptor as a ready row.  Callers which
           only probe readiness would otherwise treat a closed fd as readable
           without ever making the read that exposes EBADF. */
        return ready > 0 && (waited.returned & SYSTEM_POLL_INVALID)
                   ? -9 : ready;
}
#endif

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

#if !defined(KERNEL_MODE)
#define SYSTEM_TCGETS 0x5401
static inline bool stream_is_terminal(b32 descriptor)
{
        p64 attributes[16];

        return system_control(descriptor, SYSTEM_TCGETS, attributes) == 0;
}
#endif

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
    p8 address_to leaf, positive room, bool nofollow, bool contained,
    bool (*accept)(bipolar), bool final_directory)
{
        p8 directory_component[256];
        p8 address_to component = final_directory
                                      ? directory_component : leaf;
        positive component_room = final_directory
                                      ? sizeof(directory_component) : room;

        if (!path || !string_get(path) ||
            (!final_directory && (!leaf || !room)))
                return -22;

        positive flags = O_PATH | O_DIRECTORY | O_CLOEXEC |
                         (nofollow ? O_NOFOLLOW : 0);
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
                if (!string_get(path))
                {
                        if (final_directory)
                                return held;
                        system_close(held);
                        return -22;
                }
                positive length = string_span_without_set(path, (string_address)"/");
                if (length >= component_room)
                {
                        system_close(held);
                        return -36;
                }
                bool dotdot = length == 2 && path[0] == '.' && path[1] == '.';

                if (!length || (contained && dotdot))
                {
                        system_close(held);
                        return -22;
                }
                bool dot = length == 1 && path[0] == '.';
                if (!final_directory && !string_get(path + length))
                {
                        if (dot)
                        {
                                system_close(held);
                                return -22;
                        }
                        memory_copy_apart(component, path, length);
                        component[length] = end;
                        return held;
                }
                if (!dot)
                {
                        memory_copy_apart(component, path, length);
                        component[length] = end;
                        bipolar next = system_open_at(held, component, flags);
                        if (next == -2 && create)
                        {
                                bipolar made = system_make_directory_at(
                                    held, component, mode);
                                next = made < 0 && made != -17
                                           ? made
                                           : system_open_at(
                                                 held, component, flags);
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
                path += length;
                if (string_is(path, '/'))
                        path++;
        }
}

static COLD bipolar system_open_parent_nofollow(
    bipolar directory, string_address path, bool create, positive mode,
    p8 address_to leaf, positive room)
{
        return system_open_parent_walk(directory, path, create, mode, leaf,
                                       room, true, true, 0, false);
}

/* The caller supplies the ownership/mode policy while this shared walk keeps
   every accepted component pinned until its child has been opened. */
static COLD bipolar system_open_parent_nofollow_checked(
    bipolar directory, string_address path, bool create, positive mode,
    p8 address_to leaf, positive room, bool (*accept)(bipolar))
{
        return system_open_parent_walk(directory, path, create, mode, leaf,
                                       room, true, true, accept, false);
}

/* A command-line pathname may legitimately contain `..`; opening that
   component relative to the directory already held preserves its meaning
   without resolving any later operation through the original pathname. */
static COLD bipolar system_open_parent_pinned(
    bipolar directory, string_address path, p8 address_to leaf, positive room)
{
        return system_open_parent_walk(directory, path, false, 0, leaf, room,
                                       false, false, 0, false);
}

/* Pin a command-line directory without allowing any component to be a
   symlink.  Dot-dot retains its ordinary command-line meaning, while each
   resolved directory descriptor prevents a later rename from redirecting
   the walk.  A final slash, '.', and '/' are valid directory spellings. */
static COLD bipolar system_open_directory_nofollow(
    bipolar directory, string_address path)
{
        return system_open_parent_walk(directory, path, false, 0, null, 0,
                                       true, false, 0, true);
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

#define SYSTEM_PATH_LEAF_ROOM 256

#if defined(LINUX)
/* The statx fields needed to compare an open descriptor with a directory
   entry.  The surrounding bytes keep the kernel's fixed 256-byte ABI. */
typedef struct
{
        p32 mask;
        p32 block_size;
        p64 attributes;
        p32 hard_links;
        p32 user;
        p32 group;
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

#define SYSTEM_PATH_STATX_TYPE 0x001
#define SYSTEM_PATH_STATX_MODE 0x002
#define SYSTEM_PATH_STATX_UID  0x008
#define SYSTEM_PATH_STATX_INO  0x100
#define SYSTEM_PATH_STATX_IDENTITY                                      \
        (SYSTEM_PATH_STATX_TYPE | SYSTEM_PATH_STATX_MODE |              \
         SYSTEM_PATH_STATX_INO)
#define SYSTEM_PATH_AT_SYMLINK_NOFOLLOW AT_SYMLINK_NOFOLLOW
#define SYSTEM_PATH_AT_NO_AUTOMOUNT     AT_NO_AUTOMOUNT
#define SYSTEM_PATH_AT_EMPTY_PATH       AT_EMPTY_PATH
#define SYSTEM_PATH_RENAME_NOREPLACE    1
#define SYSTEM_PATH_AT_REMOVEDIR        AT_REMOVEDIR

static bipolar system_path_identity_at(
    bipolar directory, string_address name, positive flags, positive required,
    system_path_identity address_to identity)
{
        memory_fill(identity, 0, sizeof(*identity));
        bipolar found = system_stat_at(directory, name, flags, 0x7ff,
                                       identity);

        if (found < 0)
                return found;
        return (identity->mask & required) == required ? 0 : -5;
}

static bipolar system_path_same_opened_at(
    bipolar handle, bipolar directory, string_address name)
{
        system_path_identity opened;
        system_path_identity named;
        bipolar looked = system_path_identity_at(
            handle, (string_address)"",
            SYSTEM_PATH_AT_EMPTY_PATH | SYSTEM_PATH_AT_NO_AUTOMOUNT,
            SYSTEM_PATH_STATX_IDENTITY, address_of opened);
        bipolar found = looked < 0 ? looked : system_path_identity_at(
            directory, name,
            SYSTEM_PATH_AT_SYMLINK_NOFOLLOW | SYSTEM_PATH_AT_NO_AUTOMOUNT,
            SYSTEM_PATH_STATX_IDENTITY, address_of named);

        if (found < 0)
                return found;
        return opened.inode == named.inode &&
                       opened.device_major == named.device_major &&
                       opened.device_minor == named.device_minor &&
                       (opened.mode & 0170000) == (named.mode & 0170000)
                   ? 0 : -11;
}

/* The fd is authoritative, but the public name may be removed only while a
   parent writer cannot exchange it after the identity check.  The current
   owner can trust a directory that is not world-writable: group-writable
   0775 is how a user-private group (and Lima's mapped uid/gid) creates
   directories under umask 002.  A sticky directory is safe when its owner
   is the current user or the host root that already has authority over
   this process. */
static bool system_path_parent_cleanup_safe(bipolar directory)
{
        system_path_identity parent;
        p32 user = (p32)system_call(syscall(geteuid));
        bipolar found = system_path_identity_at(
            directory, (string_address)"",
            SYSTEM_PATH_AT_EMPTY_PATH | SYSTEM_PATH_AT_NO_AUTOMOUNT,
            SYSTEM_PATH_STATX_IDENTITY | SYSTEM_PATH_STATX_UID,
            address_of parent);

        if (found < 0 || (parent.mode & 0170000) != 0040000)
                return false;
        if (parent.mode & 01000)
                return parent.user == user || parent.user == 0;
        return parent.user == user && !(parent.mode & 0002);
}

static bipolar system_path_private_directory_valid(
    bipolar handle, bipolar directory, string_address name)
{
        system_path_identity opened;
        bipolar same = system_path_same_opened_at(handle, directory, name);
        bipolar found = same < 0 ? same : system_path_identity_at(
            handle, (string_address)"",
            SYSTEM_PATH_AT_EMPTY_PATH | SYSTEM_PATH_AT_NO_AUTOMOUNT,
            SYSTEM_PATH_STATX_IDENTITY | SYSTEM_PATH_STATX_UID,
            address_of opened);

        if (found < 0)
                return found;
        return opened.user == (p32)system_call(syscall(geteuid)) &&
                       (opened.mode & 0177777) == 0040700
                   ? 0 : -13;
}

/* Remove an empty private directory only when both checks around its public
   name are meaningful.  In a non-sticky writable parent the name may be
   exchanged after any check, so retaining an empty 0700 directory is the
   only identity-safe cleanup. */
static fn system_path_private_directory_close(
    bipolar parent, string_address name, bipolar handle)
{
        if (handle < 0)
                return;

        if (system_path_parent_cleanup_safe(parent) &&
            system_path_same_opened_at(handle, parent, name) >= 0)
                (void)system_remove_at(parent, name,
                                       SYSTEM_PATH_AT_REMOVEDIR);
        system_close(handle);
}

/* Create, open and validate an owner-only directory before returning its fd.
   The fd remains authoritative if a parent writer later moves the outer
   name.  On a hostile parent, failed setup deliberately leaves harmless
   residue rather than unlinking a name whose identity can change. */
static COLD bipolar system_path_private_directory_open_at(
    bipolar directory, string_address near, string_address marker,
    positive marker_length, p8 address_to name, positive room)
{
        bipolar made = -17;
        positive nonce = system_nonce();

        if (!near || !string_get(near) || string_last_of(near, '/'))
                return -22;

        for (positive attempt = 0; attempt < 128 && made == -17; attempt++)
        {
                if (!system_temporary_name(
                        near, name, room, marker, marker_length,
                        nonce + attempt))
                        return -22;
                made = system_make_directory_exact_at(
                    directory, name, 0700);
        }
        if (made < 0)
                return made;

        bipolar handle = system_open_at(
            directory, name,
            FILE_READ | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (handle == -13)
                /* An unusual umask may remove owner-read from the requested
                   0700 mode.  O_PATH still pins the directory so the modern
                   descriptor-only chmod can make it exactly private. */
                handle = system_open_at(
                    directory, name,
                    O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (handle < 0)
                return handle;

        bipolar valid = system_path_same_opened_at(handle, directory, name);
        system_path_identity opened;
        if (valid >= 0)
                valid = system_path_identity_at(
                    handle, (string_address)"",
                    SYSTEM_PATH_AT_EMPTY_PATH | SYSTEM_PATH_AT_NO_AUTOMOUNT,
                    SYSTEM_PATH_STATX_IDENTITY | SYSTEM_PATH_STATX_UID,
                    address_of opened);
        if (valid >= 0 &&
            (opened.user != (p32)system_call(syscall(geteuid)) ||
             (opened.mode & 0170000) != 0040000))
                valid = -13;
        bool change_mode = valid >= 0 && (opened.mode & 07777) != 0700;
        if (change_mode)
                valid = system_call_2(syscall(fchmod), (positive)handle,
                                      0700);
        if (change_mode && valid == -9)
                valid = system_call_4(
                    syscall(fchmodat2), (positive)handle,
                    (positive)(string_address)"", 0700,
                    SYSTEM_PATH_AT_EMPTY_PATH);
        if (change_mode && valid < 0 &&
            system_path_parent_cleanup_safe(directory) &&
            system_path_same_opened_at(handle, directory, name) >= 0)
        {
                /* fchmodat2 arrived after the oldest supported kernels.
                   A sticky or caller-private parent makes the legacy named
                   fallback stable; verify the same inode again afterward. */
                valid = system_call_3(
                    syscall(fchmodat), (positive)directory,
                    (positive)name, 0700);
                if (valid >= 0)
                        valid = system_path_same_opened_at(
                            handle, directory, name);
        }
        if (valid >= 0)
                valid = system_path_private_directory_valid(
                    handle, directory, name);
        if (valid < 0)
        {
                system_path_private_directory_close(
                    directory, name, handle);
                return valid;
        }
        return handle;
}

#define SYSTEM_PATH_STAGE_LEAF ((string_address)"object")

/* A public entry is first moved under an fd-held, validated 0700 directory.
   All later remove or publish operations recheck the caller's open handle
   against that protected entry.  parent and opened remain caller-owned and
   must stay open until one of the consuming operations below returns. */
typedef struct
{
        bipolar parent;
        bipolar directory;
        bipolar opened;
        bool verified;
        p8 original[SYSTEM_PATH_LEAF_ROOM];
        p8 private_name[SYSTEM_PATH_LEAF_ROOM];
} system_path_stage;

static fn system_path_stage_reset(system_path_stage address_to stage)
{
        stage->parent = -1;
        stage->directory = -1;
        stage->opened = -1;
        stage->verified = false;
        stage->original[0] = end;
        stage->private_name[0] = end;
}

static fn system_path_stage_release(system_path_stage address_to stage)
{
        if (stage->directory >= 0)
                system_path_private_directory_close(
                    stage->parent, stage->private_name, stage->directory);
        system_path_stage_reset(stage);
}

static bipolar system_path_stage_return_candidate(
    system_path_stage address_to stage)
{
        return system_call_5(
            syscall(renameat2), (positive)stage->directory,
            (positive)SYSTEM_PATH_STAGE_LEAF, (positive)stage->parent,
            (positive)stage->original, SYSTEM_PATH_RENAME_NOREPLACE);
}

/* Allocate the fd-held 0700 side of a transaction before creating its
   object.  Callers that can create directly as `object` avoid even a brief
   public staging name; the compatibility wrapper below moves an already
   opened public candidate into the same transaction. */
static bipolar system_path_stage_begin_at(
    system_path_stage address_to stage, bipolar directory,
    string_address name)
{
        positive length = name ? string_length(name) : 0;

        system_path_stage_reset(stage);
        if (!length || length >= sizeof(stage->original) ||
            string_last_of(name, '/') ||
            (length == 1 && name[0] == '.') ||
            (length == 2 && name[0] == '.' && name[1] == '.'))
                return -22;

        /* Every completed transaction must be able to remove its outer
           directory by an identity-checked name.  Starting in a non-sticky
           writable parent would otherwise leave one private directory per
           success and permit namespace exhaustion. */
        if (!system_path_parent_cleanup_safe(directory))
                return -13;

        memory_copy_end(stage->original, name, length);
        stage->parent = directory;
        stage->opened = -1;
        stage->directory = system_path_private_directory_open_at(
            directory, name, (string_address)".moonwater-stage-",
            sizeof(".moonwater-stage-") - 1, stage->private_name,
            sizeof(stage->private_name));
        if (stage->directory < 0)
        {
                bipolar failed = stage->directory;
                system_path_stage_reset(stage);
                return failed;
        }
        return 0;
}

static bipolar system_path_stage_bind_opened(
    system_path_stage address_to stage, bipolar opened)
{
        if (opened < 0 || stage->directory < 0)
                return -22;

        bipolar same = system_path_same_opened_at(
            opened, stage->directory, SYSTEM_PATH_STAGE_LEAF);
        if (same < 0)
                return same;

        stage->opened = opened;
        stage->verified = true;
        return 0;
}

static bipolar system_path_stage_opened_at(
    system_path_stage address_to stage, bipolar directory,
    string_address name, bipolar opened)
{
        if (opened < 0)
                return -22;

        bipolar begun = system_path_stage_begin_at(
            stage, directory, name);
        if (begun < 0)
                return begun;

        bipolar moved = system_call_5(
            syscall(renameat2), (positive)directory, (positive)name,
            (positive)stage->directory, (positive)SYSTEM_PATH_STAGE_LEAF,
            SYSTEM_PATH_RENAME_NOREPLACE);
        if (moved < 0)
        {
                system_path_stage_release(stage);
                return moved;
        }

        bipolar same = system_path_stage_bind_opened(stage, opened);
        if (same < 0)
        {
                (void)system_path_stage_return_candidate(stage);
                system_path_stage_release(stage);
                return same;
        }

        return 0;
}

/* Publish the verified staged object with the caller's rename policy.  A
   failed publication retains the protected object; it never falls back to a
   pathname copy or an overwrite with different identity. */
static bipolar system_path_stage_publish_at(
    system_path_stage address_to stage, bipolar directory,
    string_address name, positive flags)
{
        bipolar same = flags & ~(positive)SYSTEM_PATH_RENAME_NOREPLACE ? -22 :
                       !stage->verified ? -22 : system_path_same_opened_at(
            stage->opened, stage->directory, SYSTEM_PATH_STAGE_LEAF);
        bipolar moved = same < 0 ? same : system_call_5(
            syscall(renameat2), (positive)stage->directory,
            (positive)SYSTEM_PATH_STAGE_LEAF, (positive)directory,
            (positive)name, flags);

        /* A failed rename, particularly EXDEV, leaves the fd-held source
           available for caller-controlled recovery. */
        if (moved >= 0)
                system_path_stage_release(stage);
        return moved;
}

static bipolar system_path_stage_remove(
    system_path_stage address_to stage, positive flags)
{
        bipolar same = !stage->verified ? -22 : system_path_same_opened_at(
            stage->opened, stage->directory, SYSTEM_PATH_STAGE_LEAF);
        bipolar removed = same < 0 ? same : system_remove_at(
            stage->directory, SYSTEM_PATH_STAGE_LEAF, flags);

        if (removed < 0 && same >= 0 &&
            system_path_same_opened_at(
                stage->opened, stage->directory,
                SYSTEM_PATH_STAGE_LEAF) >= 0)
                (void)system_path_stage_return_candidate(stage);
        system_path_stage_release(stage);
        return removed;
}

/* Discard only the object still bound to the stage descriptor.  Unlike a
   failed user-visible removal, an unpublished output must never be restored
   to its old public temporary name: metadata may already have handed it to a
   different uid.  A nonempty directory is retained behind its private 0700
   parent when it cannot be removed safely. */
static bipolar system_path_stage_discard(
    system_path_stage address_to stage, positive flags)
{
        bipolar same = !stage->verified ? -22 : system_path_same_opened_at(
            stage->opened, stage->directory, SYSTEM_PATH_STAGE_LEAF);
        bipolar removed = same < 0 ? same : system_remove_at(
            stage->directory, SYSTEM_PATH_STAGE_LEAF, flags);

        system_path_stage_release(stage);
        return removed;
}

/* A removal is judged by the parent that holds the name.  Given a path of
   more than one component that parent is the prefix resolved under
   `directory`, not `directory` itself: judging the latter refused root's
   cleanup of its own 0700 directory in sticky /tmp whenever the working
   directory belonged to someone else, and could accept a parent it never
   looked at.  The prefix is opened and the leaf removed through it, so both
   checks below see the directory that actually holds the name. */
static bipolar system_path_remove_opened_at(
    bipolar directory, string_address name, bipolar handle, positive flags)
{
        positive length = name ? string_length(name) : 0;

        while (length > 1 && name[length - 1] == '/')
                length--;
        p8 address_to slash = memory_last_of(name, '/', length);
        positive cut = slash ? (positive)(slash - name) + 1 : 0;

        if (cut && cut < length)
        {
                p8 path[4096];

                if (length >= sizeof(path))
                        return -36;

                memory_copy(path, name, length);
                path[length] = end;
                path[cut - 1] = end;

                bipolar parent = system_open_at(
                    directory, cut == 1 ? (string_address)"/" : path,
                    O_PATH | O_DIRECTORY | O_CLOEXEC);
                if (parent < 0)
                        return parent;

                bipolar removed = system_path_remove_opened_at(
                    parent, path + cut, handle, flags);
                system_close(parent);
                return removed;
        }

        /* A directory removal may fail after a concurrent child appears.
           Detaching its name first would turn ENOTEMPTY into data loss when
           the old name is claimed before restoration.  Stable parents can
           use the single identity-checked rmdir path; unstable parents fail
           before any namespace change. */
        if (flags & SYSTEM_PATH_AT_REMOVEDIR)
        {
                bipolar same = !system_path_parent_cleanup_safe(directory)
                                   ? -13
                                   : system_path_same_opened_at(
                                         handle, directory, name);
                return same < 0 ? same
                                : system_remove_at(directory, name, flags);
        }

        system_path_stage stage;
        bipolar staged = system_path_stage_opened_at(
            address_of stage, directory, name, handle);

        return staged < 0 ? staged
                          : system_path_stage_remove(address_of stage, flags);
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

#endif
#endif

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

#if defined(LIBRARY_THREAD_RUNTIME)
/* The child of a fork is one thread whatever its parent was, so it takes its
   copy of the count back to zero -- or every lock it holds from then on would
   wait for threads that were never copied. Only when the copy is not zero
   already: a store to that page in every child would copy the page. */
#define system_fork()                                                         \
        ({                                                                    \
                bipolar system_fork_answer =                                  \
                        system_call_2(syscall(clone), SIGCHLD, 0);            \
                if_rare(system_fork_answer == 0 && threads_live)              \
                        threads_live = 0;                                     \
                system_fork_answer;                                           \
        })
#else
#define system_fork() system_call_2(syscall(clone), SIGCHLD, 0)
#endif

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

/* Every codec opened its window with the same field resets under its own
   name; the two sources are one opener each. */
static inline fn byte_input_open_fd(byte_input address_to input, bipolar fd,
                                    p8 address_to buf, positive room)
{
        address_to input = (byte_input){.fd = fd, .buf = buf, .room = room};
}

static inline fn byte_input_open_memory(byte_input address_to input,
                                        p8 address_to mem, positive length,
                                        p8 address_to buf, positive room)
{
        address_to input = (byte_input){.fd = -1, .mem = mem,
                                        .mem_len = length, .buf = buf,
                                        .room = room};
}

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
        positive padding = difference_or_zero(width, length);
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
        field.padding = difference_or_zero(width, length);
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
   batches, not one callback per escaped byte. Policy 64 is JSON's spelling.
   limit clips the emitted bytes, a partial final escape included. */
static fn writer_escaped_bulk(writer output, address_any data, positive length,
                             p8 policy, positive limit)
{
        p8 address_to bytes = data;
        while (length && limit)
        {
                p8 escaped[256];
                address_any from = bytes;
                positive plain = memory_escape_index(bytes, length, policy);
                positive2 chunk = {.x = plain, .y = plain};
                if (!plain)
                {
                        chunk = memory_into_escaped(escaped, bytes, length,
                                                    sizeof(escaped), policy | 128);
                        from = escaped;
                }
                positive kept = min(chunk.y, limit);
                output(from, kept);
                bytes += chunk.x;
                length -= chunk.x;
                limit -= kept;
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
        writer_escaped_bulk(output, data, length, policy, positive_max);
}

#ifndef KERNEL_MODE // pathname diagnostics are for utilities
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
#endif // KERNEL_MODE

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

#ifndef KERNEL_MODE // the kernel writes no JSON
static fn writer_json_string(writer output, string_address value)
{
        writer_json_span(output, (byte_span){value, string_length(value)}, false, false);
}
#endif // KERNEL_MODE

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
        writer_escaped_bulk(output, text.bytes, text.length, policy, limit);
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
                        positive pad = difference_or_zero(width, length);
                        if (last && !view->pad_last &&
                            (!(cell.flags & TABLE_RIGHT) || (!length && !view->pad_empty))) pad = 0;
                        if (last && view->pad_last && row != TABLE_HEADING)
                                pad = difference_or_zero(width + view->pad_extra, length);
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

#ifndef KERNEL_MODE // column lists are utility arguments
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
#endif // KERNEL_MODE

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

#ifndef KERNEL_MODE // the kernel has no argv, and bits_first_set is not declared there
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
#endif // KERNEL_MODE

#if !defined(KERNEL_MODE)
/*
        Streaming digests over the hash block cores.

        One state serves every algorithm: the chaining words, the message
        length, and the partial block. A write that starts on a block
        boundary hands its whole blocks to the core where they lie and keeps
        only the tail; nothing is copied twice. BLAKE2b differs in one place
        only: its last block is compressed with the final flag, so a write
        that ends exactly on a boundary holds that block back until it knows
        whether more follows.

        size is the digest length in bytes. Every algorithm but BLAKE2b has
        one, and BLAKE2b takes 1 to 64, the output length its parameter block
        is keyed by -- a shorter BLAKE2b is a different hash, not a prefix.
*/
#define DIGEST_MD5 0
#define DIGEST_SHA1 1
#define DIGEST_SHA224 2
#define DIGEST_SHA256 3
#define DIGEST_SHA384 4
#define DIGEST_SHA512 5
#define DIGEST_BLAKE2B 6

typedef struct
{
        union
        {
                p32 narrow[16];
                p64 wide[11];
        } state;
        p64 bytes;
        p8 block[128];
        p8 used;
        p8 algorithm;
        p8 size;
} digest_state;

static inline positive digest_block_size(const digest_state address_to digest)
{
        return digest->algorithm >= DIGEST_SHA384 ? 128 : 64;
}

static inline fn digest_open(digest_state address_to digest, positive algorithm,
                             positive size)
{
        static const p32 narrow[4][8] = {
            {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476},
            {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0},
            {0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939,
             0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4},
            {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19},
        };
        static const p64 wide[2][8] = {
            {0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull,
             0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
             0x67332667ffc00b31ull, 0x8eb44a8768581511ull,
             0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull},
            {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull,
             0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
             0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
             0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull},
        };

        memory_fill(digest, 0, sizeof(address_to digest));
        digest->algorithm = (p8)algorithm;
        digest->size = (p8)size;

        if (algorithm <= DIGEST_SHA256)
                memory_copy(digest->state.narrow, narrow[algorithm], sizeof(narrow[0]));
        else if (algorithm <= DIGEST_SHA512)
                memory_copy(digest->state.wide, wide[algorithm - DIGEST_SHA384],
                            sizeof(wide[0]));
        else
        {
                // The parameter block: output length, no key, fanout and depth 1.
                memory_copy(digest->state.wide, wide[1], sizeof(wide[0]));
                digest->state.wide[0] ^= 0x01010000u ^ (p64)size;
        }
}

static inline fn digest_run(digest_state address_to digest, const p8 address_to data,
                            positive blocks)
{
        switch (digest->algorithm)
        {
        case DIGEST_MD5:
                md5_blocks(digest->state.narrow, data, blocks);
                break;
        case DIGEST_SHA1:
                sha1_blocks(digest->state.narrow, data, blocks);
                break;
        case DIGEST_SHA224:
        case DIGEST_SHA256:
                sha256_blocks(digest->state.narrow, data, blocks);
                break;
        case DIGEST_SHA384:
        case DIGEST_SHA512:
                sha512_blocks(digest->state.wide, data, blocks);
                break;
        default:
                blake2b_blocks(digest->state.wide, data, blocks, 128);
        }
}

static inline fn digest_write(digest_state address_to digest, address_any bytes,
                              positive length)
{
        const p8 address_to data = bytes;
        positive size = digest_block_size(digest);
        bool held = digest->algorithm == DIGEST_BLAKE2B;

        if (!length)
                return;

        digest->bytes += length;

        if (digest->used)
        {
                positive take = size - digest->used;

                if (take > length)
                        take = length;

                memory_copy(digest->block + digest->used, data, take);
                digest->used += (p8)take;
                data += take;
                length -= take;

                if (digest->used < size || (held && !length))
                        return;

                digest_run(digest, digest->block, 1);
                digest->used = 0;
        }

        positive whole = length / size;

        if (held && whole && whole * size == length)
                whole--;

        if (whole)
        {
                digest_run(digest, data, whole);
                data += whole * size;
                length -= whole * size;
        }

        memory_copy(digest->block, data, length);
        digest->used = (p8)length;
}

static inline fn digest_close(digest_state address_to digest, p8 address_to out)
{
        positive size = digest_block_size(digest);
        positive used = digest->used;

        if (digest->algorithm == DIGEST_BLAKE2B)
        {
                memory_fill(digest->block + used, 0, 128 - used);
                digest->state.wide[10] = ~(p64)0;
                blake2b_blocks(digest->state.wide, digest->block, 1, used);

                for (positive i = 0; i < digest->size; i++)
                        out[i] = (p8)(digest->state.wide[i / 8] >> (8 * (i % 8)));
                return;
        }

        p64 bits = digest->bytes << 3;

        digest->block[used++] = 0x80;
        if (used > size - (size == 128 ? 16 : 8))
        {
                memory_fill(digest->block + used, 0, size - used);
                digest_run(digest, digest->block, 1);
                used = 0;
        }
        memory_fill(digest->block + used, 0, size - used);

        for (positive i = 0; i < 8; i++)
                if (digest->algorithm == DIGEST_MD5)
                        digest->block[56 + i] = (p8)(bits >> (8 * i));
                else
                        digest->block[size - 1 - i] = (p8)(bits >> (8 * i));
        if (size == 128)
                digest->block[119] = (p8)(digest->bytes >> 61);

        digest_run(digest, digest->block, 1);

        for (positive i = 0; i < digest->size; i++)
        {
                if (digest->algorithm == DIGEST_MD5)
                        out[i] = (p8)(digest->state.narrow[i / 4] >> (8 * (i % 4)));
                else if (digest->algorithm <= DIGEST_SHA256)
                        out[i] = (p8)(digest->state.narrow[i / 4] >> (24 - 8 * (i % 4)));
                else
                        out[i] = (p8)(digest->state.wide[i / 8] >> (56 - 8 * (i % 8)));
        }
}
#endif // !KERNEL_MODE

#endif

/*
        The allocator: malloc, free, calloc, realloc, and the aligned pair.

        Compiled from compiler_memory.c after the size specializers and
        top_bit_known, not from the first include of this file. A literal-size
        copy in here still folds, and allocator_class_of can see top_bit_known.
        Configurations that include this file alone -- the standalone printf
        check is one -- never define LIBRARY_COMMON_ALLOCATOR, so they do not
        grow an undefined jump to memory_take.
*/
#if defined(LIBRARY_COMMON_ALLOCATOR) && !defined(STANDARD_SKIP_ALLOCATOR)
/* ---- allocator.c ---- */
/*
        Experimental C standard library

        The allocator: malloc, free, calloc, realloc, and the aligned pair

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_ALLOCATOR
#define STANDARD_MODERN_C_STANDARD_ALLOCATOR

/*
        Nothing here in a kernel build, and nothing here on Windows.

        src/core.c defines STANDARD_MODERN_C_KERNEL and then includes
        compiler_memory.c, so everything in this file would otherwise be
        compiled into the module, where mmap is not a thing that can be
        called and where a symbol named free would be a very bad idea. A
        module allocates with kmalloc and vmalloc and always has.

        Windows is out for the plainer reason that memory() and memory_free()
        -- the mmap pair this stands on -- are themselves inside library.c's
        "not Windows" guard, so there is nothing underneath to build on.
*/
#if !defined(KERNEL_MODE) && !defined(WINDOWS)

/*
        This is ordinary C on purpose, for the same reason netlink.c is.

        library.c and everything it includes holds declarations and assembly
        and nothing else, and that is checked. An allocator is not a floor. It
        is policy: which shelf a size goes on, when a chunk is asked of the
        kernel, whether a block that shrank is worth moving. None of that is a
        thing one machine does differently from another, so it is written once
        here rather than three times there, and the same bytes are what run on
        x86_64, arm64 and riscv64.

        What it stands on is the assembly. memory() is mmap and memory_free()
        is munmap, both already written for all three; memory_copy_apart is
        memcpy and memory_zero is the store loop; bits_leading_zeros is one
        instruction on two of the three targets and a six step fold on the
        third. Nothing below reimplements any of them.

        THE CONTRACT PROBLEM, WHICH IS THE WHOLE DESIGN

        memory_free(address, size) is munmap and munmap wants the length. The
        caller of free(p) has one pointer and no length, and by the time it
        calls it has usually forgotten there ever was one. So the length has
        to be written down somewhere the pointer can reach, which means a
        header, which means the returned pointer is not the start of what was
        allocated.

        That is where alignment bites. malloc must return something suitable
        for any type the program might put there, which on all three of these
        targets is sixteen bytes -- long double on x86_64, and the pair
        instructions on arm64 and riscv64 want it even where the ABI would
        settle for eight. The obvious reading is that the header must
        therefore be sixteen bytes too, and that reading is wrong, and paying
        it costs half of every small allocation.

        What has to be a multiple of sixteen is the STRIDE, not the header. If
        blocks start at addresses that are eight modulo sixteen and every
        block size is a multiple of sixteen, then base plus eight is sixteen
        aligned for every block in the chunk, forever, and the header is one
        word. So that is the layout:

              block base   ->  [ tag ][ payload ..................... ]
                                 8 b    size - 8 bytes, 16 byte aligned

        The tag sits at payload minus eight and says what the block is. Two
        kinds of block need a second word -- a mapping has to remember how
        many bytes to hand back to munmap, and an over-aligned block has to
        remember where the real allocation started -- and those two put it at
        payload minus sixteen, in front of the tag:

              [ extra ][ tag ][ payload ......... ]     mapped, or shifted

        Reading is still one load at payload minus eight in every case, and
        the second word is only reached after the tag has already said it is
        there. glibc arrives at the same place from the other direction: its
        chunk header is two words, but one of them is the previous chunk's
        payload, so a live small chunk also costs eight.

        WHERE THIS CAME FROM

        src/sh/awk.c has had a working size class allocator in it for a while:
        awk_take and awk_give, twenty two power of two classes, the class in
        the eight bytes before the block, a bump pointer for fresh chunks and
        one free list per class. This is that allocator promoted, and it is
        worth being exact about what changed, because the changes are the
        difference between something awk can live with and something every
        program in the tree has to live with.

          - awk returns block + 8 from a block whose base is a power of two
            multiple, so its payloads are eight byte aligned and not sixteen.
            awk only ever puts characters and its own structures there and
            never noticed. A malloc cannot ship that, hence the stride above.

          - awk's classes are powers of two, so a 40 byte structure occupies
            64 bytes and a 1025 byte buffer occupies 2048. Here each power of
            two is cut into quarters above 64 -- 64 80 96 112 128 160 192 224
            and so on -- which caps the rounding waste at 25% instead of just
            under 100%, for one extra shift in the lookup and 52 free list
            heads instead of 22.

          - awk reports allocation failure and exits. A library returns null.

          - awk abandons whatever is left of a chunk when the next request
            does not fit. Here the remainder is cut into the largest classes
            that fit and pushed onto their free lists, so a chunk boundary
            costs nothing at all.

          - awk's chunk is four megabytes, always, so a program that allocates
            one string pays four megabytes of address space for it. Here the
            first chunk is 64 KiB and each next one doubles to a ceiling of
            four megabytes, which is the usual shape and costs one variable.

          - The free list link overwrites awk's class word, so a block on the
            list has forgotten what it is until it is popped again. Here the
            link lives in the payload -- every class is at least sixteen bytes
            so there are always at least eight to put it in -- and the tag is
            true at every moment of a block's life. That is what makes it safe
            for free() to look at the tag and refuse a block whose tag is not
            one it wrote.

        THREADS

        Every thread pops and pushes its own shelves. The heads live in the
        thread block (platform/linux.inc), so memory_take and memory_give are
        the same load and store they always were, through fs or tp instead of
        a .bss array, and neither takes a lock. A block freed on a thread
        other than the one that took it simply joins the freeing thread's
        shelf: blocks of a class are interchangeable.

        What threads share is the bump pointer, what is left beside it, the
        chunk schedule, and a depot of whole free chains that joined threads
        left behind. allocator_lock covers exactly those, and only the slow
        path touches them. While threads_live reads zero the lock is a load
        and a store (its elision), so a program that never starts a thread
        pays nothing it did not pay before. A thread among others cuts a
        small batch into its own shelf while it holds the lock, so the next
        few allocations of that class need no lock at all.

        thread_join hands the joined thread's shelves to the depot, where the
        next thread whose shelf runs dry -- the first thread included --
        takes a whole chain back before cutting anything new.

        WHAT IS NEVER GIVEN BACK

        Chunk memory is never unmapped. A block freed goes onto its class's
        free list and waits there for a request of that class; it is not
        coalesced with its neighbours, because there are no neighbour links to
        coalesce with and adding them would put a second word back into every
        small block. So the high water mark of each class is held for the life
        of the process. Blocks too big for any class are their own mapping and
        those are handed straight back to the kernel on free, which is where
        the memory a long running program actually notices lives.
*/

//      The smallest alignment malloc may return. Sixteen on all three of
//      these targets and not likely to grow; if it ever does, every class
//      size below has to become a multiple of the new number.
#define ALLOCATOR_ALIGNMENT 16

//      One word in front of the payload for a class block, two for the two
//      kinds that carry a second word. The wide one is what a mapping's base
//      is offset by, so it also keeps the payload sixteen aligned.
#define ALLOCATOR_HEADER 8
#define ALLOCATOR_HEADER_WIDE 16

/*
        Four kilobytes, and it is a rounding unit rather than a claim about
        the machine. arm64 is configured with sixteen or sixty four kilobyte
        pages on plenty of real systems, and this number being smaller than
        the true page is harmless in both directions: mmap rounds a length up
        on its own, and munmap rounds a length up to the same true page, so a
        mapping recorded as a multiple of four kilobytes is unmapped exactly
        and completely.

        That holds for mremap too, and mremap is the one that would hurt if it
        did not: rounding a length up to four kilobytes can never cross a
        larger page boundary that the true rounding would not have crossed,
        because the true rounding of a length is itself a multiple of four
        kilobytes and is at least the length, so the four kilobyte rounding
        always lands at or below it and both then round to the same page. A
        length that came out short of the real mapping would make mremap move
        the front of a mapping and leave the tail of it behind, and that is
        the failure this paragraph exists to rule out.

        The only consequence is that malloc_usable_size reports less than the
        kernel really left there on a machine with larger pages, which is the
        safe direction to be wrong in.
*/
#define ALLOCATOR_PAGE 4096

//      The first chunk asked of the kernel, and the largest. Doubling from
//      one to the other means a program that allocates a handful of strings
//      touches 64 KiB of address space and a program that allocates a million
//      of them stops asking after a dozen calls.
#define ALLOCATOR_CHUNK_FIRST (64u << 10)
#define ALLOCATOR_CHUNK_MAX (4u << 20)

/*
        A ceiling on any single size this will consider, so that adding a
        header, an alignment's worth of padding and a page of rounding to it
        cannot wrap. It is a sixteenth of the address space, which is 2^60 on
        these three and 2^28 where positive is thirty two bits wide, and in
        both cases it is far past anything the kernel would map anyway. The
        point is not the number, the point is that every sum below is proven
        not to overflow by one comparison at the top.
*/
#define ALLOCATOR_LIMIT (((positive)-1) >> 4)

/*
        Fifty two shelves. Sixteen, thirty two and forty eight on their own,
        and from sixty four upward each power of two cut into quarters, up to
        a quarter of a megabyte. Everything above that is its own mapping.

        Every one of them is a multiple of sixteen, which is what keeps the
        payloads aligned as the bump pointer walks a chunk, and the table is
        written out rather than computed so that the arithmetic in
        allocator_class_of can be checked against something that is not
        itself.
*/
#define ALLOCATOR_CLASSES 52
#define ALLOCATOR_LARGEST 262144

//      Tags that are not class indexes. Both are just past the shelves, so a
//      single unsigned compare separates a live block of a class from
//      everything else.
#define ALLOCATOR_MAPPED ((positive)ALLOCATOR_CLASSES)
#define ALLOCATOR_SHIFTED ((positive)ALLOCATOR_CLASSES + 1)

/*
        And a whole band of tags above those, one per shelf, that mean "on the
        free list of this shelf right now".

        This is what the link living in the payload buys. awk's allocator puts
        the free list link where the class word is, so a block on the list has
        no tag at all and a second free of the same pointer walks straight
        through and pushes it a second time -- after which two allocations of
        that shelf return the same address and the program has two owners of
        one block and no way to find out. Here the tag is a separate word that
        nothing on the list path needs, so freeing can move it into this band
        and taking can move it back, and a second free finds a tag that is not
        a shelf and does nothing at all.

        It is not a heap checker. It does not notice a pointer into the middle
        of a block, or a write that ran off the end of the block before it, or
        a free of a stack address that happens to have a small number eight
        bytes in front of it. What it does is turn the single most common
        memory bug in C from silent list corruption into nothing happening,
        for one addition on each side.
*/
#define ALLOCATOR_FREED ((positive)ALLOCATOR_CLASSES + 2)

//      mremap's flag word: the kernel may pick a new address rather than
//      failing when the mapping cannot grow where it stands.
#define ALLOCATOR_MREMAP_MAYMOVE 1

//      What posix_memalign answers with. It reports through its return value
//      rather than through errno, which is the one place in the C library
//      where that is true, and it is why this file needs no errno at all.
#define ALLOCATOR_EINVAL 22
#define ALLOCATOR_ENOMEM 12

static const positive allocator_class_size[ALLOCATOR_CLASSES] = {
        16, 32, 48,
        64, 80, 96, 112,
        128, 160, 192, 224,
        256, 320, 384, 448,
        512, 640, 768, 896,
        1024, 1280, 1536, 1792,
        2048, 2560, 3072, 3584,
        4096, 5120, 6144, 7168,
        8192, 10240, 12288, 14336,
        16384, 20480, 24576, 28672,
        32768, 40960, 49152, 57344,
        65536, 81920, 98304, 114688,
        131072, 163840, 196608, 229376,
        262144,
};

//      One head per shelf, holding payload addresses rather than bases. The
//      link to the next free block lives in the payload's first eight bytes,
//      which every class has room for, so a pop is a load and a store and the
//      tag is never disturbed.
//
//      The heads themselves are in library.c beside memory_take, which is the
//      only routine that pops one on the path that matters. What is here is
//      the rest of the family reaching the same object. The two literals that
//      assembly spells out are checked against this file's constants below.
_Static_assert(ALLOCATOR_CLASSES == THREAD_SHELVES,
               "a thread block carries one shelf head per class");

//      The calling thread's shelves, which is what the name always meant in
//      a process of one thread.
#define allocator_free_list (thread_self()->shelves)
_Static_assert(ALLOCATOR_LARGEST - ALLOCATOR_HEADER == 262136,
               "library.c's memory_take compares the request against 262136");

//      The base of the next block to be cut from the current chunk, which is
//      always eight modulo sixteen, and how many bytes are left after it.
static p8 address_to allocator_bump;
static positive allocator_bump_left;

//      How large the next chunk asked of the kernel will be. Zero means none
//      has been asked for yet, which is what a program that never allocates
//      pays: three words of bss and no syscall.
static positive allocator_chunk_next;

//      What threads share, and the lock that covers it: the three words
//      above, and the depot of free chains joined threads handed back.
#define ALLOCATOR_DEPOT_SLOTS 8
#define ALLOCATOR_BATCH 8
static lock allocator_lock;
static address_any allocator_depot[ALLOCATOR_CLASSES][ALLOCATOR_DEPOT_SLOTS];
static p8 allocator_depot_count[ALLOCATOR_CLASSES];

//      The address of the tag, and of the second word the two wide kinds put
//      in front of it. Written as functions returning the address rather than
//      as macros returning the value so that both reading and writing them
//      read the same at the call site.
static positive address_to allocator_tag(address_any block)
{
        return (positive address_to)block - 1;
}

static positive address_to allocator_extra(address_any block)
{
        return (positive address_to)block - 2;
}

//      The free list link, which is the payload's own first word.
static address_any address_to allocator_link(address_any block)
{
        return (address_any address_to)block;
}

#define allocator_page_round(bytes)                                         \
        (((bytes) + (ALLOCATOR_PAGE - 1)) & ~(positive)(ALLOCATOR_PAGE - 1))

/*
        Which shelf a request of this many bytes -- header included -- belongs
        on, or ALLOCATOR_CLASSES when it belongs on none of them.

        The four smallest are answered by a shift because the quarter cut has
        no meaning below sixty four: a quarter of thirty two is eight and the
        stride would stop being sixteen. Sixteen, thirty two, forty eight and
        sixty four are sixteen apart, so the shelf is how many whole sixteens
        the request needs, which is one subtraction and one shift. What stood
        here was four compares and four branches arriving at the same four
        numbers, and it was the first thing every small allocation ran into.
        It is now ahead of the ceiling test rather than behind it, because a
        small request is the common one and it should reach its answer on one
        compare; a request too large for any shelf pays the extra compare and
        is about to call the kernel anyway.

        Nothing arrives here with a want of zero, which is what makes the
        subtraction safe. The two allocating callers add ALLOCATOR_HEADER
        before asking, so the least either can present is eight, and the
        remainder walk only asks while at least sixteen bytes are left.

        From sixty five upward the answer is arithmetic. Take the position of
        the highest set bit, call it high, so that the request sits in
        [2^high, 2^(high+1)). A quarter of that interval is 2^(high-2), and
        rounding the request up to a whole number of quarters gives a value
        from four to eight. Four through seven are the four shelves of this
        interval; eight is the next interval's first shelf, and the index
        arithmetic below lands on it without a branch, because the four
        shelves of every interval are consecutive.

        Which is the reason the shelves above sixty four are laid out in the
        table in groups of four in the first place.

        The highest set bit comes from top_bit_known rather than from
        bits_leading_zeros. They answer the same question and the assembly one
        is the better instruction on two of the three machines, but it is an
        assembly symbol: the compiler cannot see through a call to one, so
        what stood here was a call through the PLT with a stack frame built
        around it and the argument spilled across it, for a value already in a
        register. top_bit_known is the umbrella's own spelling of the same
        question and it is inline everywhere -- bsr on x86_64, clz on arm64,
        and on a riscv baseline with no Zbb to count leading zeros with, the
        same halving search the riscv bodies in library.c use. Folding it in
        is also what let the whole routine be inlined into allocator_take,
        which it was not before: measured on x86_64 over two million small
        allocations, 158.2 million instructions and 80.0 million cycles
        became 119.7 million and 32.7 million.
*/
static b32 allocator_class_of(positive want)
{
        if (want <= 64)
                return (b32)((want - 1) >> 4);

        if (want > ALLOCATOR_LARGEST)
                return ALLOCATOR_CLASSES;

        b32 high = (b32)top_bit_known(want);
        positive step = (positive)1 << (high - 2);
        positive quarter = (want + step - 1) >> (high - 2);

        return 3 + 4 * (high - 6) + (b32)(quarter - 4);
}

/*
        How many bytes a fresh allocation of this size would actually leave
        usable. realloc asks this rather than comparing sizes, because the
        question it needs answered is not "is the new size smaller" but "would
        a new block be a different block at all" -- and inside one shelf the
        answer is no, whichever direction the size moved.
*/
static positive allocator_fit(positive bytes)
{
        b32 class = allocator_class_of(bytes + ALLOCATOR_HEADER);

        if (class < ALLOCATOR_CLASSES)
                return allocator_class_size[class] - ALLOCATOR_HEADER;

        return allocator_page_round(bytes + ALLOCATOR_HEADER_WIDE) -
               ALLOCATOR_HEADER_WIDE;
}

/*
        Spend what is left of the current chunk before abandoning it.

        A chunk ends when the next request does not fit in what remains, and
        what remains at that moment is anything from nothing to one byte short
        of the largest class. Cutting it into the biggest shelves that fit and
        pushing those onto their free lists turns the whole of it back into
        allocations, so the only memory a chunk boundary loses is whatever is
        left under sixteen bytes.

        allocator_class_of rounds up, so the shelf it names for the remainder
        is either exactly the remainder or one too big, and stepping back one
        is enough. The loop is bounded by fifty two iterations because each
        turn takes at least sixteen bytes and each next shelf is no larger
        than the one before.
*/
static fn allocator_spend_remainder(void)
{
        while (allocator_bump_left >= allocator_class_size[0])
        {
                b32 class = allocator_class_of(allocator_bump_left);

                if (class >= ALLOCATOR_CLASSES)
                        class = ALLOCATOR_CLASSES - 1;
                else if (allocator_class_size[class] > allocator_bump_left)
                        class--;

                address_any block = (address_any)(allocator_bump + ALLOCATOR_HEADER);

                address_to allocator_tag(block) = ALLOCATOR_FREED + class;
                address_to allocator_link(block) = allocator_free_list[class];
                allocator_free_list[class] = block;

                allocator_bump += allocator_class_size[class];
                allocator_bump_left -= allocator_class_size[class];
        }
}

/*
        The one place a block comes from.

        fresh, when a caller passes an address for it, comes back true only
        when the payload is known to be untouched kernel memory and therefore
        already zero. That is exactly two cases: a block cut from the bump
        pointer, because a chunk is freshly mapped and the bump pointer only
        ever moves forward over it, and a mapping of its own. A block off a
        free list has been written by whoever had it last and says false, and
        so does a block cut from a chunk's remainder, because pushing it onto
        a free list wrote a link into its payload. calloc is the only caller
        that asks, and the only thing it does with a false is zero the block
        it would otherwise have had to zero anyway.
*/
/*
        Cutting a fresh block of a class from the current chunk, asking the
        kernel for the next chunk when this one is spent. The shared half of
        the allocator: under allocator_lock whenever another thread exists.
*/
static inline __attribute__((always_inline)) address_any
allocator_cut(b32 class, bool address_to fresh)
{
        positive size = allocator_class_size[class];

        if (allocator_bump_left < size)
        {
                allocator_spend_remainder();

                positive chunk = allocator_chunk_next;

                if (!chunk)
                        chunk = ALLOCATOR_CHUNK_FIRST;

                //      A shelf larger than the chunk schedule has reached
                //      gets a chunk of its own size instead, rather than the
                //      schedule being jumped forward for one request.
                if (chunk < size + ALLOCATOR_HEADER)
                        chunk = allocator_page_round(size + ALLOCATOR_HEADER);

                positive got = (positive)memory(chunk);

                if (!got || system_failed(got))
                        return null;

                //      Eight bytes of the page go unused so that the first
                //      base lands eight past a sixteen byte boundary, which
                //      is what puts every payload in the chunk on one.
                allocator_bump = (p8 address_to)(got + ALLOCATOR_HEADER);
                allocator_bump_left = chunk - ALLOCATOR_HEADER;

                allocator_chunk_next = chunk < ALLOCATOR_CHUNK_MAX
                                               ? chunk + chunk
                                               : ALLOCATOR_CHUNK_MAX;

                if (allocator_chunk_next > ALLOCATOR_CHUNK_MAX)
                        allocator_chunk_next = ALLOCATOR_CHUNK_MAX;
        }

        address_any block = (address_any)(allocator_bump + ALLOCATOR_HEADER);

        allocator_bump += size;
        allocator_bump_left -= size;

        address_to allocator_tag(block) = (positive)class;

        if (fresh)
                address_to fresh = 1;

        return block;
}

/*
        The slow path with company, or with chains waiting in the depot.
*/
static COLD __attribute__((noinline)) address_any
allocator_take_shared(b32 class, bool address_to fresh)
{
        address_any block;

        lock_take(address_of allocator_lock);

        if (allocator_depot_count[class])
        {
                block = allocator_depot[class][--allocator_depot_count[class]];
                lock_release(address_of allocator_lock);

                allocator_free_list[class] = address_to allocator_link(block);
                address_to allocator_tag(block) = (positive)class;

                return block;
        }

        block = allocator_cut(class, fresh);

        if (block && threads_live)
        {
                positive size = allocator_class_size[class];
                positive more = ALLOCATOR_BATCH;

                while (more-- && allocator_bump_left >= size)
                {
                        address_any extra =
                                (address_any)(allocator_bump + ALLOCATOR_HEADER);

                        allocator_bump += size;
                        allocator_bump_left -= size;

                        address_to allocator_tag(extra) = ALLOCATOR_FREED + class;
                        address_to allocator_link(extra) = allocator_free_list[class];
                        allocator_free_list[class] = extra;
                }
        }

        lock_release(address_of allocator_lock);

        return block;
}

static address_any allocator_take(positive bytes, bool address_to fresh)
{
        if (fresh)
                address_to fresh = 0;

        if (bytes >= ALLOCATOR_LIMIT)
                return null;

        b32 class = allocator_class_of(bytes + ALLOCATOR_HEADER);

        //      Too big for any shelf: its own mapping, and the length written
        //      down in front of the tag because munmap will want it back.
        if (class >= ALLOCATOR_CLASSES)
        {
                positive whole =
                        allocator_page_round(bytes + ALLOCATOR_HEADER_WIDE);
                positive got = (positive)memory(whole);

                //      memory() is the raw trap and returns the kernel's
                //      answer unchanged, so a failure is a small negative
                //      number wearing an unsigned hat.
                if (!got || system_failed(got))
                        return null;

                address_any block = (address_any)(got + ALLOCATOR_HEADER_WIDE);

                address_to allocator_tag(block) = ALLOCATOR_MAPPED;
                address_to allocator_extra(block) = whole;

                if (fresh)
                        address_to fresh = 1;

                return block;
        }

        if (allocator_free_list[class])
        {
                address_any block = allocator_free_list[class];

                allocator_free_list[class] = address_to allocator_link(block);

                //      Back from the freed band to the plain shelf number,
                //      which is what says this block is live.
                address_to allocator_tag(block) = (positive)class;

                return block;
        }

        if_rare(threads_live | allocator_depot_count[class])
                return allocator_take_shared(class, fresh);

        return allocator_cut(class, fresh);
}

/*
        malloc.

        A request of zero is a request for a block: the standard allows null
        and allows a pointer, and a pointer is the answer that does not make
        every caller check twice, so zero lands on the sixteen byte shelf like
        anything else under nine bytes and comes back with eight usable bytes
        and a tag free() will recognise. Two calls to malloc(0) return two
        different pointers, which is what a program that uses the pointer as
        an identity expects.
*/
//      What library.c's memory_take jumps to when the shelf could not answer:
//      a request past the largest shelf, an empty shelf, or a size that would
//      wrap. Everything the fast path skipped is redone here, because a slow
//      path that runs once per refill can afford to.
//
//      pub, and it has to be. The only thing that reaches this is a jump
//      inside a top-level __asm__ string, which the compiler treats as text
//      it cannot read: nothing it can see refers to the name. The shell and
//      the image are built with -flto -fwhole-program, where that is licence
//      to delete the body, and the link then fails on an undefined
//      allocator_take_slow. pub carries KEEP -- __attribute__((used)) -- and
//      is what says the reference exists somewhere the compiler is not
//      looking. The same goes for allocator_give_slow below.
pub address_any allocator_take_slow(positive bytes)
{
        return allocator_take(bytes, null);
}

//      Every non-empty shelf of a thread goes to the depot whole; a full depot
//      slot has the chain spliced in front of it. thread_join's call, made
//      after the kernel has cleared the thread's id and before its block is
//      unmapped, so reading its shelves races with nothing; and a consumer's
//      own call, so blocks other threads allocated and it freed go back to
//      where those threads look when their shelves run dry.
static fn allocator_shelves_hand(thread address_to it)
{
        b32 class;

        lock_take(address_of allocator_lock);

        for (class = 0; class < ALLOCATOR_CLASSES; class++)
        {
                address_any head = it->shelves[class];

                if (!head)
                        continue;

                it->shelves[class] = null;

                if (allocator_depot_count[class] < ALLOCATOR_DEPOT_SLOTS)
                {
                        allocator_depot[class][allocator_depot_count[class]++] = head;
                        continue;
                }

                address_any tail = head;

                while (address_to allocator_link(tail))
                        tail = address_to allocator_link(tail);

                address_to allocator_link(tail) =
                        allocator_depot[class][ALLOCATOR_DEPOT_SLOTS - 1];
                allocator_depot[class][ALLOCATOR_DEPOT_SLOTS - 1] = head;
        }

        lock_release(address_of allocator_lock);
}

pub fn allocator_thread_retire(thread address_to it)
{
        allocator_shelves_hand(it);
}

pub fn allocator_shelves_share(void)
{
        allocator_shelves_hand(thread_self());
}

/*
        free.

        Null is a no-op, and that is not a courtesy: the cleanup path of
        almost every function in a C program frees things that may never have
        been allocated, and a free that could not take null would put a test
        around every one of them.

        The tag decides the rest. A mapping goes back to the kernel whole, an
        over-aligned block hands the question to the allocation underneath it,
        and everything else goes onto the free list of the shelf it has said
        it belongs to since it was cut.

        The shelf comparison is deliberately first. It is the only path in a
        warmed malloc/free loop, while shifted and mapped blocks are the rare
        cases. Putting their two equality tests first cost that loop two
        branches and four retired instructions per pair.

        A tag in the freed band means this block is already on a list, so the
        second free of it does nothing. A tag that is none of the above means
        the pointer did not come from here at all, or points into the middle
        of something, or the block in front of it overran and wrote over the
        word. Nothing here can tell those apart and there is no abort to reach
        for, so the block is left exactly as it is. That leaks, and leaking is
        the containment: the alternative is to index the free list array with
        whatever the number happened to be and write a pointer through it.
*/
//      The shelf push is assembly in library.c beside the pop. What is left
//      here is everything a shelf number does not cover, reached by a jump
//      from it: the block is known to carry a tag of ALLOCATOR_CLASSES or
//      more, so the shelf test is not repeated.
pub fn allocator_give_slow(address_any block)
{
        positive tag = address_to allocator_tag(block);

        if (tag == ALLOCATOR_SHIFTED)
        {
                memory_give((address_any)address_to allocator_extra(block));
                return;
        }

        if (tag == ALLOCATOR_MAPPED)
        {
                memory_free((address_any)((positive)block -
                                          ALLOCATOR_HEADER_WIDE),
                            address_to allocator_extra(block));
                return;
        }

        //      A tag in the freed band, or one this allocator did not write.
}

/*
        malloc_usable_size.

        How many bytes are really there, which is at least what was asked for
        and usually more, because a shelf is a rounded size. A program is
        allowed to use all of it. Reporting a shelf's whole payload rather
        than the original request is both the standard behaviour and the
        honest one -- the memory is spent either way -- and it is what lets
        realloc decide in one comparison whether a block needs to move.

        An over-aligned block reports what is left of the allocation
        underneath it after the padding that got it onto its boundary, which
        is the only figure a caller can safely write into.

        As in free, the ordinary shelf is recognized first: asking the usable
        size of a live malloc block is the common call, and needs only the one
        range check before the table lookup.
*/
pub PURE positive memory_usable_size(address_any block)
{
        if (!block)
                return 0;

        positive tag = address_to allocator_tag(block);

        if (tag < ALLOCATOR_CLASSES)
                return allocator_class_size[tag] - ALLOCATOR_HEADER;

        if (tag == ALLOCATOR_SHIFTED)
        {
                positive inner = address_to allocator_extra(block);
                positive lead = (positive)block - inner;
                positive whole = memory_usable_size((address_any)inner);

                return difference_or_zero(whole, lead);
        }

        if (tag == ALLOCATOR_MAPPED)
                return address_to allocator_extra(block) - ALLOCATOR_HEADER_WIDE;

        return 0;
}

/*
        calloc.

        Two things beyond malloc. The multiplication has to be checked,
        because calloc(count, size) is the one allocation call in C that takes
        two numbers and multiplies them, and every historical hole of this
        shape has been an unchecked multiply wrapping to a small number and a
        loop then writing count elements into it. The check is the
        multiplication itself, which is where the answer was all along.

        What stood here was count > MAX / size, which is exact and is what
        the interface needs, and which on all three of these machines is a
        sixty four bit division: twenty to forty cycles, not foldable because
        neither operand is known, and in front of every call. The comment
        that came with it said that cost sat on the cold side of a call about
        to touch every byte of the result anyway, and that is false on
        precisely the path the next paragraph is proud of -- a calloc large
        enough to get its own mapping writes nothing at all, so the division
        was the whole of the call.

        The wide half of the product is the same test and is already computed
        by the multiply. __builtin_mul_overflow is mul and seto on x86_64,
        mul and umulh on arm64, mul and mulhu on riscv64, inline on all three
        with no reach into a libgcc helper that a -nostdlib link would have
        no symbol for, which was worth checking before trusting it. The two
        forms agree everywhere including both corners: a size of zero and a
        count of zero each give a product of zero and no overflow, which is
        what the division form arrived at by skipping itself. Measured on
        x86_64 over two million calloc calls, 228.0 million instructions and
        44.7 million cycles became 184.0 million and 38.7 million.

        And the zeroing is skipped exactly when it can be proven unnecessary.
        A block cut from a chunk the kernel has only just handed over, or a
        mapping of its own, is already zero and stays zero until somebody
        writes to it; a block off a free list held somebody else's data ten
        instructions ago. allocator_take knows which of those it did and says
        so. For a large calloc that is the whole cost of the call: the pages
        are not even faulted in until they are read.
*/
pub address_any memory_take_zeroed(positive count, positive size)
{
        positive bytes;

        if (__builtin_mul_overflow(count, size, address_of bytes))
                return null;

        bool fresh = 0;
        address_any block = allocator_take(bytes, address_of fresh);

        if (!block)
                return null;

        if (!fresh && bytes)
                memory_zero(block, bytes);

        return block;
}

/*
        realloc, and its four corners.

        A null block is malloc, because that is what makes a grow-as-you-go
        loop start from nothing without a special first turn. A size of zero
        frees and answers null, which is what glibc does and what every
        program written before C23 deprecated it expects; a program that wants
        the other reading can test the size itself.

        Otherwise the question is whether the block has to move at all, and
        the answer is not "did the size go up". Sizes inside one shelf all get
        the same block, so the test is whether a fresh allocation of the new
        size would have a different usable size than this one already has. If
        it would not, the block stays exactly where it is, and a loop that
        grows a buffer a byte at a time crosses a shelf boundary about fifty
        times over the whole address space instead of copying every turn.

        When it would, the block moves, and min(old, new) bytes come with it.
        The old usable size is the right thing to copy up to rather than the
        old request, which is not written down anywhere: every byte of it is
        this block's and copying a few more of them than the caller ever wrote
        is free.

        The one case that does not move is a mapping still too large for any
        shelf. mremap resizes those in the page tables, which for anything
        over a megabyte is the difference between a syscall and a memcpy, and
        it is allowed to move it, in which case the kernel has already brought
        the contents along. If the kernel refuses -- and it can, there is no
        guarantee here -- the copy underneath catches it.

        A failed allocation while shrinking answers with the original block
        rather than null. It is still there, it is still large enough, and
        nothing was freed; answering null would be true of the new block and a
        lie about the old one, and callers write p = realloc(p, n).
*/
pub address_any memory_resize(address_any block, positive bytes)
{
        if (!block)
                return memory_take(bytes);

        if (!bytes)
        {
                memory_give(block);
                return null;
        }

        if (bytes >= ALLOCATOR_LIMIT)
                return null;

        positive usable = memory_usable_size(block);
        positive tag = address_to allocator_tag(block);

        if (allocator_fit(bytes) == usable)
                return block;

#if defined(LINUX)
        //      Linux only, because mremap is a Linux call and the macOS
        //      table beside it has no number to name. Everywhere else the
        //      copy below is the whole of realloc for a mapping, which is
        //      slower and no less correct.
        if (tag == ALLOCATOR_MAPPED &&
            bytes + ALLOCATOR_HEADER > ALLOCATOR_LARGEST)
        {
                positive have = address_to allocator_extra(block);
                positive whole =
                        allocator_page_round(bytes + ALLOCATOR_HEADER_WIDE);
                positive moved = (positive)system_call_4(
                        syscall(mremap),
                        (positive)block - ALLOCATOR_HEADER_WIDE, have, whole,
                        ALLOCATOR_MREMAP_MAYMOVE);

                if (moved && !system_failed(moved))
                {
                        address_any grown =
                                (address_any)(moved + ALLOCATOR_HEADER_WIDE);

                        address_to allocator_tag(grown) = ALLOCATOR_MAPPED;
                        address_to allocator_extra(grown) = whole;

                        return grown;
                }
        }
#endif

        address_any grown = memory_take(bytes);

        if (!grown)
                return bytes > usable ? null : block;

        memory_copy_apart(grown, block, bytes < usable ? bytes : usable);
        memory_give(block);

        return grown;
}

/* The cold half of memory_resize_reserve. On x86 GCC's IPA folding produces
   the smaller image when it may choose the outline boundary itself; on the
   fixed-width targets keeping it cold and out of line avoids cloning this
   allocator path into every editor and getline caller. */
#if X64
#define MEMORY_RESIZE_GROWTH
#else
#define MEMORY_RESIZE_GROWTH COLD __attribute__((noinline))
#endif
static address_any MEMORY_RESIZE_GROWTH memory_resize_growth(
    address_any block, positive have, positive wanted, positive first,
    positive address_to grown)
{
        positive room = memory_growth(have, wanted, first);

        if (!room)
                return null;

        block = memory_resize(block, room);

        if (block)
                address_to grown = room;

        return block;
}
#undef MEMORY_RESIZE_GROWTH

/*
        aligned_alloc, and the shifted block it invents.

        Sixteen and under is already true of every block this allocator hands
        out, so those requests are plain malloc and cost nothing extra. Above
        that the only way to land on a boundary is to ask for enough room to
        walk forward to one: the alignment itself, plus the sixteen bytes the
        walk has to start past so that the header written at the destination
        cannot land on the inner block's own header.

        The block that comes back is tagged shifted and remembers the inner
        pointer, and free, realloc and malloc_usable_size all follow that one
        word back to the real allocation. Which means the padding in front is
        not tracked and not reused -- it is simply part of a larger block that
        will be freed whole.

        realloc of one of these answers with an ordinary sixteen byte aligned
        block, because a resize is a new allocation and nothing in the block
        records what alignment it was originally asked to sit on. glibc does
        the same and C says nothing about the case, but a caller that keeps
        needing the boundary has to ask for it again rather than resize.

        C11 says the size passed here should be a multiple of the alignment.
        glibc does not enforce that and neither does this, because refusing
        would break the many callers that ask for a page aligned buffer of
        exactly the length they have, and because there is no case where
        honouring the request is unsafe.
*/
pub address_any memory_take_aligned(positive alignment, positive bytes)
{
        //      A power of two, and not zero. The and-with-one-less test is
        //      also true of zero, so zero is refused first.
        if (!alignment || (alignment & (alignment - 1)))
                return null;

        if (alignment <= ALLOCATOR_ALIGNMENT)
                return memory_take(bytes);

        if (alignment >= ALLOCATOR_LIMIT ||
            bytes >= ALLOCATOR_LIMIT - alignment - ALLOCATOR_HEADER_WIDE)
                return null;

        address_any inner =
                memory_take(bytes + alignment + ALLOCATOR_HEADER_WIDE);

        if (!inner)
                return null;

        positive walk = (positive)inner + ALLOCATOR_HEADER_WIDE;
        positive landed = (walk + alignment - 1) & ~(alignment - 1);
        address_any block = (address_any)landed;

        address_to allocator_tag(block) = ALLOCATOR_SHIFTED;
        address_to allocator_extra(block) = (positive)inner;

        return block;
}

/*
        posix_memalign.

        The same allocation with the older interface around it: the result
        goes through a pointer and the failure comes back as the errno value
        itself rather than being left in a global. It is stricter than
        aligned_alloc about the alignment -- a power of two AND at least the
        width of a pointer -- and that stricter rule is the standard's, not an
        opinion, so four is refused here and accepted above.

        On failure the caller's pointer is left alone rather than being set to
        null, which is what glibc does and what a caller checking the return
        value will never notice either way.
*/
pub b32 memory_take_aligned_into(address_any address_to result,
                                 positive alignment, positive bytes)
{
        if (!result)
                return ALLOCATOR_EINVAL;

        if (!alignment || (alignment & (alignment - 1)) ||
            alignment < sizeof(address_any))
                return ALLOCATOR_EINVAL;

        address_any block = memory_take_aligned(alignment, bytes);

        if (!block)
                return ALLOCATOR_ENOMEM;

        address_to result = block;
        return 0;
}

/*
        The names C knows these by.

        Aliases rather than wrappers, so malloc and memory_take are one symbol
        at one address and neither costs a jump to reach the other. A program
        may take the address of either and compare them and they will be
        equal, which is the honest answer: they are the same function and the
        prose name is the one it was written under.
*/
//      memory_take is assembly in library.c, and GCC's alias attribute wants
//      a C definition in this translation unit to point at. A .set is the
//      same thing one layer down and does not care how the target was
//      written, which is how library.c spells every other standard name.
__asm__(ASM_ALIAS(malloc, memory_take));
__asm__(ASM_ALIAS(free, memory_give));

address_any malloc(positive size);
fn free(address_any block);

pub address_any calloc(positive count, positive size)
        __attribute__((alias("memory_take_zeroed")));

pub address_any realloc(address_any block, positive bytes)
        __attribute__((alias("memory_resize")));

pub address_any aligned_alloc(positive alignment, positive bytes)
        __attribute__((alias("memory_take_aligned")));

pub b32 posix_memalign(address_any address_to result, positive alignment,
                       positive bytes)
        __attribute__((alias("memory_take_aligned_into")));

pub PURE positive malloc_usable_size(address_any block)
        __attribute__((alias("memory_usable_size")));

//      memalign is aligned_alloc with the arguments in the same order and
//      without C11's multiple-of rule, which this does not enforce anyway, so
//      it is the same function. Programs old enough to call it exist.
pub address_any memalign(positive alignment, positive bytes)
        __attribute__((alias("memory_take_aligned")));

#endif // !KERNEL_MODE && !WINDOWS

#endif // STANDARD_MODERN_C_STANDARD_ALLOCATOR
#if defined(LIBRARY_THREAD_RUNTIME)
/*
        THE POOL

        One per process, started the first time a call is worth threads, and
        parked on a futex word between calls. It never writes a descriptor:
        a parallel_for job touches only what its caller gave it by index, and
        a parallel_ordered job fills its own output, which the sink receives
        on the calling thread in index order. So the bytes a utility writes
        cannot depend on how many threads ran, only on how the caller chose
        to cut its work -- which it must do from the data or from a fixed
        size, never from parallel_width().

        WIDTH

        The CPUs sched_getaffinity grants at first use, so taskset decides it.
        The caller counts as one: width - 1 workers are started, slots 1 up,
        and the caller is slot 0. A beside job's thread is slot width.

        WHEN IT RUNS INLINE

        count below two, bytes below PARALLEL_MINIMUM_BYTES, a width of one,
        a call from inside a job or a sink (nesting), or workers that could
        not be started. Inline runs index 0, 1, 2 ... on the caller, and an
        ordered inline run hands each job's bytes to the sink before the next
        job starts. PARALLEL_SPREAD as bytes skips the size test for work
        that is heavy per byte.

        A RUN

        The caller publishes a run and bumps the generation word; each woken
        worker counts itself busy, reads the run, and claims indices with a
        compare-and-swap on next until none are left or the run is stopped.
        The caller claims beside them. When nothing is left to claim the
        caller withdraws the run and sleeps until busy is zero, so no worker
        can still be reading a run that lived on the caller's stack. Both
        halves of every handshake -- busy against the withdrawn run, a
        finished job against a sleeping caller, a moved limit against a
        sleeping claimer -- write with a bus-locked instruction before they
        read the other side's word, which is what keeps a wake from being
        lost on a machine with a store buffer.

        ORDERED RUNS

        A ring of window = 2 x width outputs. Index i uses entry i % window,
        and nobody may claim an index at or past emitted + window, so an
        entry is never reused before its last owner was emitted. That bound
        is the memory bound and the backpressure: a worker that runs ahead
        sleeps on limit_word until the caller emits. The caller emits
        whenever the next entry is done, runs a job itself when it can claim
        one, and otherwise sleeps on finished_word.

        STOPPING

        parallel_stop marks the run the calling thread is working inside and
        wakes everybody. No job starts after a claimer sees it and no sink is
        called after the caller sees it. Which later jobs had already run is
        unspecified, which is why a per-item error that must keep the output
        exact is recorded for the sink rather than stopping the run.

        FORK

        A forked child keeps the parent's pool bookkeeping but none of its
        threads. The pool remembers the thread id that started its workers
        and starts afresh when a different one asks; system_fork has already
        set threads_live back to zero in the child.
*/
#define PARALLEL_MINIMUM_BYTES (256ull << 10)
#define PARALLEL_SPREAD positive_max
#define PARALLEL_WORKERS_MAX 63
#define PARALLEL_OUTPUT_FIRST 65536

typedef fn(address_to parallel_job)(address_any context, positive index);

typedef struct
{
        p8 address_to bytes;
        positive room;
        positive used;
        //      Grown with the allocator rather than mapped: a tree node's
        //      output, of which there are many and most are small.
        positive heap;
} parallel_output;

typedef fn(address_to parallel_emit_job)(address_any context, positive index,
                                         parallel_output address_to output);
typedef bool(address_to parallel_sink)(address_any context, positive index,
                                       address_any data, positive length);

typedef struct
{
        parallel_output output;
        b32 done;
        b32 spare;
} parallel_entry;

typedef struct
{
        parallel_job job;
        parallel_emit_job emit;
        address_any context;
        positive count;
        positive next;
        positive limit;
        positive window;
        parallel_entry address_to ring;
        b32 stop;
        b32 limit_word;
        b32 finished_word;
        b32 waiting;
        b32 claimers_waiting;
        b32 spare;
        //      A run that is not an index range brings its own participation.
        fn(address_to work)(address_any run);
} parallel_run;

static struct
{
        positive width;
        positive workers;
        b32 owner;
        b32 generation;
        b32 busy;
        b32 quit;
        parallel_run address_to run;
        thread address_to worker[PARALLEL_WORKERS_MAX];
} parallel_pool;

static struct
{
        thread address_to handle;
        b32 owner;
        b32 spare;
        parallel_job job;
        address_any context;
        parallel_run run;
} parallel_beside_state;

pub positive parallel_width(void)
{
        if (!parallel_pool.width)
        {
                p8 mask[128] = {0};
                bipolar got = system_call_3(syscall(sched_getaffinity), 0,
                                            sizeof(mask), (positive)mask);
                positive count = 0;
                bipolar at;

                for (at = 0; at < got && at < (bipolar)sizeof(mask); at++)
                {
                        p8 bits = mask[at];

                        while (bits)
                        {
                                count += bits & 1;
                                bits >>= 1;
                        }
                }

                if (!count)
                        count = 1;

                if (count > PARALLEL_WORKERS_MAX + 1)
                        count = PARALLEL_WORKERS_MAX + 1;

                parallel_pool.width = count;
        }

        return parallel_pool.width;
}

pub positive parallel_slot(void)
{
        return thread_self()->slot;
}

static fn parallel_run_stop(parallel_run address_to run)
{
        atomic_exchange(address_of run->stop, 1);
        atomic_inc(address_of run->limit_word);
        thread_wake(address_of run->limit_word, 1 << 30);
        atomic_inc(address_of run->finished_word);
        thread_wake(address_of run->finished_word, 1 << 30);
}

pub fn parallel_stop(void)
{
        parallel_run address_to run = thread_self()->run;

        if (run)
                parallel_run_stop(run);
}

pub bool parallel_stopped(void)
{
        parallel_run address_to run = thread_self()->run;

        return run && atomic_load(address_of run->stop) != 0;
}

pub p8 address_to parallel_reserve(parallel_output address_to output,
                                   positive length)
{
        p8 address_to at;

        if (length > positive_max - output->used)
        {
                parallel_stop();
                return null;
        }

        if (output->room - output->used < length && output->heap)
        {
                positive want = output->used + length;
                positive room = output->room ? output->room * 2 : 256;
                p8 address_to grown;

                if (room < want)
                        room = want;

                grown = memory_resize(output->bytes, room);

                if (!grown)
                {
                        parallel_stop();
                        return null;
                }

                output->bytes = grown;
                output->room = room;
        }
        else if (output->room - output->used < length &&
                 !memory_reserve((address_any address_to)address_of output->bytes,
                                 address_of output->room, output->used,
                                 output->used + length, 1, PARALLEL_OUTPUT_FIRST))
        {
                parallel_stop();
                return null;
        }

        at = output->bytes + output->used;
        output->used += length;

        return at;
}

pub bool parallel_write(parallel_output address_to output, address_any data,
                        positive length)
{
        p8 address_to at;

        if (!length)
                return true;

        at = parallel_reserve(output, length);

        if (!at)
                return false;

        memory_copy(at, data, length);
        return true;
}

/*
        A claim. A worker that meets the ordered limit sleeps on limit_word;
        the caller, which is the one that moves the limit, is told no and
        goes to emit instead.
*/
static bool parallel_claim(parallel_run address_to run, positive address_to index,
                           bool may_sleep)
{
        for (;;)
        {
                positive at;
                positive limit;

                if (atomic_load(address_of run->stop))
                        return false;

                at = atomic_load(address_of run->next);

                if (at >= run->count)
                        return false;

                limit = run->ring ? atomic_load(address_of run->limit) : run->count;

                if (at >= limit)
                {
                        b32 word;

                        if (!may_sleep)
                                return false;

                        word = atomic_load(address_of run->limit_word);
                        atomic_inc(address_of run->claimers_waiting);

                        if (at >= atomic_load(address_of run->limit) &&
                            !atomic_load(address_of run->stop))
                                thread_wait(address_of run->limit_word, word);

                        atomic_dec(address_of run->claimers_waiting);
                        continue;
                }

                if (atomic_compare_exchange(address_of run->next, at, at + 1))
                {
                        address_to index = at;
                        return true;
                }
        }
}

static fn parallel_run_one(parallel_run address_to run, positive index)
{
        parallel_entry address_to entry;

        if (!run->ring)
        {
                run->job(run->context, index);
                return;
        }

        entry = address_of run->ring[index % run->window];
        entry->output.used = 0;
        run->emit(run->context, index, address_of entry->output);
        atomic_exchange(address_of entry->done, 1);
        atomic_inc(address_of run->finished_word);

        if (atomic_load(address_of run->waiting))
                thread_wake(address_of run->finished_word, 1);
}

static fn parallel_participate(parallel_run address_to run)
{
        positive index;

        while (parallel_claim(run, address_of index, true))
                parallel_run_one(run, index);
}

static fn parallel_worker(address_any argument)
{
        thread address_to self = thread_self();
        b32 seen = 0;

        self->slot = (positive)argument;

        for (;;)
        {
                b32 now = atomic_load(address_of parallel_pool.generation);
                parallel_run address_to run;

                if (now == seen)
                {
                        thread_wait(address_of parallel_pool.generation, now);
                        continue;
                }

                seen = now;

                if (atomic_load(address_of parallel_pool.quit))
                        return;

                atomic_inc(address_of parallel_pool.busy);
                run = atomic_load(address_of parallel_pool.run);

                if (run)
                {
                        self->run = run;

                        if (run->work)
                                run->work(run);
                        else
                                parallel_participate(run);

                        self->run = null;
                }

                if (__atomic_sub_fetch(address_of parallel_pool.busy, 1,
                                       __ATOMIC_SEQ_CST) == 0)
                        thread_wake(address_of parallel_pool.busy, 1);
        }
}

static fn parallel_forget(void)
{
        memory_fill(address_of parallel_pool.worker, 0,
                    sizeof(parallel_pool.worker));
        parallel_pool.workers = 0;
        parallel_pool.generation = 0;
        parallel_pool.busy = 0;
        parallel_pool.quit = 0;
        parallel_pool.run = null;
}

//      Workers for this process, started if none are; false means run inline.
static bool parallel_ready(void)
{
        b32 me = (b32)system_call(syscall(gettid));
        positive slot;

        if (parallel_pool.workers && parallel_pool.owner != me)
                parallel_forget();

        if (parallel_pool.workers)
                return true;

        parallel_pool.owner = me;

        for (slot = 1; slot < parallel_width(); slot++)
        {
                thread address_to handle =
                        thread_start(parallel_worker, (address_any)slot);

                if (!handle)
                        break;

                parallel_pool.worker[parallel_pool.workers++] = handle;
        }

        return parallel_pool.workers != 0;
}

/*
        Joins every worker and forgets the width, so the next call measures
        affinity again -- or runs at the width given, which is what a check
        uses to prove output does not move with it. Zero means measure.
*/
pub fn parallel_reset(positive width)
{
        b32 me = (b32)system_call(syscall(gettid));
        positive at;

        if (parallel_pool.workers && parallel_pool.owner == me)
        {
                atomic_exchange(address_of parallel_pool.quit, 1);
                atomic_inc(address_of parallel_pool.generation);
                thread_wake(address_of parallel_pool.generation, 1 << 30);

                for (at = 0; at < parallel_pool.workers; at++)
                        thread_join(parallel_pool.worker[at]);
        }

        parallel_forget();
        parallel_pool.width = width > PARALLEL_WORKERS_MAX + 1
                                      ? PARALLEL_WORKERS_MAX + 1
                                      : width;
}

static bool parallel_inline_wanted(positive count, positive bytes)
{
        return count < 2 || bytes < PARALLEL_MINIMUM_BYTES ||
               thread_self()->run || parallel_width() == 1;
}

static fn parallel_publish(parallel_run address_to run)
{
        thread_self()->run = run;
        atomic_exchange(address_of parallel_pool.run, run);
        atomic_inc(address_of parallel_pool.generation);
        thread_wake(address_of parallel_pool.generation, 1 << 30);
}

static fn parallel_withdraw(void)
{
        b32 busy;

        atomic_exchange(address_of parallel_pool.run, (parallel_run address_to)null);

        while ((busy = atomic_load(address_of parallel_pool.busy)) != 0)
                thread_wait(address_of parallel_pool.busy, busy);

        thread_self()->run = null;
}

pub bool parallel_for(parallel_job job, address_any context, positive count,
                      positive bytes)
{
        parallel_run run = {0};
        positive index;

        run.job = job;
        run.context = context;
        run.count = count;
        run.limit = count;

        if (parallel_inline_wanted(count, bytes) || !parallel_ready())
        {
                address_any outer = thread_self()->run;

                thread_self()->run = address_of run;

                for (index = 0; index < count && !run.stop; index++)
                        job(context, index);

                thread_self()->run = outer;
                return !run.stop;
        }

        parallel_publish(address_of run);
        parallel_participate(address_of run);
        parallel_withdraw();

        return !run.stop;
}

pub bool parallel_ordered(parallel_emit_job job, parallel_sink sink,
                          address_any context, positive count, positive bytes)
{
        parallel_run run = {0};
        positive emitted = 0;
        positive at;

        run.emit = job;
        run.context = context;
        run.count = count;

        if (parallel_inline_wanted(count, bytes) || !parallel_ready())
        {
                address_any outer = thread_self()->run;
                parallel_output output = {0};

                thread_self()->run = address_of run;

                for (at = 0; at < count && !run.stop; at++)
                {
                        output.used = 0;
                        job(context, at, address_of output);

                        if (run.stop)
                                break;

                        if (!sink(context, at, output.bytes, output.used))
                                run.stop = 1;
                }

                if (output.bytes)
                        memory_release((address_any address_to)address_of output.bytes,
                                       address_of output.room, address_of output.used, 1);

                thread_self()->run = outer;
                return !run.stop;
        }

        run.window = 2 * parallel_width();
        run.limit = run.window;
        run.ring = memory_take_zeroed(run.window, sizeof(parallel_entry));

        if (!run.ring)
                return false;

        parallel_publish(address_of run);

        while (emitted < count)
        {
                parallel_entry address_to entry = address_of run.ring[emitted % run.window];
                positive index;
                b32 word;

                if (atomic_load(address_of entry->done))
                {
                        if (!atomic_load(address_of run.stop) &&
                            !sink(context, emitted, entry->output.bytes,
                                  entry->output.used))
                                parallel_run_stop(address_of run);

                        entry->output.used = 0;
                        atomic_exchange(address_of entry->done, 0);
                        emitted++;
                        atomic_exchange(address_of run.limit, emitted + run.window);
                        atomic_inc(address_of run.limit_word);

                        if (atomic_load(address_of run.claimers_waiting))
                                thread_wake(address_of run.limit_word, 1 << 30);

                        continue;
                }

                if (atomic_load(address_of run.stop))
                        break;

                if (parallel_claim(address_of run, address_of index, false))
                {
                        parallel_run_one(address_of run, index);
                        continue;
                }

                word = atomic_load(address_of run.finished_word);
                atomic_exchange(address_of run.waiting, 1);

                if (!atomic_load(address_of entry->done) &&
                    !atomic_load(address_of run.stop))
                        thread_wait(address_of run.finished_word, word);

                atomic_exchange(address_of run.waiting, 0);
        }

        parallel_withdraw();

        for (at = 0; at < run.window; at++)
                if (run.ring[at].output.bytes)
                        memory_release((address_any address_to)address_of run.ring[at].output.bytes,
                                       address_of run.ring[at].output.room,
                                       address_of run.ring[at].output.used, 1);

        memory_give(run.ring);

        return !run.stop;
}

static fn parallel_beside_entry(address_any argument)
{
        thread address_to self = thread_self();

        (void)argument;
        self->slot = parallel_pool.width;
        self->run = address_of parallel_beside_state.run;
        parallel_beside_state.job(parallel_beside_state.context, 0);
        self->run = null;
}

pub bool parallel_beside(parallel_job job, address_any context)
{
        b32 me = (b32)system_call(syscall(gettid));

        if (parallel_beside_state.handle && parallel_beside_state.owner != me)
                parallel_beside_state.handle = null;

        if (parallel_beside_state.handle || thread_self()->run ||
            parallel_width() == 1)
                return false;

        parallel_beside_state.run = (parallel_run){0};
        parallel_beside_state.job = job;
        parallel_beside_state.context = context;
        parallel_beside_state.owner = me;
        parallel_beside_state.handle = thread_start(parallel_beside_entry, null);

        return parallel_beside_state.handle != null;
}

pub bool parallel_beside_wait(void)
{
        b32 me = (b32)system_call(syscall(gettid));

        if (!parallel_beside_state.handle || parallel_beside_state.owner != me)
        {
                parallel_beside_state.handle = null;
                return false;
        }

        thread_join(parallel_beside_state.handle);
        parallel_beside_state.handle = null;

        return !parallel_beside_state.run.stop;
}

/*
        THE TREE

        parallel_tree walks a directory tree on the pool and hands the bytes
        its jobs write to the sink on the calling thread in preorder, whatever
        order the jobs ran in. A node is one directory. enter reads it and
        calls parallel_child for each subdirectory to walk, and a child's
        whole subtree lands at the byte position of that call, so the order
        of the output is the structure the jobs wrote and nothing else. leave,
        when there is one, runs once every child has been left and its bytes
        follow the last child.

        RECORDS

        Each node is a record: the caller's pointer, the name it was reached
        by, its place among its siblings, the bytes enter and leave wrote, and
        its handle. A record exists from parallel_child until the sink has
        been told the node finished, and the calling thread frees it there, so
        a record another thread might still read is never freed: a job
        touches a node only before marking it complete, and the emitter frees
        only after the mark. Freed records and outputs are handed back to the
        allocator depot every PARALLEL_TREE_SHARE_EVERY frees, or the calling
        thread's shelves would grow by everything the workers allocated.

        SCHEDULING

        Pending nodes sit on one list, and a node's children go on its front
        in the order enter named them, so the next thread to claim takes the
        first child: the walk runs close to the order it is emitted in. The
        emitter keeps a stack of the nodes it is inside. When the node it
        needs next has not run it runs that node itself; when that node is
        running elsewhere it runs any pending node it has room for, and
        otherwise sleeps on the progress word. Workers claim only while the
        entered-but-unemitted nodes and their bytes are under the held limits,
        and sleep on the room word past them; the emitter is never held back,
        so the limits bound memory without being able to stall the walk.

        HANDLES

        The pool opens every directory but the root, which is the caller's.
        A directory's handle is needed while it has children not yet opened
        through it and while its leave has not run. At most cap of them are
        open, a share of RLIMIT_NOFILE. Past the cap the least recently used
        handle that nobody is using is closed, with its device and inode
        written down, and it is opened again by name through its parent when
        a child, a leaf or its leave needs it -- through the parent's own reopen,
        recursively, if that was closed too -- and refused as ESTALE if it
        is no longer the same directory. A job holds at most its own handle
        and pins at most two others, so with no more than (cap - 1) / 3
        threads in a run there is always an idle handle to close and nothing
        ever waits for one.

        LEAVES

        parallel_leaf schedules a job that opens nothing: a file's work in a
        large directory, say. Its bytes go where it was called, like a child's
        subtree, and its job is handed the directory that called it, which
        stays pinned open for exactly as long as the leaf runs. A leaf has no
        children and no leave.
*/
#define PARALLEL_TREE_INLINE_NODES 32
#define PARALLEL_TREE_HELD_NODES 4096
#define PARALLEL_TREE_HELD_BYTES (64ull << 20)
#define PARALLEL_TREE_SHARE_EVERY 1024
#define PARALLEL_TREE_MAGIC 0x74726565u
//      ESTALE, which standard.c names after this file.
#define PARALLEL_TREE_STALE 116

enum
{
        PARALLEL_TREE_PENDING = 0,
        PARALLEL_TREE_ENTERING = 1,
        PARALLEL_TREE_ENTERED = 2,
        PARALLEL_TREE_COMPLETE = 3,
};

typedef fn(address_to parallel_node_job)(address_any context, address_any node,
                                         bipolar directory,
                                         parallel_output address_to output);
typedef bool(address_to parallel_node_sink)(address_any context, address_any node,
                                            address_any data, positive length,
                                            bool finished);

typedef struct parallel_tree_node
{
        parallel_output output;
        parallel_output tail;
        p32 magic;
        b32 state;
        address_any user;
        struct parallel_tree_node address_to parent;
        struct parallel_tree_node address_to first_child;
        struct parallel_tree_node address_to last_child;
        struct parallel_tree_node address_to sibling;
        positive offset;
        struct parallel_tree_node address_to pending_previous;
        struct parallel_tree_node address_to pending_next;
        struct parallel_tree_node address_to idle_previous;
        struct parallel_tree_node address_to idle_next;
        parallel_node_job job;
        bipolar handle;
        bipolar open_error;
        positive unstarted;
        positive unfinished;
        positive using;
        positive device;
        positive inode;
        b8 running;
        b8 idle;
        b8 left;
        b8 held;
        b8 leaf;
        b8 spare[3];
        p32 name_length;
        p8 name[];
} parallel_tree_node;

typedef struct
{
        parallel_run base;
        parallel_node_job enter;
        parallel_node_job leave;
        parallel_node_sink sink;
        address_any context;
        bipolar root_directory;
        positive open_flags;
        lock guard;
        parallel_tree_node address_to pending;
        parallel_tree_node address_to idle_newest;
        parallel_tree_node address_to idle_oldest;
        positive pending_count;
        positive open_count;
        positive open_most;
        positive cap;
        positive evictions;
        positive reopens;
        positive held_nodes;
        positive held_bytes;
        positive held_nodes_limit;
        positive entered;
        b32 seats;
        b32 complete;
        b32 progress_waiters;
        b32 room_waiters;
} parallel_tree_run;

typedef struct
{
        parallel_tree_node address_to node;
        parallel_tree_node address_to cursor;
        positive offset;
        positive stage;
} parallel_tree_frame;

//      What the last run on this process saw, for the checks.
static struct
{
        positive cap;
        positive open_most;
        positive evictions;
        positive reopens;
        positive threads;
} parallel_tree_last;

static fn parallel_tree_wake_progress(parallel_tree_run address_to run)
{
        atomic_inc(address_of run->base.finished_word);

        if (atomic_load(address_of run->progress_waiters))
                thread_wake(address_of run->base.finished_word, 1 << 30);
}

static fn parallel_tree_wake_room(parallel_tree_run address_to run)
{
        atomic_inc(address_of run->base.limit_word);

        if (atomic_load(address_of run->room_waiters))
                thread_wake(address_of run->base.limit_word, 1 << 30);
}

static bool parallel_tree_room(parallel_tree_run address_to run)
{
        return atomic_load(address_of run->held_nodes) < run->held_nodes_limit &&
               atomic_load(address_of run->held_bytes) < PARALLEL_TREE_HELD_BYTES;
}

//      -- handles, all under run->guard --------------------------------------

static fn parallel_tree_idle_remove(parallel_tree_run address_to run,
                                    parallel_tree_node address_to node)
{
        if (!node->idle)
                return;

        if (node->idle_previous)
                node->idle_previous->idle_next = node->idle_next;
        else
                run->idle_newest = node->idle_next;

        if (node->idle_next)
                node->idle_next->idle_previous = node->idle_previous;
        else
                run->idle_oldest = node->idle_previous;

        node->idle_previous = null;
        node->idle_next = null;
        node->idle = 0;
}

static fn parallel_tree_idle_push(parallel_tree_run address_to run,
                                  parallel_tree_node address_to node)
{
        parallel_tree_idle_remove(run, node);

        node->idle_next = run->idle_newest;

        if (run->idle_newest)
                run->idle_newest->idle_previous = node;
        else
                run->idle_oldest = node;

        run->idle_newest = node;
        node->idle = 1;
}

static fn parallel_tree_close(parallel_tree_run address_to run,
                              parallel_tree_node address_to node)
{
        parallel_tree_idle_remove(run, node);
        system_close(node->handle);
        node->handle = -1;
        run->open_count--;
}

//      A handle nobody is using is closed when nothing will need it again,
//      and otherwise becomes the newest idle one.
static fn parallel_tree_settle(parallel_tree_run address_to run,
                               parallel_tree_node address_to node)
{
        if (!node->parent || node->handle < 0 || node->running || node->using)
                return;

        if (!atomic_load(address_of run->base.stop) &&
            (node->unstarted || (run->leave && !node->left)))
                parallel_tree_idle_push(run, node);
        else
                parallel_tree_close(run, node);
}

static fn parallel_tree_facts(bipolar handle, positive address_to device,
                              positive address_to inode)
{
        positive facts[24] = {0};

        system_call_2(syscall(fstat), (positive)handle, (positive)facts);
        address_to device = facts[0];
        address_to inode = facts[1];
}

//      Room for one more handle: a free place under the cap, or the oldest
//      idle handle closed to make one.
static fn parallel_tree_slot(parallel_tree_run address_to run)
{
        if (run->open_count >= run->cap && run->idle_oldest)
        {
                parallel_tree_node address_to victim = run->idle_oldest;

                parallel_tree_facts(victim->handle, address_of victim->device,
                                    address_of victim->inode);
                parallel_tree_close(run, victim);
                run->evictions++;
        }

        run->open_count++;

        if (run->open_count > run->open_most)
                run->open_most = run->open_count;
}

static fn parallel_tree_unpin(parallel_tree_run address_to run,
                              parallel_tree_node address_to node)
{
        if (!node->parent)
                return;

        node->using--;
        parallel_tree_settle(run, node);
}

//      A node's own handle, opened again if it was closed to make room, and
//      marked in use until parallel_tree_unpin.
static bipolar parallel_tree_pin(parallel_tree_run address_to run,
                                 parallel_tree_node address_to node)
{
        bipolar through;
        bipolar handle;

        if (!node->parent)
                return run->root_directory;

        if (node->handle >= 0)
        {
                node->using++;
                parallel_tree_idle_remove(run, node);
                return node->handle;
        }

        if (node->open_error)
                return node->open_error;

        through = parallel_tree_pin(run, node->parent);

        if (through < 0)
                return through;

        parallel_tree_slot(run);
        handle = system_open_at(through, (string_address)node->name,
                                FILE_READ | O_DIRECTORY | O_CLOEXEC | run->open_flags);
        parallel_tree_unpin(run, node->parent);

        if (handle >= 0)
        {
                positive device;
                positive inode;

                parallel_tree_facts(handle, address_of device, address_of inode);

                if (device != node->device || inode != node->inode)
                {
                        system_close(handle);
                        handle = -PARALLEL_TREE_STALE;
                }
        }

        if (handle < 0)
        {
                run->open_count--;
                node->open_error = handle;
                return handle;
        }

        run->reopens++;
        node->handle = handle;
        node->using++;

        return handle;
}

//      -- running nodes ------------------------------------------------------

static parallel_tree_node address_to parallel_tree_claim(parallel_tree_run address_to run,
                                                         parallel_tree_node address_to wanted)
{
        parallel_tree_node address_to node;

        lock_take(address_of run->guard);

        node = wanted ? (atomic_load(address_of wanted->state) == PARALLEL_TREE_PENDING
                                 ? wanted
                                 : null)
                      : run->pending;

        if (node)
        {
                if (node->pending_previous)
                        node->pending_previous->pending_next = node->pending_next;
                else
                        run->pending = node->pending_next;

                if (node->pending_next)
                        node->pending_next->pending_previous = node->pending_previous;

                node->pending_previous = null;
                node->pending_next = null;
                atomic_sub(address_of run->pending_count, 1);
                atomic_exchange(address_of node->state, PARALLEL_TREE_ENTERING);
        }

        lock_release(address_of run->guard);

        return node;
}

static fn parallel_tree_finish(parallel_tree_run address_to run,
                               parallel_tree_node address_to node)
{
        thread address_to self = thread_self();
        address_any outer = self->run;

        for (;;)
        {
                parallel_tree_node address_to parent = node->parent;
                bool last;

                if (run->leave && !node->leaf)
                {
                        bipolar directory;

                        lock_take(address_of run->guard);
                        directory = parallel_tree_pin(run, node);
                        lock_release(address_of run->guard);

                        self->run = address_of run->base;

                        if (!atomic_load(address_of run->base.stop))
                                run->leave(run->context, node->user, directory,
                                           address_of node->tail);

                        self->run = outer;
                        atomic_add(address_of run->held_bytes, node->tail.used);

                        lock_take(address_of run->guard);
                        node->left = 1;

                        if (directory >= 0)
                                parallel_tree_unpin(run, node);

                        lock_release(address_of run->guard);
                }

                last = parent && __atomic_sub_fetch(address_of parent->unfinished, 1,
                                                    __ATOMIC_SEQ_CST) == 0;

                //      Nothing of the node is read after this: the emitter may
                //      free it as soon as it sees the mark.
                atomic_exchange(address_of node->state, PARALLEL_TREE_COMPLETE);
                parallel_tree_wake_progress(run);

                if (!last)
                        return;

                node = parent;
        }
}

static fn parallel_tree_enter(parallel_tree_run address_to run,
                              parallel_tree_node address_to node)
{
        thread address_to self = thread_self();
        address_any outer = self->run;
        parallel_tree_node address_to parent = node->parent;
        bipolar directory;

        atomic_add(address_of run->held_nodes, 1);
        node->held = 1;

        if (!parent)
                directory = run->root_directory;
        else if (node->leaf)
        {
                lock_take(address_of run->guard);
                directory = parallel_tree_pin(run, parent);
                parent->unstarted--;
                node->running = 1;
                lock_release(address_of run->guard);
        }
        else
        {
                bipolar through;

                lock_take(address_of run->guard);
                through = parallel_tree_pin(run, parent);

                if (through >= 0)
                        parallel_tree_slot(run);

                lock_release(address_of run->guard);

                directory = through >= 0
                                    ? system_open_at(through, (string_address)node->name,
                                                     FILE_READ | O_DIRECTORY | O_CLOEXEC |
                                                             run->open_flags)
                                    : through;

                lock_take(address_of run->guard);
                parent->unstarted--;

                if (through >= 0)
                {
                        if (directory < 0)
                                run->open_count--;

                        parent->using--;
                }

                parallel_tree_settle(run, parent);
                node->handle = directory >= 0 ? directory : -1;
                node->open_error = directory < 0 ? directory : 0;
                node->running = 1;
                lock_release(address_of run->guard);
        }

        self->run = address_of run->base;

        if (!atomic_load(address_of run->base.stop))
                (node->leaf ? node->job : run->enter)(run->context, node->user, directory,
                                                      address_of node->output);

        self->run = outer;

        atomic_add(address_of run->held_bytes, node->output.used);
        atomic_add(address_of run->entered, 1);

        lock_take(address_of run->guard);
        node->running = 0;

        if (node->leaf && directory >= 0)
                parallel_tree_unpin(run, parent);
        else if (node->leaf)
                parallel_tree_settle(run, parent);

        if (node->first_child)
        {
                parallel_tree_node address_to child;
                parallel_tree_node address_to previous = null;

                for (child = node->first_child; child; child = child->sibling)
                {
                        child->pending_previous = previous;
                        child->pending_next = child->sibling;
                        previous = child;
                }

                node->last_child->pending_next = run->pending;

                if (run->pending)
                        run->pending->pending_previous = node->last_child;

                run->pending = node->first_child;
                atomic_add(address_of run->pending_count, node->unstarted);
        }

        atomic_exchange(address_of node->state, PARALLEL_TREE_ENTERED);

        if (parent && !node->leaf)
                parallel_tree_settle(run, node);

        lock_release(address_of run->guard);

        if (node->first_child)
                parallel_tree_wake_progress(run);
        else
                parallel_tree_finish(run, node);
}

static fn parallel_tree_work(address_any argument)
{
        parallel_tree_run address_to run = argument;

        if (__atomic_sub_fetch(address_of run->seats, 1, __ATOMIC_SEQ_CST) < 0)
        {
                atomic_add(address_of run->seats, 1);
                return;
        }

        for (;;)
        {
                parallel_tree_node address_to node;
                b32 word;

                if (atomic_load(address_of run->base.stop) ||
                    atomic_load(address_of run->complete))
                        break;

                if (!parallel_tree_room(run))
                {
                        word = atomic_load(address_of run->base.limit_word);
                        atomic_inc(address_of run->room_waiters);

                        if (!parallel_tree_room(run) &&
                            !atomic_load(address_of run->base.stop) &&
                            !atomic_load(address_of run->complete))
                                thread_wait(address_of run->base.limit_word, word);

                        atomic_dec(address_of run->room_waiters);
                        continue;
                }

                node = atomic_load(address_of run->pending_count)
                               ? parallel_tree_claim(run, null)
                               : null;

                if (node)
                {
                        parallel_tree_enter(run, node);
                        continue;
                }

                word = atomic_load(address_of run->base.finished_word);
                atomic_inc(address_of run->progress_waiters);

                if (!atomic_load(address_of run->pending_count) &&
                    !atomic_load(address_of run->base.stop) &&
                    !atomic_load(address_of run->complete))
                        thread_wait(address_of run->base.finished_word, word);

                atomic_dec(address_of run->progress_waiters);
        }

        atomic_add(address_of run->seats, 1);
}

static fn parallel_tree_publish_maybe(parallel_tree_run address_to run,
                                      bool address_to published, bool caller_only)
{
        if (address_to published || caller_only ||
            atomic_load(address_of run->entered) < PARALLEL_TREE_INLINE_NODES ||
            !atomic_load(address_of run->pending_count))
                return;

        address_to published = true;

        if (run->seats > 0 && parallel_ready())
                parallel_publish(address_of run->base);
}

//      The emitter's wait for a node to reach a state, running what it can.
static bool parallel_tree_await(parallel_tree_run address_to run,
                                parallel_tree_node address_to node, b32 wanted,
                                bool address_to published, bool caller_only)
{
        for (;;)
        {
                parallel_tree_node address_to other;
                b32 word;

                if (atomic_load(address_of node->state) >= wanted)
                        return true;

                if (atomic_load(address_of run->base.stop))
                        return false;

                if (atomic_load(address_of node->state) == PARALLEL_TREE_PENDING &&
                    (other = parallel_tree_claim(run, node)) != null)
                {
                        parallel_tree_enter(run, other);
                        parallel_tree_publish_maybe(run, published, caller_only);
                        continue;
                }

                if (parallel_tree_room(run) && atomic_load(address_of run->pending_count) &&
                    (other = parallel_tree_claim(run, null)) != null)
                {
                        parallel_tree_enter(run, other);
                        parallel_tree_publish_maybe(run, published, caller_only);
                        continue;
                }

                word = atomic_load(address_of run->base.finished_word);
                atomic_inc(address_of run->progress_waiters);

                if (atomic_load(address_of node->state) < wanted &&
                    !atomic_load(address_of run->base.stop) &&
                    !(parallel_tree_room(run) && atomic_load(address_of run->pending_count)))
                        thread_wait(address_of run->base.finished_word, word);

                atomic_dec(address_of run->progress_waiters);
        }
}

static fn parallel_tree_release(parallel_tree_run address_to run,
                                parallel_tree_node address_to node)
{
        if (node->handle >= 0 && node->parent)
        {
                lock_take(address_of run->guard);
                parallel_tree_close(run, node);
                lock_release(address_of run->guard);
        }

        if (node->held)
        {
                atomic_sub(address_of run->held_nodes, 1);
                atomic_sub(address_of run->held_bytes,
                           node->output.used + node->tail.used);
        }

        memory_give(node->output.bytes);
        memory_give(node->tail.bytes);
        memory_give(node);
}

static parallel_tree_node address_to parallel_tree_record(address_any user,
                                                          string_address name,
                                                          positive length)
{
        parallel_tree_node address_to node =
                memory_take(sizeof(parallel_tree_node) + length + 1);

        if (!node)
                return null;

        memory_fill(node, 0, sizeof(parallel_tree_node));
        node->magic = PARALLEL_TREE_MAGIC;
        node->user = user;
        node->handle = -1;
        node->output.heap = 1;
        node->tail.heap = 1;
        node->name_length = (p32)length;
        memory_copy(node->name, name, length);
        node->name[length] = 0;

        return node;
}

static parallel_tree_node address_to parallel_tree_adopt(parallel_output address_to output,
                                                         string_address name,
                                                         address_any node)
{
        parallel_tree_node address_to parent = (parallel_tree_node address_to)(address_any)output;
        parallel_tree_node address_to child;

        if (parent->magic != PARALLEL_TREE_MAGIC || parent->leaf ||
            atomic_load(address_of parent->state) != PARALLEL_TREE_ENTERING)
                return null;

        child = parallel_tree_record(node, name, string_length(name));

        if (!child)
        {
                parallel_stop();
                return null;
        }

        child->parent = parent;
        child->offset = parent->output.used;

        if (parent->last_child)
                parent->last_child->sibling = child;
        else
                parent->first_child = child;

        parent->last_child = child;
        parent->unstarted++;
        parent->unfinished++;

        return child;
}

pub bool parallel_child(parallel_output address_to output, string_address name,
                        address_any node)
{
        return parallel_tree_adopt(output, name, node) != null;
}

pub bool parallel_leaf(parallel_output address_to output, parallel_node_job job,
                       address_any node)
{
        parallel_tree_node address_to leaf =
                parallel_tree_adopt(output, (string_address)"", node);

        if (!leaf)
                return false;

        leaf->leaf = 1;
        leaf->job = job;

        return true;
}

//      Every record below first and its siblings, children first, each
//      finished to the sink so the caller can free its node.
static fn parallel_tree_discard(parallel_tree_run address_to run,
                                parallel_tree_node address_to first)
{
        parallel_tree_node address_to address_to stack = null;
        positive room = 0;
        positive used = 0;
        parallel_tree_node address_to node;

        for (node = first; node; node = node->sibling)
        {
                if (used == room)
                {
                        positive grown_room = room ? room * 2 : 64;
                        parallel_tree_node address_to address_to grown =
                                memory_resize(stack, grown_room * sizeof(address_any));

                        if (!grown)
                                break;

                        stack = grown;
                        room = grown_room;
                }

                stack[used++] = node;
        }

        while (used)
        {
                parallel_tree_node address_to top = stack[used - 1];

                if (top->first_child)
                {
                        parallel_tree_node address_to child = top->first_child;

                        top->first_child = null;

                        for (; child; child = child->sibling)
                        {
                                if (used == room)
                                {
                                        positive grown_room = room * 2;
                                        parallel_tree_node address_to address_to grown =
                                                memory_resize(stack, grown_room * sizeof(address_any));

                                        if (!grown)
                                                break;

                                        stack = grown;
                                        room = grown_room;
                                }

                                stack[used++] = child;
                        }

                        continue;
                }

                used--;
                run->sink(run->context, top->user, null, 0, true);
                parallel_tree_release(run, top);
        }

        memory_give(stack);
}

pub bool parallel_tree(parallel_node_job enter, parallel_node_job leave,
                       parallel_node_sink sink, address_any context,
                       bipolar root_directory, address_any root_node,
                       positive open_flags)
{
        parallel_tree_run run = {0};
        parallel_tree_frame address_to frames = null;
        positive frames_room = 0;
        positive depth = 0;
        positive limits[2] = {0, 0};
        positive freed = 0;
        positive threads;
        thread address_to self = thread_self();
        address_any outer = self->run;
        bool caller_only = outer != null || parallel_width() == 1;
        bool published = false;
        parallel_tree_node address_to root;

        run.base.work = parallel_tree_work;
        run.enter = enter;
        run.leave = leave;
        run.sink = sink;
        run.context = context;
        run.root_directory = root_directory;
        run.open_flags = open_flags;

        system_call_4(syscall(prlimit64), 0, 7, 0, (positive)limits);
        run.cap = limits[0] / 2;

        if (run.cap < 16)
                run.cap = 16;

        if (run.cap > (1u << 20))
                run.cap = 1u << 20;

        threads = (run.cap - 1) / 3;

        if (threads > parallel_width())
                threads = parallel_width();

        run.seats = (b32)(threads ? threads - 1 : 0);
        run.held_nodes_limit = PARALLEL_TREE_HELD_NODES + 256 * parallel_width();

        root = parallel_tree_record(root_node, (string_address)"", 0);

        if (!root)
                return false;

        frames = memory_take(64 * sizeof(parallel_tree_frame));

        if (!frames)
        {
                memory_give(root);
                return false;
        }

        frames_room = 64;
        root->state = PARALLEL_TREE_ENTERING;
        frames[depth++] = (parallel_tree_frame){root, null, 0, 0};

        self->run = address_of run.base;
        parallel_tree_enter(address_of run, root);

        while (depth && !atomic_load(address_of run.base.stop))
        {
                parallel_tree_frame address_to top = address_of frames[depth - 1];
                parallel_tree_node address_to node = top->node;

                if (top->stage == 0)
                {
                        if (!parallel_tree_await(address_of run, node, PARALLEL_TREE_ENTERED,
                                                 address_of published, caller_only))
                                break;

                        top->cursor = node->first_child;
                        top->offset = 0;
                        top->stage = 1;
                }

                if (top->stage == 1)
                {
                        parallel_tree_node address_to child = top->cursor;
                        positive until = child ? child->offset : node->output.used;

                        if (atomic_load(address_of run.base.stop))
                                break;

                        if (until > top->offset &&
                            !sink(context, node->user, node->output.bytes + top->offset,
                                  until - top->offset, false))
                        {
                                parallel_run_stop(address_of run.base);
                                break;
                        }

                        top->offset = until;

                        if (child)
                        {
                                top->cursor = child->sibling;

                                if (depth == frames_room)
                                {
                                        parallel_tree_frame address_to grown = memory_resize(
                                                frames, frames_room * 2 * sizeof(parallel_tree_frame));

                                        if (!grown)
                                        {
                                                //      The child is not on the stack yet: put
                                                //      it back where the cleanup will find it.
                                                top->cursor = child;
                                                parallel_run_stop(address_of run.base);
                                                break;
                                        }

                                        frames = grown;
                                        frames_room *= 2;
                                }

                                frames[depth++] = (parallel_tree_frame){child, null, 0, 0};
                                continue;
                        }

                        top->stage = 2;
                }

                if (!parallel_tree_await(address_of run, node, PARALLEL_TREE_COMPLETE,
                                         address_of published, caller_only))
                        break;

                if (atomic_load(address_of run.base.stop))
                        break;

                if (node->tail.used &&
                    !sink(context, node->user, node->tail.bytes, node->tail.used, false))
                {
                        parallel_run_stop(address_of run.base);
                        break;
                }

                depth--;

                if (!sink(context, node->user, null, 0, true))
                        parallel_run_stop(address_of run.base);

                parallel_tree_release(address_of run, node);
                parallel_tree_wake_room(address_of run);

                if (++freed % PARALLEL_TREE_SHARE_EVERY == 0 && threads_live)
                        allocator_shelves_share();
        }

        atomic_exchange(address_of run.complete, 1);
        parallel_tree_wake_progress(address_of run);
        parallel_tree_wake_room(address_of run);

        if (published && parallel_pool.run == address_of run.base)
                parallel_withdraw();

        self->run = address_of run.base;

        //      A stopped walk: every record still standing is finished to the
        //      sink, deepest first, so the caller can free what it gave.
        while (depth)
        {
                parallel_tree_frame address_to top = address_of frames[--depth];
                parallel_tree_node address_to node = top->node;
                parallel_tree_node address_to rest =
                        top->stage ? top->cursor
                                   : (atomic_load(address_of node->state) >= PARALLEL_TREE_ENTERED
                                              ? node->first_child
                                              : null);

                parallel_tree_discard(address_of run, rest);
                sink(context, node->user, null, 0, true);
                parallel_tree_release(address_of run, node);
        }

        self->run = outer;
        memory_give(frames);

        if (threads_live)
                allocator_shelves_share();

        parallel_tree_last.cap = run.cap;
        parallel_tree_last.open_most = run.open_most;
        parallel_tree_last.evictions = run.evictions;
        parallel_tree_last.reopens = run.reopens;
        parallel_tree_last.threads = published ? threads : 1;

        return !atomic_load(address_of run.base.stop);
}
#endif // LIBRARY_THREAD_RUNTIME

#endif // LIBRARY_COMMON_ALLOCATOR
