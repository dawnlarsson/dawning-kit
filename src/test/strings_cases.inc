/*
        Experimental C standard library

        The cases for the rest of <string.h>, and for setjmp

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_TEST_STRINGS_CASES
#define STANDARD_MODERN_C_TEST_STRINGS_CASES

/*
        One list of cases, two libraries under it.

        src/test/strings.c builds this against the library, freestanding, on all
        three machines. src/test/strings_reference.c builds the same lines
        against the host's glibc. Both print the same verdict, and the point
        of the arrangement is that an expectation written here has to be true
        of glibc as well -- a wrong expectation fails the reference build
        rather than quietly agreeing with a wrong implementation, which is
        exactly the failure a hand-written expected-value table cannot catch.

        The two hosts supply everything below through text_ names: text_string
        for a string pointer, text_byte for one of its bytes, text_size for a
        length, TX() to make a literal one of them, and one macro per routine.
        Nothing here mentions strdup or string_duplicate, so nothing here
        knows which library it is testing.

        Cases that glibc cannot be asked -- the null start strtok_r rejects
        instead of faulting -- are marked where they appear and are the only
        places the two builds are not answering the identical question.
*/

/*
        strdup, whose only two questions are whether the bytes arrived and
        whether they arrived somewhere else.
*/
static void text_case_duplicate(void)
{
        text_byte source[] = "abcdef";
        text_string one = text_duplicate(TX("hello"));
        text_string two = text_duplicate(TX(""));
        text_string three = text_duplicate(source);

        check("strdup content", one != 0 && text_equal(one, TX("hello")));
        check("strdup length", one != 0 && text_length(one) == 5);
        check("strdup empty length", two != 0 && text_length(two) == 0);
        check("strdup empty terminator", two != 0 && two[0] == 0);
        check("strdup is a copy", three != 0 && three != (text_string)source);
        check("strdup copy content",
              three != 0 && text_equal(three, TX("abcdef")));
        check("strdup copy is writable", three != 0 && (three[0] = 'z') == 'z');
        check("strdup copy did not disturb its source", source[0] == 'a');
}

/*
        strndup, and the one byte that is not in the bound.

        The block is the bound plus a terminator, so strndup(s, 3) on "hello"
        is four bytes and not three. The last case has a source that is
        exactly its own array with no bytes after it, so a bound equal to the
        length proves nothing is read past the terminator.
*/
static void text_case_duplicate_max(void)
{
        text_byte source[] = "hello";
        text_byte tight[4] = {'a', 'b', 'c', 0};
        text_string a = text_duplicate_max(source, 3);
        text_string b = text_duplicate_max(source, 5);
        text_string c = text_duplicate_max(source, 40);
        text_string d = text_duplicate_max(source, 0);
        text_string e = text_duplicate_max(tight, 3);

        check("strndup short bound", a != 0 && text_equal(a, TX("hel")));
        check("strndup short length", a != 0 && text_length(a) == 3);
        check("strndup exact bound", b != 0 && text_equal(b, TX("hello")));
        check("strndup bound past the end", c != 0 && text_equal(c, TX("hello")));
        check("strndup zero bound", d != 0 && text_length(d) == 0);
        check("strndup zero bound terminator", d != 0 && d[0] == 0);
        check("strndup stops at the terminator", e != 0 && text_equal(e, TX("abc")));
}

