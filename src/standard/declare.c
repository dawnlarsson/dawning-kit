/*
        Experimental C standard library

        The prototypes for the standard names library.c makes symbols of

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        The names a C program spells, said out loud.

        library.c attaches the standard spelling of a routine with ASM_ALIAS,
        which emits `.set memcpy, memory_copy`: one address wearing two
        labels. That is the whole of what a linker needs and none of what a
        compiler needs. nm on a built object shows "T memcpy", "T strlen",
        "T sqrt", "T toupper" -- and `&memcpy` did not compile, because no
        translation unit in the tree had ever said what memcpy was. Ninety
        eight names were in that state. The code was right there, the symbol
        was right there, and a program that called one failed at the parse.

        This file is those prototypes and nothing else. No bodies, no macros
        wearing the names, no storage of any kind. It is included first among
        the standard families for exactly that reason: a file that emits
        nothing cannot collide with a file that emits something, so putting
        it first costs nothing and puts every standard name in scope before
        any of the eight families that might want one.

        Three rules decided every line below.

        THE TYPES ARE C's, NOT THE HOUSE'S. Everywhere else in this tree a
        length is `positive` and a string is `string_address`, and those are
        the right names for the prose routines, which are declared with
        exactly those types beside their assembly. They are the wrong names
        here. `positive` is `unsigned long long`; size_t on all three
        architectures this builds for is `unsigned long`. Same width, same
        register, different type. A program that brings its own
        `size_t strlen(const char *)` -- which is what a ported file does the
        moment somebody writes out what <string.h> would have said -- would
        meet `positive strlen(string_address)` here and get a conflicting
        types error, and the file written to let real C compile would be the
        reason it did not. So a size is `sized`, which library.c defines as
        typeof(sizeof(0)) and which is therefore the compiler's own size_t by
        construction rather than by resemblance, and a character is `char`,
        because `p8` is `unsigned char` and C's string functions do not take
        one. src/test/declare.c is the proof: one case file, compiled once
        against these declarations and once against the host's real headers,
        and it has to build both ways.

        THE PROTOTYPE IS THE STANDARD'S, EVEN WHEN THE ROUTINE IS NOT. C says
        memchr takes an int and converts it to an unsigned char; the assembly
        reads the low byte of that register and ignores the rest, which is
        the same thing, so `int` is the honest declaration even though the
        prose routine says `p8`. That is a difference of spelling. A
        difference of meaning is a different matter and is never smoothed
        over -- there is exactly one, it is named below, and it is repeated
        at its own declaration.

        NOTHING IS RENAMED. Each name here is a second label on an address,
        so `&memcpy == &memory_copy` and a function pointer taken through
        either is the same pointer. In particular no name here is a macro,
        which is what makes taking the address work and what lets a program
        supply its own declaration alongside.

        ONE PLACE WHERE THE ROUTINE IS NOT WHAT C DESCRIBES, and it was
        there before this file was. A declaration cannot change what the
        assembly does, and one that lied about it would be worse than no
        declaration at all -- so the number here is measured and not
        estimated. src/test/declare_cases.inc puts four hundred and forty two
        lines of raw answer through these names and through the host's glibc
        and diffs the two, and exactly one line differs.

        strncpy is string_copy_max, which stops at the source's terminator
        and writes nothing after it. C says pad the rest of the bound with
        zeroes. The alias predates this file and the note beside it in
        library.c says the same thing; the case file writes out both
        behaviours so the difference lives in the test rather than only in a
        comment about it.

        AND THREE THAT LOOK LIKE DIVERGENCES AND ARE NOT, which is the reason
        they are measured rather than assumed.

        strrchr is string_last_of_or_end, and the name reads like a promise
        to answer with the end of the string when the byte is absent. It does
        not: a miss is a null pointer, exactly as C says. The "or end" is
        about the one input where the byte hunted for IS the terminator,
        where the answer is the end of the string -- which is also what C
        says. The assembly at library.c:2895 says so and the test says so.

        memmove and memcpy are one routine, memory_copy, and that routine is
        safe for overlap in both directions. So memmove is right and memcpy
        is merely stricter than it has to be. The test slides an eight byte
        block over itself each way to say which direction the guarantee runs
        in, because one address under both names is the shape a wrong answer
        would also have.

        strstr and memmem answer an empty needle with the front of the
        haystack, which is what C and POSIX respectively require, and strnchr
        is not a C name at all -- it is the kernel's, and its length comes
        before its character.

        And one thing this file deliberately does not do. The known-size
        specializers in the umbrella are macros over the PROSE names, so
        memory_copy(d, s, 8) folds to two moves and memcpy(d, s, 8) is a
        call. Making the standard names fold too would mean defining them as
        macros, which would break both `&memcpy` and a program's own
        declaration -- the two things this file exists to allow. Code in this
        tree that wants the fold calls the prose name, which is what it
        already does.
*/
#ifndef STANDARD_MODERN_C_STANDARD_DECLARE
#define STANDARD_MODERN_C_STANDARD_DECLARE

