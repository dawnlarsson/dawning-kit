"""Grammar for the text utilities: coreutils' text tools, grep, sed, cmp, expr
and the util-linux line filters, walked by test/differential.py.

Each program src/sh/text.c answers to is declared as a surface: the options
with the values that reach distinct code paths, the operand shapes it takes,
and the inputs worth feeding it. The spaces too large to list -- the regular
expressions grep and sed match, sed's command language, sort's key
definitions, tr's set syntax, expr's expression grammar -- are composed
procedurally from atoms with a fixed seed, and the engine's covering array
pairs every composed value with every other option. Nothing here is a case
somebody wrote down; the old hand-written lanes were the map of what was once
worth asserting, and every option, fixture shape and boundary they exercised
has an equivalent below.

Every private name is prefixed text_ so this module can be pasted verbatim
into differential.py beside the other domains.
"""
import random
import re

from differential import Utility, Option, INPUTS, FIXTURES


# ----------------------------------------------------------------------------
#       Inputs and fixture files the text tools need beyond the shared ones.
# ----------------------------------------------------------------------------

def text_random_lines(seed, count, longest, alphabet="abc"):
    rng = random.Random(seed)
    return ("\n".join("".join(rng.choice(alphabet) for _ in range(rng.randint(0, longest)))
                      for _ in range(count)) + "\n").encode()


_TEXT_FMT = (b"The quick brown fox jumps over the lazy dog while another sentence follows behind.\n"
             b"This continuation has enough words to require a thoughtful set of line breaks.\n\n"
             b"Short final paragraph.\n")
_TEXT_FMT_INDENT = (b"  First indented line carries enough words to wrap across the requested width.\n"
                    b"    Second indented line has a distinct margin and several more useful words.\n"
                    b"    Third indented line continues that secondary margin.\n")
_TEXT_FMT_PREFIX = (b"  >  Alpha beta gamma delta epsilon zeta eta theta.\n"
                    b"  >  Iota kappa lambda mu nu xi omicron.\n"
                    b"plain material must pass through unchanged\n")
_TEXT_FMT_TABS = (b"\talpha beta gamma delta epsilon zeta eta theta iota kappa lambda\n"
                  b"\tsecond line stays in the same tabbed paragraph for filling\n")
_TEXT_COL = (b"ab\rZ\nA\bB\bC\nabc\tdef\rP\na\x0eB\x0fc\nx\x01Y\n   a\rX\n"
             b"\x1b\x07half\n\x1b\x08rev\n\x1b\x09fwd\n\v\rX\nlast")
_TEXT_UL = b"_\x08u_\x08n_\x08d\nover\x08s\x08t\x1b[4mesc\x1b[0m\n_\x08_\x08x\n\tz_\x08z\nplain\n"

INPUTS.update({
    "text_a": b"alpha\nbeta\ngamma\n",
    "text_a_prefix": b"alpha\nbe",
    "text_fifteen": b"".join(b"%d\n" % n for n in range(1, 16)),
    "text_regex": (b"aaa\nab\nabab\na\n\nbbb\nabc\nAbC\nxyz\n a b\nfoo123bar\nend.\n*x\n**\n"
                   b"aabb\nba\nalpha beta gamma\nx-y_z\n"),
    "text_words": (b"The quick brown fox\nsword word wordy\nfoo_bar foo-bar\n- a\n--b\nx - a\n"
                   b"a-b\n-x\n-ab\nplain a\nthe the\n"),
    "text_random_lines": text_random_lines(20260827, 40, 6),
    "text_sort_numbers": (b"10\n9\n100\n2\n-3\n2.5\n0\n1e3\n0x10\n+7\n 4\n007\n-0\n.5\n5.\n-.5\n"
                          b"1,000\n \n\n-\n+\n.\nabc\n5abc\n0005\n5.10\n5.9\n"),
    "text_sort_human": (b"3\n1K\n2M\n500\n-1G\n2\n900K\n1.5K\n1k\n0K\n-0\nK\n1.K\n.5M\n2T\n2P\n"
                        b"3E\n1Z\n1Y\n1R\n1Q\n25.G\n-2K\n2KB\n  4M\nK6\n0\n"),
    "text_sort_version": (b"1.10\n1.9\n1.2.3\nfoo-1.0.tar.gz\nfoo-1.0~rc1\nfoo-2.tar.gz\n.hidden\n"
                          b"1.0\n1.0.0\nabc\nabc1\nabc10\nabc2\n\n~\n1~\na.b\na.c\n01\n1\n"),
    "text_sort_month": (b"Mar\nJAN\nfeb\nnotamonth\nDec 3\n  Apr\nmay\nJune\nJul\naugust\nSEP\n"
                        b"Oct\nnov\n\nma\nM\n"),
    "text_sort_keys": (b"b 2 x\na 10 y\nc 1 z\na 3 w\nb 2 x\n  a 3 q\nB 2 X\nb\t2\tx\n\n c 1 z\n"
                       b"c  1 z\nfield1 field2 field3\nz1 a2 m3\nz1 b2 a3\n"),
    "text_sort_zero_run": (b"0" * 70 + b"2\n" + b"0" * 64 + b"10\n" + b"0" * 63 + b"1\n" +
                           b"0" * 96 + b"0\n"),
    "text_names0": b"a.txt\x00b.txt\x00",
    "text_names0_nonl": b"a.txt\x00missing",
    "text_cut": (b"abcdefghi\na:b:c:d\n\t\tx\ty\nno delim\n  spaced  out  here\n::\n:\n"
                 b"a:b:c:d:e:f:g:h:i:j\na b\tc  d\n"),
    "text_tr": bytes(range(256)) + b"\naabbcc  dd\nHello World\n",
    "text_tr_refill": b"a" * 65535 + b"b" + b"a" * 65537 + b"c\n",
    "text_tr_ds": b"aabbcc\n",
    "text_uniq": b"apple\napple\nApple\nbanana\n1 x\n2 x\n3 y\nxxa\nxxb\n\n\nend\nend\n",
    "text_uniq_edge": ((b"x" * 65535 + b"\n") * 2 + (b"y" * 65536 + b"\n") * 2 + b"tail\ntail"),
    "text_tabs": b"  \tA\tB\b\tC\r\tD\f\tE\n        X        Y\n\t\tz\n   a   b\n\t \t\n",
    "text_tabs_wide": b" " * 65535 + b"\tX\b\tY\n",
    "text_fmt": _TEXT_FMT,
    "text_fmt_indent": _TEXT_FMT_INDENT,
    "text_fmt_prefix": _TEXT_FMT_PREFIX,
    "text_fmt_tabs": _TEXT_FMT_TABS,
    "text_pr_many": b"".join(b"r%02d\n" % n for n in range(30)),
    "text_pr_formfeed": b"before\fafter\ntail\n",
    "text_ptx": b"one two?  three four!\nfive six\nAlpha beta alpha.\n",
    "text_ptx_refs": b"REF1 red green\nREF2 blue red\n",
    "text_sections": b"a\n\\:\\:\\:\nhdr\n\\:\\:\nbody1\nbody2\n\\:\nfoot\n",
    "text_nl_flush": b"x" * 65528 + b"\ny\n",
    "text_wc_boundary": b"x" * 65535 + b" y\n",
    "text_col": _TEXT_COL,
    "text_ul": _TEXT_UL,
    "text_b64": b"Zm9vYmFyAAECfn+A/v8=\n",
    "text_b64_garbage": b"Z!m@9v\n",
    "text_b64_badpad": b"Zg=\n",
    "text_b32": b"MZXW6YTBOIAACAT6P6AP57Y=\n",
    "text_b32_garbage": b"M!Z@XW6===\n",
    "text_bytes": bytes(range(256)) * 3,
    "text_encoding_binary": b"foobar\x00\x01\x02\x7e\x7f\x80\xfe\xff",
    "text_z85": b"HelloWorld",
    "text_column": b"a b c\nlonger word here\n\nx\n1\t2\t3\nname:value\nlonger:x\n  lead\n",
    "text_join_left": b"1 a\n1 b\n2 c\n4 lone\n",
    "text_sorted_a": b"a\nc\ne\n",
    "text_expr_long": b"",
})

_TEXT_TREE = {
    "tree/one.txt": b"alpha here\nbeta here\n",
    "tree/two.log": b"alpha again\n",
    "tree/three.txt": b"gamma only\n",
    "tree/inner/deep.txt": b"alpha inner\n",
    "tree/other/far.log": b"alpha other\n",
    "tree/link.txt": ("link", "one.txt"),
    "tree/.hidden.txt": b"alpha hidden\n",
    "loop/inner/deep.txt": b"alpha down here\n",
    "loop/inner/back": ("link", ".."),
}

FIXTURES["text"] = {
    **FIXTURES["basic"],
    **_TEXT_TREE,
    "left": b"1 a\n1 b\n2 c\n4 lone\n",
    "right": b"1 x\n1 y\n3 z\n4 pair\n",
    "cleft": b"a\nc\ne\n",
    "cright": b"b\nc\nd\n",
    "fleft": b"a:K:L\nb:M:N\n",
    "fright": b"x:K:R\ny:Q:S\n",
    "hleft": b"name left extra\n1 x\n2 y z\n",
    "hright": b"name right\n1 q r\n3 s\n",
    "zleft": b"1 a\x002 b\x00",
    "zright": b"1 x\x003 y\x00",
    "caseleft": b"A x\na y\nb z\n",
    "caseright": b"a q\na r\nc s\n",
    "badrun": b"a 1\na 2\n0 bad\nz last\n",
    "goodrun": b"a R\nb R\n",
    "unordered": b"b\na\n",
    "unordered2": b"b\nc\na\n",
    "sorted2": b"b\nd\nf\n",
    "same": b"alpha\nbeta\ngamma\n",
    "prefix": b"alpha\nbe",
    "changed": b"alpha\nbeta\ngammX\n",
    "block1": b"0123456789\n" * 7000,
    "block3": b"0123456789\n" * 5999 + b"01234X6789\n" + b"0123456789\n" * 1000,
    "block4": (b"0123456789\n" * 7000)[:65536],
    "words": INPUTS["text_words"],
    "regex": INPUTS["text_regex"],
    "fifteen": INPUTS["text_fifteen"],
    "sections": INPUTS["text_sections"],
    "tabs": INPUTS["text_tabs"],
    "tabs_part": b"1234",
    "para": _TEXT_FMT,
    "pr_many": INPUTS["text_pr_many"],
    "pr_ff": INPUTS["text_pr_formfeed"],
    "ptx_src": INPUTS["text_ptx"],
    "ptx_refs": INPUTS["text_ptx_refs"],
    "ptx_ignore": b"one\nAlpha\n",
    "ptx_only": b"two\nred\nAlpha\n",
    "ptx_breaks": b" ,.!?\t\n",
    "dict": b"apple\nbanana\nbanana split\nBerry\ncherry\ndate\n",
    "dict_tabs": b"apple\tfruit\nbanana\tfruit\nberry\n",
    "pats": b"alpha\nzeta\n",
    "pats_empty": b"",
    "excludes": b"*.log\nthree*\n",
    "excludes_nonl": b"*.txt",
    "script.sed": b"s/a/A/\n2d\n",
    "script2.sed": b"/beta/{\np\n}\n",
    "script3.sed": b"1a\\\nappended\n$i\\\ninserted\n",
    "names0": b"a.txt\x00b.txt\x00",
    "names0_missing": b"a.txt\x00missing\x00",
    "big": INPUTS["many_lines"],
    "wide": INPUTS["edge_65537"],
    "versions": INPUTS["text_sort_version"],
    "months": INPUTS["text_sort_month"],
    "human": INPUTS["text_sort_human"],
    "keys": INPUTS["text_sort_keys"],
    "col_in": _TEXT_COL,
    "ul_in": _TEXT_UL,
    "b64": INPUTS["text_b64"],
    "blank_runs": INPUTS["blank_runs"],
    "mixed": INPUTS["mixed_case"],
    "unsorted": INPUTS["unsorted"],
}