/*
        strtok_r: runs of delimiters are one delimiter, and the input is
        destroyed as it goes.

        The last group is the reason strtok_r exists at all -- two walks
        interleaved, each carrying its own place, neither seeing the other.
*/
static void text_case_token_next(void)
{
        text_byte one[] = "a,b,,c";
        text_byte two[] = "  hello   world  ";
        text_byte three[] = ",,,";
        text_byte four[] = "";
        text_byte five[] = "abc";
        text_byte six[] = "a;b,c";
        text_byte left[] = "1 2 3";
        text_byte right[] = "x y z";
        text_string place = 0;
        text_string left_place = 0;
        text_string right_place = 0;
        text_string got;

        got = text_token_next(one, TX(","), &place);
        check("strtok_r first token", got != 0 && text_equal(got, TX("a")));
        got = text_token_next(0, TX(","), &place);
        check("strtok_r second token", got != 0 && text_equal(got, TX("b")));
        got = text_token_next(0, TX(","), &place);
        check("strtok_r skips the empty field", got != 0 && text_equal(got, TX("c")));
        got = text_token_next(0, TX(","), &place);
        check("strtok_r exhausted", got == 0);
        got = text_token_next(0, TX(","), &place);
        check("strtok_r stays exhausted", got == 0);
        check("strtok_r terminated the first token", one[1] == 0);
        check("strtok_r terminated the second token", one[3] == 0);
        check("strtok_r left the skipped delimiter alone", one[4] == ',');

        place = 0;
        got = text_token_next(two, TX(" "), &place);
        check("strtok_r skips leading blanks", got != 0 && text_equal(got, TX("hello")));
        got = text_token_next(0, TX(" "), &place);
        check("strtok_r crosses a run of blanks", got != 0 && text_equal(got, TX("world")));
        got = text_token_next(0, TX(" "), &place);
        check("strtok_r ends on trailing blanks", got == 0);

        place = 0;
        got = text_token_next(three, TX(","), &place);
        check("strtok_r all delimiters", got == 0);

        place = 0;
        got = text_token_next(four, TX(","), &place);
        check("strtok_r empty input", got == 0);

        place = 0;
        got = text_token_next(five, TX(""), &place);
        check("strtok_r empty delimiters", got != 0 && text_equal(got, TX("abc")));
        got = text_token_next(0, TX(""), &place);
        check("strtok_r empty delimiters once only", got == 0);

        place = 0;
        got = text_token_next(six, TX(",;"), &place);
        check("strtok_r two delimiters first", got != 0 && text_equal(got, TX("a")));
        got = text_token_next(0, TX(",;"), &place);
        check("strtok_r two delimiters second", got != 0 && text_equal(got, TX("b")));
        got = text_token_next(0, TX(",;"), &place);
        check("strtok_r two delimiters third", got != 0 && text_equal(got, TX("c")));
        got = text_token_next(0, TX(",;"), &place);
        check("strtok_r two delimiters done", got == 0);

        got = text_token_next(left, TX(" "), &left_place);
        check("strtok_r interleaved left first", got != 0 && text_equal(got, TX("1")));
        got = text_token_next(right, TX(" "), &right_place);
        check("strtok_r interleaved right first", got != 0 && text_equal(got, TX("x")));
        got = text_token_next(0, TX(" "), &left_place);
        check("strtok_r interleaved left second", got != 0 && text_equal(got, TX("2")));
        got = text_token_next(0, TX(" "), &right_place);
        check("strtok_r interleaved right second", got != 0 && text_equal(got, TX("y")));
        got = text_token_next(0, TX(" "), &left_place);
        check("strtok_r interleaved left third", got != 0 && text_equal(got, TX("3")));
        got = text_token_next(0, TX(" "), &right_place);
        check("strtok_r interleaved right third", got != 0 && text_equal(got, TX("z")));
        got = text_token_next(0, TX(" "), &left_place);
        check("strtok_r interleaved left done", got == 0);
        got = text_token_next(0, TX(" "), &right_place);
        check("strtok_r interleaved right done", got == 0);
}

/*
        strtok, which is strtok_r with one place shared by the whole program.
*/
static void text_case_token(void)
{
        text_byte one[] = "one:two::three";
        text_string got;

        got = text_token(one, TX(":"));
        check("strtok first", got != 0 && text_equal(got, TX("one")));
        got = text_token(0, TX(":"));
        check("strtok second", got != 0 && text_equal(got, TX("two")));
        got = text_token(0, TX(":"));
        check("strtok skips the empty field", got != 0 && text_equal(got, TX("three")));
        got = text_token(0, TX(":"));
        check("strtok exhausted", got == 0);
}

/*
        strsep, which keeps every field strtok throws away.

        The same input as the first strtok_r group, so the two answers can be
        read against each other: four fields here where there were three
        tokens there, and the difference is the one between them.
*/
static void text_case_split(void)
{
        text_byte one[] = "a,b,,c";
        text_byte empty[] = "";
        text_byte plain[] = "abc";
        text_byte leading[] = ",a";
        text_byte trailing[] = "a,";
        text_string walk;
        text_string got;

        walk = one;
        got = text_split_next(&walk, TX(","));
        check("strsep first field", got != 0 && text_equal(got, TX("a")));
        got = text_split_next(&walk, TX(","));
        check("strsep second field", got != 0 && text_equal(got, TX("b")));
        got = text_split_next(&walk, TX(","));
        check("strsep keeps the empty field", got != 0 && text_equal(got, TX("")));
        got = text_split_next(&walk, TX(","));
        check("strsep last field", got != 0 && text_equal(got, TX("c")));
        check("strsep holder is null at the end", walk == 0);
        got = text_split_next(&walk, TX(","));
        check("strsep past the end", got == 0);

        walk = empty;
        got = text_split_next(&walk, TX(","));
        check("strsep empty input is one empty field",
              got != 0 && text_equal(got, TX("")));
        check("strsep empty input ends the walk", walk == 0);
        got = text_split_next(&walk, TX(","));
        check("strsep empty input past the end", got == 0);

        walk = plain;
        got = text_split_next(&walk, TX(","));
        check("strsep with no delimiter", got != 0 && text_equal(got, TX("abc")));
        check("strsep with no delimiter ends the walk", walk == 0);

        walk = plain;
        got = text_split_next(&walk, TX(""));
        check("strsep empty delimiters", got != 0 && text_equal(got, TX("abc")));
        check("strsep empty delimiters ends the walk", walk == 0);

        walk = leading;
        got = text_split_next(&walk, TX(","));
        check("strsep leading delimiter is an empty field",
              got != 0 && text_equal(got, TX("")));
        got = text_split_next(&walk, TX(","));
        check("strsep after a leading delimiter",
              got != 0 && text_equal(got, TX("a")));
        check("strsep leading delimiter ends the walk", walk == 0);

        walk = trailing;
        got = text_split_next(&walk, TX(","));
        check("strsep before a trailing delimiter",
              got != 0 && text_equal(got, TX("a")));
        got = text_split_next(&walk, TX(","));
        check("strsep trailing delimiter is an empty field",
              got != 0 && text_equal(got, TX("")));
        check("strsep trailing delimiter ends the walk", walk == 0);
}

