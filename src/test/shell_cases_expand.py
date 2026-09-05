"""Deterministic generated variable, expansion and arithmetic cases.

The shared runner owns isolation, limits, comparison and shrinking.  This
module only describes small semantic programs; each family varies operands,
state and quoting instead of multiplying a fixed assertion into a large test
count.
"""


def quote(text):
    return "'" + text.replace("'", "'\"'\"'") + "'"


def program(*lines):
    """Keep setup, mutation and observation separable for line shrinking."""
    return "\n".join(lines) + "\n"


def parameter_default(rng):
    state = rng.choice(("unset", "empty", "value"))
    operation = rng.choice(("-", ":-", "+", ":+", "=", ":="))
    quoted = rng.choice((False, True))
    value = rng.choice(("alpha", "a b", "a::b", "abcabc"))
    fallback = rng.choice(("fallback", "two words", "q:r"))
    setup = "unset x"
    if state == "empty":
        setup = "x="
    elif state == "value":
        setup = "x=" + quote(value)
    form = "${x" + operation + "$fallback}"
    if quoted:
        form = '"' + form + '"'
    script = program(setup, f"fallback={quote(fallback)}", "IFS=:",
                     f"set -- {form}",
                     "printf 'argc=%s' \"$#\"; for item do printf '<%s>' \"$item\"; done",
                     "printf '|x=<%s>\\n' \"${x-unset}\"")
    return "parameter-default", ("bash", "posix", "dash"), script


def parameter_trim(rng):
    value = rng.choice(("abcabc", "prefix-middle-suffix", "a/b/c", "000123"))
    operation = rng.choice(("#", "##", "%", "%%"))
    pattern = rng.choice(("a*", "*c", "prefix-*", "*/", "0*", "?"))
    script = program(f"x={quote(value)}", f"p={quote(pattern)}",
                     f"printf '<%s>|<%s>\\n' \"${{x{operation}$p}}\" \"$x\"")
    return "parameter-trim", ("bash", "posix", "dash"), script


def splitting_and_glob(rng):
    separator = rng.choice((":", ",", " "))
    value = rng.choice(("a::b c", "a,b,,c", "  a  b ", "a.txt:b.txt"))
    tail = rng.choice(("*.txt", "a*", "no-match-*"))
    script = program(f"IFS={quote(separator)}", f"x={quote(value)}",
                     f"set -- $x {tail}",
                     "printf 'argc=%s' \"$#\"; for item do printf '<%s>' \"$item\"; done",
                     "echo")
    return "split-glob", ("bash", "posix", "dash"), script


def arithmetic(rng):
    left = rng.randrange(-40, 41)
    right = rng.randrange(1, 16)
    third = rng.randrange(0, 8)
    operator = rng.choice(("+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^"))
    # Shifts need a small nonnegative count; every other operation accepts the
    # same bounded right operand, keeping the oracle away from overflow/zero.
    operand = third if operator in ("<<", ">>") else right
    expression = f"(x {operator} {operand}) + (y > 2)"
    script = program(f"x={left}", f"y={right}", f"r=$(({expression}))",
                     "printf 'r=%s x=%s y=%s\\n' \"$r\" \"$x\" \"$y\"")
    return "arithmetic", ("bash", "posix", "dash"), script


def arithmetic_side_effect(rng):
    start = rng.randrange(0, 8)
    limit = rng.randrange(start, start + 8)
    truth = rng.choice((0, 1))
    expression = (f"({truth} && (i += 3)) || (i += 2), "
                  f"i < {limit} ? i + 5 : i - 1")
    script = program(f"i={start}", f"r=$(({expression}))",
                     "printf 'r=%s i=%s\\n' \"$r\" \"$i\"")
    return "arithmetic-effects", ("bash", "posix"), script


def arithmetic_comma(rng):
    first = rng.randrange(0, 8)
    second = rng.randrange(1, 8)
    sequence = f"i += {first}, i += {second}, i * 2"
    context = rng.choice(("group", "value", "conditional", "short-circuit"))
    setup = "i=0"
    if context == "value":
        setup += "; expression=" + quote(sequence)
        expression = "expression"
    elif context == "conditional":
        expression = f"{rng.randrange(0, 2)} ? {sequence} : 99"
    elif context == "short-circuit":
        expression = f"{rng.randrange(0, 2)} && ({sequence})"
    else:
        expression = "(" * rng.randrange(1, 5) + sequence
        expression += ")" * (len(expression) - len(expression.lstrip("(")))
    return "arithmetic-comma-" + context, ("bash", "posix"), program(
        setup, "r=$((" + expression + "))",
        "printf 'r=%s i=%s\\n' \"$r\" \"$i\"")


