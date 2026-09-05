"""Builtin operand grammars composed with data, quoting and shell state."""

import shlex


MODES = ("bash", "posix", "dash")


def listing(rng):
    byte = rng.choice((chr(rng.randrange(1, 128)), "é", "λ", "🌙", "\u0080"))
    value = rng.choice((byte, "head" + byte, byte + "tail", "a " + byte + "z",
                        "~start", "#start", "", "simple"))
    command = rng.choice(("set", "declare", "declare -p zz", "export -p", "readonly -p"))
    setup = rng.choice(("zz=", "export zz=", "readonly zz="))
    pattern = "^zz=" if command in ("set", "declare") else " zz="
    script = (setup + shlex.quote(value) + "\n" + command +
              " | /bin/grep " + shlex.quote(pattern) + "\n")
    return "builtin-listing", ("bash", "posix"), script


def read_fields(rng):
    text = rng.choice((" a  b c ", "a::b:", ":a:b", "a\\ b c", "\\", "", "one\ntwo"))
    text += rng.choice(("", "\n"))
    separator = rng.choice((" ", ":", " :", ""))
    names = rng.choice(("a", "a b", "a b c"))
    option = rng.choice(("", "-r"))
    script = ("printf '%s' " + shlex.quote(text) + " > feed\n" +
              "a=old; b=old; c=old\nIFS=" + shlex.quote(separator) +
              f" read {option} {names} < feed\n" +
              "s=$?\nprintf '%s:<%s>:<%s>:<%s>\\n' \"$s\" \"$a\" \"$b\" \"$c\"\n")
    return "builtin-read-fields", MODES, script


def printf_formats(rng):
    form = rng.choice(("%s", "<%.3s>", "%d", "%u", "%x", "%o", "%b", "%c",
                       "%f", "%g", "%a", "%A", "%08d", "%-6s", "%#x"))
    values = ("", "a b", "007", "0x10", "-1", "+7", " 2", "'A", "bad", "a\\nb", "12tail",
              "1e3", "0x1p2", "-0", ".5", " ", "08")
    operands = rng.sample(values, rng.randrange(1, 4))
    script = ("printf " + shlex.quote(form + "|") + " " +
              " ".join(shlex.quote(word) for word in operands) +
              "\ns=$?\nprintf '\\nstatus:%s\\n' \"$s\"\n")
    # Default Bash on x86 uses an 80-bit long double and consequently a
    # different legal leading hex digit/exponent. Keep byte-exact comparisons
    # for its POSIX mode and dash; the Bash family below checks numeric
    # round-tripping independently instead of treating spellings as equal.
    modes = ("posix", "dash") if form in ("%a", "%A") else MODES
    return "builtin-printf-operands", modes, script


def printf_hex_roundtrip(rng):
    form = rng.choice(("%a", "%A", "%+.13a", "%#.13A"))
    # Exactly representable values exercise representation, sign and parser
    # round-tripping without claiming binary64 has Bash's extended precision.
    value = rng.choice(("0", "-0", "1.5", "08", "0x1p2", "-0x1.8p-12",
                        "0x1.fffffffffffffp+20", "0x1p-1022"))
    script = ("encoded=$(printf " + shlex.quote(form) + " " + shlex.quote(value) +
              ")\ns=$?\nprintf 'status:%s:value:%.17g\\n' \"$s\" \"$encoded\"\n")
    return "builtin-printf-hex-roundtrip", MODES, script


def getopts_state(rng):
    spec = rng.choice(("ab:", ":ab:", "a:b", ":a:b"))
    words = rng.choice((("-a", "-b", "value", "tail"), ("-abvalue",),
                        ("-z", "-a"), ("-b",), ("--", "-a"), ("operand", "-a")))
    start = rng.choice((1, 1, 2))
    script = ("set -- " + " ".join(shlex.quote(word) for word in words) + "\n" +
              f"OPTIND={start}\nn=0\nwhile getopts {shlex.quote(spec)} option; do\n" +
              "printf '<%s>:<%s>:%s\\n' \"$option\" \"${OPTARG-unset}\" \"$OPTIND\"\n" +
              "n=$((n+1)); [ \"$n\" -lt 8 ] || break\ndone\n" +
              "printf 'end:%s:%s:<%s>\\n' \"$OPTIND\" \"$option\" \"${OPTARG-unset}\"\n")
    return "builtin-getopts-state", MODES, script


def getopts_reset(rng):
    reset = rng.choice(("OPTIND=0", "OPTIND=1", "OPTIND=2", "unset OPTIND", ":"))
    word = rng.choice(("-ab", "-abc", "-abvalue"))
    spec = rng.choice(("abc", "ab:", ":ab:"))
    script = (f"set -- {word} -c tail\ngetopts {spec} o\n" +
              "printf '%s:%s:<%s>\\n' \"$o\" \"$OPTIND\" \"${OPTARG-unset}\"\n" +
              reset + f"\ngetopts {spec} o\n" +
              "printf '%s:%s:<%s>\\n' \"$o\" \"$OPTIND\" \"${OPTARG-unset}\"\n")
    return "builtin-getopts-reset", MODES, script


def getopts_scope(rng):
    declaration = rng.choice(("local OPTIND=1", "local OPTIND=2", "local OPTIND", ":"))
    parameters = rng.choice(("explicit", "function", "set"))
    calls = rng.randrange(1, 4)
    words = rng.choice(("-xy", "-x -y"))
    setup = f"set -- {words}\n" if parameters == "set" else ""
    operands = " " + words if parameters == "explicit" else ""
    invocation = "f " + words if parameters == "function" else "f"
    script = ("set -- -ab -c\ngetopts abc o\nf() {\n" + declaration + "\n" + setup +
              ("getopts xy o" + operands + "\n") * calls +
              "printf 'in:%s:%s:<%s>\\n' \"$o\" \"$OPTIND\" \"${OPTARG-unset}\"\n}\n" +
              invocation + "\ngetopts abc o\n" +
              "printf 'out:%s:%s:<%s>\\n' \"$o\" \"$OPTIND\" \"${OPTARG-unset}\"\n")
    return "builtin-getopts-scope", MODES, script


GENERATORS = (listing, read_fields, printf_formats, printf_hex_roundtrip,
              getopts_state, getopts_reset, getopts_scope)


def cases(rng, budget):
    order = list(GENERATORS)
    rng.shuffle(order)
    for index in range(budget):
        yield order[index % len(order)](rng)
