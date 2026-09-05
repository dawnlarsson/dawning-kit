/*
        Experimental C standard library

        The small routines a C program expects to find, floored

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_PLATFORM_STANDARD
#define STANDARD_MODERN_C_PLATFORM_STANDARD

/*
        Classifying a byte, and the two that change its case.

        These are the ones a C library spends its time in and the ones every
        implementation writes as a table lookup: a 256-entry array of flag
        bits, indexed by the character, one load and one test. That is a
        cache line touched to answer a question about a range.

        Every one of them below is arithmetic instead, and branchless. A range
        test is a subtract and an unsigned compare -- c - '0' below ten is
        exactly "is a digit", because a byte under '0' wraps to an enormous
        unsigned number and fails the same compare that a byte over '9' fails.
        One subtract, one compare, one set. No memory at all.

        Where two ranges answer one question they are disjoint -- no byte is
        both a digit and a letter -- and disjoint ranges add rather than or.
        x86_64 lets each compare drop its carry straight into the total and
        never builds a second answer byte; arm64 chains the compares through
        the flags with ccmp and spends one cset at the end.

        The case pair uses the answer as a number rather than as a result.
        arm64 and riscv64 shift the one or zero five places, making thirty two
        or zero, and add or subtract that. x86_64 has both answers in hand
        before the comparison resolves and picks between them with a
        conditional move, which is shorter and puts only itself on the chain.
        Neither branches, so nothing is predicted wrongly on text that
        alternates case.

        The argument is an int, and may be EOF, which is -1. Subtracting and
        comparing unsigned handles that with nothing written down: -1 - '0'
        is a very large number and fails every range here.
*/

CONST b32 byte_is_digit(b32 value);
CONST b32 byte_is_upper(b32 value);
CONST b32 byte_is_lower(b32 value);
CONST b32 byte_is_alpha(b32 value);
CONST b32 byte_is_alnum(b32 value);
CONST b32 byte_is_space(b32 value);
CONST b32 byte_is_hexadecimal(b32 value);
CONST b32 byte_to_upper(b32 value);
CONST b32 byte_to_lower(b32 value);

/*
        Absolute value, without a branch and without a comparison.

        The sign of a two's complement number is its top bit repeated, which
        an arithmetic shift produces directly: all ones for a negative, all
        zeros otherwise. Exclusive-or with that flips every bit of a negative
        and leaves a positive alone, and subtracting it adds the one that
        turns a flip into a negation. Three instructions, no branch, so a
        stream of alternating signs costs what a stream of one sign costs.

        Two widths because the C library has two and they are not the same
        function: abs takes an int and labs a long, and reading a 64 bit
        register when an int was passed reads whatever happened to be in the
        top half of it.
*/
CONST b32 absolute_whole(b32 value);
CONST bipolar absolute_wide(bipolar value);

/*
        Square root, which is one instruction everywhere this runs.

        x86-64 has had sqrtsd since SSE2, which is in the baseline; arm64 has
        fsqrt; and riscv has fsqrt.d in the D extension, which this project
        builds for. So the floor here is not an approximation to be tuned but
        the instruction the hardware already has, and a library that called
        anything else would be slower and less accurate at once.

        The absolute value is not quite one instruction on x86, which has no
        scalar fabs: clearing the sign bit through a general register is three
        moves and beats loading a mask constant from memory to and with.

        These are the only two here that take a decimal, and a decimal in a
        signature is refused by the arm64 kernel build whether or not anything
        calls it, so they and their bodies stay out of a kernel build entirely
        -- which is correct anyway, since kernel code may not touch the
        floating point registers without asking first.
*/
#ifndef KERNEL_MODE
decimal square_root(decimal value);
CONST decimal absolute(decimal value);
#endif



/*
        The rest of the classes, which are the same arithmetic seen from a
        different side.

        Printable is one range and graphic is that range without the space at
        the bottom of it, so both are the subtract-and-compare above with
        different constants. Control is the complement inside ASCII, and the
        delete byte sits alone at the top, away from the other thirty two, so
        it is the one class here that needs a second test -- and the two
        cannot both be true at once, which is why x86 adds the carry from the
        second compare to the answer of the first instead of computing two
        flags and or-ing them.

        Punctuation is the one with any depth to it. It is defined by exclusion
        -- graphic and not alphanumeric -- and writing it as its four literal
        ranges is both longer and harder to read than the definition. Written
        as the definition it is three range tests, and because a letter or a
        digit is always graphic, the second and third answers can only ever be
        subtracted from an answer that is already one: the result is graphic
        minus digit minus letter, which is a flag without a mask or a branch.
        That subtraction is only safe because the letter test folds bit five
        rather than clearing it, so no value outside the graphic range can be
        counted as a letter and drive the answer below zero. The exhaustive
        test covers well past the byte to say so.

        Blank is space or tab and nothing else -- the two the standard names,
        not the six that isspace takes.

        ASCII is the top bit, asked about the whole int rather than a byte:
        isascii is the one function here defined for every integer and not
        just for a byte and EOF, so a negative fails it by being an enormous
        unsigned number and everything from 128 up fails it by being too big.
        toascii is the same bit, discarded rather than tested, and it is the
        only one of these that returns a byte instead of an answer.
*/
CONST b32 byte_is_printable(b32 value);
CONST b32 byte_is_graphic(b32 value);
CONST b32 byte_is_control(b32 value);
CONST b32 byte_is_punctuation(b32 value);
CONST b32 byte_is_blank(b32 value);
CONST b32 byte_is_ascii(b32 value);
CONST b32 byte_to_ascii(b32 value);

/*
        The three set scans, which take the set as a string rather than as a
        table somebody built earlier.

        string_span above is the same question with the set already made: a
        byte per member, and the comment there says why a byte and not a bit.
        That measurement does not carry over, because it assumed the table was
        built once and scanned many times. Here the set arrives as a string and
        has to be turned into something searchable on every single call, and
        the cost of clearing two hundred and fifty six bytes to answer a
        question about four is the whole expense of the routine. Thirty two
        bytes clear in four stores. So the trade inverts: a bit per member,
        four sixty four bit words on the stack, and the shift and mask that
        string_span refused to pay are paid once per input byte instead.

        The alternative every naive implementation reaches for is the nested
        loop, which walks the set again for each byte of the source. That is
        O(n*m) and it is not slower by a constant: strcspn(line, " \t\n") over
        a kilobyte walks the three-byte set a thousand times.

        Three things fall out of the bitmap rather than being written:

        The set's terminator is not a member, because the build loop stops at
        it and never sets bit zero. So for the run of bytes that ARE in the
        set, the source's terminator ends the run by itself: bit zero is clear,
        the test fails, and there is no separate compare against the end.

        For the two that want the run of bytes that are NOT in the set, bit
        zero is set before the build loop instead of cleared, which puts the
        terminator into the stopping set. Again no second test: the loop stops
        at the first rejected byte or at the end, whichever comes first, and
        one bit is the difference between the two routines.

        And the shift count needs no masking anywhere. x86_64's shl and bt with
        a register count, arm64's lsl and lsr by register, and RV64's sll and
        srl all take the low six bits of the count for a sixty four bit
        operand, so a byte of 0..255 selects the bit inside its word by itself
        and only the word index is computed. That is the same one instruction
        saved on all three.

        The four words live on the stack and not in registers. Selecting one of
        four registers by a value only known at run time costs three compares
        and three conditional moves; an aligned load from a line that was
        written a moment ago costs one. No vector register is touched either,
        by design: the kernel builds may not use SSE or NEON, and the RV64
        baseline here has no vector unit at all, so the thirty two bytes are
        cleared with plain stores.

        string_first_of_set answers null when nothing was found, including
        when the set is empty. The terminator is never a member of the set
        however the set string was written, because the build loop stops at
        its own terminator and never sets bit zero, so reaching the end of the
        source is a miss and not a find. That is worth saying beside
        string_first_of, where a NUL is a legal thing to look for and
        string_first_of(source, 0) therefore succeeds at the terminator: this
        routine has no way to ask that question, and never answers it.
*/
PURE positive string_span_of_set(string_address source, string_address accept);
PURE positive string_span_without_set(string_address source, string_address reject);
PURE string_address string_first_of_set(string_address source, string_address accept);

/*
        Comparison with the two spellings of a letter treated as one.

        strcasecmp and strncasecmp, and the C contract is precise about which
        way the fold goes: the bytes are compared after tolower, not after
        toupper. That is not a detail, because six bytes sit between 'Z' and
        'a' -- [ \ ] ^ _ and ` -- and the two directions disagree on every one
        of them. Folding down puts 'Z' at 'z', above '_', and the answer for
        "Z" against "_" is positive; folding up puts it at 'Z', below, and the
        answer is negative. Anything that sorts option names or configuration
        keys sees that sign. memory_compare_ascii_case above folds the other
        way on purpose -- it answers equal-or-not over a counted block and
        never returns an ordering -- so the two are not each other and neither
        should be made to call the other.

        The fold itself is one arithmetic fact. A byte is upper case exactly
        when the byte minus 'A' is below twenty six read as unsigned, which is
        one subtract and one unsigned compare and no second bound; the one or
        zero that falls out, shifted five places, is thirty two or nothing, and
        adding it folds. Four instructions a byte, the same four whether or not
        the byte is a letter, and no table: a two hundred and fifty six byte
        lookup is a cache line fetched to answer what three instructions
        answer. gcc, given the obvious C, instead branches on "is this byte a
        letter" and specialises both sides; its loop is two instructions
        shorter a byte and pays a data-dependent branch for them, and mixed
        text mispredicts it. Measured on a 9950X, best of twenty five, cycles
        a call, the branchless byte loop lost to gcc at every length -- 32
        against 25 at fourteen bytes, 140 against 113 at sixty four -- because
        both are throughput bound on instructions and gcc was issuing fewer.
        A byte loop is not the floor here. Width is.

        So the same fold is done sixteen bytes at a time, and it is the same
        fact again in vector form. Adding 0x3f to every byte moves 'A' through
        'Z' to 0x80 through 0x99, which is the bottom of the signed range and
        nothing else is: a byte below 'A' lands positive, a byte above 'Z'
        lands at 0x9a or higher, and a byte with the high bit set wraps back
        down into the positives. One signed byte compare against 0x9a answers
        the range for sixteen bytes at once, and the mask it makes, anded with
        thirty two and added, folds them. riscv has no vector at the baseline,
        so it does the same range test on eight bytes in an integer register:
        the high bits are masked away first so the two biased adds cannot carry
        between bytes, and 0x80 is left standing in every byte that was a
        letter.

        The wide body never answers. It concludes one of two things -- every
        byte folded-equal and none of them the terminator, so step on, or
        something happened here -- and where something happened it advances
        both pointers to that byte and lets the byte loop below say what. The
        byte loop is therefore the only code that produces a value or stops at
        a terminator, which is why exhausting it exhausts the routine.

        The unbounded form has string_compare's problem exactly: two pointers
        at whatever alignments the caller chose and nothing but a terminator to
        say where they end. It takes string_compare's answer too -- a read of
        sixteen bytes cannot fault when all sixteen are in a page that already
        holds a byte we are allowed to read, and the offset within the page
        says whether they are, so 0xff0 is the last offset at which sixteen
        still fit. Near a boundary it steps sixteen bytes a byte at a time and
        tries again, which happens for at most fifteen bytes in every four
        thousand and ninety six. The bounded form needs none of that: n bounds
        the read, which is the same licence string_compare_max takes, and it
        means these read up to n bytes and not up to the terminator.

        On riscv the answer is different again and it is the one string_compare_max
        gives: an unaligned ld is not part of the RV64I floor, so the word body
        runs only when the two pointers share a residue, after peeling bytes
        until they are aligned. Aligned reads cannot leave their page, so that
        form needs no page argument in either routine.

        Guest instructions a call, counted with kit/insn.c against the obvious
        C at -O2 -fno-builtin, on equal strings that differ in case everywhere:

                                 strcasecmp          strncasecmp
            bytes            gcc     ours          gcc     ours
                1             42       83           48       52
                4             84       83           96      100
                8            140       83          160      164
               16            252       83          288       72
               32            476      114          544       97
               64            924      176         1056      147
              256           3612      548         4128      447

        arm64 and riscv are the same shape: at sixty four bytes arm64 is 855
        against 151 and 1050 against 125, riscv 919 against 297 and 985 against
        286. riscv is the slower of the three because it has no vector at the
        baseline and because its constants cost ten instructions to build,
        which a four byte string cannot amortise -- the same fixed cost
        string_length's block explains, and it stands for the same reason.

        The short end is where these lose and it is worth saying plainly. The
        unbounded form is behind gcc at one byte on every architecture and
        level at four, because it pays for the constants and the first wide
        step before it learns the string was short; the bounded form is behind
        below sixteen, where n forbids the wide step at all. From eight bytes
        up the unbounded form is ahead, and from sixteen the bounded one, by
        three to nine times.

        Against a real libc rather than against gcc: glibc's AVX2 strcasecmp
        is still ahead, 5 cycles against 11 at fourteen bytes and 32 against 58
        at two hundred and fifty six on the same 9950X. The gap is width and
        nothing else -- thirty two bytes a step against sixteen. Sixteen is
        what every x86-64 has without asking a feature byte, without a
        vzeroupper on the way out and without a second body to keep correct,
        and that is the trade this made.
*/
PURE b32 string_compare_folded(string_address source, string_address input);
PURE b32 string_compare_folded_max(string_address source, string_address input,
                                   positive bound);

/*
        The five scans and copies a C library still owed, and the two BSD
        spellings whose arguments are in a different order.

        Every one of them is a length the library already knows how to find
        followed by a copy the library already knows how to make, so none of
        them walks a byte at a time: memory_copy_until asks memory_first_of
        where the stop byte is and hands the span to memory_copy_apart, and
        the three string routines ask string_length or string_length_max and
        do the same. That is two passes over the bytes rather than one, and
        the reasoning above string_copy_max_end is the reasoning here -- a
        fused loop cannot read a word ahead of a store into the same span,
        and a byte loop that can costs a taken branch per byte, which is more
        than a second wide pass costs at every length worth measuring. The
        crossover against the compiler's byte loop is sixteen bytes; the
        block above each routine says where its own is.

        memory_copy_until is memccpy: it stops after the byte it was given
        has been copied and answers the position just past it, or null when
        the byte was not in the span at all, in which case the whole span was
        copied. Source and destination must not overlap, which is what memccpy
        says and what memory_copy_apart assumes.

        string_copy_end is stpcpy, which is string_copy with the useful answer
        instead of the useless one: the destination back is a value the caller
        already had, and where the terminator landed is the value it wanted.

        string_copy_max_endptr is stpncpy, and it is the one with a rule that
        surprises people. It pads: a source shorter than the bound leaves the
        rest of the bound zeroed. It does not terminate: a source that fills
        the bound leaves no terminator at all, and the answer is then one past
        the last byte written rather than a string. Both halves are the same
        answer -- destination plus however much of the source was taken -- so
        there is no branch in the return, only in whether there is padding to
        write. This is deliberately not what string_copy_max does, which pads
        nothing; the two exist side by side because strncpy and stpncpy differ
        in exactly that.

        string_append_max is strncat, which is not the n-limited strncpy in
        any sense: the bound counts source bytes only, the terminator is
        always written, and it goes at destination + bound + 1 in the worst
        case. So it is the walk to the end of the destination followed by
        string_copy_max_end, which is precisely that contract already -- and
        it inherits that routine's precondition, which is that the buffer
        holds bound + 1 bytes past where the append begins.

        memory_zero and memory_copy_source_first exist because bzero and
        bcopy are not renames. bzero takes the size where memset takes the
        fill byte, and bcopy takes the source first and is specified to
        survive overlap, so it routes to memory_copy -- the memmove-shaped
        one -- and not to memory_copy_apart. bcmp is a rename: it is memcmp
        with only the zero/non-zero part of the answer promised, and memcmp
        promises more, so the alias is safe in the direction it is used.
*/
address_any memory_copy_until(address_any destination, address_any source,
                              b8 value, positive size);
p8 address_to string_copy_end(string_address destination, string_address source);
p8 address_to string_copy_max_endptr(string_address destination,
                                     string_address source, positive bound);
string_address string_append_max(string_address destination,
                                 string_address source, positive bound);
fn memory_zero(address_any destination, positive size);
fn memory_copy_source_first(address_any source, address_any destination,
                            positive size);

/*
        Parsing an integer out of the front of a string, with the C contract.

        This is the strtol family and the atoi family, and the contract is the
        whole of the work: skip leading whitespace, take an optional sign, take
        the longest run of digits the base allows, and say where the run
        stopped. The arithmetic is a multiply and an add per digit and never a
        divide -- accumulating forwards means the base multiplies what is
        already there, and the only thing that wants a division is the overflow
        cutoff, which the discarded half of the multiply answers instead. The
        obvious C pays that divide once per call on every architecture, and two
        of them on riscv64, where the remainder is a second instruction.

        string_to_bipolar, which is already here, is not this and cannot be
        aliased to it. It reads backwards from the terminator, so "12a34" is
        thirty four to it and forty two to atoi, and it has no end pointer, no
        base and no whitespace. Both contracts are wanted; they are two
        routines. The parameter that C spells endptr is spelled stopped here
        because end is already the name of the NUL byte.

        A base of zero reads the prefix: "0x" or "0X" is sixteen, a leading
        zero is eight, anything else is ten. Bases two through thirty six are
        taken as given, and with sixteen the "0x" may be written out. Any other
        base converts nothing.

        The prefix has one corner and it is worth naming, because it is where
        implementations differ from each other and from themselves. "0x" with
        nothing usable after it is not a failed conversion: the subject
        sequence is the "0", the answer is zero, and the end pointer lands on
        the "x". That falls out of stepping over the prefix only when a
        hexadecimal digit follows it -- leave the pointer on the "0" instead
        and the ordinary loop takes that digit and stops on the "x" by itself,
        with no case written for it. The same shape covers "0" alone, "00",
        "0xg" and a "0x" at the very end of a string.

        Nothing here reads past the terminator. The second byte is only loaded
        once the first has been found to be a "0", and the third only once the
        second has been found to be an "x", which means neither of them was the
        terminator. A string ending exactly at a page boundary was run against
        an unreadable page after it to check that rather than to assert it.

        The raw pair saturates and says nothing, because the platform library
        has no errno and its pure atoi/atol users must not acquire state. The
        userspace-only checked pair runs that same scanner once and copies its
        register-held overflow fact through an optional pointer. numbers.c uses
        those entries to implement C's ERANGE contract without shared storage,
        a second digit walk or a side effect in the raw routines.

        An unrecognised base stores the input pointer through the end pointer
        rather than leaving it untouched, which is what glibc does there. C
        leaves it undefined, and a caller that reads back an end pointer nobody
        wrote reads its own uninitialised stack; handing it the input is the
        same thing the "no conversion" case hands it and costs one store on a
        path nothing takes twice.

        C23 lets a "0b" prefix mean base two under bases zero and two, and the
        glibc these were measured against does it. This does not: the contract
        implemented here is the older one, and adopting the newer one silently
        turns strtol("0b101", null, 0) from zero into five. The change is the
        "0x" block with 98 in place of 120 and a binary digit test, if it is
        ever wanted.

        Four raw routines, two userspace checked entries and one private core.
        string_to_whole is atoi,
        which is the wide parse with the answer read as an int -- not a
        separate 32 bit accumulation, because atoi("99999999999999999999") is
        the low half of the clamped long and comes out minus one, where an
        int-sized saturation would answer two billion and match nothing.
        riscv64 is the architecture where that narrowing is not free: lp64d
        wants a b32 return sign extended in a0, so the core carries a
        narrowing bit that only the riscv block reads, and x86_64 and arm64
        leave the top half of the return register unspecified the way their own
        ABIs say they may.
*/
bipolar string_to_number(string_address input, string_address address_to stopped,
                         b32 base);
positive string_to_number_unsigned(string_address input,
                                   string_address address_to stopped, b32 base);
#ifndef KERNEL_MODE
//      The checked pair are the same single scan with its sticky overflow
//      fact copied out. Like stopped, out_of_range may be null.
bipolar string_to_number_checked(string_address input,
                                 string_address address_to stopped, b32 base,
                                 b32 address_to out_of_range);
positive string_to_number_unsigned_checked(
        string_address input, string_address address_to stopped, b32 base,
        b32 address_to out_of_range);
#endif
PURE bipolar string_to_whole_wide(string_address input);
PURE b32 string_to_whole(string_address input);

/*
        Counting the set bits of a word, and finding the lowest one of them.

        Four questions, and on two of the three architectures the hardware
        answers each of them outright: x86 has bsf, bsr and popcnt, arm64
        has rbit, clz and a vector cnt. The riscv baseline this builds for
        has none of them -- count-leading-zeros, count-trailing-zeros and
        population count all live in Zbb, which is deliberately not in that
        baseline, and asking the compiler for __builtin_ctzll there emits a
        call to a libgcc helper that a -nostdlib link cannot resolve. So on
        riscv the whole group is shifts, masks and a multiply, and the
        population count is the only real work in it, because the other
        three are that same count asked a different way:

              trailing zeros   the ones below the lowest set bit
              leading zeros    the ones a downward smear leaves above
              first set        the ones the borrow from a subtract flips

        One less than the lowest set bit taken on its own is a run of ones
        exactly as long as the count of trailing zeros. Smearing every set
        bit downwards and complementing leaves exactly the leading zeros.
        Subtracting one borrows through the trailing zeros and stops at the
        lowest set bit, so the exclusive-or of a word with one less than
        itself is that bit and everything under it, which counts the
        one-based position ffs is asking for. Each of the three is a
        prologue of four to six instructions ending in a jump into the
        count, and each arrives at the answer for zero rather than testing
        for it: a word with nothing set smears, isolates and borrows into
        the whole word, which counts 64, and the ffs prologue masks its run
        away to count nothing.

        Those zero answers are the definitions and not accidents of the
        sequence. ffs is 0 for a word with no bits set because C says so;
        trailing and leading zeros are 64 there because a word of zeros has
        sixty four of them. That is worth naming, because the compiler
        builtins these resemble are undefined at zero and libgcc's helpers
        answer 0 to both.

        The three helpers are 64 bit and only 64 bit. No C name is attached
        to them, so there is nothing to stay compatible with and no reason
        to keep two widths of each; a caller holding a 32 bit value gets a
        leading zero count 32 larger than a 32 bit count would give, which
        is the truth about the register it passed. The ffs pair is two
        widths because C has two of it: ffs takes an int and ffsl a long,
        and reading a 64 bit register when an int was passed reads whatever
        happened to be in the top half of it.
*/
CONST b32 bits_counted(positive value);
CONST b32 bits_trailing_zeros(positive value);
CONST b32 bits_leading_zeros(positive value);
CONST b32 bits_first_set(b32 value);
CONST b32 bits_first_set_wide(bipolar value);