# ----------------------------------------------------------------------------
#       Composed spaces: regular expressions, sed programs, sort keys, tr
#       sets, cut lists and expr expressions.
# ----------------------------------------------------------------------------

_TEXT_BRE_ATOMS = (
    "a", "b", "c", ".", "[ab]", "[^a]", "\\(a\\)", "\\(ab\\)", "[a-c]", "a*", "b*", ".*",
    "\\(a\\)*", "ab", "a\\|b", "\\(a\\|b\\)", "a\\{1,2\\}", "[abc]\\{2\\}", "a\\+", "b\\?",
    "\\(ab\\)*", "\\(a\\|bc\\)", "\\(ab\\|a\\)", "[[:alpha:]]", "[[:digit:]]\\+", "[[:space:]]",
    "[]a]", "[a-]", "[^]a]", "\\<a", "a\\>", "\\ba", "\\w", "\\W", "\\sa", "\\S", "\\(a\\)\\1",
    "\\(\\(a\\)b\\)\\2", "a\\{2\\}", "a\\{,2\\}", "a\\{2,\\}", "x", "\\.", "\\*", "\\(^a\\)",
    "\\(a$\\)", "[.]", "[*]", "\\(a*\\)*", "\\(a\\|\\)", "\\$", "a\\{0\\}", "[[:upper:]]",
    "[a-cx-z]", "\\`a", "a\\'",
)

_TEXT_ERE_ATOMS = (
    "a", "b", "c", ".", "[ab]", "[^a]", "(a)", "(ab)", "[a-c]", "a*", "b*", ".*", "(a)*", "ab",
    "a|b", "(a|b)", "a{1,2}", "[abc]{2}", "a+", "b?", "(ab)+", "(a|b)*", "(a|bc)", "((a|b)+)",
    "(a)\\1", "(a|)b", "(a*)*b", "a{0,2}b", "(ab|a)*", "[[:upper:]]", "[[:alnum:]]+", "\\<b",
    "b\\>", "\\bb\\b", "\\w+", "\\W", "(^a|b$)", "a{2}", "a{,2}", "a{2,}", "x", "\\.", "\\(",
    "[.]", "(a)(b)?\\1", "()", "a{0}", "(a){2}", "(a|aa)*", "a|ab", "(a)(l)(p)(h)(a)()()()()()",
)


def text_regex_pool(seed, atoms, count):
    """count distinct patterns of one to three atoms, some anchored."""
    rng = random.Random(seed)
    pool = []
    seen = set()
    while len(pool) < count:
        pattern = "".join(rng.choice(atoms) for _ in range(rng.randint(1, 3)))
        if rng.random() < 0.25:
            pattern = "^" + pattern
        if rng.random() < 0.25:
            pattern += "$"
        if pattern in seen:
            continue
        seen.add(pattern)
        pool.append(pattern)
    return pool


_TEXT_GREP_FIXED = (
    "alpha", "^delta", "gamma$", "d.lta", "al*pha", "a.*a", "[dz]", "[0-9]", "[^0-9a-z]",
    "[[:digit:]]", "\\(al\\)pha", "\\(a\\)lph\\1", "delta\\|zeta", "[0-9]\\{3\\}", "[0-9]\\+",
    "alphas\\?", "\\<beta", "\\bbeta\\b", "beta\\>", "delta|zeta", "(al)+pha", "[0-9]{3}", "alphas?",
    "alpha beta", "delta epsilon", "beta", "", ".*", "newline", "a[a-z]*", "^$", "\\(ab\\)*",
    "^\\(ab\\)*$", "^a*$", "^.$", "^.*b$", "\\.", "[],]", "[a-]", "[^abc]", "^a\\{3\\}$",
    "^a\\{1,2\\}$", "^a\\{2,\\}$", "^(ab){2}$", "^(ab)?a$", "^((a|b)+)$", "^(aaa|bbb)$", "a$|b$",
    "\\(a\\)\\1", "[a-b]\\{3\\}", "*a", "a^b", "a$b", "^*", "^**", "\\(^*\\)", "a\\{,3\\}",
    "a{,2}", "[ab]", "-|-b", "a|ab", "(a)\\1|(b)\\2", "[\\1]", "the", "word", "^alpha.*gamma$",
    "^missing.*gamma$", "(alpha.*)?gamma", "missing|alpha.*gamma", "(alpha).*gamma",
    "(a)lph\\1.*gamma", "alpha\nbeta", "a\nb\nc", "needle", "\\w\\+", "\\W", "a\\{4000\\}",
    "aaaa$", "x0|x1|x2|x3|x4|x5|x6|x7|x8|x9|x10|x11|x12|x13|x14|x15|x16|x17|x18|x19|x20|x21|x22"
    "|x23|x24|x25|x26|x27|x28|x29|x30|x31|x32|x33", "\\(", "(", "[", "a\\{1", "a{1", "\\",
)

_TEXT_GREP_PATTERNS = tuple(_TEXT_GREP_FIXED) + tuple(
    text_regex_pool(0x47524550, _TEXT_BRE_ATOMS, 20)) + tuple(
    text_regex_pool(0x45524550, _TEXT_ERE_ATOMS, 20))

_TEXT_GREP_OPERAND_PATTERNS = {"alpha", "a", "needle", "x", "^$", "--nosuchflag", "", "alpha\nbeta",
                               "beta", "-x"}


def text_has_pattern(argv, letters, longs, operand_words):
    for index, word in enumerate(argv):
        if word in letters or any(word.startswith(name) for name in longs):
            return True
    return any(word in operand_words for word in argv)


def text_grep_valid(argv):
    return text_has_pattern(argv, ("-e", "-f"), ("--regexp", "--file"), _TEXT_GREP_OPERAND_PATTERNS)


_TEXT_GREP_OPTIONS = (
    Option("-E"), Option("-F"), Option("-G"), Option("-P"),
    Option("--extended-regexp"), Option("--fixed-strings"), Option("--basic-regexp"),
    Option("--perl-regexp"),
    Option("-e", _TEXT_GREP_PATTERNS, False, repeat=True, weight=3),
    Option("--regexp", ("alpha", "a|b", "^$", ""), True),
    Option("-f", ("pats", "pats_empty", "missing", "-", "dir"), None),
    Option("--file", ("pats",), True),
    Option("-i"), Option("--ignore-case"), Option("--no-ignore-case"), Option("-y"),
    Option("-w"), Option("--word-regexp"), Option("-x"), Option("--line-regexp"),
    Option("-z"), Option("--null-data"),
    Option("-s"), Option("--no-messages"), Option("-v"), Option("--invert-match"),
    Option("-m", ("0", "1", "2", "100", "-1", "x", "1K"), None),
    Option("--max-count", ("1",), True),
    Option("-b"), Option("--byte-offset"), Option("-n"), Option("--line-number"),
    Option("--line-buffered"),
    Option("-H"), Option("--with-filename"), Option("-h"), Option("--no-filename"),
    Option("--label", ("X", ""), True),
    Option("-o"), Option("--only-matching"), Option("-q"), Option("--quiet"), Option("--silent"),
    Option("--binary-files", ("binary", "text", "without-match", "bogus"), True),
    Option("-a"), Option("--text"), Option("-I"),
    Option("-d", ("read", "recurse", "skip", "bogus"), None),
    Option("--directories", ("skip",), True),
    Option("-D", ("read", "skip", "bogus"), None), Option("--devices", ("skip",), True),
    Option("-r"), Option("--recursive"), Option("-R"), Option("--dereference-recursive"),
    Option("--include", ("*.txt", "?.txt", "[ot]*", "*.zzz", "*", "a-b.txt", "[!b]*"), True,
           repeat=True),
    Option("--exclude", ("*.log", "*", "three*", "a.txt", "[^a]*"), True, repeat=True),
    Option("--exclude-from", ("excludes", "excludes_nonl", "pats_empty", "missing", "dir", "-"),
           True),
    Option("--exclude-dir", ("inner", "inner/", "other", "tree", "*"), True, repeat=True),
    Option("-L"), Option("--files-without-match"), Option("-l"), Option("--files-with-matches"),
    Option("-c"), Option("--count"), Option("-T"), Option("--initial-tab"), Option("-Z"),
    Option("--null"),
    Option("-B", ("0", "1", "2", "x", "-1", "8193"), None),
    Option("--before-context", ("1",), True),
    Option("-A", ("0", "1", "2", "x"), None), Option("--after-context", ("1",), True),
    Option("-C", ("0", "1", "2", "x"), None), Option("--context", ("1",), True),
    Option("-1"), Option("-2"),
    Option("--group-separator", ("--", "", "##"), True), Option("--no-group-separator"),
    Option("--color", ("never", "always", "auto", "sometimes"), True), Option("--color"),
    Option("--colour", ("always",), True),
    Option("-U"), Option("--binary"),
)

_TEXT_GREP_OPERANDS = (
    (), ("a.txt",), ("a.txt", "b.txt"), ("-",), ("missing",), ("dir",), ("tree",),
    ("tree", "a.txt"), ("unreadable",), ("binary",), ("nonl",), ("empty",), ("words",),
    ("link",), ("dangling",), ("big",), ("two words",), ("loop",), ("-", "a.txt"),
    ("a.txt", "missing", "b.txt"), ("regex",), ("wide",),
)

_TEXT_GREP_STDIN = (
    "text_regex", "text", "text_words", "mixed_case", "empty", "nonl", "nul_lines", "high",
    "blanks", "crlf", "edge_65537", "many_lines", "text_random_lines", "words", "long",
)

_TEXT_GREP_EXTRA = (
    ("alpha",), ("alpha", "a.txt"), ("-c", "alpha", "a.txt", "b.txt"), ("-in", "ALPHA"),
    ("--", "--nosuchflag"), ("-N", "a"), ("--nosuchflag", "a"), ("-Q", "a"),
    ("-e", "alpha", "-e", "beta"), ("-e", "\\(a\\)\\1", "-e", "\\(b\\)\\1", "regex"),
    ("-E", "-x", "(a)\\1|(b)\\2", "regex"), ("-x", "\\(a\\)\\1", "regex"),
    ("-w", "\\(b\\)\\1", "regex"), ("-ow", "[ab]", "words"), ("-cw", "b", "words"),
    ("-Eo", "(a|aa)*", "regex"), ("-Eo", "a|ab", "regex"), ("-Ewo", "-e", "-|-b", "words"),
    ("-Ewo", "a|ab", "regex"), ("-Ewxo", "a|ab", "regex"), ("-Ewxo", "(a)(b)", "regex"),
    ("-Ewxo", "", "regex"), ("-Eo", "(a)(l)(p)(h)(a)()()()()()", "a.txt"),
    ("-Ewo", "(a)(a)(a)(a)(a)(a)(a)(a)(a)\\9", "regex"),
    ("-rl", "--include=*.txt", "--include=*.log", "alpha", "tree"),
    ("-rl", "--include=*.txt", "--exclude=three*", "alpha", "tree"),
    ("-rl", "--exclude-dir=inner", "--exclude-dir=other", "alpha", "tree"),
    ("-rl", "--exclude-from=excludes", "--exclude-from=pats", "alpha", "tree"),
    ("-l", "--include=*.log", "alpha", "tree/one.txt", "tree/two.log"),
    ("--include=*.log", "alpha", "tree/one.txt", "tree/two.log"),
    ("--exclude=*.txt", "alpha", "tree/one.txt", "tree/two.log"),
    ("-c", "--include=*.log", "alpha", "tree/one.txt", "tree/two.log"),
    ("-r", "alpha", "tree", "empty"), ("-rs", "alpha", "missing"), ("-rh", "alpha", "tree"),
    ("-R", "alpha", "loop"), ("-r", "alpha"), ("-rc", "alpha", "tree", "dir"),
    ("-d", "recurse", "alpha", "tree"), ("-d", "skip", "alpha", "tree"),
    ("-A1", "-C2", "5", "fifteen"), ("-C2", "-A1", "5", "fifteen"), ("-C1", "^[13]$", "fifteen"),
    ("-c", "-A1", "alpha", "a.txt"), ("-zZ", "-H", "a", "-"), ("-z", "-C1", "b", "-"),
    ("-z", "-e", "a", "-e", "b"), ("-F", "-f", "pats_empty", "-e", "needle", "big"),
    ("-F", "-f", "pats_empty", "-f", "pats", "a.txt"), ("-F", "-v", "-m0", "needle", "big"),
    ("-F", "-v", "-c", "-n", "-e", "", "big"), ("-m", "2", "-c", "alpha", "a.txt"),
    ("-f", "/dev/null", "a.txt"), ("-v", "-f", "/dev/null", "a.txt"),
    ("--label=X", "-H", "a"), ("--label=X", "-c", "-H", "a", "-"),
    ("--color=always", "-o", "alpha", "words"), ("--color=always", "-Hnb", "alpha", "a.txt"),
    ("--color=always", "-n", "-A1", "alpha", "a.txt"), ("--color=always", "-v", "-A1", "alpha", "a.txt"),
    ("--color=always", "^", "a.txt"), ("--color=always", "-Hc", "alpha", "a.txt"),
    ("--color=always", "-l", "alpha", "a.txt"), ("--color=always", "-az", "a", "-"),
    ("--color=always", "-e", "alpha", "-e", "beta", "a.txt"),
    ("--color=always", "-F", "a.*a", "regex"), ("--color=always", "-i", "alpha", "a.txt"),
    ("--color=always", "-w", "word", "words"), ("--color=always", "-x", "alpha", "a.txt"),
    ("--color=always", "-e", "", "a.txt"), ("--color=always", "-E", "aba|ba", "regex"),
    ("--color=always", "alpha", "nonl"), ("--color=always", "-w", "[ab]", "words"),
    ("--color=always", "-c", "alpha", "controls"), ("--color=auto", "alpha", "a.txt"),
    ("-a", "alpha", "binary"), ("alpha", "binary"), ("-c", "\\x00", "binary"),
    ("-o", "\\{,3\\}", "regex"), ("-o", "a\\{,3\\}", "regex"), ("-oE", "a{,2}", "regex"),
    ("-E", "(", "a.txt"), ("\\(", "a.txt"), ("[", "a.txt"), ("-E", "a{1", "a.txt"),
    ("-P", "a", "a.txt"), ("--perl-regexp", "a", "a.txt"),
)


