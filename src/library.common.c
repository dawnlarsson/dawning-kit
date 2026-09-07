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

/* One-shot I/O stays visibly distinct from the EINTR-retrying helpers. */
#define system_read_once(handle, into, length)                               \
        system_call_3(syscall(read), (positive)(handle), (positive)(into),   \
                      (positive)(length))

#define system_write_once(handle, data, length)                              \
        system_call_3(syscall(write), (positive)(handle), (positive)(data),  \
                      (positive)(length))

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

#define system_duplicate(from, to, flags)                                    \
        system_call_3(syscall(dup3), (positive)(from), (positive)(to),       \
                      (positive)(flags))

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

#define system_remove_at(directory, path, flags)                             \
        system_call_3(syscall(unlinkat), (positive)(bipolar)(directory),     \
                      (positive)(path), (positive)(flags))

/* The common moving byte store.  Naming the three words once also names the
   only correct reserve/release argument order; subsystems keep semantic
   typedefs without rebuilding either operation around them. */
typedef struct
{
        p8 address_to bytes;
        positive room;
        positive used;
} byte_store;

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
/* Read through EOF into reusable storage, leaving the descriptor open.
   Preserve raw read errors; -12 is ENOMEM at the Linux syscall boundary. */
static HOT bipolar file_store_read(positive handle, byte_store address_to store)
{
        store->used = 0;
        while (store->used <= positive_max - 4097 &&
               byte_store_reserve(store, store->used + 4097, 4096))
        {
                bipolar got = system_read_retry(
                    handle, store->bytes + store->used,
                    store->room - store->used - 1);

                if (got < 0)
                        return got;
                if (!got)
                {
                        store->bytes[store->used] = end;
                        return 0;
                }

                store->used += (positive)got;
        }

        return -12;
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

/* printf, scanf and seq have one flag grammar. */
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

/* The bounded form is for a format that is a counted run rather than a
   string, awk's, whose bytes after the run are whatever the value store
   holds next; the flag bytes are never the terminator, so the plain form
   is the same walk with no bound to reach. */
static inline INLINE positive conversion_flags_take_max(
    string_address address_to source, positive length)
{
        string_address at = address_to source;
        positive flags = 0;

        // Counted down rather than compared against an end address: the
        // unbounded caller passes the largest count there is, and adding
        // that to a pointer would wrap it.
        while (length)
        {
                p8 byte = string_get(at);
                p8 flag = byte < array_count(conversion_flag_bytes)
                              ? conversion_flag_bytes[byte] : 0;

                if (!flag)
                        break;

                flags |= flag;
                at++;
                length--;
        }

        address_to source = at;
        return flags;
}

static inline INLINE positive conversion_flags_take(
    string_address address_to source)
{
        return conversion_flags_take_max(source, positive_max);
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
                writer_fill(write, field->padding, field->zero ? '0' : ' ');
        }
        positive skip = !field->left && field->zero ? field->sign : 0;
        write(field->bytes + skip, field->length - skip);
        writer_fill(write, field->zeroes, '0');
        if (field->left) writer_fill(write, field->padding, ' ');
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

/* JSON byte-string policy shared by UUID output and util-linux tables.
   Controls retain the shared \u00xx spelling; quote/backslash use short
   escapes. Printable spans cross the writer once, not once per byte. */
static fn writer_json_string(writer output, string_address value)
{
        positive length = string_length(value);
        if (length <= 20)
        {
                // A short cell, including both quotes, crosses the writer once.
                p8 escaped[122];
                escaped[0] = '"';
                positive2 chunk = memory_into_escaped(escaped + 1, value, length,
                                                       sizeof(escaped) - 2, 64);
                escaped[chunk.y + 1] = '"';
                output(escaped, chunk.y + 2);
                return;
        }
        output("\"", 1);
        writer_hex_escaped(output, value, length, 64);
        output("\"", 1);
}

/* Select byte indexes from a comma-separated list of named records.  Every
   schema keeps its name pointer first; stride lets tables retain the rest of
   their private shape.  The caller may seed a default prefix before an
   append-form list and chooses the few syntax policies that differ. */
#define NAME_LIST_CASE_SENSITIVE 1
#define NAME_LIST_UNIQUE 2
#define NAME_LIST_REJECT_TRAILING 4
static COLD bool name_list_select(
    string_address text, const void address_to definitions, positive stride,
    positive definition_count, p8 address_to selected,
    positive address_to selected_count, positive maximum, p8 policy)
{
        if (definition_count > 256 || stride < sizeof(string_address) ||
            address_to selected_count > maximum)
                return false;

        while (*text)
        {
                string_address comma = string_first_of_or_end(text, ',');
                positive length = (positive)(comma - text);
                positive found = definition_count;

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
                        return false;
        }

        return address_to selected_count != 0;
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

#endif
