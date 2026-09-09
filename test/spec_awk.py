"""awk against GNU awk 5.4 on the machine.

One program, a whole language. The Utility below walks the option surface
(-F in every separator shape, -v assignments, -f program files, --) over
programs that read the shared inputs, so a separator meets every input shape
and a program every separator. The language itself is walked by seeded
generators: each awk_gen_* below builds argvs over a grammar -- expressions
at every precedence, every printf conversion with every flag, field and
record splitting, every builtin at its boundaries, control flow with
functions and arrays, getline in every form, redirection whose effects are
compared, the command line, regular expressions and token soups. Their
argvs are the Utility's extra list, deterministic and always run, because
the engine's FAMILIES are shell scripts and awk is not reached through a
shell here.

What ours refuses on purpose -- gawk's extensions -- is generated too, as
awk_refusals(), so the ledger says so case by case; gawk --posix agrees
with ours on every one of them (checked on the box).

Programs never iterate an array in print order without sorting first, never
print rand() values, never count ENVIRON and never take log or sqrt of a
negative: those are the four places where gawk's answer is not the
language's. Everything else is compared as it comes.
"""
import hashlib
import random

from differential import Utility, Option, INPUTS, FIXTURES


# ----------------------------------------------------------------------------
#       Inputs and the fixture.
# ----------------------------------------------------------------------------

def awk_lines(rng, count, longest, alphabet):
    return "\n".join("".join(rng.choice(alphabet) for _ in range(rng.randint(0, longest)))
                     for _ in range(count)) + "\n"


def awk_seeded(name):
    return random.Random(int.from_bytes(hashlib.sha256(b"awk:" + name.encode()).digest()[:8], "little"))


AWK_MIXED = awk_lines(awk_seeded("mixed"), 8, 9, "ab:, \tx1").encode()
AWK_MIXED_WORDS = awk_lines(awk_seeded("mixed-words"), 6, 7, "ab c1 ").encode()

INPUTS.update({
    "awk_csv": b"name,qty,price\napple,3,1.25\n,,\nbanana,,0.5\n\"quoted, field\",1,2\ncherry,10,\n",
    "awk_numbers": (b"1\n1.0\n1e2\n+3\n-0\n 7 \n0\n\nabc\n1x\n3.\n.5\n0x10\n1e\n  \n00\n1.0e1\n"
                    b"inf\nnan\n+inf\n-nan\n010\n1e+308\n1e-320\n-1.5e3\n+inf5\n-nan(1)\n5\r\n"),
    "awk_paragraphs": b"\n\nfirst para line one\nline two\n\n\n\nsecond para\n\nthird:a:b\nthird again\n\n",
    "awk_mixed": AWK_MIXED,
    "awk_words": AWK_MIXED_WORDS,
    "awk_grid": b"1 2 3\n4 5 6\n7 8 9\n",
    "awk_rec_65535": b"x" * 65535 + b"\n\na\n\n\nb\n",
    "awk_rec_65536": b"x" * 65536 + b"\n\na\n\n\nb\n",
    "awk_rec_65537": b"x" * 65537 + b"\n\na\n\n\nb\n",
    "awk_field_65536": b"a " + b"y" * 65536 + b" c\nd\n",
})


def awk_span_program(n):
    return b"#" + b" " * n + b"\nBEGIN { print 17 }#" + b" " * n


FIXTURES["awk"] = {
    **FIXTURES["basic"],
    "grid": INPUTS["awk_grid"],
    "letters": b"a\nb\nc\nd\ne\nf\ng\nh\n",
    "one": b"x\n",
    "five": b"5 x\n",
    "colons": b"one:two:three\nfour:five:six\nnodelim\nseven::nine\n:lead\ntrail:\n",
    "looks": b"10.0\n010\n1e2\n+3\n abc \n\n0x10\n3.\n",
    "words": b"+inf\n-inf\n+nan\n-nan\n+INF\n-NaN\n +inf \n+inf5\n+infinity\n-nan(1)\n+in\ninf\nnan\n",
    "spaced": b"  ab  cd ef  \n\tgh\tij\t\n\nplain\n",
    "paragraphs": INPUTS["awk_paragraphs"],
    "csv": INPUTS["awk_csv"],
    "mixed": AWK_MIXED,
    "crlf": b"5\r\n6\r\n",
    "wide": INPUTS["wide_words"],
    "many": INPUTS["many_lines"],
    "rec_65535": INPUTS["awk_rec_65535"],
    "rec_65536": INPUTS["awk_rec_65536"],
    "rec_65537": INPUTS["awk_rec_65537"],
    "field_65536": INPUTS["awk_field_65536"],
    "grow_131073": b"lead\n" + b" " * 131072 + b"x\ntail",
    "p_print.awk": b"{ print NR \": \" $0 }\n",
    "p_lib.awk": (b"function twice(n) { return n * 2 }\n"
                  b"function join(a, n, s,  i, r) { for (i = 1; i <= n; i++) r = r (i > 1 ? s : \"\") a[i]; return r }\n"),
    "p_main.awk": b"BEGIN { print twice(21) }\n{ n = split($0, parts); print join(parts, n, \"-\") }\n",
    "p_begin.awk": b"BEGIN { x = 1 }\n",
    "p_end.awk": b"BEGIN { print x + 1 }\nEND { print NR }\n",
    "p_bad.awk": b"BEGIN { print ( }\n",
    "p_span_4095.awk": awk_span_program(4095),
    "p_span_4096.awk": awk_span_program(4096),
    "p_span_65536.awk": awk_span_program(65536),
    "p_span_65537.awk": awk_span_program(65537),
}

AWK_DATA = ("grid", "letters", "one", "five", "colons", "looks", "words", "spaced", "paragraphs",
            "csv", "mixed", "crlf", "numbers", "fields", "a.txt", "b.txt", "empty", "nonl")
AWK_BIG = ("wide", "many", "rec_65535", "rec_65536", "rec_65537", "field_65536")
AWK_PROGRAM_FILES = ("p_print.awk", "p_lib.awk", "p_main.awk", "p_begin.awk", "p_end.awk", "p_bad.awk")


def awk_str(text):
    """An awk string literal for this text."""
    out = text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")
    return '"' + out + '"'


# ----------------------------------------------------------------------------
#       The option walk: programs that read the input, one per shape.
# ----------------------------------------------------------------------------

AWK_WALK_PROGRAMS = (
    "{ print }",
    "{ print NF }",
    "{ print $1, $NF }",
    "{ print NR, FNR, FILENAME }",
    "{ $1 = $1; print }",
    "{ NF = 2; print; print NF }",
    "{ $5 = \"X\"; print NF, $0 }",
    "{ print length($0), length($1), length() }",
    "{ for (i = NF; i > 0; i--) printf \"[%s]\", $i; print \"\" }",
    "{ n = split($0, p); print n, p[1], p[n] }",
    "{ print toupper($1) tolower($NF) }",
    "{ printf \"%s|%d|%.2f\\n\", $1, $2, $3 }",
    "{ a[$1]++ } END { n = 0; for (k in a) n += a[k]; print n, length(a) }",
    "{ s += $1; c++ } END { print s + 0, c + 0, NR }",
    "{ print $1 + 0, ($1 == $1 + 0), ($1 < 10), ($1 == \"10\") }",
    "$1 > 5",
    "$2 ~ /e/ { print $2 }",
    "/a/,/c/",
    "NR % 2 == 0",
    "!NF { print \"blank\", NR }",
    "NR == 2 { print; exit 3 }",
    "END { print NR, NF, \"[\" $0 \"]\" }",
    "BEGIN { print \"begin\" } END { print \"end\" }",
    "{ sub(/a/, \"A\"); print }",
    "{ n = gsub(/[0-9]/, \"#\"); print n, $0 }",
    "{ if (match($0, /[a-z]+/)) print RSTART, RLENGTH; else print \"no\" }",
    "{ print substr($0, 2, 3) \"|\" substr($0, length($0)) }",
    "{ print index($0, \" \"), index($0, \"a\") }",
    "{ getline line; print NR, \"[\" line \"]\", $0 }",
    "NR == 1 { while ((getline l) > 0) n++; print n + 0, NR }",
    "{ print > \"out\" } END { close(\"out\"); while ((getline l < \"out\") > 0) c++; print c + 0 }",
    "{ print $1 | \"sort\" } END { close(\"sort\"); print \"done\" }",
    "{ $0 = $0 \"!\"; print NF, $0 }",
    "{ $2 = \"\"; print; print NF }",
    "{ x = $0; gsub(/ +/, \"_\", x); print x }",
    "{ print ($0 < \"m\"), ($0 == \"\"), (length($0) > 3) }",
    "{ printf \"%c%c\", $1, 65; print \"\" }",
    "{ print; next; print \"never\" }",
    "FNR == 1 { nextfile } { print \"no\" } END { print NR }",
    "function f(s) { return length(s) * 2 } { print f($0) }",
    "{ print (NF ? $NF : \"empty\") }",
    "length > 5",
    "length($0) == 0 { c++ } END { print c + 0 }",
    "{ OFS = \"-\"; $1 = $1; print }",
    "BEGIN { ORS = \"|\" } { print $1 } END { printf \"\\n\" }",
    "{ print NF; NF = 0; print \"[\" $0 \"]\" }",
    "{ $3 = \"z\"; $0 = $0 \"!\"; print; print NF }",
    "BEGIN { RS = \"\" } { print NR \"[\" $0 \"]\", NF }",
    "BEGIN { RS = \";\" } { print NR \"[\" $0 \"]\" }",
    "BEGIN { RS = \"[0-9]+\" } { print NR \"[\" $0 \"]\" }",
    "BEGIN { FS = \":\" } { print NF, $2 }",
    "{ FS = \":\" } { print $1 }",
    "1",
    "0",
    "",
    "# nothing",
)

AWK_WALK_OPERANDS = tuple((program,) for program in AWK_WALK_PROGRAMS) + (
    ("{ print FILENAME, FNR, NR, $0 }", "grid"),
    ("{ print FILENAME, FNR, NR }", "letters", "colons"),
    ("{ print v, $0 }", "v=1", "one", "v=2", "one"),
    ("{ print FILENAME \":\" $0 }", "-"),
    ("{ print }", "missing"),
    ("{ print FILENAME }", "empty", "grid"),
    ("{ print NR, length($0), NF }", "rec_65536"),
    ("{ print NF, length($2) }", "field_65536"),
    # Data only: for the -f forms, whose program comes from the file.
    ("grid",),
    ("colons", "letters"),
    ("x=2", "grid"),
    (),
)

AWK_WALK_PROGRAM_SET = frozenset(AWK_WALK_PROGRAMS) | {
    operands[0] for operands in AWK_WALK_OPERANDS if operands and operands[0].startswith("{")}


def awk_valid(argv):
    """A -f case has its program in the file, so a walk program beside it
    would only be a file that is not there. Nothing else is pruned: both
    tools erroring is behaviour, and the extra list is never pruned."""
    from_file = any(word == "-f" or (word.startswith("-f") and not word.startswith("--"))
                    for word in argv)
    return not (from_file and any(word in AWK_WALK_PROGRAM_SET for word in argv))


# ----------------------------------------------------------------------------
#       Generated programs.
# ----------------------------------------------------------------------------

AWK_NUMBERS = ("0", "1", "-1", "2", "0.5", "-0.5", "1/3", "2/3", "1e6", "1e-6", "1e16", "1e17",
               "2^53", "2^53+1", "2^62", "1e300", "1e-300", "123456.789", "0.1+0.2", "100000",
               "1000000", "3.0", "-0.0", "1e100", "9.995", "0.0001", "0.00001",
               "12345678901234567890", "1.005", "255", "65536", "1e300*1e300", "-1e300*1e300")
