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
/* Read a whole file into reusable owned storage. Procfs may return short
   reads before EOF, so this is deliberately a loop rather than file_slurp. */
static HOT bool file_store_slurp(string_address path,
                                 byte_store address_to store)
{
        bipolar handle = system_open_at(AT_FDCWD, path,
                                        FILE_READ | O_CLOEXEC);

        if (handle < 0)
                return false;

        store->used = 0;
        while (store->used <= positive_max - 4097 &&
               byte_store_reserve(store, store->used + 4097, 4096))
        {
                bipolar got = system_read_retry(
                    (positive)handle, store->bytes + store->used,
                    store->room - store->used - 1);

                if (got < 0)
                        break;
                if (!got)
                {
                        store->bytes[store->used] = end;
                        system_close(handle);
                        return true;
                }

                store->used += (positive)got;
        }

        system_close(handle);
        return false;
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

/* Regex, glob and tr share the [:name:] submachine.  A null limit means the
   surrounding string's terminator is the bound. */
static PURE inline INLINE string_address byte_class_end(string_address text,
                                                         string_address limit)
{
        if (text[0] != '[' || text[1] != ':')
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

#endif