def arithmetic_array(rng):
    """Exercise one prepared indexed target through every lvalue form."""
    left, right, created = sorted(rng.sample(range(1, 12), 3))
    first = rng.randrange(1, 10)
    second = rng.randrange(1, 10)
    made = rng.randrange(1, 10)
    delta = rng.randrange(1, 6)
    operator = rng.choice(("+=", "*=", "^=", "<<="))
    script = program(
        f"a=([{left}]={first} [{right}]={second}); b=([1]={right})",
        f"i={left}; j=1; new={created}",
        "read=$((a[i]))",
        f"write=$((a[i]={made}))",
        f"compound=$((a[i++]{operator}{delta})); compound_i=$i; i={left}",
        f"pre=$((++a[i++])); pre_i=$i; i={left}",
        f"post=$((a[i++]++)); post_i=$i; i={left}",
        "nested=$((a[b[j]]))",
        f"create=$((a[new]={made + delta}))",
        "held=$i; left_short=$((0 && a[i++]++)); right_short=$((1 || a[i++]++))",
        "printf 'read=%s write=%s compound=%s:%s pre=%s:%s post=%s:%s nested=%s create=%s ' "
        '"$read" "$write" "$compound" "$compound_i" "$pre" "$pre_i" '
        '"$post" "$post_i" "$nested" "$create"',
        "printf 'i=%s held=%s short=%s:%s values=%s:%s:%s\\n' "
        '"$i" "$held" "$left_short" "$right_short" '
        '"${a[i]}" "${a[b[j]]}" "${a[new]}"')
    return "arithmetic-array", ("bash", "posix"), script


def substitution_status(rng):
    prior = rng.choice(("true", "false"))
    first = rng.choice(("true", "false"))
    second = rng.choice(("true", "false"))
    script = program(prior, f"a=$({first}) b=$? c=$({second}) d=$?",
                     "printf 'b=%s d=%s status=%s\\n' \"$b\" \"$d\" \"$?\"")
    return "substitution-status", ("bash", "posix"), script


def indexed_array(rng):
    first, second = sorted(rng.sample(range(0, 8), 2))
    value1 = rng.choice(("alpha", "a b", ""))
    value2 = rng.choice(("beta", "q:r", "two words"))
    action = rng.choice(("write", "append", "unset"))
    if action == "write":
        mutate = f"a[{second + 2}]={quote(value2)}"
    elif action == "append":
        mutate = f"a[{first}]+={quote(value2)}"
    else:
        mutate = f"unset 'a[{first}]'"
    script = program("declare -a a", f"a[{first}]={quote(value1)}",
                     f"a[{second}]={quote(value2)}", mutate,
                     "printf 'n=%s keys=' \"${#a[@]}\"",
                     "printf '<%s>' \"${!a[@]}\"; printf '|values='",
                     "printf '<%s>' \"${a[@]}\"; echo")
    return "indexed-array", ("bash", "posix"), script


def associative_array(rng):
    keys = rng.sample(("x", "y", "long-key", "2", "a b"), 3)
    values = rng.sample(("one", "two words", "", "q:r", "last"), 3)
    action = rng.choice(("write", "append", "unset"))
    setup = [f"m[{quote(key)}]={quote(value)}"
             for key, value in zip(keys[:2], values[:2])]
    if action == "write":
        mutate = f"m[{quote(keys[2])}]={quote(values[2])}"
    elif action == "append":
        mutate = f"m[{quote(keys[0])}]+={quote(values[2])}"
    else:
        mutate = f"unset 'm[{keys[0]}]'"
    # Associative key iteration order is not specified. Query the generated
    # keys directly and use only the count as the aggregate observation.
    observations = "".join(
        f"printf '|{i}=<%s>' \"${{m[{quote(key)}]-unset}}\"; "
        for i, key in enumerate(keys)) + "echo"
    script = program("declare -A m", *setup, mutate,
                     "printf 'n=%s' \"${#m[@]}\"", observations)
    return "associative-array", ("bash", "posix"), script


def nameref(rng):
    form = rng.choice(("scalar", "indexed", "element", "chain"))
    action = rng.choice(("write", "append", "unset"))
    if form == "scalar":
        setup = "target=old; declare -n n=target"
        read = 'printf "target=<%s> n=<%s>\\n" "$target" "${n-unset}"'
    elif form == "indexed":
        setup = "a=([1]=old [3]=keep); declare -n n=a"
        read = 'printf "n=%s one=<%s> three=<%s>\\n" "${#a[@]}" "${a[1]-}" "${a[3]-}"'
    elif form == "element":
        setup = "a=([1]=old [3]=keep); declare -n n='a[1]'"
        read = 'printf "n=%s one=<%s> three=<%s>\\n" "${#a[@]}" "${a[1]-}" "${a[3]-}"'
    else:
        setup = "target=old; declare -n middle=target; declare -n n=middle"
        read = 'printf "target=<%s> n=<%s>\\n" "$target" "${n-unset}"'
    if action == "write":
        mutate = "n=new"
    elif action == "append":
        mutate = "n+=tail"
    else:
        mutate = "unset n"
    return "nameref", ("bash", "posix"), program(setup, mutate, read)