def text_sed_scripts():
    """Fixed programs from every corner of the language, then composed ones."""
    fixed = [
        "s/alpha/ALPHA/", "s/a/A/g", "s/a/A/2", "s/a/A/2g", "s/alpha/X/p", "s/alpha/[&]/",
        "s/\\(al\\)pha/\\1/", "s/\\(one\\):\\(two\\)/\\2:\\1/", "s/x*/-/g", "/alpha/d", "2,4d",
        "2p", "$p", "3,7p", "/delta/,/zeta/p", "/alpha/!p", "3q", "=", "y/abc/xyz/", "s,one,ONE,",
        "2,4{p}", "s/a/1/;s/b/2/", "s/^alpha/X/", "s/gamma$/X/", "s/[0-9]\\+/N/", "s/hello/X/I",
        "2a added", "2i added", "2c changed", "1h;$ {x;p}", "/^$/d", "s/^$/EMPTY/",
        "s/alpha|zeta/X/", "s/(al)+pha/X/", "s/(a)(l)/\\2\\1/", "s/a{2}/X/", "N;P;D",
        "$!N;s/\\n/+/", ":a;N;$!ba;s/\\n/,/g", "s/a/X/;t e;s/b/Y/;:e", "s/a/X/;T e;s/b/Y/;:e",
        "/a/b e\ns/b/Y/\n:e", ":a;s/a/X/;ta", "1~2p", "0~3p", "3~1p", "4~0p", "2,+2p", "2,~3p",
        "0,/a/p", "0,/a/!p", "1~2d", "Q", "3Q", "3Q5", "2{p;Q}", "F", "1F", "w out", "2w out",
        "$w out", "s/a/X/w out", "s/[ae]/X/gw out", "s/zzz/X/w out", "1w out\n2w out",
        "w /dev/stdout", "s/alpha/X/w /dev/stdout", "1r b.txt", "$r b.txt", "/delta/r b.txt",
        "r b.txt", "r missing", "1R b.txt", "R b.txt", "1{r b.txt\na APPENDED\n}", "1a one\n1a two",
        "1{a one\nr b.txt\na two\n}", "b", "2b\ns/./X/", "/alpha/{:l;p;b};p", ":x;p;$!{n;bx}",
        ":", "b nowhere", "s/a/X/;ta;b;:a;s/$/!/", ":a\n$!{N;ba\n}\ns/\\n/-/g", "l", "l 5", "l 0",
        "1l", "z", "z;s/^/Z/", "n;d", "$!n", "N", "D", "P", "G", "H;$!d;x", "g", "x", "1!G;h;$!d",
        "s/./&\\n/", "s/a/\\U&/", "s/\\(a\\)\\(l\\)/\\u\\1\\U\\2\\E!/", "s/A/\\L&/gI",
        "s/alpha/\\n/", "s/a/\\t/", "s/a/a\\\\b/", "s/a/\\&/", "s///", "s/a//", "s/a/b/3",
        "s/a/b/0", "s/a/b/gg", "s/a/b/pp", "s/a/b/x", "s/a", "s/a/b", "p;p", "3!!p", "5,3p",
        "/3/,5p", "2,4s/^/> /", "2,4!p", "10,$p", "$d", "$=", "y/\\t/ /", "y/abc/xy/", "y/a/",
        "y/\\n/X/", "a\\\nfoo\\\nbar", "i\\", "c\\\nX", "2a\\", "a", "1e echo hi", "s/a/echo X/e",
        "e", "\\%a%p", "/A/Ip", "/a/Mp", "s/^/>/mg", "//p", "s//X/", "/a/s//X/", "{p", "p}", "}",
        "{", ";;p", "# comment\np", "p # c", "v", "v 4.2", "#n\np", "s/a/b/;#c", "y", "q 3", "q x",
        "Q9", "=;=", "n;n;p", "$!{$!d}", "2,1p", "0p", "0,5p", "/a/,3p", "1,/a/p", "$,1p",
        "1,$!d", "N;$p", "n;$p", "h;H;g;G;x;p", "s/a/expanded/g;H;x;p", "N;D;p",
        "s/\\(\\([a-z]\\)[0-9]\\)/[\\1|\\2]/", "/^a*a*a*$/p;/^\\(ab\\)*$/p",
        "/^a*a*a*a*a*a*a*a*a*$/p", "s/[[:blank:]]\\+/ /g", "s/^[ \\t]*//", "s/(a|ab)(b|)/[\\1][\\2]/",
        "s/a/b/w", "w", "r", "b ", "t", "T", "1d;1p",
    ]
    rng = random.Random(0x53454431)
    replacements = ["X", "[&]", "", "Y&Y", "a\\nb", "\\t", "a\\\\b", "\\U&", "\\l&\\E", "&&"]
    flags = ["", "g", "2", "gp", "p", "I", "3g", "2p", "w out", "M", "e"]
    addresses = ["", "1", "2", "$", "/a/", "2,3", "1,$", "/a/,/b/", "1~2", "2,+1", "2,~2", "0,/a/",
                 "/a/!", "\\%a%", "/A/I", "1!", "$!", "/b/,$", "2,/x/", "/^$/"]
    commands = ["p", "d", "=", "l", "q", "Q", "q7", "y/ab/BA/", "a text", "i text", "c text",
                "h", "H", "g", "G", "x", "n", "N", "P", "D", "z", "F", "w out", "r b.txt",
                "{p;p}", "{=;d}", "!d", "s/a/b/;p", "b", "t", "{N;s/\\n/ /}"]
    composed = []
    seen = set(fixed)
    while len(composed) < 50:
        pattern = "".join(rng.choice(_TEXT_BRE_ATOMS) for _ in range(rng.randint(1, 3)))
        if rng.random() < 0.2:
            pattern = "^" + pattern
        if rng.random() < 0.2:
            pattern += "$"
        replacement = rng.choice(replacements)
        if "\\1" in replacement and "\\(" not in pattern:
            replacement = "X"
        if rng.random() < 0.3:
            replacement = "<\\1>" if "\\(" in pattern else "X"
        script = rng.choice(addresses) + "s/" + pattern + "/" + replacement + "/" + rng.choice(flags)
        if script not in seen:
            seen.add(script)
            composed.append(script)
    while len(composed) < 90:
        script = rng.choice(addresses) + rng.choice(commands)
        if rng.random() < 0.3:
            script += ";" + rng.choice(addresses) + rng.choice(commands)
        if script not in seen:
            seen.add(script)
            composed.append(script)
    return tuple(fixed + composed)


_TEXT_SED_SCRIPTS = text_sed_scripts()
_TEXT_SED_OPERAND_SCRIPTS = {"s/a/A/", "$p", "p", "2p", "s/alpha/beta/", "s/x/y/", "s/a/A/g",
                             "s/./X/", "-n", "=", "N;$p", "n;$p", "s/x/y/;#c"}


def text_sed_valid(argv):
    return text_has_pattern(argv, ("-e", "-f"), ("--expression", "--file"), _TEXT_SED_OPERAND_SCRIPTS)


_TEXT_SED_EXTRA = (
    ("s/a/A/", "a.txt"), ("-n", "$p", "a.txt", "b.txt"), ("p",), ("-s", "-n", "$p", "a.txt", "b.txt"),
    ("-s", "-n", "=", "a.txt", "b.txt"), ("-s", "-n", "N;$p", "a.txt", "b.txt"),
    ("-s", "-n", "n;$p", "a.txt", "b.txt"), ("-n", "N;$p", "a.txt", "b.txt"),
    ("--", "s/a/A/"), ("-Q", "p"), ("--nosuchflag", "p"), ("-i", "s/a/A/", "link"),
    ("-i.bak", "--follow-symlinks", "s/alpha/beta/", "link"), ("-n", "-i", "2p", "a.txt"),
    ("-i", "s/a/A/", "a.txt", "b.txt"), ("-i.bak", "s/a/A/", "a.txt"), ("--in-place=.orig", "s/a/A/", "a.txt"),
    ("-i", "s/a/A/"), ("-i", "s/a/A/", "-"), ("-i", "s/a/A/", "missing"), ("-i", "s/a/A/", "dir"),
    ("--follow-symlinks", "s/a/A/"), ("-f", "script.sed", "-e", "p", "a.txt"),
    ("-e", "p", "-f", "script.sed"), ("-f", "script3.sed", "a.txt"), ("s/x/y/", "missing"),
    ("-z", "s/a/A/"), ("-z", "-n", "=", "-"), ("-z", "$d"), ("-z", "-n", "h;H;g;G;x;p"),
    ("-z", "w out", "-n"), ("-E", "s/(a|ab)(b|)/[\\1][\\2]/"), ("-n", "1F", "a.txt", "b.txt"),
    ("-n", "F", "a.txt"), ("-n", "F"), ("--posix", "s/a/A/"), ("--sandbox", "s/a/A/"),
    ("--sandbox", "w out"), ("--sandbox", "r b.txt"), ("--sandbox", "1e echo"),
    ("-u", "s/a/A/"), ("-l", "2", "l"), ("-l", "0", "l"), ("-l", "x", "l"), ("--debug", "p"),
    ("0,/alpha/p", "-n", "a.txt", "repeats"), ("-s", "-n", "0,/a/p", "a.txt", "repeats"),
    ("1r b.txt", "a.txt"), ("1{r b.txt\na APPENDED\n}", "a.txt"), ("1{a one\nr b.txt\na two\n}", "a.txt"),
    ("-n", "/needle/p", "big"), ("-n", "4999p", "big"), ("s/a*/X/", "wide"), ("s/a/b/g", "wide"),
    ("-e", "s/a/X/w out", "-n", "-e", "s/b/Y/"), ("-n", "s/alpha/X/w /dev/stdout"),
)