/*
        strcasestr, including the long haystack and the four byte needle that
        together reach the anchored path rather than the byte loop.
*/
static void text_case_search_folded(void)
{
        text_byte hay[] = "Hello World";
        text_byte overlap[] = "aaaB";
        text_byte brackets[] = "Z_[]";
        text_byte long_hay[] =
            "the quick brown fox jumps over the lazy dog and then some "
            "more filler so this is well past sixty four bytes HERE";
        text_string got;

        got = text_search_folded(hay, TX("world"));
        check("strcasestr folds the needle", got == hay + 6);
        got = text_search_folded(hay, TX("WORLD"));
        check("strcasestr folds the haystack", got == hay + 6);
        got = text_search_folded(hay, TX("hello world"));
        check("strcasestr whole haystack", got == hay);
        got = text_search_folded(hay, TX(""));
        check("strcasestr empty needle is the front", got == hay);
        got = text_search_folded(hay, TX("xyz"));
        check("strcasestr absent needle", got == 0);
        got = text_search_folded(hay, TX("Hello World and more"));
        check("strcasestr needle longer than haystack", got == 0);

        got = text_search_folded(overlap, TX("aab"));
        check("strcasestr overlapping candidate", got == overlap + 1);

        got = text_search_folded(brackets, TX("z_"));
        check("strcasestr underscore is itself", got == brackets);
        got = text_search_folded(brackets, TX("z["));
        check("strcasestr bracket is not a letter", got == 0);

        got = text_search_folded(long_hay, TX("here"));
        check("strcasestr long haystack",
              got == long_hay + text_length(long_hay) - 4);
        got = text_search_folded(long_hay, TX("QUICK"));
        check("strcasestr long haystack early hit", got == long_hay + 4);
        got = text_search_folded(long_hay, TX("zebra"));
        check("strcasestr long haystack miss", got == 0);
}

/*
        strlcpy: the answer is what the source wanted, and the buffer always
        ends inside itself.

        Every case fills the buffer with a guard byte first, so a routine that
        wrote one byte too many is caught by the guard rather than by luck.
*/
static void text_case_copy_bounded(void)
{
        text_byte buffer[16];
        text_size answer;

        text_fill(buffer, 0x7e, sizeof buffer);
        answer = text_copy_bounded(buffer, TX("hello"), 10);
        check("strlcpy answer is the source length", answer == 5);
        check("strlcpy content", text_equal(buffer, TX("hello")));
        check("strlcpy wrote no further", buffer[6] == 0x7e);

        text_fill(buffer, 0x7e, sizeof buffer);
        answer = text_copy_bounded(buffer, TX("hello"), 6);
        check("strlcpy exact fit answer", answer == 5);
        check("strlcpy exact fit content", text_equal(buffer, TX("hello")));
        check("strlcpy exact fit wrote no further", buffer[6] == 0x7e);

        text_fill(buffer, 0x7e, sizeof buffer);
        answer = text_copy_bounded(buffer, TX("hello"), 3);
        check("strlcpy truncated answer", answer == 5);
        check("strlcpy truncated content", text_equal(buffer, TX("he")));
        check("strlcpy truncated wrote no further", buffer[3] == 0x7e);

        text_fill(buffer, 0x7e, sizeof buffer);
        answer = text_copy_bounded(buffer, TX("hello"), 1);
        check("strlcpy capacity one answer", answer == 5);
        check("strlcpy capacity one is empty", buffer[0] == 0);
        check("strlcpy capacity one wrote no further", buffer[1] == 0x7e);

        text_fill(buffer, 0x7e, sizeof buffer);
        answer = text_copy_bounded(buffer, TX("hello"), 0);
        check("strlcpy capacity zero answer", answer == 5);
        check("strlcpy capacity zero writes nothing", buffer[0] == 0x7e);

        text_fill(buffer, 0x7e, sizeof buffer);
        answer = text_copy_bounded(buffer, TX(""), 4);
        check("strlcpy empty source answer", answer == 0);
        check("strlcpy empty source content", buffer[0] == 0);
        check("strlcpy empty source wrote no further", buffer[1] == 0x7e);
}