AWK_STRINGS = ('"abc"', '"10"', '"3x"', '""', '" 12 "', '".5"', '"1e2"', '"0x1A"', '"+inf"',
               '"-nan"', '"010"', '"1e"', '"a b"', '"-3"')
AWK_SORT = ("function asrt(a,  n, k, t, i, j) { n = 0; for (k in a) { n++; t[n] = k } "
            "for (i = 2; i <= n; i++) for (j = i; j > 1 && t[j-1] > t[j]; j--) "
            "{ k = t[j]; t[j] = t[j-1]; t[j-1] = k } "
            "for (i = 1; i <= n; i++) print t[i] \"=\" a[t[i]]; return n } ")
AWK_REGEXES = ("a", "ab", "a*", "a+", "a?", "[ab]", "[^a]", ".", "^a", "b$", "a|b", "(ab)+",
               "a{2}", "a{1,2}", "a{2,}", "[a-c]+", "(a|bc)*", "x?b", ".*a", "^$", "^ab$",
               "[[:digit:]]", "[[:alpha:]][[:digit:]]", "[[:space:]]+", "[[:upper:]]", "\\.",
               "a\\.b", "[.]", "\\t", "\\101", "\\x41", "[]a]", "[a-]", "\\$", "\\^", "(|a)", "()",
               "\\(", "a\\*", "[^ ]+", "[0-9]+\\.[0-9]*", "^[ \\t]*$", "(^| )a", "b( |$)",
               "\\\\", "[\\\\]", "a\\|b", "[[:alnum:]_]+", ":+", "[,:]")


def awk_gen_expressions(rng, count):
    """Expressions over every operator, printed in parentheses so no > is a
    redirection; two per program, over a record and a few variables."""
    variables = ("x", "y", "u", "i", "$1", "$2", "$3", "$0", "NF", "NR")

    def atom():
        r = rng.random()
        if r < 0.28:
            return rng.choice(AWK_NUMBERS)
        if r < 0.42:
            return rng.choice(AWK_STRINGS)
        if r < 0.58:
            return rng.choice(variables)
        if r < 0.70:
            inner = expr(0)
            return rng.choice((f"int({inner})", f"length({inner})", f"substr({inner}, 2)",
                               f"sqrt(({inner}) * ({inner}))", "exp(0)", f"log(1 + ({inner}) * ({inner}))",
                               f"sin({inner})", f"cos({inner})", f"atan2({inner}, 1)",
                               f"toupper({inner})", f"index({inner}, \"a\")", "(\"k\" in a)",
                               f"(({inner}) in a)", f"tolower({inner})"))
        if r < 0.80:
            return rng.choice(("x++", "++x", "y--", "--i", "i++", "$2++", "++$1"))
        if r < 0.90:
            return rng.choice(("-", "+", "!", "- -", "!!", "-!")) + atom()
        target = rng.choice(("x", "y", "i", "$2", "a[\"k\"]", "a[i]"))
        return f"({target} {rng.choice(('=', '+=', '-=', '*=', '/=', '%=', '^=', '**='))} {expr(0)})"

    def expr(depth):
        if depth <= 0:
            return atom()
        r = rng.random()
        left, right = expr(depth - 1), expr(depth - 1)
        if r < 0.40:
            return f"{left} {rng.choice(('+', '-', '*', '/', '%', '^', '**'))} {right}"
        if r < 0.55:
            return f"({left} {rng.choice(('<', '<=', '==', '!=', '>', '>='))} {right})"
        if r < 0.65:
            return rng.choice((f"{left} {right}", f"{left} \" \" {right}", f"({left}) ({right})"))
        if r < 0.75:
            return f"{left} {rng.choice(('&&', '||'))} {right}"
        if r < 0.82:
            return f"({left} ? {right} : {expr(depth - 1)})"
        if r < 0.92:
            pattern = rng.choice(AWK_REGEXES[:24])
            form = f"/{pattern}/" if rng.random() < 0.6 else awk_str(pattern)
            return f"({left} {rng.choice(('~', '!~'))} {form})"
        return f"({left})"

    out = []
    for _ in range(count):
        depth = rng.choice((1, 1, 2, 2, 3))
        first, second, third = expr(depth), expr(depth), expr(1)
        preamble = "$0 = \"10 abc 2.5\"; x = \"010\"; y = 3; i = 1; a[\"k\"] = 1; "
        if rng.random() < 0.7:
            body = f"print ({first}); print ({second}), ({third})"
        else:
            body = f"printf \"%s|%d|%.3g\\n\", ({first}), ({second}), ({third})"
        argv = ["BEGIN { " + preamble + body + " }"]
        if rng.random() < 0.3:
            argv = ["-v", "u=" + rng.choice(("010", "abc", "1e2", "", " 3 ", "0x10"))] + argv
        out.append(tuple(argv))
    return out


