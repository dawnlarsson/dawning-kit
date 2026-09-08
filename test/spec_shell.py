"""The shell: its language as FAMILIES of whole programs, its process
surface (startup flags, argv[0] policy, set options, terminals) as Utility
grammars wrapped into scripts, and the pinned policy rows.

Preliminary: absorbed generators only.
"""

import shlex

from differential import Utility, Option, INPUTS, FIXTURES

shell_ALL = ("bash", "posix", "dash")
shell_BASH = ("bash", "posix")

# ---- absorbed: test/shell_cases_lex.py ----



shell_TOKEN_LENGTHS = (1, 7, 15, 31, 63, 255, 1023)
shell_DELIMITER_LENGTHS = (1, 7, 31, 127)


def shell_lex_payload(rng, prefix="v"):
    """Stable bytes at a chosen lexer boundary length.

    Random hex labels made byte-distinct scripts exercise identical grammar
    and defeated the shared runner's exact-script deduplication. Length is a
    real scanner/storage dimension; spelling is intentionally fixed.
    """
    return prefix + "x" * rng.choice(shell_TOKEN_LENGTHS)


def shell_lex_quotes(rng):
    payload = shell_lex_payload(rng)
    split = max(1, len(payload) // 2)
    pieces = [payload[:split], "' #)${} '", '"$v"', "\\ ", payload[split:]]
    rng.shuffle(pieces)
    word = "".join(pieces)
    return ("quote-placement", shell_ALL,
            "v='V W'\nprintf '<%s>\\n' " + word + "\n")


def shell_lex_substitution(rng):
    payload = shell_lex_payload(rng)
    command = "printf '%s' '" + payload + "'"
    depth = rng.randint(1, 4)
    for level in range(depth):
        if level & 1:
            command = "printf '%s' \"$(" + command + ")\""
        else:
            command = "printf '[%s]' \"$(" + command + ")\""
    return (f"substitution-depth-{depth}", shell_ALL,
            "printf '<%s>\\n' \"$(" + command + ")\"\n")


def shell_lex_heredoc(rng):
    payload = shell_lex_payload(rng, "body-")
    delimiter = "MW_" + "D" * rng.choice(shell_DELIMITER_LENGTHS)
    quoted = bool(rng.getrandbits(1))
    strip = bool(rng.getrandbits(1))
    operator = "<<-" if strip else "<<"
    header = operator + ("'" + delimiter + "'" if quoted else delimiter)
    indent = "\t" if strip else ""
    body = payload + " ) } # " + ("$v" if not quoted else "${v}")
    script = (
        "v=VALUE\n"
        "answer=$(\n"
        "cat " + header + "\n" + indent + body + "\n" +
        indent + delimiter + "\n"
        "printf '%s' :tail\n"
        ")\n"
        "printf '<%s>\\n' \"$answer\"\n"
    )
    return ("heredoc-quoted" if quoted else "heredoc-expanded", shell_ALL, script)


def shell_lex_comment_boundary(rng):
    payload = shell_lex_payload(rng)
    shape = rng.randrange(3)
    if shape == 0:
        script = ("printf '<%s>\\n' \"$(\n"
                  "printf '%s' '" + payload + "'\n"
                  "# ignored close: ) } `\n"
                  "printf '%s' :after\n)\"\n")
    elif shape == 1:
        script = ("printf '<%s>\\n' " + payload + "\\\n"
                  "joined # comment after the joined word\n")
    else:
        script = ("printf '<%s>' '" + payload + "'; # operator boundary\n"
                  "printf '<after>\\n'\n")
    return (f"comment-continuation-{shape}", shell_ALL, script)


def shell_lex_operators(rng):
    payload = shell_lex_payload(rng)
    shape = rng.randrange(4)
    scripts = (
        "{ printf '<%s>' '" + payload + "'; false; } || printf '<fallback>\\n'\n",
        "true && (printf '<%s>' '" + payload + "'; printf '<sub>')\nprintf '<end>\\n'\n",
        "false || { printf '<%s>' '" + payload + "'; true; } && printf '<and>\\n'\n",
        "printf '<%s>' '" + payload + "' | { cat; printf '<pipe>\\n'; }\n",
    )
    return (f"operator-boundary-{shape}", shell_ALL, scripts[shape])


def shell_lex_redirection(rng):
    payload = shell_lex_payload(rng)
    shape = rng.randrange(3)
    if shape == 0:
        script = ("name=out\nprintf '%s\\n' '" + payload + "' >\"$name\"\n"
                  "cat <out\n")
    elif shape == 1:
        script = ("{ printf '%s' '" + payload + "'; printf '%s\\n' :two; } 3>unused >out\n"
                  "cat out\n")
    else:
        script = ("printf '%s\\n' '" + payload + "' > out\n"
                  "while IFS= read -r line; do printf '<%s>\\n' \"$line\"; done <out\n")
    return (f"redirection-boundary-{shape}", shell_ALL, script)


def shell_lex_syntax_mutation(rng):
    shape = rng.randrange(6)
    scripts = (
        "printf '<before>\\n'\nprintf '%s\\n' \"unterminated\n",
        "printf '<before>\\n'\nprintf '%s\\n' 'unterminated\n",
        "printf '<before>\\n'\nprintf '%s\\n' \"$(printf nested\"\n",
        "v=abc\nprintf '<before>\\n'\nprintf '%s\\n' \"${v#'a}\"\n",
        "printf '<before>\\n'\nif true; then printf '<open>'\n",
        "printf '<before>\\n'\nprintf '%s\\n' $((1 + (2 * 3)\n",
    )
    return (f"syntax-mutation-{shape}", shell_ALL, scripts[shape])


def shell_lex_stray_terminator(rng):
    # Each spelling is legal only while a matching grammar production owns
    # it. A non-interactive reader must reject the whole input, not recover at
    # the next physical line as an interactive prompt would.
    token = rng.choice(("}", ")", "then", "else", "fi", "do", "done",
                        "esac", ";;"))
    return ("stray-terminator-" + token.replace(";", "semi"), shell_ALL,
            token + "\nprintf '<after>\\n'\n")


def shell_lex_function_metadata(rng):
    suffix = "n" * rng.choice(shell_TOKEN_LENGTHS)
    names = ["fa_" + suffix, "fb_" + suffix, "fc_" + suffix]
    rng.shuffle(names)
    definitions = "; ".join(name + "() { :; }" for name in names)
    shape = rng.randrange(3)
    if shape == 0:
        script = (definitions + "\ndeclare -F\ndeclare -F " + names[1] +
                  " missing_" + suffix + " " + names[0] +
                  "\nprintf '<%s>\\n' \"$?\"\n")
    elif shape == 1:
        script = (definitions + "\nunset -f " + names[0] +
                  "\ncompgen -A function 'f'\n")
    else:
        script = (definitions + "\nreadonly -f " + names[1] +
                  "\nunset -f " + names[1] + " 2>/dev/null\n"
                  "printf '<unset:%s>\\n' \"$?\"\n" + names[1] +
                  "() { printf bad; }\nprintf '<define:%s>\\n' \"$?\"\n"
                  "declare -F " + names[1] + "\n")
    return (f"function-metadata-{shape}", ("bash", "posix"), script)


def shell_lex_nested_syntax(rng):
    broken = rng.choice(("}\nprintf inner-forbidden\\n\n",
                         "if true; then", "printf '%s' \"unterminated"))
    source = rng.choice((False, True))
    wrapped = rng.choice(("", "command "))
    if source:
        setup = "printf '%s' " + shlex.quote(broken) + " > generated.bad\n"
        command = wrapped + ". ./generated.bad"
    else:
        setup = ""
        command = wrapped + "eval " + shlex.quote(broken)
    body = command + "\nprintf 'reader-after:%s\\n' \"$?\"\n"
    if rng.randrange(2):
        body = "command eval " + shlex.quote(body) + "\n"
    return ("nested-syntax-boundary", shell_ALL,
            setup + body + "printf 'outer-after:%s\\n' \"$?\"\n")





# ---- absorbed: test/shell_cases_exec.py ----


shell_VALUE_LENGTHS = (1, 7, 31, 127, 511)


def shell_exec_value(rng, prefix, byte):
    """Vary a storage boundary, not an otherwise irrelevant random label."""
    return prefix + byte * rng.choice(shell_VALUE_LENGTHS)


def shell_exec_special_prefix(rng):
    old = shell_exec_value(rng, "old-", "o")
    new = shell_exec_value(rng, "new-", "n")
    tail = rng.choice((":", "eval :", "export x"))
    return ("special-prefix", ("bash", "posix", "dash"),
            f"x={old}; x={new} {tail}; printf '%s:%s\\n' \"$x\" \"$?\"")


def shell_exec_command_exception(rng):
    name = rng.choice(("1bad", "bad-name", "9"))
    action = rng.choice((f"export {name}=x", f"readonly {name}=x"))
    return ("special-command-exception", ("bash", "posix"),
            f"command {action}; s=$?; printf 'after:%s\\n' \"$s\"")


def shell_exec_disabled_special(rng):
    if rng.randrange(2):
        script = ("enable -n :; x=old; x=new :; "
                  "printf 'x=%s s=%s\\n' \"$x\" \"$?\"")
    else:
        script = ("enable -n return; function return { echo FUNCTION; }; "
                  "return; echo AFTER")
    return ("disabled-special", ("bash", "posix"), script)


def shell_exec_control_status(rng):
    first = rng.randrange(1, 8)
    second = rng.randrange(1, 8)
    branch = rng.choice((
        f"if (exit {first}); then echo bad; else (exit {second}) || :; fi",
        f"(exit {first}) && echo bad || (exit {second}) || :",
        f"n=0; while (exit {first}); do n=$((n+1)); done; (exit {second}) || :",
        f"for n in 1 2; do (exit {first}) && echo bad || :; done; (exit {second}) || :",
    ))
    return ("nested-control-status", ("bash", "posix", "dash"),
            branch + "; printf 'done:%s\\n' \"$?\"")


def shell_exec_errexit_context(rng):
    code = rng.randrange(2, 10)
    shape = rng.choice((
        f"set -e; (exit {code}) && echo bad; echo after",
        f"set -e; if (exit {code}); then echo bad; fi; echo after",
        f"set -e; ! (exit {code}); echo after",
        f"set -e; (exit {code}) || echo caught; echo after",
    ))
    return ("errexit-tested-context", ("bash", "posix", "dash"), shape)


def shell_exec_child_exit(rng):
    body = rng.choice((
        "(trap 'echo CHILD' EXIT; echo body)",
        "v=$(trap 'echo CHILD' EXIT; echo body); printf '<%s>\\n' \"$v\"",
        "cat <(trap 'echo CHILD' EXIT; echo body)",
    ))
    modes = ("bash", "posix") if "<(" in body else ("bash", "posix", "dash")
    return ("child-exit-trap", modes,
            f"trap 'echo PARENT' EXIT; {body}")


def shell_exec_inherited_exit(rng):
    depth = rng.randrange(1, 4)
    body = "echo body"
    for _ in range(depth):
        # Spaces keep nested subshells distinct from Bash's (( arithmetic
        # command token.
        body = f"( {body} )"
    return ("inherited-exit-trap", ("bash", "posix", "dash"),
            f"trap 'echo PARENT' EXIT; {body}")


def shell_exec_rhs_status(rng):
    first = rng.randrange(1, 8)
    last = rng.randrange(1, 8)
    initial = rng.choice(("true", "false"))
    return ("assignment-rhs-status", ("bash", "posix"),
            f"{initial}; a=$(exit {first}) b=$? c=$(exit {last}) d=$?; "
            "printf '%s:%s:%s\\n' \"$b\" \"$d\" \"$?\"")


def shell_exec_function_scope(rng):
    # These sizes cross copy/storage boundaries while fixed byte patterns let
    # the runner collapse cases with the same execution shape.
    outer = shell_exec_value(rng, "outer-", "o")
    prefix = shell_exec_value(rng, "prefix-", "p")
    inner = shell_exec_value(rng, "inner-", "i")
    return ("function-prefix-scope", ("bash", "posix", "dash"),
            "f() { printf 'in:%s\\n' \"$x\"; x=" + inner + "; }; "
            f"x={outer}; x={prefix} f; printf 'out:%s:%s\\n' \"$x\" \"$?\"")


def shell_exec_nested_loop_items(rng):
    """Keep an outer loop's indexed slice live while an inner slice grows."""
    outer_count = rng.randrange(2, 8)
    inner_count = rng.randrange(max(outer_count, 9), 18)
    outer = [f"outer-{at}-" + "o" * rng.choice(shell_VALUE_LENGTHS)
             for at in range(outer_count)]
    inner = [f"inner-{at}-" + "i" * rng.choice(shell_VALUE_LENGTHS)
             for at in range(inner_count)]
    outer_words = " ".join(outer)
    inner_words = " ".join(inner)
    shape = rng.choice(("direct", "function", "positional", "glob",
                        "for-select", "select-for"))

    if shape == "function":
        script = (f"inside() {{ for inner in {inner_words}; do :; done; }}\n"
                  f"for outer in {outer_words}; do inside; "
                  "printf '<%s>\\n' \"$outer\"; done\n")
        modes = ("bash", "posix", "dash")
    elif shape == "positional":
        script = (f"set -- {outer_words}\nfor outer; do\n"
                  f"set -- {inner_words}\nfor inner; do :; done\n"
                  "printf '<%s>\\n' \"$outer\"\ndone\n")
        modes = ("bash", "posix", "dash")
    elif shape == "glob":
        script = (f"for outer in *; do for inner in {inner_words}; do :; done; "
                  "printf '<%s>\\n' \"$outer\"; done\n")
        modes = ("bash", "posix", "dash")
    elif shape == "for-select":
        script = (f"for outer in {outer_words}; do\n"
                  f"select inner in {inner_words}; do break; done <<'ANSWER'\n"
                  "1\nANSWER\nprintf '<%s>\\n' \"$outer\"\ndone\n")
        modes = ("bash", "posix")
    elif shape == "select-for":
        script = (f"select outer in {outer_words}; do\n"
                  f"for inner in {inner_words}; do :; done\n"
                  "printf '<%s>\\n' \"$outer\"\n"
                  "test \"$REPLY\" = 2 && break\ndone <<'ANSWER'\n"
                  "1\n2\nANSWER\n")
        modes = ("bash", "posix")
    else:
        script = (f"for outer in {outer_words}; do "
                  f"for inner in {inner_words}; do :; done; "
                  "printf '<%s>\\n' \"$outer\"; done\n")
        modes = ("bash", "posix", "dash")

    return "nested-loop-items", modes, script


def shell_exec_loop_control_transition(rng):
    """Both item sources retain outer-loop control and parameter ownership."""
    kind = rng.choice(("for", "select"))
    explicit = rng.choice(("", " in first '' 'two words'"))
    action = rng.choice((":", "false", "break", "continue", "break 2", "continue 2"))
    script = ("set -- first '' 'two words'\nhits=0\n"
              "for outer in one two; do\n" + kind + " inner" + explicit + "; do\n"
              "hits=$((hits+1)); printf '%s:<%s>:%s\\n' \"$outer\" \"$inner\" \"$hits\"\n"
              "set -- changed params\n" + action + "\necho body-tail\ndone")
    if kind == "select":
        script += " <<'CHOICES'\n\n0\nnot-a-number\n1\n2\nCHOICES\n"
    else:
        script += "\n"
    script += ("printf 'loop:%s\\n' \"$?\"\ndone\n"
               "printf 'end:%s:%s:<%s>\\n' \"$?\" \"$hits\" \"$*\"\n")
    modes = ("bash", "posix") if kind == "select" else ("bash", "posix", "dash")
    return "loop-control-transition", modes, script


def shell_exec_function_serialization(rng):
    """Round-trip retained ASTs, varying structure as well as operand bytes."""
    value = rng.choice(("plain", "two words", "quote'and\"slash\\", "é🌙"))
    quoted = "'" + value.replace("'", "'\"'\"'") + "'"
    body = "printf '<%s:%s>\\n' \"$1\" \"$x\""
    layers = rng.sample(("group", "subshell", "if", "for", "case", "andor"),
                        rng.randrange(1, 5))
    for layer in layers:
        if layer == "group":
            body = "{\n" + body + "\n}"
        elif layer == "subshell":
            body = "(\n" + body + "\n)"
        elif layer == "if":
            body = "if test -n \"$1\"; then\n" + body + "\nelse echo EMPTY; fi"
        elif layer == "for":
            body = "for x in a 'b c'; do\n" + body + "\ndone"
        elif layer == "case":
            body = "case $1 in v*)\n" + body + "\n;; *) echo OTHER;; esac"
        else:
            body = "false || {\n" + body + "\n}"
    attribute = rng.choice(("", "export -f f\n"))
    return ("function-serialization", ("bash", "posix"),
            "x=" + quoted + "\nf() {\n" + body + "\n}\n" + attribute +
            "saved=$(declare -f f); status=$?; unset -f f\n"
            "eval \"$saved\"; f value\n"
            "printf 'status=%s x=<%s>\\n' \"$status\" \"$x\"\n")


def shell_exec_function_heredoc_serialization(rng):
    headers = []
    bodies = []
    for at in range(rng.randrange(1, 5)):
        delimiter = f"END_{at}"
        headers.append("<<" + ("'" + delimiter + "'" if rng.randrange(2) else delimiter))
        body = rng.choice(("", "$VALUE\n", "MOONWATER_FUNCTION_EOF_0\n",
                           "a\\\nb\n", "a\\\\b\n", "\t$VALUE\n",
                           "$(printf touched > marker)text\n"))
        bodies.append(body + delimiter + "\n")
    return ("function-heredoc-serialization", ("bash", "posix"),
            "VALUE=before\nf() { cat " + " ".join(headers) + "\n" +
            "".join(bodies) + "}\n"
            "saved=$(declare -f f); status=$?; unset -f f\n"
            "VALUE=after; eval \"$saved\"; f\n"
            "printf 'status=%s\\n' \"$status\"\n")


def shell_exec_function_control_heredoc_serialization(rng):
    """Put retained documents across real pipeline/list grammar boundaries."""
    def header(at):
        delimiter = f"CONTROL_{at}"
        if rng.randrange(2):
            return "<<'" + delimiter + "'", delimiter
        return "<<" + delimiter, delimiter

    def document(delimiter, body=None):
        if body is None:
            body = rng.choice(("", "$VALUE\n", "a\\\nb\n", "a\\\\b\n",
                               "MOONWATER_FUNCTION_EOF_0\n",
                               "$(printf made > control-marker)text\n"))
        return body + delimiter + "\n"

    shape = rng.choice(("pipeline", "andor", "pipeerr", "conditional",
                        "nested", "group-andor", "while", "for", "case"))
    first, first_delimiter = header(0)
    second, second_delimiter = header(1)
    if shape == "pipeline":
        body = (f"cat {first} | cat {second}\n" +
                document(first_delimiter) + document(second_delimiter))
    elif shape == "andor":
        body = (f"false && cat {first} || cat {second}\n" +
                document(first_delimiter) + document(second_delimiter))
    elif shape == "pipeerr":
        body = ("{ printf 'err\\n' >&2; cat; } " + first +
                " |& sed 's/^/seen:/'\n" + document(first_delimiter))
    elif shape == "conditional":
        condition = rng.choice(("needle\n", "other\n"))
        body = (f"if cat {first} | grep -q needle\n" +
                document(first_delimiter, condition) +
                f"then cat {second}\n" + document(second_delimiter) +
                "else printf 'miss\\n'\nfi\n")
    elif shape == "nested":
        body = (f"inner() {{ cat {first} | sed 's/^/inner:/'\n" +
                document(first_delimiter) + "}\ninner\n")
    elif shape == "group-andor":
        body = (f"{{ cat {first} | cat\n" +
                document(first_delimiter) + f"}} && cat {second}\n" +
                document(second_delimiter))
    elif shape == "while":
        body = (f"while grep -q go {first}\n" +
                document(first_delimiter, "go\n") +
                f"do cat {second}\n" + document(second_delimiter) +
                "break\ndone\n")
    elif shape == "for":
        body = (f"for x in one two\ndo cat {first}\n" +
                document(first_delimiter) + "done\n")
    else:
        body = (f"case x in\nx) cat {first}\n" +
                document(first_delimiter) + ";;\nesac\n")

    transport = rng.choice((
        "saved=$(declare -f f); status=$?; unset -f f\n"
        "eval \"$saved\"; f\n",
        "export -f f; status=$?; /bin/bash -c f\n",
    ))
    return ("function-control-heredoc-serialization", ("bash", "posix"),
            "VALUE=expanded\nf() {\n" + body + "}\n" + transport +
            "printf 'status=%s\\n' \"$status\"\n")


def shell_exec_redirect_cardinality(rng):
    pattern = rng.choice(("?.txt", "a.*", "missing.*"))
    operator = rng.choice((">", ">|"))
    # Redirect pathname expansion is a Bash policy; dash deliberately does
    # not share Bash's default noninteractive glob behavior.
    return ("redirect-cardinality", ("bash", "posix"),
            f"printf 'DATA' {operator} {pattern}; s=$?; "
            "printf 'status:%s\\n' \"$s\"")


def shell_exec_pipeline_context(rng):
    left = rng.randrange(1, 8)
    right = rng.randrange(1, 8)
    shape = rng.choice((
        f"(exit {left}) | (exit {right}); printf 'p:%s\\n' \"$?\"",
        f"! (exit {left}) | (exit {right}); printf 'p:%s\\n' \"$?\"",
        f"(exit {left}) | true; printf 'p:%s\\n' \"$?\"",
    ))
    return ("pipeline-status", ("bash", "posix", "dash"), shape)


def shell_exec_subshell_scope(rng):
    outer = shell_exec_value(rng, "outer-", "o")
    inner = shell_exec_value(rng, "inner-", "i")
    depth = rng.randrange(1, 4)
    body = f"x={inner}; printf 'in:%s\\n' \"$x\""
    for _ in range(depth):
        body = f"( {body} )"
    return ("subshell-scope", ("bash", "posix", "dash"),
            f"x={outer}; {body}; printf 'out:%s\\n' \"$x\"")


def shell_exec_composed_status(rng):
    """Compose a failing command through two to four execution contexts."""
    code = rng.randrange(1, 9)
    body = f"printf 'before:{code}\\n'\n(exit {code})"
    layers = rng.sample(("group", "subshell", "function", "if", "andor",
                         "not", "pipeline", "redirect"),
                        rng.randrange(2, 7))
    function_number = 0
    for layer in layers:
        if layer == "group":
            body = "{\n" + body + "\n}"
        elif layer == "subshell":
            body = "(\n" + body + "\n)"
        elif layer == "function":
            function_number += 1
            name = f"generated_f{function_number}"
            body = f"{name}() {{\n{body}\n}}\n{name}"
        elif layer == "if":
            body = ("if\n" + body +
                    "\nthen\n  printf 'then\\n'\nelse\n  printf 'else\\n'\nfi")
        elif layer == "andor":
            body = "{\n" + body + "\n} || printf 'caught\\n'"
        elif layer == "not":
            body = "! {\n" + body + "\n}"
        elif layer == "pipeline":
            body = "{\n" + body + "\n} | cat"
        else:
            body = "{\n" + body + "\n} > generated.deep"
    policy = rng.randrange(4)
    if policy == 0:
        option = "set -e\n"
        modes = ("bash", "posix", "dash")
    elif policy == 1:
        option = "set -e -o pipefail\n"
        modes = ("bash", "posix")
    elif policy == 2:
        option = "trap '\''printf err:%s\\n \"$?\"'\'' ERR\n"
        modes = ("bash", "posix")
    else:
        option = "set +e\n"
        modes = ("bash", "posix", "dash")
    return ("composed-control", modes,
            option + body + "\ns=$?\nprintf 'end:%s\\n' \"$s\"")


def shell_exec_deep_control(rng):
    code = rng.randrange(1, 9)
    depth = rng.randrange(8, 33)
    body = f"(exit {code})"
    for _ in range(depth):
        layer = rng.randrange(4)
        if layer == 0:
            body = "{\n" + body + "\n}"
        elif layer == 1:
            body = ("if\n" + body +
                    "\nthen\n  :\nelse\n  :\nfi")
        elif layer == 2:
            body = "{\n" + body + "\n} || :"
        else:
            body = "! {\n" + body + "\n}"
    option = rng.choice(("set -e\n", "set +e\n"))
    return ("deep-control", ("bash", "posix", "dash"),
            option + body + "\nprintf 'deep:%s:%s\\n' \"$?\" " +
            str(depth))


def shell_exec_special_scope(rng):
    bad = rng.choice(("1bad", "bad-name", "8bad"))
    operation = rng.choice((f"export {bad}=x", f"readonly {bad}=x",
                            "unset -Z"))
    if rng.randrange(2):
        operation = "command " + operation
    context = rng.choice(("direct", "function", "group", "subshell"))
    if context == "function":
        operation = f"f() {{\n{operation}\nprintf 'inside-after\\n'\n}}\nf"
    elif context == "group":
        operation = "{\n" + operation + "\nprintf 'group-after\\n'\n}"
    elif context == "subshell":
        operation = "(\n" + operation + "\nprintf 'sub-after\\n'\n)"
    return ("special-error-scope", ("bash", "posix"),
            operation + "\ns=$?\nprintf 'outer:%s\\n' \"$s\"")


def shell_exec_descriptor_order(rng):
    first = rng.choice(("1", "2"))
    if first == "1":
        redirects = "> generated.out 2>&1"
    else:
        redirects = "2>&1 > generated.out"
    body = "printf 'stdout\\n'; printf 'stderr\\n' >&2"
    context = rng.choice(("{ " + body + "; }",
                          "( " + body + " )",
                          "f() { " + body + "; }; f"))
    return ("descriptor-order", ("bash", "posix", "dash"),
            f"{context} {redirects}\n"
            "s=$?\nprintf 'status:%s\\n' \"$s\"\ncat generated.out")


def shell_exec_readonly_scope(rng):
    old = shell_exec_value(rng, "old-", "o")
    new = shell_exec_value(rng, "new-", "n")
    wrapped = rng.randrange(2)
    assign = f"x={new} :"
    if wrapped:
        assign = f"x={new} command :"
    return ("readonly-special-prefix", ("bash", "posix"),
            f"x={old}\nreadonly x\nf() {{\n{assign}\nprintf 'inner:%s:%s\\n' \"$x\" \"$?\"\n}}\n"
            "f\nprintf 'outer:%s:%s\\n' \"$x\" \"$?\"")





# ---- absorbed: test/shell_cases_expand.py ----



def shell_quote(text):
    return "'" + text.replace("'", "'\"'\"'") + "'"


def shell_program(*lines):
    """Keep setup, mutation and observation separable for line shrinking."""
    return "\n".join(lines) + "\n"


def shell_expand_parameter_default(rng):
    state = rng.choice(("unset", "empty", "value"))
    operation = rng.choice(("-", ":-", "+", ":+", "=", ":="))
    quoted = rng.choice((False, True))
    value = rng.choice(("alpha", "a b", "a::b", "abcabc"))
    fallback = rng.choice(("fallback", "two words", "q:r"))
    setup = "unset x"
    if state == "empty":
        setup = "x="
    elif state == "value":
        setup = "x=" + shell_quote(value)
    form = "${x" + operation + "$fallback}"
    if quoted:
        form = '"' + form + '"'
    script = shell_program(setup, f"fallback={shell_quote(fallback)}", "IFS=:",
                     f"set -- {form}",
                     "printf 'argc=%s' \"$#\"; for item do printf '<%s>' \"$item\"; done",
                     "printf '|x=<%s>\\n' \"${x-unset}\"")
    return "parameter-default", ("bash", "posix", "dash"), script


def shell_expand_parameter_trim(rng):
    value = rng.choice(("abcabc", "prefix-middle-suffix", "a/b/c", "000123"))
    operation = rng.choice(("#", "##", "%", "%%"))
    pattern = rng.choice(("a*", "*c", "prefix-*", "*/", "0*", "?"))
    script = shell_program(f"x={shell_quote(value)}", f"p={shell_quote(pattern)}",
                     f"printf '<%s>|<%s>\\n' \"${{x{operation}$p}}\" \"$x\"")
    return "parameter-trim", ("bash", "posix", "dash"), script


def shell_expand_splitting_and_glob(rng):
    separator = rng.choice((":", ",", " "))
    value = rng.choice(("a::b c", "a,b,,c", "  a  b ", "a.txt:b.txt"))
    tail = rng.choice(("*.txt", "a*", "no-match-*"))
    script = shell_program(f"IFS={shell_quote(separator)}", f"x={shell_quote(value)}",
                     f"set -- $x {tail}",
                     "printf 'argc=%s' \"$#\"; for item do printf '<%s>' \"$item\"; done",
                     "echo")
    return "split-glob", ("bash", "posix", "dash"), script


def shell_expand_arithmetic(rng):
    left = rng.randrange(-40, 41)
    right = rng.randrange(1, 16)
    third = rng.randrange(0, 8)
    operator = rng.choice(("+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^"))
    # Shifts need a small nonnegative count; every other operation accepts the
    # same bounded right operand, keeping the oracle away from overflow/zero.
    operand = third if operator in ("<<", ">>") else right
    expression = f"(x {operator} {operand}) + (y > 2)"
    script = shell_program(f"x={left}", f"y={right}", f"r=$(({expression}))",
                     "printf 'r=%s x=%s y=%s\\n' \"$r\" \"$x\" \"$y\"")
    return "arithmetic", ("bash", "posix", "dash"), script


def shell_expand_arithmetic_side_effect(rng):
    start = rng.randrange(0, 8)
    limit = rng.randrange(start, start + 8)
    truth = rng.choice((0, 1))
    expression = (f"({truth} && (i += 3)) || (i += 2), "
                  f"i < {limit} ? i + 5 : i - 1")
    script = shell_program(f"i={start}", f"r=$(({expression}))",
                     "printf 'r=%s i=%s\\n' \"$r\" \"$i\"")
    return "arithmetic-effects", ("bash", "posix"), script


def shell_expand_arithmetic_precedence(rng):
    """Vary adjacent grammar levels and mutations of the right operand."""
    operation = rng.choice(("+", "-", "*", "/", "%", "<<", ">>",
                            "<", "<=", ">", ">=", "==", "!="))
    left, right = rng.randrange(-20, 21), rng.randrange(1, 6)
    tail = rng.randrange(1, 5)
    operand = f"(i += {tail})" if operation not in ("<<", ">>") else "(i %= 4)"
    expression = rng.choice((f"x {operation} {operand} + 2 * 3",
                             f"(x {operation} {operand}) == (x < i)",
                             f"0 && (x {operation} {operand})",
                             f"1 || (x {operation} {operand})"))
    return "arithmetic-precedence", ("bash", "posix"), shell_program(
        f"x={left}; i={right}", f"r=$(({expression}))",
        "printf 'r=%s x=%s i=%s\\n' \"$r\" \"$x\" \"$i\"")


def shell_expand_arithmetic_comma(rng):
    first = rng.randrange(0, 8)
    second = rng.randrange(1, 8)
    sequence = f"i += {first}, i += {second}, i * 2"
    context = rng.choice(("group", "value", "conditional", "short-circuit"))
    setup = "i=0"
    if context == "value":
        setup += "; expression=" + shell_quote(sequence)
        expression = "expression"
    elif context == "conditional":
        expression = f"{rng.randrange(0, 2)} ? {sequence} : 99"
    elif context == "short-circuit":
        expression = f"{rng.randrange(0, 2)} && ({sequence})"
    else:
        expression = "(" * rng.randrange(1, 5) + sequence
        expression += ")" * (len(expression) - len(expression.lstrip("(")))
    return "arithmetic-comma-" + context, ("bash", "posix"), shell_program(
        setup, "r=$((" + expression + "))",
        "printf 'r=%s i=%s\\n' \"$r\" \"$i\"")


def shell_expand_arithmetic_array(rng):
    """Exercise one prepared indexed target through every lvalue form."""
    left, right, created = sorted(rng.sample(range(1, 12), 3))
    first = rng.randrange(1, 10)
    second = rng.randrange(1, 10)
    made = rng.randrange(1, 10)
    delta = rng.randrange(1, 6)
    operator = rng.choice(("+=", "*=", "^=", "<<="))
    script = shell_program(
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


def shell_expand_substitution_status(rng):
    prior = rng.choice(("true", "false"))
    first = rng.choice(("true", "false"))
    second = rng.choice(("true", "false"))
    script = shell_program(prior, f"a=$({first}) b=$? c=$({second}) d=$?",
                     "printf 'b=%s d=%s status=%s\\n' \"$b\" \"$d\" \"$?\"")
    return "substitution-status", ("bash", "posix"), script


def shell_expand_indexed_array(rng):
    first, second = sorted(rng.sample(range(0, 8), 2))
    value1 = rng.choice(("alpha", "a b", ""))
    value2 = rng.choice(("beta", "q:r", "two words"))
    action = rng.choice(("write", "append", "unset"))
    if action == "write":
        mutate = f"a[{second + 2}]={shell_quote(value2)}"
    elif action == "append":
        mutate = f"a[{first}]+={shell_quote(value2)}"
    else:
        mutate = f"unset 'a[{first}]'"
    script = shell_program("declare -a a", f"a[{first}]={shell_quote(value1)}",
                     f"a[{second}]={shell_quote(value2)}", mutate,
                     "printf 'n=%s keys=' \"${#a[@]}\"",
                     "printf '<%s>' \"${!a[@]}\"; printf '|values='",
                     "printf '<%s>' \"${a[@]}\"; echo")
    return "indexed-array", ("bash", "posix"), script


def shell_expand_associative_array(rng):
    # ab and bA collide under the shared 33-based hash: equality still needs
    # the key bytes, including on the COW write/removal paths.
    keys = rng.sample(("x", "y", "ab", "bA", "long-key", "2", "a b"), 3)
    values = rng.sample(("one", "two words", "", "q:r", "last"), 3)
    action = rng.choice(("write", "append", "unset"))
    setup = [f"m[{shell_quote(key)}]={shell_quote(value)}"
             for key, value in zip(keys[:2], values[:2])]
    if action == "write":
        mutate = f"m[{shell_quote(keys[2])}]={shell_quote(values[2])}"
    elif action == "append":
        mutate = f"m[{shell_quote(keys[0])}]+={shell_quote(values[2])}"
    else:
        mutate = f"unset 'm[{keys[0]}]'"
    # Associative key iteration order is not specified. Query the generated
    # keys directly and use only the count as the aggregate observation.
    observations = "".join(
        f"printf '|{i}=<%s>' \"${{m[{shell_quote(key)}]-unset}}\"; "
        for i, key in enumerate(keys)) + "echo"
    script = shell_program("declare -A m", *setup, mutate,
                     "printf 'n=%s' \"${#m[@]}\"", observations)
    return "associative-array", ("bash", "posix"), script


def shell_expand_nameref(rng):
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
    return "nameref", ("bash", "posix"), shell_program(setup, mutate, read)


def shell_expand_local_scope(rng):
    outer = rng.choice(("outer", "a b", ""))
    inner = rng.choice(("inner", "q:r", "two words"))
    action = rng.choice(("assign", "unset", "readonly", "bare"))
    if action == "assign":
        body = f"local x={shell_quote(inner)}; x=changed; printf 'in=<%s>\\n' \"$x\""
    elif action == "unset":
        body = f"local x={shell_quote(inner)}; unset x; printf 'in=<%s>\\n' \"${{x-unset}}\""
    elif action == "readonly":
        body = f"local x={shell_quote(inner)}; readonly x; printf 'in=<%s>\\n' \"$x\""
    else:
        body = "local x; printf 'in=<%s>\\n' \"${x-unset}\"; local x; x=inner"
    script = shell_program(f"x={shell_quote(outer)}", f"f() {{ {body}; }}", "f",
                     "printf 'out=<%s>\\n' \"$x\"")
    return "local-scope", ("bash", "posix", "dash"), script


def shell_expand_indirect_special(rng):
    special = rng.choice(("#", "?"))
    positional = rng.randrange(0, 4)
    parameters = " ".join(shell_quote(f"p{i}") for i in range(positional))
    prior = rng.choice(("true", "false"))
    script = shell_program(f"set -- {parameters}", prior, "printf 'before|'",
                     f"printf '<%s>' \"${{!{special}}}\"",
                     "printf '|after\\n'")
    return "indirect-special", ("bash", "posix"), script


def shell_expand_substring(rng):
    value = rng.choice(("", "a", "a b::c", "0123456789", "a" * 33))
    offset = rng.choice((-40, -10, -2, -1, 0, 1, 3, 10, 40,
                         -(1 << 63), (1 << 63) - 1))
    length = rng.choice((None, -40, -2, -1, 0, 1, 3, 40))
    suffix = "" if length is None else rng.choice((":n", ":", ": "))
    form = "${x: i" + suffix + "}"
    if rng.choice((False, True)):
        form = '"' + form + '"'
    return "substring-boundaries", ("bash", "posix"), shell_program(
        f"x={shell_quote(value)}; i={offset}; n={length or 0}; IFS=:",
        "set -- " + form,
        "printf 'n=%s' \"$#\"; printf '<%s>' \"$@\"; echo")


def shell_expand_sequence_slice(rng):
    kind = rng.choice(("positional", "dense", "sparse"))
    values = rng.sample(("", "a", "two words", "x:y", "*", "last"),
                        rng.randrange(0, 6))
    if kind == "positional":
        setup = "set -- " + " ".join(shell_quote(value) for value in values)
        name = rng.choice(("@", "*"))
    else:
        indices = (list(range(len(values))) if kind == "dense" else
                   sorted(rng.sample(range(1, 13), len(values))))
        setup = "a=(" + " ".join(f"[{index}]={shell_quote(value)}"
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
    return "sequence-slice-" + kind, ("bash", "posix"), shell_program(
        setup, f"i={offset}; n={length or 0}; IFS={shell_quote(rng.choice((' ', ':', '')))}",
        "observe() { printf 'n=%s' \"$#\"; for v do",
        "if [ \"$v\" = \"$0\" ]; then v=argv-zero; fi; printf '<%s>' \"$v\"; done; echo; }",
        "observe " + form)


def shell_expand_array_transform(rng):
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
    return "array-transform-fields", ("bash", "posix"), shell_program(
        "a=(" + " ".join(shell_quote(value) for value in values) + ")",
        "IFS=" + shell_quote(separator),
        "set -- " + form,
        "printf 'n=%s' \"$#\"; for v do printf '<%s>' \"$v\"; done; echo")


def shell_expand_array_sequence_transition(rng):
    """Reuse one sparse inventory across keys, slices and byte modifiers."""
    values = rng.sample(("", "a", "two words", "x:y", "AB", "aa", "slash/a"),
                        rng.randrange(0, 7))
    indices = sorted(rng.sample((0, 1, 2, 7, 31, 99, 101), len(values)))
    setup = "a=(" + " ".join(f"[{i}]={shell_quote(v)}" for i, v in zip(indices, values)) + ")"
    form = rng.choice(("@", "*"))
    op = rng.choice(("", "#?", "%?", "/a/x", "//a/a/b", "^^", ",,", "@Q",
                     ": 0:2", ": -2:1", ": 999:1"))
    name = rng.choice(("a", "ref"))
    if name == "ref":
        setup += "\ndeclare -n ref=a"
    expression = '"${' + name + '[' + form + ']' + op + '}"'
    expression = rng.choice(("", "pre", "''")) + expression + rng.choice(("", "post", "''"))
    # Array ordering is stable by numeric index; exercise adjacent output
    # boundaries and repeated replacement-word reuse in the same expansion.
    return "array-sequence-transition", ("bash", "posix"), shell_program(
        setup, "IFS=" + shell_quote(rng.choice(("", " ", ":"))),
        "observe() { printf 'n=%s' \"$#\"; for v do printf '<%s>' \"$v\"; done; echo; }",
        "observe " + expression + ' "${!' + name + '[@]}" ' + expression)


def shell_expand_arithmetic_simple_transition(rng):
    """The complete grammar also handles the former name/literal shortcut."""
    value = rng.choice(("0", "17", "-21", "007", "0x10", " 23 ", "", "unset"))
    operator = rng.choice(("+", "-"))
    literal = rng.choice(("0", "1", "07", "0x20", "2147483647"))
    spacing = rng.choice(("", " ", "\t", "\n"))
    expression = "x" + spacing + operator + spacing + literal
    setup = "unset x" if value == "unset" else "x=" + shell_quote(value)
    return "arithmetic-simple-transition", shell_ALL, shell_program(
        setup, "set " + ("+u" if value == "unset" else rng.choice(("-u", "+u"))),
        "f() { printf 'value:%s\\n' \"$((" + expression + "))\"; }",
        "f; printf 'after:%s:<%s>\\n' \"$?\" \"${x-unset}\"")


def shell_expand_sequence_empty_fields(rng):
    values = rng.choice((("",), ("", ""), ("", "a"), ("a", ""),
                         ("a", "", "b"), ("", "a b", ""), ()))
    form = rng.choice(("$@", "$*", '"$@"', '"$*"'))
    form = rng.choice(("", "pre", "''")) + form + rng.choice(("", "post", "''"))
    return "sequence-empty-fields", ("bash", "posix", "dash"), shell_program(
        "set -- " + " ".join(shell_quote(value) for value in values),
        "IFS=" + shell_quote(rng.choice(("", " ", ":"))),
        "observe() { printf 'n=%s' \"$#\"; for v do printf '<%s>' \"$v\"; done; echo; }",
        "observe " + form)


def shell_expand_slice_effects(rng):
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
            command = "eval " + shell_quote(body) + "; echo caller:$?"
        else:
            command = ("printf '%s\\n' " + shell_quote(body) + " > source\n" +
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
    return "slice-effects-" + wrapper, ("bash", "posix"), shell_program(
        setup, "i=0; n=0", command,
        "printf 'after:%s i=%s n=%s\\n' \"$?\" \"$i\" \"$n\"")


def shell_expand_character_value(rng):
    # Cross ASCII word boundaries with two-, three- and four-byte characters,
    # combining marks and a zero-width joiner. These are valid UTF-8 strings,
    # not arbitrary labels that merely inflate the case count.
    unit = rng.choice(("é", "Ω", "界", "🌙", "e\u0301", "👩\u200d💻", "éΩ界🌙"))
    return "a" * rng.choice((0, 1, 7, 8, 15, 16, 31, 32)) + unit * rng.choice((1, 2, 5)) + rng.choice(("", "z", "/é"))


def shell_expand_character_length(rng):
    value = shell_expand_character_value(rng)
    shape = rng.choice(("scalar", "positional", "array", "nameref"))
    modes = shell_ALL if shape in ("scalar", "positional") else ("bash", "posix")
    if shape == "scalar":
        setup, operand = "x=" + shell_quote(value), "x"
    elif shape == "positional":
        setup, operand = "set -- " + shell_quote(value), "1"
    elif shape == "array":
        setup, operand = "a=([3]=" + shell_quote(value) + ")", "a[3]"
    else:
        setup, operand = "x=" + shell_quote(value) + "; declare -n n=x", "n"
    return "character-length", modes, shell_program(setup,
        "printf 'length:%s:<%s>\\n' \"${#" + operand + "}\" \"${" + operand + "}\"")


def shell_expand_character_slice(rng):
    value = shell_expand_character_value(rng)
    offset = rng.choice((-99, -8, -3, -1, 0, 1, 2, 7, 8, 15, 16, 33, 99))
    length = rng.choice(("", ":0", ":1", ":2", ":8", ":99", ":-1"))
    return "character-slice", ("bash", "posix"), shell_program(
        "x=" + shell_quote(value),
        "printf '<%s>\\n' \"${x: " + str(offset) + length + "}\"",
        "printf 'after:%s\\n' \"$?\"")


def shell_expand_character_patterns(rng):
    value = rng.choice(("é", "Ω界", "éxΩ", "aé界z", "🌙🌙", "e\u0301"))
    pattern = rng.choice(("?", "??", "???", "*?", "?*", "*?Ω", "é?*", "*界*"))
    return "character-patterns", shell_ALL, shell_program(
        "x=" + shell_quote(value),
        f"case \"$x\" in {pattern}) echo match;; *) echo no;; esac",
        "printf '<%s>' \"${x#" + pattern + "}\" \"${x##" + pattern +
        "}\" \"${x%" + pattern + "}\" \"${x%%" + pattern + "}\"; echo")


def shell_expand_character_glob(rng):
    pattern = rng.choice(("u_?", "u_??", "u_?*", "u_*?", "u_?x", "u_é?"))
    return "character-glob", shell_ALL, shell_program(
        ": > u_é; : > u_Ω; : > u_界x; : > u_🌙; : > u_éx",
        f"set -- {pattern}; printf '<%s>\\n' \"$@\"")


def shell_expand_bracket_match(rng):
    # ASCII membership, ranges, quoting and class predicates have different
    # folding rules. Unicode sets/ranges have their own family below; Unicode
    # class predicates and case mapping need locale tables, not byte folding.
    # Observe the same generated pattern through both matcher callers.
    value = rng.choice(("a", "A", "b", "B", "c", "Z", "z", "_", "-", "]",
                        "|", ")", "aa", ""))
    pattern = rng.choice(("[!A]", "[^a-c]", "[a-C]", "[B-a]", "[A-z]",
                          "[[:lower:]]", "[![:upper:]]", "[[:digit:]A]",
                          "[a-\\c]", "[\\--0]", "[]a]", "[!]]", "[|)]",
                          "[éΩ]", "[!é]", "[Α-Ω]", "[🌙-🌟]", "*[éΩ]?"))
    fold = rng.choice(("-s", "-u"))
    return "bracket-match", ("bash", "posix"), shell_program(
        f"shopt {fold} nocasematch", "x=" + shell_quote(value), "p=" + shell_quote(pattern),
        'case "$x" in $p) echo case:yes;; *) echo case:no;; esac',
        '[[ $x == $p ]]; printf "test:%s\\n" "$?"',
        '[[ $x != $p ]]; printf "inverse:%s\\n" "$?"')


def shell_expand_extended_match(rng):
    head = rng.choice(("@", "?", "*", "+", "!"))
    body = rng.choice(("foo|bar", "|foo", "", "[|)]|foo", "[!A]|b",
                       "[[:lower:]]|z", "[éΩ]|界", "@(a|b)|c", "a|aa"))
    prefix, suffix = rng.choice((("", ""), ("x", "y"), ("*", "?")))
    value = rng.choice(("", "FOO", "bar", "A", "B", "c", "|", ")", "é",
                        "Ω", "界", "aa", "aaaa", "xFOOy", "xy", "xAy"))
    # Bash accepts an empty subject for *!(a)? (even *!(a)x), discarding
    # the required suffix. Keep that oracle quirk in the explicit divergence
    # regression; the differential family checks meaningful suffix matches.
    if head == "!" and prefix == "*" and not value:
        prefix = ""
    if "[:" in body and not value.isascii():
        value = "A"  # This family covers ASCII predicates, not locale tables.
    fold = rng.choice(("-s", "-u"))
    return "extended-match", ("bash", "posix"), shell_program(
        f"shopt -s extglob; shopt {fold} nocasematch",
        "x=" + shell_quote(value), "p=" + shell_quote(prefix + head + "(" + body + ")" + suffix),
        'case "$x" in $p) echo case:yes;; *) echo case:no;; esac',
        '[[ $x == $p ]]; printf "test:%s\\n" "$?"',
        'shopt -u extglob; [[ $x == $p ]]; printf "implicit:%s\\n" "$?"')


def shell_expand_character_bracket(rng):
    value = rng.choice(("é", "Ω", "Α", "界", "🌙", "🌟", "éx", "aΩ", "a", ""))
    pattern = rng.choice(("[éΩ]", "[!éΩ]", "[Α-Ω]", "[🌙-🌟]", "[éΩ]?",
                          "*[éΩ]", "[!🌙]*", "[aé]", "[\\é-\\ê]"))
    # Arch dash's pathname matcher rejects multibyte range endpoints although
    # its case/trim matcher accepts them. Bash supplies the character-range
    # oracle; retain dash for literal/negated Unicode sets.
    modes = ("bash", "posix") if "-" in pattern else shell_ALL
    return "character-bracket", modes, shell_program(
        "x=" + shell_quote(value), "p=" + shell_quote(pattern),
        'case "$x" in $p) echo match;; *) echo no;; esac',
        'printf "<%s>" "${x#$p}" "${x##$p}" "${x%$p}" "${x%%$p}"; echo',
        ': > u_é; : > u_Ω; : > u_Α; : > u_界; : > u_🌙; : > u_🌟; : > u_a',
        'set -- u_$p; printf "<%s>\\n" "$@"')


def shell_expand_composed_pattern(rng, depth=2):
    """A bounded grammar, not a fixed menu of complete patterns."""
    leaves = ("a", "B", "é", "Ω", "?", "*", "[aBé]", "[!Ω]", "")
    if not depth or rng.randrange(3) == 0:
        return rng.choice(leaves)
    if rng.randrange(2):
        return shell_expand_composed_pattern(rng, depth - 1) + shell_expand_composed_pattern(rng, depth - 1)
    # Negative groups are varied separately, including their Bash empty-tail
    # divergence. Nullable positive groups can be composed without that quirk.
    head = rng.choice(("@", "?", "*", "+"))
    return head + "(" + "|".join(shell_expand_composed_pattern(rng, depth - 1)
                                 for _ in range(rng.randrange(1, 4))) + ")"


def shell_expand_pattern_composition(rng):
    value = "".join(rng.choice(("a", "A", "b", "B", "é", "Ω"))
                    for _ in range(rng.randrange(9)))
    pattern = shell_expand_composed_pattern(rng)
    fold = rng.choice(("-s", "-u"))
    return "pattern-composition", ("bash", "posix"), shell_program(
        f"shopt -s extglob; shopt {fold} nocasematch",
        "x=" + shell_quote(value), "p=" + shell_quote(pattern),
        'case "$x" in $p) echo case:yes;; *) echo case:no;; esac',
        '[[ $x == $p ]]; printf "test:%s\\n" "$?"')


def shell_expand_pattern_replacement(rng):
    value = rng.choice(("", "FOOfoo", "aBaB", "éΩé", "aéBΩ", "🌙é🌟", "Ω"))
    pattern = rng.choice(("foo", "a", "B", "?", "[!é]", "[éΩ]", "[a-C]",
                          "?*", "@(foo|B)", "@()", "+(|a)", "*Ω", "Ω*"))
    anchor = rng.choice(("", "#", "%"))
    operation = rng.choice(("/", "//"))
    replacement = rng.choice(("X", "", "[&]", "xy"))
    fold = rng.choice(("-s", "-u"))
    return "pattern-replacement", ("bash", "posix"), shell_program(
        f"shopt -s extglob; shopt {fold} nocasematch",
        "x=" + shell_quote(value), "p=" + shell_quote(anchor + pattern),
        "r=" + shell_quote(replacement),
        'printf "<%s>\\n" "${x' + operation + '$p/$r}"',
        'printf "source:<%s>\\n" "$x"')


def shell_expand_held_scanner(rng):
    # The same quoting/substitution boundaries reach several syntax scanners.
    # Delimiters in the payload must never become delimiters of the caller.
    payload = rng.choice(("a b", "a,b", "a/b", "a:b", "a;b", "a'b", "a)b"))
    word = rng.choice((
        '$(printf %s ' + shell_quote(payload) + ')',
        '`printf %s ' + shell_quote(payload) + '`',
        '"pre$(printf "%s" ' + shell_quote(payload) + ')post"',
        '${missing:-' + shell_quote(payload) + '}',
        "$'" + payload.replace("'", "\\'") + "'",
    ))
    site = rng.choice(("array", "brace", "replace", "conditional"))
    if site == "array":
        observe = 'a=(' + word + ' tail); printf "<%s>\\n" "${a[@]}"'
    elif site == "brace":
        observe = 'printf "<%s>\\n" {' + word + ',tail}'
    elif site == "replace":
        observe = 'printf "<%s>\\n" "${x/' + word + '/Q}"'
    else:
        observe = '[[ ' + word + ' == ' + word + ' ]]; echo status:$?'
    return "held-scanner-" + site, ("bash", "posix"), shell_program(
        "unset missing", "x=" + shell_quote("pre" + payload + "post " + payload), observe)


def shell_expand_ansi_escape_transition(rng):
    token = rng.choice(("\\" + format(rng.randrange(512), "03o"),
                        "\\x" + format(rng.randrange(256), "02x"),
                        "\\c" + rng.choice(("@", "?", "A", "[", "\\\\")),
                        "\\x", "\\q", "\\e", "\\E", "\\n", "\\'", "\\\\"))
    raw = (rng.choice(("", "lead", " " * 31, "x" * 257)) +
           token * rng.choice((1, 2, 7, 31)) +
           rng.choice(("", "tail", "\\nend", "`tick", "\\0hidden")))
    return "ansi-escape-transition", ("bash", "posix"), shell_program(
        "x=" + shell_quote(raw),
        'printf "E:<%s>\\n" "pre${x@E}post"',
        "printf 'S:<%s>\\n' pre$'" + raw + "'post")


def shell_expand_ansi_quote_transition(rng):
    # ASCII encoders must agree byte-for-byte, as well as round-trip. High
    # bytes have separate locale-dependent quoting policy, not this oracle.
    byte = rng.randrange(1, 128)
    payload = (rng.choice(("", "x" * 31, "word" * 64)) +
               ("\\%03o" % byte) * rng.choice((1, 2, 7, 31)) +
               rng.choice(("", "tail", "\\nend", "\\\\quote")))
    return "ansi-quote-transition", ("bash", "posix"), shell_program(
        "v=$'" + payload + "'",
        'printf "Q:<%s>\\n" "${v@Q}"; printf "P:<%q>\\n" "$v"',
        'declare -p v; eval "back=${v@Q}"; printf "R:<%s>\\n" "$back"')






FAMILIES = (
    shell_lex_quotes,
    shell_lex_substitution,
    shell_lex_heredoc,
    shell_lex_comment_boundary,
    shell_lex_operators,
    shell_lex_redirection,
    shell_lex_syntax_mutation,
    shell_lex_stray_terminator,
    shell_lex_function_metadata,
    shell_lex_nested_syntax,
    shell_exec_special_prefix,
    shell_exec_command_exception,
    shell_exec_disabled_special,
    shell_exec_control_status,
    shell_exec_errexit_context,
    shell_exec_child_exit,
    shell_exec_inherited_exit,
    shell_exec_rhs_status,
    shell_exec_function_scope,
    shell_exec_nested_loop_items,
    shell_exec_loop_control_transition,
    shell_exec_function_serialization,
    shell_exec_function_heredoc_serialization,
    shell_exec_function_control_heredoc_serialization,
    shell_exec_redirect_cardinality,
    shell_exec_pipeline_context,
    shell_exec_subshell_scope,
    shell_exec_composed_status,
    shell_exec_deep_control,
    shell_exec_special_scope,
    shell_exec_descriptor_order,
    shell_exec_readonly_scope,
    shell_expand_parameter_default,
    shell_expand_parameter_trim,
    shell_expand_splitting_and_glob,
    shell_expand_arithmetic,
    shell_expand_arithmetic_side_effect,
    shell_expand_arithmetic_precedence,
    shell_expand_arithmetic_comma,
    shell_expand_arithmetic_array,
    shell_expand_substitution_status,
    shell_expand_indexed_array,
    shell_expand_associative_array,
    shell_expand_nameref,
    shell_expand_local_scope,
    shell_expand_indirect_special,
    shell_expand_substring,
    shell_expand_sequence_slice,
    shell_expand_array_transform,
    shell_expand_array_sequence_transition,
    shell_expand_arithmetic_simple_transition,
    shell_expand_sequence_empty_fields,
    shell_expand_slice_effects,
    shell_expand_character_length,
    shell_expand_character_slice,
    shell_expand_character_patterns,
    shell_expand_character_glob,
    shell_expand_bracket_match,
    shell_expand_extended_match,
    shell_expand_character_bracket,
    shell_expand_pattern_composition,
    shell_expand_pattern_replacement,
    shell_expand_held_scanner,
    shell_expand_ansi_escape_transition,
    shell_expand_ansi_quote_transition,
)

UTILITIES = ()
