/*
        Experimental C standard library

        The rest of <string.h>: duplication, tokenising, bounded joins

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_TEXT
#define STANDARD_MODERN_C_STANDARD_TEXT

/*
        Guarded out of the kernel build and out of a no-platform build. core.c
        includes this umbrella and library.c sets KERNEL_MODE from __MODULE__,
        so without this the module would pull in a second struct stat, a second
        open and a second errno beside the ones <linux/...> already declares.
        The three families that shipped without this guard were each correct in
        isolation and wrong together; the ones that had it were right.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        This is ordinary C, and it is here rather than in library.c for the
        reason src/net/netlink.c gives: library.c holds declarations and
        __asm__ blocks and nothing else, and that is checked. Nothing below is
        a floor. Every routine here is a policy decision sitting on top of
        scans that are already assembly on all three machines -- where a
        token ends, what a bound does to a copy that does not fit, whether an
        empty run between two delimiters is a token or is nothing. Those are
        arguments about behaviour, they are the same argument on every
        machine, and writing them three times in assembly would be writing the
        same disagreement three times.

        So the searching is borrowed and only the deciding is written:

          string_length, string_length_max          how far a string goes
          string_span_of_set                        strspn, a 256 bit set
          string_span_without_set                   strcspn, the same set
          memory_copy_apart                         memcpy, halves that do not touch
          memory_search_ascii_case                  memmem under a case fold

        The three set-driven ones are what makes this file short. A tokeniser
        written the obvious way walks the delimiter string once per input
        byte, which is O(n*m) and not slower by a constant -- splitting a
        kilobyte line on " \t\n" walks three bytes a thousand times. The
        assembly builds a bitmap of the set once and then answers each byte
        with a shift and a bit test, so strtok over a line costs the line
        plus the delimiters, once each, however many tokens come out.
*/

/*
        setjmp lives next door and is pulled in from here.

        It has nothing to do with strings. It is in this file's orbit because
        it is the one routine in the structural half of the library that
        cannot be written in C at all, so it is assembly in a platform .inc,
        and it needed a file that was already opting in to a compatibility
        layer to carry it. If it later earns a place inside library.c's own
        include graph, move the line rather than the file: the .inc guards
        itself and does not care which side it is read from.
*/
#include "../platform/setjmp.inc"

/*
        Where the memory comes from, and the one dependency this file has that
        is not already assembly.

        strdup and strndup are the only two routines in <string.h> that
        allocate, and there is no way to write either without saying who
        allocates. malloc is the allocator family's, declared here rather than
        included so that the two files can be merged in either order; the
        declaration and the definition have to agree on the spelling of the
        size, which is positive on every target this builds for.

        A program that links this file therefore needs the allocator, whether
        or not it calls strdup: the reference is emitted for the whole
        translation unit and the link fails on the undefined symbol even when
        nothing reaches it. That is the ordinary libc arrangement and it is
        only worth saying because these two files can arrive separately.
*/
address_any malloc(positive size);

/*
        strdup: the string again, somewhere it will outlive its source.

        The length is measured once and the terminator is copied with the
        rest, which is why the copy is length + 1 and not length followed by a
        store. memory_copy_apart rather than memory_copy because a fresh
        allocation cannot overlap the source it is being filled from, and that
        promise is the whole difference between the two names.

        A null answer is an allocation that failed, and it is the caller's to
        notice. There is no errno here to set.
*/
string_address string_duplicate(string_address source)
{
        positive size = string_length(source) + 1;
        string_address copy = (string_address)malloc(size);

        if (copy == null)
                return null;

        memory_copy_apart(copy, source, size);

        return copy;
}

/*
        strndup, whose bound is on the source and not on the answer.

        At most bound bytes are taken, and a terminator is always written, so
        the block is one byte longer than the bound when the source fills it.
        That asymmetry is the whole of the routine and it is the thing people
        get wrong: strndup(s, 4) on "hello" allocates five bytes, not four.

        string_length_max stops at the bound or at the terminator, whichever
        comes first, so a source shorter than the bound costs its own length
        and the block is exactly as short as it should be. It also means the
        bytes past the terminator are never read, which matters when the bound
        is larger than the buffer the string sits in -- strnlen's licence, and
        the reason this cannot just call string_length and clamp.
*/
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