/*
        strlcat, and the destination that holds no terminator inside the
        capacity -- which is where an implementation either notices or walks
        off the end of a buffer.
*/
static void text_case_append_bounded(void)
{
        text_byte buffer[16];
        text_size answer;

        text_fill(buffer, 0x7e, sizeof buffer);
        buffer[0] = 'a';
        buffer[1] = 'b';
        buffer[2] = 0;
        answer = text_append_bounded(buffer, TX("cd"), 10);
        check("strlcat answer is the whole length", answer == 4);
        check("strlcat content", text_equal(buffer, TX("abcd")));
        check("strlcat wrote no further", buffer[5] == 0x7e);

        text_fill(buffer, 0x7e, sizeof buffer);
        buffer[0] = 'a';
        buffer[1] = 'b';
        buffer[2] = 0;
        answer = text_append_bounded(buffer, TX("cdef"), 5);
        check("strlcat truncated answer", answer == 6);
        check("strlcat truncated content", text_equal(buffer, TX("abcd")));
        check("strlcat truncated wrote no further", buffer[5] == 0x7e);

        text_fill(buffer, 0x7e, sizeof buffer);
        buffer[0] = 'a';
        buffer[1] = 'b';
        buffer[2] = 0;
        answer = text_append_bounded(buffer, TX("cd"), 3);
        check("strlcat no room answer", answer == 4);
        check("strlcat no room content", text_equal(buffer, TX("ab")));
        check("strlcat no room wrote no further", buffer[3] == 0x7e);

        text_fill(buffer, 0x7e, sizeof buffer);
        buffer[0] = 'a';
        buffer[1] = 'b';
        buffer[2] = 0;
        answer = text_append_bounded(buffer, TX("cd"), 2);
        check("strlcat capacity equals the held length", answer == 4);
        check("strlcat capacity equals held writes nothing",
              text_equal(buffer, TX("ab")));

        text_fill(buffer, 0x7e, sizeof buffer);
        buffer[0] = 'a';
        buffer[1] = 'b';
        buffer[2] = 0;
        answer = text_append_bounded(buffer, TX("cd"), 0);
        check("strlcat capacity zero answer", answer == 2);
        check("strlcat capacity zero writes nothing",
              text_equal(buffer, TX("ab")));

        text_fill(buffer, 'x', sizeof buffer);
        answer = text_append_bounded(buffer, TX("y"), 4);
        check("strlcat unterminated destination answer", answer == 5);
        check("strlcat unterminated destination untouched",
              buffer[0] == 'x' && buffer[3] == 'x' && buffer[4] == 'x');

        text_fill(buffer, 0x7e, sizeof buffer);
        buffer[0] = 0;
        answer = text_append_bounded(buffer, TX("cd"), 8);
        check("strlcat onto an empty destination answer", answer == 2);
        check("strlcat onto an empty destination content",
              text_equal(buffer, TX("cd")));
}

/*
        memfrob, whose whole specification is that doing it twice undoes it.
*/
static void text_case_frob(void)
{
        text_byte block[6] = "hello";

        text_frob(block, 5);
        check("memfrob first byte", block[0] == (text_byte)('h' ^ 42));
        check("memfrob last byte", block[4] == (text_byte)('o' ^ 42));
        check("memfrob left the terminator", block[5] == 0);
        text_frob(block, 5);
        check("memfrob is its own inverse", text_equal(block, TX("hello")));

        text_frob(block, 0);
        check("memfrob of nothing", text_equal(block, TX("hello")));
}

/*
        The non-local exit.

        text_case_jump_level_one through three make three real frames between
        the mark and the jump, and each adds to a volatile counter so the
        depth reached is checked rather than assumed. Nothing tail calls: the
        statement after each nested call is dead but it is what stops the
        compiler from turning three frames into none.

        text_case_scramble_jump is the deepest step and it is assembly,
        because the thing being tested is that longjmp puts the callee-saved
        registers back, and a register the C chain never touched proves
        nothing. It writes a pattern into every one of them and then jumps.
*/
text_jump_state text_case_state;
static volatile unsigned long long text_case_depth;
static volatile double text_case_seed_decimal = 0.5;
static volatile unsigned long long text_case_seed_whole = 0x0102030405060708ULL;

void text_case_scramble_jump(text_jump_state state, int value);
void text_case_jump_level_one(int value);
unsigned long long text_case_register_probe(void);

void text_case_jump_level_three(int value)
{
        text_case_depth += 4;
        text_case_scramble_jump(text_case_state, value);
        text_case_depth += 1000;
}

void text_case_jump_level_two(int value)
{
        text_case_depth += 2;
        text_case_jump_level_three(value);
        text_case_depth += 1000;
}

void text_case_jump_level_one(int value)
{
        text_case_depth += 1;
        text_case_jump_level_two(value);
        text_case_depth += 1000;
}

/*
        Eight doubles and eight integers, each derived from a volatile read so
        the compiler cannot fold them away, live across the mark and read
        after it.

        On arm64 that is what asks for d8 through d15, and on riscv64 for fs0
        through fs11 -- the halves of the save list a byte-only test never
        reaches. Where the compiler spills them to the stack instead the sums
        still have to come back, which tests the saved stack pointer. Neither
        value is written between the mark and the jump, so both are defined
        after the second arrival rather than merely likely.
*/
static double text_case_jump_decimals(void)
{
        volatile int came;
        double base = text_case_seed_decimal;
        double a = base + 1.0;
        double b = base + 2.0;
        double c = base + 3.0;
        double d = base + 4.0;
        double e = base + 5.0;
        double f = base + 6.0;
        double g = base + 7.0;
        double h = base + 8.0;

        came = text_jump_mark(text_case_state);

        if (came == 0)
                text_case_jump_level_one(5);

        return a + b + c + d + e + f + g + h;
}