_TEXT_SORT_KEYS = (
    "1", "2", "3", "1,1", "2,2", "2n", "2nr", "1.2", "1.2,1.3", "2b", "2,3", "9", "1,1r", "2h",
    "1V", "1M", "1d", "1f", "1i", "1R", "1g", "2.2,2.7n", "1,", "1.", "1.0", "1nV", "0", "x",
    "1Q", "1b,1", "2,2b", "-1", "1,1b", "3,3n", "2,2f", "1.3,1.5", "2n,3", "1r,1", "1bd", "1fV",
    "1,1M", "1h,1", "1.1,1.1", "2.0", "1,2.0", "1n", "1b", "2,2n", "3", "2.3", "1,1n", "1,3",
)


_TEXT_CUT_LISTS = (
    "1", "2-4", "3-", "-3", "1,3-", "1,3,5", "5-2", "0", "", "1,", "1x", "-", "2,4-6", "4-6,2",
    "1-3,5-", "-3,5-7", "1,1,1", "2-", "99", "18446744073709551615", "18446744073709551616-",
    "1-18446744073709551616", "1-2,4", "6-", "3990-", "5000", "4999-5001", "1-100", "2", "3,1",
    "2,2", "0-", "-0", "+1",
)


def text_cut_valid(argv):
    lists = 0
    for word in argv:
        if word in ("-b", "-c", "-f", "-F") or word.startswith(("--bytes", "--characters", "--fields")) \
                or (word.startswith(("-b", "-c", "-f", "-F")) and len(word) > 2 and not word.startswith("--")):
            lists += 1
    return lists == 1


_TEXT_TR_OPERANDS = (
    ("a-z", "A-Z"), ("a", "b"), ("abc", "x"), ("[:lower:]", "[:upper:]"), ("[:upper:]", "[:lower:]"),
    ("\\n", " "), (" ", "\\n"), ("a-c", "\\0"), ("[:space:]",), ("a",), (), ("a", "b", "c"),
    ("[a*3]", "xyz"), ("z-a", "b"), ("aeiou",), ("[:digit:]",), ("[:alpha:]", "x"), ("[:blank:]",),
    ("[:punct:]",), ("[:alnum:]", "[:digit:]"), ("[=a=]", "x"), ("\\157", "O"), ("\\1570", "X"),
    ("\\157q", "X"), ("one", "[X*]"), ("ab", "abcdef"), ("a-e", "1-5"), ("\\376-\\377", "XY"),
    ("[:upper:]",), ("abc", "xy"), ("a-z", "A-M"), ("a-y", "X"), ("an",), ("[a*1024]b", "[x*1024]y"),
    ("[a*]", "x"), ("a", "[b*]"), ("[a*0]", "x"), ("a\\", "x"), ("\\", "x"), ("[:foo:]", "x"),
    ("[=", "x"), ("a-", "x"), ("-a", "x"), ("a-b-c", "xyz"), ("[:lower:]", "x"), ("abc", "[:upper:]"),
    ("a", "[=x=]"), ("ab", "[=x=]"), ("[x*]", "y"), ("a", "[x*][y*]"), ("[a*01]", "x"),
    ("[a*x]", "x"), ("[::]", "x"), ("abc", ""), ("", "x"), ("", ""), ("\\400", "x"), ("\\x41", "x"),
    ("a\\-z", "x"), ("[:lower:]", "[:upper:]x"), ("a[:lower:]", "[:upper:]"), ("[:lower:]", "x[:upper:]"),
    ("a-z0-9", "A-Z"), ("\\t", " "), ("\\a\\b\\f\\r\\v", "12345"), ("[:cntrl:]", "?"), ("[:graph:]",),
    ("[:print:]", "."), ("[:xdigit:]", "h"), ("aB.", "x"), ("\\000-\\377", "x"), ("a", "B"),
    ("ab", "ba"), ("abcdefghijklmnopqrstuvwxyz", "zyxwvutsrqponmlkjihgfedcba"), ("a-a", "x"),
    ("[a-c]", "x"), ("a-c", "[x*2]y"), ("aB.",), ("\\0",), ("a", "\\n"),
)


def text_expr_operands():
    """Every adjacent precedence pair, the keywords, the errors, and composed
    expressions over a small grammar."""
    fixed = [
        ("1", "+", "1"), ("5", "-", "9"), ("3", "*", "4"), ("7", "/", "2"), ("-3", "/", "2"),
        ("7", "%", "3"), ("2", "+", "3", "*", "4"), ("10", "-", "3", "-", "2"),
        ("(", "1", "+", "2", ")", "*", "3"), ("0",), ("00",), ("",), ("abc",), ("0.0",),
        ("1", "=", "1"), ("1", "=", "2"), ("abc", "=", "abc"), ("abc", "<", "abd"), ("3", "<", "10"),
        ("10", ">", "9"), ("1", "!=", "2"), ("1", "=", "1", "=", "1"), ("abc", "|", "0"),
        ("", "|", "abc"), ("0", "|", "0"), ("abc", "&", "def"), ("abc", "&", ""), ("", "&", "abc"),
        ("1", "|", "1", "/", "0"), ("0", "&", "1", "/", "0"), ("abc", ":", "a.c"), ("abc", ":", "b"),
        ("abc", ":", "^a"), ("aab", ":", "a*"), ("bbb", ":", "a*"), ("abc", ":", "abc$"),
        ("abcdef", ":", "abc\\(d\\)e"), ("abc", ":", "\\(b\\)"), ("match", "abc", "a\\(b\\)"),
        ("match", "abc", "x"), ("length", "abcde"), ("length", ""), ("substr", "abcde", "2", "3"),
        ("substr", "abcde", "0", "2"), ("substr", "abcde", "2", "100"), ("substr", "abcde", "9", "2"),
        ("substr", "abcde", "x", "2"), ("substr", "abcde", "2"), ("index", "abcde", "cd"),
        ("index", "abcde", "xyz"), ("index", "abcde", ""), ("5", "/", "0"), ("5", "%", "0"),
        ("foo", "+", "1"), (" 1", "+", "1"), ("1", "+"), ("(", "1"), ("1", "1"), (), ("--", "1"),
        ("+", "length"), ("+", "+"), ("+",), ("+", "1"), ("(", ")"), (")",), ("1", ")"),
        ("9223372036854775807", "+", "1"), ("-9223372036854775808", "-", "1"),
        ("9223372036854775807", "*", "2"), ("-9223372036854775808", "/", "-1"),
        ("-9223372036854775808", "%", "-1"), ("99999999999999999999", "+", "0"),
        ("1", "!", "2"), ("1", "<<", "2"), ("1", "<=x", "2"), ("1", "&&", "2"), ("1", "||", "2"),
        ("1", "+1", "2"), ("1", "", "2"), ("1", ">>", "2"), ("abc", ":", "\\(a\\)\\(b\\)"),
        ("abc", ":", "a\\|b"), ("abc", ":", "\\(x\\)*"), ("abc", ":", ".*"), ("", ":", "a*"),
        ("abc", ":", "[[:alpha:]]*"), ("a.c", ":", "a\\.c"), ("abc", ":", "a\\{1,2\\}"),
        ("abc", ":", "\\("), ("abc", ":", "*a"), ("length", "a", "b"), ("match",), ("substr", "a"),
        ("index", "a"), ("length",), ("1", "+", "1", "+"), ("-1", "+", "-1"), ("+1", "+", "1"),
        ("1", "<", "a"), ("a", "<", "1"), ("10", "<", "9a"), ("-", "1"), ("(", "(", "1", ")", ")"),
        ("substr", "x" * 20000, "1", "20000"), ("length", "x" * 20000),
    ]
    operators = ["|", "&", "=", "!=", "<", "<=", ">", ">=", "+", "-", "*", "/", "%"]
    for left in operators:
        for right in operators:
            fixed.append(("7", left, "2", right, "3"))
    for value in ("0", "00", "", "word", "-1"):
        for operator in ("|", "&"):
            for tail in (["1", "/", "0"], ["x", "+", "1"], ["(", "1", "+", "2", ")"], ["(", "1", "+", ")"]):
                fixed.append((value, operator, *tail))
    rng = random.Random(0x45585052)
    atoms = ["0", "1", "2", "-3", "10", "abc", "", "007", "9223372036854775807", "(", ")"]
    for _ in range(40):
        words = []
        for _ in range(rng.randint(1, 4)):
            words.append(rng.choice(atoms[:9]))
            words.append(rng.choice(operators + [":", "match", "length"]))
        words.append(rng.choice(atoms[:9]))
        fixed.append(tuple(words))
    return tuple(fixed)


def text_pr_normalize(channel, data):
    """pr's page header carries a date and time: the file's modification time
    for a named operand and now for standard input, neither of which is the
    program's behaviour. Everything else in the header is compared."""
    if channel != "stdout":
        return data
    return re.sub(rb"\d{4}-\d{2}-\d{2} \d{2}:\d{2}", b"<DATE>", data)


def text_column_valid(argv):
    table = any(word in ("-t", "--table", "-J", "--json", "-K", "--table-header-as-columns")
                for word in argv)
    fill = any(word in ("-x", "--fillrows") for word in argv)
    # column 2.42.2 never returns from --table-maxout beside
    # --table-header-as-columns; there is no answer to compare against.
    header = any(word in ("-K", "--table-header-as-columns") for word in argv)
    maxout = any(word in ("-m", "--table-maxout") for word in argv)
    return not (table and fill) and not (header and maxout)


_TEXT_ENCODINGS = ("--base64", "--base64url", "--base32", "--base32hex", "--base16",
                   "--base2msbf", "--base2lsbf", "--z85", "--base58")


def text_basenc_valid(argv):
    return sum(1 for word in argv if word in _TEXT_ENCODINGS) <= 1


_TEXT_ENCODING_OPTIONS = (
    Option("-d"), Option("--decode"), Option("-i"), Option("--ignore-garbage"),
    Option("-w", ("0", "1", "4", "7", "76", "100", "-1", "x", "18446744073709551616"), None),
    Option("--wrap", ("0", "5"), True),
)
_TEXT_ENCODING_OPERANDS = ((), ("a.txt",), ("-",), ("missing",), ("empty",), ("binary",),
                           ("a.txt", "b.txt"), ("dir",), ("unreadable",), ("b64",), ("nonl",))
_TEXT_ENCODING_STDIN = ("text", "empty", "text_b64", "text_b64_garbage", "text_b64_badpad",
                        "text_b32", "text_b32_garbage", "text_encoding_binary", "edge_65537",
                        "nonl", "blanks", "text_bytes", "text_z85", "edge_131072")

_TEXT_COUNT_VALUES = ("0", "1", "2", "3", "-1", "-3", "+2", "100", "-0", "1K", "1kB", "x", "-99",
                      "18446744073709551616", "5", "+1")
_TEXT_BYTE_VALUES = ("0", "1", "5", "-3", "+3", "70000", "1K", "-0", "x", "10", "-999", "+30",
                     "18446744073709551616")
_TEXT_HEAD_TAIL_OPERANDS = ((), ("a.txt",), ("a.txt", "b.txt"), ("-", "a.txt"), ("a.txt", "-", "b.txt"),
                            ("missing",), ("empty",), ("nonl",), ("dir",), ("big",), ("wide",),
                            ("a.txt", "missing", "b.txt"), ("unreadable",), ("two words",),
                            ("fifteen",), ("a.txt", "b.txt", "repeats"), ("empty", "fifteen"))
_TEXT_HEAD_TAIL_STDIN = ("text", "empty", "nonl", "nul", "edge_65537", "many_lines", "blanks",
                         "text_fifteen", "long", "blank_runs")

_TEXT_TAB_LISTS = ("3", "3,5", "3,5,/4", "3,5,+4", "0", "1", "8", "3 5", "5,3", "3,/4,+2", "x",
                   "-1", "20000", "4,8,12", "/4", "+4", ",", "3,", "65536,65544", "2")
