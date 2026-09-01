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

#define byte_store_reserve(store, wanted, step)                              \
        ({ __auto_type _byte_store = (store);                                \
           array_store_reserve(_byte_store->bytes, _byte_store->room,         \
                               _byte_store->used, (wanted), (step)); })

#define byte_store_release(store)                                            \
        ({ __auto_type _byte_store = (store);                                \
           array_store_release(_byte_store->bytes, _byte_store->room,         \
                               _byte_store->used); })

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

static const p8 conversion_flag_bytes[128] = {
    ['-'] = CONVERSION_FLAG_LEFT, ['+'] = CONVERSION_FLAG_PLUS,
    [' '] = CONVERSION_FLAG_SPACE, ['#'] = CONVERSION_FLAG_ALTERNATE,
    ['0'] = CONVERSION_FLAG_ZERO,
};

static inline INLINE positive conversion_flags_take(
    string_address address_to source)
{
        string_address at = address_to source;
        positive flags = 0;

        while (true)
        {
                p8 byte = string_get(at);
                p8 flag = byte < 128 ? conversion_flag_bytes[byte] : 0;

                if (!flag)
                        break;

                flags |= flag;
                at++;
        }

        address_to source = at;
        return flags;
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