static unsigned long long text_case_jump_wholes(void)
{
        volatile int came;
        unsigned long long base = text_case_seed_whole;
        unsigned long long a = base + 1;
        unsigned long long b = base + 2;
        unsigned long long c = base + 3;
        unsigned long long d = base + 4;
        unsigned long long e = base + 5;
        unsigned long long f = base + 6;
        unsigned long long g = base + 7;
        unsigned long long h = base + 8;

        came = text_jump_mark(text_case_state);

        if (came == 0)
                text_case_jump_level_one(6);

        return a + b + c + d + e + f + g + h;
}

static void text_case_jump(void)
{
        volatile int came;

        text_case_depth = 0;
        came = text_jump_mark(text_case_state);

        if (came == 0)
                text_case_jump_level_one(7);

        check("setjmp answers seven where longjmp said seven", came == 7);
        check("longjmp crossed three frames", text_case_depth == 7);

        text_case_depth = 0;
        came = text_jump_mark(text_case_state);

        if (came == 0)
                text_case_jump_level_one(0);

        check("longjmp of zero arrives as one", came == 1);
        check("longjmp of zero crossed three frames", text_case_depth == 7);

        text_case_depth = 0;
        came = text_jump_mark(text_case_state);

        if (came == 0)
                text_case_jump_level_one(-3);

        check("longjmp carries a negative value", came == -3);

        check("doubles survive the jump", text_case_jump_decimals() == 40.0);
        check("integers survive the jump",
              text_case_jump_wholes() == 8 * 0x0102030405060708ULL + 36);

        check("every callee-saved register comes back",
              text_case_register_probe() == 0);
}

/*
        The list, so that the two builds cannot run different ones.

        Both hosts used to write this out for themselves, which is the one
        place in the arrangement where they could drift: a case added here and
        called from only one main would be checked against one library and
        silently not the other, and the two verdict lines would still look
        comparable, because both are the number of cases that ran.
*/
static void text_case_all(void)
{
        text_case_duplicate();
        text_case_duplicate_max();
        text_case_token_next();
        text_case_token();
        text_case_split();
        text_case_search_folded();
        text_case_copy_bounded();
        text_case_append_bounded();
        text_case_frob();
        text_case_jump();
}


/*
        The two assembly helpers, and why a C test cannot be either of them.

        text_case_scramble_jump writes a pattern into every callee-saved
        register the ABI names and then jumps. That is the half of the test
        that makes the other half mean anything: if the registers still held
        what the marking frame put there, a longjmp that restored none of them
        would pass.

        text_case_register_probe is the marking frame, written in assembly for
        the same reason. It saves the caller's registers the way any function
        must, fills every one of them with a known value, marks, and -- on the
        arrival that comes back through the jump -- exclusive-ors each against
        the value it should still hold and ors the differences together. Zero
        means every register on that machine's list came back. C cannot ask
        that question about x19 or s7; it can only hope the register allocator
        happened to use one.

        The values are sixteen bit constants on the two machines whose move
        immediate is sixteen bits wide, so no constant pool is needed and the
        whole probe is position independent by construction. The doubles are
        set and read as bit patterns through the general registers rather than
        compared as floating point, because what is being tested is whether
        sixty four bits came back, not whether two numbers are equal.

        Both call setjmp and longjmp by their libc names, which is what lets
        this same block test glibc in the reference build.
*/
#if defined(__x86_64__)

__asm__(
    ".text\n"
    ".globl text_case_scramble_jump\n"
    "text_case_scramble_jump:\n"
    "   movabs $0xd1d1d1d1d1d1d1d1, %rbx\n"
    "   movabs $0xd2d2d2d2d2d2d2d2, %rbp\n"
    "   movabs $0xd3d3d3d3d3d3d3d3, %r12\n"
    "   movabs $0xd4d4d4d4d4d4d4d4, %r13\n"
    "   movabs $0xd5d5d5d5d5d5d5d5, %r14\n"
    "   movabs $0xd6d6d6d6d6d6d6d6, %r15\n"
    "   jmp longjmp\n"

    ".globl text_case_register_probe\n"
    "text_case_register_probe:\n"
    "   push %rbx\n   push %rbp\n   push %r12\n"
    "   push %r13\n   push %r14\n   push %r15\n"
    "   sub $8, %rsp   # six pushes and the return address leave this odd\n"
    "   movabs $0xb1b1b1b1b1b1b1b1, %rbx\n"
    "   movabs $0xb2b2b2b2b2b2b2b2, %rbp\n"
    "   movabs $0xb3b3b3b3b3b3b3b3, %r12\n"
    "   movabs $0xb4b4b4b4b4b4b4b4, %r13\n"
    "   movabs $0xb5b5b5b5b5b5b5b5, %r14\n"
    "   movabs $0xb6b6b6b6b6b6b6b6, %r15\n"
    "   lea text_case_state(%rip), %rdi\n"
    "   call setjmp\n"
    "   test %eax, %eax\n"
    "   jnz 2f\n"
    "   mov $9, %edi\n"
    "   call text_case_jump_level_one   # three frames, then back to here\n"
    "2: xor %eax, %eax\n"
    "   movabs $0xb1b1b1b1b1b1b1b1, %rcx\n   xor %rbx, %rcx\n   or %rcx, %rax\n"
    "   movabs $0xb2b2b2b2b2b2b2b2, %rcx\n   xor %rbp, %rcx\n   or %rcx, %rax\n"
    "   movabs $0xb3b3b3b3b3b3b3b3, %rcx\n   xor %r12, %rcx\n   or %rcx, %rax\n"
    "   movabs $0xb4b4b4b4b4b4b4b4, %rcx\n   xor %r13, %rcx\n   or %rcx, %rax\n"
    "   movabs $0xb5b5b5b5b5b5b5b5, %rcx\n   xor %r14, %rcx\n   or %rcx, %rax\n"
    "   movabs $0xb6b6b6b6b6b6b6b6, %rcx\n   xor %r15, %rcx\n   or %rcx, %rax\n"
    "   add $8, %rsp\n"
    "   pop %r15\n   pop %r14\n   pop %r13\n"
    "   pop %r12\n   pop %rbp\n   pop %rbx\n"
    "   ret\n");

