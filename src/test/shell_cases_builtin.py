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


def query_namespaces(rng):
    """Compose namespace precedence with PATH and per-name/aggregate status."""
    # Verbose type deliberately retains dash's wording/status; terse queries
    # share Bash's surface. Absolute PATH entries avoid POSIX path spelling.
    query = rng.choice(("type -t", "type -at", "type -p", "type -P",
                        "command -v", "command -pv"))
    state = rng.choice(("", "mw_name() { :; }",
                        "shopt -s expand_aliases; alias mw_name='printf alias'",
                        "mw_name() { :; }; shopt -s expand_aliases; alias mw_name=echo",
                        "hash -p /bin/true mw_name"))
    # Standard-path lookup deliberately ignores the shell's hash override.
    if query == "command -pv" and state.startswith("hash"):
        query = "command -v"
    names = rng.choice(("mw_name", "echo printf", "if case", "mw_missing",
                        "mw_missing mw_name", "mw_name mw_missing", "-- '' mw_name"))
    path = rng.choice(('"$PWD/first:$PWD/second"', '"$PWD/second:$PWD/first"',
                       "/does/not/exist"))
    script = ("/bin/mkdir first second\n"
              "printf '#!/bin/sh\\nexit 0\\n' > first/mw_name\n"
              "/bin/cp first/mw_name second/mw_name\n"
              "/bin/chmod +x first/mw_name second/mw_name\n" +
              "PATH=" + path + "\n" + state + "\n" + query + " " + names +
              "\nprintf 'status:%s\\n' \"$?\"\n")
    return "builtin-query-namespaces", ("bash", "posix"), script


def declaration_lifecycle(rng):
    """Local and declare share storage, but not scope and failure policies."""
    setup = rng.choice(("mw_value=OLD", "export mw_value=OLD",
                        "readonly mw_value=OLD", "declare -i mw_value=17"))
    command = rng.choice(("local", "declare", "typeset"))
    flags = rng.choice(("", "-x", "+x", "-i", "-r"))
    # Compound/inherited-integer += and local -g have separate compatibility
    # policies; this family varies the shared scalar assignment lifecycle.
    operand = rng.choice(("mw_value", "mw_value=23"))
    if not setup.startswith("readonly") and rng.getrandbits(1):
        operand += " mw_tail=end"
    inherit = rng.choice(("shopt -u localvar_inherit", "shopt -s localvar_inherit"))
    report = "declare -p mw_value mw_tail; printf 'report:%s\\n' \"$?\""
    script = (setup + "\n" + inherit + "\nf() {\n" + command + " " + flags + " " +
              operand + "\nprintf 'declaration:%s\\n' \"$?\"\n" + report +
              "\n}\nf\n" + report + "\n")
    return "builtin-declaration-lifecycle", ("bash", "posix"), script


def inventory_state(rng):
    """Sort live names after removal/reinsertion, with independent attributes."""
    functions = bool(rng.getrandbits(1))
    names = ["mw_" + name for name in rng.sample(("z", "a", "middle", "aa", "B", "_"), 5)]
    removed = rng.choice(names)
    if functions:
        setup = "\n".join(name + "() { :; }" for name in names)
        setup += "\nunset -f " + removed
        if rng.getrandbits(1):
            setup += "\n" + removed + "() { echo replaced; }"
        marked = rng.choice([name for name in names if name != removed])
        setup += "\nexport -f " + marked
        command = rng.choice(("declare -F", "declare -Fx", "export -pf"))
        if command == "export -pf":
            # Function formatting differs; exercise the emitted definition's
            # semantics and attributes by round-tripping it, not its layout.
            command = ('saved=$(export -pf); unset -f ' + marked +
                       '; eval "$saved"; ' + marked + '; declare -Fx')
            # Bash POSIX command substitution drops the export metadata.
            return "builtin-inventory-state", ("bash",), setup + "\n" + command + "\n"
        return "builtin-inventory-state", ("bash", "posix"), setup + "\n" + command + "\n"
    setup = "\n".join(name + "='two words'" for name in names)
    setup += "\nunset " + removed
    if rng.getrandbits(1):
        setup += "\n" + removed + "=replaced"
    marked = rng.choice([name for name in names if name != removed])
    setup += "\n" + rng.choice(("export ", "readonly ")) + marked
    command = rng.choice(("declare -p", "export -p", "readonly -p"))
    return "builtin-inventory-state", ("bash", "posix"), (
        setup + "\n" + command + " | /bin/grep -E '^(declare [^ ]+|export|readonly) mw_'\n")


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