def local_scope(rng):
    outer = rng.choice(("outer", "a b", ""))
    inner = rng.choice(("inner", "q:r", "two words"))
    action = rng.choice(("assign", "unset", "readonly", "bare"))
    if action == "assign":
        body = f"local x={quote(inner)}; x=changed; printf 'in=<%s>\\n' \"$x\""
    elif action == "unset":
        body = f"local x={quote(inner)}; unset x; printf 'in=<%s>\\n' \"${{x-unset}}\""
    elif action == "readonly":
        body = f"local x={quote(inner)}; readonly x; printf 'in=<%s>\\n' \"$x\""
    else:
        body = "local x; printf 'in=<%s>\\n' \"${x-unset}\"; local x; x=inner"
    script = program(f"x={quote(outer)}", f"f() {{ {body}; }}", "f",
                     "printf 'out=<%s>\\n' \"$x\"")
    return "local-scope", ("bash", "posix", "dash"), script


def indirect_special(rng):
    special = rng.choice(("#", "?"))
    positional = rng.randrange(0, 4)
    parameters = " ".join(quote(f"p{i}") for i in range(positional))
    prior = rng.choice(("true", "false"))
    script = program(f"set -- {parameters}", prior, "printf 'before|'",
                     f"printf '<%s>' \"${{!{special}}}\"",
                     "printf '|after\\n'")
    return "indirect-special", ("bash", "posix"), script


def substring(rng):
    value = rng.choice(("", "a", "a b::c", "0123456789", "a" * 33))
    offset = rng.choice((-40, -10, -2, -1, 0, 1, 3, 10, 40,
                         -(1 << 63), (1 << 63) - 1))
    length = rng.choice((None, -40, -2, -1, 0, 1, 3, 40))
    suffix = "" if length is None else rng.choice((":n", ":", ": "))
    form = "${x: i" + suffix + "}"
    if rng.choice((False, True)):
        form = '"' + form + '"'
    return "substring-boundaries", ("bash", "posix"), program(
        f"x={quote(value)}; i={offset}; n={length or 0}; IFS=:",
        "set -- " + form,
        "printf 'n=%s' \"$#\"; printf '<%s>' \"$@\"; echo")


def sequence_slice(rng):
    kind = rng.choice(("positional", "dense", "sparse"))
    values = rng.sample(("", "a", "two words", "x:y", "*", "last"),
                        rng.randrange(0, 6))
    if kind == "positional":
        setup = "set -- " + " ".join(quote(value) for value in values)
        name = rng.choice(("@", "*"))
    else:
        indices = (list(range(len(values))) if kind == "dense" else
                   sorted(rng.sample(range(1, 13), len(values))))
        setup = "a=(" + " ".join(f"[{index}]={quote(value)}"
                                  for index, value in zip(indices, values)) + ")"
        name = "a[" + rng.choice(("@", "*")) + "]"
    offset = rng.choice((-20, -5, -2, -1, 0, 1, 2, 5, 12, 20,
                         -(1 << 63), (1 << 63) - 1))
    length = rng.choice((None, -2, 0, 1, 2, 5, 20))
    suffix = "" if length is None else rng.choice((":n", ":", ": "))
    form = "${" + name + ": i" + suffix + "}"
    if rng.choice((False, True)):
        form = '"' + form + '"'
    # $0 differs by input transport. Normalize it only in the observed words,
    # after expansion, so offset zero can still exercise its inclusion.
    return "sequence-slice-" + kind, ("bash", "posix"), program(
        setup, f"i={offset}; n={length or 0}; IFS={quote(rng.choice((' ', ':', '')))}",
        "observe() { printf 'n=%s' \"$#\"; for v do",
        "if [ \"$v\" = \"$0\" ]; then v=argv-zero; fi; printf '<%s>' \"$v\"; done; echo; }",
        "observe " + form)