/*
        Rounding a decimal to a whole one, choosing between two, and the
        multiply that rounds once.

        Nine of these ten are one instruction on at least two of the three
        architectures, and that is the whole reason they are worth writing
        down: a C library that reaches for a helper function here is paying a
        call for something the silicon does in a cycle, and every one of them
        is named from inside somebody's loop.

        x86-64-v2 carries SSE4.1, so ROUNDSD answers four of them outright.
        Its immediate is a rounding direction in the low two bits and a bit
        that suppresses the inexact signal in bit three, which is why trunc,
        floor, ceil and nearbyint are the same instruction four times with a
        different byte. arm64 spells those four FRINTZ, FRINTM, FRINTP and
        FRINTI, and has FRINTA besides, which is round-half-away-from-zero and
        exists nowhere else. riscv64 has no round-to-integral instruction at
        all in the D extension -- that arrived with Zfa, which is outside this
        baseline -- so there the five are built out of a conversion to a 64 bit
        integer and back, guarded by the observation below.

        The guard is the whole of the riscv trick. A double whose exponent is
        at or above 1075, which is 1023 plus 52, has no fraction left to round:
        the value is already whole, and infinities and not-a-numbers land above
        that line too, so one unsigned compare takes every case the conversion
        could not survive. Below the line the magnitude fits an integer with
        room to spare, FCVT.L.D rounds it in the direction the mode names,
        FCVT.D.L brings it back exactly, and FSGNJ.D puts the original sign on
        the answer -- which is what makes trunc(-0.5) the negative zero it is
        supposed to be rather than the positive one an integer round trip
        produces. None of these five ever changes the sign of its argument, so
        replacing the sign wholesale is safe rather than merely usually right.

        The conversion signals inexact and these operations are defined not to,
        so the flags are read before it and written back after. That is two CSR
        accesses on the common path, and they are here because nearbyint is
        defined by not raising that signal: take them out and the alias at the
        bottom of this file stops being true. Nothing in this library can read
        the flags, so a reader who puts them out of scope can drop the pair and
        lose nothing else.

        round and fdim are the two that are not a single instruction anywhere.

        round rounds halfway cases away from zero, which is not one of the four
        IEEE directions. arm64 has it as FRINTA and riscv has it as a
        conversion mode -- RMM, to nearest with ties to maximum magnitude -- so
        it costs there exactly what the other four cost. x86 builds it, by
        adding the largest double below a half, wearing the sign of the input,
        and truncating. Every value whose fraction is at least a half is
        carried past the next whole number by that addition and no value below
        a half is, because the shortfall of one part in 2^54 is always smaller
        than the gap that would have to be crossed. Adding a plain half instead
        is the famous wrong answer, on 0.49999999999999994, which becomes
        exactly one when a half is added to it.

        fdim is x - y where that is positive, a positive zero everywhere else,
        and a not-a-number when either argument is one. Written as "not less
        than or equal" it is a compare and a mask on x86, because the mask a
        false compare leaves is all zero bits and all zero bits is already the
        positive zero the definition asks for; there is nothing left to select.

        fmin and fmax are hardware everywhere but not the obvious instruction
        on x86. MINSD returns its source operand whenever either input is a
        not-a-number, so it answers correctly when the first argument is one
        and wrongly when the second is, and C wants the other argument in both
        cases. One BLENDVPD on a self-unordered compare covers the half it gets
        wrong. arm64 and riscv have the C behaviour in one instruction --
        FMINNM and FMAXNM are IEEE minNum and maxNum, and FMIN.D and FMAX.D
        were defined that way in the RISC-V specification -- so only x86 pays.

        Two things these three do not agree about, and C declines to settle
        either. A pair of zeros compares equal, so the ordering does not say
        which comes back: x86 hands back the first argument and the other two
        answer the way IEEE minNum does, with the negative zero for the smaller
        and the positive one for the larger. And a signalling not-a-number:
        x86 and riscv treat it like a quiet one and return the other argument,
        while arm64's FMINNM quiets it and returns that, which is what IEEE
        minNum specifies and what gcc and glibc do on that architecture. Both
        are tested, per architecture, rather than papered over.

        fma is the one that is not at the floor on x86 and cannot be. A fused
        multiply-add with a single rounding is FMA3 and FMA3 is x86-64-v3; the
        v2 baseline this builds against does not have it, and gcc answers a
        call to fma() there with a call to fma(). So it is the same shape as
        the string routines: one instruction when the feature byte says the
        machine has it, and a body underneath for the machine that does not.
        See the x86 block for what that body does and why it is in the general
        registers rather than in double-double arithmetic.

        Every routine here takes or returns a decimal, so all of it -- the
        declarations, the bodies and the names at the bottom -- stays out of a
        kernel build. The arm64 kernel build refuses a decimal in a signature
        whether or not anything calls it, and kernel code may not touch the
        floating point registers without asking first.
*/
#ifndef KERNEL_MODE
decimal decimal_truncated(decimal value);
decimal decimal_floor(decimal value);
decimal decimal_ceiling(decimal value);
decimal decimal_rounded(decimal value);
decimal decimal_nearest(decimal value);
CONST decimal decimal_with_sign(decimal magnitude, decimal sign);
decimal decimal_smaller(decimal first, decimal second);
decimal decimal_larger(decimal first, decimal second);
decimal decimal_difference(decimal first, decimal second);
decimal decimal_multiply_add(decimal first, decimal second, decimal addend);
#endif

/*
        The same questions, asked of a float.

        Everything above takes a decimal, which is sixty four bits wide, so a
        program working in single precision has to widen going in and narrow
        coming out of every call. These are the f-suffixed half of the C
        library's arithmetic done in the width it was asked in.

        There is no house name for a thirty two bit float -- decimal is f64
        in the profile this builds, and f32 only when the target has nothing
        wider -- so these are written in f32, the type library.c declares, and
        the prose name says narrow where the wide one says nothing.

        Seven of the nine are one instruction on at least two of the three
        machines, and on arm64 that is the whole story: frintz, frintm,
        frintp and frinta are the four roundings, fminnm and fmaxnm are the
        two extremes that have to let a number beat a NaN, and fsqrt and fabs
        are themselves. riscv64 has the square root, the absolute value, the
        two extremes and the sign transplant as single instructions as well.

        The two places where there was work to do:

        riscv64 has no round-to-integral instruction at all without the Zfa
        extension, which this baseline does not have, and glibc's answer is
        sixteen instructions that fetch a constant from memory and save and
        restore the accrued exception flags around the conversion. But
        fcvt.w.s already carries a static rounding mode, and the four it
        offers -- rtz, rdn, rup, rmm -- are exactly truncation, floor,
        ceiling and round-half-away-from-zero. So all four are the same seven
        instructions with one field changed. Anything whose magnitude is two
        to the twenty third or more is already whole and is handed straight
        back, which is also how infinity and every NaN leave, and is why the
        test is on the magnitude rather than on the class. Everything else
        goes out through an integer register and back, and then the
        argument's sign is put back on, so that the ceiling of minus a
        quarter is minus zero rather than zero.

        x86_64 has roundss for three of the four, but its immediate has no
        half-away-from-zero mode -- SSE4.1 offers nearest-even, down, up and
        truncate and nothing else -- which is why glibc reaches for a
        thirty two instruction branchy routine for roundf. Adding a half and
        truncating is the obvious trick and it is wrong: 0.49999997 plus a
        half rounds up to one and truncates to one, where the answer is zero.
        Adding the float just below a half is right instead, and right for a
        reason. The largest fraction below a half is a half less one step of
        the argument, so the exact sum falls short of the next whole number
        by that step plus two to the minus twenty five, comfortably more than
        the half step rounding up would need. A fraction of exactly a half
        falls short by two to the minus twenty five alone, which is at or
        under the half step, so it carries; the single place it is an exact
        tie is the argument one half itself, where ties-to-even gives one and
        truncation then gives the one that rounding wanted. Six
        instructions, no branch, no table and nothing loaded from memory,
        because the sign is transplanted onto the constant in a general
        register on the way past.

        That addition is also the one instruction in the whole group that
        reads the dynamic rounding mode. roundss takes its direction from the
        immediate and ignores MXCSR, and the riscv64 conversions take theirs
        from a static field, but addss obeys whatever MXCSR says. So
        narrow_rounded on x86_64 wants the rounding mode left at nearest,
        which is where the hardware starts, where nothing in this tree ever
        moves it -- there is no fenv here and MXCSR is written nowhere -- and
        where it was measured. Forced to toward-zero or to downward by hand,
        narrow_rounded of a half answers zero on x86_64 and one on the other
        two. It is the only routine here that is not independent of the
        rounding mode, and the only one that would have to change if this
        library ever grew fesetround.

        The two extremes are the other place x86_64 has to work. minss and
        maxss hand back their second operand whenever the comparison is
        false, which is what a NaN on either side makes it, so minss alone
        answers with a NaN where C says the number wins. One quiet unordered
        compare and a three instruction select repair it, and one mask covers
        both sides: the minimum is a NaN exactly when the second argument is.

        Where the three machines are entitled to differ, they do, and the
        tests assert neither answer.

        A signalling NaN comes back quieted from roundss and from frintz, and
        comes back untouched from the riscv64 four, whose branch returns the
        argument without ever putting it through an arithmetic instruction. C
        has no way to write a signalling NaN in the first place.

        Handed a negative zero and a positive zero, the two extremes have
        arguments that compare equal, and C then permits either. fminnm and
        fmin.s answer with the negative zero, which is what IEEE minNum says;
        minss answers with whichever came second. Six more instructions would
        buy agreement on a pair the standard says need not agree, so they are
        not spent.

        Nothing here reads the exception flags, and nothing here preserves
        them: the riscv64 conversion raises inexact on a fraction and the
        x86_64 rounding raises it on the addition, where roundss on its own
        would not. That is the two instruction CSR round trip glibc pays and
        this does not.

        Every signature is a float, and a float in a signature is refused by
        the arm64 kernel build whether or not anything calls it, so the
        declarations and all three bodies stay out of a kernel build --
        which is right anyway, since kernel code may not touch the floating
        point registers without asking first.
*/
#ifndef KERNEL_MODE
f32 narrow_square_root(f32 value);
CONST f32 narrow_absolute(f32 value);
f32 narrow_smaller(f32 value, f32 other);
f32 narrow_larger(f32 value, f32 other);
f32 narrow_truncated(f32 value);
f32 narrow_floor(f32 value);
f32 narrow_ceiling(f32 value);
f32 narrow_rounded(f32 value);
CONST f32 narrow_with_sign(f32 magnitude, f32 sign);
#endif