def read_ifs_snapshot(rng):
    """Classify once, even when the first assignment replaces IFS itself."""
    separator = rng.choice(("", ":", " \t:", "," * 4097 + ":", "é:"))
    text = rng.choice(("one:two::three:", " first\tsecond:third ",
                       "left\\:quoted:right", "aébé:c", "", "tail\\"))
    text += rng.choice(("", "\n"))
    first = rng.choice(("a", "IFS"))
    raw = rng.choice(("", "-r"))
    setup = ("unset IFS" if rng.randrange(5) == 0 else
             "IFS=" + shlex.quote(separator))
    script = ("printf '%s' " + shlex.quote(text) + " > feed\n" +
              "a=old; b=old; c=old\n" + setup + "\n" +
              f"read {raw} {first} b c < feed\ns=$?\n" +
              "printf '%s:<%s>:<%s>:<%s>\\n' \"$s\" \"$" + first +
              "\" \"$b\" \"$c\"\n")
    # dash disables splitting on an invalid multibyte IFS in the C locale;
    # Bash and our byte-oriented reader instead classify its individual bytes.
    modes = ("bash", "posix") if "é" in separator else MODES
    return "builtin-read-ifs-snapshot", modes, script


def read_limit_state(rng):
    """Compose Bash's byte/count/delimiter options over one retained fd."""
    shape = rng.randrange(4)
    attached = bool(rng.getrandbits(1))
    descriptor = rng.choice((3, 9, 10, 11, 31, 62))

    if shape < 2:
        count = rng.choice((0, 1, 2, 3, 7))
        exact = shape == 1
        option = ("-N" if exact else "-n") + str(count)
        if not attached:
            option = ("-N " if exact else "-n ") + str(count)
        # -N consumes newlines and backslashes as bytes. -n stops at newline
        # and gives back a backslash-newline unless -r is also present.
        text = rng.choice(("ab:cdef", "a\nbcdef", "a\\\nbcdef"))
        script = (
            "printf '%s' " + shlex.quote(text) + " > feed\n"
            "exec 3<feed\n"
            f"IFS= read -r -u3 {option} first\n"
            "one=$?\n"
            f"IFS= read -r -u 3 {option} second\n"
            "two=$?\n"
            "printf '%s:<%s>:%s:<%s>\\n' \"$one\" \"$first\" \"$two\" \"$second\"\n"
        )
    elif shape == 2:
        delimiter = rng.choice((":", ",", "|"))
        option = "-d" + delimiter if attached else "-d " + shlex.quote(delimiter)
        text = "aa" + delimiter + "bb" + delimiter + "cc"
        script = (
            "printf '%s' " + shlex.quote(text) + " > feed\n"
            "exec 3<feed\n"
            f"IFS= read -r -u3 {option} first\n"
            "one=$?\n"
            f"IFS= read -r -u 3 {option} second\n"
            "two=$?\n"
            "printf '%s:<%s>:%s:<%s>\\n' \"$one\" \"$first\" \"$two\" \"$second\"\n"
        )
    else:
        # An empty -d operand means NUL. The script contains an escape, not a
        # NUL byte, so it remains a valid command string for every runner.
        # Quotes are gone before read sees argv, so an empty value necessarily
        # occupies the next argument; there is no attached spelling of it.
        option = "-d ''"
        script = (
            "printf 'aa\\0bb\\0cc' > feed\n"
            "exec 3<feed\n"
            f"IFS= read -r -u3 {option} first\n"
            "one=$?\n"
            f"IFS= read -r -u 3 {option} second\n"
            "two=$?\n"
            "printf '%s:<%s>:%s:<%s>\\n' \"$one\" \"$first\" \"$two\" \"$second\"\n"
        )

    # dash has only its portable -r surface; Bash and Bash POSIX mode share
    # these documented builtin extensions.
    # Vary the actual fd in both opens and retained reads; command-only
    # execution cannot discover script-input ownership bugs. Keep 63 out of
    # this differential family: Bash's private reader occupies it under the
    # runner's fd limit and read -u63 can consume its script buffer instead
    # of the redirected data. Startup tests check private-fd collisions with
    # explicit expected results, independent of that implementation detail.
    script = script.replace("exec 3<", f"exec {descriptor}<")
    script = script.replace("-u3 ", f"-u{descriptor} ")
    script = script.replace("-u 3 ", f"-u {descriptor} ")
    return "builtin-read-limit-state", ("bash", "posix"), script


def read_array_state(rng):
    separator = rng.choice((":", ",", " ", " :"))
    text = rng.choice(("a:b::c", ":a:b:", " a  b c ", "one,two,,four"))
    option = rng.choice(("-a values", "-avalues"))
    script = (
        "printf '%s\\n' " + shlex.quote(text) + " > feed\n"
        "values=(old stale)\n"
        "IFS=" + shlex.quote(separator) + f" read -r {option} < feed\n"
        "s=$?\nprintf '%s:%s:<%s>:<%s>:<%s>:<%s>\\n' \"$s\" "
        "\"${#values[@]}\" \"${values[0]-}\" \"${values[1]-}\" "
        "\"${values[2]-}\" \"${values[3]-}\"\n"
    )
    return "builtin-read-array-state", ("bash", "posix"), script


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