#elif defined(__aarch64__)

__asm__(
    ".text\n"
    ".globl text_case_scramble_jump\n"
    "text_case_scramble_jump:\n"
    "   mov x19, #0xdd01\n   mov x20, #0xdd02\n   mov x21, #0xdd03\n"
    "   mov x22, #0xdd04\n   mov x23, #0xdd05\n   mov x24, #0xdd06\n"
    "   mov x25, #0xdd07\n   mov x26, #0xdd08\n   mov x27, #0xdd09\n"
    "   mov x28, #0xdd0a\n   mov x29, #0xdd0b\n"
    "   mov x2, #0xee01\n   fmov d8,  x2\n"
    "   mov x2, #0xee02\n   fmov d9,  x2\n"
    "   mov x2, #0xee03\n   fmov d10, x2\n"
    "   mov x2, #0xee04\n   fmov d11, x2\n"
    "   mov x2, #0xee05\n   fmov d12, x2\n"
    "   mov x2, #0xee06\n   fmov d13, x2\n"
    "   mov x2, #0xee07\n   fmov d14, x2\n"
    "   mov x2, #0xee08\n   fmov d15, x2\n"
    "   b longjmp\n"

    ".globl text_case_register_probe\n"
    "text_case_register_probe:\n"
    "   stp x29, x30, [sp, #-176]!\n"
    "   mov x29, sp\n"
    "   stp x19, x20, [sp, #16]\n"
    "   stp x21, x22, [sp, #32]\n"
    "   stp x23, x24, [sp, #48]\n"
    "   stp x25, x26, [sp, #64]\n"
    "   stp x27, x28, [sp, #80]\n"
    "   stp d8,  d9,  [sp, #96]\n"
    "   stp d10, d11, [sp, #112]\n"
    "   stp d12, d13, [sp, #128]\n"
    "   stp d14, d15, [sp, #144]\n"
    "   mov x19, #0x1901\n   mov x20, #0x2002\n   mov x21, #0x2103\n"
    "   mov x22, #0x2204\n   mov x23, #0x2305\n   mov x24, #0x2406\n"
    "   mov x25, #0x2507\n   mov x26, #0x2608\n   mov x27, #0x2709\n"
    "   mov x28, #0x280a\n"
    "   mov x2, #0x1108\n   fmov d8,  x2\n"
    "   mov x2, #0x1209\n   fmov d9,  x2\n"
    "   mov x2, #0x130a\n   fmov d10, x2\n"
    "   mov x2, #0x140b\n   fmov d11, x2\n"
    "   mov x2, #0x150c\n   fmov d12, x2\n"
    "   mov x2, #0x160d\n   fmov d13, x2\n"
    "   mov x2, #0x170e\n   fmov d14, x2\n"
    "   mov x2, #0x180f\n   fmov d15, x2\n"
    "   adrp x0, text_case_state\n"
    "   add x0, x0, :lo12:text_case_state\n"
    "   bl setjmp\n"
    "   cbnz w0, 2f\n"
    "   mov w0, #9\n"
    "   bl text_case_jump_level_one   // three frames, then back to here\n"
    "2: mov x0, xzr\n"
    "   mov x1, #0x1901\n   eor x1, x19, x1\n   orr x0, x0, x1\n"
    "   mov x1, #0x2002\n   eor x1, x20, x1\n   orr x0, x0, x1\n"
    "   mov x1, #0x2103\n   eor x1, x21, x1\n   orr x0, x0, x1\n"
    "   mov x1, #0x2204\n   eor x1, x22, x1\n   orr x0, x0, x1\n"
    "   mov x1, #0x2305\n   eor x1, x23, x1\n   orr x0, x0, x1\n"
    "   mov x1, #0x2406\n   eor x1, x24, x1\n   orr x0, x0, x1\n"
    "   mov x1, #0x2507\n   eor x1, x25, x1\n   orr x0, x0, x1\n"
    "   mov x1, #0x2608\n   eor x1, x26, x1\n   orr x0, x0, x1\n"
    "   mov x1, #0x2709\n   eor x1, x27, x1\n   orr x0, x0, x1\n"
    "   mov x1, #0x280a\n   eor x1, x28, x1\n   orr x0, x0, x1\n"
    "   mov x1, sp\n   eor x1, x29, x1   // the frame pointer, against the stack\n"
    "   orr x0, x0, x1\n"
    "   fmov x1, d8\n    mov x2, #0x1108\n   eor x1, x1, x2\n   orr x0, x0, x1\n"
    "   fmov x1, d9\n    mov x2, #0x1209\n   eor x1, x1, x2\n   orr x0, x0, x1\n"
    "   fmov x1, d10\n   mov x2, #0x130a\n   eor x1, x1, x2\n   orr x0, x0, x1\n"
    "   fmov x1, d11\n   mov x2, #0x140b\n   eor x1, x1, x2\n   orr x0, x0, x1\n"
    "   fmov x1, d12\n   mov x2, #0x150c\n   eor x1, x1, x2\n   orr x0, x0, x1\n"
    "   fmov x1, d13\n   mov x2, #0x160d\n   eor x1, x1, x2\n   orr x0, x0, x1\n"
    "   fmov x1, d14\n   mov x2, #0x170e\n   eor x1, x1, x2\n   orr x0, x0, x1\n"
    "   fmov x1, d15\n   mov x2, #0x180f\n   eor x1, x1, x2\n   orr x0, x0, x1\n"
    "   ldp x19, x20, [sp, #16]\n"
    "   ldp x21, x22, [sp, #32]\n"
    "   ldp x23, x24, [sp, #48]\n"
    "   ldp x25, x26, [sp, #64]\n"
    "   ldp x27, x28, [sp, #80]\n"
    "   ldp d8,  d9,  [sp, #96]\n"
    "   ldp d10, d11, [sp, #112]\n"
    "   ldp d12, d13, [sp, #128]\n"
    "   ldp d14, d15, [sp, #144]\n"
    "   ldp x29, x30, [sp], #176\n"
    "   ret\n");