def awk_gen_printf(rng, count):
    """Every conversion with every flag, width and precision, star forms,
    %%, unknown and incomplete directives; then CONVFMT and OFMT."""
    conversions = "diouxXeEfFgGcs"
    flags = ("", "-", "+", " ", "0", "#", "-0", "+0", " 0", "#0", "-+", "- ", "#-", "+ 0")
    widths = ("", "1", "5", "12", "*")
    precisions = ("", ".0", ".1", ".3", ".10", ".*")
    values = AWK_NUMBERS + AWK_STRINGS + ("65", "300", "-1", "1e19", "-1e19", "2^63", "2^64")
    out = []
    for _ in range(count):
        pieces, args = [], []
        for _ in range(rng.choice((1, 2, 2, 3))):
            width, precision = rng.choice(widths), rng.choice(precisions)
            conversion = rng.choice(conversions)
            pieces.append(f"[%{rng.choice(flags)}{width}{precision}{conversion}]")
            if width == "*":
                args.append(rng.choice(("0", "3", "8", "-6", "2.7")))
            if precision == ".*":
                args.append(rng.choice(("0", "1", "4", "-1", "12")))
            args.append(rng.choice(values))
        r = rng.random()
        if r < 0.08:
            pieces.append(rng.choice(("%%", "[%5%]", "[%-3%]", "%y", "[%5b]", "%")))
        if r > 0.92 and args:
            args.pop()
        if r > 0.85:
            args.append("\"extra\"")
        fmt = "".join(pieces) + ("\\n" if not pieces[-1].endswith("%") else "")
        call = f"printf \"{fmt}\"" + ("".join(", " + a for a in args))
        if rng.random() < 0.25:
            call = f"s = sprintf(\"{fmt}\"" + "".join(", " + a for a in args) + "); print length(s); print s"
        out.append(("BEGIN { " + call + " }",))
    # Floating formats only: gawk warns about any other CONVFMT or OFMT, and
    # a CONVFMT of %s makes it crash (both are pinned once, from awk_refusals).
    formats = ("%.2g", "%.2f", "%.10g", "%e", "%.0f", "%5.1f", "%.30g", "%g", "%G", "%#.3e", "%+.1f")
    for _ in range(count // 4):
        conv, ofmt = rng.choice(formats), rng.choice(formats)
        value = rng.choice(AWK_NUMBERS)
        out.append(("BEGIN { CONVFMT = \"%s\"; OFMT = \"%s\"; x = %s; print x; print x \"\"; "
                    "a[x] = 1; for (k in a) print k; print (x \"\" == x) }" % (conv, ofmt, value),))
    return out


def awk_gen_fields(rng, count):
    """Field and record splitting: every separator shape by option, by -v
    and by assignment, OFS and ORS, over the data files."""
    # A backslash meant for the pattern is doubled: -F and -v values go
    # through string escapes first, and gawk warns about an unknown one.
    separators = (":", ",", "\\t", " ", "t", "[0-9]+", "[,:]", ".", "|", "ab", "a+", " *", "]",
                  "\\\\.", "x", "", "[ ]", "  ", "\\\\|", "^", "[[:space:]]+", "e")
    programs = (
        "{print NF}", "{print $1}", "{print $2 \"|\" $NF}", "{print NR, NF, $0}", "{$1=$1; print}",
        "{NF=2; print; print NF}", "{print length($0), length($1)}", "{$3=\"X\"; print NF, $0}",
        "{for(i=1;i<=NF;i++) printf \"[%s]\", $i; print \"\"}", "{print $(NF > 1 ? NF - 1 : 1)}",
        "{i = 2; print $(i + 1)}", "{x = $9; print NF}", "{NF = NF + 2; print}",
        "{$(NF + 2) = \"e\"; print NF, $0}", "{print $1 + 0, ($1 == $1 + 0), ($1 < 10)}",
        "{n = split($0, p, FS); print n, p[1], p[n]}", "{$0 = $2 \" \" $1; print NF, $0}",
        "{sub(/[a-z]/, \"A\", $1); print}", "{print (NF > 2) ? \"big\" : \"small\"}",
        "{$1 = \"\"; print; print NF}", "{print $NF; $NF = \"\"; print NF, $0}",
        "END {print NR, NF, $0}", "{$2 = 7; print; print ($2 > 1), ($2 < 100)}",
        "{a[$1] = $2} END {" + AWK_SORT.split("{", 1)[1].rsplit("return", 1)[0] + "}",
        "{print}", "$1 == \"a\" || $2 == \"b\"", "NF > 1 && $1 !~ /^[0-9]/ {print $2}",
    )
    out = []
    for _ in range(count):
        argv = []
        program = rng.choice(programs)
        shape = rng.random()
        separator = rng.choice(separators)
        if shape < 0.35:
            argv += ["-F", separator] if rng.random() < 0.6 else ["-F" + separator] if separator else ["-F", ""]
        elif shape < 0.55:
            argv += ["-v", "FS=" + separator]
        elif shape < 0.70:
            program = "BEGIN { FS = " + awk_str(separator) + " } " + program
        if rng.random() < 0.3:
            record = rng.choice(("", ";", "a", "\\n", "[0-9]+", "x+", "ab", "\n\n", "\\n\\n"))
            argv += ["-v", "RS=" + record]
        if rng.random() < 0.3:
            ofs = rng.choice(("-", "", ":", "||", "\\t"))
            argv += ["-v", "OFS=" + ofs]
        if rng.random() < 0.2:
            argv += ["-v", "ORS=" + rng.choice(("|", "", "\\n\\n", ";"))]
        argv.append(program)
        files = rng.choice(((rng.choice(AWK_DATA),), (rng.choice(AWK_DATA), rng.choice(AWK_DATA)),
                            ("x=1", rng.choice(AWK_DATA)), (rng.choice(AWK_DATA), "FS=:", "colons"),
                            (rng.choice(AWK_BIG),), ("mixed",), ("csv",), ("spaced",)))
        argv += list(files)
        out.append(tuple(argv))
    return out


def awk_gen_strings(rng, count):
    """The string builtins at their boundaries: substr, index, length,
    split, sub, gsub, match, tolower, toupper, sprintf, comparison."""
    texts = ("abc", "aaa", "banana", "", "a b  c", "x:y::z", "AbC dEf", "12abc", "caf\u00e9", "a.b.c",
             "hello", "abababc", " lead", "trail ", "1e3", "a\\b", "a\"b", "tab\there")
    starts = ("-2", "-1", "0", "1", "2", "3", "5", "6", "9", "1.5", "2.7", "0.5", "-0.5", "1e10",
              "-1e10", "1e300*1e300", "\"2\"", "\"x\"")
    lengths = ("-1", "0", "1", "2", "3", "5", "99", "0.4", "1.5", "2.5", "1e10", "-1e300*1e300")
    replacements = ("[&]", "\\\\&", "\\\\\\\\&", "x\\\\y", "", "&&", "\\\\\\\\\\\\\\\\&", "<&>", "\\\\q", "x\\\\")
    patterns = ("/a/", "/^a/", "/a*/", "/x*/", "/$/", "//", "\"a\"", "\"[bc]\"", "/b|c/", "/[ ]+/",
                "/\\./", "\"\\\\.\"", "/(a)(b)/", "/./", "/^/", "\"^\"")
    out = []
    for _ in range(count):
        t = awk_str(rng.choice(texts))
        calls = []
        for _ in range(rng.choice((2, 3, 3, 4))):
            kind = rng.random()
            if kind < 0.18:
                if rng.random() < 0.5:
                    calls.append(f"print \"[\" substr({t}, {rng.choice(starts)}) \"]\"")
                else:
                    calls.append(f"print \"[\" substr({t}, {rng.choice(starts)}, {rng.choice(lengths)}) \"]\"")
            elif kind < 0.28:
                needle = rng.choice(("\"\"", "\"a\"", "\"ab\"", "\"z\"", t, "\"b\"", "\" \"", "\".\""))
                calls.append(f"print index({t}, {needle})")
            elif kind < 0.36:
                calls.append(rng.choice((f"print length({t})", f"$0 = {t}; print length", f"$0 = {t}; print length()",
                                         f"x = {t}; print length(x)", "a[1]; a[2]; print length(a)",
                                         f"print length({t} {t})", f"print length(12345), length(1e6), length(0.1)")))
            elif kind < 0.50:
                sep = rng.choice(("", ", \" \"", ", \":\"", ", \"\"", ", /[,:]/", ", \"a+\"", ", \".\"", ", \"\\\\.\"",
                                  ", / /", ", /a/", ", \"b\"", ", \" +\""))
                calls.append(f"n = split({t}, p{sep}); s = n; for (i = 1; i <= n; i++) s = s \"[\" p[i] \"]\"; print s")
            elif kind < 0.68:
                fn = rng.choice(("sub", "gsub"))
                where = rng.random()
                pattern, repl = rng.choice(patterns), awk_str(rng.choice(replacements)) if rng.random() < 0.8 else "\"" + rng.choice(replacements) + "\""
                if where < 0.4:
                    calls.append(f"s = {t}; n = {fn}({pattern}, {repl}, s); print n, s")
                elif where < 0.6:
                    calls.append(f"$0 = {t}; n = {fn}({pattern}, {repl}); print n, $0, NF")
                elif where < 0.8:
                    calls.append(f"$0 = {t}; n = {fn}({pattern}, {repl}, $1); print n, $0, NF")
                else:
                    calls.append(f"e[1] = {t}; n = {fn}({pattern}, {repl}, e[1]); print n, e[1]")
            elif kind < 0.80:
                calls.append(f"print match({t}, {rng.choice(patterns)}), RSTART, RLENGTH")
            elif kind < 0.88:
                calls.append(f"print toupper({t}) \"|\" tolower({t}) \"|\" toupper(12) tolower(1e3)")
            elif kind < 0.94:
                other = awk_str(rng.choice(texts))
                calls.append(f"print ({t} < {other}), ({t} == {other}), ({t} > {other}), {t} {other}, length({t} {other})")
            else:
                calls.append(f"print sprintf(\"%s-%d-%5.2f-%c\", {t}, {rng.choice(starts)}, {rng.choice(lengths)}, {t})")
        out.append(("BEGIN { " + "; ".join(calls) + " }",))
    return out


def awk_gen_control(rng, count):
    """Control flow, arrays and functions, assembled from statement
    templates with small random constants; arrays are printed sorted."""
    templates = (
        "for (i = 0; i < N; i++) { if (i % 2) continue; if (i > M) break; s = s i } print s",
        "i = 0; while (i < N) { i++; if (i == M) break } print i",
        "i = 0; do { i++ } while (i < N); print i",
        "do print \"once\"; while (0)",
        "i = 0; while (i++ < N) ; print i",
        "for (;;) { if (++i >= N) break } print i",
        "if (N > M) print \"gt\"; else if (N == M) print \"eq\"; else print \"lt\"",
        "if (N) if (M) print \"both\"; else print \"first\"",
        "x = N; x += M; x *= 2; x /= 3; x %= 5; x ^= 2; print x",
        "for (i = 1; i <= N; i++) a[i] = i * M; delete a[2]; print length(a), (2 in a), (3 in a); asrt(a)",
        "for (i = 1; i <= N; i++) a[i] = i; delete a; print length(a)",
        "for (i = 1; i <= N; i++) a[i] = i; for (k in a) if (k % 2) delete a[k]; asrt(a)",
        "a[N, M] = 1; a[M, N] = 2; for (k in a) { split(k, p, SUBSEP); c += p[1] + p[2] } print c, ((N, M) in a), ((N, N) in a)",
        "SUBSEP = \":\"; a[N, M] = 1; asrt(a)",
        "a[\"x\"] = N; a[\"y\"] = M; a[\"10\"] = 1; a[\"9\"] = 2; a[1e2] = 3; a[0.5 + 0.5] = 4; asrt(a)",
        "print fact(N), fib(M), gcd(N * 6, M * 4), ack(2, M % 4)",
        "print depth(N * 50)",
        "fill(arr, M); print length(arr), arr[1], arr[M]; print total(arr)",
        "print f_local(N), f_local(M), \"[\" loc \"]\"",
        "swap(N, M); print N, M",
        "x = N; print (x == N), (x < M), (x \"\" == N \"\"), (u == 0), (u == \"\"), length(u)",
        "n = split(\"c b a\", w); for (i = 1; i <= n; i++) o[w[i]] = i; asrt(o)",
        "s = \"\"; for (i = N; i >= 1; i--) s = s i; print s, length(s)",
        "i = 0; while (1) { if (++i > N) break; if (i % M == 0) continue; s = s \".\" } print s",
        "print (N ? \"t\" : \"f\"), (M ? \"t\" : \"f\"), !N, !M, -N, +M",
        "x = N; y = x++ + ++x; print x, y; z = x-- - --x; print x, z",
        "CONVFMT = \"%.2g\"; a[N / 3] = 1; a[M / 7] = 2; asrt(a)",
        "exit N % 4",
        "print \"before\"; exit; print \"never\"",
        "if (N > M) exit 2; print \"kept\"",
    )
    functions = (
        "function fact(n) { return n <= 1 ? 1 : n * fact(n - 1) } "
        "function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2) } "
        "function gcd(a, b) { return b ? gcd(b, a % b) : a } "
        "function ack(m, n) { if (m == 0) return n + 1; if (n == 0) return ack(m - 1, 1); return ack(m - 1, ack(m, n - 1)) } "
        "function depth(n) { return n == 0 ? 0 : 1 + depth(n - 1) } "
        "function fill(a, n,  i) { for (i = 1; i <= n; i++) a[i] = i * i } "
        "function total(a,  k, s) { for (k in a) s += a[k]; return s } "
        "function f_local(n,  loc, t) { loc = n * 2; t[loc] = 1; return loc + length(t) } "
        "function swap(a, b,  t) { t = a; a = b; b = t } ")
    out = []
    for _ in range(count):
        n, m = rng.randint(0, 9), rng.randint(1, 7)
        body = "; ".join(rng.choice(templates).replace("N", str(n)).replace("M", str(m))
                         for _ in range(rng.choice((1, 2, 2, 3))))
        program = functions + AWK_SORT + "BEGIN { " + body + " }"
        if rng.random() < 0.3:
            rule = rng.choice(("NR % 2 { next } { print NR, $0 }", "{ if (NR == 2) next; print }",
                               "NR == 2 { exit 5 } { print } END { print \"end\", NR }",
                               "FNR == 1 { nextfile } END { print NR }",
                               "{ a[NR] = $0 } END { for (i = NR; i >= 1; i--) print a[i] }",
                               "{ n[$1]++ } END { asrt(n) }",
                               "{ f(); print \"after\" } function f() { if (NR == 1) next }"))
            program = functions + AWK_SORT + rule
            out.append((program, rng.choice(("letters", "grid", "words", "spaced")), rng.choice(("grid", "one"))))
        else:
            out.append((program,))
    return out


def awk_gen_getline(rng, count):
    """getline in every form, in every kind of rule, with NR, FNR, NF and
    $0 watched; commands and files closed and read again."""
    forms = (
        ("NR == 1 { r = getline; print r, $0, NR, NF }", "letters"),
        ("NR == 1 { r = getline x; print r, x, $0, NR }", "letters"),
        ("NR == 1 { getline $2; print $0, NF }", "grid"),
        ("{ print (getline), NR }", "one"),
        ("BEGIN { while ((getline l < \"letters\") > 0) n++; print n, NR }",),
        ("BEGIN { getline < \"grid\"; print $0, NF, NR }",),
        ("BEGIN { print (getline x < \"missing\"), (getline x < \"dir\") }",),
        ("BEGIN { f = \"one\"; getline a < f; close(f); getline b < f; print a, b, close(f) }",),
        ("BEGIN { \"echo hi\" | getline x; print x, NR }",),
        ("BEGIN { \"echo a b\" | getline; print $0, NF, NR }",),
        ("BEGIN { while ((\"printf '1\\\\n2\\\\n3\\\\n'\" | getline l) > 0) n++; print n }",),
        ("BEGIN { cmd = \"echo x\"; cmd | getline; print close(cmd); cmd | getline y; print y }",),
        ("BEGIN { c = \"exit 4\"; c | getline x; print close(c) }",),
        ("BEGIN { c = \"cat grid\"; while ((c | getline) > 0) s += $1; close(c); print s, NR, NF }",),
        ("NR == 1 { while ((getline line) > 0) n++; print n, NR, $0 }", "letters"),
        ("END { print (getline), (getline x), NR }", "grid"),
        ("BEGIN { print (getline), $0, NR }", "letters"),
        ("BEGIN { print (getline), $0, NR } END { print NR }", "letters", "grid"),
        ("{ getline; print NR \":\" $0 }", "letters"),
        ("{ getline x < \"grid\"; print NR, FNR, x }", "letters"),
        ("BEGIN { getline x < \"-\"; print \"[\" x \"]\" }",),
        ("BEGIN { getline x < \"/dev/stdin\"; print \"[\" x \"]\" }",),
        ("BEGIN { \"echo 010\" | getline v; print (v == 10), (v == \"010\"), (v < 9) }",),
        ("NR == 1 { getline x; print (x == 10), (x == \"010\") }", "looks"),
        ("BEGIN { RS = \"\" } NR == 1 { getline; print NR \"[\" $0 \"]\" NF }", "paragraphs"),
        ("BEGIN { while ((getline l < \"rec_65536\") > 0) n += length(l); print n }",),
        ("BEGIN { RS = \";\" } { getline x < \"colons\"; print NR, x }", "colons"),
        ("{ while ((getline l < FILENAME) > 0) c++ } END { print c, NR }", "grid"),
        ("BEGIN { \"seq 3\" | getline a; \"seq 3\" | getline b; print a, b; close(\"seq 3\"); \"seq 3\" | getline c; print c }",),
        ("BEGIN { print \"w\" > \"t\"; close(\"t\"); getline z < \"t\"; print z; print \"v\" > \"t\"; close(\"t\"); print (getline z < \"t\"), z }",),
        ("function rd(f,  l) { getline l < f; return l } BEGIN { print rd(\"one\"), rd(\"grid\") }",),
        ("BEGIN { x = getline y < \"one\" > 0; print x }",),
        ("BEGIN { while (getline line < \"grid\" > 0) n++; print n }",),
        ("{ \"echo \" $1 | getline r; close(\"echo \" $1); print r }", "letters"),
    )
    out = list(forms)
    for _ in range(count - len(forms)):
        kind = rng.choice(("", "x", "< \"grid\"", "x < \"letters\"", "\"echo q\" |", "\"cat one\" |", "y < \"missing\""))
        where = rng.choice(("BEGIN", "NR == 1", "NR == 2", "END", "$0 ~ /a/"))
        watch = rng.choice(("NR, NF, $0", "NR, x, y", "FNR, FILENAME", "$1, $NF, NF"))
        if kind.endswith("|"):
            stmt = f"r = ({kind} getline{rng.choice(('', ' v'))})"
        else:
            stmt = f"r = (getline {kind})"
        program = f"{where} {{ {stmt}; print r, {watch} }}"
        if rng.random() < 0.5:
            program += " END { print \"end\", NR }"
        out.append((program, rng.choice(("letters", "grid", "one", "colons"))))
    return out


def awk_gen_redirect(rng, count):
    """print and printf to files, appends and commands; the directory is
    compared afterwards, and close() and system() answer with statuses."""
    fixed = (
        ("BEGIN { print \"a\" > \"out\"; print \"b\" > \"out\"; close(\"out\") }",),
        ("BEGIN { print \"a\" > \"out\"; close(\"out\"); print \"b\" >> \"out\"; close(\"out\") }",),
        ("BEGIN { printf \"%s|\", \"z\" > \"out\" }",),
        ("BEGIN { print \"a\" > \"one_out\"; print \"b\" > \"two_out\" }",),
        ("{ print $1 > \"col\" }", "grid"),
        ("{ print $1 > $2 }", "grid"),
        ("{ print > (\"by_\" NR) }", "letters"),
        ("BEGIN { print \"b\\na\" | \"sort\"; close(\"sort\"); print \"done\" }",),
        ("BEGIN { printf \"%s\\n%s\\n\", \"b\", \"a\" | \"sort\" }",),
        ("BEGIN { print \"x\"; print \"b\\na\" | \"sort\"; print \"z\" }",),
        ("BEGIN { print \"1\" | \"cat\"; print \"2\" | \"cat 1>&2\"; print \"3\" }",),
        ("BEGIN { print \"q\" | \"cat > piped\"; close(\"cat > piped\"); getline z < \"piped\"; print z }",),
        ("BEGIN { print \"up\" | \"tr a-z A-Z\"; r = close(\"tr a-z A-Z\"); print r }",),
        ("BEGIN { print \"x\" | \"exit 7\"; print close(\"exit 7\") }",),
        ("BEGIN { print \"x\" | \"cat\"; print close(\"cat\"), close(\"cat\"), close(\"never\") }",),
        ("BEGIN { print (1 > 2), (1 < 2) }",),
        ("BEGIN { print 1, 2 > \"/dev/stdout\" }",),
        ("BEGIN { print \"e\" > \"/dev/stderr\"; print \"o\" }",),
        ("BEGIN { print \"x\" > \"dir\" }",),
        ("BEGIN { print \"x\" > \"dir/made\"; close(\"dir/made\"); getline y < \"dir/made\"; print y }",),
        ("BEGIN { print \"a\"; r = system(\"echo b\"); print \"c\" r }",),
        ("BEGIN { print system(\"exit 3\"), system(\"exit 255\"), system(\"true\") }",),
        ("BEGIN { system(\"echo hi > sysout\"); getline z < \"sysout\"; print z }",),
        ("BEGIN { printf \"a\"; system(\"printf b\"); print \"c\" }",),
        ("BEGIN { printf \"a\" | \"cat\"; system(\"\"); print \"b\" }",),
        ("BEGIN { print fflush(), fflush(\"/dev/stdout\"), fflush(\"nosuch\") }",),
        ("BEGIN { print \"x\" > \"ff\"; print fflush(\"ff\"); close(\"ff\"); print fflush(\"ff\") }",),
        ("BEGIN { for (i = 0; i < 40; i++) print i > (\"f\" i) }",),
        ("BEGIN { for (i = 0; i < 40; i++) print i | (\"cat > p\" i) }",),
        ("BEGIN { for (i = 0; i < 20000; i++) print i | \"wc -l\"; close(\"wc -l\") }",),
        ("BEGIN { print \"a\" > \"s\"; close(\"s\"); getline l < \"s\"; print l > \"s\"; close(\"s\"); getline m < \"s\"; print m }",),
        ("BEGIN { ORS = \"X\"; printf \"a\\n\"; print \"b\" > \"o\" }",),
        ("BEGIN { print \"x\" > \"unreadable\" }",),
        ("BEGIN { print \"x\" >> \"a.txt\"; close(\"a.txt\"); while ((getline l < \"a.txt\") > 0) n++; print n }",),
        ("BEGIN { OFS = \"-\"; print 1, 2, 3 > \"o\"; printf(\"%d %d\\n\", 4, 5) >> \"o\"; close(\"o\"); getline l < \"o\"; print l }",),
        ("END { print NR > \"count\" }", "grid", "letters"),
        ("BEGIN { print \"x\" | \"\" }",),
        ("BEGIN { print \"x\" > \"\" }",),
        ("BEGIN { print \"x\" > u }",),
    )
    out = list(fixed)
    commands = ("cat", "sort", "sort -r", "tr a-z A-Z", "wc -l", "cat > piped", "head -1", "exit 2")
    for _ in range(count - len(fixed)):
        target = rng.choice(("> \"out\"", ">> \"out\"", "| \"%s\"" % rng.choice(commands), "> (\"o\" NR)", "> \"/dev/stdout\""))
        what = rng.choice(("print $1", "print", "printf \"%s:%s\\n\", NR, $NF", "print NF, $0", "print toupper($0)"))
        closer = rng.choice(("", " END { close(%s) }" % target.split(None, 1)[1] if "NR" not in target else "",
                             " END { print \"end\" }"))
        out.append((f"{{ {what} {target} }}{closer}", rng.choice(("grid", "letters", "colons", "mixed"))))
    return out


def awk_gen_cmdline(rng, count):
    """-v and operand assignments, ARGV and ARGC, -f files, --, FILENAME
    across files, missing and directory operands."""
    fixed = (
        ("-v", "x=5", "BEGIN { print x, x + 1 }"),
        ("-v", "x=a\\tb", "BEGIN { print length(x) }"),
        ("-v", "x=010", "BEGIN { print (x == 10), (x == \"010\") }"),
        ("-v", "x=1", "-v", "y=2", "BEGIN { print x + y }"),
        ("-vx=5", "BEGIN { print x }"),
        ("-v", "x", "BEGIN { print \"ok\" }"),
        ("-v", "1x=2", "BEGIN { print \"ok\" }"),
        ("-v", "=2", "BEGIN { print \"ok\" }"),
        ("-v", "x=y=z", "BEGIN { print x }"),
        ("-v", "FS=:", "{ print $2 }", "colons"),
        ("-v", "OFS=-", "{ $1 = $1; print }", "grid"),
        ("-v", "RS=:", "{ print NR, $0 }", "colons"),
        ("-v", "NF=3", "BEGIN { print NF, \"[\" $0 \"]\" }"),
        ("-v", "NR=10", "{ print NR }", "grid"),
        ("-v", "x=\\1412", "BEGIN { print x }"),
        ("{ print $2 }", "FS=:", "colons"),
        ("{ print v, FILENAME }", "v=1", "one", "v=2", "one"),
        ("BEGIN { print \"[\" v \"]\" } { print v }", "v=9", "one"),
        ("END { print \"[\" v \"]\" }", "v=9"),
        ("{ n++ } END { print n + 0 }", "1x=2", "one"),
        ("BEGIN { print ARGC, ARGV[0], ARGV[1], ARGV[2] }", "one", "two"),
        ("BEGIN { ARGV[1] = \"grid\"; ARGC = 2 } { print FILENAME, $0 }", "missing"),
        ("BEGIN { delete ARGV[1] } { n++ } END { print n + 0 }", "missing", "grid"),
        ("BEGIN { ARGV[1] = \"\" } { n++ } END { print n + 0 }", "missing", "grid"),
        ("BEGIN { ARGC = 1 } { n++ } END { print n + 0 }", "missing"),
        ("BEGIN { ARGV[ARGC++] = \"grid\" } { n++ } END { print n + 0, ARGC }"),
        ("BEGIN { ARGV[2] = \"x=7\"; ARGC = 3 } { print x, $0 }", "one"),
        ("BEGIN { print (\"PATH\" in ENVIRON), (\"NOSUCH_VAR\" in ENVIRON), ENVIRON[\"LC_ALL\"], (length(ENVIRON[\"HOME\"]) > 0) }",),
        ("BEGIN { for (k in ENVIRON) n++; print (n > 3) }",),
        ("--", "{ print $0 }", "one"),
        ("--", "BEGIN { print \"dd\" }"),
        ("{ print FILENAME, $0 }", "-"),
        ("{ print FILENAME, FNR, NR }", "one", "grid"),
        ("{ print FILENAME, FNR, NR }", "grid", "missing", "letters"),
        ("{ print FILENAME \":\" $0 }", "dir", "one"),
        ("{ print \"read\" } END { print NR }", "dir"),
        ("{ print \"[\" FILENAME \"]\" } END { print \"[\" FILENAME \"]\" }",),
        ("END { print \"[\" FILENAME \"]\", NR }",),
        ("BEGIN { print \"[\" FILENAME \"]\" }",),
        ("BEGIN { getline; print \"[\" FILENAME \"]\" }",),
        ("-f", "p_begin.awk", "-f", "p_end.awk"),
        ("-f", "p_begin.awk", "-f", "p_end.awk", "grid"),
        ("-f", "p_lib.awk", "-f", "p_main.awk", "grid"),
        ("-f", "p_main.awk", "grid"),
        ("-f", "p_print.awk", "letters", "grid"),
        ("-f", "p_print.awk", "{ print NR }", "grid"),
        ("-f", "p_bad.awk"),
        ("-f", "missing.awk"),
        ("-f", "dir"),
        ("-f", "p_span_4095.awk"), ("-f", "p_span_4096.awk"), ("-f", "p_span_65536.awk"), ("-f", "p_span_65537.awk"),
        ("-fp_print.awk", "one"),
        ("-F", "", "{ print NF }", "one"),
        ("-F", ":", "-F", ",", "{ print NF }", "csv"),
        ("-F:", "-v", "OFS=|", "{ $1 = $1 } 1", "colons"),
        ("-F", "\\", "{ print NF }", "grid"),
        ("-F", "\\\\|", "{ print NF }", "grid"),
        ("-F", "[", "{ print NF }", "grid"),
        ("-F", "^", "{ print NF }", "grid"),
        ("-F", "\\t", "{ print NF }", "spaced"),
        ("-Ft", "{ print NF }", "spaced"),
        ("-F", "t", "{ print NF }", "letters"),
        (),
        ("-F",),
        ("-v",),
        ("-f",),
        ("-x", "BEGIN { print 1 }"),
        ("",),
        ("", "grid"),
        ("# nothing", "grid"),
        ("length", "grid"),
        ("ordinary_name", "grid"),
        ("NR%3==1", "letters"),
        ("BEGIN { exit 3 } END { print \"end\" }",),
        ("BEGIN { exit 1 } END { exit }",),
        ("BEGIN { exit 1 } END { exit 5 }",),
        ("BEGIN { exit 256 }",), ("BEGIN { exit -1 }",), ("BEGIN { exit \"3x\" }",),
        ("{ exit } END { print NR }", "letters"),
        ("END { exit 7 }",),
        ("function f() { exit 2 } BEGIN { f(); print \"no\" } END { print \"end\" }",),
    )
    out = list(fixed)
    for i in range(count - len(fixed)):
        argv = []
        for _ in range(rng.choice((1, 2, 3, 30))):
            argv += ["-v", "v%d=%s" % (len(argv) // 2, rng.choice(("1", "a b", "010", "\\x41", "", "1e3")))]
        argv.append("BEGIN { for (i = 0; i < 40; i++) if ((\"v\" i) in a) print i; print v0, v1, v29 } END { print n + 0 }")
        argv += list(rng.choice(((), ("one",), ("n=1", "one"), ("grid", "n=2", "one"), ("missing",))))
        out.append(tuple(argv))
    return out


def awk_gen_regex(rng, count):
    """Every pattern shape through every use: as a pattern, with ~ and !~,
    match, gsub, split, a dynamic string, -F and RS."""
    out = []
    for _ in range(count):
        pattern = rng.choice(AWK_REGEXES)
        data = rng.choice(("mixed", "words", "letters", "colons", "spaced", "looks", "csv", "grid"))
        use = rng.random()
        dynamic = awk_str(pattern)
        if use < 0.15:
            program = f"/{pattern}/ {{ print NR }}"
        elif use < 0.25:
            program = f"!/{pattern}/ {{ print NR }}"
        elif use < 0.35:
            program = f"{{ print ($0 ~ /{pattern}/), ($0 !~ {dynamic}) }}"
        elif use < 0.45:
            program = f"{{ n = gsub(/{pattern}/, \"<&>\"); print n, $0 }}"
        elif use < 0.55:
            program = f"{{ print match($0, /{pattern}/), RSTART, RLENGTH }}"
        elif use < 0.65:
            program = f"{{ n = split($0, a, /{pattern}/); print n, \"[\" a[1] \"]\", \"[\" a[n] \"]\" }}"
        elif use < 0.72:
            program = f"{{ r = {dynamic}; if ($0 ~ r) n++; if (match($0, r)) m += RLENGTH }} END {{ print n + 0, m + 0 }}"
        elif use < 0.80:
            out.append(("-F", pattern.replace("\\", "\\\\"), "{ print NF \"|\" $1 \"|\" $NF }", data))
            continue
        elif use < 0.87:
            out.append(("-v", "RS=" + pattern.replace("\\", "\\\\"), "{ print NR \"[\" $0 \"]\" }", data))
            continue
        elif use < 0.93:
            program = f"{{ if (sub(/{pattern}/, \"[&]\", $1)) print; else print \"no\" }}"
        else:
            program = f"/{pattern}/,/{rng.choice(AWK_REGEXES[:20])}/ {{ print NR \":\" $0 }}"
        out.append((program, data))
    return out


def awk_gen_syntax(rng, count):
    """Programs made of tokens in an order nobody meant, and structural
    shapes: comments, continuation, terminators. Both must agree whether it
    is a program, and on the answer when it is."""
    tokens = ("BEGIN", "END", "{", "}", "(", ")", "[", "]", ";", ",", "$", "print", "printf", "if", "else",
              "while", "for", "do", "in", "delete", "getline", "function", "return", "next", "exit",
              "break", "continue", "+", "-", "*", "/", "%", "^", "=", "==", "!=", "<", ">", "<=", ">=",
              "&&", "||", "!", "~", "!~", "?", ":", "++", "--", "+=", "-=", "|", ">>", "x", "y", "a",
              "1", "2", "\"s\"", "/re/", "NF", "NR", "$0", "$1", "length", "substr", "split", "sub",
              "gsub", "sprintf", "sin", "int", "\n", "# comment\n", "**", "!x", "a[1]", "f()", "-1", "1e3")
    fixed = (
        ("BEGIN { ; print 1;; print 2 ; }",),
        ("BEGIN {\n        print 1\n        print 2\n}",),
        ("BEGIN { # nothing here\n        print 1 # nor here\n}",),
        ("BEGIN { x = 1 + \\\n        2; print x }",),
        ("BEGIN { x = 1 +\n2; print x; if (x &&\n1) print \"and\" }",),
        ("BEGIN { if (0) print 1\nelse print 2 }",),
        ("BEGIN { if (0) print 1; else print 2 }",),
        ("BEGIN { if (1) if (0) print \"a\"; else print \"b\" }",),
        ("BEGIN { print 1,\n2 }",),
        ("BEGIN { a = 1 ||\n0; b = 1 &&\n0; print a, b }",),
        ("BEGIN { print length }",),
        ("BEGIN { print length\n}",),
        ("BEGIN { $0 = \"a b\"; print length + 1, length * 2 }",),
        ("BEGIN { print 1 " " -1; print 1\" \"-1; print 1 -1, 1 - 1, \"x\" 1+1 }",),
        ("BEGIN { print -2^2, 2^-2, -2**2, 2^3^2, 2**3**2 }",),
        ("BEGIN { print !\"a\" ~ \"0\", (!\"a\") ~ \"0\", 1 ~ 1, \"1\" ~ 1 }",),
        ("BEGIN { a[1,2]; print ((1,2) in a), (1,2) in a }",),
        ("BEGIN { print (1,2) }",),
        ("BEGIN { print (1)(2), (1)-(2), (1) (2) }",),
        ("BEGIN { $0 = \"1 2 3\"; i = 1; print $i++, i, $++i, i, $i--, i }",),
        ("BEGIN { x = 8; print x /2/ 1; a = 4; b = 2; c = 1; print a/b/c }",),
        ("BEGIN { print 1 > 2 \"x\"; close(\"2x\"); getline y < \"2x\"; print y }",),
        ("BEGIN { print 2 " " 3 > \"o\" }",),
        ("BEGIN { print(1)(2) > \"o\"; close(\"o\"); getline l < \"o\"; print l }",),
        ("BEGIN { x = 5; x += x -= 2; print x }",),
        ("BEGIN { x = y = z = 3; print x y z }",),
        ("BEGIN { print 1 == 1 == 1, 2 < 3 < 1 }",),
        ("BEGIN { print 1 in a, \"x\" in a ? \"y\" : \"n\" }",),
        ("BEGIN { print $ 1, $(1), $ NF, $NF }",),
        ("BEGIN { print 010 + 0, 0x10 + 0, 1e3, .5, 5., 1.e2 }",),
        ("BEGIN { print \"a\" \"b\" \"c\", \"a\"\"b\" }",),
        ("BEGIN { print \"\\101\\x41\\t|\\/\\\"|\\q\" }",),
        ("BEGIN { print 1; } ; ; BEGIN { print 2 }",),
        ("BEGIN { print 1 }\n\n\nEND { print 2 }\n",),
        ("BEGIN { print 1 } END { print 2 } { print 3 }", "one"),
        ("function f(x) { return x } BEGIN { print f(3) }",),
        ("function f(x) { return x } BEGIN { print f (3) }",),
        ("func f(x) { return x } BEGIN { print f(3) }",),
        ("function f(x) { return x } BEGIN { print f(1, 2) }",),
        ("function f(x, x) { return x } BEGIN { print f(1, 2) }",),
        ("function f() { return 1 } function f() { return 2 } BEGIN { print f() }",),
        ("function NR() { return 1 } BEGIN { print NR() }",),
        ("function f(a) { a[1] = 1 } BEGIN { f(x); x = 1 }",),
        ("function f(a) { a[1] = 1 } BEGIN { x = 1; f(x) }",),
        ("BEGIN { a[1] = 1; print \"[\" a \"]\" }",),
        ("BEGIN { a[1] = 1; a = 2 }",),
        ("BEGIN { x = 1; x[1] = 2 }",),
        ("BEGIN { getline < \"one\" \"two\" }",),
        ("BEGIN { print > \"o\" \"p\" }",),
        ("BEGIN { print 1, 2 > \"o\" > \"p\" }",),
        ("BEGIN { print }",),
        ("BEGIN { printf }",),
        ("BEGIN { print ( }",),
        ("BEGIN { print ) }",),
        ("BEGIN { print \"unterminated }",),
        ("BEGIN { print /unterminated }",),
        ("BEGIN { print \"a\nb\" }",),
        ("BEGIN { x = }",),
        ("BEGIN { if () print 1 }",),
        ("BEGIN { for (;;;) print 1 }",),
        ("BEGIN { while (1) }",),
        ("BEGIN { delete }",),
        ("BEGIN { delete 1 }",),
        ("BEGIN { next }",), ("END { next }",), ("BEGIN { nextfile }",),
        ("BEGIN { return }",), ("{ return 1 }",),
        ("BEGIN { break }",), ("BEGIN { continue }",), ("BEGIN { while (0) { } break }",),
        ("BEGIN { exit exit }",),
        ("BEGIN { a b c }",),
        ("BEGIN { 1 2 3 }",),
        ("BEGIN { $ }",),
        ("BEGIN { getline getline }",),
        ("BEGIN { print length() length }",),
        ("BEGIN { printf(\"%d\\n\", 1) (2) }",),
        ("BEGIN { print(1)(2) }",),
        ("BEGIN { print -1 \" \" -1 }",),
        ("BEGIN { print 1 - -1, 1 - - 1, 1--1 }",),
        ("BEGIN { x = 1; print x++ + ++x, x }",),
        ("BEGIN { print 1e, 1e+ }",),
        ("BEGIN { print .e1 }",),
        ("BEGIN { print 1..2 }",),
        ("BEGIN { print @ }",),
        ("BEGIN { print `x` }",),
        ("BEGIN { print 'x' }",),
        ("BEGIN { a[\"x\"] = 1; delete a[\"x\"]; delete a[\"y\"]; delete a; print length(a) }",),
        ("BEGIN { a[1]=1; for (k in a) delete a[k]; for ((k) in a) print k }",),
        ("BEGIN { print sin(), cos(), atan2(1), exp(), log(), sqrt(), int(), rand(1), srand(1, 2) }",),
        ("BEGIN { print substr(\"x\"), index(\"x\"), split(\"x\"), sub(/a/), gsub(/a/), match(\"x\"), sprintf() }",),
        ("BEGIN { print toupper(), tolower(), close(), system(), length(1, 2), substr(\"a\", 1, 2, 3) }",),
        ("BEGIN { print split(\"a\", \"b\", \"c\"), match(\"a\", \"b\", \"c\") }",),
        ("BEGIN { print fflush(1, 2) }",),
        ("BEGIN { print length a }",),
        ("BEGIN { print length \"x\" }",),
        ("BEGIN { print length(\"x\") \"y\" }",),
        ("BEGIN { print length\n(\"x\") }",),
        ("BEGIN { print substr(\"hello\", 2)(3) }",),
        ("BEGIN { print 1 } # trailing comment",),
        ("BEGIN { print 1 } \\\n END { print 2 }",),
        ("\nBEGIN { print 1 }\n",),
        ("BEGIN { print 1 }; END { print 2 };",),
        ("BEGIN { print 1 } END { print 2 } function f() {}",),
        ("BEGIN { print 1 } function",),
        ("BEGIN",), ("END",), ("{",), ("}",), ("BEGIN {",), ("BEGIN }",), ("BEGIN { }",), ("{ }",), (";",), ("\n",),
    )
    out = list(fixed)
    for _ in range(count):
        program = " ".join(rng.choice(tokens) for _ in range(rng.randint(1, 14)))
        out.append((program, "grid"))
    return out


def awk_gen_numbers(rng, count):
    """Numbers printed and compared: the string and number rules over values
    that look like numbers and values that do not, from every source."""
    looks = ("1", "10", "10.0", "010", "1e2", "+3", "-0", " 7 ", "0", "", "abc", "1x", "3.", ".5",
             "0x10", "1e", "  ", "00", "1.0e1", "inf", "nan", "+inf", "-nan", "1e+308", "1e-320",
             "-1.5e3", "5\r", "\t3", "3\n", "+.", "- 1", "1_000", "١")
    out = []
    for _ in range(count):
        left, right = rng.choice(looks), rng.choice(looks)
        source = rng.random()
        if source < 0.35:
            out.append(("-v", "a=" + left, "-v", "b=" + right,
                        "BEGIN { print (a < b), (a == b), (a > b), (a < 1), (a == 1), (b == \"\"), a + 0, b + 0, length(a) }"))
        elif source < 0.55:
            out.append(("NR == 1 { a = $1 } NR == 2 { print (a < $1), (a == $1), (a > $1), ($1 == 0), ($1 == \"\"), $1 + 0 }",
                        rng.choice(("looks", "words", "numbers", "csv"))))
        elif source < 0.75:
            out.append(("BEGIN { split(%s, p, \":\"); print (p[1] < p[2]), (p[1] == p[2]), p[1] + 0, p[2] + 0, (p[1] == %s) }"
                        % (awk_str(left + ":" + right), rng.choice(("0", "1", "10", "\"1\"", "\"\""))),))
        elif source < 0.90:
            value = rng.choice(AWK_NUMBERS)
            out.append(("BEGIN { x = %s; print x, x \"\", -x, x + 0, (x == %s), int(x), (x < 0), x %% 7, x ^ 0.5 }"
                        % (value, rng.choice(AWK_NUMBERS)),))
        else:
            a, b = rng.choice(AWK_NUMBERS), rng.choice(AWK_NUMBERS)
            out.append(("BEGIN { print %s %s %s, (%s < %s), (%s == %s) }" % (a, rng.choice(("+", "-", "*", "%", "/", "^")), b, a, b, a, b),))
    return out


def awk_gen_records(rng, count):
    """Record separators over the boundary inputs: the reader's refill at
    65535, 65536 and 65537 bytes, in every RS mode."""
    out = []
    for size in ("65535", "65536", "65537"):
        for separator in ("\\n", "", "x+", "a"):
            out.append(("-v", "RS=" + separator, "{ print NR, length($0), NF }", "rec_" + size))
    out.append(("{ print NR, length($0), substr($0, 1, 4), substr($0, length($0)) }", "grow_131073"))
    out.append(("{ print NF, length($2), substr($2, 65530) }", "field_65536"))
    out.append(("{ $2 = \"m\"; print NF, length($0) }", "field_65536"))
    out.append(("{ print NF, $1, $3000 }", "wide"))
    out.append(("{ $3001 = \"z\"; print NF; NF = 5; print }", "wide"))
    out.append(("END { print NR, $0 }", "many"))
    out.append(("NR % 1000 == 0", "many"))
    out.append(("BEGIN { for (i = 1; i <= 100000; i++) s = s \"x\"; $0 = s; print length($0), NF }",))
    out.append(("BEGIN { for (i = 1; i <= 100; i++) s = s i \" \"; $0 = s; print NF, $50, $100 }",))
    for fields in ("63", "64", "65", "127", "128", "129", "1023", "1024", "1025", "8193"):
        out.append(("-v", "n=" + fields,
                    "BEGIN { $1 = \"held\"; $n = \"tail\"; print NF, $1, $n, \"[\" $(n-1) \"]\"; NF = 1; NF = n + 1; print NF, $1, \"[\" $n \"]\", \"[\" $(n+1) \"]\" }"))
    for _ in range(count):
        out.append(("-v", "RS=" + rng.choice(("", ";", ":", "\\n", "[0-9]+", "ab", "x", "\\n\\n", "a|b")),
                    rng.choice(("{ print NR \"[\" $0 \"]\" }", "{ print NR, NF, $2 }", "END { print NR, \"[\" $0 \"]\" }",
                                "{ print NR, length($0) }")),
                    rng.choice(("colons", "paragraphs", "mixed", "nonl", "empty", "spaced", "letters", "csv", "crlf"))))
    return out


def awk_audit():
    """What the hand-written lane found worth asserting and no generator
    reaches on its own: evaluation order with side effects, the regex
    cache, growth of every table, signals through system(), the C locale's
    spelling of infinities."""
    rows = [
        ("{ print (FILENAME == 0), (FILENAME == FILENAME \"\") }", "one"),
        ("function g() { return \"hi\" } function f() { return g() } BEGIN { print f(); print (f() \"\") }",),
        ("function g() { return 1 } function h() { return 2 } function f(c) { return c ? g() : h() } BEGIN { print f(1), f(0) }",),
        ("{ $2 = 7; print; print ($2 > 100), ($2 < 100) }", "five"),
        ("BEGIN { x = 0; sub(/z/, \"\", x); if (x) print \"true\"; else print \"false\"; y = 5; sub(/z/, \"\", y); print (y < 10) }",),
        ("{ $1 = \"\"; sub(/^ /, \"\"); print }", "grid"),
        ("{ $2 = \"a\"; gsub(/a/, \"X\"); print }", "grid"),
        ("{ $3 = \"z\"; $0 = $0 \"!\"; print; print NF }", "grid"),
        ("BEGIN { print 00000000000000000001 + 0, 0.00000000000000000012 + 0, 0.0000000000000000001, 0.000000000000000000000123 }",),
        ("{ print ($1 == 5), ($1 < 10) }", "crlf"),
        ("BEGIN { while (\"echo a; echo b\" | getline line > 0) print line }",),
        ("BEGIN { \"echo hi\" | getline; print NR; \"echo yo\" | getline x; print NR, x }",),
        ("NR == 1 { getline y < FILENAME; print NR, y }", "one"),
        ("BEGIN { s = \"abc\"; sub(/b/, \"\\\\\\\\\\\\&\", s); print s; t = \"abc\"; sub(/b/, \"\\\\\\\\&\", t); print t; u = \"abc\"; sub(/b/, \"\\\\&\", u); print u }",),
        ("function f() { next } { f(); print \"after\", $0 } END { print NR }", "letters"),
        ("function f() { nextfile } { f(); print \"after\", $0 } END { print NR }", "letters", "grid"),
        ("BEGIN { printf \"%.3x|%.10x|%x\\n\", 123456789e20, 1.2e23, 2^70 }",),
        ("BEGIN { s = sprintf(\"%.1000f\", 1e300); print length(s), substr(s, 1, 8), substr(s, length(s) - 3) }",),
        ("BEGIN { printf \"[%1500s][%01500d][%-1500s]\\n\", \"x\", 7, \"y\" }",),
        ("BEGIN { printf \"a\\0%*.*d\\0z\", 4, 2, 7 }",),
        ("BEGIN { printf \"%\\0s\", 7 }",),
        ("BEGIN { printf \"[%*.*d]|%d\\n\", -7, 3, 7, 9 }",),
        ("BEGIN { printf \"[%*.*s]|%d\\n\", 6, -1, \"abcd\", 9 }",),
        ("BEGIN { printf \"[%*d]\\n\", -5, 42 }",),
        ("BEGIN { printf \"[%.0f][%.0f][%.0f][%.0f]\\n\", 999999999, 1000000000, 1000000001, 1e100 }",),
        ("BEGIN { printf \"[%x][%o][%u][%d][%.3x][%10x][%-10x][%#x]\\n\", -1e30, -1e30, -1e30, -1e30, -1e30, -1e30, -1e30, -1e30 }",),
        ("BEGIN { x = -0.0; printf \"[%f][%.0f][%g][%e][%d][%5.1f][%05.1f][%+f]\\n\", x, x, x, x, x, x, x, x; print x, x \"\" }",),
        ("BEGIN { print 2^53, 2^62, 2^64, 1e16, 1e17, 1/3, 0.0000001, 1e-300, 2/7, 1/3 \"x\", 1000000 \"x\", 0.5 \"x\" }",),
        ("BEGIN { x = 1e300 * 1e300; print x, -x, 1/x, (x > 1e308), (x == x) }",),
        ("BEGIN { print \"+inf\" + 0, \"-inf\" + 0, \"+INF\" + 0, \" -Inf \" + 0, (\"+inf\" + 0 > 1e308), \"+inf5\" + 0, \"+infinity\" + 0, \"inf\" + 0 }",),
        ("BEGIN { x = \"+nan\" + 0; y = \"-NaN\" + 0; print x, y, (x == x), (y < 1), \"nan\" + 0, \"-nan(1)\" + 0, length(x \"\") }",),
        ("{ print \"[\" $1 \"]\", $1 + 0, ($1 < 1), ($1 == $1 + 0), ($1 == \"+inf\"), length($1 + 0) }", "words"),
        ("BEGIN { x = \"012\"; for (i = 0; i < 5; i++) { print x + 0, x == 12, x == \"012\", x; x = \"013\" } x = \"\"; print x + 0, !x, x == 0, x == \"\" }",),
        ("BEGIN { split(\"-1e300 -1e100 -1e20 -1e10 -1 0 1 1e10 1e20 1e100 1e300\", v); for (i = 1; i <= 11; i++) printf \"%.12g %.12g\\n\", sin(v[i] + 0), cos(v[i] + 0) }",),
        ("BEGIN { split(\"-inf -1 -0 +0 1 +inf\", v); for (i = 1; i <= 6; i++) for (j = 1; j <= 6; j++) printf \"%.12g\\n\", atan2(v[i] * 1, v[j] * 1) }",),
        ("BEGIN { for (i = -200; i <= 200; i++) { x = i / 13; printf \"%.10g %.10g %.10g %.10g\\n\", exp(x), log(exp(x)), sin(x), cos(x) } }",),
        ("BEGIN { print 1^(1/0.0001 * 0), (-1)^exp(1000), (-1)^(-exp(1000)), (-2)^4097, (-2)^4098, 2^-1, (-2)^3, 10^-3, 2^3^2, -2^2, 2**3 }",),
        ("BEGIN { print -7 % 3, 7 % -3, 7.5 % 2, 5.5 % 2, -5.5 % 2, 5 % 2.5, 1e30 % 7 }",),
        ("BEGIN { print -\"3x\", +\"4y\", !\"\", !\"a\", !0, !1, \"3.5e2\" + 0, \" 12 \" + 0, \".5\" + 0, \"1e\" + 0, \"12abc\" + 0 }",),
        ("BEGIN { x = 1; print (x += (x = 2)), x }",),
        ("BEGIN { $0 = \"1 2 3\"; $3 += (NF = 1); print NF, \"[\" $0 \"]\", \"[\" $3 \"]\" }",),
        ("BEGIN { $0 = \"1 2 3\"; $0 += ($1 = 2); print NF, \"[\" $0 \"]\", \"[\" $1 \"]\" }",),
        ("BEGIN { a[1] = 1; i = 1; print (a[i++] += (a[1] = 2)), a[1], i }",),
        ("BEGIN { i = \"x\"; a[\"x\"] = 1; print (a[i] += (i = \"y\")), a[\"x\"], i }",),
        ("function zap() { delete a[1]; return 2 } BEGIN { a[1] = 1; print (a[1] += zap()), a[1] }",),
        ("function zap() { delete a[1]; a[1] = 9; return 2 } BEGIN { a[1] = 1; print (a[1] += zap()), a[1] }",),
        ("function p() { delete a[1]; a[1] = \"abc\"; return \"a\" } BEGIN { a[1] = \"abc\"; print sub(p(), \"X\", a[1]), a[1] }",),
        ("function k() { print \"K\"; return \"x\" } function p() { print \"P\"; return \"a\" } function r() { print \"R\"; return \"X\" } BEGIN { a[\"x\"] = \"abc\"; print sub(p(), r(), a[k()]), a[\"x\"] }",),
        ("function p() { delete a[\"key\"]; delete a[\"target\"]; a[\"target\"] = \"abc\"; return \"a\" } BEGIN { a[\"key\"] = \"target\"; a[\"target\"] = \"old\"; print sub(p(), \"X\", a[a[\"key\"]]), a[\"target\"] }",),
        ("function p() { return \"a\" } function r(i) { for (i = 0; i < 8; i++) (\"x\" ~ (\"r\" i)); return \"X\" } function k(i) { for (i = 0; i < 8; i++) (\"x\" ~ (\"k\" i)); return \"x\" } BEGIN { a[\"x\"] = \"abc\"; print sub(p(), r(), a[k()]), a[\"x\"] }",),
        ("function p() { NF = 1; return \"2\" } BEGIN { $0 = \"1 2 3\"; print sub(p(), \"X\", $3), NF, \"[\" $0 \"]\", \"[\" $3 \"]\" }",),
        ("function field() { n++; return 3 } function p() { NF = 1; return \"a\" } BEGIN { $0 = \"a b abc\"; print sub(p(), \"X\", $(field())), n, NF, \"[\" $0 \"]\" }",),
        ("function field() { NF = 1; return 3 } BEGIN { $0 = \"1 2 3\"; print sub(\"z\", \"X\", $(field())), NF, \"[\" $0 \"]\" }",),
        ("BEGIN { $0 = \"a   b\"; q = NF; n = sub(/z/, \"x\", $3); print q, n, NF, \"[\" $0 \"]\" }",),
        ("NR == 1 { $1 = $1; next } { n = sub(/z/, \"X\", $3); print n, NF, \"[\" $0 \"]\" }", "grid"),
        ("BEGIN { $0 = \"x\"; for (i = 0; i < 10000; i++) sub(/z/, \"Z\"); print $0 }",),
        ("BEGIN { i = 0; print atan2(i++, i++), i }",),
        ("BEGIN { srand(1); ok = 1; for (i = 0; i < 100; i++) { x = rand(); if (x < 0 || x >= 1) ok = 0 } print ok }",),
        ("BEGIN { srand(1); print srand(5); print srand(7) }",),
        ("BEGIN { srand(3); x = rand(); srand(3); print (x == rand()) }",),
        ("BEGIN { print \"a\"; r = system(\"echo b\"); print \"c\" r }",),
        ("BEGIN { print system(\"kill -TERM $$\") }",),
        ("BEGIN { print system(\"kill -KILL $$\") }",),
        ("BEGIN { print system(\"ulimit -c 0; kill -SEGV $$\") }",),
        ("BEGIN { print system(\"ulimit -c 0; kill -ABRT $$\") }",),
        ("BEGIN { for (i = 1; i <= 140; i++) a[\"k\" i] = i; for (i = 1; i <= 140; i++) s += a[\"k\" i]; print length(a), s, a[\"k1\"], a[\"k140\"] }",),
        ("BEGIN { for (i = 1; i <= 4096; i++) a[\"long-key-\" i \"-abcdefghijklmnop\"] = i; for (i = 3; i <= 4096; i += 3) delete a[\"long-key-\" i \"-abcdefghijklmnop\"]; for (i = 3; i <= 4096; i += 3) a[\"long-key-\" i \"-abcdefghijklmnop\"] = i * 2; for (k in a) sum += a[k]; print length(a), sum }",),
        ("BEGIN { s = \"abcdefghij\"; n = 0; if (s~/a/) n++; if (s~/b/) n++; if (s~/c/) n++; if (s~/d/) n++; if (s~/e/) n++; if (s~/f/) n++; if (s~/g/) n++; if (s~/h/) n++; if (s~/i/) n++; if (s~/j/) n++; if (s~/ab/) n++; if (s~/bc/) n++; if (s~/cd/) n++; if (s~/de/) n++; if (s~/ef/) n++; if (s~/fg/) n++; if (s~/gh/) n++; if (s~/hi/) n++; if (s~/ij/) n++; if (s~/ja/) n++; if (s~/a.c/) n++; if (s~/b.d/) n++; if (s~/c.e/) n++; if (s~/d.f/) n++; if (s~/e.g/) n++; if (s~/x/) n++; if (s~/y/) n++; if (s~/z/) n++; if (s~/aa/) n++; if (s~/bb/) n++; if (s~/[a-c]/) n++; if (s~/[x-z]/) n++; if (s~/a+/) n++; if (s~/q*/) n++; if (s~/^a/) n++; if (s~/(ab|cd)/) n++; if (s~/z|a/) n++; print n }",),
        ("{ r = \"^\" $0 \"$\"; if ($0 ~ r) n++ } END { print n }", "letters"),
        ("{ if (/a/) x++; r = \"^\" $0; if ($0 ~ r) y++; if (/b/) z++ } END { print x, y, z }", "letters"),
        ("BEGIN { a[\"b\"] = 1; a[\"a\"] = 2; a[\"c\"] = 3; n = 0; for (k in a) n++; print n }",),
        ("BEGIN { CONVFMT = \"%.2g\"; a[12] = 1; a[3.14159] = 2; n = 0; for (k in a) n += length(k); print n, (12 in a), (\"3.1\" in a) }",),
        ("BEGIN { if (\"x\" in a) print \"y\"; print length(a); x = a[\"k\"]; print length(a), (\"k\" in a) }",),
        ("function f(n,  a) { a[n] = 1; return length(a) } BEGIN { print f(1), f(2) }",),
        ("function f(a) { return length(a) } BEGIN { print f(x), length(x) }",),
        ("function f() { return } BEGIN { x = f(); print \"[\" x \"]\", x + 0 }",),
        ("function f() { x = 1 } BEGIN { print \"[\" f() \"]\" }",),
        ("function f(n) { return n == 0 ? 0 : 1 + f(n - 1) } BEGIN { print f(200) }",),
        ("function f(n) { return n == 0 ? 0 : 1 + f(n - 1) } BEGIN { print f(1000) }",),
        ("BEGIN { s = \"\"; for (i = 0; i < 5000; i++) s = s \"ab\"; print length(s), substr(s, 9999) }",),
        ("{ print length($0), length($1), length() }", "spaced"),
        ("BEGIN { print index(\"aaaa\", \"aaa\"), index(\"abababc\", \"babc\"), index(\"abc\", \"\"), index(\"\", \"\"), index(\"\", \"a\") }",),
        ("BEGIN { n = split(\":::\", p, \":\"); print n; for (i = 1; i <= n; i++) print i \"[\" p[i] \"]\" }",),
        ("BEGIN { n = split(\"abc\", a, \"x*\"); print n, a[1], a[2]; print split(\"\", b), length(b) }",),
        ("BEGIN { a[9] = \"old\"; n = split(\"x y\", a); print n, length(a), \"[\" a[9] \"]\" }",),
        ("BEGIN { s = \"abc\"; n = gsub(/x*/, \"-\", s); print n, s; t = \"baac\"; n = gsub(/a*/, \"-\", t); print n, t }",),
        ("BEGIN { print match(\"aaa\", /a*/), RSTART, RLENGTH; match(\"abc\", /b/); x = RSTART; match(\"abc\", /z/); print x, RSTART, RLENGTH }",),
        ("BEGIN { print substr(\"hello\", 1.9, 1.9) \"|\" substr(\"hello\", 2.7, 2.2) \"|\" substr(\"hello\", 6) \"|\" substr(\"hello\", 2, 99) }",),
        ("BEGIN { print toupper(\"aBc1_\"), tolower(\"AbC1_\"), int(3.9), int(-3.9), int(\"4.7x\"), int(0), int(\"1e3\") }",),
        ("BEGIN { print sqrt(2), sqrt(0), sqrt(9), sqrt(1e10), exp(1), log(2), exp(0), log(1), exp(-1), sin(0), cos(0), sin(1), cos(1), sin(3.14159) }",),
        ("BEGIN { print atan2(0, 1), atan2(1, 1), atan2(1, 0), atan2(-1, -1), sin(10), cos(10), sin(-2.5), exp(10), log(1000) }",),
        ("BEGIN { print close(\"nothing\") }",),
        ("BEGIN { print length(\"hello\"), length(\"\"), length(12345), 1 2, \"a\" \"b\", 1 \" \" 2 }",),
        ("BEGIN { x = 5; print x++, x, ++x, x, x--, x, --x, x; y = \"5\"; y++; print y; z = \"a\"; z++; print z }",),
        ("{ i = 1; print $i++, i, $1 }", "grid"),
        ("BEGIN { x = 10; x -= 1; x *= 2; x /= 3; x %= 4; x ^= 2; print x }",),
        ("BEGIN { r = \"^a\"; print (\"abc\" ~ r), (\"bc\" ~ r), (\"a.b\" ~ \"a\\\\.b\"), (\"axb\" ~ \"a\\\\.b\") }",),
        ("{ x = /al/; print x }", "words"),
        ("BEGIN { a[\"x\"] = 1; print (\"x\" in a), (\"y\" in a), \"x\" in a ? \"y\" : \"n\" }",),
        ("BEGIN { print (1 < 2), (2 < 1), (1 == 1.0), (10 < 9), (\"abc\" < \"abd\"), (\"10\" < \"9\"), (\"a\" == \"a\"), (10 == \"10\"), (10 == \"10.0\"), (0 == \"\") }",),
        ("BEGIN { print (u == 0), (u == \"\"), (u < 1), (u < \"1\") }",),
        ("NR == 1 { a = $1 } NR == 2 { print (a < $1), (a > $1) }", "looks"),
        ("{ if ($1 < \"5\") print $1 }", "numbers"),
        ("BEGIN { print 1 ? \"a\" : \"b\", 0 ? \"a\" : \"b\"; x = 2; print x == 1 ? \"one\" : x == 2 ? \"two\" : \"many\" }",),
        ("BEGIN { print 1 && 1, 1 && 0, 0 || 1, 0 || 0, !1 || 1; x = 0; 0 && x++; print x; 1 || x++; print x }",),
        ("BEGIN { while (++i < 6) { if (i == 2) continue; if (i == 4) break; s = s i } print i, s }",),
        ("BEGIN { do { ++i; if (i == 2) continue; s = s i } while (++tests < 4); print i, tests, s }",),
        ("BEGIN { do { ++i; if (i == 2) break; s = s i } while (++tests < 4); print i, tests, s }",),
        ("BEGIN { do ; while (++i < 4); print i }",),
        ("BEGIN { for (i = 0; i < 6; i += 1 + 0 * steps++) { if (i == 2) continue; if (i == 4) break; s = s i } print i, steps, s }",),
        ("function f(k) { do { while (1) { for (;;) { return k + 3 } } } while (0) } BEGIN { print f(4) }",),
        ("function f() { do { while (1) { next } } while (0) } { if (NR % 2) f(); print NR, $0 } END { print \"end\", NR }", "letters"),
        ("{ do { while (1) { for (;;) { exit 3 } } } while (0) } END { print \"end\", NR }", "letters"),
        ("BEGIN { for (i = 0; i < 3; i++) { for (j = 0; j < 3; j++) { if (j == 1) continue; if (j == 2) break; print i, j } } }",),
        ("BEGIN { a[1]; a[2]; a[3]; for (k in a) delete a[k]; print length(a); for (i = 1; i <= 5; i++) b[i] = i; for (k in b) if (k % 2) delete b[k]; print length(b) }",),
        ("BEGIN { SUBSEP = \"-\"; a[1, 2] = 1; for (k in a) print k; b[1, 2] = \"x\"; for (k in b) { split(k, p, SUBSEP); print p[1], p[2] } print ((1, 2) in b), ((1, 3) in b), ((1 SUBSEP 2) in b) }",),
        ("BEGIN { a[1] = 1; a[\"1\"] = 2; print length(a), a[1], a[\"1\"], a[1.0] }",),
        ("BEGIN { print -0, 0 * -1, -0 \"\", 1e6, 1e6 \"\", 123456789012 \"\", 0.1 + 0.2, 1e-5 \"\" }",),
        ("BEGIN { OFMT = \"%.2f\"; print 3.14159; print 3.14159 \"\"; print 3, 3.0, 100000; CONVFMT = \"%.2g\"; x = 3.14159; print x \"\"; print x }",),
        ("BEGIN { ORS = \"X\"; printf \"a\\n\" }",),
        ("BEGIN { f = \"%s-%d\\n\"; printf f, \"a\", 2 }",),
        ("{ printf \"%d:%d\\n\", $1, $2 }", "grid"),
        ("{ NR = 10; print NR }", "letters"),
        ("NR == 1 { FNR = 99 } { print FNR }", "letters"),
        ("{ FILENAME = \"x\"; print FILENAME }", "one"),
        ("END { $2 = \"X\"; print; print NF; $0 = \"a b\"; print NF }", "grid"),
        ("{ OFS = \"-\"; $1 = $1; OFS = \"+\"; print }", "grid"),
        ("BEGIN { $0 = \"a\"; print \"[\" $1000 \"]\", NF; $0 = \"\"; print NF, length($0) }",),
        ("BEGIN { print ((((((((((1 + 2)))))))))) }",),
        ("BEGIN { printf \"%s%s%s%s%s%s%s%s%s%s\\n\", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }",),
        ("BEGIN { print \"a\\tb\", \"c\\\\d\", \"e\\\"f\", length(\"\\061\\x41\"), \"\\1012\", \"\\x414\", \"\\1q\", \"\\x@z\" }",),
        ("BEGIN { print (\"a1\" ~ /[[:alpha:]][[:digit:]]/), (\"11\" ~ /[[:alpha:]]/), (\"abc\" ~ /^abc$/), (\"abc\" ~ /^b/), (\"\" ~ /^$/) }",),
        ("BEGIN { print length(\"héllo\"), toupper(\"héllo\"), substr(\"héllo\", 2, 2), index(\"héllo\", \"l\") }",),
    ]
    for escaped in ("\\x41", "\\x4142", "\\xFF", "\\x", "\\xZ", "\\000", "\\400", "\\777", "\\0777",
                    "\\a\\b\\f\\n\\r\\t\\v", "\\q", "\\\\", "", "plain"):
        rows.append(("-v", "x=" + escaped, "BEGIN { printf \"<%s> %d\\n\", x, length(x) }"))
        rows.append(("END { printf \"<%s> %d\\n\", x, length(x) }", "x=" + escaped))
    for operator in ("+", "-", "*", "/", "%", "^", "**"):
        rows.append(("{ x = 8; a[1] = 8; print (x %s= 2), x, (a[1] %s= 2), a[1]; print ($1 %s= 2), $0 }"
                     % (operator, operator, operator), "five"))
    many = []
    for i in range(100):
        many += ["-v", "v%d=%d" % (i, i)]
    rows.append(tuple(many + ["BEGIN { print v0, v63, v64, v99 }"]))
    return rows


def awk_refusals():
    """gawk's extensions, which ours refuses or reads as plain awk: each is
    pinned in the ledger with its reason, and gawk --posix agrees with ours
    on every one of them."""
    return (
        ("BEGIN { print gensub(/a/, \"b\", \"g\", \"aa\") }",),
        ("BEGIN { print (systime() > 0) }",),
        ("BEGIN { print strftime(\"%Y\", 0) }",),
        ("BEGIN { print mktime(\"2026 01 01 0 0 0\") }",),
        ("BEGIN { a[1] = \"b\"; a[2] = \"a\"; n = asort(a); print n, a[1] }",),
        ("BEGIN { a[\"b\"]; a[\"a\"]; n = asorti(a, d); print n, d[1] }",),
        ("BEGIN { n = patsplit(\"a1b2\", p, /[0-9]/); print n }",),
        ("BEGIN { print strtonum(\"0x10\"), strtonum(\"010\") }",),
        ("BEGIN { print and(12, 10), or(12, 10), xor(12, 10), lshift(1, 3), rshift(8, 2), compl(0) }",),
        ("BEGIN { print typeof(x), typeof(1), isarray(a) }",),
        ("BEGIN { print length(x) }",),
        ("BEGINFILE { print \"bf\" } { print } ENDFILE { print \"ef\" }", "one"),
        ("BEGIN { IGNORECASE = 1; print (\"A\" ~ /a/), \"[\" RT \"]\" }",),
        ("BEGIN { FPAT = \"[^,]+\" } { print NF }", "csv"),
        ("BEGIN { FIELDWIDTHS = \"1 2\" } { print $1, $2 }", "grid"),
        ("BEGIN { print PROCINFO[\"version\"] != \"\" }",),
        ("{ print ARGIND, ERRNO }", "one"),
        ("BEGIN { print 0x10, 011, 0x1A + 1 }",),
        ("BEGIN { print (\"a b\" ~ /\\yb/), (\"a b\" ~ /a\\>/), (\"ab\" ~ /\\<a/), (\"a\" ~ /\\S/), (\"a b\" ~ /a\\sb/) }",),
        ("BEGIN { printf \"[%2$s][%1$s]\\n\", \"a\", \"b\" }",),
        ("BEGIN { printf \"[%s][%d]\\n\" }",),
        ("BEGIN { printf \"%s %s\\n\", \"a\" }",),
        ("BEGIN { a[1] = 1; print \"[\" a \"]\" }",),
        ("BEGIN { a[1] = 1; printf \"%s\\n\", a }",),
        ("function f(a) { a[1] = 1 } BEGIN { f(x); x = 1; print x }",),
        ("function f(a) { a[1] = 1 } BEGIN { x = 1; f(x); print x }",),
        ("function NR() { return 1 } BEGIN { print NR() }",),
        ("function f(a, a) { return a } BEGIN { print f(1, 2) }",),
        ("function f(x) { return x } BEGIN { print f (3) }",),
        ("function f(x) { return x } BEGIN { print f(1, 2) }",),
        ("BEGIN { $0 = \"b\"; print index(\"abc\", /b/) }",),
        ("BEGIN { print sub(/1/, \"x\", 123) }",),
        ("BEGIN { print log(-1), sqrt(-1), exp(1000), log(0) }",),
        ("BEGIN { x = log(-1); print (x == x), (x != x), (x < 1), (x > 1) }",),
        ("BEGIN { print \"[\" substr(\"hello\", log(-1), 2) \"]\" }",),
        ("BEGIN { OFMT = \"%d\"; print 3.7; OFMT = \"%s\"; print 2.5; CONVFMT = \"%x\"; x = 255; print x \"\" }",),
        ("BEGIN { print (\"AWKPATH\" in ENVIRON), (\"AWKLIBPATH\" in ENVIRON) }",),
        ("BEGIN { print (\"a\" ~ /^a{,2}$/), (\"a{,2}\" ~ /^a{,2}$/) }",),
        ("BEGIN { printf \"[%lld][%qd][%jd]\\n\", 1, 2, 3 }",),
        ("BEGIN { print (\"q\" ~ /\\q/), (\"d\" ~ /\\d/) }",),
        ("BEGIN { print \"a\" > \"/dev/fd/1\" }",),
        ("BEGIN { \"echo x\" |& getline y; print y }",),
        ("BEGIN { switch (1) { case 1: print \"one\" } }",),
        ("@include \"p_lib.awk\"\nBEGIN { print twice(2) }",),
        ("function f(n) { return n == 0 ? 0 : 1 + f(n - 1) } BEGIN { print f(3000) }",),
        ("function f(n) { return n == 0 ? 0 : 1 + f(n - 1) } BEGIN { print f(20000) }",),
        ("BEGIN { print \"a\" > \"s\"; close(\"s\"); getline l < \"s\"; print l > \"s\"; close(\"s\"); getline m < \"s\"; print m }",),
        ("BEGIN { print substr(\"hello\", 2, 1e300 * 1e300) }",),
    )


AWK_REFUSED_OPTIONS = (
    ("-b", "BEGIN { print 1 }"), ("-c", "BEGIN { print 1 }"), ("-C",), ("-d", "BEGIN { print 1 }"),
    ("-D", "BEGIN { print 1 }"), ("-e", "BEGIN { print 1 }"), ("-E", "p_print.awk", "one"),
    ("-g", "BEGIN { print 1 }"), ("-h",), ("-i", "p_lib.awk", "BEGIN { print twice(1) }"),
    ("-I", "BEGIN { print 1 }"), ("-k", "{ print NF }", "csv"), ("-l", "x", "BEGIN { print 1 }"),
    ("-L", "BEGIN { print 1 }"), ("-M", "BEGIN { print 2^70 }"), ("-N", "BEGIN { print 1 }"),
    ("-n", "BEGIN { print 1 }"), ("-o", "BEGIN { print 1 }"), ("-O", "BEGIN { print 1 }"),
    ("-p", "BEGIN { print 1 }"), ("-P", "BEGIN { print 1 }"), ("-r", "BEGIN { print 1 }"),
    ("-s", "BEGIN { print 1 }"), ("-S", "BEGIN { print 1 }"), ("-t", "BEGIN { print 1 }"), ("-V",),
    ("--characters-as-bytes", "BEGIN { print 1 }"), ("--traditional", "BEGIN { print 1 }"), ("--copyright",),
    ("--dump-variables", "BEGIN { print 1 }"), ("--debug", "BEGIN { print 1 }"),
    ("--source=BEGIN { print 1 }",), ("--exec=p_print.awk", "one"), ("--gen-pot", "BEGIN { print 1 }"),
    ("--help",), ("--include=p_lib.awk", "BEGIN { print twice(1) }"), ("--trace", "BEGIN { print 1 }"),
    ("--csv", "{ print NF }", "csv"), ("--load=x", "BEGIN { print 1 }"), ("--lint", "BEGIN { print 1 }"),
    ("--bignum", "BEGIN { print 2^70 }"), ("--use-lc-numeric", "BEGIN { print 1 }"),
    ("--non-decimal-data", "BEGIN { print 1 }"), ("--pretty-print", "BEGIN { print 1 }"),
    ("--optimize", "BEGIN { print 1 }"), ("--profile", "BEGIN { print 1 }"), ("--posix", "BEGIN { print 1 }"),
    ("--re-interval", "BEGIN { print 1 }"), ("--no-optimize", "BEGIN { print 1 }"),
    ("--sandbox", "BEGIN { print 1 }"), ("--lint-old", "BEGIN { print 1 }"), ("--version",),
    ("--field-separator=:", "{ print $2 }", "colons"), ("--assign=x=1", "BEGIN { print x }"),
    ("--file=p_print.awk", "one"),
)


def awk_extra():
    """Every generated argv, deterministic: the seeds come from the generator
    names, so adding one does not reshuffle another."""
    rows = []
    for name, generator, count in (
            ("expressions", awk_gen_expressions, 180),
            ("printf", awk_gen_printf, 180),
            ("fields", awk_gen_fields, 200),
            ("strings", awk_gen_strings, 200),
            ("control", awk_gen_control, 160),
            ("getline", awk_gen_getline, 70),
            ("redirect", awk_gen_redirect, 70),
            ("cmdline", awk_gen_cmdline, 90),
            ("regex", awk_gen_regex, 180),
            ("syntax", awk_gen_syntax, 100),
            ("numbers", awk_gen_numbers, 120),
            ("records", awk_gen_records, 30)):
        rows.extend(generator(awk_seeded(name), count))
    rows.extend(awk_audit())
    rows.extend(awk_refusals())
    rows.extend(AWK_REFUSED_OPTIONS)
    seen = set()
    unique = []
    for row in rows:
        if row not in seen:
            seen.add(row)
            unique.append(tuple(row))
    return tuple(unique)


UTILITIES = (
    Utility("awk",
            options=(Option("-F", (":", ",", "\\t", " ", "t", "[,:]+", ".", "|", "ab", "\\\\|"), None),
                     Option("-v", ("x=1", "x=abc", "x=010", "x=a\\tb", "OFS=-", "ORS=|", "RS=", "RS=;",
                                   "RS=[0-9]+", "FS=,", "CONVFMT=%.2g", "OFMT=%.2f", "SUBSEP=:", "NF=3", "x"),
                            False, repeat=True),
                     Option("-f", ("p_print.awk", "p_lib.awk", "p_main.awk", "p_bad.awk", "missing.awk"),
                            None, repeat=True),
                     Option("--")),
            operands=AWK_WALK_OPERANDS,
            stdin=("text", "empty", "nonl", "spaces", "tabs", "fields", "numbers", "crlf", "high",
                   "nul_lines", "edge_65536", "awk_csv", "awk_numbers", "awk_paragraphs", "awk_mixed",
                   "awk_rec_65536"),
            fixture="awk",
            stderr="loose",
            valid=awk_valid,
            timeout=10.0,
            max_flags=5,
            extra=awk_extra()),
)