def printf_dynamic_fields(rng):
    """Drive width/precision argument consumption, signs and format reuse."""
    kind = rng.randrange(4)
    width = rng.choice((-12, -5, 0, 1, 7, 16))
    precision = rng.choice((-3, -1, 0, 1, 4, 9))

    if kind == 0:
        form = "<%*.*s>|"
        values = (str(width), str(precision),
                  rng.choice(("abcdef", "a b c", "")))
    elif kind == 1:
        form = "<%0*.*d>|"
        values = (str(width), str(precision),
                  rng.choice(("-17", "0", "42", "007")))
    elif kind == 2:
        form = "<%#*.*x>|"
        values = (str(width), str(precision),
                  rng.choice(("0", "15", "255", "0x123")))
    else:
        form = "<%*.*b>|"
        values = (str(width), str(precision),
                  rng.choice(("a\\nb", "tab\\there", "", "abcdef")))

    operands = values * rng.choice((1, 2, 3))
    script = (
        "printf " + shlex.quote(form) + " " +
        " ".join(shlex.quote(word) for word in operands) +
        "\ns=$?\nprintf '\\nstatus:%s\\n' \"$s\"\n"
    )
    return "builtin-printf-dynamic-fields", MODES, script


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


def printf_collectors(rng):
    """The output and %b field stores must never overwrite one another."""
    value = rng.choice(("", "a\\nb", "x\\ty", "\\141\\142", "x\\cy", "tail\\"))
    width, precision = rng.choice((-20, -1, 0, 2, 12)), rng.choice((-1, 0, 1, 7))
    form = "prefix:<%*.*b>:<%s>|"
    operands = [str(width), str(precision), value, "kept"] * rng.randrange(1, 4)
    return "builtin-printf-collectors", ("bash", "posix"), (
        "out=before\nprintf -v out " + shlex.quote(form) + " " +
        " ".join(map(shlex.quote, operands)) +
        "\nprintf 'first:%s:<%s>\\n' \"$?\" \"$out\"\n"
        "printf -v out '%b:%s' 'z\\n' again\nprintf 'next:<%s>\\n' \"$out\"\n")


def option_walk(rng):
    """Bundled letters, attached operands, --, and the next option word."""
    command = rng.choice(("cd", "pwd", "exec", "shopt", "hash", "enable"))
    if command == "cd":
        words = rng.choice(("-LP dir", "-PL -- dir", "-- dir", "-L -P dir", "-Lz dir"))
        script = f"cd {words}\nprintf 'status:%s\\n' \"$?\"\npwd\n"
    elif command == "pwd":
        words = rng.choice(("-LP", "-PL -- -z", "-- -z", "-L -P", "-Lz"))
        script = f"pwd {words}\nprintf 'status:%s\\n' \"$?\"\n"
    elif command == "exec":
        words = rng.choice(("-a named", "-anamed", "-la named", "-a first -a second",
                            "-a '' --", "--", "-a", "-az -Q"))
        tail = "" if words == "-a" else " /bin/bash -c 'printf \"zero:<%s> argc:%s\\n\" \"$0\" \"$#\"' x y"
        script = f"exec {words}{tail}\necho survived\n"
    elif command == "shopt":
        words = rng.choice(("-sq extglob", "-s -q -- extglob", "-uq extglob", "-qz extglob"))
        script = f"shopt {words}\nprintf 'status:%s\\n' \"$?\"\nshopt -q extglob; echo $?\n"
    elif command == "hash":
        words = rng.choice(("-p /bin/true", "-p/bin/true", "-rp /bin/true",
                            "-p /bin/false -p/bin/true", "-p /bin/true --", "-Q"))
        script = f"hash {words} named\nprintf 'status:%s\\n' \"$?\"\nhash -t named\n"
    else:
        words = rng.choice(("-n true", "-- true", "-n -- true", "-Q true"))
        script = f"enable {words}\nprintf 'status:%s\\n' \"$?\"\ntype -t true\n"
    return "builtin-option-walk-" + command, ("bash", "posix"), script


def mapfile_records(rng):
    """Delimiter positions around the short scan and input refill fences."""
    length = rng.choice((0, 1, 15, 16, 17, 31, 32, 33, 4095, 4096, 4097))
    delimiter = rng.choice(("\n", ":", "\0"))
    records = ["x" * length, "", "tail"]
    payload = delimiter.join(records) + (delimiter if rng.choice((False, True)) else "")
    encoded = payload.replace("\0", "\\000")
    trim = rng.choice(("", "-t"))
    origin = rng.choice(("", "-O 3"))
    skip = rng.choice((0, 1))
    options = f"{trim} {origin} -s {skip} -d " + shlex.quote("" if delimiter == "\0" else delimiter)
    return "builtin-mapfile-records", ("bash", "posix"), (
        "printf %b " + shlex.quote(encoded) + " > records\na=(old old old old old)\n"
        f"mapfile {options} a < records\n"
        "printf 'status:%s n:%s\\n' \"$?\" \"${#a[@]}\"; printf '<%s>\\n' \"${a[@]}\"\n")


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


GENERATORS = (listing, query_namespaces, declaration_lifecycle, inventory_state,
              read_fields, read_ifs_snapshot, read_limit_state, read_array_state,
              printf_formats, printf_hex_roundtrip, printf_dynamic_fields,
              printf_collectors, option_walk, mapfile_records,
              getopts_state, getopts_reset, getopts_scope)


def cases(rng, budget):
    order = list(GENERATORS)
    rng.shuffle(order)
    for index in range(budget):
        yield order[index % len(order)](rng)