/*
        strtok_r, and the empty-token rule that is the entire reason it and
        strsep are both in the header.

        This one skips runs of delimiters. "a,,b" split on "," is two tokens,
        not three, and a string that is nothing but delimiters yields none at
        all. strsep below does the opposite on the same input and neither is
        the mistaken version of the other: a tokeniser wants "two spaces are
        one space", and a field parser wants ",," to mean a field that was
        left blank. Reaching for the wrong one is how a CSV reader silently
        drops a column.

        The scan is two spans and no byte loop. string_span_of_set measures
        the leading run of delimiters, which is what gets skipped;
        string_span_without_set measures the run that is not delimiters, which
        is the token. Both stop at the terminator without being told to,
        because the set is built from a C string and the build loop stops at
        its own terminator, so bit zero is never set and the source's end is
        never a member.

        Where the token ended matters twice. If it ended at the terminator the
        input is exhausted and the saved position is that terminator, so the
        next call finds an empty string and answers nothing -- and, since the
        string ends there, no byte is written into the caller's buffer. If it
        ended at a delimiter, that delimiter becomes the terminator of the
        token just handed back, which is why strtok destroys its input, and
        the saved position is the byte after it.

        A first call with a null source and nothing saved answers nothing.
        glibc dereferences the null there and the program stops; a routine
        that cannot be given a starting point has no answer to give, and
        returning it is more useful than a fault.
*/
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

        if (address_to stop == end)
        {
                address_to saved = stop;
                return start;
        }

        address_to stop = end;
        address_to saved = stop + 1;

        return start;
}

/*
        Where strtok keeps the place strtok_r is handed.

        One pointer in .bss, shared by every caller, which is what makes
        strtok the function nobody may call from two places at once and
        strtok_r the function they should have called instead. It is a plain
        static and not thread-local on purpose: a freestanding program built
        with no startup code has no thread pointer set up, so __thread here
        would fault on the first call rather than be safe.
*/
static string_address text_token_place = null;

// strtok: strtok_r with the one place in this file standing in for the
// caller's, and every hazard that implies.
string_address string_token(string_address source, string_address delimiters)
{
        return string_token_next(source, delimiters, address_of text_token_place);
}

/*
        strsep, which keeps the empty fields strtok throws away.

        The holder is read, advanced past the token, and written back, so the
        caller's pointer walks the string and reaching null is how the walk
        ends. A delimiter found is overwritten with a terminator and the
        holder lands on the byte after it -- so two delimiters in a row give a
        second call whose token is the empty string, which is the point.

        When no delimiter is found the whole remainder is the token and the
        holder becomes null rather than pointing at the terminator. That is
        what stops the loop: a caller writes while ((field = strsep(&line,
        ","))) and the null it eventually gets is the end, not a field.

        An empty delimiter set is not a special case here and needs no test
        for one. string_span_without_set with nothing in the set runs to the
        terminator, the byte there is the terminator, so the holder becomes
        null and the whole string comes back once.
*/
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