#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        library.c defines a function-like macro floor(a) that casts through
        bipolar. The token `floor` followed by an open bracket therefore
        became a cast and not a call, which is a truncation toward zero and
        not a floor at all for a negative argument, and it is enough to turn
        the declaration below into a syntax error. Nothing in the tree ever
        expanded it. decimal_floor, which the symbol `floor` is a second
        label on, is the real thing.
*/
#undef floor

/*
        <string.h>, the part of it library.c writes in assembly.

        strncpy is the one name in this group that does not mean what C means
        by it: it pads nothing. The divergence is at the top of the file and
        both behaviours are written out in src/test/declare_cases.inc.
*/
void address_to memcpy(void address_to destination, const void address_to source,
                       sized size);
void address_to memmove(void address_to destination, const void address_to source,
                        sized size);
void address_to memset(void address_to destination, int value, sized size);
void address_to memchr(const void address_to block, int value, sized size);
void address_to memrchr(const void address_to block, int value, sized size);
int memcmp(const void address_to first, const void address_to second, sized size);
void address_to memmem(const void address_to haystack, sized haystack_size,
                       const void address_to needle, sized needle_size);
void address_to memccpy(void address_to destination, const void address_to source,
                        int stop, sized size);

sized strlen(const char address_to source);
sized strnlen(const char address_to source, sized bound);
int strcmp(const char address_to source, const char address_to input);
int strncmp(const char address_to source, const char address_to input, sized bound);
char address_to strchr(const char address_to source, int character);
char address_to strrchr(const char address_to source, int character);
char address_to strchrnul(const char address_to source, int character);
char address_to strnchr(const char address_to source, sized bound, int character);
char address_to strstr(const char address_to haystack, const char address_to needle);
char address_to strcpy(char address_to destination, const char address_to source);
char address_to strncpy(char address_to destination, const char address_to source,
                        sized bound);
char address_to strcat(char address_to destination, const char address_to source);
char address_to strncat(char address_to destination, const char address_to source,
                        sized bound);
char address_to stpcpy(char address_to destination, const char address_to source);
char address_to stpncpy(char address_to destination, const char address_to source,
                        sized bound);
sized strspn(const char address_to source, const char address_to accept);
sized strcspn(const char address_to source, const char address_to reject);
char address_to strpbrk(const char address_to source, const char address_to accept);
int strcasecmp(const char address_to source, const char address_to input);
int strncasecmp(const char address_to source, const char address_to input,
                sized bound);

/*
        The names an instrumented build routes through, which are the same
        three addresses. They are not C's and no program should reach for
        them; they are declared because the symbols are there and a
        declaration that stops at the ones with a standard behind them would
        leave the reader guessing whether the rest were an oversight.
*/
void address_to __memcpy(void address_to destination, const void address_to source,
                         sized size);
void address_to __memmove(void address_to destination, const void address_to source,
                          sized size);
void address_to __memset(void address_to destination, int value, sized size);

/*
        <strings.h>, which is where the pre-standard spellings went and where
        POSIX left them. bcopy takes its arguments the other way round from
        memmove and always has.
*/
int bcmp(const void address_to first, const void address_to second, sized size);
void bzero(void address_to destination, sized size);
void bcopy(const void address_to source, void address_to destination, sized size);

/*
        <ctype.h>.

        Every one of these takes an int and not a char, and the reason is
        EOF: the argument is "an unsigned char, or EOF", so the range is
        -1 through 255 and a type that could not hold -1 could not say EOF.
        A program that passes a plain char with the top bit set has undefined
        behaviour in C and gets whatever the routine does with a sign
        extended byte here, which is measured in src/test/declare.c against
        glibc for every value from -1 to 255.
*/
int isalnum(int value);
int isalpha(int value);
int isblank(int value);
int iscntrl(int value);
int isdigit(int value);
int isgraph(int value);
int islower(int value);
int isprint(int value);
int ispunct(int value);
int isspace(int value);
int isupper(int value);
int isxdigit(int value);
int tolower(int value);
int toupper(int value);
int isascii(int value);
int toascii(int value);