#if X64
__asm__(
    ASM_SECTION

    //
    //       The exclusive-or comes first so that setb writes into a register
    //       that is already zero. That is the widening the trailing movzbl
    //       used to pay for, and it also shortens the chain: the byte set no
    //       longer waits on the lea that wrote the rest of the register.
    //       It is the shape the ctype-rest group below already uses.
    //
    ASM_FUNC(byte_is_digit)
    "xor %eax, %eax\n   lea -48(%rdi), %edx\n   cmp $10, %edx\n   setb %al\n"
    ASM_RET
    ASM_END(byte_is_digit)

    ASM_FUNC(byte_is_upper)
    "xor %eax, %eax\n   lea -65(%rdi), %edx\n   cmp $26, %edx\n   setb %al\n"
    ASM_RET
    ASM_END(byte_is_upper)

    ASM_FUNC(byte_is_lower)
    "xor %eax, %eax\n   lea -97(%rdi), %edx\n   cmp $26, %edx\n   setb %al\n"
    ASM_RET
    ASM_END(byte_is_lower)

    //
    //       Setting bit five folds the upper case range onto the lower one,
    //       so one range test answers for both.
    //
    ASM_FUNC(byte_is_alpha)
    "xor %eax, %eax\n   or $32, %edi\n   sub $97, %edi\n"
    "cmp $26, %edi\n   setb %al\n"
    ASM_RET
    ASM_END(byte_is_alpha)

    //
    //       No byte is both a digit and a letter, so the two range tests add
    //       rather than or: each compare leaves its answer in the carry and
    //       adc puts it into the total, which is what byte_is_punctuation
    //       below does and for the same reason. No flag byte is ever set,
    //       and the argument is consumed by the test that comes last.
    //
    ASM_FUNC(byte_is_alnum)
    "xor %eax, %eax\n"
    "lea -48(%rdi), %edx\n   cmp $10, %edx\n   adc $0, %eax\n"
    "or $32, %edi\n   sub $97, %edi\n   cmp $26, %edi\n   adc $0, %eax\n"
    ASM_RET
    ASM_END(byte_is_alnum)

    //
    //       Tab through carriage return are five in a row, so they are one
    //       range, and the space is the only one left over. The space is not
    //       in that range, so the equality and the range add the same way the
    //       two ranges above do: sete for the one it is, adc for the other.
    //       The lea sits after the sete because it must not disturb the flags
    //       the sete reads.
    //
    ASM_FUNC(byte_is_space)
    "xor %eax, %eax\n   cmp $32, %edi\n   sete %al\n"
    "lea -9(%rdi), %edx\n   cmp $5, %edx\n   adc $0, %eax\n"
    ASM_RET
    ASM_END(byte_is_space)

    ASM_FUNC(byte_is_hexadecimal)
    "xor %eax, %eax\n"
    "lea -48(%rdi), %edx\n   cmp $10, %edx\n   adc $0, %eax\n"
    "or $32, %edi\n   sub $97, %edi\n   cmp $6, %edi\n   adc $0, %eax\n"
    ASM_RET
    ASM_END(byte_is_hexadecimal)

    //
    //       Both answers are ready before the comparison resolves -- the byte
    //       as it came and the byte with thirty two taken off or put on -- so
    //       the conditional move is the only instruction the chain runs
    //       through, where the shift the other two machines use has to wait
    //       for the comparison and then be waited on. Still branchless, which
    //       is the point of not looking it up in a table.
    //
    ASM_FUNC(byte_to_upper)
    "lea -97(%rdi), %eax\n   lea -32(%rdi), %edx\n   cmp $26, %eax\n"
    "mov %edi, %eax\n   cmovb %edx, %eax\n"
    ASM_RET
    ASM_END(byte_to_upper)

    ASM_FUNC(byte_to_lower)
    "lea -65(%rdi), %eax\n   lea 32(%rdi), %edx\n   cmp $26, %eax\n"
    "mov %edi, %eax\n   cmovb %edx, %eax\n"
    ASM_RET
    ASM_END(byte_to_lower)

    ASM_FUNC(absolute_whole)
    "mov %edi, %eax\n   mov %edi, %edx\n   sar $31, %edx\n"
    "xor %edx, %eax\n   sub %edx, %eax\n"
    ASM_RET
    ASM_END(absolute_whole)

    ASM_FUNC(absolute_wide)
    "mov %rdi, %rax\n   mov %rdi, %rdx\n   sar $63, %rdx\n"
    "xor %rdx, %rax\n   sub %rdx, %rax\n"
    ASM_RET
    ASM_END(absolute_wide)

#ifndef KERNEL_MODE
    ASM_FUNC(square_root)
    "sqrtsd %xmm0, %xmm0\n"
    ASM_RET
    ASM_END(square_root)

    //
    //       No scalar fabs here, so the sign bit is cleared where the bits
    //       can be reached: through a general register, which is cheaper than
    //       loading a mask from memory to and with.
    //
    ASM_FUNC(absolute)
    "movq %xmm0, %rax\n   btr $63, %rax\n   movq %rax, %xmm0\n"
    ASM_RET
    ASM_END(absolute)
#endif

    // ------------------------------------------------------------
    //       ctype-rest
    // ------------------------------------------------------------

    ASM_FUNC(byte_is_printable)
    "sub $32, %edi\n   xor %eax, %eax\n   cmp $95, %edi\n   setb %al\n"
    ASM_RET
    ASM_END(byte_is_printable)

    ASM_FUNC(byte_is_graphic)
    "sub $33, %edi\n   xor %eax, %eax\n   cmp $94, %edi\n   setb %al\n"
    ASM_RET
    ASM_END(byte_is_graphic)

    //
    //       Below thirty two and equal to a hundred and twenty seven cannot
    //       both hold, so adding the carry out of the second compare to the
    //       first answer is the or, and is a compare and an add rather than a
    //       second set byte, a widen and an or.
    //
    ASM_FUNC(byte_is_control)
    "xor %eax, %eax\n   cmp $127, %edi\n   sete %al\n"
    "cmp $32, %edi\n   adc $0, %eax\n"
    ASM_RET
    ASM_END(byte_is_control)

    //
    //       Graphic, then the two classes that are subtracted back out of it.
    //       Nothing is ever set here: each compare leaves its answer in the
    //       carry and adc and sbb move it into the total, so eleven
    //       instructions carry three range tests and never touch a flag byte.
    //       The letter test comes last because it is the one that consumes
    //       the argument.
    //
    ASM_FUNC(byte_is_punctuation)
    "xor %eax, %eax\n"
    "lea -33(%rdi), %edx\n   cmp $94, %edx\n   adc $0, %eax\n"
    "lea -48(%rdi), %edx\n   cmp $10, %edx\n   sbb $0, %eax\n"
    "or $32, %edi\n   sub $97, %edi\n   cmp $26, %edi\n   sbb $0, %eax\n"
    ASM_RET
    ASM_END(byte_is_punctuation)

    //
    //       Two equalities, and neither is a carry, so this one does pay for
    //       the second set byte. The or is a byte or, which leaves the top of
    //       the answer as the exclusive-or already cleared it.
    //
    ASM_FUNC(byte_is_blank)
    "xor %eax, %eax\n   cmp $32, %edi\n   sete %al\n"
    "cmp $9, %edi\n   sete %dl\n   or %dl, %al\n"
    ASM_RET
    ASM_END(byte_is_blank)

    ASM_FUNC(byte_is_ascii)
    "xor %eax, %eax\n   cmp $127, %edi\n   setbe %al\n"
    ASM_RET
    ASM_END(byte_is_ascii)

    ASM_FUNC(byte_to_ascii)
    "mov %edi, %eax\n   and $127, %eax\n"
    ASM_RET
    ASM_END(byte_to_ascii)

    // ------------------------------------------------------------
    //       set-scan
    // ------------------------------------------------------------
    // The run of bytes that are all in the set. The build loop leaves bit zero
    // clear, so the source's terminator ends the run without being looked for.
    ASM_FUNC(string_span_of_set)
    "sub $32, %rsp\n   xor %eax, %eax\n"
    "movq %rax, (%rsp)\n   movq %rax, 8(%rsp)\n   movq %rax, 16(%rsp)\n   movq %rax, 24(%rsp)\n"
    //
    //       shl takes the count modulo sixty four, so cl holding the whole
    //       byte already means the bit inside the word; ecx is free to become
    //       the word index straight afterwards.
    //
    "movzbl (%rsi), %ecx\n   test %cl, %cl\n   jz 2f\n"
    "1:  mov $1, %rdx\n   shl %cl, %rdx\n   shr $6, %ecx\n   or %rdx, (%rsp,%rcx,8)\n"
    "inc %rsi\n   movzbl (%rsi), %ecx\n   test %cl, %cl\n   jnz 1b\n"
    //
    //       eax is still the zero the four stores above used, and the entry
    //       into the middle of the loop is what keeps the increment and the
    //       branch to one apiece per byte.
    //
    "2:  jmp 4f\n"
    "3:  inc %rax\n"
    "4:  movzbl (%rdi,%rax), %ecx\n   mov %ecx, %edx\n   shr $6, %edx\n"
    "mov (%rsp,%rdx,8), %rdx\n   bt %rcx, %rdx\n   jc 3b\n"
    "add $32, %rsp\n"
    ASM_RET
    ASM_END(string_span_of_set)

    // The complement, and the one bit of difference: the terminator is put
    // into the stopping set so that the end of the source stops the run too.
    ASM_FUNC(string_span_without_set)
    "sub $32, %rsp\n   xor %eax, %eax\n"
    "movq $1, (%rsp)\n   movq %rax, 8(%rsp)\n   movq %rax, 16(%rsp)\n   movq %rax, 24(%rsp)\n"
    "movzbl (%rsi), %ecx\n   test %cl, %cl\n   jz 2f\n"
    "1:  mov $1, %rdx\n   shl %cl, %rdx\n   shr $6, %ecx\n   or %rdx, (%rsp,%rcx,8)\n"
    "inc %rsi\n   movzbl (%rsi), %ecx\n   test %cl, %cl\n   jnz 1b\n"
    "2:  jmp 4f\n"
    "3:  inc %rax\n"
    "4:  movzbl (%rdi,%rax), %ecx\n   mov %ecx, %edx\n   shr $6, %edx\n"
    "mov (%rsp,%rdx,8), %rdx\n   bt %rcx, %rdx\n   jnc 3b\n"
    "add $32, %rsp\n"
    ASM_RET
    ASM_END(string_span_without_set)

    // Where that run stopped, as an address, and null rather than the
    // terminator when it stopped because the source ran out. ecx still holds
    // the byte that stopped it, so the two cases are told apart without
    // reading the source a second time.
    ASM_FUNC(string_first_of_set)
    "sub $32, %rsp\n   xor %eax, %eax\n"
    "movq $1, (%rsp)\n   movq %rax, 8(%rsp)\n   movq %rax, 16(%rsp)\n   movq %rax, 24(%rsp)\n"
    "movzbl (%rsi), %ecx\n   test %cl, %cl\n   jz 2f\n"
    "1:  mov $1, %rdx\n   shl %cl, %rdx\n   shr $6, %ecx\n   or %rdx, (%rsp,%rcx,8)\n"
    "inc %rsi\n   movzbl (%rsi), %ecx\n   test %cl, %cl\n   jnz 1b\n"
    "2:  jmp 4f\n"
    "3:  inc %rax\n"
    "4:  movzbl (%rdi,%rax), %ecx\n   mov %ecx, %edx\n   shr $6, %edx\n"
    "mov (%rsp,%rdx,8), %rdx\n   bt %rcx, %rdx\n   jnc 3b\n"
    "add $32, %rsp\n"
    "add %rdi, %rax\n   xor %edi, %edi\n   test %ecx, %ecx\n   cmovz %rdi, %rax\n"
    ASM_RET
    ASM_END(string_first_of_set)

    // ------------------------------------------------------------
    //       case-compare
    // ------------------------------------------------------------
    //
    //      The first byte, before any of the setup.
    //
    //      Same argument string_compare makes: a dispatch compares a word
    //      against a list of names and nearly all of them differ where the
    //      word begins, and one byte from each is always readable because the
    //      caller handed us two strings. It answers that call before three
    //      broadcast constants and two page checks are paid for.
    //
    ASM_FUNC(string_compare_folded)
    "movzbl (%rdi), %eax\n   movzbl (%rsi), %ecx\n"
    //
    //      lea does the subtract without touching the flags the compare is
    //      about to set, and holds the folded byte ready for a move that may
    //      or may not take it. cmovb is below-unsigned, which is the whole
    //      range test: only 'A' through 'Z' land under twenty six.
    //
    "lea -'A'(%rax), %r8d\n   lea 32(%rax), %r9d\n   cmp $26, %r8d\n   cmovb %r9d, %eax\n"
    "lea -'A'(%rcx), %r8d\n   lea 32(%rcx), %r9d\n   cmp $26, %r8d\n   cmovb %r9d, %ecx\n"
    "sub %ecx, %eax\n   jne 9f\n"
    "test %ecx, %ecx\n   jz 9f  # both ended here, and eax is zero\n"
    "inc %rdi\n   inc %rsi\n"
    ASM_USERSPACE_WIDE(
    //
    //      0x3f moves 'A' through 'Z' to 0x80 through 0x99 and nothing else
    //      into that block, so one signed compare against 0x9a is the range
    //      test for sixteen bytes. SSE2 and no feature byte: every x86-64
    //      has these, so there is no narrow body to keep correct beside this
    //      one and no vzeroupper on any path out.
    //
    "mov $0x3f3f3f3f, %eax\n   movd %eax, %xmm5\n   pshufd $0, %xmm5, %xmm5\n"
    "mov $0x9a9a9a9a, %eax\n   movd %eax, %xmm6\n   pshufd $0, %xmm6, %xmm6\n"
    "mov $0x20202020, %eax\n   movd %eax, %xmm7\n   pshufd $0, %xmm7, %xmm7\n"
    "pxor %xmm4, %xmm4\n"
    //
    //      Would either read cross a page? 0xff0 is the last offset at which
    //      sixteen bytes still fit, which is string_compare's argument at
    //      0xff8 with sixteen in place of eight.
    //
    "10:  mov %edi, %ecx\n   and $0xfff, %ecx\n   cmp $0xff0, %ecx\n   ja 1f\n"
    "mov %esi, %ecx\n   and $0xfff, %ecx\n   cmp $0xff0, %ecx\n   ja 1f\n"
    "movdqu (%rdi), %xmm0\n   movdqu (%rsi), %xmm1\n"
    "movdqa %xmm0, %xmm2\n   paddb %xmm5, %xmm2\n   movdqa %xmm6, %xmm3\n"
    "pcmpgtb %xmm2, %xmm3\n   pand %xmm7, %xmm3\n   paddb %xmm3, %xmm0\n"
    "movdqa %xmm1, %xmm2\n   paddb %xmm5, %xmm2\n   movdqa %xmm6, %xmm3\n"
    "pcmpgtb %xmm2, %xmm3\n   pand %xmm7, %xmm3\n   paddb %xmm3, %xmm1\n"
    //
    //      One mask answers both questions. Bytes that agree, and-not bytes
    //      that are the terminator, leaves ones exactly where the loop may
    //      keep going -- and folding cannot make a zero byte or unmake one,
    //      so the folded word is as good as the original to look for it in.
    //
    "pcmpeqb %xmm0, %xmm1\n   pcmpeqb %xmm4, %xmm0\n   pandn %xmm1, %xmm0\n"
    "pmovmskb %xmm0, %ecx\n   cmp $0xffff, %ecx\n   jne 11f\n"
    "add $16, %rdi\n   add $16, %rsi\n   jmp 10b\n"
    //
    //      Something happened in these sixteen. The lowest zero says where;
    //      both pointers go there and the byte loop below says what it was.
    //      Nothing is concluded here, which is why a difference sitting past
    //      a terminator cannot escape into the answer.
    //
    "11:  not %ecx\n   bsf %ecx, %ecx\n   add %rcx, %rdi\n   add %rcx, %rsi\n"
    )
    //
    //      Sixteen bytes the slow way, then the wide loop again -- by then a
    //      page boundary that turned it back is behind us. Reached also with
    //      the answer one or two bytes away, which is the ordinary case.
    //
    "1:  mov $16, %r10d\n"
    "2:  movzbl (%rdi), %eax\n   movzbl (%rsi), %ecx\n"
    "lea -'A'(%rax), %r8d\n   lea 32(%rax), %r9d\n   cmp $26, %r8d\n   cmovb %r9d, %eax\n"
    "lea -'A'(%rcx), %r8d\n   lea 32(%rcx), %r9d\n   cmp $26, %r8d\n   cmovb %r9d, %ecx\n"
    "sub %ecx, %eax\n   jne 9f\n"
    "test %ecx, %ecx\n   jz 9f\n"
    "inc %rdi\n   inc %rsi\n   dec %r10d\n   jnz 2b\n"
    ASM_USERSPACE_WIDE("   jmp 10b\n")
    "jmp 1b  # a kernel build has no wide body to go back to\n"
    "9:  #\n"
    ASM_RET
    ASM_END(string_compare_folded)
    //
    //      The bound makes every read here legal without a page argument,
    //      which is the whole difference between this and the routine above;
    //      it also means this reads up to n bytes and not up to the
    //      terminator, exactly as string_compare_max does.
    //
    ASM_FUNC(string_compare_folded_max)
    "xor %eax, %eax\n   test %rdx, %rdx\n   jz 9f\n"
    ASM_USERSPACE_WIDE(
    "cmp $16, %rdx\n   jb 1f\n"
    "mov $0x3f3f3f3f, %eax\n   movd %eax, %xmm5\n   pshufd $0, %xmm5, %xmm5\n"
    "mov $0x9a9a9a9a, %eax\n   movd %eax, %xmm6\n   pshufd $0, %xmm6, %xmm6\n"
    "mov $0x20202020, %eax\n   movd %eax, %xmm7\n   pshufd $0, %xmm7, %xmm7\n"
    "pxor %xmm4, %xmm4\n"
    "10:  movdqu (%rdi), %xmm0\n   movdqu (%rsi), %xmm1\n"
    "movdqa %xmm0, %xmm2\n   paddb %xmm5, %xmm2\n   movdqa %xmm6, %xmm3\n"
    "pcmpgtb %xmm2, %xmm3\n   pand %xmm7, %xmm3\n   paddb %xmm3, %xmm0\n"
    "movdqa %xmm1, %xmm2\n   paddb %xmm5, %xmm2\n   movdqa %xmm6, %xmm3\n"
    "pcmpgtb %xmm2, %xmm3\n   pand %xmm7, %xmm3\n   paddb %xmm3, %xmm1\n"
    "pcmpeqb %xmm0, %xmm1\n   pcmpeqb %xmm4, %xmm0\n   pandn %xmm1, %xmm0\n"
    "pmovmskb %xmm0, %ecx\n   cmp $0xffff, %ecx\n   jne 11f\n"
    "add $16, %rdi\n   add $16, %rsi\n   sub $16, %rdx\n   cmp $16, %rdx\n   jae 10b\n"
    "jmp 1f\n"
    "11:  not %ecx\n   bsf %ecx, %ecx\n   add %rcx, %rdi\n   add %rcx, %rsi\n   sub %rcx, %rdx\n"
    )
    //
    //      Counting up from minus n so the loop ends when the index reaches
    //      zero rather than on a compare against it. The two additions cancel
    //      modulo the word, so the pointer this builds addresses the right
    //      byte for any n a caller can pass, including one that runs it past
    //      the end of the address space.
    //
    "1:  xor %eax, %eax\n   test %rdx, %rdx\n   jz 9f\n"
    "add %rdx, %rdi\n   add %rdx, %rsi\n   neg %rdx\n"
    "2:  movzbl (%rdi,%rdx), %eax\n   movzbl (%rsi,%rdx), %ecx\n"
    "lea -'A'(%rax), %r8d\n   lea 32(%rax), %r9d\n   cmp $26, %r8d\n   cmovb %r9d, %eax\n"
    "lea -'A'(%rcx), %r8d\n   lea 32(%rcx), %r9d\n   cmp $26, %r8d\n   cmovb %r9d, %ecx\n"
    "sub %ecx, %eax\n   jne 9f\n"
    "test %ecx, %ecx\n   jz 9f\n"
    "inc %rdx\n   jnz 2b\n"
    "9:  #\n"
    ASM_RET
    ASM_END(string_compare_folded_max)

    // ------------------------------------------------------------
    //       memory-rest
    // ------------------------------------------------------------
    //
    //       memccpy: the copy that stops after a byte, and says where it got to.
    //
    //       Three saved and an eight byte pad, which is what an entry that is
    //       eight past aligned needs to reach the calls on sixteen. The size
    //       has to survive memory_first_of because it is the copy length when
    //       the byte was not there; the destination has to survive it because
    //       the answer is measured from it.
    //
    ASM_FUNC(memory_copy_until)
    "push %rbx\n   push %r12\n   push %r13\n"
    "mov %rdi, %rbx  # the destination\n   mov %rsi, %r12  # the source\n"
    "mov %rcx, %r13  # the size\n"
    "mov %rsi, %rdi\n   mov %rdx, %rsi\n   mov %rcx, %rdx\n"
    "call memory_first_of\n"
    "mov %r13, %rdx  # a byte that is not there copies the whole span\n"
    "test %rax, %rax\n   jnz 2f\n"
    "xor %r13d, %r13d  # and answers null\n   jmp 1f\n"
    "2:  sub %r12, %rax\n   lea 1(%rax), %rdx  # everything up to and including it\n"
    "lea (%rbx,%rdx), %r13  # and the answer is just past what was copied\n"
    "1:  mov %rbx, %rdi\n   mov %r12, %rsi\n   call memory_copy_apart\n"
    "mov %r13, %rax\n"
    "pop %r13\n   pop %r12\n   pop %rbx\n"
    ASM_RET
    ASM_END(memory_copy_until)
    //
    //       stpcpy: string_copy, answering where the terminator went.
    //
    //       Two saved and a pad. The length is wanted twice -- once as the
    //       copy length plus the terminator, once as the offset of the answer
    //       -- so the answer is folded into the saved destination before the
    //       copy rather than kept in a third register.
    //
    ASM_FUNC(string_copy_end)
    "push %rbx\n   push %r12\n   sub $8, %rsp\n"
    "mov %rdi, %rbx  # the destination\n   mov %rsi, %r12  # the source\n"
    "mov %rsi, %rdi\n   call string_length\n"
    "mov %rbx, %rdi\n   mov %r12, %rsi\n   lea 1(%rax), %rdx  # the terminator is copied too\n"
    "add %rax, %rbx  # which is where it lands, and the answer\n"
    "call memory_copy_apart\n   mov %rbx, %rax\n"
    "add $8, %rsp\n   pop %r12\n   pop %rbx\n"
    ASM_RET
    ASM_END(string_copy_end)
    //
    //       stpncpy: the same, bounded, padded, and not always terminated.
    //
    //       string_length_max stops at the bound, so the copy length and the
    //       answer are the same number in both cases and only the padding is
    //       conditional. The bound turning into what is left of it before the
    //       copy is what makes the test after it free.
    //
    ASM_FUNC(string_copy_max_endptr)
    "push %rbx\n   push %r12\n   push %r13\n"
    "mov %rdi, %rbx  # the destination\n   mov %rsi, %r12  # the source\n"
    "mov %rdx, %r13  # the bound\n"
    "mov %rsi, %rdi\n   mov %rdx, %rsi\n   call string_length_max\n"
    "mov %rbx, %rdi\n   mov %r12, %rsi\n   mov %rax, %rdx\n"
    "add %rax, %rbx  # the first byte the source did not fill, and the answer\n"
    "sub %rax, %r13  # what is left of the bound is padding\n"
    "call memory_copy_apart\n"
    "test %r13, %r13\n   jz 1f\n"
    "mov %rbx, %rdi\n   xor %esi, %esi\n   mov %r13, %rdx\n   call memory_fill\n"
    "1:  mov %rbx, %rax\n"
    "pop %r13\n   pop %r12\n   pop %rbx\n"
    ASM_RET
    ASM_END(string_copy_max_endptr)
    //
    //       strncat: the end of the destination, then the bounded copy that
    //       terminates -- which is string_copy_max_end exactly, so this is the
    //       walk and the handover and nothing else.
    //
    ASM_FUNC(string_append_max)
    "push %rbx\n   push %r12\n   push %r13\n"
    "mov %rdi, %rbx  # the destination\n   mov %rsi, %r12  # the source\n"
    "mov %rdx, %r13  # the bound\n"
    "call string_length  # the destination is already in rdi\n"
    "lea (%rbx,%rax), %rdi\n   mov %r12, %rsi\n   mov %r13, %rdx\n"
    "call string_copy_max_end\n"
    "mov %rbx, %rax  # strncat hands the destination back\n"
    "pop %r13\n   pop %r12\n   pop %rbx\n"
    ASM_RET
    ASM_END(string_append_max)
    //
    //       bzero and bcopy: the argument orders, and nothing else.
    //
    //       Neither is a rename. bzero puts the size where memset puts the
    //       byte, and bcopy puts the source first and promises to survive
    //       overlap, so it goes to memory_copy rather than memory_copy_apart.
    //       Both are a shuffle and a tail call, and the shuffle is three
    //       register moves rather than an exchange because a move between
    //       registers is renamed away and an exchange is three uops that are
    //       not. gcc emits exactly these two sequences from the obvious C.
    //
    ASM_FUNC(memory_zero)
    "mov %rsi, %rdx\n   xor %esi, %esi\n   jmp memory_fill\n"
    ASM_END(memory_zero)
    ASM_FUNC(memory_copy_source_first)
    "mov %rdi, %rax\n   mov %rsi, %rdi\n   mov %rax, %rsi\n   jmp memory_copy\n"
    ASM_END(memory_copy_source_first)

    // ------------------------------------------------------------
    //       integer-parse
    // ------------------------------------------------------------
    ASM_FUNC(string_to_number)
    "xor %r11d, %r11d\n   jmp string_to_number_core\n"
    ASM_END(string_to_number)

    ASM_FUNC(string_to_number_unsigned)
    "mov $2, %r11d\n   jmp string_to_number_core\n"
    ASM_END(string_to_number_unsigned)

    //       These two preserve the pure house entry points: they ask the
    //       same core for its sticky overflow register, then store that one
    //       fact for the C strto* wrappers. There is one scan, and a normal
    //       string_to_number call pays none of this call/save/store path.
#ifndef KERNEL_MODE
    ASM_FUNC(string_to_number_checked)
    "push %rcx\n   xor %r10d, %r10d\n   call string_to_number\n"
    "pop %rcx\n   xor %edx, %edx\n   test %r10, %r10\n   setne %dl\n"
    "test %rcx, %rcx\n   je .Lnumber_x64_checked_done\n"
    "mov %edx, (%rcx)\n   .Lnumber_x64_checked_done:\n"
    ASM_RET
    ASM_END(string_to_number_checked)

    ASM_FUNC(string_to_number_unsigned_checked)
    "push %rcx\n   xor %r10d, %r10d\n   call string_to_number_unsigned\n"
    "pop %rcx\n   xor %edx, %edx\n   test %r10, %r10\n   setne %dl\n"
    "test %rcx, %rcx\n   je .Lnumber_x64_unsigned_checked_done\n"
    "mov %edx, (%rcx)\n   .Lnumber_x64_unsigned_checked_done:\n"
    ASM_RET
    ASM_END(string_to_number_unsigned_checked)
#endif

    ASM_FUNC(string_to_whole_wide)
    "xor %esi, %esi\n   mov $10, %edx\n   xor %r11d, %r11d\n"
    "jmp string_to_number_core\n"
    ASM_END(string_to_whole_wide)

    //
    //       atoi is atol with the answer read as an int, and on x86_64 that
    //       is the same register: the ABI leaves the top half of rax
    //       unspecified for an int return and every caller reads eax. The
    //       compiler emits a call and a ret here rather than a jump, which is
    //       one instruction it does not need.
    //
    ASM_FUNC(string_to_whole)
    "xor %esi, %esi\n   mov $10, %edx\n   xor %r11d, %r11d\n"
    "jmp string_to_number_core\n"
    ASM_END(string_to_whole)

    //
    //       The core. r11 is the hidden fourth argument the wrappers set, the
    //       way positive_into_core takes its flags: bit one selects the
    //       unsigned contract, bit two asks for the answer narrowed to a b32
    //       and is read on riscv64 only, and bit zero is the routine's own
    //       note that a minus sign was seen.
    //
    //       r9 is the base, taken out of edx because an int argument owns only
    //       the low half of its register and because mul wants rdx. rdi is the
    //       input pointer and stays one until the first digit is accepted --
    //       after that nothing can answer "no conversion" any more, so the
    //       register becomes the digit scratch the loop needs. Nothing is
    //       pushed and nothing is spilled: the whole parse runs in registers.
    //
    ASM_LOCAL_FUNC(string_to_number_core)
    "mov %edx, %r9d  # the base, out of the half an int owns\n"
    "lea -2(%r9), %eax\n   cmp $34, %eax\n   jbe .Lnumber_x64_start\n"
    "test %r9d, %r9d\n   jne .Lnumber_x64_none  # not zero and not 2..36\n"
    ".Lnumber_x64_start:\n   mov %rdi, %r8\n"
    //
    //       Tab through carriage return is one range and the space is the
    //       other, which is byte_is_space unrolled into the loop rather than
    //       called once per byte.
    //
    ".Lnumber_x64_space:\n   movzbl (%r8), %ecx\n   inc %r8\n"
    "cmp $32, %ecx\n   je .Lnumber_x64_space\n"
    "lea -9(%rcx), %eax\n   cmp $4, %eax\n   jbe .Lnumber_x64_space\n"
    "dec %r8  # back onto the byte that stopped the scan\n"
    "cmp $45, %ecx\n   je .Lnumber_x64_minus\n"
    "cmp $43, %ecx\n   jne .Lnumber_x64_base\n"
    "inc %r8\n   jmp .Lnumber_x64_base\n"
    ".Lnumber_x64_minus:\n   or $1, %r11d\n   inc %r8\n"
    //
    //       A base of zero reads the prefix. A leading zero is eight unless an
    //       x follows it, and the second byte is only looked at when the first
    //       was a zero, so the load cannot be past a terminator.
    //
    ".Lnumber_x64_base:\n   test %r9d, %r9d\n   jne .Lnumber_x64_given\n"
    "mov $10, %r9d\n   cmpb $48, (%r8)\n   jne .Lnumber_x64_first\n"
    "mov $8, %r9d\n   movzbl 1(%r8), %ecx\n   or $32, %ecx\n"
    "cmp $120, %ecx\n   jne .Lnumber_x64_first\n"
    "mov $16, %r9d\n   jmp .Lnumber_x64_prefix\n"
    ".Lnumber_x64_given:\n   cmp $16, %r9d\n   jne .Lnumber_x64_first\n"
    "cmpb $48, (%r8)\n   jne .Lnumber_x64_first\n"
    "movzbl 1(%r8), %ecx\n   or $32, %ecx\n   cmp $120, %ecx\n"
    "jne .Lnumber_x64_first\n"
    //
    //       The prefix is stepped over only when a hexadecimal digit follows
    //       it. Leave the pointer on the zero otherwise and the loop below
    //       takes that digit and stops on the x by itself, which is the answer
    //       C asks for -- zero, ending at the x -- with no case written for it.
    //
    ".Lnumber_x64_prefix:\n   movzbl 2(%r8), %ecx\n   lea -48(%rcx), %eax\n"
    "cmp $9, %eax\n   jbe .Lnumber_x64_take\n   or $32, %ecx\n"
    "lea -97(%rcx), %eax\n   cmp $5, %eax\n   ja .Lnumber_x64_first\n"
    ".Lnumber_x64_take:\n   add $2, %r8\n"
    //
    //       The first digit is peeled because it is the one that decides
    //       whether a conversion happened at all. Peeling it costs the
    //       classifier twice in the source and saves both the flag and the
    //       saved pointer a rolled loop would have to carry.
    //
    //       One subtract and one unsigned compare classify a byte at each end
    //       at once: anything under the zero wraps to an enormous number and
    //       fails the same compare that anything over the nine fails. Setting
    //       bit five folds the two alphabets onto one, and the base compare
    //       afterwards is what makes an eight in octal or a g in hexadecimal
    //       stop the run.
    //
    ".Lnumber_x64_first:\n   movzbl (%r8), %ecx\n   lea -48(%rcx), %eax\n"
    "cmp $9, %eax\n   jbe .Lnumber_x64_first_value\n   or $32, %ecx\n"
    "lea -97(%rcx), %eax\n   cmp $25, %eax\n   ja .Lnumber_x64_none\n"
    "add $10, %eax\n"
    ".Lnumber_x64_first_value:\n   cmp %r9d, %eax\n   jae .Lnumber_x64_none\n"
    "inc %r8\n   xor %r10d, %r10d  # nothing has overflowed yet\n"
    //
    //       Multiply and add, and the overflow is the half of the multiply
    //       that a 64 bit result throws away. mul writes it to rdx for free,
    //       the carry out of the digit add folds into it, and r10 keeps the
    //       or of every one of them -- so overflow is four instructions with
    //       no branch and no division to find a cutoff with.
    //
    ".Lnumber_x64_loop:\n   movzbl (%r8), %ecx\n   lea -48(%rcx), %edi\n"
    "cmp $9, %edi\n   jbe .Lnumber_x64_value\n   or $32, %ecx\n"
    "lea -97(%rcx), %edi\n   cmp $25, %edi\n   ja .Lnumber_x64_stop\n"
    "add $10, %edi\n"
    ".Lnumber_x64_value:\n   cmp %r9d, %edi\n   jae .Lnumber_x64_stop\n"
    "mul %r9\n   add %rdi, %rax\n   adc $0, %rdx\n   or %rdx, %r10\n"
    "inc %r8\n   jmp .Lnumber_x64_loop\n"
    ".Lnumber_x64_stop:\n   test %rsi, %rsi\n   je .Lnumber_x64_kept\n"
    "mov %r8, (%rsi)\n"
    //
    //       The signed limit is one larger on the minus side, and negating
    //       that limit is exactly the most negative number, so the same two
    //       instructions serve both ends and -9223372036854775808 is not a
    //       special case.
    //
    ".Lnumber_x64_kept:\n   test $2, %r11b\n   jne .Lnumber_x64_unsigned\n"
    "movabs $0x7fffffffffffffff, %rcx\n   mov %r11d, %edx\n   and $1, %edx\n"
    "add %rdx, %rcx\n   test %r10, %r10\n   jne .Lnumber_x64_clamp\n"
    "cmp %rcx, %rax\n   jbe .Lnumber_x64_sign\n"
    ".Lnumber_x64_clamp:\n   mov %rcx, %rax\n   mov $1, %r10d\n"
    ".Lnumber_x64_sign:\n   test $1, %r11b\n   je .Lnumber_x64_done\n"
    "neg %rax\n"
    ASM_RET
    //
    //       Unsigned overflow answers all ones and the sign is not applied to
    //       it, which is what strtoul does; an unsigned parse that did not
    //       overflow negates in the machine word, so "-1" is the largest
    //       unsigned number and not an error.
    //
    ".Lnumber_x64_unsigned:\n   test %r10, %r10\n   je .Lnumber_x64_negate\n"
    "or $-1, %rax\n"
    ASM_RET
    ".Lnumber_x64_negate:\n   test $1, %r11b\n   je .Lnumber_x64_done\n"
    "neg %rax\n"
    ".Lnumber_x64_done:\n"
    ASM_RET
    //
    //       Nothing was converted, or the base was one nobody has: the answer
    //       is zero and the end pointer is the input, untouched by the
    //       whitespace and the sign that were read past.
    //
    ".Lnumber_x64_none:\n   xor %eax, %eax\n   test %rsi, %rsi\n"
    "je .Lnumber_x64_none_done\n   mov %rdi, (%rsi)\n"
    ".Lnumber_x64_none_done:\n"
    ASM_RET
    ASM_LOCAL_END(string_to_number_core)

    // ------------------------------------------------------------
    //       bit-count
    // ------------------------------------------------------------
    //
    //       popcnt is in the x86-64-v2 baseline this builds for, which is
    //       what the kernel image is compiled at as well, so the count is
    //       the instruction. The exclusive-or in front of it is not
    //       padding: popcnt on several Intel generations carries a false
    //       dependency on its destination register, so a chain of counts
    //       serialises on whatever last wrote that register. Zeroing
    //       it is recognised at rename, costs no execution slot, and breaks
    //       the chain. This is gcc's own sequence and it is the floor.
    //
    ASM_FUNC(bits_counted)
    "xor %eax, %eax\n   popcnt %rdi, %rax\n"
    ASM_RET
    ASM_END(bits_counted)

    //
    //       bsf leaves its destination undefined on a zero input and says
    //       so in the zero flag, so the answer for zero is moved in rather
    //       than computed, and the conditional move is what makes the zero
    //       case cost the same as every other. gcc reaches for tzcnt here
    //       and then tests the input a second time to pick the 64; the flag
    //       bsf already set is the same test, one instruction earlier.
    //
    ASM_FUNC(bits_trailing_zeros)
    "bsf %rdi, %rax\n   mov $64, %edx\n   cmovz %edx, %eax\n"
    ASM_RET
    ASM_END(bits_trailing_zeros)

    //
    //       bsr answers with the position of the highest set bit and the
    //       count wanted is 63 less that, which for a position is an
    //       exclusive-or with 63 rather than a subtraction -- gcc ends the
    //       same way. What differs is the zero case: 127 is moved in
    //       because 127 ^ 63 is 64, so zero rides through that same last
    //       instruction as every other input, where gcc jumps over it.
    //
    ASM_FUNC(bits_leading_zeros)
    "bsr %rdi, %rax\n   mov $127, %edx\n   cmovz %edx, %eax\n   xor $63, %eax\n"
    ASM_RET
    ASM_END(bits_leading_zeros)

    //
    //       ffs is the position plus one, and zero for no bit at all, so
    //       -1 is moved in for zero and the increment carries it to 0.
    //       The move and the conditional move are deliberately 32 bit in
    //       both widths: bsf answers with a position under 64 and the
    //       return is an int, so the narrow encoding is the shorter one
    //       and says the same thing.
    //
    ASM_FUNC(bits_first_set)
    "bsf %edi, %eax\n   mov $-1, %edx\n   cmovz %edx, %eax\n   add $1, %eax\n"
    ASM_RET
    ASM_END(bits_first_set)

    ASM_FUNC(bits_first_set_wide)
    "bsf %rdi, %rax\n   mov $-1, %edx\n   cmovz %edx, %eax\n   add $1, %eax\n"
    ASM_RET
    ASM_END(bits_first_set_wide)

    // ------------------------------------------------------------
    //       float-round
    // ------------------------------------------------------------
#ifndef KERNEL_MODE

    //
    //      Three constants, sixteen bytes and sixteen byte aligned apiece,
    //      because ANDPD and ORPD read a whole 128 bit lane out of memory and
    //      fault on a legacy encoded operand that is not aligned. The half is
    //      the largest double below one -- 0x3fdfffffffffffff -- and the
    //      reason for it rather than a plain half is in the note above the
    //      declarations.
    //
    ASM_RODATA_OBJECT_BEGIN(decimal_round_constants, 16)
    ".quad 0x7fffffffffffffff, 0x7fffffffffffffff  /* every bit but the sign */\n"
    ".quad 0x8000000000000000, 0x8000000000000000  /* the sign bit alone */\n"
    ".quad 0x3fdfffffffffffff, 0x3fdfffffffffffff  /* the double just below a half */\n"
    ASM_OBJECT_END(decimal_round_constants)

    ASM_FUNC(decimal_truncated)
    "roundsd $11, %xmm0, %xmm0  # toward zero, and quietly\n"
    ASM_RET
    ASM_END(decimal_truncated)

    ASM_FUNC(decimal_floor)
    "roundsd $9, %xmm0, %xmm0  # toward minus infinity, and quietly\n"
    ASM_RET
    ASM_END(decimal_floor)

    ASM_FUNC(decimal_ceiling)
    "roundsd $10, %xmm0, %xmm0  # toward plus infinity, and quietly\n"
    ASM_RET
    ASM_END(decimal_ceiling)

    ASM_FUNC(decimal_nearest)
    "roundsd $12, %xmm0, %xmm0  # whichever way MXCSR says, and quietly\n"
    ASM_RET
    ASM_END(decimal_nearest)

    ASM_FUNC(decimal_rounded)
    "movapd %xmm0, %xmm1\n"
    "andpd decimal_round_constants+16(%rip), %xmm1  # the sign of x, alone\n"
    "orpd decimal_round_constants+32(%rip), %xmm1   # a half short one place, wearing it\n"
    "addsd %xmm1, %xmm0\n"
    "roundsd $11, %xmm0, %xmm0\n"
    ASM_RET
    ASM_END(decimal_rounded)

    ASM_FUNC(decimal_with_sign)
    "andpd decimal_round_constants(%rip), %xmm0\n"
    "andpd decimal_round_constants+16(%rip), %xmm1\n"
    "orpd %xmm1, %xmm0\n"
    ASM_RET
    ASM_END(decimal_with_sign)

    ASM_FUNC(decimal_smaller)
    "movapd %xmm1, %xmm2\n"
    "minsd %xmm0, %xmm2  # right unless the first argument is not a number\n"
    "cmpunordsd %xmm0, %xmm0  # and that is what this asks\n"
    "blendvpd %xmm0, %xmm1, %xmm2\n"
    "movapd %xmm2, %xmm0\n"
    ASM_RET
    ASM_END(decimal_smaller)

    ASM_FUNC(decimal_larger)
    "movapd %xmm1, %xmm2\n"
    "maxsd %xmm0, %xmm2\n"
    "cmpunordsd %xmm0, %xmm0\n"
    "blendvpd %xmm0, %xmm1, %xmm2\n"
    "movapd %xmm2, %xmm0\n"
    ASM_RET
    ASM_END(decimal_larger)

    ASM_FUNC(decimal_difference)
    "movapd %xmm0, %xmm2\n"
    "subsd %xmm1, %xmm2\n"
    "cmpnlesd %xmm1, %xmm0  # not less or equal: greater, or unordered\n"
    "andpd %xmm2, %xmm0     # and a false mask is a positive zero already\n"
    ASM_RET
    ASM_END(decimal_difference)

    //
    //      The fused multiply-add, and the long way round underneath it.
    //
    //      FMA3 is x86-64-v3 and this file builds against v2, so the one
    //      instruction cannot simply be written: the feature byte decides,
    //      the same byte the string routines read. cpu_has_avx2 stands in for
    //      "has FMA3" because no shipping part has AVX2 without it, and
    //      because the XGETBV probe that sets it already checked that the
    //      operating system enabled the AVX state a VEX encoding needs.
    //
    //      Underneath is the body a machine older than 2013 runs, and it is
    //      in the general registers rather than in double-double arithmetic
    //      for one reason: MUL puts the whole 106 bit product of two
    //      significands in rdx:rax exactly, in one instruction, and every
    //      step after it is a shift, an add and a compare that is exact by
    //      construction. Dekker splitting would be shorter to write and has
    //      an overflow precondition on its 2^27+1 multiply and an error term
    //      that is easy to get almost right; subnormal inputs, subnormal
    //      answers and overflow become visible branches here instead of
    //      subtle identities. The narrow path is slow and obviously correct,
    //      which is the right trade for code nothing modern executes.
    //
    //      Only the default rounding direction is implemented, to nearest
    //      with ties to even. This library never changes the mode.
    //
    ASM_FUNC(decimal_multiply_add)
    ASM_NARROW("cpu_has_avx2", "70f")
    "vfmadd213sd %xmm2, %xmm1, %xmm0\n"
    ASM_RET
    "70:\n"
    "movq %xmm0, %rax\n   movq %xmm1, %rsi\n   movq %xmm2, %rdi\n"
    "mov %rax, %r8\n   shr $52, %r8\n   and $0x7ff, %r8d\n"
    "mov %rsi, %r9\n   shr $52, %r9\n   and $0x7ff, %r9d\n"
    "mov %rdi, %r10\n  shr $52, %r10\n  and $0x7ff, %r10d\n"
    "cmp $0x7ff, %r8d\n   je 1f\n"
    "cmp $0x7ff, %r9d\n   je 1f\n"
    "cmp $0x7ff, %r10d\n  je 2f\n"
    "mov $0xfffffffffffff, %rdx\n"
    "and %rdx, %rax\n   and %rdx, %rsi\n   and %rdx, %rdi\n"
    "mov %rax, %rdx\n   or %r8, %rdx\n   jz 1f  # a zero factor\n"
    "mov %rsi, %rdx\n   or %r9, %rdx\n   jz 1f\n"
    "mov %rdi, %rdx\n   or %r10, %rdx\n   jz 3f  # a zero addend\n"
    "jmp 4f\n"
    //       An infinite or not-a-number factor, or a factor of zero: the
    //       product is exact or is already the answer, so the plain
    //       expression rounds once and is right. An infinite or not-a-number
    //       addend with finite factors is the addend itself, and adding a
    //       zero to it quiets a signalling one; the plain expression would be
    //       wrong here, because a product that overflowed to an infinity of
    //       the other sign would make a not-a-number out of it. A zero addend
    //       leaves the rounded product, and adding the zero would turn a
    //       negative zero product into a positive one.
    "1:  mulsd %xmm1, %xmm0\n   addsd %xmm2, %xmm0\n"
    ASM_RET
    "2:  pxor %xmm0, %xmm0\n   addsd %xmm2, %xmm0  # z, and a signalling one quieted\n"
    ASM_RET
    "3:  mulsd %xmm1, %xmm0\n"
    ASM_RET

    "4:  push %rbx\n   push %rbp\n"
    "movq %xmm0, %rbx\n   movq %xmm1, %rdx\n   xor %rdx, %rbx\n   shr $63, %rbx  # the product's sign\n"
    "movq %xmm2, %rbp\n   shr $63, %rbp  # the addend's\n"

    //       Each of the three becomes a 53 bit significand and an exponent
    //       that counts from its lowest bit. A subnormal has no implicit one,
    //       so it is shifted up until bit 52 is set and its exponent pays for
    //       the shift; after this the three are indistinguishable.
    "test %r8d, %r8d\n   jz 10f\n   bts $52, %rax\n   sub $1075, %r8d\n   jmp 11f\n"
    "10: bsr %rax, %rcx\n   mov $52, %r11d\n   sub %ecx, %r11d\n   mov %r11d, %ecx\n"
    "    shl %cl, %rax\n   mov $-1074, %r8d\n   sub %r11d, %r8d\n"
    "11: test %r9d, %r9d\n   jz 12f\n   bts $52, %rsi\n   sub $1075, %r9d\n   jmp 13f\n"
    "12: bsr %rsi, %rcx\n   mov $52, %r11d\n   sub %ecx, %r11d\n   mov %r11d, %ecx\n"
    "    shl %cl, %rsi\n   mov $-1074, %r9d\n   sub %r11d, %r9d\n"
    "13: test %r10d, %r10d\n   jz 14f\n   bts $52, %rdi\n   sub $1075, %r10d\n   jmp 15f\n"
    "14: bsr %rdi, %rcx\n   mov $52, %r11d\n   sub %ecx, %r11d\n   mov %r11d, %ecx\n"
    "    shl %cl, %rdi\n   mov $-1074, %r10d\n   sub %r11d, %r10d\n"

    //       The product of two 53 bit numbers is 106 bits and MUL puts every
    //       one of them in rdx:rax, which is the entire reason this is done in
    //       the general registers. It is normalised so its top bit is bit 105,
    //       and then placed with that bit at 127 of a 128 bit accumulator.
    //       The addend is placed the same way, its top bit at 127.
    "15: mov %rax, %r11\n   mov %rsi, %rax\n   mul %r11\n"
    "    add %r9d, %r8d  # the product's exponent\n"
    "    bt $41, %rdx\n   jc 16f\n   shld $1, %rax, %rdx\n   shl $1, %rax\n   dec %r8d\n"
    "16: mov %rdx, %r11\n   mov %rax, %rsi\n   shld $22, %rsi, %r11\n   shl $22, %rsi\n"
    "    mov %rdi, %r9\n   shl $11, %r9\n   xor %eax, %eax\n"

    //       Whichever of the two sits higher keeps its place and the other is
    //       shifted down to meet it, with one bit remembering whether anything
    //       fell off the bottom. Swapping them here means the shift is written
    //       once. The two frames are 22 below the product's exponent and 75
    //       below the addend's, so the distance between them is that plus 53.
    "    mov %r8d, %edx\n   sub %r10d, %edx\n   add $53, %edx\n"
    "    sub $22, %r8d\n   sub $75, %r10d\n"
    "    test %edx, %edx\n   jns 17f\n"
    "    xchg %r11, %r9\n   xchg %rsi, %rax\n   xchg %rbx, %rbp\n   mov %r10d, %r8d\n   neg %edx\n"
    "17: mov %edx, %ecx\n   xor %edi, %edi\n"
    "    test %ecx, %ecx\n   jz 25f\n"
    "    cmp $128, %ecx\n   jl 20f\n"
    "    mov $1, %edi\n   xor %eax, %eax\n   xor %r9d, %r9d\n   jmp 25f\n"
    "20: cmp $64, %ecx\n   jl 22f\n"
    "    sub $64, %ecx\n   mov $1, %r10\n   shl %cl, %r10\n   dec %r10\n"
    "    and %r9, %r10\n   or %rax, %r10\n   setne %dil\n"
    "    mov %r9, %rax\n   shr %cl, %rax\n   xor %r9d, %r9d\n   jmp 25f\n"
    "22: mov $1, %r10\n   shl %cl, %r10\n   dec %r10\n   and %rax, %r10\n   setne %dil\n"
    "    shrd %cl, %r9, %rax\n   shr %cl, %r9\n"

    //       Like signs add and unlike signs subtract. The shifted one is the
    //       smaller whenever anything fell off it -- both accumulators carry
    //       at least 22 low zeros, so a shift that loses a bit is a shift of
    //       at least 23, and the two are then nowhere near each other -- which
    //       is why the borrow for those lost bits is always taken from the
    //       larger, and why a total cancellation always has nothing lost.
    "25: cmp %rbx, %rbp\n   jne 30f\n"
    "    add %rax, %rsi\n   adc %r9, %r11\n   jnc 35f\n"
    "    mov %esi, %edx\n   and $1, %edx\n   or %dl, %dil\n"
    "    shrd $1, %r11, %rsi\n   shr $1, %r11\n   bts $63, %r11\n   inc %r8d\n   jmp 35f\n"
    "30: cmp %r9, %r11\n   jne 31f\n   cmp %rax, %rsi\n"
    "31: jb 32f\n   ja 33f\n"
    "    pxor %xmm0, %xmm0  # they cancelled exactly, and that zero is positive\n"
    "    pop %rbp\n   pop %rbx\n"
    ASM_RET
    "33: sub %rax, %rsi\n   sbb %r9, %r11\n   jmp 34f\n"
    "32: sub %rsi, %rax\n   sbb %r11, %r9\n   mov %rax, %rsi\n   mov %r9, %r11\n   mov %rbp, %rbx\n"
    "34: test %edi, %edi\n   jz 35f\n   sub $1, %rsi\n   sbb $0, %r11\n"

    //       What is left is a 128 bit magnitude, a frame exponent and one bit
    //       saying there is more below. Its top bit says where the answer's
    //       exponent is, and how many low bits have to go; a result too small
    //       to be normal has its own floor at 2^-1074 instead, which is what
    //       makes a subnormal answer come out right rather than nearly right.
    //       That floor also bounds the drop at 127 places, because the addend
    //       frame is never below -1201, so no shift here is ever undefined.
    "35: bsr %r11, %rcx\n   jz 40f\n   add $64, %ecx\n   jmp 41f\n"
    "40: bsr %rsi, %rcx\n"
    "41: mov %r8d, %edx\n   add %ecx, %edx\n   mov %edx, %r10d\n   sub $52, %r10d\n"
    "    cmp $-1074, %r10d\n   jge 42f\n   mov $-1074, %r10d\n"
    "42: mov %r10d, %r9d\n   sub %r8d, %r9d  # how many bits fall below the answer\n"
    "    test %r9d, %r9d\n   jg 45f\n"
    "    mov %r9d, %ecx\n   neg %ecx\n   mov %rsi, %rax\n   shl %cl, %rax\n"
    "    xor %edx, %edx\n   jmp 50f\n"
    "45: mov %r9d, %ecx\n   dec %ecx\n   cmp $64, %ecx\n   jge 47f\n"
    "    mov $1, %rbp\n   shl %cl, %rbp\n   dec %rbp\n   and %rsi, %rbp\n"
    "    shrd %cl, %r11, %rsi\n   mov %rsi, %rax\n   jmp 48f\n"
    "47: sub $64, %ecx\n   mov $1, %rbp\n   shl %cl, %rbp\n   dec %rbp\n   and %r11, %rbp\n"
    "    or %rsi, %rbp\n   mov %r11, %rax\n   shr %cl, %rax\n"
    "48: mov %eax, %edx\n   and $1, %edx  # the bit the answer is rounded on\n"
    "    shr $1, %rax\n   xor %r11d, %r11d\n   test %rbp, %rbp\n   setne %r11b\n   or %r11d, %edi\n"

    //       To nearest, and a tie to the even one.
    "50: test %edx, %edx\n   jz 51f\n   test %edi, %edi\n   jnz 52f\n   test $1, %al\n   jz 51f\n"
    "52: add $1, %rax\n"
    "51: cmp $-1074, %r10d\n   jne 60f\n   mov %rax, %rdx\n   jmp 65f\n"
    "60: mov %r10d, %ecx\n   add $52, %ecx\n"
    "    mov %rax, %rdx\n   shr $53, %rdx\n   jz 61f\n   shr $1, %rax\n   inc %ecx\n"
    "61: add $1023, %ecx\n   cmp $2047, %ecx\n   jl 62f\n"
    "    mov $0x7ff0000000000000, %rdx\n   jmp 65f\n"
    "62: mov %ecx, %edx\n   shl $52, %rdx\n   mov $0xfffffffffffff, %rcx\n   and %rcx, %rax\n   or %rax, %rdx\n"
    "65: shl $63, %rbx\n   or %rbx, %rdx\n   movq %rdx, %xmm0\n"
    "    pop %rbp\n   pop %rbx\n"
    ASM_RET
    ASM_END(decimal_multiply_add)
#endif

    // ------------------------------------------------------------
    //       float-single
    // ------------------------------------------------------------
#ifndef KERNEL_MODE
    ASM_FUNC(narrow_square_root)
    "sqrtss %xmm0, %xmm0\n"
    ASM_RET
    ASM_END(narrow_square_root)

    //
    //       All ones shifted down one place is the magnitude mask, and
    //       building it costs two instructions that do not depend on the
    //       argument at all -- against one instruction that depends on a
    //       cache line, which is what the compiler emits, and against the
    //       three that go out through a general register and back, which is
    //       two crossings of the domain boundary.
    //
    ASM_FUNC(narrow_absolute)
    "pcmpeqd %xmm1, %xmm1\n   psrld $1, %xmm1\n   andps %xmm1, %xmm0\n"
    ASM_RET
    ASM_END(narrow_absolute)

    //
    //       minss answers with its second operand whenever the comparison
    //       comes out false, and a NaN on either side makes it false. So the
    //       raw instruction returns the NaN where C wants the number, and it
    //       does so in exactly one case: the minimum is a NaN if and only if
    //       the second argument is one. One quiet unordered compare of the
    //       second argument against itself is therefore the whole condition,
    //       and the select is the usual and/andn/or, branchless because the
    //       branch here would be on data no predictor can learn.
    //
    ASM_FUNC(narrow_smaller)
    "movaps %xmm0, %xmm2\n   minss %xmm1, %xmm0\n   cmpunordss %xmm1, %xmm1\n"
    "andps %xmm1, %xmm2\n   andnps %xmm0, %xmm1\n   orps %xmm2, %xmm1\n"
    "movaps %xmm1, %xmm0\n"
    ASM_RET
    ASM_END(narrow_smaller)

    ASM_FUNC(narrow_larger)
    "movaps %xmm0, %xmm2\n   maxss %xmm1, %xmm0\n   cmpunordss %xmm1, %xmm1\n"
    "andps %xmm1, %xmm2\n   andnps %xmm0, %xmm1\n   orps %xmm2, %xmm1\n"
    "movaps %xmm1, %xmm0\n"
    ASM_RET
    ASM_END(narrow_larger)

    //
    //       Bit three of the immediate suppresses the precision exception
    //       and the low two bits are the direction: eleven truncates, nine
    //       rounds down, ten rounds up.
    //
    ASM_FUNC(narrow_truncated)
    "roundss $0x0b, %xmm0, %xmm0\n"
    ASM_RET
    ASM_END(narrow_truncated)

    ASM_FUNC(narrow_floor)
    "roundss $0x09, %xmm0, %xmm0\n"
    ASM_RET
    ASM_END(narrow_floor)

    ASM_FUNC(narrow_ceiling)
    "roundss $0x0a, %xmm0, %xmm0\n"
    ASM_RET
    ASM_END(narrow_ceiling)

    //
    //       The fourth direction is the one the immediate does not have, so
    //       it is arithmetic: put the argument's sign on 0x1.fffffep-2, the
    //       float one step below a half, add it, and truncate. The sign
    //       transplant is a mask and an or in a general register, which is
    //       where the constant has to be assembled anyway, so the crossing
    //       back into a vector register is paid once and no memory is read.
    //       Why the constant is one step below a half and not a half is
    //       written out above the declarations, along with the one thing
    //       this owes the caller: roundss ignores the dynamic rounding mode
    //       but addss does not, so this wants MXCSR at nearest, which is
    //       where it starts and where nothing here moves it.
    //
    ASM_FUNC(narrow_rounded)
    "movd %xmm0, %eax\n   and $0x80000000, %eax\n   or $0x3effffff, %eax\n"
    "movd %eax, %xmm1\n   addss %xmm1, %xmm0\n   roundss $0x0b, %xmm0, %xmm0\n"
    ASM_RET
    ASM_END(narrow_rounded)

    //
    //       The same self-made mask as the absolute value, used from both
    //       sides: and takes the magnitude out of the first argument and
    //       andn takes the sign bit out of the second, so the one constant
    //       serves twice and the answer lands where the caller expects it.
    //
    ASM_FUNC(narrow_with_sign)
    "pcmpeqd %xmm2, %xmm2\n   psrld $1, %xmm2\n   andps %xmm2, %xmm0\n"
    "andnps %xmm1, %xmm2\n   orps %xmm2, %xmm0\n"
    ASM_RET
    ASM_END(narrow_with_sign)
#endif
);
#elif ARM64
__asm__(
    ASM_SECTION

    ASM_FUNC(byte_is_digit)
    "sub w1, w0, #48\n   cmp w1, #10\n   cset w0, lo\n"
    ASM_RET
    ASM_END(byte_is_digit)

    ASM_FUNC(byte_is_upper)
    "sub w1, w0, #65\n   cmp w1, #26\n   cset w0, lo\n"
    ASM_RET
    ASM_END(byte_is_upper)

    ASM_FUNC(byte_is_lower)
    "sub w1, w0, #97\n   cmp w1, #26\n   cset w0, lo\n"
    ASM_RET
    ASM_END(byte_is_lower)

    ASM_FUNC(byte_is_alpha)
    "orr w1, w0, #32\n   sub w1, w1, #97\n   cmp w1, #26\n   cset w0, lo\n"
    ASM_RET
    ASM_END(byte_is_alpha)

    //
    //       ccmp chains the two range tests through the flags instead of
    //       building an answer for each and or-ing them: the letter test runs
    //       first, and the digit test runs only where it failed. Where it did
    //       not fail, ccmp writes all four flags clear by hand, and a clear
    //       carry is the "lower or same" the cset then reads. Both
    //       subtractions come first because ccmp needs its operand ready and
    //       must not have the flags disturbed under it.
    //
    ASM_FUNC(byte_is_alnum)
    "orr w1, w0, #32\n   sub w0, w0, #48\n   sub w1, w1, #97\n"
    "cmp w1, #25\n   ccmp w0, #9, #0, hi\n   cset w0, ls\n"
    ASM_RET
    ASM_END(byte_is_alnum)

    //
    //       The space is the only one outside the tab through carriage return
    //       run, so its equality goes first and the range runs only where
    //       that missed.
    //
    ASM_FUNC(byte_is_space)
    "sub w1, w0, #9\n   cmp w0, #32\n   ccmp w1, #4, #0, ne\n   cset w0, ls\n"
    ASM_RET
    ASM_END(byte_is_space)

    ASM_FUNC(byte_is_hexadecimal)
    "orr w1, w0, #32\n   sub w0, w0, #48\n   sub w1, w1, #97\n"
    "cmp w1, #5\n   ccmp w0, #9, #0, hi\n   cset w0, ls\n"
    ASM_RET
    ASM_END(byte_is_hexadecimal)

    //
    //       The second operand of a subtract may be shifted on the way in, so
    //       the one or zero becomes thirty two or zero inside the subtract
    //       itself and the separate lsl is not spent.
    //
    ASM_FUNC(byte_to_upper)
    "sub w1, w0, #97\n   cmp w1, #26\n   cset w1, lo\n"
    "sub w0, w0, w1, lsl #5\n"
    ASM_RET
    ASM_END(byte_to_upper)

    ASM_FUNC(byte_to_lower)
    "sub w1, w0, #65\n   cmp w1, #26\n   cset w1, lo\n"
    "add w0, w0, w1, lsl #5\n"
    ASM_RET
    ASM_END(byte_to_lower)

    //
    //       cneg is the whole of it here: negate when the flags say minus.
    //
    ASM_FUNC(absolute_whole)
    "cmp w0, #0\n   cneg w0, w0, mi\n"
    ASM_RET
    ASM_END(absolute_whole)

    ASM_FUNC(absolute_wide)
    "cmp x0, #0\n   cneg x0, x0, mi\n"
    ASM_RET
    ASM_END(absolute_wide)

#ifndef KERNEL_MODE
    ASM_FUNC(square_root)
    "fsqrt d0, d0\n"
    ASM_RET
    ASM_END(square_root)

    ASM_FUNC(absolute)
    "fabs d0, d0\n"
    ASM_RET
    ASM_END(absolute)
#endif

    // ------------------------------------------------------------
    //       ctype-rest
    // ------------------------------------------------------------

    ASM_FUNC(byte_is_printable)
    "sub w0, w0, #32\n   cmp w0, #95\n   cset w0, lo\n"
    ASM_RET
    ASM_END(byte_is_printable)

    ASM_FUNC(byte_is_graphic)
    "sub w0, w0, #33\n   cmp w0, #94\n   cset w0, lo\n"
    ASM_RET
    ASM_END(byte_is_graphic)

    //
    //       ccmp is what this architecture has instead of a second answer:
    //       compare against the delete byte, and where that did not match,
    //       compare against thirty one instead of it. Where it did match,
    //       all four flags are written by hand as clear, and a clear carry is
    //       the same "lower or same" the second compare would have produced
    //       for a byte below thirty two.
    //
    ASM_FUNC(byte_is_control)
    "cmp w0, #127\n   ccmp w0, #31, #0, ne\n   cset w0, ls\n"
    ASM_RET
    ASM_END(byte_is_control)

    //
    //       Three range tests joined by ccmp, which is an and of conditions
    //       and exactly the shape of the definition: graphic, and then not a
    //       digit, and then not a letter. Each link hands on the carry flag,
    //       and a link that is not reached writes all four flags clear, which
    //       fails the carry the next one reads. The three subtractions are
    //       done first because ccmp needs its operand ready and none of them
    //       may disturb the flags.
    //
    ASM_FUNC(byte_is_punctuation)
    "sub w1, w0, #33\n   sub w2, w0, #48\n"
    "orr w3, w0, #32\n   sub w3, w3, #97\n"
    "cmp w1, #94\n   ccmp w2, #10, #0, lo\n   ccmp w3, #26, #0, hs\n"
    "cset w0, hs\n"
    ASM_RET
    ASM_END(byte_is_punctuation)

    ASM_FUNC(byte_is_blank)
    "cmp w0, #32\n   ccmp w0, #9, #4, ne\n   cset w0, eq\n"
    ASM_RET
    ASM_END(byte_is_blank)

    ASM_FUNC(byte_is_ascii)
    "cmp w0, #128\n   cset w0, lo\n"
    ASM_RET
    ASM_END(byte_is_ascii)

    ASM_FUNC(byte_to_ascii)
    "and w0, w0, #127\n"
    ASM_RET
    ASM_END(byte_to_ascii)

    // ------------------------------------------------------------
    //       set-scan
    // ------------------------------------------------------------
    // The x86_64 block carries the reasoning; these are the same two loops in
    // AAPCS64. ubfx lifts the word index out in one instruction and lsl by a
    // register already takes its count modulo sixty four.
    ASM_FUNC(string_span_of_set)
    "sub sp, sp, #32\n"
    "stp xzr, xzr, [sp]\n   stp xzr, xzr, [sp, #16]\n"
    "mov x4, #1\n   mov x2, xzr\n"
    "ldrb w3, [x1]\n   cbz w3, 2f\n"
    "1:  ubfx x5, x3, #6, #2\n   lsl x6, x4, x3\n   ldr x7, [sp, x5, lsl #3]\n"
    "orr x7, x7, x6\n   str x7, [sp, x5, lsl #3]\n   ldrb w3, [x1, #1]!\n   cbnz w3, 1b\n"
    "2:  b 4f\n"
    "3:  add x2, x2, #1\n"
    "4:  ldrb w3, [x0, x2]\n   ubfx x5, x3, #6, #2\n   ldr x7, [sp, x5, lsl #3]\n"
    "lsr x7, x7, x3\n   tbnz w7, #0, 3b\n"
    "mov x0, x2\n   add sp, sp, #32\n"
    ASM_RET
    ASM_END(string_span_of_set)

    // The one bit of difference is in the first store: the pair puts a set bit
    // zero and a clear word one down together.
    ASM_FUNC(string_span_without_set)
    "sub sp, sp, #32\n"
    "mov x4, #1\n"
    "stp x4, xzr, [sp]\n   stp xzr, xzr, [sp, #16]\n"
    "mov x2, xzr\n"
    "ldrb w3, [x1]\n   cbz w3, 2f\n"
    "1:  ubfx x5, x3, #6, #2\n   lsl x6, x4, x3\n   ldr x7, [sp, x5, lsl #3]\n"
    "orr x7, x7, x6\n   str x7, [sp, x5, lsl #3]\n   ldrb w3, [x1, #1]!\n   cbnz w3, 1b\n"
    "2:  b 4f\n"
    "3:  add x2, x2, #1\n"
    "4:  ldrb w3, [x0, x2]\n   ubfx x5, x3, #6, #2\n   ldr x7, [sp, x5, lsl #3]\n"
    "lsr x7, x7, x3\n   tbz w7, #0, 3b\n"
    "mov x0, x2\n   add sp, sp, #32\n"
    ASM_RET
    ASM_END(string_span_without_set)

    // w3 still holds the byte that stopped the walk, so csel turns "it was the
    // terminator" into null without touching the source again.
    ASM_FUNC(string_first_of_set)
    "sub sp, sp, #32\n"
    "mov x4, #1\n"
    "stp x4, xzr, [sp]\n   stp xzr, xzr, [sp, #16]\n"
    "mov x2, x0\n"
    "ldrb w3, [x1]\n   cbz w3, 2f\n"
    "1:  ubfx x5, x3, #6, #2\n   lsl x6, x4, x3\n   ldr x7, [sp, x5, lsl #3]\n"
    "orr x7, x7, x6\n   str x7, [sp, x5, lsl #3]\n   ldrb w3, [x1, #1]!\n   cbnz w3, 1b\n"
    "2:  b 4f\n"
    "3:  add x2, x2, #1\n"
    "4:  ldrb w3, [x2]\n   ubfx x5, x3, #6, #2\n   ldr x7, [sp, x5, lsl #3]\n"
    "lsr x7, x7, x3\n   tbz w7, #0, 3b\n"
    "cmp w3, #0\n   csel x0, x2, xzr, ne\n   add sp, sp, #32\n"
    ASM_RET
    ASM_END(string_first_of_set)

    // ------------------------------------------------------------
    //       case-compare
    // ------------------------------------------------------------
    //      The x86 block carries the reasoning for all three. csel on ls is
    //      the unsigned range test there written as a select here.
    ASM_FUNC(string_compare_folded)
    "ldrb w4, [x0]\n   ldrb w5, [x1]\n"
    "sub w6, w4, #0x41  // 'A'\n   add w7, w4, #32\n   cmp w6, #25\n   csel w4, w7, w4, ls\n"
    "sub w6, w5, #0x41  // 'A'\n   add w7, w5, #32\n   cmp w6, #25\n   csel w5, w7, w5, ls\n"
    "subs w9, w4, w5\n   b.ne 9f\n"
    "cbz w5, 9f\n"
    "add x0, x0, #1\n   add x1, x1, #1\n"
#ifndef KERNEL_MODE
    //      NEON has the unsigned compare x86 has to reach through a bias for,
    //      so the range test is the subtract and cmhs straight out: twenty
    //      five at or above the byte minus 'A' is 'A' through 'Z' and nothing
    //      else, high bit set or not. A kernel build gets the byte loop
    //      below, the way the arm64 block of string_compare_max does.
    "movi v5.16b, #25\n   movi v6.16b, #0x41  // 'A'\n   movi v7.16b, #0x20\n"
    "10:  and x10, x0, #0xfff\n   cmp x10, #0xff0\n   b.hi 1f\n"
    "and x10, x1, #0xfff\n   cmp x10, #0xff0\n   b.hi 1f\n"
    "ldr q0, [x0]\n   ldr q1, [x1]\n"
    "sub v2.16b, v0.16b, v6.16b\n   cmhs v2.16b, v5.16b, v2.16b\n"
    "and v2.16b, v2.16b, v7.16b\n   add v0.16b, v0.16b, v2.16b\n"
    "sub v2.16b, v1.16b, v6.16b\n   cmhs v2.16b, v5.16b, v2.16b\n"
    "and v2.16b, v2.16b, v7.16b\n   add v1.16b, v1.16b, v2.16b\n"
    "cmeq v3.16b, v0.16b, v1.16b\n   cmeq v2.16b, v0.16b, #0\n   bic v3.16b, v3.16b, v2.16b\n"
    "uminv b4, v3.16b\n   umov w10, v4.b[0]\n   cmp w10, #0xff\n   b.ne 11f\n"
    "add x0, x0, #16\n   add x1, x1, #16\n   b 10b\n"
    //      Which byte stopped it: four bits a byte through shrn, then the
    //      lowest set bit, which is the pair string_compare_max uses.
    "11:  mvn v3.16b, v3.16b\n   shrn v3.8b, v3.8h, #4\n   fmov x10, d3\n"
    "rbit x10, x10\n   clz x10, x10\n   lsr x10, x10, #2\n"
    "add x0, x0, x10\n   add x1, x1, x10\n"
#endif
    "1:  mov w11, #16\n"
    "2:  ldrb w4, [x0]\n   ldrb w5, [x1]\n"
    "sub w6, w4, #0x41\n   add w7, w4, #32\n   cmp w6, #25\n   csel w4, w7, w4, ls\n"
    "sub w6, w5, #0x41\n   add w7, w5, #32\n   cmp w6, #25\n   csel w5, w7, w5, ls\n"
    "subs w9, w4, w5\n   b.ne 9f\n"
    "cbz w5, 9f\n"
    "add x0, x0, #1\n   add x1, x1, #1\n   subs w11, w11, #1\n   b.ne 2b\n"
#ifndef KERNEL_MODE
    "b 10b\n"
#endif
    "b 1b  // a kernel build has no wide body to go back to\n"
    "9:  mov w0, w9\n"
    ASM_RET
    ASM_END(string_compare_folded)
    //      n bounds the read, so nothing here asks about a page.
    ASM_FUNC(string_compare_folded_max)
    "mov w9, #0\n   cbz x2, 9f\n"
#ifndef KERNEL_MODE
    "cmp x2, #16\n   b.lo 1f\n"
    "movi v5.16b, #25\n   movi v6.16b, #0x41  // 'A'\n   movi v7.16b, #0x20\n"
    "10:  ldr q0, [x0]\n   ldr q1, [x1]\n"
    "sub v2.16b, v0.16b, v6.16b\n   cmhs v2.16b, v5.16b, v2.16b\n"
    "and v2.16b, v2.16b, v7.16b\n   add v0.16b, v0.16b, v2.16b\n"
    "sub v2.16b, v1.16b, v6.16b\n   cmhs v2.16b, v5.16b, v2.16b\n"
    "and v2.16b, v2.16b, v7.16b\n   add v1.16b, v1.16b, v2.16b\n"
    "cmeq v3.16b, v0.16b, v1.16b\n   cmeq v2.16b, v0.16b, #0\n   bic v3.16b, v3.16b, v2.16b\n"
    "uminv b4, v3.16b\n   umov w10, v4.b[0]\n   cmp w10, #0xff\n   b.ne 11f\n"
    "add x0, x0, #16\n   add x1, x1, #16\n   sub x2, x2, #16\n"
    "cmp x2, #16\n   b.hs 10b\n   b 1f\n"
    "11:  mvn v3.16b, v3.16b\n   shrn v3.8b, v3.8h, #4\n   fmov x10, d3\n"
    "rbit x10, x10\n   clz x10, x10\n   lsr x10, x10, #2\n"
    "add x0, x0, x10\n   add x1, x1, x10\n   sub x2, x2, x10\n"
#endif
    //      Counting up from minus n, for the reason the x86 block gives.
    "1:  mov w9, #0\n   cbz x2, 9f\n"
    "add x0, x0, x2\n   add x1, x1, x2\n   neg x3, x2\n"
    "2:  ldrb w4, [x0, x3]\n   ldrb w5, [x1, x3]\n"
    "sub w6, w4, #0x41\n   add w7, w4, #32\n   cmp w6, #25\n   csel w4, w7, w4, ls\n"
    "sub w6, w5, #0x41\n   add w7, w5, #32\n   cmp w6, #25\n   csel w5, w7, w5, ls\n"
    "subs w9, w4, w5\n   b.ne 9f\n"
    "cbz w5, 9f\n"
    "adds x3, x3, #1\n   b.ne 2b\n"
    "9:  mov w0, w9\n"
    ASM_RET
    ASM_END(string_compare_folded_max)

    // ------------------------------------------------------------
    //       memory-rest
    // ------------------------------------------------------------
    //       memccpy: the x86_64 block carries the reasoning.
    ASM_FUNC(memory_copy_until)
    "stp x29, x30, [sp, #-48]!\n   mov x29, sp\n"
    "stp x19, x20, [sp, #16]\n   str x21, [sp, #32]\n"
    "mov x19, x0  // the destination\n   mov x20, x1  // the source\n"
    "mov x21, x3  // the size\n"
    "mov x0, x1\n   mov x1, x2\n   mov x2, x3\n   bl memory_first_of\n"
    "mov x2, x21  // a byte that is not there copies the whole span\n"
    "cbnz x0, 2f\n"
    "mov x21, #0  // and answers null\n   b 1f\n"
    "2:  sub x0, x0, x20\n   add x2, x0, #1  // everything up to and including it\n"
    "add x21, x19, x2  // and the answer is just past what was copied\n"
    "1:  mov x0, x19\n   mov x1, x20\n   bl memory_copy_apart\n"
    "mov x0, x21\n"
    "ldr x21, [sp, #32]\n   ldp x19, x20, [sp, #16]\n   ldp x29, x30, [sp], #48\n"
    ASM_RET
    ASM_END(memory_copy_until)
    //       stpcpy: the x86_64 block carries the reasoning.
    ASM_FUNC(string_copy_end)
    "stp x29, x30, [sp, #-32]!\n   mov x29, sp\n   stp x19, x20, [sp, #16]\n"
    "mov x19, x0  // the destination\n   mov x20, x1  // the source\n"
    "mov x0, x1\n   bl string_length\n"
    "mov x1, x20\n   add x20, x19, x0  // where the terminator lands, and the answer\n"
    "add x2, x0, #1  // the terminator is copied too\n   mov x0, x19\n"
    "bl memory_copy_apart\n   mov x0, x20\n"
    "ldp x19, x20, [sp, #16]\n   ldp x29, x30, [sp], #32\n"
    ASM_RET
    ASM_END(string_copy_end)
    //       stpncpy: the x86_64 block carries the reasoning, padding rule and all.
    ASM_FUNC(string_copy_max_endptr)
    "stp x29, x30, [sp, #-48]!\n   mov x29, sp\n"
    "stp x19, x20, [sp, #16]\n   str x21, [sp, #32]\n"
    "mov x19, x0  // the destination\n   mov x20, x1  // the source\n"
    "mov x21, x2  // the bound\n"
    "mov x0, x1\n   mov x1, x2\n   bl string_length_max\n"
    "mov x1, x20\n   mov x2, x0\n"
    "add x20, x19, x0  // the first byte the source did not fill, and the answer\n"
    "sub x21, x21, x0  // what is left of the bound is padding\n"
    "mov x0, x19\n   bl memory_copy_apart\n"
    "cbz x21, 1f\n"
    "mov x0, x20\n   mov x1, #0\n   mov x2, x21\n   bl memory_fill\n"
    "1:  mov x0, x20\n"
    "ldr x21, [sp, #32]\n   ldp x19, x20, [sp, #16]\n   ldp x29, x30, [sp], #48\n"
    ASM_RET
    ASM_END(string_copy_max_endptr)
    //       strncat: the x86_64 block carries the reasoning.
    ASM_FUNC(string_append_max)
    "stp x29, x30, [sp, #-48]!\n   mov x29, sp\n"
    "stp x19, x20, [sp, #16]\n   str x21, [sp, #32]\n"
    "mov x19, x0  // the destination\n   mov x20, x1  // the source\n"
    "mov x21, x2  // the bound\n"
    "bl string_length  // the destination is already in x0\n"
    "add x0, x19, x0\n   mov x1, x20\n   mov x2, x21\n   bl string_copy_max_end\n"
    "mov x0, x19  // strncat hands the destination back\n"
    "ldr x21, [sp, #32]\n   ldp x19, x20, [sp, #16]\n   ldp x29, x30, [sp], #48\n"
    ASM_RET
    ASM_END(string_append_max)
    //       bzero and bcopy: the x86_64 block carries the reasoning.
    ASM_FUNC(memory_zero)
    "mov x2, x1\n   mov x1, #0\n   b memory_fill\n"
    ASM_END(memory_zero)
    ASM_FUNC(memory_copy_source_first)
    "mov x3, x0\n   mov x0, x1\n   mov x1, x3\n   b memory_copy\n"
    ASM_END(memory_copy_source_first)

    // ------------------------------------------------------------
    //       integer-parse
    // ------------------------------------------------------------
    ASM_FUNC(string_to_number)
    "mov x11, xzr\n   b string_to_number_core\n"
    ASM_END(string_to_number)

    ASM_FUNC(string_to_number_unsigned)
    "mov x11, #2\n   b string_to_number_core\n"
    ASM_END(string_to_number_unsigned)

#ifndef KERNEL_MODE
    ASM_FUNC(string_to_number_checked)
    "stp x3, x30, [sp, #-16]!\n   mov x5, xzr\n"
    "bl string_to_number\n   ldp x3, x30, [sp], #16\n"
    "cbz x3, .Lnumber_a64_checked_done\n"
    "cmp x5, #0\n   cset w4, ne\n   str w4, [x3]\n"
    ".Lnumber_a64_checked_done:\n"
    ASM_RET
    ASM_END(string_to_number_checked)

    ASM_FUNC(string_to_number_unsigned_checked)
    "stp x3, x30, [sp, #-16]!\n   mov x5, xzr\n"
    "bl string_to_number_unsigned\n   ldp x3, x30, [sp], #16\n"
    "cbz x3, .Lnumber_a64_unsigned_checked_done\n"
    "cmp x5, #0\n   cset w4, ne\n   str w4, [x3]\n"
    ".Lnumber_a64_unsigned_checked_done:\n"
    ASM_RET
    ASM_END(string_to_number_unsigned_checked)
#endif

    ASM_FUNC(string_to_whole_wide)
    "mov x1, xzr\n   mov w2, #10\n   mov x11, xzr\n"
    "b string_to_number_core\n"
    ASM_END(string_to_whole_wide)

    //
    //       atoi is atol read as an int, and AAPCS64 leaves the top half of
    //       x0 unspecified for that, so this is the same tail branch. The
    //       compiler builds a frame and calls instead.
    //
    ASM_FUNC(string_to_whole)
    "mov x1, xzr\n   mov w2, #10\n   mov x11, xzr\n"
    "b string_to_number_core\n"
    ASM_END(string_to_whole)

    //
    //       string_to_number_core: the x86_64 block carries the reasoning.
    //       x11 is the flag word the wrappers set, x3 walks the input, x4 is
    //       the base, x5 is the sticky overflow and x0 carries the input
    //       pointer until the first digit is taken and the running value
    //       afterwards.
    //
    ASM_LOCAL_FUNC(string_to_number_core)
    "mov w4, w2  // the base, out of the half an int owns\n"
    "sub w8, w4, #2\n   cmp w8, #34\n   b.ls .Lnumber_a64_start\n"
    "cbnz w4, .Lnumber_a64_none  // not zero and not 2..36\n"
    ".Lnumber_a64_start:\n   mov x3, x0\n"
    ".Lnumber_a64_space:\n   ldrb w7, [x3], #1\n   cmp w7, #32\n"
    "b.eq .Lnumber_a64_space\n"
    "sub w8, w7, #9\n   cmp w8, #4\n   b.ls .Lnumber_a64_space\n"
    "sub x3, x3, #1\n"
    "cmp w7, #45\n   b.eq .Lnumber_a64_minus\n"
    "cmp w7, #43\n   b.ne .Lnumber_a64_base\n"
    "add x3, x3, #1\n   b .Lnumber_a64_base\n"
    ".Lnumber_a64_minus:\n   orr x11, x11, #1\n   add x3, x3, #1\n"
    ".Lnumber_a64_base:\n   cbnz w4, .Lnumber_a64_given\n"
    "mov w4, #10\n   ldrb w7, [x3]\n   cmp w7, #48\n"
    "b.ne .Lnumber_a64_first\n"
    "mov w4, #8\n   ldrb w7, [x3, #1]\n   orr w7, w7, #32\n   cmp w7, #120\n"
    "b.ne .Lnumber_a64_first\n"
    "mov w4, #16\n   b .Lnumber_a64_prefix\n"
    ".Lnumber_a64_given:\n   cmp w4, #16\n   b.ne .Lnumber_a64_first\n"
    "ldrb w7, [x3]\n   cmp w7, #48\n   b.ne .Lnumber_a64_first\n"
    "ldrb w7, [x3, #1]\n   orr w7, w7, #32\n   cmp w7, #120\n"
    "b.ne .Lnumber_a64_first\n"
    ".Lnumber_a64_prefix:\n   ldrb w7, [x3, #2]\n   sub w8, w7, #48\n"
    "cmp w8, #9\n   b.ls .Lnumber_a64_take\n"
    "orr w7, w7, #32\n   sub w8, w7, #97\n   cmp w8, #5\n"
    "b.hi .Lnumber_a64_first\n"
    ".Lnumber_a64_take:\n   add x3, x3, #2\n"
    ".Lnumber_a64_first:\n   ldrb w7, [x3], #1\n   sub w6, w7, #48\n   cmp w6, #9\n"
    "b.ls .Lnumber_a64_first_value\n"
    "orr w7, w7, #32\n   sub w6, w7, #97\n   cmp w6, #25\n"
    "b.hi .Lnumber_a64_none\n   add w6, w6, #10\n"
    ".Lnumber_a64_first_value:\n   cmp w6, w4\n   b.hs .Lnumber_a64_none\n"
    "mov x0, x6\n   mov x5, xzr\n"
    //
    //       umulh is the half a 64 bit multiply throws away, which is the
    //       overflow, and adc folds the carry out of the digit into it.
    //
    //       The load advances the pointer itself, the way the whitespace
    //       scan above already does, so a digit is twelve instructions and
    //       not thirteen. The one correction that costs is the sub at
    //       .Lnumber_a64_stop, which runs once: nothing before it reads x3,
    //       and .Lnumber_a64_none answers with x0 -- the input as it came
    //       in -- so an overshot pointer is invisible on that path.
    //
    ".Lnumber_a64_loop:\n   ldrb w7, [x3], #1\n   sub w6, w7, #48\n   cmp w6, #9\n"
    "b.ls .Lnumber_a64_value\n"
    "orr w7, w7, #32\n   sub w6, w7, #97\n   cmp w6, #25\n"
    "b.hi .Lnumber_a64_stop\n   add w6, w6, #10\n"
    ".Lnumber_a64_value:\n   cmp w6, w4\n   b.hs .Lnumber_a64_stop\n"
    "umulh x8, x0, x4\n   mul x0, x0, x4\n   adds x0, x0, x6\n"
    "adc x8, x8, xzr\n   orr x5, x5, x8\n"
    "b .Lnumber_a64_loop\n"
    ".Lnumber_a64_stop:\n   sub x3, x3, #1\n   cbz x1, .Lnumber_a64_kept\n   str x3, [x1]\n"
    //
    //       The signed limit is also where x5 becomes the checked entry's
    //       answer. Branching to the cold clamp leaves the ordinary path one
    //       instruction shorter than the former pair of conditional selects,
    //       and gives the wrapper the range fact without another digit pass.
    //
    ".Lnumber_a64_kept:\n   tbnz x11, #1, .Lnumber_a64_unsigned\n"
    "mov x8, #0x7fffffffffffffff\n   and x9, x11, #1\n   add x8, x8, x9\n"
    "cmp x0, x8\n   b.hi .Lnumber_a64_clamp\n"
    "cbnz x5, .Lnumber_a64_clamp\n"
    "tbz x11, #0, .Lnumber_a64_done\n   neg x0, x0\n"
    ASM_RET
    ".Lnumber_a64_clamp:\n   mov x0, x8\n   mov x5, #1\n"
    "tbz x11, #0, .Lnumber_a64_done\n   neg x0, x0\n"
    ASM_RET
    ".Lnumber_a64_unsigned:\n   cbnz x5, .Lnumber_a64_all_ones\n"
    "tbz x11, #0, .Lnumber_a64_done\n   neg x0, x0\n"
    ASM_RET
    ".Lnumber_a64_all_ones:\n   mov x0, #-1\n"
    ".Lnumber_a64_done:\n"
    ASM_RET
    ".Lnumber_a64_none:\n   cbz x1, .Lnumber_a64_none_done\n   str x0, [x1]\n"
    ".Lnumber_a64_none_done:\n   mov x0, xzr\n"
    ASM_RET
    ASM_LOCAL_END(string_to_number_core)

    // ------------------------------------------------------------
    //       bit-count
    // ------------------------------------------------------------
    //
    //       cnt counts the set bits of every byte of a vector on its own
    //       and addv adds the eight results across it, so the word makes
    //       one trip out to the vector side and back. Four instructions,
    //       and the two moves are most of the cost.
    //
    //       A kernel build cannot go that way at all: kernel code may not
    //       touch the vector registers without asking for them first. So it
    //       folds in a general register instead, which is the same fold the
    //       riscv core runs and much shorter here, because every constant
    //       it needs is a repeating bit pattern and arm64 encodes those in
    //       the instruction itself -- nothing is materialised and nothing
    //       is loaded. That body is gcc's, and gcc is an instruction ahead
    //       of the obvious transcription: masking with 0xcccc.. and letting
    //       the add shift its second operand down by two does the second
    //       fold in two instructions where a shift, a mask and an add take
    //       three. Eleven of them, no branch, and no vector state.
    //
    ASM_FUNC(bits_counted)
#ifdef KERNEL_MODE
    "lsr x1, x0, #1\n   and x1, x1, #0x5555555555555555\n   sub x0, x0, x1\n"
    "and x1, x0, #0x3333333333333333\n   and x0, x0, #0xcccccccccccccccc\n"
    "add x0, x1, x0, lsr #2\n   add x0, x0, x0, lsr #4\n"
    "and x0, x0, #0x0f0f0f0f0f0f0f0f\n   mov x1, #0x0101010101010101\n"
    "mul x0, x0, x1\n   lsr x0, x0, #56\n"
#else
    "fmov d0, x0\n   cnt v0.8b, v0.8b\n   addv b0, v0.8b\n   fmov w0, s0\n"
#endif
    ASM_RET
    ASM_END(bits_counted)

    //
    //       Reversing the word turns the lowest set bit into the highest,
    //       and clz already answers 64 for a word with no bits in it, which
    //       is the answer both of these are defined to give.
    //
    ASM_FUNC(bits_trailing_zeros)
    "rbit x0, x0\n   clz x0, x0\n"
    ASM_RET
    ASM_END(bits_trailing_zeros)

    ASM_FUNC(bits_leading_zeros)
    "clz x0, x0\n"
    ASM_RET
    ASM_END(bits_leading_zeros)

    //
    //       csinc is what makes this four instructions rather than five: it
    //       increments the count for a word that had a bit and hands back
    //       zero for one that did not, so the plus one and the zero case
    //       are the same instruction. The comparison goes first because
    //       rbit and clz set no flags and the flags wanted are the input's.
    //       This is gcc's sequence; nothing shorter was found.
    //
    ASM_FUNC(bits_first_set)
    "cmp w0, #0\n   rbit w1, w0\n   clz w1, w1\n   csinc w0, wzr, w1, eq\n"
    ASM_RET
    ASM_END(bits_first_set)

    ASM_FUNC(bits_first_set_wide)
    "cmp x0, #0\n   rbit x1, x0\n   clz x1, x1\n   csinc x0, xzr, x1, eq\n"
    ASM_RET
    ASM_END(bits_first_set_wide)

    // ------------------------------------------------------------
    //       float-round
    // ------------------------------------------------------------
#ifndef KERNEL_MODE

    ASM_FUNC(decimal_truncated)
    "frintz d0, d0\n"
    ASM_RET
    ASM_END(decimal_truncated)

    ASM_FUNC(decimal_floor)
    "frintm d0, d0\n"
    ASM_RET
    ASM_END(decimal_floor)

    ASM_FUNC(decimal_ceiling)
    "frintp d0, d0\n"
    ASM_RET
    ASM_END(decimal_ceiling)

    ASM_FUNC(decimal_nearest)
    "frinti d0, d0\n"
    ASM_RET
    ASM_END(decimal_nearest)

    ASM_FUNC(decimal_rounded)
    "frinta d0, d0\n"
    ASM_RET
    ASM_END(decimal_rounded)

    //
    //       SLI takes the low bits of its destination and the rest from its
    //       source shifted up, so at 63 it writes exactly one bit -- the sign
    //       -- and keeps the other sixty three. The magnitude never has to be
    //       masked and neither operand goes near a general register. gcc
    //       builds the sign mask instead, in three.
    //
    ASM_FUNC(decimal_with_sign)
    "ushr d1, d1, #63\n"
    "sli d0, d1, #63\n"
    ASM_RET
    ASM_END(decimal_with_sign)

    ASM_FUNC(decimal_smaller)
    "fminnm d0, d0, d1\n"
    ASM_RET
    ASM_END(decimal_smaller)

    ASM_FUNC(decimal_larger)
    "fmaxnm d0, d0, d1\n"
    ASM_RET
    ASM_END(decimal_larger)

    //
    //       HI after a floating point compare is "greater than, or unordered",
    //       which is the whole condition fdim wants in one flag test: greater
    //       gives the difference and unordered gives the not-a-number the
    //       subtraction already produced.
    //
    ASM_FUNC(decimal_difference)
    "fsub d2, d0, d1\n"
    "fcmp d0, d1\n"
    "movi d0, #0\n"
    "fcsel d0, d2, d0, hi\n"
    ASM_RET
    ASM_END(decimal_difference)

    ASM_FUNC(decimal_multiply_add)
    "fmadd d0, d0, d1, d2\n"
    ASM_RET
    ASM_END(decimal_multiply_add)
#endif

    // ------------------------------------------------------------
    //       float-single
    // ------------------------------------------------------------
#ifndef KERNEL_MODE
    ASM_FUNC(narrow_square_root)
    "fsqrt s0, s0\n"
    ASM_RET
    ASM_END(narrow_square_root)

    ASM_FUNC(narrow_absolute)
    "fabs s0, s0\n"
    ASM_RET
    ASM_END(narrow_absolute)

    //
    //       fmin and fminnm are not the same instruction: fmin propagates a
    //       NaN and fminnm is IEEE minNum, which lets the number win. C asks
    //       for the second one.
    //
    ASM_FUNC(narrow_smaller)
    "fminnm s0, s0, s1\n"
    ASM_RET
    ASM_END(narrow_smaller)

    ASM_FUNC(narrow_larger)
    "fmaxnm s0, s0, s1\n"
    ASM_RET
    ASM_END(narrow_larger)

    //
    //       Armv8.0 has all four directions as named instructions, so these
    //       are the floor with nothing between them and it.
    //
    ASM_FUNC(narrow_truncated)
    "frintz s0, s0\n"
    ASM_RET
    ASM_END(narrow_truncated)

    ASM_FUNC(narrow_floor)
    "frintm s0, s0\n"
    ASM_RET
    ASM_END(narrow_floor)

    ASM_FUNC(narrow_ceiling)
    "frintp s0, s0\n"
    ASM_RET
    ASM_END(narrow_ceiling)

    ASM_FUNC(narrow_rounded)
    "frinta s0, s0\n"
    ASM_RET
    ASM_END(narrow_rounded)

    //
    //       There is no scalar sign transplant, so it is the vector one: the
    //       immediate form of movi builds the sign bit without touching
    //       memory, and bit inserts the bits of the second argument that the
    //       mask selects into the first.
    //
    ASM_FUNC(narrow_with_sign)
    "movi v31.4s, #0x80, lsl #24\n   bit v0.8b, v1.8b, v31.8b\n"
    ASM_RET
    ASM_END(narrow_with_sign)
#endif
);
#elif RISCV64
__asm__(
    ASM_SECTION

    ASM_FUNC(byte_is_digit)
    "addi t0, a0, -48\n   sltiu a0, t0, 10\n"
    ASM_RET
    ASM_END(byte_is_digit)

    ASM_FUNC(byte_is_upper)
    "addi t0, a0, -65\n   sltiu a0, t0, 26\n"
    ASM_RET
    ASM_END(byte_is_upper)

    ASM_FUNC(byte_is_lower)
    "addi t0, a0, -97\n   sltiu a0, t0, 26\n"
    ASM_RET
    ASM_END(byte_is_lower)

    ASM_FUNC(byte_is_alpha)
    "ori t0, a0, 32\n   addi t0, t0, -97\n   sltiu a0, t0, 26\n"
    ASM_RET
    ASM_END(byte_is_alpha)

    ASM_FUNC(byte_is_alnum)
    "ori t0, a0, 32\n   addi t0, t0, -97\n   sltiu t0, t0, 26\n"
    "addi t1, a0, -48\n   sltiu t1, t1, 10\n   or a0, t0, t1\n"
    ASM_RET
    ASM_END(byte_is_alnum)

    ASM_FUNC(byte_is_space)
    "addi t0, a0, -9\n   sltiu t0, t0, 5\n"
    "addi t1, a0, -32\n   seqz t1, t1\n   or a0, t0, t1\n"
    ASM_RET
    ASM_END(byte_is_space)

    ASM_FUNC(byte_is_hexadecimal)
    "addi t0, a0, -48\n   sltiu t0, t0, 10\n"
    "ori t1, a0, 32\n   addi t1, t1, -97\n   sltiu t1, t1, 6\n"
    "or a0, t0, t1\n"
    ASM_RET
    ASM_END(byte_is_hexadecimal)

    ASM_FUNC(byte_to_upper)
    "addi t0, a0, -97\n   sltiu t0, t0, 26\n   slli t0, t0, 5\n   sub a0, a0, t0\n"
    ASM_RET
    ASM_END(byte_to_upper)

    ASM_FUNC(byte_to_lower)
    "addi t0, a0, -65\n   sltiu t0, t0, 26\n   slli t0, t0, 5\n   add a0, a0, t0\n"
    ASM_RET
    ASM_END(byte_to_lower)

    //
    //       sraiw for the narrow one, so the sign that is repeated is the
    //       one at bit 31 rather than bit 63.
    //
    ASM_FUNC(absolute_whole)
    "sraiw t0, a0, 31\n   xor a0, a0, t0\n   subw a0, a0, t0\n"
    ASM_RET
    ASM_END(absolute_whole)

    ASM_FUNC(absolute_wide)
    "srai t0, a0, 63\n   xor a0, a0, t0\n   sub a0, a0, t0\n"
    ASM_RET
    ASM_END(absolute_wide)

#ifndef KERNEL_MODE
    ASM_FUNC(square_root)
    "fsqrt.d fa0, fa0\n"
    ASM_RET
    ASM_END(square_root)

    ASM_FUNC(absolute)
    "fabs.d fa0, fa0\n"
    ASM_RET
    ASM_END(absolute)
#endif

    // ------------------------------------------------------------
    //       ctype-rest
    // ------------------------------------------------------------

    ASM_FUNC(byte_is_printable)
    "addi t0, a0, -32\n   sltiu a0, t0, 95\n"
    ASM_RET
    ASM_END(byte_is_printable)

    ASM_FUNC(byte_is_graphic)
    "addi t0, a0, -33\n   sltiu a0, t0, 94\n"
    ASM_RET
    ASM_END(byte_is_graphic)

    ASM_FUNC(byte_is_control)
    "sltiu t0, a0, 32\n   addi t1, a0, -127\n   seqz t1, t1\n   or a0, t0, t1\n"
    ASM_RET
    ASM_END(byte_is_control)

    //
    //       No conditional move and no conditional compare here, so the
    //       three range tests are simply written out and subtracted. Each of
    //       them depends on nothing but the argument, which is what this
    //       architecture gets in trade for the extra instructions: nine that
    //       issue three abreast rather than eight that hand a flag along.
    //
    ASM_FUNC(byte_is_punctuation)
    "addi t0, a0, -33\n   sltiu t0, t0, 94\n"
    "addi t1, a0, -48\n   sltiu t1, t1, 10\n"
    "ori t2, a0, 32\n   addi t2, t2, -97\n   sltiu t2, t2, 26\n"
    "sub t0, t0, t1\n   sub a0, t0, t2\n"
    ASM_RET
    ASM_END(byte_is_punctuation)

    ASM_FUNC(byte_is_blank)
    "addi t0, a0, -9\n   seqz t0, t0\n"
    "addi t1, a0, -32\n   seqz t1, t1\n   or a0, t0, t1\n"
    ASM_RET
    ASM_END(byte_is_blank)

    //
    //       An int arrives sign extended, so a negative one is a very large
    //       unsigned number and one unsigned compare is the whole function.
    //
    ASM_FUNC(byte_is_ascii)
    "sltiu a0, a0, 128\n"
    ASM_RET
    ASM_END(byte_is_ascii)

    ASM_FUNC(byte_to_ascii)
    "andi a0, a0, 127\n"
    ASM_RET
    ASM_END(byte_to_ascii)

    // ------------------------------------------------------------
    //       set-scan
    // ------------------------------------------------------------
    // The x86_64 block carries the reasoning. There is no bitfield extract in
    // the baseline, so the word index is the shift and the mask written out:
    // (byte >> 6) * 8 is (byte >> 3) with the low three bits dropped, which is
    // one shift and one andi rather than two shifts.
    ASM_FUNC(string_span_of_set)
    "addi sp, sp, -32\n"
    "sd zero, 0(sp)\n   sd zero, 8(sp)\n   sd zero, 16(sp)\n   sd zero, 24(sp)\n"
    "li t4, 1\n   mv a3, a0\n"
    "lbu t0, 0(a1)\n   beqz t0, 2f\n"
    "1:  srli t2, t0, 3\n   andi t2, t2, 24\n   add t2, sp, t2\n   ld t3, 0(t2)\n"
    "sll t1, t4, t0\n   or t3, t3, t1\n   sd t3, 0(t2)\n   addi a1, a1, 1\n"
    "lbu t0, 0(a1)\n   bnez t0, 1b\n"
    "2:  j 4f\n"
    "3:  addi a0, a0, 1\n"
    "4:  lbu t1, 0(a0)\n   srli t2, t1, 3\n   andi t2, t2, 24\n   add t2, sp, t2\n"
    "ld t3, 0(t2)\n   srl t3, t3, t1\n   andi t3, t3, 1\n   bnez t3, 3b\n"
    "sub a0, a0, a3\n   addi sp, sp, 32\n"
    ASM_RET
    ASM_END(string_span_of_set)

    // The complement: bit zero goes down set instead of clear.
    ASM_FUNC(string_span_without_set)
    "addi sp, sp, -32\n"
    "li t4, 1\n"
    "sd t4, 0(sp)\n   sd zero, 8(sp)\n   sd zero, 16(sp)\n   sd zero, 24(sp)\n"
    "mv a3, a0\n"
    "lbu t0, 0(a1)\n   beqz t0, 2f\n"
    "1:  srli t2, t0, 3\n   andi t2, t2, 24\n   add t2, sp, t2\n   ld t3, 0(t2)\n"
    "sll t1, t4, t0\n   or t3, t3, t1\n   sd t3, 0(t2)\n   addi a1, a1, 1\n"
    "lbu t0, 0(a1)\n   bnez t0, 1b\n"
    "2:  j 4f\n"
    "3:  addi a0, a0, 1\n"
    "4:  lbu t1, 0(a0)\n   srli t2, t1, 3\n   andi t2, t2, 24\n   add t2, sp, t2\n"
    "ld t3, 0(t2)\n   srl t3, t3, t1\n   andi t3, t3, 1\n   beqz t3, 3b\n"
    "sub a0, a0, a3\n   addi sp, sp, 32\n"
    ASM_RET
    ASM_END(string_span_without_set)

    // a0 walks the source, so it is already the answer; only the terminator
    // has to be turned into null, and t1 still holds the byte that stopped it.
    ASM_FUNC(string_first_of_set)
    "addi sp, sp, -32\n"
    "li t4, 1\n"
    "sd t4, 0(sp)\n   sd zero, 8(sp)\n   sd zero, 16(sp)\n   sd zero, 24(sp)\n"
    "lbu t0, 0(a1)\n   beqz t0, 2f\n"
    "1:  srli t2, t0, 3\n   andi t2, t2, 24\n   add t2, sp, t2\n   ld t3, 0(t2)\n"
    "sll t1, t4, t0\n   or t3, t3, t1\n   sd t3, 0(t2)\n   addi a1, a1, 1\n"
    "lbu t0, 0(a1)\n   bnez t0, 1b\n"
    "2:  j 4f\n"
    "3:  addi a0, a0, 1\n"
    "4:  lbu t1, 0(a0)\n   srli t2, t1, 3\n   andi t2, t2, 24\n   add t2, sp, t2\n"
    "ld t3, 0(t2)\n   srl t3, t3, t1\n   andi t3, t3, 1\n   beqz t3, 3b\n"
    "bnez t1, 5f\n   li a0, 0\n"
    "5:  addi sp, sp, 32\n"
    ASM_RET
    ASM_END(string_first_of_set)

    // ------------------------------------------------------------
    //       case-compare
    // ------------------------------------------------------------
    //      No conditional move and no vector at the baseline, so both halves
    //      are written out of what RV64IM has. The byte fold is sltiu for the
    //      range and a shift for the thirty two; the word fold is the same
    //      range test done eight bytes at a time in an integer register.
    //
    //      An unaligned ld is not part of the RV64I floor, so the word body
    //      runs only where string_compare_max lets its own run: the two
    //      pointers must share a residue, and bytes are peeled until they are
    //      aligned. An aligned eight byte read cannot leave its page, so
    //      neither routine here needs a page argument.
    ASM_FUNC(string_compare_folded)
    "li a3, 0\n"
    "xor a4, a0, a1\n   andi a4, a4, 7\n   bnez a4, 2f  # different residues: bytewise\n"
    "6:  andi a4, a0, 7\n   beqz a4, 5f\n"
    "lbu t3, 0(a0)\n   lbu t4, 0(a1)\n"
    "addi a4, t3, -65  # 'A'\n   sltiu a4, a4, 26\n   slli a4, a4, 5\n   add t3, t3, a4\n"
    "addi a4, t4, -65  # 'A'\n   sltiu a4, a4, 26\n   slli a4, a4, 5\n   add t4, t4, a4\n"
    "sub a3, t3, t4\n   bnez a3, 9f\n   beqz t4, 9f\n"
    "addi a0, a0, 1\n   addi a1, a1, 1\n   j 6b\n"
    //
    //      li of a whole sixty four bit constant is six instructions and the
    //      constant is the same four bytes twice, so it is built once and the
    //      rest come off it: a shift for 0x80.., a subtract for 0x7f.., and
    //      the M extension for the two biases.
    //
    "5:  lui a4, 0x1010\n   addi a4, a4, 257  # 0x01010101\n"
    "slli a5, a4, 32\n   add a4, a4, a5  # 0x0101010101010101\n"
    "slli a5, a4, 7  # 0x8080808080808080\n"
    "sub a6, a5, a4  # 0x7f7f7f7f7f7f7f7f\n"
    "li a7, 63\n   mul a7, a7, a4  # 0x3f3f3f3f3f3f3f3f\n"
    "li t0, 37\n   mul t0, t0, a4  # 0x2525252525252525\n"
    //
    //      The high bit of every byte is masked away first, which is what
    //      makes the two biased adds safe: a byte of at most 0x7f plus 0x3f
    //      cannot carry into its neighbour. After them the high bit of a byte
    //      is set when the byte was at least 'A' and, in the second, when it
    //      was past 'Z'; the first and not the second and not the original
    //      high bit leaves 0x80 standing in exactly the upper case bytes, and
    //      that shifted twice is the thirty two to add.
    //
    "1:  ld t3, 0(a0)\n   ld t4, 0(a1)\n"
    "and t1, t3, a6\n   add t2, t1, a7\n   add t1, t1, t0\n   or t1, t1, t3\n"
    "not t1, t1\n   and t1, t1, t2\n   and t1, t1, a5\n   srli t1, t1, 2\n   add t3, t3, t1\n"
    "and t1, t4, a6\n   add t2, t1, a7\n   add t1, t1, t0\n   or t1, t1, t4\n"
    "not t1, t1\n   and t1, t1, t2\n   and t1, t1, a5\n   srli t1, t1, 2\n   add t4, t4, t1\n"
    "bne t3, t4, 2f  # differ: let the byte step find where\n"
    "sub t1, t3, a4\n   not t2, t3\n   and t1, t1, t2\n   and t1, t1, a5\n"
    "bnez t1, 2f  # a terminator in them: the byte step stops on it\n"
    "addi a0, a0, 8\n   addi a1, a1, 8\n   j 1b\n"
    "2:  lbu t3, 0(a0)\n   lbu t4, 0(a1)\n"
    "addi a4, t3, -65\n   sltiu a4, a4, 26\n   slli a4, a4, 5\n   add t3, t3, a4\n"
    "addi a4, t4, -65\n   sltiu a4, a4, 26\n   slli a4, a4, 5\n   add t4, t4, a4\n"
    "sub a3, t3, t4\n   bnez a3, 9f\n   beqz t4, 9f\n"
    "addi a0, a0, 1\n   addi a1, a1, 1\n   j 2b\n"
    "9:  sext.w a0, a3  # an int return is sign extended in a0\n"
    ASM_RET
    ASM_END(string_compare_folded)
    ASM_FUNC(string_compare_folded_max)
    "li a3, 0\n   beqz a2, 9f\n"
    "li t5, 8\n   bltu a2, t5, 3f  # too short to earn the constants\n"
    "xor a4, a0, a1\n   andi a4, a4, 7\n   bnez a4, 2f\n"
    "6:  andi a4, a0, 7\n   beqz a4, 5f\n"
    "lbu t3, 0(a0)\n   lbu t4, 0(a1)\n"
    "addi a4, t3, -65\n   sltiu a4, a4, 26\n   slli a4, a4, 5\n   add t3, t3, a4\n"
    "addi a4, t4, -65\n   sltiu a4, a4, 26\n   slli a4, a4, 5\n   add t4, t4, a4\n"
    "sub a3, t3, t4\n   bnez a3, 9f\n   beqz t4, 8f\n"
    "addi a0, a0, 1\n   addi a1, a1, 1\n   addi a2, a2, -1\n   beqz a2, 8f\n   j 6b\n"
    "5:  lui a4, 0x1010\n   addi a4, a4, 257  # 0x01010101\n"
    "slli a5, a4, 32\n   add a4, a4, a5  # 0x0101010101010101\n"
    "slli a5, a4, 7  # 0x8080808080808080\n"
    "sub a6, a5, a4  # 0x7f7f7f7f7f7f7f7f\n"
    "li a7, 63\n   mul a7, a7, a4  # 0x3f3f3f3f3f3f3f3f\n"
    "li t0, 37\n   mul t0, t0, a4  # 0x2525252525252525\n"
    "bltu a2, t5, 2f\n"
    "1:  ld t3, 0(a0)\n   ld t4, 0(a1)\n"
    "and t1, t3, a6\n   add t2, t1, a7\n   add t1, t1, t0\n   or t1, t1, t3\n"
    "not t1, t1\n   and t1, t1, t2\n   and t1, t1, a5\n   srli t1, t1, 2\n   add t3, t3, t1\n"
    "and t1, t4, a6\n   add t2, t1, a7\n   add t1, t1, t0\n   or t1, t1, t4\n"
    "not t1, t1\n   and t1, t1, t2\n   and t1, t1, a5\n   srli t1, t1, 2\n   add t4, t4, t1\n"
    "bne t3, t4, 2f\n"
    "sub t1, t3, a4\n   not t2, t3\n   and t1, t1, t2\n   and t1, t1, a5\n   bnez t1, 2f\n"
    "addi a0, a0, 8\n   addi a1, a1, 8\n   addi a2, a2, -8\n   bgeu a2, t5, 1b\n"
    "2:  beqz a2, 8f\n"
    "3:  lbu t3, 0(a0)\n   lbu t4, 0(a1)\n"
    "addi a4, t3, -65\n   sltiu a4, a4, 26\n   slli a4, a4, 5\n   add t3, t3, a4\n"
    "addi a4, t4, -65\n   sltiu a4, a4, 26\n   slli a4, a4, 5\n   add t4, t4, a4\n"
    "sub a3, t3, t4\n   bnez a3, 9f\n   beqz t4, 8f\n"
    "addi a0, a0, 1\n   addi a1, a1, 1\n   addi a2, a2, -1\n   bnez a2, 3b\n"
    "8:  li a3, 0\n"
    "9:  sext.w a0, a3  # an int return is sign extended in a0\n"
    ASM_RET
    ASM_END(string_compare_folded_max)

    // ------------------------------------------------------------
    //       memory-rest
    // ------------------------------------------------------------
    //       memccpy: the x86_64 block carries the reasoning.
    ASM_FUNC(memory_copy_until)
    "addi sp, sp, -48\n   sd ra, 40(sp)\n   sd s0, 32(sp)\n   sd s1, 24(sp)\n"
    "sd s2, 16(sp)\n"
    "mv s0, a0  # the destination\n   mv s1, a1  # the source\n"
    "mv s2, a3  # the size\n"
    "mv a0, a1\n   mv a1, a2\n   mv a2, a3\n   call memory_first_of\n"
    "mv a2, s2  # a byte that is not there copies the whole span\n"
    "bnez a0, 2f\n"
    "li s2, 0  # and answers null\n   j 1f\n"
    "2:  sub a0, a0, s1\n   addi a2, a0, 1  # everything up to and including it\n"
    "add s2, s0, a2  # and the answer is just past what was copied\n"
    "1:  mv a0, s0\n   mv a1, s1\n   call memory_copy_apart\n"
    "mv a0, s2\n"
    "ld ra, 40(sp)\n   ld s0, 32(sp)\n   ld s1, 24(sp)\n   ld s2, 16(sp)\n"
    "addi sp, sp, 48\n"
    ASM_RET
    ASM_END(memory_copy_until)
    //       stpcpy: the x86_64 block carries the reasoning.
    ASM_FUNC(string_copy_end)
    "addi sp, sp, -32\n   sd ra, 24(sp)\n   sd s0, 16(sp)\n   sd s1, 8(sp)\n"
    "mv s0, a0  # the destination\n   mv s1, a1  # the source\n"
    "mv a0, a1\n   call string_length\n"
    "mv a1, s1\n   add s1, s0, a0  # where the terminator lands, and the answer\n"
    "addi a2, a0, 1  # the terminator is copied too\n   mv a0, s0\n"
    "call memory_copy_apart\n   mv a0, s1\n"
    "ld ra, 24(sp)\n   ld s0, 16(sp)\n   ld s1, 8(sp)\n   addi sp, sp, 32\n"
    ASM_RET
    ASM_END(string_copy_end)
    //       stpncpy: the x86_64 block carries the reasoning, padding rule and all.
    ASM_FUNC(string_copy_max_endptr)
    "addi sp, sp, -48\n   sd ra, 40(sp)\n   sd s0, 32(sp)\n   sd s1, 24(sp)\n"
    "sd s2, 16(sp)\n"
    "mv s0, a0  # the destination\n   mv s1, a1  # the source\n"
    "mv s2, a2  # the bound\n"
    "mv a0, a1\n   mv a1, a2\n   call string_length_max\n"
    "mv a1, s1\n   mv a2, a0\n"
    "add s1, s0, a0  # the first byte the source did not fill, and the answer\n"
    "sub s2, s2, a0  # what is left of the bound is padding\n"
    "mv a0, s0\n   call memory_copy_apart\n"
    "beqz s2, 1f\n"
    "mv a0, s1\n   li a1, 0\n   mv a2, s2\n   call memory_fill\n"
    "1:  mv a0, s1\n"
    "ld ra, 40(sp)\n   ld s0, 32(sp)\n   ld s1, 24(sp)\n   ld s2, 16(sp)\n"
    "addi sp, sp, 48\n"
    ASM_RET
    ASM_END(string_copy_max_endptr)
    //       strncat: the x86_64 block carries the reasoning.
    ASM_FUNC(string_append_max)
    "addi sp, sp, -48\n   sd ra, 40(sp)\n   sd s0, 32(sp)\n   sd s1, 24(sp)\n"
    "sd s2, 16(sp)\n"
    "mv s0, a0  # the destination\n   mv s1, a1  # the source\n"
    "mv s2, a2  # the bound\n"
    "call string_length  # the destination is already in a0\n"
    "add a0, s0, a0\n   mv a1, s1\n   mv a2, s2\n   call string_copy_max_end\n"
    "mv a0, s0  # strncat hands the destination back\n"
    "ld ra, 40(sp)\n   ld s0, 32(sp)\n   ld s1, 24(sp)\n   ld s2, 16(sp)\n"
    "addi sp, sp, 48\n"
    ASM_RET
    ASM_END(string_append_max)
    //       bzero and bcopy: the x86_64 block carries the reasoning.
    ASM_FUNC(memory_zero)
    "mv a2, a1\n   li a1, 0\n   tail memory_fill\n"
    ASM_END(memory_zero)
    ASM_FUNC(memory_copy_source_first)
    "mv t0, a0\n   mv a0, a1\n   mv a1, t0\n   tail memory_copy\n"
    ASM_END(memory_copy_source_first)

    // ------------------------------------------------------------
    //       integer-parse
    // ------------------------------------------------------------
    ASM_FUNC(string_to_number)
    "li a4, 0\n   j string_to_number_core\n"
    ASM_END(string_to_number)

    ASM_FUNC(string_to_number_unsigned)
    "li a4, 2\n   j string_to_number_core\n"
    ASM_END(string_to_number_unsigned)

#ifndef KERNEL_MODE
    ASM_FUNC(string_to_number_checked)
    "addi sp, sp, -16\n   sd a3, 0(sp)\n   sd ra, 8(sp)\n   li a6, 0\n"
    "call string_to_number\n   ld t0, 0(sp)\n   ld ra, 8(sp)\n"
    "beqz t0, .Lnumber_rv_checked_done\n"
    "snez t1, a6\n   sw t1, 0(t0)\n"
    ".Lnumber_rv_checked_done:\n   addi sp, sp, 16\n"
    ASM_RET
    ASM_END(string_to_number_checked)

    ASM_FUNC(string_to_number_unsigned_checked)
    "addi sp, sp, -16\n   sd a3, 0(sp)\n   sd ra, 8(sp)\n   li a6, 0\n"
    "call string_to_number_unsigned\n   ld t0, 0(sp)\n   ld ra, 8(sp)\n"
    "beqz t0, .Lnumber_rv_unsigned_checked_done\n"
    "snez t1, a6\n   sw t1, 0(t0)\n"
    ".Lnumber_rv_unsigned_checked_done:\n   addi sp, sp, 16\n"
    ASM_RET
    ASM_END(string_to_number_unsigned_checked)
#endif

    ASM_FUNC(string_to_whole_wide)
    "li a1, 0\n   li a2, 10\n   li a4, 0\n   j string_to_number_core\n"
    ASM_END(string_to_whole_wide)

    //
    //       This is the architecture where atoi is not free: lp64d wants a b32
    //       return sign extended in a0, so the flag word asks the core for the
    //       narrowing and this stays a tail branch instead of the frame and
    //       the call the compiler builds around it.
    //
    ASM_FUNC(string_to_whole)
    "li a1, 0\n   li a2, 10\n   li a4, 4\n   j string_to_number_core\n"
    ASM_END(string_to_whole)

    //
    //       string_to_number_core: the x86_64 block carries the reasoning.
    //       a4 is the flag word, a3 walks the input, a5 is the base, a6 is the
    //       sticky overflow, a7 is the digit, and a0 is the input pointer
    //       until the first digit is taken and the running value after it.
    //
    //       lp64d hands an int in sign extended, so the base needs no
    //       narrowing here the way it does on the other two: a negative base
    //       is already an enormous unsigned number and the one range test
    //       below refuses it.
    //
    //       Three constants live in registers across both loops because this
    //       architecture has no compare-with-immediate branch, and reloading
    //       them would be an instruction per byte.
    //
    ASM_LOCAL_FUNC(string_to_number_core)
    "addi t0, a2, -2\n   li t1, 35\n   bltu t0, t1, .Lnumber_rv_start\n"
    "bnez a2, .Lnumber_rv_none  # not zero and not 2..36\n"
    ".Lnumber_rv_start:\n   mv a5, a2\n   mv a3, a0\n"
    "li t3, 5\n   li t4, 32\n   li t5, 10\n   li t6, 26\n"
    ".Lnumber_rv_space:\n   lbu t0, 0(a3)\n   addi a3, a3, 1\n"
    "beq t0, t4, .Lnumber_rv_space\n"
    "addi t1, t0, -9\n   bltu t1, t3, .Lnumber_rv_space\n"
    "addi a3, a3, -1\n"
    "li t1, 45\n   beq t0, t1, .Lnumber_rv_minus\n"
    "li t1, 43\n   bne t0, t1, .Lnumber_rv_base\n"
    "addi a3, a3, 1\n   j .Lnumber_rv_base\n"
    ".Lnumber_rv_minus:\n   ori a4, a4, 1\n   addi a3, a3, 1\n"
    ".Lnumber_rv_base:\n   bnez a5, .Lnumber_rv_given\n"
    "li a5, 10\n   lbu t0, 0(a3)\n   li t1, 48\n"
    "bne t0, t1, .Lnumber_rv_first\n"
    "li a5, 8\n   lbu t0, 1(a3)\n   ori t0, t0, 32\n   li t1, 120\n"
    "bne t0, t1, .Lnumber_rv_first\n"
    "li a5, 16\n   j .Lnumber_rv_prefix\n"
    ".Lnumber_rv_given:\n   li t1, 16\n   bne a5, t1, .Lnumber_rv_first\n"
    "lbu t0, 0(a3)\n   li t1, 48\n   bne t0, t1, .Lnumber_rv_first\n"
    "lbu t0, 1(a3)\n   ori t0, t0, 32\n   li t1, 120\n"
    "bne t0, t1, .Lnumber_rv_first\n"
    ".Lnumber_rv_prefix:\n   lbu t0, 2(a3)\n   addi t1, t0, -48\n"
    "bltu t1, t5, .Lnumber_rv_take\n"
    "ori t0, t0, 32\n   addi t1, t0, -97\n   li t2, 6\n"
    "bgeu t1, t2, .Lnumber_rv_first\n"
    ".Lnumber_rv_take:\n   addi a3, a3, 2\n"
    ".Lnumber_rv_first:\n   lbu t0, 0(a3)\n   addi a7, t0, -48\n"
    "bltu a7, t5, .Lnumber_rv_first_value\n"
    "ori t0, t0, 32\n   addi a7, t0, -97\n"
    "bgeu a7, t6, .Lnumber_rv_none\n   addi a7, a7, 10\n"
    ".Lnumber_rv_first_value:\n   bgeu a7, a5, .Lnumber_rv_none\n"
    "addi a3, a3, 1\n   mv a0, a7\n   li a6, 0\n"
    //
    //       mulhu is the half the 64 bit multiply throws away, and sltu turns
    //       the digit add into its own carry: an unsigned sum below what was
    //       added to it is a sum that wrapped.
    //
    ".Lnumber_rv_loop:\n   lbu t0, 0(a3)\n   addi a7, t0, -48\n"
    "bltu a7, t5, .Lnumber_rv_value\n"
    "ori t0, t0, 32\n   addi a7, t0, -97\n"
    "bgeu a7, t6, .Lnumber_rv_stop\n   addi a7, a7, 10\n"
    ".Lnumber_rv_value:\n   bgeu a7, a5, .Lnumber_rv_stop\n"
    "mulhu t1, a0, a5\n   mul a0, a0, a5\n   add a0, a0, a7\n"
    "sltu t2, a0, a7\n   add t1, t1, t2\n   or a6, a6, t1\n"
    "addi a3, a3, 1\n   j .Lnumber_rv_loop\n"
    ".Lnumber_rv_stop:\n   beqz a1, .Lnumber_rv_kept\n   sd a3, 0(a1)\n"
    ".Lnumber_rv_kept:\n   andi t1, a4, 2\n   bnez t1, .Lnumber_rv_unsigned\n"
    "li t1, -1\n   srli t1, t1, 1\n   andi t2, a4, 1\n   add t1, t1, t2\n"
    "bnez a6, .Lnumber_rv_clamp\n   bgeu t1, a0, .Lnumber_rv_sign\n"
    ".Lnumber_rv_clamp:\n   mv a0, t1\n   li a6, 1\n"
    ".Lnumber_rv_sign:\n   andi t2, a4, 1\n"
    "beqz t2, .Lnumber_rv_done\n   sub a0, zero, a0\n"
    "j .Lnumber_rv_done\n"
    ".Lnumber_rv_unsigned:\n   beqz a6, .Lnumber_rv_negate\n"
    "li a0, -1\n   j .Lnumber_rv_done\n"
    ".Lnumber_rv_negate:\n   andi t2, a4, 1\n"
    "beqz t2, .Lnumber_rv_done\n   sub a0, zero, a0\n"
    //
    //       The narrowing bit, which only this architecture reads.
    //
    ".Lnumber_rv_done:\n   andi t1, a4, 4\n"
    "beqz t1, .Lnumber_rv_wide\n   sext.w a0, a0\n"
    ".Lnumber_rv_wide:\n"
    ASM_RET
    ".Lnumber_rv_none:\n   beqz a1, .Lnumber_rv_none_done\n   sd a0, 0(a1)\n"
    ".Lnumber_rv_none_done:\n   li a0, 0\n"
    ASM_RET
    ASM_LOCAL_END(string_to_number_core)

    // ------------------------------------------------------------
    //       bit-count
    // ------------------------------------------------------------
    //
    //       The core, and on this baseline the only one of the four that is
    //       real work. Bits are folded in pairs, then in nibbles, then in
    //       bytes, and the eight byte totals are summed by one multiply:
    //       multiplying by a word of set low bytes puts the sum of every
    //       byte in the top one, and a shift brings it down. The M
    //       extension is in the baseline, so that multiply is allowed.
    //
    //       Ten of the twenty two instructions build the four masks, and
    //       that is where the sequence is won. Each mask is a shift and an
    //       exclusive-or away from the one above it -- 0x0f0f.. shifted two
    //       and folded is 0x3333.., that shifted one and folded is
    //       0x5555.., and 0x0f0f.. shifted three and masked back on itself
    //       is 0x0101.. -- so one lui and addi pair is all that is
    //       materialised. gcc builds all four from scratch, four
    //       instructions each, and that alone is the six instruction
    //       difference between its sequence and this one.
    //
    ASM_FUNC(bits_counted)
    "lui  t0, 0x0f0f1\n   addi t0, t0, -241\n   slli t1, t0, 32\n   or t0, t0, t1\n"
    "slli t1, t0, 2\n   xor t1, t1, t0\n"
    "slli t2, t1, 1\n   xor t2, t2, t1\n"
    "srli t3, t0, 3\n   and t3, t3, t0\n"
    "srli t4, a0, 1\n   and t4, t4, t2\n   sub a0, a0, t4\n"
    "srli t4, a0, 2\n   and t4, t4, t1\n   and a0, a0, t1\n   add a0, a0, t4\n"
    "srli t4, a0, 4\n   add a0, a0, t4\n   and a0, a0, t0\n"
    "mul  a0, a0, t3\n   srli a0, a0, 56\n"
    ASM_RET
    ASM_END(bits_counted)

    //
    //       Three prologues onto that core, each one turning its question
    //       into a count of ones and jumping in. The jump is a real jump
    //       rather than a copy of the core because it is one instruction
    //       against twenty two, and it is never mispredicted.
    //
    //       x & -x keeps only the lowest set bit, and one less than a
    //       single bit is a run of ones exactly as long as the count of
    //       trailing zeros. A word with no bits in it isolates nothing, and
    //       zero less one is the whole word set, which counts 64 -- the
    //       answer this is defined to give, arrived at rather than tested
    //       for.
    //
    ASM_FUNC(bits_trailing_zeros)
    "neg t0, a0\n   and a0, a0, t0\n   addi a0, a0, -1\n   j bits_counted\n"
    ASM_END(bits_trailing_zeros)

    //
    //       Or-ing the word into itself at six doubling distances smears
    //       every set bit down to the bottom, so what is left above the
    //       highest one is untouched zeros; complementing turns those into
    //       the ones to be counted. Zero smears to zero and complements to
    //       the whole word, so it counts 64 the same way.
    //
    ASM_FUNC(bits_leading_zeros)
    "srli t0, a0, 1\n   or a0, a0, t0\n"
    "srli t0, a0, 2\n   or a0, a0, t0\n"
    "srli t0, a0, 4\n   or a0, a0, t0\n"
    "srli t0, a0, 8\n   or a0, a0, t0\n"
    "srli t0, a0, 16\n   or a0, a0, t0\n"
    "srli t0, a0, 32\n   or a0, a0, t0\n"
    "not a0, a0\n   j bits_counted\n"
    ASM_END(bits_leading_zeros)

    //
    //       Subtracting one borrows through the trailing zeros and stops at
    //       the lowest set bit, so the exclusive-or of the word with it is
    //       the run of bits the borrow touched: that bit and everything
    //       under it, which counts the one-based position ffs is asking
    //       for. Zero has no bit for the borrow to stop at and would count
    //       the whole word, so the run is masked by the sign-spread of
    //       "was there anything set", which is nothing at all for zero and
    //       everything otherwise. Masking the input to the count rather
    //       than the count itself is what leaves the jump as a tail jump.
    //
    //       The narrow one is the wide one instruction for instruction,
    //       and correct for the same reason rather than by coincidence: the
    //       ABI hands an int sign-extended, and every bit an extension adds
    //       sits above the low thirty two, so it can never be the lowest
    //       one. A zero int extends to a zero word, and answers zero.
    //
    ASM_FUNC(bits_first_set)
    "snez t0, a0\n   neg t0, t0\n   addi t1, a0, -1\n"
    "xor a0, a0, t1\n   and a0, a0, t0\n   j bits_counted\n"
    ASM_END(bits_first_set)

    ASM_FUNC(bits_first_set_wide)
    "snez t0, a0\n   neg t0, t0\n   addi t1, a0, -1\n"
    "xor a0, a0, t1\n   and a0, a0, t0\n   j bits_counted\n"
    ASM_END(bits_first_set_wide)

    // ------------------------------------------------------------
    //       float-round
    // ------------------------------------------------------------
#ifndef KERNEL_MODE

    //
    //       The five that have no instruction: the exponent guard and the
    //       integer round trip are written out above the declarations, and
    //       the only difference between these five is the rounding mode named
    //       on the FCVT -- rtz, rdn, rup, dyn and rmm, in that order.
    //
    ASM_FUNC(decimal_truncated)
    "fmv.x.d t0, fa0\n"
    "slli t0, t0, 1\n"
    "srli t0, t0, 53\n"
    "li t1, 1075\n"
    "bgeu t0, t1, 1f\n"
    "frflags t2\n"
    "fcvt.l.d t3, fa0, rtz\n"
    "fcvt.d.l ft0, t3\n"
    "fsgnj.d fa0, ft0, fa0\n"
    "fsflags t2\n"
    "1:\n"
    ASM_RET
    ASM_END(decimal_truncated)

    ASM_FUNC(decimal_floor)
    "fmv.x.d t0, fa0\n"
    "slli t0, t0, 1\n"
    "srli t0, t0, 53\n"
    "li t1, 1075\n"
    "bgeu t0, t1, 1f\n"
    "frflags t2\n"
    "fcvt.l.d t3, fa0, rdn\n"
    "fcvt.d.l ft0, t3\n"
    "fsgnj.d fa0, ft0, fa0\n"
    "fsflags t2\n"
    "1:\n"
    ASM_RET
    ASM_END(decimal_floor)

    ASM_FUNC(decimal_ceiling)
    "fmv.x.d t0, fa0\n"
    "slli t0, t0, 1\n"
    "srli t0, t0, 53\n"
    "li t1, 1075\n"
    "bgeu t0, t1, 1f\n"
    "frflags t2\n"
    "fcvt.l.d t3, fa0, rup\n"
    "fcvt.d.l ft0, t3\n"
    "fsgnj.d fa0, ft0, fa0\n"
    "fsflags t2\n"
    "1:\n"
    ASM_RET
    ASM_END(decimal_ceiling)

    ASM_FUNC(decimal_nearest)
    "fmv.x.d t0, fa0\n"
    "slli t0, t0, 1\n"
    "srli t0, t0, 53\n"
    "li t1, 1075\n"
    "bgeu t0, t1, 1f\n"
    "frflags t2\n"
    "fcvt.l.d t3, fa0\n"
    "fcvt.d.l ft0, t3\n"
    "fsgnj.d fa0, ft0, fa0\n"
    "fsflags t2\n"
    "1:\n"
    ASM_RET
    ASM_END(decimal_nearest)

    ASM_FUNC(decimal_rounded)
    "fmv.x.d t0, fa0\n"
    "slli t0, t0, 1\n"
    "srli t0, t0, 53\n"
    "li t1, 1075\n"
    "bgeu t0, t1, 1f\n"
    "frflags t2\n"
    "fcvt.l.d t3, fa0, rmm\n"
    "fcvt.d.l ft0, t3\n"
    "fsgnj.d fa0, ft0, fa0\n"
    "fsflags t2\n"
    "1:\n"
    ASM_RET
    ASM_END(decimal_rounded)

    ASM_FUNC(decimal_with_sign)
    "fsgnj.d fa0, fa0, fa1\n"
    ASM_RET
    ASM_END(decimal_with_sign)

    ASM_FUNC(decimal_smaller)
    "fmin.d fa0, fa0, fa1\n"
    ASM_RET
    ASM_END(decimal_smaller)

    ASM_FUNC(decimal_larger)
    "fmax.d fa0, fa0, fa1\n"
    ASM_RET
    ASM_END(decimal_larger)

    //
    //       No conditional move for the floating point registers here, so the
    //       select is done on the bits: FLE.D answers "not greater and not
    //       unordered", one less than that is a mask of all ones or all zeros,
    //       and a masked-off difference is the positive zero fdim wants.
    //
    ASM_FUNC(decimal_difference)
    "fsub.d ft0, fa0, fa1\n"
    "fle.d t0, fa0, fa1\n"
    "addi t0, t0, -1\n"
    "fmv.x.d t1, ft0\n"
    "and t1, t1, t0\n"
    "fmv.d.x fa0, t1\n"
    ASM_RET
    ASM_END(decimal_difference)

    ASM_FUNC(decimal_multiply_add)
    "fmadd.d fa0, fa0, fa1, fa2\n"
    ASM_RET
    ASM_END(decimal_multiply_add)
#endif

    // ------------------------------------------------------------
    //       float-single
    // ------------------------------------------------------------
#ifndef KERNEL_MODE
    ASM_FUNC(narrow_square_root)
    "fsqrt.s fa0, fa0\n"
    ASM_RET
    ASM_END(narrow_square_root)

    ASM_FUNC(narrow_absolute)
    "fabs.s fa0, fa0\n"
    ASM_RET
    ASM_END(narrow_absolute)

    //
    //       fmin.s already lets a number beat a NaN, which is the C rule,
    //       so this is one instruction and not a select.
    //
    ASM_FUNC(narrow_smaller)
    "fmin.s fa0, fa0, fa1\n"
    ASM_RET
    ASM_END(narrow_smaller)

    ASM_FUNC(narrow_larger)
    "fmax.s fa0, fa0, fa1\n"
    ASM_RET
    ASM_END(narrow_larger)

    //
    //       No round-to-integral instruction without Zfa, so the rounding
    //       mode of the integer conversion is the rounding: rtz truncates,
    //       rdn floors, rup ceilings and rmm is the half-away-from-zero one
    //       that C's round wants. The four bodies below differ in that one
    //       field and nowhere else.
    //
    //       lui alone builds two to the twenty third, and a magnitude at or
    //       above it is already whole -- as are infinity and every NaN,
    //       whose exponents are larger still, so one unsigned compare
    //       against the magnitude covers the whole of "leave this alone" and
    //       no class test is needed. Below it the conversion is exact in
    //       thirty two bits with room to spare, and the sign is put back
    //       afterwards because zero has no sign to convert back from: the
    //       ceiling of minus a quarter has to be minus zero.
    //
    //       The way back is fcvt.s.w with the dynamic mode where glibc writes
    //       a static one. It cannot matter: the integer is smaller than two
    //       to the twenty third in magnitude, so every rounding mode gives
    //       the same exact float. The mode on the way out, which can matter,
    //       is the static field.
    //
    //       glibc spends sixteen instructions here, two of them a CSR round
    //       trip to keep the conversion from raising inexact and two of them
    //       a load of the constant this builds. Nothing in this library
    //       reads the accrued flags, so neither is bought.
    //
    ASM_FUNC(narrow_truncated)
    "fabs.s ft0, fa0\n   fmv.x.w t0, ft0\n   lui t1, 0x4b000\n"
    "bgeu t0, t1, .Lnarrow_truncated_riscv_whole\n"
    "fcvt.w.s t2, fa0, rtz\n   fcvt.s.w ft0, t2\n   fsgnj.s fa0, ft0, fa0\n"
    ".Lnarrow_truncated_riscv_whole:\n"
    ASM_RET
    ASM_END(narrow_truncated)

    ASM_FUNC(narrow_floor)
    "fabs.s ft0, fa0\n   fmv.x.w t0, ft0\n   lui t1, 0x4b000\n"
    "bgeu t0, t1, .Lnarrow_floor_riscv_whole\n"
    "fcvt.w.s t2, fa0, rdn\n   fcvt.s.w ft0, t2\n   fsgnj.s fa0, ft0, fa0\n"
    ".Lnarrow_floor_riscv_whole:\n"
    ASM_RET
    ASM_END(narrow_floor)

    ASM_FUNC(narrow_ceiling)
    "fabs.s ft0, fa0\n   fmv.x.w t0, ft0\n   lui t1, 0x4b000\n"
    "bgeu t0, t1, .Lnarrow_ceiling_riscv_whole\n"
    "fcvt.w.s t2, fa0, rup\n   fcvt.s.w ft0, t2\n   fsgnj.s fa0, ft0, fa0\n"
    ".Lnarrow_ceiling_riscv_whole:\n"
    ASM_RET
    ASM_END(narrow_ceiling)

    ASM_FUNC(narrow_rounded)
    "fabs.s ft0, fa0\n   fmv.x.w t0, ft0\n   lui t1, 0x4b000\n"
    "bgeu t0, t1, .Lnarrow_rounded_riscv_whole\n"
    "fcvt.w.s t2, fa0, rmm\n   fcvt.s.w ft0, t2\n   fsgnj.s fa0, ft0, fa0\n"
    ".Lnarrow_rounded_riscv_whole:\n"
    ASM_RET
    ASM_END(narrow_rounded)

    //
    //       fsgnj.s is the sign transplant by name: the magnitude of the
    //       first operand with the sign of the second, which is what fabs.s
    //       and fneg.s are spellings of as well.
    //
    ASM_FUNC(narrow_with_sign)
    "fsgnj.s fa0, fa0, fa1\n"
    ASM_RET
    ASM_END(narrow_with_sign)
#endif
);
#endif