_TEXT_TAB_OPERANDS = ((), ("tabs",), ("a.txt",), ("tabs_part", "tabs"), ("missing",), ("dir",),
                      ("-",), ("tabs", "-"), ("nonl",), ("empty",))
_TEXT_TAB_STDIN = ("tabs", "text_tabs", "spaces", "empty", "nonl", "controls", "crlf",
                   "text_tabs_wide", "text", "edge_65535")


# ----------------------------------------------------------------------------
#       The programs.
# ----------------------------------------------------------------------------

UTILITIES = (
    Utility("base64", options=_TEXT_ENCODING_OPTIONS, operands=_TEXT_ENCODING_OPERANDS,
            stdin=_TEXT_ENCODING_STDIN, fixture="text",
            extra=(("--nosuchflag",), ("-Q",), ("-d", "-w", "0"), ("-di",))),
    Utility("base32", options=_TEXT_ENCODING_OPTIONS, operands=_TEXT_ENCODING_OPERANDS,
            stdin=_TEXT_ENCODING_STDIN, fixture="text",
            extra=(("--nosuchflag",), ("-Q",), ("-w0",), ("-di",))),
    Utility("basenc",
            options=_TEXT_ENCODING_OPTIONS + tuple(Option(name) for name in _TEXT_ENCODINGS),
            operands=_TEXT_ENCODING_OPERANDS, stdin=_TEXT_ENCODING_STDIN, fixture="text",
            valid=text_basenc_valid,
            extra=(("--base64", "--base32"), ("--nosuchflag",), ("--z85", "-w4"), ("--z85", "-w1"),
                   ("--base2lsbf", "-di"), ("--base16", "-d", "-i"))),
    Utility("cat",
            options=(Option("-A"), Option("--show-all"), Option("-b"), Option("--number-nonblank"),
                     Option("-e"), Option("-E"), Option("--show-ends"), Option("-n"), Option("--number"),
                     Option("-s"), Option("--squeeze-blank"), Option("-t"), Option("-T"),
                     Option("--show-tabs"), Option("-u"), Option("-v"), Option("--show-nonprinting")),
            operands=((), ("a.txt",), ("a.txt", "b.txt"), ("-",), ("a.txt", "-", "b.txt"), ("nonl",),
                      ("empty",), ("missing",), ("dir",), ("binary",), ("unreadable",), ("dangling",),
                      ("link",), ("two words",), ("a.txt", "missing", "b.txt"), ("big",), ("wide",)),
            stdin=("text", "empty", "nonl", "blanks", "tabs", "controls", "high", "edge_65536",
                   "blank_runs", "crlf", "nul_lines", "text_uniq", "long"),
            fixture="text", extra=(("-Z", "a.txt"), ("--nosuchflag", "a.txt"), ("--", "-n"))),
    Utility("cmp",
            options=(Option("-b"), Option("--print-bytes"), Option("-l"), Option("--verbose"),
                     Option("-s"), Option("--quiet"), Option("--silent"),
                     Option("-n", ("0", "1", "3", "5", "60", "9999", "1K", "1kB", "1KiB", "+1", "zz",
                                   "18446744073709551616", "65000"), None),
                     Option("--bytes", ("3",), True),
                     Option("-i", ("0", "1", "5", "5:7", "0:0", "9999", "zz", "1K", "3:", "1:2"), None),
                     Option("--ignore-initial", ("5:7",), True)),
            operands=(("a.txt", "same"), ("a.txt", "changed"), ("changed", "a.txt"), ("a.txt", "prefix"),
                      ("prefix", "a.txt"), ("a.txt", "empty"), ("empty", "empty"), ("a.txt", "missing"),
                      ("missing", "missing"), ("-", "a.txt"), ("a.txt", "-"), ("-", "-"), ("a.txt",), (),
                      ("a.txt", "changed", "5"), ("a.txt", "changed", "5", "7"), ("a.txt", "changed", "0", "0"),
                      ("a.txt", "changed", "1", "2", "3"), ("dir", "a.txt"), ("block1", "block3"),
                      ("block1", "block4"), ("block1", "block1"), ("binary", "a.txt"),
                      ("--", "a.txt", "changed"), ("a.txt", "changed", "zz"), ("nonl", "a.txt"),
                      ("tabs", "words"), ("unreadable", "a.txt")),
            stdin=("text_a", "text_a_prefix", "text", "empty"), fixture="text"),
    Utility("col",
            options=(Option("-b"), Option("--no-backspaces"), Option("-f"), Option("--fine"),
                     Option("-p"), Option("--pass"), Option("-h"), Option("--tabs"), Option("-x"),
                     Option("--spaces"),
                     Option("-l", ("1", "2", "128", "0", "x", "100000"), None),
                     Option("--lines", ("3",), True)),
            operands=((), ("a.txt",)),
            stdin=("text_col", "text", "controls", "empty", "nonl", "tabs", "crlf", "edge_65536",
                   "spaces", "text_ul", "high", "blanks", "text_tabs"),
            fixture="text", extra=(("-Q",), ("--nosuchflag",))),
    Utility("colcrt",
            options=(Option("-"), Option("--no-underlining"), Option("-2"), Option("--half-lines")),
            operands=((), ("a.txt",), ("ul_in",), ("col_in",), ("missing",), ("a.txt", "b.txt"),
                      ("-",), ("dir",), ("wide",)),
            stdin=("text_ul", "text_col", "text", "empty", "nonl", "tabs", "controls", "edge_65536",
                   "high"),
            fixture="text", extra=(("-Q",), ("--nosuchflag",))),
    Utility("colrm",
            operands=((), ("3",), ("3", "5"), ("1", "1"), ("0",), ("5", "3"), ("x",), ("3", "5", "7"),
                      ("100",), ("0", "0"), ("1",), ("2", "2"), ("-1",), ("3", "x"), ("9", "12")),
            stdin=("text", "tabs", "text_col", "empty", "nonl", "edge_65536", "spaces", "controls",
                   "high", "text_ul", "text_tabs"),
            fixture="text", extra=(("-Q",), ("--nosuchflag",), ("--help",))),
    Utility("column",
            options=(Option("-t"), Option("--table"),
                     Option("-n", ("tbl", ""), None), Option("--table-name", ("tbl",), True),
                     Option("-O", ("3,1", "D,A", "1", "A,B,C,D", "9", "Z", "3,1,3,2", "D,A,D", "4,1,4"), None),
                     Option("--table-order", ("2,1",), True),
                     Option("-N", ("A,B,C,D", "A,B", "A", "A,,C", "A,B,C", ""), None),
                     Option("--table-columns", ("A,B",), True),
                     Option("-l", ("2", "3", "0", "1", "x"), None),
                     Option("-E", ("B", "1", "A,C", "Z"), None), Option("-d"), Option("--table-noheadings"),
                     Option("-m"), Option("-e"), Option("-K"), Option("--table-header-as-columns"),
                     Option("-H", ("A", "2", "A,C", "Z"), None), Option("--table-hide", ("1",), True),
                     Option("-R", ("A", "2", "B,C", "C"), None), Option("-T", ("B", "1"), None),
                     Option("-W", ("B", "1", "A"), None), Option("--wrap-separator", ("|",), True),
                     Option("-L"), Option("--keep-empty-lines"), Option("-J"), Option("--json"),
                     Option("-r", ("1",), None), Option("-i", ("1",), None), Option("-p", ("2",), None),
                     Option("-c", ("0", "1", "20", "40", "80", "200", "unlimited", "x", "-1", "8"), None),
                     Option("--output-width", ("30",), True),
                     Option("-o", ("|", ",", "", "::"), None), Option("--output-separator", ("|",), True),
                     Option("-s", (":", ",", " ", "", ":,", "\t"), None), Option("--separator", (":",), True),
                     Option("--input-separator", (":",), True),
                     Option("-x"), Option("--fillrows"),
                     Option("-S", ("0", "1", "3", "20", "x"), None), Option("--use-spaces", ("2",), True),
                     Option("--color", ("never", "auto", "always"), True), Option("--color"),
                     Option("-C", ("name=A", "name=A,right"), None), Option("--table-colorscheme", ("x",), True)),
            operands=((), ("a.txt",), ("fields",), ("missing",), ("a.txt", "fields"), ("-",), ("dir",),
                      ("empty",), ("wide",), ("keys",), ("tabs",)),
            stdin=("text_column", "text", "fields", "spaces", "tabs", "empty", "nonl", "blanks",
                   "edge_65536", "text_sort_keys", "unsorted", "high", "many_lines"),
            fixture="text", valid=text_column_valid,
            extra=(("-x", "-t"), ("--nosuchflag",), ("-Q",),
                   ("-t", "-s", ":", "-N", "A,B,C", "-O", "C,A,C"), ("-t", "-s", ":", "-N", "A,B,C", "-O", "C,A,C", "-o", "|"),
                   ("-t", "-s", ":", "-N", "A,B,C", "-O", "C,A,C", "-R", "C"),
                   ("-t", "-s", ":", "-N", "A,B,C", "-O", "C,A,C", "-E", "B"),
                   ("-t", "-s", ":", "-N", "A,B,C", "-O", "C,A,C", "-c", "unlimited"),
                   ("-t", "-N", "A,B,C,D", "-O", "3,1,3,2", "-H", "2"), ("-t", "-N", "A,B,C,D", "-O", "D,A,D", "-R", "A"),
                   ("-t", "-K", "-O", "D,B,D", "-H", "A"), ("-t", "-J", "-N", "A,B,C,D", "-O", "4,1,4", "-H", "2"),
                   ("-c", "20", "fields"), ("-c", "20", "-x", "fields"), ("-c", "20", "-S", "1"),
                   ("-c", "20", "-S", "3", "-x"))),
    Utility("comm",
            options=(Option("-1"), Option("-2"), Option("-3"), Option("-12"), Option("-13"), Option("-23"),
                     Option("-z"), Option("--zero-terminated"), Option("--check-order"),
                     Option("--nocheck-order"),
                     Option("--output-delimiter", ("::", "", ",", "\t"), True), Option("--total")),
            operands=(("cleft", "cright"), ("cleft", "cleft"), ("unordered", "unordered"),
                      ("unordered", "unordered2"), ("zleft", "zright"), ("-", "cright"), ("cleft", "-"),
                      ("cleft",), (), ("cleft", "cright", "a.txt"), ("missing", "cright"), ("empty", "cright"),
                      ("cleft", "empty"), ("dir", "cright"), ("wide", "wide"), ("-", "-"), ("a.txt", "b.txt"),
                      ("repeats", "repeats"), ("big", "big")),
            stdin=("text_sorted_a", "empty", "nonl", "nul", "text", "text_fifteen"), fixture="text",
            extra=(("--nosuchflag", "cleft", "cright"), ("-Q", "cleft", "cright"))),
    Utility("cut",
            options=(Option("-b", _TEXT_CUT_LISTS, True), Option("--bytes", ("1-3", "2-"), True),
                     Option("-c", _TEXT_CUT_LISTS, None), Option("--characters", ("1-3",), True),
                     Option("-f", _TEXT_CUT_LISTS, False), Option("--fields", ("2", "1"), True),
                     Option("-F", ("1", "2-", "1,3", "2"), None),
                     Option("-d", (":", ",", " ", "", "ab", "\t", "\\t"), None),
                     Option("--delimiter", (":",), True),
                     Option("-s"), Option("--only-delimited"), Option("-w"), Option("--whitespace-delimited"),
                     Option("--whitespace-delimited=trimmed"),
                     Option("-z"), Option("--zero-terminated"), Option("-n"), Option("--no-partial"),
                     Option("--complement"),
                     Option("-O", ("X", "", "::"), None), Option("--output-delimiter", ("X", ""), True)),
            operands=((), ("fields",), ("a.txt",), ("fields", "a.txt"), ("-",), ("missing",), ("dir",),
                      ("nonl",), ("empty",), ("wide",), ("tabs",), ("fields", "missing", "a.txt")),
            stdin=("fields", "text_cut", "tabs", "spaces", "empty", "nonl", "nul_lines", "high",
                   "edge_65537", "text", "words", "text_random_lines"),
            fixture="text", valid=text_cut_valid,
            extra=(("-b", "2", "-c", "1-3"), ("-c", "1", "-f", "1"), ("-f", "1", "-c", "1"), ("-d", ",", "-c", "1"),
                   ("-s", "-c", "1"), ("-w", "-c", "1"), ("-w", "-d", ":", "-f", "1"), ("-Z", "-c", "1"),
                   ("--nosuchflag", "-c", "1"), ("-c",), ("-f", "1", "-f", "3", "-d", ":"),
                   ("-d", ":", "-f", "1,1,1"), ("-d", ":", "-f", "3,1"), ("-d", ",", "-f", "2,2"),
                   ("-c1-3", "--complement"), ("-c1,3", "--output-delimiter=X"), ("-c1-2,4", "--output-delimiter=X"),
                   ("-c1,2", "--output-delimiter=X"), ("-OX", "-c1,3"), ("-c4999,5001", "-OX", "wide"),
                   ("-c5000", "wide"), ("-c4999-5001", "wide"), ("-c1-", "wide"), ("-c", "1", "nonl"),
                   (), ("-d", ":"), ("--complement",))),
    Utility("expand",
            options=(Option("-i"), Option("--initial"), Option("-t", _TEXT_TAB_LISTS, None, repeat=True),
                     Option("--tabs", ("3,5", "4"), True), Option("-4"), Option("-3,5"), Option("-8"),
                     Option("-0")),
            operands=_TEXT_TAB_OPERANDS, stdin=_TEXT_TAB_STDIN, fixture="text",
            extra=(("--nosuchflag",), ("-Q",), ("-t", "3", "-t", "5"), ("-t", "65536,65544"),
                   ("-t", "5,3"), ("--first-only",), ("-a",))),
    Utility("unexpand",
            options=(Option("-a"), Option("--all"), Option("--first-only"),
                     Option("-t", _TEXT_TAB_LISTS, None, repeat=True), Option("--tabs", ("3,5", "4"), True),
                     Option("-4"), Option("-3,5"), Option("-8"), Option("-0")),
            operands=_TEXT_TAB_OPERANDS, stdin=_TEXT_TAB_STDIN, fixture="text",
            extra=(("--nosuchflag",), ("-Q",), ("-t", "3", "-t", "5"), ("-a", "--first-only"),
                   ("-t", "3,/4,+2"), ("-t", "3 5"), ("-i",))),
    Utility("expr", operands=text_expr_operands(), stdin=("empty",), fixture="text",
            extra=(("--help",), ("--version",), ("--nosuchflag",), ("-x",), ("--", "--", "1"))),
    Utility("fmt",
            options=(Option("-c"), Option("--crown-margin"),
                     Option("-p", ("> ", "  >  ", "#", "", "\t"), None), Option("--prefix", ("  >  ",), True),
                     Option("-s"), Option("--split-only"), Option("-t"), Option("--tagged-paragraph"),
                     Option("-u"), Option("--uniform-spacing"),
                     Option("-w", ("10", "20", "37", "75", "80", "1", "0", "2500", "2501", "x", "-5", "3"), None),
                     Option("--width", ("39", "37"), True),
                     Option("-g", ("20", "31", "93", "x", "21", "1"), None), Option("--goal", ("31",), True),
                     Option("-37"), Option("-20"), Option("-1")),
            operands=((), ("para",), ("a.txt", "b.txt"), ("missing",), ("-",), ("dir",), ("nonl",),
                      ("empty",), ("words",), ("nonl", "para"), ("wide",), ("big",)),
            stdin=("text_fmt", "text_fmt_indent", "text_fmt_prefix", "text_fmt_tabs", "text", "empty",
                   "nonl", "wide_words", "long", "blank_runs", "spaces", "text_random_lines"),
            fixture="text", extra=(("--not-a-mode",), ("-Q",), ("-w", "20", "-g", "21"), ("-w39", "-g31"),
                                   ("-p", "> ", "-w", "20"))),
    Utility("fold",
            options=(Option("-b"), Option("--bytes"), Option("-c"), Option("--characters"), Option("-s"),
                     Option("--spaces"),
                     Option("-w", ("1", "3", "5", "8", "20", "80", "0", "+8", "-1", "nope", "2000000", "100"), None),
                     Option("--width", ("8", "4"), True), Option("-5"), Option("-20"), Option("-1")),
            operands=((), ("a.txt",), ("a.txt", "b.txt"), ("-",), ("missing",), ("dir",), ("nonl",),
                      ("empty",), ("tabs",), ("wide",), ("para",), ("a.txt", "missing", "b.txt")),
            stdin=("text", "tabs", "wide_words", "long", "nonl", "empty", "controls", "high", "edge_65537",
                   "text_fmt", "crlf", "text_random_lines", "spaces", "nul_lines"),
            fixture="text", extra=(("--nosuchflag",), ("-Q",), ("-w", "20", "-w", "60"), ("-c", "-w", "3", "tabs"))),
    Utility("grep", options=_TEXT_GREP_OPTIONS, operands=_TEXT_GREP_OPERANDS, stdin=_TEXT_GREP_STDIN,
            fixture="text", valid=text_grep_valid, extra=_TEXT_GREP_EXTRA,
            env=(("GREP_COLORS", "ms=01;31:mc=01;31:sl=:cx=:fn=35:ln=32:bn=32:se=36"),)),
    Utility("head",
            options=(Option("-n", _TEXT_COUNT_VALUES, None), Option("--lines", ("3", "+3", "-2"), True),
                     Option("-c", _TEXT_BYTE_VALUES, None), Option("--bytes", ("10", "-4"), True),
                     Option("-q"), Option("--quiet"), Option("--silent"), Option("-v"), Option("--verbose"),
                     Option("-z"), Option("--zero-terminated"), Option("-3"), Option("-0"), Option("-2")),
            operands=_TEXT_HEAD_TAIL_OPERANDS, stdin=_TEXT_HEAD_TAIL_STDIN, fixture="text",
            extra=(("--nosuchflag",), ("-Q",), ("-n", "2", "-c", "5"), ("-c", "5", "-n", "2"), ("-c", "9", "-c", "3"),
                   ("-n", "9", "-n", "3"), ("-n", "-1", "a.txt", "repeats"), ("-c", "-4", "a.txt"),
                   ("-c", "-1", "empty"), ("-c", "-3", "big"), ("-n", "-1", "big"), ("-z", "-n", "-1"))),
    Utility("tail",
            options=(Option("-n", _TEXT_COUNT_VALUES, None), Option("--lines", ("3", "+3", "+12"), True),
                     Option("-c", _TEXT_BYTE_VALUES, None), Option("--bytes", ("10", "+30"), True),
                     Option("-q"), Option("--quiet"), Option("--silent"), Option("-v"), Option("--verbose"),
                     Option("-z"), Option("--zero-terminated"), Option("-3"), Option("-0"),
                     Option("--retry"), Option("--pid", ("1", "99999"), True),
                     Option("-s", ("2", "0.5"), None), Option("--sleep-interval", ("2",), True),
                     Option("--max-unchanged-stats", ("2",), True), Option("--debug")),
            operands=_TEXT_HEAD_TAIL_OPERANDS, stdin=_TEXT_HEAD_TAIL_STDIN, fixture="text",
            extra=(("--nosuchflag",), ("-Q",), ("-n", "2", "-c", "3"), ("-c", "3", "-n", "2"), ("-c", "9", "-c", "3"),
                   ("-n", "+1", "fifteen"), ("-c", "+30", "big"), ("-c", "1", "wide"), ("-n", "5", "empty", "fifteen"),
                   ("-n", "2", "a.txt", "b.txt", "repeats"), ("-q", "-c", "5", "a.txt", "b.txt"))),
    Utility("join",
            options=(Option("-a", ("1", "2", "3", "0", "x"), None, repeat=True), Option("-e", ("EMPTY", ""), None),
                     Option("-i"), Option("--ignore-case"), Option("-j", ("1", "2", "0", "x"), None),
                     Option("-o", ("0,1.2,2.2", "auto", "1.2", "0", "2.3,1.1", "1.9", "x", "3.1", "0 1.2", "1.2,2.2,0"), None),
                     Option("-t", (":", " ", "", "ab", "\\0", "\t", "\n"), None),
                     Option("-v", ("1", "2", "3"), None, repeat=True),
                     Option("-1", ("1", "2", "0", "x", "18446744073709551615", "18446744073709551616"), None),
                     Option("-2", ("1", "2", "0", "x"), None),
                     Option("--check-order"), Option("--nocheck-order"), Option("--header"),
                     Option("-z"), Option("--zero-terminated")),
            operands=(("left", "right"), ("fleft", "fright"), ("unordered", "unordered"), ("unordered", "unordered2"),
                      ("hleft", "hright"), ("zleft", "zright"), ("-", "right"), ("left", "-"), ("left",), (),
                      ("left", "right", "a.txt"), ("missing", "right"), ("empty", "right"), ("left", "empty"),
                      ("caseleft", "caseright"), ("badrun", "goodrun"), ("goodrun", "badrun"), ("left", "left"),
                      ("dir", "right"), ("wide", "wide"), ("a.txt", "b.txt"), ("-", "-")),
            stdin=("text_join_left", "empty", "nonl", "text", "text_sorted_a", "nul"), fixture="text",
            extra=(("--nosuchflag", "left", "right"), ("-Q", "left", "right"),
                   ("-t", ":", "-1", "2", "-2", "2", "fleft", "fright"),
                   ("-o", "auto", "-t", ":", "-1", "2", "-2", "2", "-a1", "-a2", "fleft", "fright"),
                   ("-o", "auto", "--header", "-a1", "-a2", "-e", "EMPTY", "hleft", "hright"),
                   ("-o", "auto", "-z", "-a1", "-a2", "zleft", "zright"), ("-a1", "-e", "EMPTY", "-o", "0,1.2,2.3", "left", "right"),
                   ("--check-order", "-a1", "-", "goodrun"), ("-v1", "-v2", "left", "right"), ("-a1", "-a2", "left", "right"),
                   ("-z", "--nocheck-order", "-t", "", "zleft", "zleft"), ("--nocheck-order", "-t", "", "-t", "", "left", "left"),
                   ("--nocheck-order", "-t", "", "-t", ":", "left", "left"), ("--nocheck-order", "-t", ":", "-t", ",", "fleft", "fleft"))),
    Utility("line", operands=((), ("a.txt",)), stdin=("text", "empty", "nonl", "newline", "nul"), fixture="text"),
    Utility("look",
            options=(Option("-a"), Option("--alternative"), Option("-d"), Option("--alphanum"), Option("-f"),
                     Option("--ignore-case"), Option("-t", (":", " ", "\t", "a", ""), None),
                     Option("--terminate", (":",), True)),
            operands=(("ban", "dict"), ("Ber", "dict"), ("ber", "dict"), ("", "dict"), ("zzz", "dict"),
                      ("ban", "missing"), ("banana split", "dict"), ("ban:x", "dict"), ("a", "dict", "extra"),
                      ("apple", "dict_tabs"), ("ban", "dir"), ("ban", "empty"), ("ban", "unsorted"),
                      ("apple", "repeats"), ("A", "dict"), ("date", "dict"), ("d", "dict"), ("ban ana", "dict")),
            stdin=("empty",), fixture="text", extra=(("--nosuchflag", "a", "dict"), ("-Q", "a", "dict"))),
    Utility("nl",
            options=(Option("-b", ("a", "t", "n", "p^alpha", "pnowhere", "p", "x", "p^needle", "pa"), None),
                     Option("--body-numbering", ("a",), True),
                     Option("-d", ("\\:", "x", "xy", "", "abc", ":"), None), Option("--section-delimiter", ("\\:",), True),
                     Option("-f", ("a", "t", "n", "x"), None), Option("--footer-numbering", ("a",), True),
                     Option("-h", ("a", "t", "n", "x"), None), Option("--header-numbering", ("a",), True),
                     Option("-i", ("1", "2", "0", "-1", "x", "18446744073709551616"), None),
                     Option("--line-increment", ("2",), True),
                     Option("-l", ("1", "2", "3", "0", "x"), None), Option("--join-blank-lines", ("3",), True),
                     Option("-n", ("ln", "rn", "rz", "xx"), None), Option("--number-format", ("ln",), True),
                     Option("-p"), Option("--no-renumber"),
                     Option("-s", (":", "", "ab", "\t", ";"), None), Option("--number-separator", (":",), True),
                     Option("-v", ("0", "1", "5", "-3", "x", "9223372036854775807"), None),
                     Option("--starting-line-number", ("5",), True),
                     Option("-w", ("1", "3", "6", "0", "-1", "x", "20", "4"), None), Option("--number-width", ("3",), True)),
            operands=((), ("a.txt",), ("sections",), ("a.txt", "b.txt"), ("missing",), ("dir",), ("-",),
                      ("nonl",), ("empty",), ("blank_runs",), ("big",), ("wide",), ("a.txt", "missing")),
            stdin=("text", "text_sections", "blanks", "blank_runs", "empty", "nonl", "text_nl_flush", "words",
                   "many_lines", "text_fifteen", "text_random_lines"),
            fixture="text", extra=(("-Z",), ("--nosuchflag",), ("-w", "3", "-w", "6"), ("-b", "a", "-n", "rn", "nonl"),
                                   ("-w", "4", "-s", ";", "nonl"), ("-nln", "-w6"), ("-ha", "-fa", "sections"),
                                   ("-p", "-ha", "-fa", "sections"), ("-ba", "-l3"), ("-ba", "-l2"))),
    Utility("paste",
            options=(Option("-d", (",", ",:", "\\n\\t\\b", "", "\\0", "ab", "\\", "\\x", "\t", ":"), None),
                     Option("--delimiters", (",",), True), Option("-s"), Option("--serial"), Option("-z"),
                     Option("--zero-terminated")),
            operands=((), ("a.txt", "b.txt"), ("a.txt",), ("a.txt", "b.txt", "c.txt"), ("-", "-"), ("a.txt", "-"),
                      ("missing", "a.txt"), ("empty", "a.txt"), ("zleft", "zright"), ("wide", "b.txt"), ("dir",),
                      ("nonl", "a.txt"), ("a.txt", "nonl"), ("-",), ("big", "a.txt"), ("a.txt", "missing", "b.txt")),
            stdin=("text", "text_fifteen", "empty", "nonl", "nul", "text_sorted_a", "blanks"), fixture="text",
            extra=(("--nosuchflag", "a.txt"), ("-Q", "a.txt"))),
    Utility("pr",
            options=(Option("+2"), Option("+3:4"), Option("+0"), Option("+x"), Option("+1:1"),
                     Option("--pages", ("3:4", "2"), True),
                     Option("-1"), Option("-2"), Option("-3"), Option("--columns", ("2", "0", "x"), True),
                     Option("-a"), Option("--across"), Option("-c"), Option("--show-control-chars"),
                     Option("-d"), Option("--double-space"),
                     Option("-D", ("%Y", "%Y-%m-%d", "", "x"), None), Option("--date-format", ("%Y",), True),
                     Option("-e"), Option("-e4"), Option("-ex4"), Option("-e,"), Option("--expand-tabs"),
                     Option("--expand-tabs=,4"),
                     Option("-F"), Option("-f"), Option("--form-feed"),
                     Option("-h", ("HEAD", "", "a b"), None), Option("--header", ("HEAD",), True),
                     Option("-i"), Option("-i4"), Option("-ix4"), Option("--output-tabs"), Option("--output-tabs=,4"),
                     Option("-J"), Option("--join-lines"),
                     Option("-l", ("1", "2", "7", "10", "11", "12", "66", "0", "x", "8"), None),
                     Option("--length", ("7",), True),
                     Option("-m"), Option("--merge"),
                     Option("-n"), Option("-n:3"), Option("-n:"), Option("-n3"), Option("--number-lines"),
                     Option("--number-lines=:3"),
                     Option("-N", ("1", "5", "0", "-2", "x"), None), Option("--first-line-number", ("5",), True),
                     Option("-o", ("0", "4", "x", "80"), None), Option("--indent", ("4",), True),
                     Option("-r"), Option("--no-file-warnings"),
                     Option("-s"), Option("-s:"), Option("-s,"), Option("--separator"), Option("--separator=:"),
                     Option("-S"), Option("-S::"), Option("--sep-string"), Option("--sep-string=::"),
                     Option("-t"), Option("--omit-header"), Option("-T"), Option("--omit-pagination"),
                     Option("-v"), Option("--show-nonprinting"),
                     Option("-w", ("8", "20", "30", "72", "1", "0", "x", "31", "41"), None), Option("--width", ("20",), True),
                     Option("-W", ("8", "20", "1", "0", "x"), None), Option("--page-width", ("20",), True)),
            operands=((), ("a.txt",), ("pr_many",), ("a.txt", "b.txt"), ("-",), ("missing",), ("pr_ff",), ("wide",),
                      ("tabs",), ("dir",), ("big",), ("cleft", "sorted2"), ("fifteen",), ("controls" if False else "nonl",),
                      ("a.txt", "missing", "b.txt")),
            stdin=("text_pr_many", "text", "text_pr_formfeed", "empty", "nonl", "tabs", "many_lines", "edge_65537",
                   "text_fifteen", "controls", "text_random_lines", "blank_runs"),
            fixture="text", normalize=text_pr_normalize,
            extra=(("--nosuchflag",), ("-Q",), ("-t", "-l", "7", "+3:4", "pr_many"), ("-l", "12", "-D", "%Y-%m-%d", "-h", "HEAD", "pr_many"),
                   ("-l", "12", "-D", "%Y", "-F", "pr_ff"), ("-t", "-3", "-w", "31", "pr_many"), ("-t", "-3", "-w", "41", "-l", "8", "fifteen"),
                   ("-t", "-3", "-n:3", "-w", "41", "-l", "8", "fifteen"), ("-t", "-3", "-a", "-w", "31", "pr_many"),
                   ("-t", "-2", "-w", "20", "-s:", "pr_many"), ("-t", "-2", "-w", "22", "-S", "::", "pr_many"),
                   ("-t", "-2", "-J", "-w", "20", "pr_many"), ("-t", "-l", "7", "-n", "-N", "5", "+2", "pr_many"),
                   ("-t", "-2", "-n", "-w", "30", "pr_many"), ("-l", "12", "-D", "%Y", "-o", "4", "-w", "40", "pr_many"),
                   ("-t", "-W", "1", "pr_many"), ("-t", "-1", "-w", "8", "wide"), ("-t", "-1", "wide"), ("-t", "-2", "-s:", "wide"),
                   ("-m", "-t", "wide"), ("-t", "-e4", "tabs"), ("-t", "-2", "-i4", "-w", "20", "pr_many"),
                   ("-m", "-t", "-w", "30", "cleft", "sorted2"), ("-m", "-t", "-n", "-w", "36", "cleft", "sorted2"),
                   ("-t", "-l", "3", "-T", "controls"), ("-t", "-n", "-l", "3", "controls"))),
    Utility("ptx",
            options=(Option("-A"), Option("--auto-reference"), Option("-G"), Option("--traditional"),
                     Option("-F", ("++", "", "/"), None), Option("--flag-truncation", ("++",), True),
                     Option("-M", ("yy", ""), None), Option("--macro-name", ("yy",), True),
                     Option("-O"), Option("--format=roff"), Option("-T"), Option("--format=tex"),
                     Option("-R"), Option("--right-side-refs"),
                     Option("-S", ("\\n", "[.?!]", "", "x"), None), Option("--sentence-regexp", ("\\n",), True),
                     Option("-W", ("[a-z][a-z]*", "[A-Za-z]+", "", "x", "\\w+"), None),
                     Option("--word-regexp", ("[a-z][a-z]*",), True),
                     Option("-b", ("ptx_breaks", "missing", "empty"), None), Option("--break-file", ("ptx_breaks",), True),
                     Option("-f"), Option("--ignore-case"),
                     Option("-g", ("1", "5", "0", "x", "20"), None), Option("--gap-size", ("2",), True),
                     Option("-i", ("ptx_ignore", "missing", "empty"), None), Option("--ignore-file", ("ptx_ignore",), True),
                     Option("-o", ("ptx_only", "missing", "empty"), None), Option("--only-file", ("ptx_only",), True),
                     Option("-r"), Option("--references"), Option("-t"), Option("--typeset-mode"),
                     Option("-w", ("24", "40", "50", "72", "100", "0", "x", "32", "10"), None), Option("--width", ("40",), True)),
            operands=((), ("ptx_src",), ("ptx_refs",), ("ptx_src", "ptx_refs"), ("-",), ("missing",), ("empty",), ("dir",),
                      ("words",), ("wide",), ("para",)),
            stdin=("text_ptx", "text_ptx_refs", "text", "empty", "nonl", "words", "wide_words", "text_fmt",
                   "text_random_lines", "high", "tabs"),
            fixture="text", extra=(("--nosuchflag",), ("-Q",), ("-f", "-i", "ptx_ignore"), ("-i", "ptx_ignore", "-o", "ptx_only"),
                                   ("-A", "-R"), ("-r", "-R", "ptx_refs"), ("-f", "-W", "[a-z][a-z]*"), ("-g", "5", "-w", "50"),
                                   ("-F", "++", "-w", "32"), ("-A", "ptx_src", "ptx_refs"))),
    Utility("rev",
            options=(Option("-0"), Option("--zero")),
            operands=((), ("a.txt",), ("a.txt", "nonl"), ("missing",), ("dir",), ("-",), ("binary",), ("wide",),
                      ("empty",), ("a.txt", "missing", "b.txt"), ("big",)),
            stdin=("text", "empty", "nonl", "high", "long", "edge_65536", "nul", "blanks", "tabs", "crlf",
                   "text_random_lines", "edge_131072"),
            fixture="text", extra=(("--nosuchflag",), ("-Q",))),
    Utility("sed",
            options=(Option("-n"), Option("--quiet"), Option("--silent"), Option("--debug"),
                     Option("-e", _TEXT_SED_SCRIPTS, False, repeat=True, weight=3),
                     Option("--expression", ("s/a/A/", "p", "$d"), True),
                     Option("-f", ("script.sed", "script2.sed", "script3.sed", "empty", "missing", "dir"), None),
                     Option("--file", ("script.sed",), True),
                     Option("--follow-symlinks"), Option("-i"), Option("-i.bak"), Option("--in-place"),
                     Option("--in-place=.bak"),
                     Option("-l", ("1", "2", "10", "70", "0", "x"), None), Option("--line-length", ("2",), True),
                     Option("--posix"), Option("-E"), Option("-r"), Option("--regexp-extended"),
                     Option("-s"), Option("--separate"), Option("--sandbox"), Option("-u"), Option("--unbuffered"),
                     Option("-z"), Option("--null-data")),
            operands=((), ("a.txt",), ("a.txt", "b.txt"), ("-",), ("missing",), ("empty",), ("nonl",), ("dir",),
                      ("link",), ("fifteen",), ("words",), ("a.txt", "missing", "b.txt"), ("unreadable",), ("big",),
                      ("wide",), ("regex",), ("two words",)),
            stdin=("text", "text_regex", "text_fifteen", "empty", "nonl", "nul", "blanks", "text_words", "high",
                   "crlf", "edge_65537", "text_random_lines", "many_lines", "blank_runs"),
            fixture="text", valid=text_sed_valid, extra=_TEXT_SED_EXTRA),
    Utility("sort",
            options=(Option("-b"), Option("--ignore-leading-blanks"), Option("-d"), Option("--dictionary-order"),
                     Option("-f"), Option("--ignore-case"), Option("-g"), Option("--general-numeric-sort"),
                     Option("-i"), Option("--ignore-nonprinting"), Option("-M"), Option("--month-sort"),
                     Option("-h"), Option("--human-numeric-sort"), Option("-n"), Option("--numeric-sort"),
                     Option("-R"), Option("--random-sort"), Option("--random-source", ("a.txt", "/dev/zero"), True),
                     Option("-r"), Option("--reverse"),
                     Option("--sort", ("general-numeric", "human-numeric", "month", "numeric", "random", "version", "bogus"), True),
                     Option("-V"), Option("--version-sort"),
                     Option("--batch-size", ("2", "1", "x", "32"), True),
                     Option("-c"), Option("--check"), Option("--check", ("quiet", "silent", "diagnose-first", "bogus"), True),
                     Option("-C"), Option("--compress-program", ("gzip", "nosuch"), True), Option("--debug"),
                     Option("--files0-from", ("names0", "names0_missing", "missing", "-", "empty", "a.txt"), True),
                     Option("-k", _TEXT_SORT_KEYS, False, repeat=True), Option("--key", ("2", "1,1", "2n"), True),
                     Option("-m"), Option("--merge"),
                     Option("-o", ("out", "/dev/stdout", "a.txt", "dir", "missing/x"), None), Option("--output", ("out",), True),
                     Option("-s"), Option("--stable"),
                     Option("-S", ("2", "1K", "10%", "x", "1", "0"), None), Option("--buffer-size", ("2",), True),
                     Option("-t", (":", " ", "\t", "", "::", "\\0", "\\t", "\\", ","), None),
                     Option("--field-separator", (":",), True),
                     Option("-T", (".", "dir", "missing"), None), Option("--temporary-directory", ("dir",), True),
                     Option("--parallel", ("1", "2", "0", "x"), True),
                     Option("-u"), Option("--unique"), Option("-z"), Option("--zero-terminated")),
            operands=((), ("a.txt",), ("numbers",), ("unsorted",), ("repeats",), ("a.txt", "b.txt"), ("cleft", "sorted2"),
                      ("unordered", "cleft"), ("-",), ("missing",), ("empty",), ("dir",), ("big",), ("nonl",), ("fields",),
                      ("unreadable",), ("versions",), ("months",), ("human",), ("keys",), ("a.txt", "missing"),
                      ("cleft", "cleft"), ("mixed",), ("blank_runs",), ("a.txt", "a.txt")),
            stdin=("text", "numbers", "text_sort_numbers", "text_sort_human", "text_sort_version", "text_sort_month",
                   "text_sort_keys", "fields", "repeats", "mixed_case", "unsorted", "empty", "nonl", "nul", "blanks",
                   "text_sort_zero_run", "edge_65536", "many_lines", "high", "text_names0", "text_random_lines",
                   "spaces"),
            fixture="text",
            extra=(("--nosuchflag",), ("-Q",), ("-n", "-h"), ("-h", "-n"), ("-n", "-V"), ("-V", "-n"), ("-h", "--sort=numeric"),
                   ("-k1nV",), ("-k1Q",), ("-k1,",), ("-k1.",), ("-k1.0",), ("-r", "-k1n"), ("-r", "-k1"), ("-r", "-k1b"),
                   ("-f", "-k1,1r"), ("-n", "-k1r"), ("-t", ":", "-k2"), ("-t", ":", "-k3"), ("-t", "", "-k1"), ("-t", "::", "-k1"),
                   ("-t", "\\t", "-k2"), ("-t", "\\0", "-k1"), ("-t", "\\", "-k1"), ("-k1,1", "-k2n"), ("-k2", "-k1"),
                   ("-nC", "cleft"), ("-nm", "cleft", "sorted2"), ("-nms", "cleft", "sorted2"), ("-nmu", "cleft", "sorted2"),
                   ("-m", "cleft", "sorted2"), ("-m", "unsorted", "sorted2"), ("-mu", "cleft", "cleft"), ("-c", "cleft"),
                   ("-C", "unsorted"), ("-cu", "repeats"), ("-c", "-C"), ("-o", "a.txt", "a.txt"), ("-o", "a.txt", "a.txt", "a.txt"),
                   ("-z", "-t", ":", "-k2,2n"), ("-t:", "-k2,2n"), ("-t:", "-k2,2nr", "-s"), ("-t:", "-k2,2n", "-u"),
                   ("-t:", "-k3,3", "-k2,2n"), ("-t:", "-k2.2,2.7n"), ("-k3n", "big"), ("-u", "big"), ("-n", "big"),
                   ("--sort=random",), ("-g",), ("--debug",), ("--random-source=a.txt", "-R"))),
    Utility("sum",
            options=(Option("-r"), Option("-s"), Option("--sysv")),
            operands=((), ("a.txt",), ("a.txt", "b.txt"), ("-",), ("missing",), ("empty",), ("binary",), ("dir",), ("big",),
                      ("wide",), ("a.txt", "missing", "b.txt"), ("unreadable",), ("two words",)),
            stdin=("text", "empty", "text_bytes", "edge_65537", "nonl", "edge_131072", "high"), fixture="text",
            extra=(("--nosuchflag",), ("-Q",), ("-r", "-s"), ("-s", "-r"))),
    Utility("tac",
            options=(Option("-b"), Option("--before"), Option("-r"), Option("--regex"),
                     Option("-s", (":", "", "a", "ab", "alpha", "[0-9]", "a*", "^", "\n", "\\n", "x*", "e", " "), None),
                     Option("--separator", (":", ""), True)),
            operands=((), ("a.txt",), ("a.txt", "b.txt"), ("-",), ("missing",), ("nonl",), ("empty",), ("dir",), ("big",),
                      ("wide",), ("fields",), ("a.txt", "missing", "b.txt"), ("unreadable",), ("regex",)),
            stdin=("text", "empty", "nonl", "blanks", "fields", "nul_lines", "edge_65537", "many_lines", "words",
                   "text_regex", "blank_runs", "high", "text_random_lines"),
            fixture="text", extra=(("--nosuchflag",), ("-Q",), ("-r", "-s", "a\\|b"), ("-b", "-r", "-s", "[ae]"))),
    Utility("tee",
            options=(Option("-a"), Option("--append"), Option("-i"), Option("--ignore-interrupts"), Option("-p"),
                     Option("--output-error"), Option("--output-error", ("warn", "warn-nopipe", "exit", "exit-nopipe", "bogus"), True)),
            operands=((), ("out",), ("out", "out2"), ("a.txt",), ("dir",), ("unreadable",), ("-",), ("dir/nope/x",),
                      ("out", "out"), ("/dev/null",), ("a.txt", "b.txt"), ("out", "dir", "out2")),
            stdin=("text", "empty", "nonl", "edge_65537", "nul", "high", "many_lines"), fixture="text",
            extra=(("--nosuchflag",), ("-Q",), ("-ai", "/dev/null"), ("--", "out"))),
    Utility("tr",
            options=(Option("-c"), Option("-C"), Option("--complement"), Option("-d"), Option("--delete"), Option("-s"),
                     Option("--squeeze-repeats"), Option("-t"), Option("--truncate-set1")),
            operands=_TEXT_TR_OPERANDS,
            stdin=("text", "empty", "nonl", "high", "nul", "edge_65537", "spaces", "text_tr", "text_tr_refill",
                   "mixed_case", "words", "controls", "text_tr_ds", "text_bytes", "text_random_lines"),
            fixture="text", extra=(("--nosuchflag", "a", "b"), ("-Q", "a", "b"), ("-ds", "a", "b"), ("-ds", "aeiou", " "),
                                   ("-cs", "a"), ("-d", "-c", "0-9\\n"), ("-c", "o\\n", "X"), ("-cd", "a-z\\n"),
                                   ("-s", "\\000-\\377"), ("-ds", "a", "B"), ("-s", "a", "B"), ("-cs", "a", "B"),
                                   ("-ct", "a-z", "X"), ("-c", "-t", "a-z", "XY"), ("-d", "a", "A"), ("-s", "a", "b", "c"),
                                   ("-d", "abc", "-s"), ("-t", "a-z", "A-M"), ("-t", "abc", "xy"), ("-t", "abc", ""))),
    Utility("ul",
            options=(Option("-t", ("dumb", "xterm", "vt100", "bogus", "", "ansi"), None), Option("-T", ("xterm",), None),
                     Option("--terminal", ("dumb", "xterm"), True), Option("-i"), Option("--indicated")),
            operands=((), ("a.txt",), ("ul_in",), ("missing",), ("a.txt", "b.txt"), ("-",), ("dir",), ("col_in",), ("wide",)),
            stdin=("text_ul", "text", "controls", "empty", "nonl", "tabs", "text_col", "high", "edge_65536", "crlf"),
            fixture="text", extra=(("--nosuchflag",), ("-Q",))),
    Utility("uniq",
            options=(Option("-c"), Option("--count"), Option("-d"), Option("--repeated"), Option("-D"),
                     Option("--all-repeated"), Option("--all-repeated", ("none", "prepend", "separate", "bogus"), True),
                     Option("-f", ("1", "2", "9", "0", "x", "-1", "18446744073709551616"), None),
                     Option("--skip-fields", ("1",), True),
                     Option("--group"), Option("--group", ("prepend", "append", "both", "separate", "bogus"), True),
                     Option("-i"), Option("--ignore-case"), Option("-s", ("1", "3", "0", "x", "-1"), None),
                     Option("--skip-chars", ("1",), True), Option("-u"), Option("--unique"), Option("-z"),
                     Option("--zero-terminated"), Option("-w", ("1", "2", "3", "0", "x", "-1"), None),
                     Option("--check-chars", ("2",), True)),
            operands=((), ("repeats",), ("repeats", "-"), ("repeats", "out"), ("-",), ("missing",), ("repeats", "out", "a.txt"),
                      ("empty",), ("nonl",), ("dir",), ("two words",), ("unsorted",), ("mixed",), ("repeats", "dir"),
                      ("repeats", "unreadable"), ("-", "out"), ("big",), ("wide",), ("keys",)),
            stdin=("repeats", "text", "empty", "nonl", "mixed_case", "nul", "edge_65536", "text_uniq", "unsorted", "blanks",
                   "text_uniq_edge", "text_random_lines", "text_sort_keys", "blank_runs"),
            fixture="text", extra=(("--nosuchflag",), ("-Q",), ("-cd",), ("-c", "-D"), ("--group", "-c"), ("-f1", "-f2", "keys"),
                                   ("-z", "-c", "-"), ("-ic",), ("-f2", "keys"), ("-w2", "-s1"), ("-D", "-i"))),
    Utility("wc",
            options=(Option("-c"), Option("--bytes"), Option("-m"), Option("--chars"), Option("-l"), Option("--lines"),
                     Option("-L"), Option("--max-line-length"), Option("-w"), Option("--words"), Option("--debug"),
                     Option("--files0-from", ("names0", "names0_missing", "missing", "-", "empty", "a.txt", "dir"), True),
                     Option("--total", ("auto", "always", "only", "never", "o", "a", "bogus", ""), True)),
            operands=((), ("a.txt",), ("a.txt", "b.txt", "empty"), ("-",), ("missing",), ("dir",), ("big",), ("wide",),
                      ("a.txt", "missing"), ("unreadable",), ("nonl",), ("binary",), ("tabs",), ("-", "a.txt"), ("two words",),
                      ("dangling",), ("link",), ("/proc/version",)),
            stdin=("text", "empty", "nonl", "tabs", "high", "long", "edge_65535", "text_wc_boundary", "wide_words", "controls",
                   "nul_lines", "crlf", "text_names0", "text_names0_nonl", "blanks", "text_random_lines"),
            fixture="text", extra=(("--nosuchflag",), ("-Q",), ("-lwcmL", "tabs"), ("-lwmc",), ("-lm",), ("-wc",), ("-wm",),
                                   ("--total=only", "--total=always", "a.txt"), ("--total=auto", "missing", "a.txt"),
                                   ("-c", "empty"), ("-c", "/proc/version"), ("-L", "a.txt", "tabs"))),
)