/*
        <stdlib.h>'s conversions and absolutes.

        abs, labs and llabs are three names for two routines: labs and llabs
        are both absolute_wide, which is correct on every architecture this
        builds for because all three are LP64 and long and long long are the
        same width in the same register. It would be wrong on a machine where
        they were not, and no such machine is a target.
*/
int atoi(const char address_to input);
long atol(const char address_to input);
long long atoll(const char address_to input);
long strtol(const char address_to input, char address_to address_to stopped,
            int base);
long long strtoll(const char address_to input, char address_to address_to stopped,
                  int base);
unsigned long strtoul(const char address_to input, char address_to address_to stopped,
                      int base);
unsigned long long strtoull(const char address_to input,
                            char address_to address_to stopped, int base);
int abs(int value);
long labs(long value);
long long llabs(long long value);

/*
        <strings.h> again: the bit index, one based, zero for no bits set.
*/
int ffs(int value);
int ffsl(long value);
int ffsll(long long value);

/*
        <math.h>, the part of it that is one instruction on all three
        architectures. The rest of math is C in src/standard/math.c and is
        declared there.

        floor is why this file begins with an #undef, and the reason is up
        there with it.
*/
double sqrt(double value);
double fabs(double value);
double trunc(double value);
double floor(double value);
double ceil(double value);
double round(double value);
double nearbyint(double value);
double rint(double value);
double copysign(double magnitude, double sign);
double fmin(double first, double second);
double fmax(double first, double second);
double fdim(double first, double second);
double fma(double first, double second, double addend);

float sqrtf(float value);
float fabsf(float value);
float truncf(float value);
float floorf(float value);
float ceilf(float value);
float roundf(float value);
float copysignf(float magnitude, float sign);
float fminf(float first, float second);
float fmaxf(float first, float second);

/*
        The rest of <string.h>, whose bodies are C in src/standard/text.c and
        whose standard names are attached there by the same ASM_ALIAS with
        the same missing prototype. They are declared here rather than there
        for one reason: this file is included before that one, so a program
        that includes the umbrella has every standard name in scope at the
        same point, and there is one place to look for the list.

        strdup and strndup allocate, so they need src/standard/allocator.c
        under them, which the umbrella includes.
*/
char address_to strdup(const char address_to source);
char address_to strndup(const char address_to source, sized bound);
char address_to strtok(char address_to source, const char address_to delimiters);
char address_to strtok_r(char address_to source, const char address_to delimiters,
                         char address_to address_to holder);
char address_to strsep(char address_to address_to holder,
                       const char address_to delimiters);
char address_to strcasestr(const char address_to haystack,
                           const char address_to needle);
sized strlcpy(char address_to destination, const char address_to source,
              sized bound);
sized strlcat(char address_to destination, const char address_to source,
              sized bound);
void address_to memfrob(void address_to block, sized size);

/*
        The non-local jump.

        setjmp's prototype cannot be separated from its buffer, and this
        library's buffer is not glibc's. jump_state is thirty two `positive`
        slots, two hundred and fifty six bytes, and it is that on all three
        architectures on purpose -- setjmp.inc says why. glibc's jmp_buf is a
        different shape and a different size, and a different one per
        architecture at that. So a program that supplies its own <setjmp.h>
        shaped declaration is not merely spelling a type differently, it is
        declaring an object that is not this one, and if it is the smaller of
        the two the failure is a smashed stack at the second arrival rather
        than anything that points at the cause.

        Declaring it here with jump_state means such a program gets a
        conflicting types error at the declaration instead, which is the
        outcome to want. The right fix in ported code is to use jump_state.

        setjmp and _setjmp are one address, because nothing here carries a
        signal mask, so the unprefixed name does not save one either --
        setjmp.inc says so at length and this is only the reminder. There is
        no sigsetjmp on purpose.

        returns_twice and noreturn are carried over from the prose
        declarations because they are not decoration: without the first the
        compiler may keep a value in a callee-saved register across the call,
        and the second arrival undoes that.

        The parameter is spelled `positive address_to` and not `jump_state`
        because the two are the same type here and only one of them is in
        scope. A parameter declared as an array is a pointer to its element,
        so `jump_mark(jump_state)` and `jump_mark(positive address_to)` are
        one function type and the compiler cannot tell them apart. Spelling
        it out is what lets this file stay ahead of setjmp.inc in the include
        order, which text.c is what pulls in; a declaration file that had to
        include a file full of assembly to declare four names would not be a
        declaration file any more.
*/
int setjmp(positive address_to state) __attribute__((returns_twice));
int _setjmp(positive address_to state) __attribute__((returns_twice));
void longjmp(positive address_to state, int value) DEAD_END;
void _longjmp(positive address_to state, int value) DEAD_END;

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_DECLARE