/*
        And the names a C program knows them by.

        Out of a kernel build, where the kernel writes its own isdigit and
        two definitions of it is one too many.
*/
#ifndef KERNEL_MODE
__asm__(
    ASM_ALIAS(isdigit,  byte_is_digit)
    ASM_ALIAS(isupper,  byte_is_upper)
    ASM_ALIAS(islower,  byte_is_lower)
    ASM_ALIAS(isalpha,  byte_is_alpha)
    ASM_ALIAS(isalnum,  byte_is_alnum)
    ASM_ALIAS(isspace,  byte_is_space)
    ASM_ALIAS(isxdigit, byte_is_hexadecimal)
    ASM_ALIAS(toupper,  byte_to_upper)
    ASM_ALIAS(tolower,  byte_to_lower)
    ASM_ALIAS(sqrt,     square_root)
    ASM_ALIAS(fabs,     absolute)
    ASM_ALIAS(abs,      absolute_whole)
    ASM_ALIAS(labs,     absolute_wide)
    ASM_ALIAS(llabs,    absolute_wide)

    ASM_ALIAS(isprint,  byte_is_printable)
    ASM_ALIAS(isgraph,  byte_is_graphic)
    ASM_ALIAS(iscntrl,  byte_is_control)
    ASM_ALIAS(ispunct,  byte_is_punctuation)
    ASM_ALIAS(isblank,  byte_is_blank)
    ASM_ALIAS(isascii,  byte_is_ascii)
    ASM_ALIAS(toascii,  byte_to_ascii)

    ASM_ALIAS(strspn,    string_span_of_set)
    ASM_ALIAS(strcspn,   string_span_without_set)
    ASM_ALIAS(strpbrk,   string_first_of_set)

    ASM_ALIAS(strcasecmp,  string_compare_folded)
    ASM_ALIAS(strncasecmp, string_compare_folded_max)

    ASM_ALIAS(memrchr,  memory_last_of)
    ASM_ALIAS(memccpy,  memory_copy_until)
    ASM_ALIAS(stpcpy,   string_copy_end)
    ASM_ALIAS(stpncpy,  string_copy_max_endptr)
    ASM_ALIAS(strncat,  string_append_max)
    ASM_ALIAS(bcmp,     memory_compare)
    ASM_ALIAS(bzero,    memory_zero)
    ASM_ALIAS(bcopy,    memory_copy_source_first)

    ASM_ALIAS(atoi,     string_to_whole)
    ASM_ALIAS(atol,     string_to_whole_wide)
    ASM_ALIAS(atoll,    string_to_whole_wide)

    ASM_ALIAS(ffs,      bits_first_set)
    ASM_ALIAS(ffsl,     bits_first_set_wide)
    ASM_ALIAS(ffsll,    bits_first_set_wide)

    ASM_ALIAS(trunc,     decimal_truncated)
    ASM_ALIAS(floor,     decimal_floor)
    ASM_ALIAS(ceil,      decimal_ceiling)
    ASM_ALIAS(round,     decimal_rounded)
    ASM_ALIAS(nearbyint, decimal_nearest)
    ASM_ALIAS(rint,      decimal_nearest)
    ASM_ALIAS(copysign,  decimal_with_sign)
    ASM_ALIAS(fmin,      decimal_smaller)
    ASM_ALIAS(fmax,      decimal_larger)
    ASM_ALIAS(fdim,      decimal_difference)
    ASM_ALIAS(fma,       decimal_multiply_add)

    ASM_ALIAS(sqrtf,     narrow_square_root)
    ASM_ALIAS(fabsf,     narrow_absolute)
    ASM_ALIAS(fminf,     narrow_smaller)
    ASM_ALIAS(fmaxf,     narrow_larger)
    ASM_ALIAS(truncf,    narrow_truncated)
    ASM_ALIAS(floorf,    narrow_floor)
    ASM_ALIAS(ceilf,     narrow_ceiling)
    ASM_ALIAS(roundf,    narrow_rounded)
    ASM_ALIAS(copysignf, narrow_with_sign)
);
#endif

#endif // STANDARD_MODERN_C_PLATFORM_STANDARD