def array_transform(rng):
    values = rng.sample(("", "a", "a b", "aa", "x:y", "AB"), rng.randrange(0, 5))
    operation = rng.choice(("#a*", "%*a", "/a/", "//a/x", "^^", ",,"))
    form = "${a[" + rng.choice(("@", "*")) + "]" + operation + "}"
    quoted = rng.choice((False, True))
    separator = rng.choice((" ", ":", ""))
    # Bash 5.3.15 leaks its internal SOH quote marker and joins with spaces
    # for unquoted array #/% trimming under an empty IFS. Do not make that
    # oracle defect Moonwater's specification; cover empty IFS trim quoted,
    # and retain unquoted empty IFS for replacement and case conversion.
    if not separator and operation[0] in "#%":
        quoted = True
    if quoted:
        form = '"' + form + '"'
    return "array-transform-fields", ("bash", "posix"), program(
        "a=(" + " ".join(quote(value) for value in values) + ")",
        "IFS=" + quote(separator),
        "set -- " + form,
        "printf 'n=%s' \"$#\"; for v do printf '<%s>' \"$v\"; done; echo")


def sequence_empty_fields(rng):
    values = rng.choice((("",), ("", ""), ("", "a"), ("a", ""),
                         ("a", "", "b"), ("", "a b", ""), ()))
    form = rng.choice(("$@", "$*", '"$@"', '"$*"'))
    form = rng.choice(("", "pre", "''")) + form + rng.choice(("", "post", "''"))
    return "sequence-empty-fields", ("bash", "posix", "dash"), program(
        "set -- " + " ".join(quote(value) for value in values),
        "IFS=" + quote(rng.choice(("", " ", ":"))),
        "observe() { printf 'n=%s' \"$#\"; for v do printf '<%s>' \"$v\"; done; echo; }",
        "observe " + form)


def slice_effects(rng):
    setup, name = rng.choice((("x=abcdef", "x"), ("x=", "x"), ("unset x", "x"),
                              ("a=([2]=abc [5]=def)", "a[@]"),
                              ("a=()", "a[@]"), ("set -- abc def", "@")))
    offset = rng.choice(("i++", "99", "-99", "1/0", "(i+=2)"))
    length = rng.choice(("n++", "1/0", "-99", "2"))
    command = "printf 'slice=<%s>\\n' \"${" + name + ": " + offset + ":" + length + "}\""
    wrapper = rng.choice(("direct", "function", "eval", "source", "loop", "time",
                          "negate", "redirect", "for-items", "case", "conditional",
                          "while", "until"))
    if wrapper == "function":
        command = "f() { " + command + "; echo tail; }\nf"
    elif wrapper in ("eval", "source"):
        body = command + "; echo tail" + rng.choice(("", "\necho nested:$?"))
        if wrapper == "eval":
            command = "eval " + quote(body) + "; echo caller:$?"
        else:
            command = ("printf '%s\\n' " + quote(body) + " > source\n" +
                       ". ./source; echo caller:$?")
    elif wrapper == "loop":
        command = "for v in a b; do " + command + "; echo tail; done"
    elif wrapper == "time":
        command = "TIMEFORMAT=; time " + command
    elif wrapper == "negate":
        command = "! " + command
    elif wrapper == "redirect":
        # An ambiguous redirect in Bash reevaluates its word while producing
        # the diagnostic, repeating side effects. Keep this family about
        # expansion-error recovery: scalar redirects avoid that oracle quirk,
        # and the exec domain separately tests ambiguous redirect statuses.
        if name in ("a[@]", "@"):
            setup, name = "x=abcdef", "x"
        command = ": > \"${" + name + ": " + offset + ":" + length + "}\""
    elif wrapper == "for-items":
        command = ("for v in \"${" + name + ": " + offset + ":" + length +
                   "}\"; do printf '<%s>' \"$v\"; done")
    elif wrapper == "case":
        command = "case \"${" + name + ": " + offset + ":" + length + "}\" in *) echo case;; esac"
    elif wrapper == "conditional":
        command = "[[ \"${" + name + ": " + offset + ":" + length + "}\" = abc ]]"
    elif wrapper in ("while", "until"):
        command = wrapper + " " + command + "; do break; done"
    return "slice-effects-" + wrapper, ("bash", "posix"), program(
        setup, "i=0; n=0", command,
        "printf 'after:%s i=%s n=%s\\n' \"$?\" \"$i\" \"$n\"")


GENERATORS = (
    parameter_default,
    parameter_trim,
    splitting_and_glob,
    arithmetic,
    arithmetic_side_effect,
    arithmetic_comma,
    arithmetic_array,
    substitution_status,
    indexed_array,
    associative_array,
    nameref,
    local_scope,
    indirect_special,
    substring,
    sequence_slice,
    array_transform,
    sequence_empty_fields,
    slice_effects,
)


def cases(rng, budget):
    """Yield a balanced, deterministic budget of semantic programs."""
    offset = rng.randrange(len(GENERATORS))
    for index in range(budget):
        yield GENERATORS[(offset + index) % len(GENERATORS)](rng)