/*
        strcasestr: string_search with the two spellings of a letter counted
        as one.

        memory_search_ascii_case already is this search over counted blocks,
        with the rarest-folded-byte anchor and the vector hunt, so all that is
        left is to hand it lengths.

        Which is the whole of the difficulty. It is a counted search and
        wants a length, and the haystack arrives as a C string with none.
        Measuring the whole of it first is the obvious way to supply one and
        it is the wrong one: a needle sitting at offset zero of a megabyte
        then costs a megabyte read to find something at the first byte. That
        is not a small constant. Measured on x86_64, two thousand such
        searches took 66.7 million cycles that way, and glibc, whose
        strcasestr has no length to take, took 1.56 million.

        So the haystack is measured a window at a time and searched as it is
        measured, and the walk stops at the first window that holds the
        needle or at the first that holds the terminator. A window that came
        back short of its bound is the end of the string and there is nowhere
        further to look. A window that filled its bound is followed by
        another, starting a needle short of where this one ended so that a
        match lying across the seam is inside exactly one of them, and each
        window is twice the last so that a long haystack reaches its end in a
        logarithmic number of calls rather than a linear one.

        The first window is a kilobyte past the needle, which is chosen so
        that the ordinary case pays nothing for any of this: a haystack
        shorter than that is one string_length_max and one search, exactly
        what the single measurement was. What it buys at the other end,
        measured the same way: on x86_64 the early match falls from 66.7
        million cycles to 1.42 million, and the search that runs to the end
        of a megabyte without matching stays where it was, 150.7 million
        cycles against 146.6.

        That second figure is the one worth doubting, because the
        overlapping windows walk the haystack about twice over, so it was
        taken on all three rather than on the one. Wall clock against the
        single measurement, x86_64 native and the other two under qemu: 1.03,
        0.99 and 0.97 times -- no direction to it. The extra bytes do not
        show because the length scan and the search are both wide and the
        search is where the time was. glibc on that same input, on x86_64,
        takes 6.08 thousand million cycles.

        An empty needle sits at the front of the haystack. That is what
        memmem answers, what strstr answers, and what memory_search_ascii_case
        answers for a zero length needle, so there is no disagreement to
        resolve and no branch here to resolve it -- except that measuring any
        part of a haystack for a needle that cannot fail is work, and the
        seam arithmetic below is written for a needle with a length in it, so
        the empty case leaves before the walk starts rather than being
        reasoned about inside it.

        The fold is ASCII and nothing else. A byte with the high bit set
        compares as itself, so this finds nothing a locale would find and
        nothing it would not; see the note on string_compare_folded for why
        the fold goes down to lower case rather than up.
*/
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

/*
        strlcpy and strlcat, and why they are not called _max like everything
        else with a bound.

        string_copy_max is strncpy and string_append_max is strncat, and both
        of those take the bound to mean something these two do not.
        strncpy(d, s, n) writes no terminator when the source filled n, and
        pads the rest of n with zeros when it did not. strlcpy(d, s, n) treats
        n as the size of the buffer, always leaves a terminator inside it, and
        pads nothing. The bound counts a different thing in each pair, so a
        shared suffix would be a lie in one of them; _bounded means the buffer
        and _max means the count, and the two names are as far apart as the
        behaviours.

        The answer is the length the result wanted, not the length it got.
        That is the whole point of the pair: a caller compares it against the
        capacity and knows whether anything was lost, which strncpy cannot
        tell it at all and snprintf tells it the same way. It does mean the
        source is measured in full even when almost none of it is copied, and
        that is the documented cost of the interface.

        A capacity of zero touches the destination not at all, so a null
        destination with a zero capacity is a legal way to ask only how long
        the source is.
*/
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

/*
        strlcat, whose one real decision is what to do with a destination that
        holds no terminator inside the capacity.

        string_length_max stops at the capacity, so a destination that is not
        a string within its own buffer measures exactly capacity and is
        detected by that equality. BSD's answer, and glibc's since 2.38, is to
        return capacity plus the source length and write nothing: there is no
        end to append to, and guessing one would write past a buffer whose
        real extent nobody here knows. The answer is still larger than the
        capacity, so the caller's truncation test is right for the wrong
        reason and the caller still finds out.

        Otherwise the room left is capacity minus what is held, which is at
        least one because the equality above was false, so the subtraction
        below it cannot wrap. The result length reported is what was held plus
        the whole source, again the length that was wanted rather than the one
        that fit.
*/
positive string_append_bounded(string_address destination,
                               string_address source, positive capacity)
{
        positive held = string_length_max(destination, capacity);
        positive length = string_length(source);
        positive room;
        positive fits;

        if (held == capacity)
                return capacity + length;

        room = capacity - held;
        fits = length < room ? length : room - 1;

        memory_copy_apart(destination + held, source, fits);
        destination[held + fits] = end;

        return held + length;
}

/*
        And the names a C program knows these by.

        Second labels rather than forwarders, exactly as library.c attaches
        strlen to string_length: a .set is one address with two names and a
        call through either is the same call. The alternative -- an alias
        attribute on a C declaration -- would say the same thing to the
        linker and would additionally hand gcc a prototype that disagrees with
        its own builtin about whether a string is char or p8, which is a
        warning per name and no benefit.

        Nothing here declares strdup or strtok to C. Code in this tree calls
        the prose names; code ported in gets the symbol at link time and its
        own header's prototype at compile time, which is the arrangement every
        other libc name in this library already has.
*/
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
