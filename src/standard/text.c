/*
        Experimental C standard library

        The rest of <string.h>: duplication, tokenising, bounded joins

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_TEXT
#define STANDARD_MODERN_C_STANDARD_TEXT

/* Userspace compatibility: exclude kernel and no-platform builds. */
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/* C string policy composes the shared assembly scans and copies. */

/* Non-local jumps share this userspace compatibility include. */
#include "../platform/setjmp.inc"

/* Duplication requires the allocator; its definition may follow this file. */
address_any malloc(positive size);

/* strdup measures once and copies the terminator into fresh storage.
   A failed allocation returns null. */
string_address string_duplicate(string_address source)
{
        positive size = string_length(source) + 1;
        string_address copy = (string_address)malloc(size);

        if (copy == null)
                return null;

        memory_copy_apart(copy, source, size);

        return copy;
}

/* strndup reads at most bound source bytes and always appends a terminator.
   The allocation is min(source length, bound) + 1 bytes. */
string_address string_duplicate_max(string_address source, positive bound)
{
        positive length = string_length_max(source, bound);
        string_address copy = (string_address)malloc(length + 1);

        if (copy == null)
                return null;

        memory_copy_apart(copy, source, length);
        copy[length] = end;

        return copy;
}

/* strtok_r skips empty fields, replaces a delimiter with NUL, and saves the
   next position. At exhaustion it saves the existing terminator without
   writing it. A null source resumes saved state; no saved state returns null. */
string_address string_token_next(string_address source,
                                 string_address delimiters,
                                 string_address address_to saved)
{
        string_address start;
        string_address stop;

        if (source == null)
                source = address_to saved;

        if (source == null)
                return null;

        source += string_span_of_set(source, delimiters);

        if (address_to source == end)
        {
                address_to saved = source;
                return null;
        }

        start = source;
        stop = source + string_span_without_set(source, delimiters);

        if (address_to stop != end)
                address_to stop++ = end;
        address_to saved = stop;

        return start;
}

/* strtok shares one cursor. No TLS: freestanding startup may lack a thread
   pointer. Concurrent or nested tokenization needs string_token_next. */
static string_address text_token_place = null;

// strtok: strtok_r with the one place in this file standing in for the
// caller's, and every hazard that implies.
string_address string_token(string_address source, string_address delimiters)
{
        return string_token_next(source, delimiters, address_of text_token_place);
}

/* strsep preserves empty fields. It replaces a delimiter with NUL and
   advances the holder; without a delimiter it returns the remaining string
   once and clears the holder. An empty delimiter set therefore yields one field. */
string_address string_split_next(string_address address_to holder,
                                 string_address delimiters)
{
        string_address start = address_to holder;
        string_address stop;

        if (start == null)
                return null;

        stop = start + string_span_without_set(start, delimiters);

        if (address_to stop != end)
        {
                address_to stop = end;
                stop += 1;
        }
        else
        {
                stop = null;
        }

        address_to holder = stop;

        return start;
}

/* ASCII-only strcasestr; high bytes compare unchanged. An empty needle
   returns the haystack. Grow bounded windows with needle_length - 1 overlap
   so seam-spanning matches survive and an early match avoids a full strlen.
   Windows start 1 KiB beyond the needle and double up to the 1 MiB threshold.
   Native x86 early-match measurements improved from 66.7M to 1.42M cycles;
   full misses remained within 3% on x86 and emulated ARM64/RV64. */
#define TEXT_SEARCH_WINDOW 1024

PURE string_address string_search_folded(string_address haystack,
                                         string_address needle)
{
        positive needle_length = string_length(needle);
        positive window;
        positive base = 0;

        if (needle_length == 0)
                return haystack;

        window = needle_length + TEXT_SEARCH_WINDOW;

        for (;;)
        {
                positive have = string_length_max(haystack + base, window);
                address_any found = memory_search_ascii_case(
                        haystack + base, have, needle, needle_length);

                if (found)
                        return (string_address)found;

                if (have < window)
                        return null;

                base += window - (needle_length - 1);

                if (window < ((positive)1 << 20))
                        window += window;
        }
}

/* strlcpy takes destination capacity, writes at most capacity - 1 bytes
   plus NUL, and returns the full source length. Zero capacity permits a null
   destination. Unlike the _max APIs, this bound includes the terminator. */
positive string_copy_bounded(string_address destination, string_address source,
                             positive capacity)
{
        positive length = string_length(source);

        if (capacity != 0)
        {
                positive fits = length < capacity ? length : capacity - 1;

                memory_copy_apart(destination, source, fits);
                destination[fits] = end;
        }

        return length;
}

/* strlcat returns the desired total length. An unterminated destination
   within capacity is untouched and returns capacity + source length. */
// Keep the shared copy body inline: an extra call loses on short strings.
FLAT positive string_append_bounded(string_address destination,
                               string_address source, positive capacity)
{
        positive held = string_length_max(destination, capacity);

        if (held == capacity)
                return capacity + string_length(source);

        return held + string_copy_bounded(destination + held, source,
                                           capacity - held);
}

/* Libc symbols alias the shared bodies. Imported callers provide their own
   char-based prototypes; internal callers use the string_address APIs. */
__asm__(
    ASM_ALIAS(strdup,     string_duplicate)
    ASM_ALIAS(strndup,    string_duplicate_max)
    ASM_ALIAS(strtok,     string_token)
    ASM_ALIAS(strtok_r,   string_token_next)
    ASM_ALIAS(strsep,     string_split_next)
    ASM_ALIAS(strcasestr, string_search_folded)
    ASM_ALIAS(strlcpy,    string_copy_bounded)
    ASM_ALIAS(strlcat,    string_append_bounded)
);

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_TEXT