#elif defined(__riscv)

__asm__(
    ".text\n"
    ".globl text_case_scramble_jump\n"
    "text_case_scramble_jump:\n"
    "   li s0, 0xd001\n   li s1, 0xd002\n   li s2, 0xd003\n   li s3, 0xd004\n"
    "   li s4, 0xd005\n   li s5, 0xd006\n   li s6, 0xd007\n   li s7, 0xd008\n"
    "   li s8, 0xd009\n   li s9, 0xd00a\n   li s10, 0xd00b\n   li s11, 0xd00c\n"
    "   li t0, 0xe001\n   fmv.d.x fs0, t0\n"
    "   li t0, 0xe002\n   fmv.d.x fs1, t0\n"
    "   li t0, 0xe003\n   fmv.d.x fs2, t0\n"
    "   li t0, 0xe004\n   fmv.d.x fs3, t0\n"
    "   li t0, 0xe005\n   fmv.d.x fs4, t0\n"
    "   li t0, 0xe006\n   fmv.d.x fs5, t0\n"
    "   li t0, 0xe007\n   fmv.d.x fs6, t0\n"
    "   li t0, 0xe008\n   fmv.d.x fs7, t0\n"
    "   li t0, 0xe009\n   fmv.d.x fs8, t0\n"
    "   li t0, 0xe00a\n   fmv.d.x fs9, t0\n"
    "   li t0, 0xe00b\n   fmv.d.x fs10, t0\n"
    "   li t0, 0xe00c\n   fmv.d.x fs11, t0\n"
    "   tail longjmp\n"

    ".globl text_case_register_probe\n"
    "text_case_register_probe:\n"
    "   addi sp, sp, -224\n"
    "   sd ra, 216(sp)\n"
    "   sd s0, 0(sp)\n     sd s1, 8(sp)\n     sd s2, 16(sp)\n"
    "   sd s3, 24(sp)\n    sd s4, 32(sp)\n    sd s5, 40(sp)\n"
    "   sd s6, 48(sp)\n    sd s7, 56(sp)\n    sd s8, 64(sp)\n"
    "   sd s9, 72(sp)\n    sd s10, 80(sp)\n   sd s11, 88(sp)\n"
    "   fsd fs0, 96(sp)\n  fsd fs1, 104(sp)\n fsd fs2, 112(sp)\n"
    "   fsd fs3, 120(sp)\n fsd fs4, 128(sp)\n fsd fs5, 136(sp)\n"
    "   fsd fs6, 144(sp)\n fsd fs7, 152(sp)\n fsd fs8, 160(sp)\n"
    "   fsd fs9, 168(sp)\n fsd fs10, 176(sp)\n fsd fs11, 184(sp)\n"
    "   li s0, 0x1001\n   li s1, 0x1002\n   li s2, 0x1003\n   li s3, 0x1004\n"
    "   li s4, 0x1005\n   li s5, 0x1006\n   li s6, 0x1007\n   li s7, 0x1008\n"
    "   li s8, 0x1009\n   li s9, 0x100a\n   li s10, 0x100b\n   li s11, 0x100c\n"
    "   li t0, 0x2001\n   fmv.d.x fs0, t0\n"
    "   li t0, 0x2002\n   fmv.d.x fs1, t0\n"
    "   li t0, 0x2003\n   fmv.d.x fs2, t0\n"
    "   li t0, 0x2004\n   fmv.d.x fs3, t0\n"
    "   li t0, 0x2005\n   fmv.d.x fs4, t0\n"
    "   li t0, 0x2006\n   fmv.d.x fs5, t0\n"
    "   li t0, 0x2007\n   fmv.d.x fs6, t0\n"
    "   li t0, 0x2008\n   fmv.d.x fs7, t0\n"
    "   li t0, 0x2009\n   fmv.d.x fs8, t0\n"
    "   li t0, 0x200a\n   fmv.d.x fs9, t0\n"
    "   li t0, 0x200b\n   fmv.d.x fs10, t0\n"
    "   li t0, 0x200c\n   fmv.d.x fs11, t0\n"
    "   lla a0, text_case_state\n"
    "   call setjmp\n"
    "   bnez a0, 2f\n"
    "   li a0, 9\n"
    "   call text_case_jump_level_one   # three frames, then back to here\n"
    "2: li a0, 0\n"
    "   li t0, 0x1001\n   xor t0, s0, t0\n    or a0, a0, t0\n"
    "   li t0, 0x1002\n   xor t0, s1, t0\n    or a0, a0, t0\n"
    "   li t0, 0x1003\n   xor t0, s2, t0\n    or a0, a0, t0\n"
    "   li t0, 0x1004\n   xor t0, s3, t0\n    or a0, a0, t0\n"
    "   li t0, 0x1005\n   xor t0, s4, t0\n    or a0, a0, t0\n"
    "   li t0, 0x1006\n   xor t0, s5, t0\n    or a0, a0, t0\n"
    "   li t0, 0x1007\n   xor t0, s6, t0\n    or a0, a0, t0\n"
    "   li t0, 0x1008\n   xor t0, s7, t0\n    or a0, a0, t0\n"
    "   li t0, 0x1009\n   xor t0, s8, t0\n    or a0, a0, t0\n"
    "   li t0, 0x100a\n   xor t0, s9, t0\n    or a0, a0, t0\n"
    "   li t0, 0x100b\n   xor t0, s10, t0\n   or a0, a0, t0\n"
    "   li t0, 0x100c\n   xor t0, s11, t0\n   or a0, a0, t0\n"
    "   fmv.x.d t0, fs0\n    li t1, 0x2001\n   xor t0, t0, t1\n   or a0, a0, t0\n"
    "   fmv.x.d t0, fs1\n    li t1, 0x2002\n   xor t0, t0, t1\n   or a0, a0, t0\n"
    "   fmv.x.d t0, fs2\n    li t1, 0x2003\n   xor t0, t0, t1\n   or a0, a0, t0\n"
    "   fmv.x.d t0, fs3\n    li t1, 0x2004\n   xor t0, t0, t1\n   or a0, a0, t0\n"
    "   fmv.x.d t0, fs4\n    li t1, 0x2005\n   xor t0, t0, t1\n   or a0, a0, t0\n"
    "   fmv.x.d t0, fs5\n    li t1, 0x2006\n   xor t0, t0, t1\n   or a0, a0, t0\n"
    "   fmv.x.d t0, fs6\n    li t1, 0x2007\n   xor t0, t0, t1\n   or a0, a0, t0\n"
    "   fmv.x.d t0, fs7\n    li t1, 0x2008\n   xor t0, t0, t1\n   or a0, a0, t0\n"
    "   fmv.x.d t0, fs8\n    li t1, 0x2009\n   xor t0, t0, t1\n   or a0, a0, t0\n"
    "   fmv.x.d t0, fs9\n    li t1, 0x200a\n   xor t0, t0, t1\n   or a0, a0, t0\n"
    "   fmv.x.d t0, fs10\n   li t1, 0x200b\n   xor t0, t0, t1\n   or a0, a0, t0\n"
    "   fmv.x.d t0, fs11\n   li t1, 0x200c\n   xor t0, t0, t1\n   or a0, a0, t0\n"
    "   ld ra, 216(sp)\n"
    "   ld s0, 0(sp)\n     ld s1, 8(sp)\n     ld s2, 16(sp)\n"
    "   ld s3, 24(sp)\n    ld s4, 32(sp)\n    ld s5, 40(sp)\n"
    "   ld s6, 48(sp)\n    ld s7, 56(sp)\n    ld s8, 64(sp)\n"
    "   ld s9, 72(sp)\n    ld s10, 80(sp)\n   ld s11, 88(sp)\n"
    "   fld fs0, 96(sp)\n  fld fs1, 104(sp)\n fld fs2, 112(sp)\n"
    "   fld fs3, 120(sp)\n fld fs4, 128(sp)\n fld fs5, 136(sp)\n"
    "   fld fs6, 144(sp)\n fld fs7, 152(sp)\n fld fs8, 160(sp)\n"
    "   fld fs9, 168(sp)\n fld fs10, 176(sp)\n fld fs11, 184(sp)\n"
    "   addi sp, sp, 224\n"
    "   ret\n");

#else
#error "the register probe needs a body for this architecture"
#endif

#endif // STANDARD_MODERN_C_TEST_STRINGS_CASES
