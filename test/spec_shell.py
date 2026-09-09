"""The shell: its language and its process, compared with bash and dash.

FAMILIES are seeded generators of whole programs, one per corner of the
language: quoting, every parameter form, arithmetic, splitting, pathname and
brace expansion, redirections, pipelines, compound commands, functions,
traps, subshells, job control in scripts and the reader's own boundaries.
Each returns (family, modes, script); the engine runs the script through the
reference shell of each mode and through ours under the same name.

UTILITIES are grammars for the process surface, wrapped into scripts: every
startup flag combined with every way a program is handed over; every `set`
option against behaviour probes; the names argv[0] may carry; the exact envp
a parent can send; bash privileged mode; and, through a python pty driver,
job control at a terminal, interactive recovery and a vanished terminal.
The relaunched shell is reached through /proc/self/exe, so the reference
relaunches itself and ours relaunches ours.

Two small policy utilities hold the deliberate differences pinned by name.
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






# ----------------------------------------------------------------------------
#       Shared helpers for the whole module.
# ----------------------------------------------------------------------------

def shell_quote(text):
    return "'" + text.replace("'", "'\"'\"'") + "'"


def shell_program(*lines):
    """Keep setup, mutation and observation separable for line shrinking."""
    return "\n".join(lines) + "\n"


def shell_words(argv):
    return " ".join(shlex.quote(word) for word in argv)


# The shell under test relaunched from inside a case. /proc/self/exe resolves
# in the process that execs it, so a link called bash or dash next to the
# fixture files is the running shell under that name: /bin/bash for the
# reference and ours for the candidate, personality chosen the same way the
# outer shell's was. The link is identical on both sides, so it is no effect.
shell_SELF = ('if [ -n "${BASH_VERSION+x}" ]; then shell_me=bash; else shell_me=dash; fi\n'
              'ln -s /proc/self/exe "./$shell_me" 2>/dev/null\n')

# The same binary handed to a python driver, which cannot use the link (its
# own /proc/self/exe is python): the shell names its executable through $$.
shell_SELF_PATH = '"$(readlink /proc/$$/exe)" "$shell_me"'

# Scripts fed on standard input to the process-surface utilities.
INPUTS["shell_probe"] = (b'printf "<%s>" "$-" "$0" "$#" "$@"; echo\n'
                         b'echo "second:$?"\n'
                         b'read shell_line; echo "read:[$shell_line]"\n'
                         b'third line for read\n'
                         b'echo last\n')
INPUTS["shell_onecmd"] = b"echo one; echo two\necho three\n"
INPUTS["shell_onecmd_compound"] = b"if true; then\necho one\nfi\necho forbidden\n"
INPUTS["shell_onecmd_heredoc"] = b"cat <<EOF\none\nEOF\necho forbidden\n"
INPUTS["shell_onecmd_blank"] = b"\n# first line\necho one\necho forbidden\n"
INPUTS["shell_onecmd_false"] = b"false\necho forbidden\n"
INPUTS["shell_onecmd_toggle"] = b"set +t; echo one\necho two\nset -t; echo three\necho forbidden\n"

# Probe bodies the startup, set and name grammars run: each is sensitive to
# a different startup option, so a single divergence names its option.
shell_PROBE_FLAGS = 'printf "<%s>" "$-" "$0" "$#" "$@"; echo'
shell_PROBE_ERREXIT = 'echo one; false; echo two; (exit 3); echo three; if false; then :; fi; echo four'
shell_PROBE_NOUNSET = 'echo "[${shell_undefined_probe}]"; echo after; echo "[${shell_undefined_probe-default}]"'
shell_PROBE_CLOBBER = ('echo a > made; echo b > made; echo "over=$?"; echo c >| made; echo "force=$?"; '
                       'echo d >> made; echo "append=$?"; cat made; : > /dev/null; echo "devnull=$?"')
shell_PROBE_GLOB = 'echo a.tx? [ab].txt *.txt "*.txt" no*such; set -- *; echo "$#"'
shell_PROBE_EXPORT = 'shell_v=1; /bin/sh -c '"'"'echo "${shell_v-unset}"'"'"'; export | grep -c shell_v'
# Brace expansion and bash's arithmetic extensions are provided under every
# name; against dash that is a pinned policy (policy_dash), not this probe's.
shell_PROBE_BRACE = ('if [ -n "${BASH_VERSION+x}" ]; then echo {a,b} {1..3} pre{x,y}post; echo $((2 ** 3)) $((10#08)); '
                     'else echo posix-shell; fi')
shell_PROBE_MONITOR = 'sleep 0.1 & wait; echo "bg=$?"; case $- in *m*) echo monitor;; *) echo no-monitor;; esac'
shell_PROBE_KEYWORD = 'shell_x=old; echo a shell_x=new b; echo "${shell_x}"; /bin/sh -c '"'"'echo "${shell_x-unset}"'"'"' one shell_x=env two'
shell_PROBE_READ = 'read shell_line; echo "read:[$shell_line]"; echo "status:$?"'
shell_PROBE_OPTIONS = 'set -o; set +o'
shell_PROBE_ALIASES = 'alias shell_a="echo aliased"; eval shell_a; shell_a 2>/dev/null; echo "alias=$?"'
shell_PROBE_HASH = 'hash -r; command -v true; hash 2>&1 | wc -l'
shell_PROBE_PERSONALITY = ('echo "${BASH_VERSION:+B}:${POSIXLY_CORRECT-unset}:${SHELLOPTS:+S}"; '
                           'shopt -q extglob 2>/dev/null; echo "shopt=$?"; '
                           'local shell_l=1 2>/dev/null; echo "local=$?"; type shopt 2>/dev/null | wc -l')
shell_PROBE_ONECMD = 'echo one; echo two; case $- in *t*) echo onecmd;; esac'
shell_PROBE_TRACE = 'shell_x=1; echo "$shell_x"; for shell_i in a b; do :; done; case x in x) ;; esac; f() { :; }; f'

shell_PROBES = (shell_PROBE_FLAGS, shell_PROBE_ERREXIT, shell_PROBE_NOUNSET, shell_PROBE_CLOBBER,
                shell_PROBE_GLOB, shell_PROBE_EXPORT, shell_PROBE_BRACE, shell_PROBE_MONITOR,
                shell_PROBE_KEYWORD, shell_PROBE_READ, shell_PROBE_OPTIONS, shell_PROBE_ALIASES,
                shell_PROBE_HASH, shell_PROBE_PERSONALITY, shell_PROBE_ONECMD, shell_PROBE_TRACE)

# What the relaunched shell said on standard error, minus the diagnostics
# whose wording is each shell's own: trace lines (PS4 is set to a marker) and
# verbose echoes of the input are the shape under test; a diagnostic counts
# only as present.
shell_STDERR_REPLAY = ("grep -v '^[a-zA-Z./_-]*: ' err.txt\n"
                       "grep -q '^[a-zA-Z./_-]*: ' err.txt && echo diagnostic\n")

shell_SET_O_NAMES = ("allexport", "braceexpand", "emacs", "errexit", "errtrace", "functrace",
                     "hashall", "histexpand", "history", "ignoreeof", "interactive-comments",
                     "keyword", "monitor", "noclobber", "noexec", "noglob", "nolog", "notify",
                     "nounset", "onecmd", "physical", "pipefail", "posix", "privileged",
                     "verbose", "vi", "xtrace", "bogus", "")


# ----------------------------------------------------------------------------
#       Startup: every flag the shell takes on its command line, combined,
#       with every way a program can be handed to it.
# ----------------------------------------------------------------------------

def shell_startup_files():
    return ("printf 'echo startup:$0:$#:${1-none}; shell_value=loaded\\n' > start\n"
            "printf 'echo literal-name\\n' > 'start file'; cp 'start file' 'start*'; cp 'start file' '$FILE'\n"
            "printf 'echo startup; exit 7\\n' > exit-start\n"
            "printf 'set -e\\nfalse\\necho forbidden\\n' > errexit-start\n"
            "printf 'echo rc-file\\n' > rc\n"
            "printf '%s\\n' " + shlex.quote(shell_PROBE_FLAGS) + " > probe.sh\n"
            "printf 'echo one\\nread shell_line\\nline for read\\necho \"[$shell_line]\"\\n' > reading.sh\n"
            "mkdir -p dir\n")


def shell_startup_script(argv, stdin):
    prefix = []
    words = []
    for word in argv:
        if word.startswith("--env="):
            prefix.append(word[len("--env="):])
        else:
            words.append(word)
    # A prefix assignment, not env(1): env would be the process that opens
    # ./bash, and /proc/self/exe answers whoever opens it -- so env re-ran
    # itself and every case in this family compared two usage errors.
    return (shell_SELF + shell_startup_files() +
            "PS4='TRACE ' " + " ".join(prefix) + (" " if prefix else "") +
            '"./$shell_me" ' + shell_words(words) + " 2>err.txt\n"
            'echo "status=$?"\n' + shell_STDERR_REPLAY +
            'rm -f err.txt "./$shell_me"\n')


shell_STARTUP = Utility(
    "startup",
    options=(
        Option("-e"), Option("-u"), Option("-x"), Option("-v"), Option("-n"), Option("-f"),
        Option("-C"), Option("-a"), Option("-b"), Option("-m"), Option("-h"), Option("-k"),
        Option("-t"), Option("-i"), Option("-p"), Option("-B"), Option("-H"), Option("-P"),
        Option("-E"), Option("-T"), Option("-r"), Option("-Z"), Option("+e"), Option("+m"),
        Option("+B"), Option("+x"),
        Option("-o", values=shell_SET_O_NAMES, repeat=True),
        Option("+o", values=("errexit", "monitor", "posix", "braceexpand", "interactive-comments",
                             "histexpand", "bogus")),
        Option("-O", values=("extglob", "expand_aliases", "nullglob", "xpg_echo", "bogus")),
        Option("+O", values=("expand_aliases", "interactive_comments")),
        Option("--posix"), Option("--noprofile"), Option("--norc"), Option("--noediting"),
        Option("--rcfile", values=("rc", "missing")), Option("--init-file", values=("rc",)),
        Option("--verbose"), Option("--debugger"), Option("--restricted"), Option("--bogus"),
        Option("--env", values=("BASH_ENV=start", "BASH_ENV='start file'", "BASH_ENV='start*'",
                                "BASH_ENV='$FILE'", "BASH_ENV=missing", "BASH_ENV=",
                                "BASH_ENV=exit-start", "BASH_ENV=errexit-start", "BASH_ENV=~/start",
                                "BASH_ENV='$(exit 7)'", "ENV=start", "ENV='start file'",
                                "POSIXLY_CORRECT=1", "POSIXLY_CORRECT=", "IFS=:", "PS1='$ '", "HOME=/nonexistent",
                                "PATH=", "CDPATH=dir"),
               attached=True, repeat=True),
    ),
    operands=(
        ("-c", shell_PROBE_FLAGS),
        ("-c", shell_PROBE_FLAGS, "name0", "one", "two words", ""),
        ("-c", shell_PROBE_ERREXIT), ("-c", shell_PROBE_NOUNSET), ("-c", shell_PROBE_CLOBBER),
        ("-c", shell_PROBE_GLOB), ("-c", shell_PROBE_EXPORT), ("-c", shell_PROBE_BRACE),
        ("-c", shell_PROBE_MONITOR), ("-c", shell_PROBE_KEYWORD), ("-c", shell_PROBE_READ),
        ("-c", shell_PROBE_OPTIONS), ("-c", shell_PROBE_ALIASES), ("-c", shell_PROBE_HASH),
        ("-c", shell_PROBE_PERSONALITY), ("-c", shell_PROBE_ONECMD), ("-c", shell_PROBE_TRACE),
        ("-c", "echo \"$shell_value\"; echo \"$0:$#:$*\"", "named", "arg"),
        ("-c",), ("-c", ""), ("-c", "\n\n"), ("-c", "# comment only"), ("-c", ":"), ("-c", "true"),
        ("-c", "false"), ("-c", " false # trailing"), ("-c", ":#word"),
        ("-c", "echo one\necho two\nexit 5\necho three"),
        ("-c", "--", shell_PROBE_FLAGS), ("--", "-c", shell_PROBE_FLAGS),
        ("-ec", shell_PROBE_ERREXIT), ("-ce", shell_PROBE_ERREXIT), ("-nc", shell_PROBE_FLAGS),
        ("-xvc", shell_PROBE_TRACE), ("-tc", shell_PROBE_ONECMD), ("-kc", shell_PROBE_KEYWORD),
        ("-ic", shell_PROBE_FLAGS), ("-pc", shell_PROBE_FLAGS), ("-lc", shell_PROBE_FLAGS),
        ("-eu", "-c", shell_PROBE_NOUNSET), ("-e", "+e", "-c", shell_PROBE_ERREXIT),
        ("probe.sh",), ("probe.sh", "one", "two words"), ("./probe.sh", "-x", "--", "-c"),
        ("reading.sh",), ("-", "probe.sh"), ("--", "probe.sh"), ("missing.sh",), ("dir",),
        ("-s",), ("-s", "one", "two"), ("-s", "--", "-x", "-y"), ("+s", "probe.sh"), (),
        ("-o",), ("-o", "bogus", "-c", shell_PROBE_FLAGS), ("-c", "-e", shell_PROBE_ERREXIT),
    ),
    stdin=("shell_probe", "empty", "shell_onecmd", "shell_onecmd_compound", "shell_onecmd_heredoc",
           "shell_onecmd_blank", "shell_onecmd_false", "shell_onecmd_toggle", "text"),
    fixture="shell",
    modes=shell_ALL,
    script=shell_startup_script,
    max_flags=5,
    timeout=10.0,
)


# ----------------------------------------------------------------------------
#       set: every option, alone and together, against behaviour probes run
#       in the same shell afterwards.
# ----------------------------------------------------------------------------

def shell_set_script(argv, stdin):
    flags = []
    probe = shell_PROBE_FLAGS
    positional = []
    at = 0
    while at < len(argv):
        word = argv[at]
        if word == "--probe":
            probe = shell_PROBES[int(argv[at + 1])]
            at += 2
            continue
        if word == "--":
            positional = argv[at:]
            break
        flags.append(word)
        at += 1
    # A bare `set` lists every variable, which is the builtin's own surface
    # and the environment's; the option behaviour needs at least one word.
    return shell_program(
        "echo start",
        ("set " + shell_words(flags + positional) + " 2>err.txt") if flags or positional else ": > err.txt",
        'echo "set=$?"',
        "PS4='TRACE '",
        probe,
        'echo "probe=$?"',
        "exec 2>err2.txt",
        "printf 'echo traced\\n' > traced.sh; . ./traced.sh",
        "exec 2>&1",
        "cat err.txt err2.txt | grep -v '^[a-zA-Z./_-]*: ' ; rm -f err.txt err2.txt traced.sh")


shell_SET = Utility(
    "set_options",
    options=(
        Option("-e"), Option("-u"), Option("-x"), Option("-v"), Option("-n"), Option("-f"),
        Option("-C"), Option("-a"), Option("-b"), Option("-m"), Option("-h"), Option("-k"),
        Option("-t"), Option("-p"), Option("-B"), Option("-H"), Option("-P"), Option("-E"),
        Option("-T"), Option("-i"), Option("-Z"), Option("-"), Option("+e"), Option("+u"),
        Option("+f"), Option("+B"), Option("+m"), Option("+H"), Option("+h"), Option("+C"),
        Option("-o", values=shell_SET_O_NAMES, repeat=True),
        Option("+o", values=shell_SET_O_NAMES, repeat=True),
        Option("--probe", values=tuple(str(n) for n in range(len(shell_PROBES)))),
        Option("--", values=("one", "-x")),
    ),
    operands=((),),
    stdin=("text", "empty"),
    fixture="shell",
    modes=shell_ALL,
    script=shell_set_script,
    max_flags=5,
    timeout=8.0,
)


# ----------------------------------------------------------------------------
#       argv[0]: the names one binary answers to. A bash-named link is
#       compared with bash; every POSIX name with dash. A login name reads
#       the system profile, so it is given --noprofile.
# ----------------------------------------------------------------------------

def shell_name_script(argv, stdin):
    name = argv[-1]
    flags = list(argv[:-1])
    if name.startswith("-"):
        flags = ["--noprofile"] + flags if name == "-bash" else flags
    probe = shell_PROBE_PERSONALITY
    for word in list(flags):
        if word.startswith("--probe="):
            probe = shell_PROBES[int(word[len("--probe="):])]
            flags.remove(word)
    link = shlex.quote(name.lstrip("-"))
    tail = " " + shell_words(flags) + " -c " + shlex.quote(probe) + " name0 a b 2>err.txt"
    if name.startswith("-"):
        # A login name needs exec -a, which only the bash family has.
        launch = "(exec -a " + shlex.quote(name) + " ./" + link + tail + ")"
    else:
        launch = "./" + link + tail
    return shell_program(
        "ln -s /proc/self/exe ./" + link + " 2>/dev/null",
        launch,
        'echo "status=$?"',
        shell_STDERR_REPLAY.rstrip("\n"),
        "rm -f err.txt ./" + link)


shell_NAME_OPTIONS = (
    Option("--posix"), Option("-e"), Option("-u"), Option("-x"), Option("-C"), Option("-f"),
    Option("-o", values=("posix", "errexit", "nounset", "braceexpand", "bogus")),
    Option("--probe", values=tuple(str(n) for n in range(len(shell_PROBES))), attached=True),
)

shell_NAMES_BASH = Utility(
    "names_bash",
    options=shell_NAME_OPTIONS,
    operands=(("bash",), ("-bash",), ("rbash",)),
    stdin=("text",),
    fixture="shell",
    modes=shell_BASH,
    script=shell_name_script,
    timeout=8.0,
)

shell_NAMES_POSIX = Utility(
    "names_posix",
    options=shell_NAME_OPTIONS,
    operands=(("dash",), ("sh",), ("moonwater",), ("mwsh",)),
    stdin=("text",),
    fixture="shell",
    modes=("dash",),
    script=shell_name_script,
    timeout=8.0,
)


# ----------------------------------------------------------------------------
#       Environment: the exact envp the shell is started with, built by hand
#       through execve so duplicates, invalid names and empty entries reach
#       the shell the way a careless parent would send them.
# ----------------------------------------------------------------------------

shell_ENV_DRIVER = r'''
import ctypes, os, sys
shell, name = sys.argv[1], sys.argv[2]
entries = []
for item in sys.argv[3:]:
    if item == "dup":
        entries += ["MW_DUPLICATE=first", "MW_DUPLICATE=last"]
    elif item == "many":
        entries += ["MW_MANY_%d=value" % n for n in range(600)]
    elif item == "huge":
        entries += ["MW_HUGE=" + "x" * 50000]
    elif item == "empty-entry":
        entries += [""]
    elif item == "path":
        entries += ["PATH=" + os.environ["PATH"]]
    else:
        entries += [item]
probe = os.environ["SHELL_PROBE"]
argv = [name, "-c", probe]
Argv = (ctypes.c_char_p * (len(argv) + 1))(*[a.encode() for a in argv], None)
Envp = (ctypes.c_char_p * (len(entries) + 1))(*[e.encode() for e in entries], None)
libc = ctypes.CDLL(None, use_errno=True)
sys.stdout.flush()
libc.execve(shell.encode(), Argv, Envp)
print("execve failed:", ctypes.get_errno())
sys.exit(99)
'''

# What the parent sent, not what the shell invents when nothing was sent:
# PATH, SHELL, OPTIND, IFS, PS4 and _ are each shell's own startup defaults
# and differ by design (ours name the image root), so they are left out and
# pinned once rather than reported by every case in this family.
shell_ENV_PROBE = ('printf "<%s>" "${MW_DUPLICATE-unset}" "${MW_HUGE:+huge}" "${#MW_HUGE}" "${MW_MANY_599-unset}" '
                   '"${HOME-unset}" "${MW_PLAIN-unset}" "${POSIXLY_CORRECT-unset}"; echo; '
                   'unset MW_DUPLICATE; MW_DUPLICATE=owned; echo "$MW_DUPLICATE"; '
                   'set | grep -c "^MW_MANY_[0-9][0-9]*=" ; env | grep -c "^MW_MANY_"; '
                   'env | LC_ALL=C sort | grep -E "^(MW_|HOME=|LC_|TERM=|LINENO=|RANDOM=|PPID=|UID=|EUID=|POSIXLY_)" | grep -v -e "^MW_MANY_" -e "^MW_HUGE="; '
                   'echo "~=${HOME:+home}"; /bin/sh -c \'echo "child:${MW_DUPLICATE-unset}"\' 2>/dev/null; '
                   'set -- $IFS; echo "ifs-fields=$#"; case $- in *e*|*f*|*x*) echo option-imported;; *) echo options-ignored;; esac; '
                   'if shell_imported 2>/dev/null; then echo function-imported; else echo function-ignored; fi')


def shell_env_script(argv, stdin):
    return (shell_SELF +
            "SHELL_PROBE=" + shlex.quote(shell_ENV_PROBE) + " python3 - " + shell_SELF_PATH + " " +
            shell_words(argv) + " <<'PY' 2>err.txt\n" + shell_ENV_DRIVER + "PY\n"
            'echo "status=$?"\n' + shell_STDERR_REPLAY + 'rm -f err.txt "./$shell_me"\n')


shell_ENVIRONMENT = Utility(
    "environment",
    options=(
        Option("--entry", values=("dup", "many", "huge", "empty-entry", "path", "1bad=x", "bad-name=x",
                                  "=x", "noequals", "a=b=c", "IFS=:", "IFS=", "PS4=TRACE ",
                                  "HOME=/nonexistent", "OPTIND=5", "SHLVL=abc", "SHLVL=3", "_=inherited",
                                  "PWD=/nonexistent", "OLDPWD=/tmp", "POSIXLY_CORRECT=1",
                                  "SHELLOPTS=xtrace:noglob", "BASHOPTS=nullglob",
                                  "BASH_FUNC_shell_imported%%=() { echo imported; }",
                                  "BASH_FUNC_shell_bad%%=() { echo", "LINENO=99", "RANDOM=7", "PPID=1",
                                  "UID=0", "EUID=0", "BASH_ENV=missing", "ENV=missing",
                                  "LC_ALL=C.UTF-8", "TERM=dumb", "PS1=P ", "MW_PLAIN=v"),
               attached=True, repeat=True),
    ),
    operands=((),),
    stdin=("empty",),
    fixture="shell",
    modes=shell_ALL,
    script=shell_env_script,
    max_flags=6,
    timeout=8.0,
)


# ----------------------------------------------------------------------------
#       Privileged mode: a bash-named shell started with real and effective
#       IDs that differ. The identities are made through sudo and python's
#       setresuid in a disposable child; the driver's own credentials never
#       change. Without passwordless sudo both sides fail the same way.
# ----------------------------------------------------------------------------

shell_PRIVILEGE_DRIVER = r'''
import os, sys
shell, name, identity, root, startup, probe = sys.argv[1:7]
rest = sys.argv[7:]
extra = rest[:rest.index("--")]
flags = rest[rest.index("--") + 1:]
if identity == "mismatch":
    os.setresgid(65534, 0, 0)
    os.setresuid(65534, 0, 0)
elif identity == "saved":
    os.setresgid(65534, 65534, 0)
    os.setresuid(65534, 65534, 0)
env = {"PATH": "/usr/bin:/bin", "HOME": root, "ROOT": root, "LC_ALL": "C",
       "BASH_ENV": startup, "CDPATH": root + "/search"}
for item in extra:
    key, _, value = item.partition("=")
    env[key] = value
sys.stdout.flush()
os.execve(shell, [name] + flags + ["-c", probe], env)
'''

shell_PRIVILEGE_SHOW = ('/usr/bin/python3 -c "import os; print(os.getresuid(), os.getresgid())"')
shell_PRIVILEGE_PROBES = (
    shell_PRIVILEGE_SHOW + '; printf "%s\\n" "$-"',
    shell_PRIVILEGE_SHOW + '; set +p; ' + shell_PRIVILEGE_SHOW + '; printf "%s\\n" "$-"',
    shell_PRIVILEGE_SHOW + '; set +o privileged; ' + shell_PRIVILEGE_SHOW,
    'set -p; ' + shell_PRIVILEGE_SHOW + '; printf "%s\\n" "$-"',
    'cd place 2>/dev/null; printf "%s:%s\\n" "$?" "${PWD##*/}"',
    'set +p; cd place 2>/dev/null; printf "%s:%s\\n" "$?" "${PWD##*/}"',
    'printf "visible:%s:%s\\n" "$CDPATH" "${GLOBIGNORE-unset}"; case $- in *e*|*f*|*x*) echo option-imported;; *) echo options-ignored;; esac; '
    'if shopt -q nullglob; then echo shopt-imported; else echo shopt-ignored; fi; cd "$ROOT/glob"; printf "glob:<%s>\\n" * no-match-*; '
    'if shell_poison 2>/dev/null; then echo function-imported; else echo function-ignored; fi',
    'printf "body\\n"',
)


def shell_privileged_script(argv, stdin):
    identity = "plain"
    flags = []
    probe = shell_PRIVILEGE_PROBES[0]
    startup = ""
    extra = []
    for word in argv:
        if word.startswith("--identity="):
            identity = word.split("=", 1)[1]
        elif word.startswith("--probe="):
            probe = shell_PRIVILEGE_PROBES[int(word.split("=", 1)[1])]
        elif word.startswith("--startup="):
            startup = word.split("=", 1)[1]
        elif word.startswith("--extra="):
            extra.append(word.split("=", 1)[1])
        else:
            flags.append(word)
    runner = "" if identity == "plain" else "sudo -n "
    return (shell_SELF +
            "mkdir -p search/place glob; : > glob/one; : > glob/two; printf 'echo startup-forbidden\\n' > startup\n"
            + runner + "python3 - " + shell_SELF_PATH + " " + identity + ' "$PWD" ' +
            ('"$PWD/' + startup + '"' if startup else '""') + " " + shlex.quote(probe) + " " +
            shell_words(extra) + " -- " + shell_words(flags) + " <<'PY' 2>err.txt\n" +
            shell_PRIVILEGE_DRIVER + "PY\n"
            'echo "status=$?"\n' + shell_STDERR_REPLAY + 'rm -f err.txt "./$shell_me"\n')


shell_PRIVILEGED = Utility(
    "privileged",
    options=(
        Option("--identity", values=("plain", "mismatch", "saved"), attached=True),
        Option("-p"), Option("+p"), Option("-o", values=("privileged",)), Option("+o", values=("privileged",)),
        Option("--probe", values=tuple(str(n) for n in range(len(shell_PRIVILEGE_PROBES))), attached=True),
        Option("--startup", values=("startup", "missing"), attached=True),
        Option("--extra", values=("GLOBIGNORE=*", "SHELLOPTS=errexit:noglob:xtrace", "BASHOPTS=nullglob",
                                  "BASH_FUNC_shell_poison%%=() { echo POISON; }"),
               attached=True, repeat=True),
    ),
    operands=((),),
    stdin=("empty",),
    fixture="shell",
    modes=("bash",),
    script=shell_privileged_script,
    max_flags=5,
    timeout=10.0,
)


# ----------------------------------------------------------------------------
#       Terminals. A python driver gives the shell a pseudo-terminal of its
#       own (a session it leads, echo off, 24x80), types a sequence, and
#       prints the transcript with terminal escapes, the shell's name in
#       diagnostics and process ids taken out, then how the shell ended.
# ----------------------------------------------------------------------------

shell_PTY_DRIVER = r'''
import fcntl, os, pty, re, select, signal, struct, subprocess, sys, termios, time
shell, name = sys.argv[1], sys.argv[2]
flags = [f for f in sys.argv[3:] if f]
steps = STEPS
settings = SETTINGS
master, slave = pty.openpty()
attributes = termios.tcgetattr(slave)
attributes[3] &= ~termios.ECHO
termios.tcsetattr(slave, termios.TCSANOW, attributes)
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))

def session():
    if settings.get("session", True):
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)
    if settings.get("ignore_hup"):
        signal.signal(signal.SIGHUP, signal.SIG_IGN)

env = {"PATH": os.environ["PATH"], "HOME": os.getcwd(), "TERM": "dumb", "PS1": "$ ", "PS2": "> ",
       "LC_ALL": "C", "PS4": "+ ", "TMPDIR": os.getcwd()}
process = subprocess.Popen([name] + flags, executable=shell, stdin=slave, stdout=slave, stderr=slave,
                           close_fds=True, preexec_fn=session, env=env)
os.close(slave)
seen = bytearray()

def settle(seconds):
    stop = time.monotonic() + seconds
    while time.monotonic() < stop:
        if select.select([master], [], [], 0.05)[0]:
            try:
                seen.extend(os.read(master, 65536))
            except OSError:
                return

settle(settings.get("startup", 0.4))
for typed, pause in steps:
    if typed == b"<CLOSE>":
        break
    try:
        os.write(master, typed)
    except OSError:
        break
    settle(pause)
if any(typed == b"<CLOSE>" for typed, _ in steps):
    os.close(master)
    master = None
deadline = time.monotonic() + settings.get("linger", 3.0)
while process.poll() is None and time.monotonic() < deadline:
    if master is not None:
        settle(0.1)
    else:
        time.sleep(0.05)
if process.poll() is None:
    outcome = "still running"
    try:
        os.killpg(os.getpgid(process.pid), signal.SIGKILL)
    except OSError:
        process.kill()
    process.wait()
elif process.returncode < 0:
    outcome = "signal %d" % -process.returncode
else:
    outcome = "exit %d" % process.returncode
if master is not None:
    settle(0.1)
    os.close(master)
plain = re.sub(rb"\x1b\[[0-9;?]*[ -/]*[@-~]", b"", bytes(seen)).replace(b"\r", b"")
plain = re.sub(rb"(?m)^(?:bash|sh|dash|-bash|-sh|-dash|[^\s:]*/(?:bash|sh|dash)): ", b"", plain)
if settings.get("pids"):
    plain = re.sub(rb"\b\d{3,7}\b", b"<pid>", plain)
sys.stdout.buffer.write(plain)
sys.stdout.buffer.write(b"\n[" + outcome.encode() + b"]\n")
'''


def shell_pty_script(steps, settings, flags=()):
    """A whole case: the shell relaunched on a pty and driven through steps."""
    driver = (shell_PTY_DRIVER.replace("STEPS", repr([(t.encode() if isinstance(t, str) else t, p)
                                                      for t, p in steps]))
              .replace("SETTINGS", repr(settings)))
    return (shell_SELF +
            "python3 - " + shell_SELF_PATH + " " + shell_words(flags) + " <<'PY'\n" + driver + "PY\n"
            'rm -f "./$shell_me"\n')


def shell_terminal_jobs_script(argv, stdin):
    choice = {}
    at = 0
    while at < len(argv):
        word = argv[at]
        if at + 1 < len(argv) and not argv[at + 1].startswith("-") and word in ("--jobs", "--kill", "--disown", "--wait"):
            choice[word] = argv[at + 1]
            at += 2
        elif "=" in word:
            key, value = word.split("=", 1)
            choice[key] = value
            at += 1
        else:
            choice[word] = ""
            at += 1
    steps = [("echo START\n", 0.3)]
    if choice.get("--start", "stop") == "background":
        steps.append(("sleep 5 &\n", 0.3))
    else:
        steps.append(("sleep 5\n", 0.4))
        steps.append((b"\x1a", 0.6))
    if "--jobs" in choice:
        steps.append(("jobs %s\n" % choice["--jobs"], 0.4))
    if "--bg" in choice:
        steps.append(("bg\n", 0.4))
        steps.append(("jobs\n", 0.4))
    if "--fg" in choice:
        steps.append(("fg\n", 0.4))
        steps.append((b"\x1a", 0.6))
    if "--kill" in choice:
        steps.append(("kill %s\n" % choice["--kill"], 0.4))
    if "--wait" in choice:
        steps.append(("wait %s; echo wait=$?\n" % choice["--wait"], 0.6))
    if "--disown" in choice:
        steps.append(("disown %s\n" % choice["--disown"], 0.3))
    steps.append(("jobs\n", 0.4))
    steps.append(("echo END\n", 0.3))
    ending = choice.get("--exit", "exit")
    if ending == "eof":
        steps.append((b"\x04", 0.5))
        steps.append((b"\x04", 0.5))
    else:
        steps.append(("exit\n", 0.5))
        steps.append(("exit\n", 0.5))
    flags = []
    if "--posix" in choice:
        flags.append("--posix")
    return shell_pty_script(steps, {"pids": True, "linger": 4.0}, flags)


def shell_terminal_jobs_valid(argv):
    if "--wait" in argv:
        index = argv.index("--wait")
        killed = "--kill" in argv and argv[argv.index("--kill") + 1] in ("%1", "-TERM %1", "-KILL %1", "-9 %1", "-s TERM %1")
        if not killed:
            return False
    return True


shell_TERMINAL_JOBS = Utility(
    "terminal_jobs",
    options=(
        Option("--start", values=("stop", "background"), attached=True),
        Option("--jobs", values=("", "-l", "-p", "-r", "-s", "-n", "%1", "%%", "%+", "%-", "%sleep", "%?eep", "%9")),
        Option("--bg"), Option("--fg"),
        Option("--kill", values=("%1", "-CONT %1", "-STOP %1", "-TERM %1", "-KILL %1", "-9 %1", "-s TERM %1",
                                 "%2", "-l", "-l 9", "-l 143", "%sleep", "%?eep")),
        Option("--wait", values=("%1", "$!", "-n", "")),
        Option("--disown", values=("", "-h", "-a", "%1", "-r")),
        Option("--exit", values=("exit", "eof"), attached=True),
    ),
    operands=((),),
    stdin=("empty",),
    fixture="shell",
    modes=shell_ALL,
    script=shell_terminal_jobs_script,
    valid=shell_terminal_jobs_valid,
    max_flags=4,
    timeout=30.0,
)


shell_SESSION_FATALS = (
    'echo RAN-BAD "${x@Z}"; echo SAME-LINE',
    'echo RAN-BAD "${!x}"; echo SAME-LINE',
    'echo RAN-BAD "${y:?boom}"; echo SAME-LINE',
    'echo RAN-BAD "$((1/0))"; echo SAME-LINE',
    'echo RAN-BAD >"${y:?boom}"; echo SAME-LINE',
    'for v in ${x@Z}; do echo RAN-BAD; done; echo SAME-LINE',
    'case ${x@Z} in *) echo RAN-BAD;; esac; echo SAME-LINE',
    'echo RAN-BAD "${x@Z}" | cat; echo SAME-LINE',
    'value=$(echo "${x@Z}"; echo RAN-BAD >&2); echo SAME-LINE',
    'echo RAN-BAD "${y:=${x@Z}}"; echo SAME-LINE',
    'readonly r=1; r=2; echo SAME-LINE',
    'shift 5; echo SAME-LINE',
    'cd /nonexistent; echo SAME-LINE',
    'nosuchcommand_xyz; echo SAME-LINE',
    'echo RAN-BAD > /nonexistent/dir/file; echo SAME-LINE',
    'exec 3< /nonexistent; echo SAME-LINE',
    'set -u; echo "$unset_here"; echo SAME-LINE',
    'return 3; echo SAME-LINE',
    'break; echo SAME-LINE',
    'f() { return 1 2; echo RAN-BAD; }; f; echo SAME-LINE',
    'exit bad; echo SAME-LINE',
    'let 1/0; echo SAME-LINE',
    ': $((08)); echo SAME-LINE',
)


def shell_terminal_session_script(argv, stdin):
    choice = {}
    for word in argv:
        key, _, value = word.partition("=")
        choice[key] = value
    steps = [("echo START\n", 0.3)]
    if "--setup" in choice:
        steps.append((choice["--setup"] + "\n", 0.3))
    if "--fatal" in choice:
        steps.append((shell_SESSION_FATALS[int(choice["--fatal"])] + "\n", 0.4))
        steps.append(("echo STATUS:$?\n", 0.3))
    if "--heredoc" in choice:
        steps.append(("cat <<EOF\n", 0.2))
        steps.append(("line $x\n", 0.2))
        steps.append(("EOF\n", 0.3))
    if "--continuation" in choice:
        steps.append(("echo one \\\n", 0.2))
        steps.append(("two\n", 0.3))
    if "--quote" in choice:
        steps.append(("echo 'a\n", 0.2))
        steps.append(("b'\n", 0.3))
    if "--compound" in choice:
        steps.append(("for v in 1 2; do\n", 0.2))
        steps.append(("echo $v\n", 0.2))
        steps.append(("done\n", 0.3))
    if "--syntax" in choice:
        steps.append((choice["--syntax"] + "\n", 0.3))
        steps.append(("echo AFTER-SYNTAX:$?\n", 0.3))
    if "--comment" in choice:
        steps.append(("echo one # two\n", 0.3))
    if "--trap" in choice:
        steps.append(("trap 'echo CAUGHT' USR1; unset y; echo \"$(kill -USR1 $$)\" \"${y:?boom}\"; echo SAME-LINE\n", 0.5))
        steps.append(("echo STATUS:$?\n", 0.3))
    if "--interrupt" in choice:
        steps.append(("sleep 3\n", 0.3))
        steps.append((b"\x03", 0.4))
        steps.append(("echo INT:$?\n", 0.3))
    if "--suspend" in choice:
        steps.append((b"\x1a", 0.3))
        steps.append(("echo TSTP\n", 0.3))
    if "--ignoreeof" in choice:
        steps.append(("set -o ignoreeof\n", 0.3))
        steps.append((b"\x04", 0.4))
        steps.append(("echo AFTER-EOF\n", 0.3))
    if "--status" in choice:
        steps.append(("false; echo STATUS:$?\n", 0.3))
    steps.append(("echo NEXT\n", 0.3))
    ending = choice.get("--end", "exit")
    if ending == "eof":
        steps.append((b"\x04", 0.5))
    elif ending == "exit-3":
        steps.append(("exit 3\n", 0.5))
    elif ending == "exit-bad":
        steps.append(("exit bad\n", 0.5))
    else:
        steps.append(("exit\n", 0.5))
    if "--ignoreeof" in choice:
        steps.append(("exit\n", 0.5))
    flags = [f for f in ("-i", "--posix", "--norc", "--noprofile", "-m", "+m", "-e", "-u", "-x", "-v")
             if "--flag=" + f in argv or f in choice.get("--flags", "").split(",")]
    return shell_pty_script(steps, {"linger": 4.0}, flags)


shell_TERMINAL_SESSION = Utility(
    "terminal_session",
    options=(
        Option("--setup", values=("x=ab", "unset x y", "x=bad-name", "y=", "set -e", "set -u", "set -o posix",
                                  "set +o interactive-comments", "shopt -u interactive_comments", "PS1='P> '",
                                  "PS2='+ '", "PS1=", "set -o vi", "set -o emacs"), attached=True),
        Option("--fatal", values=tuple(str(n) for n in range(len(shell_SESSION_FATALS))), attached=True),
        Option("--heredoc"), Option("--continuation"), Option("--quote"), Option("--compound"),
        Option("--syntax", values=("}", "fi", "done", ";;", "echo 'open", "if true; then", "echo )",
                                   "echo \"$(\"", "eval '}'"), attached=True),
        Option("--comment"), Option("--trap"), Option("--interrupt"), Option("--suspend"),
        Option("--ignoreeof"), Option("--status"),
        Option("--end", values=("exit", "eof", "exit-3", "exit-bad"), attached=True),
        Option("--flags", values=("-i", "--posix", "-i,--posix", "-m", "+m", "-e", "-u", "-x", "-v", "-i,-x"),
               attached=True),
    ),
    operands=((),),
    stdin=("empty",),
    fixture="shell",
    modes=shell_ALL,
    script=shell_terminal_session_script,
    max_flags=4,
    timeout=30.0,
)


shell_TERMINAL_PROBES = (
    'printf "<%s>\\n" "$-"',
    '/usr/bin/python3 -c "import os; print(\\"FG\\" if os.tcgetpgrp(0) == os.getpgrp() else \\"BG\\")"',
    'stty size',
    '[ -t 0 ] && echo tty0; [ -t 1 ] && echo tty1; [ -t 2 ] && echo tty2',
    'case $- in *m*) echo monitor;; *) echo no-monitor;; esac; case $- in *i*) echo interactive;; *) echo batch;; esac',
    'echo one; echo two',
    'read line; echo "read:[$line]"',
    'sleep 0.2 & jobs; wait; echo "wait=$?"',
    'echo "$PS1|$PS2|${PS4}"',
    'shopt -q expand_aliases 2>/dev/null; echo "aliases=$?"; shopt -q interactive_comments 2>/dev/null; echo "comments=$?"',
)


def shell_terminal_startup_script(argv, stdin):
    flags = []
    probe = shell_TERMINAL_PROBES[0]
    typed = None
    for word in argv:
        if word.startswith("--probe="):
            probe = shell_TERMINAL_PROBES[int(word.split("=", 1)[1])]
        elif word == "--typed":
            typed = "typed for read\n"
        else:
            flags.append(word)
    steps = []
    if typed:
        steps.append((typed, 0.4))
    return shell_pty_script(steps, {"linger": 3.0, "startup": 0.5}, flags + ["-c", probe])


shell_TERMINAL_STARTUP = Utility(
    "terminal_startup",
    options=(
        Option("-i"), Option("-m"), Option("+m"), Option("-s"), Option("--norc"), Option("--posix"),
        Option("-o", values=("monitor", "interactive-comments", "emacs", "vi", "posix")),
        Option("+o", values=("monitor", "interactive-comments", "emacs")),
        Option("--probe", values=tuple(str(n) for n in range(len(shell_TERMINAL_PROBES))), attached=True),
        Option("--typed"),
    ),
    operands=((),),
    stdin=("empty",),
    fixture="shell",
    modes=shell_ALL,
    script=shell_terminal_startup_script,
    max_flags=4,
    timeout=20.0,
)


def shell_terminal_vanish_script(argv, stdin):
    choice = {}
    for word in argv:
        key, _, value = word.partition("=")
        choice[key] = value
    steps = [("echo START\n", 0.3)]
    running = choice.get("--running", "")
    if running:
        steps.append((running + "\n", 0.5))
    steps.append((b"<CLOSE>", 0))
    settings = {"linger": 4.0, "session": "--nosetsid" not in choice, "ignore_hup": "--ignore-hup" in choice}
    return shell_pty_script(steps, settings, ["-i"] if "--interactive" in choice else [])


shell_TERMINAL_VANISH = Utility(
    "terminal_vanish",
    options=(
        Option("--running", values=("sleep 30", "sleep 30 &", "read x", "cat", "trap '' HUP", "trap 'echo HUP' HUP",
                                    "set -o ignoreeof", "while :; do read x || break; done",
                                    "sleep 30 & wait", "cat <<EOF"), attached=True),
        Option("--nosetsid"), Option("--ignore-hup"), Option("--interactive"),
    ),
    operands=((),),
    stdin=("empty",),
    fixture="shell",
    modes=shell_ALL,
    script=shell_terminal_vanish_script,
    max_flags=4,
    timeout=20.0,
)


# ----------------------------------------------------------------------------
#       Policy rows: where this shell deliberately answers differently from
#       one reference, one operand per answer so each is pinned by name.
# ----------------------------------------------------------------------------

shell_POLICY_SCRIPTS = {
    "power": "echo $((2 ** 3)); echo after",
    "post-increment": "x=1; echo $((x++)) $x",
    "post-decrement": "x=1; echo $((x--)) $x",
    "pre-increment": "x=1; echo $((++x)) $x",
    "word-is-a-name": "x=bar; echo $((x + 1)); echo after",
    "base-prefix": "echo $((16#ff)) $((2#101))",
    "brace-list": "echo {a,b}",
    "brace-range": "echo {1..3}",
    "double-bracket": "[[ x == x ]] && echo dbl; echo $?",
    "tilde-user": "echo ~root ~nosuchuser",
    "star-trim": 'set -- ab cd; printf "[%s]" ${*%b} END; echo',
    "at-trim": 'set -- ab cb; printf "[%s]" ${@%b} END; echo',
    "negative-group-empty": 'shopt -s extglob; x=; p="*!(a)?"; [[ $x == $p ]]; echo "$?"',
}


def shell_policy_script(argv, stdin):
    return shell_POLICY_SCRIPTS[argv[-1]] + "\n"


shell_POLICY_DASH = Utility(
    "policy_dash",
    operands=(("power",), ("post-increment",), ("post-decrement",), ("pre-increment",),
              ("word-is-a-name",), ("base-prefix",), ("brace-list",), ("brace-range",),
              ("double-bracket",), ("tilde-user",), ("star-trim",), ("at-trim",)),
    stdin=("empty",),
    fixture="shell",
    modes=("dash",),
    script=shell_policy_script,
)

shell_POLICY_BASH = Utility(
    "policy_bash",
    operands=(("negative-group-empty",), ("tilde-user",), ("star-trim",)),
    stdin=("empty",),
    fixture="shell",
    modes=shell_BASH,
    script=shell_policy_script,
)


# ----------------------------------------------------------------------------
#       Language families. Each is a seeded generator of one whole program;
#       the names below are the corners of the language the old hand-written
#       lanes asserted, now varied instead of listed.
# ----------------------------------------------------------------------------

shell_OBSERVE = "observe() { printf 'n=%s' \"$#\"; for v do printf '<%s>' \"$v\"; done; echo; }"


def shell_lang_dollar_single(rng):
    """$'...' with every escape it knows, in every place a word can stand."""
    escapes = ("\\a", "\\b", "\\e", "\\E", "\\f", "\\n", "\\r", "\\t", "\\v", "\\\\", "\\'", "\\\"",
               "\\?", "\\101", "\\x41", "\\u00e9", "\\U0001F319", "\\cA", "\\c?", "\\c\\\\", "\\0",
               "\\1", "\\xg", "\\z", "\\")
    body = "".join(rng.choice(escapes) for _ in range(rng.randrange(1, 4)))
    body = rng.choice(("", "pre")) + body + rng.choice(("", "post"))
    site = rng.choice(("word", "adjacent", "quoted", "assignment", "case", "heredoc", "substitution"))
    if site == "word":
        script = "printf '%s' $'" + body + "' | od -An -tu1"
    elif site == "adjacent":
        script = "printf '%s' before$'" + body + "'after | od -An -tu1"
    elif site == "quoted":
        script = "printf '%s' \"$'" + body + "'\" | od -An -tu1"
    elif site == "assignment":
        script = "v=$'" + body + "'; printf '%s' \"$v\" | od -An -tu1; echo \"${#v}\""
    elif site == "case":
        script = "case $'" + body + "' in $'" + body + "') echo same;; *) echo other;; esac"
    elif site == "heredoc":
        script = "cat <<EOF | od -An -tu1\n$'" + body + "'\nEOF"
    else:
        script = "printf '%s' \"$(printf %s $'" + body + "')\" | od -An -tu1"
    return "dollar-single-" + site, shell_BASH, shell_program(script)


def shell_lang_locale_quote(rng):
    text = rng.choice(("hello", "a $x b", "$(echo sub)", "a  b", "*", "", "it's", "back\\\\slash", "\\$x"))
    site = rng.choice(("word", "adjacent", "assignment", "case", "ifs"))
    if site == "word":
        script = 'x=v; printf "[%s]" $"' + text + '" END; echo'
    elif site == "adjacent":
        script = 'x=v; printf "[%s]" pre$"' + text + '"post "$"x "a$" END; echo'
    elif site == "assignment":
        script = 'x=v; v=$"' + text + '"; printf "[%s]\\n" "$v"'
    elif site == "case":
        script = 'x=v; case ab in $"ab") echo yes;; *) echo no;; esac; case $"' + text + '" in *) echo any;; esac'
    else:
        script = 'x=v; IFS=" "; printf "[%s]" $"' + text + '" END; echo'
    return "locale-quote", shell_BASH, shell_program(script)


def shell_lang_parameter_operators(rng):
    """Every ${} operator against every state, with a word of every shape."""
    state = rng.choice(("unset", "empty", "value", "spaces", "star"))
    setup = {"unset": "unset x", "empty": "x=", "value": "x=abc", "spaces": "x='a  b'", "star": "x='*'"}[state]
    operator = rng.choice(("-", ":-", "=", ":=", "+", ":+", "?", ":?"))
    word = rng.choice(("D", "D E", "$y", "\"$y\"", "~", "~/x", "$(echo sub)", "$((1+2))", "'q w'", "*", "", "a}b",
                       "${y:-nested}", "\\$y", "{a,b}"))
    quoted = rng.choice((False, True))
    form = "${x" + operator + word + "}"
    if quoted:
        form = '"' + form + '"'
    context = rng.choice(("args", "assign", "case", "redirect", "heredoc", "arith"))
    if context == "args":
        use = "observe " + form
    elif context == "assign":
        use = "z=" + form + "; printf '<%s>\\n' \"$z\""
    elif context == "case":
        use = "case " + form + " in D*) echo D;; '') echo empty;; *) echo other;; esac"
    elif context == "redirect":
        use = "echo out > out" + form.replace("*", "s").replace("~", "h").replace("/", "_").replace(" ", "_").replace("$(echo sub)", "sub") + " 2>/dev/null; ls | wc -l"
    elif context == "heredoc":
        use = "cat <<EOF\n<" + form + ">\nEOF"
    else:
        use = "echo $(( ${x:-0} + 1 ))"
    modes = shell_ALL if "{a,b}" not in word else shell_BASH
    return "parameter-operators", modes, shell_program(
        shell_OBSERVE, setup, "y='Y Z'", "HOME=/hh", "cd /tmp 2>/dev/null; cd - >/dev/null 2>&1 || :",
        "exec 2>/dev/null", use, "printf 'status=%s x=<%s>\\n' \"$?\" \"${x-unset}\"")


def shell_lang_parameter_length_trim(rng):
    value = rng.choice(("", "a", "abc", "a.b.c", "/usr/local/bin", "aXbXc", "a*c", "  spaced  ", "x" * 70))
    setup = rng.choice(("x=" + shell_quote(value), "unset x", "set -- " + shell_quote(value) + " b c"))
    name = "x" if "x=" in setup or setup == "unset x" else rng.choice(("1", "*", "@", "#"))
    operator = rng.choice(("#", "##", "%", "%%"))
    pattern = rng.choice(("*", "?", "a", "a*", "*c", "*.", ".*", "*/", "/*", "[abc]", "[!a]", "\\*", "'*'", '"*"',
                          "$p", '"$p"', "", "?*", "*?"))
    form = rng.choice(("${#" + name + "}", "${" + name + operator + pattern + "}",
                       '"${' + name + operator + pattern + '}"', '"${#' + name + '}"'))
    return "parameter-length-trim", shell_ALL, shell_program(
        shell_OBSERVE, setup, "p='a*'", "set " + rng.choice(("-u", "+u")),
        "observe " + form + " 2>/dev/null", "printf 'status=%s\\n' \"$?\"")


def shell_lang_transforms(rng):
    letter = rng.choice(("Q", "E", "P", "A", "a", "K", "k", "U", "u", "L", "Z", "", "QQ"))
    value = rng.choice(("plain", "a b", "it's", "tab\\there", "$'\\001\\177'", "", "UPPER lower", "\\u \\w",
                        "é", "a\"b", "back\\\\slash"))
    kind = rng.choice(("scalar", "array", "assoc", "unset", "positional", "exported", "integer", "readonly"))
    if kind == "scalar":
        setup, name = "v=" + shell_quote(value), "v"
    elif kind == "array":
        setup, name = "v=(" + shell_quote(value) + " two)", rng.choice(("v[@]", "v[*]", "v[0]", "v"))
    elif kind == "assoc":
        setup, name = "declare -A v; v[k]=" + shell_quote(value), rng.choice(("v[@]", "v[k]"))
    elif kind == "unset":
        setup, name = "unset v", "v"
    elif kind == "positional":
        setup, name = "set -- " + shell_quote(value) + " two", rng.choice(("1", "@", "*"))
    elif kind == "exported":
        setup, name = "export v=" + shell_quote(value), "v"
    elif kind == "integer":
        setup, name = "declare -i v=7", "v"
    else:
        setup, name = "declare -r v=" + shell_quote(value), "v"
    form = "${" + name + "@" + letter + "}"
    quoted = rng.choice((True, False))
    if quoted:
        form = '"' + form + '"'
    return "transforms", shell_BASH, shell_program(
        shell_OBSERVE, setup, "observe " + form + " 2>/dev/null; echo \"status=$?\"",
        "eval \"back=${v@Q}\" 2>/dev/null && printf 'round=<%s>\\n' \"$back\"" if kind in ("scalar", "exported") else ":")


def shell_lang_indirection(rng):
    shape = rng.choice(("name", "prefix-star", "prefix-at", "keys", "positional", "special", "nested", "operator",
                        "invalid", "unset-target"))
    if shape == "name":
        script = "target=value; x=target; printf '<%s>' \"${!x}\" \"${!x-def}\" \"${!x:2}\"; echo"
    elif shape == "prefix-star":
        script = "pre_b=1 pre_a=2 pre__=3 pre_9=4; IFS=" + shell_quote(rng.choice((":", "", " "))) + "; printf '<%s>' \"${!pre_*}\" ${!pre_*}; echo"
    elif shape == "prefix-at":
        script = "pre_b=1 pre_a=2; printf '<%s>' \"${!pre_@}\" \"X${!pre_@}Y\" \"${!none_@}\"; echo"
    elif shape == "keys":
        script = "a=([3]=x [7]=y); declare -A m; m[k]=v m[j]=w; printf '<%s>' \"${!a[@]}\" \"${#a[@]}\"; echo; for k in \"${!m[@]}\"; do :; done; echo \"${#m[@]}\""
    elif shape == "positional":
        script = "set -- one two; x=2; printf '<%s>' \"${!1}\" \"${!x}\" \"${!#}\"; echo"
    elif shape == "special":
        script = "set -- one two; false; printf '<%s>' \"${!?}\" \"${!#}\" \"${!@}\" \"${!*}\"; echo"
    elif shape == "nested":
        script = "x=y; y=z; z=deep; printf '<%s>' \"${!x}\" \"${!${x}}\"; echo"
    elif shape == "operator":
        script = "x=y; y=abcabc; printf '<%s>' \"${!x#a}\" \"${!x/b/X}\" \"${!x:2:3}\" \"${!x^^}\"; echo"
    elif shape == "invalid":
        script = "x=" + rng.choice(("bad-name", "1bad", "", "a b")) + "; printf before; printf '<%s>' \"${!x}\"; echo after"
    else:
        script = "unset t; x=t; set " + rng.choice(("-u", "+u")) + "; printf '<%s>' \"${!x}\" \"${!x-d}\" \"${!x:=made}\" \"$t\"; echo"
    return "indirection-" + shape, shell_BASH, shell_program(script + " 2>/dev/null", "echo \"status=$?\"")


def shell_lang_arith_expression(rng, portable=False, depth=3):
    """A random expression over every operator, precedence left to the shell."""
    if depth <= 0 or rng.randrange(4) == 0:
        leaf = rng.choice(("literal", "literal", "name", "base", "hex", "octal", "negative"))
        if leaf == "literal":
            return str(rng.choice((0, 1, 2, 3, 7, 10, 255, 4096, 65535, 2147483647, 9223372036854775807)))
        if leaf == "name":
            return rng.choice(("x", "y", "z", "unsetname", "x", "y"))
        if leaf == "base":
            return rng.choice(("0x10", "010", "16#ff", "2#1010", "8#17", "36#z", "10#08")) if not portable else rng.choice(("0x10", "010"))
        if leaf == "hex":
            return rng.choice(("0xff", "0XA", "0x0"))
        if leaf == "octal":
            return rng.choice(("07", "0777", "00", "08" if rng.randrange(6) == 0 else "07"))
        return "-" + str(rng.randrange(1, 9))
    kind = rng.choice(("binary", "binary", "binary", "unary", "ternary", "assign", "group", "comma", "incdec"))
    if kind == "binary":
        operator = rng.choice(("+", "-", "*", "/", "%", "<<", ">>", "<", "<=", ">", ">=", "==", "!=", "&", "^",
                               "|", "&&", "||", "**"))
        if portable and operator == "**":
            operator = "*"
        return shell_lang_arith_expression(rng, portable, depth - 1) + " " + operator + " " + shell_lang_arith_expression(rng, portable, depth - 1)
    if kind == "unary":
        return rng.choice(("-", "+", "!", "~")) + shell_lang_arith_expression(rng, portable, depth - 1)
    if kind == "ternary":
        return (shell_lang_arith_expression(rng, portable, depth - 1) + " ? " + shell_lang_arith_expression(rng, portable, depth - 1)
                + " : " + shell_lang_arith_expression(rng, portable, depth - 1))
    if kind == "assign":
        operator = rng.choice(("=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "|=", "^="))
        return rng.choice(("x", "y", "z")) + " " + operator + " " + shell_lang_arith_expression(rng, portable, depth - 1)
    if kind == "group":
        return "(" + shell_lang_arith_expression(rng, portable, depth - 1) + ")"
    if kind == "comma":
        return shell_lang_arith_expression(rng, portable, depth - 1) + ", " + shell_lang_arith_expression(rng, portable, depth - 1)
    if portable:
        return rng.choice(("x", "y")) + " + 1"
    return rng.choice(("x++", "++x", "y--", "--y", "x++ + ++y"))


def shell_lang_arithmetic_grammar(rng):
    portable = rng.randrange(3) == 0
    expression = shell_lang_arith_expression(rng, portable)
    context = rng.choice(("expansion", "expansion", "command", "condition", "let", "for", "assign-int", "substring"))
    values = ("0", "1", "-1", "7", "010", "0x1f", "", "abc", "2+3", "9223372036854775807", "-9223372036854775808")
    setup = "x=" + rng.choice(values) + "; y=" + rng.choice(values) + "; z=" + rng.choice(values[:6])
    modes = shell_ALL if portable and context in ("expansion",) else shell_BASH
    if context == "expansion":
        body = "printf 'r=<%s>\\n' \"$((" + expression + "))\""
    elif context == "command":
        body = "((" + expression + ")); echo \"cmd=$?\""
    elif context == "condition":
        body = "if ((" + expression + ")); then echo true; else echo false; fi; while ((" + expression + ")); do echo loop; break; done"
    elif context == "let":
        body = "let " + shell_quote(expression) + "; echo \"let=$?\""
    elif context == "for":
        body = "for ((i = 0; i < 3 && (" + expression + ", 1); i++)); do printf '%s ' \"$i\"; done; echo"
    elif context == "assign-int":
        body = "declare -i n; n=" + shell_quote(expression) + "; echo \"n=$n\""
    else:
        body = "s=abcdefgh; printf '<%s>\\n' \"${s:(" + expression + ") & 3:2}\""
    return "arithmetic-grammar", modes, shell_program(setup, body + " 2>/dev/null",
                                                        "printf 'status=%s x=%s y=%s z=%s\\n' \"$?\" \"$x\" \"$y\" \"$z\"")


def shell_lang_arithmetic_errors(rng):
    expression = rng.choice(("5 / 0", "5 % 0", "x /= 0", "1 +", "* 3", "", "1,2", "08", "0x", "2 ** -1", "1 ? : 2",
                             "99#1", "1#0", "2#2", "10#-5", "1/0 || 1", "0 && 1/0", "1 || 1/0", "x=12ab; x",
                             "a=b; b=a; a", "9223372036854775807 + 1", "-9223372036854775807 - 1 - 1",
                             "(-9223372036854775807 - 1) / -1", "(-9223372036854775807 - 1) % -1", "1 << 64",
                             "1 << -1", "-1 >> 64", "2 ** 63", "2 ** 64", "x[1]", "$x", "\"1\" + 1", "1 = 2",
                             "3 += 1", "++5", "x++ ++", "( 1", "1 )", "1 2", "!"))
    wrapper = rng.choice(("direct", "subshell", "function", "eval", "conditional", "assignment", "index"))
    use = "echo \"$((" + expression + "))\""
    if wrapper == "subshell":
        use = "(" + use + "); echo sub:$?"
    elif wrapper == "function":
        use = "f() { " + use + "; echo tail; }; f"
    elif wrapper == "eval":
        use = "eval " + shell_quote(use) + "; echo eval:$?"
    elif wrapper == "conditional":
        use = "if " + use + "; then echo yes; else echo no; fi"
    elif wrapper == "assignment":
        use = "v=$((" + expression + ")); echo \"v=<$v>\""
    elif wrapper == "index":
        use = "a=(p q); echo \"${a[" + expression + "]}\""
    modes = shell_BASH if wrapper == "index" or "**" in expression or "#" in expression else shell_ALL
    return "arithmetic-errors", modes, shell_program("x=1; unset y", "echo before", use + " 2>/dev/null",
                                                       "echo \"after:$?\"")


def shell_lang_ifs_splitting(rng):
    ifs = rng.choice((None, "unset", ":", " :", "", " ", "\t", "\\n", ":,", "a", " \t\n", "::", "x y"))
    value = rng.choice(("a b\tc", "a   b", "  a", "a  ", "   ", "a:b:c", "a::b", ":a", "a:", ":", "a : b", "a  :  b",
                        "a,b:c", "xay", "a\\nb\\nc", "", "*", "a.txt b.txt", "$y", "one\\ntwo"))
    form = rng.choice(("$x", "\"$x\"", "${x}", "${x-D E}", "${x:-a:b}", "$x\"$x\"", "pre${x}post", "$(printf '%s' \"$x\")",
                       "$*", "\"$*\"", "$@", "\"$@\"", "${@#a}", "\"${*}\"", "$((1+1))$x", "~", "'$x'"))
    #       "resplit" changes IFS between splits of one value: a quoted field
    #       must leave the separator that follows it current. It came from a
    #       hand-written case this grammar replaced, and the shapes above set
    #       IFS once and never move it.
    context = rng.choice(("observe", "for", "set", "read", "case", "assign", "resplit"))
    setup = ["x=" + shell_quote(value).replace("\\n", "\n") if "\\n" in value else "x=" + shell_quote(value),
             "y='Y  Z'", "set -- 'p q' '' r"]
    if ifs == "unset":
        setup.append("unset IFS")
    elif ifs is not None:
        setup.append("IFS=" + ("$'" + ifs + "'" if "\\n" in ifs else shell_quote(ifs)))
    if context == "observe":
        use = "observe " + form
    elif context == "for":
        use = "for v in " + form + "; do printf '<%s>' \"$v\"; done; echo"
    elif context == "set":
        use = "set -- " + form + "; printf 'n=%s' \"$#\"; printf '<%s>' \"$@\"; echo"
    elif context == "read":
        use = "printf '%s\\n' " + shell_quote(value) + " | { read -r a b c; printf '<%s>' \"$a\" \"$b\" \"$c\"; echo; }"
    elif context == "case":
        use = "case " + form + " in a) echo a;; *) echo other;; esac"
    elif context == "resplit":
        use = ("printf '<%s>\\n' \"$x\" \"\" $x; IFS=,; printf '<%s>\\n' \"$x\" \"\" $x; "
               "IFS=''; printf '<%s>\\n' \"$x\" \"\" $x")
    else:
        use = "z=" + form + "; printf '<%s>\\n' \"$z\""
    modes = shell_ALL if ("\\n" not in str(ifs)) else shell_BASH
    return "ifs-splitting-" + context, modes, shell_program(shell_OBSERVE, *setup, use + " 2>/dev/null",
                                                              "echo \"status=$?\"")


shell_GLOB_FILES = ("a.txt", "b.txt", "c.TXT", "abc", "a-b", "1one", "2two", ".hidden", "Gamma", "x y", "[br]", "a*b",
                    "dir/inside", "dir/.dot", "dir/sub/deep", "link", "-dash", "~tilde", "é", "a\\b")


def shell_lang_pathname_expansion(rng):
    pattern = rng.choice(("*", "?", "??", "*.txt", "*.TXT", "[a-c]*", "[!a]*", "[^a]*", "[[:alpha:]]*", "[[:digit:]]*",
                          "[[:upper:]]*", "[![:digit:]]*.txt", "[[:nosuch:]]*", "*[", "[ab", "a[-.]*", "\\*", "'*'",
                          "\"*\"", "a\\*b", "$p", "\"$p\"", ".*", ".h*", "[.]*", "dir/*", "dir/*/*", "*/", "*/inside",
                          "d*/in*", "no*such", "nosuch*/x", "*.[ct]*", "[]a]*", "[!]]*", "a*", "?.txt", "-*", "~*",
                          "*b", "*/.*", "@(a.txt|abc)", "!(*.txt)", "+(a)*", "*(a|b)*", "?(a)bc", "**", "**/", "**/deep",
                          "dir/**", "[a-\\c]*", "é*", "*\\\\*", "x\\ *", "\"x y\"*"))
    options = []
    for option in ("nullglob", "failglob", "dotglob", "nocaseglob", "globstar", "extglob", "globskipdots"):
        if rng.randrange(4) == 0:
            options.append("shopt -" + rng.choice(("s", "u")) + " " + option)
    noglob = rng.choice(("", "", "set -f", "set -o noglob"))
    context = rng.choice(("observe", "for", "redirect", "case", "assign", "ls-count", "array"))
    if context == "observe":
        use = "observe " + pattern
    elif context == "for":
        use = "for f in " + pattern + "; do printf '<%s>' \"$f\"; done; echo"
    elif context == "redirect":
        use = "echo data > " + pattern + " 2>/dev/null; echo \"redirect=$?\"; ls | LC_ALL=C sort | wc -l"
    elif context == "case":
        use = "case a.txt in " + pattern + ") echo match;; *) echo none;; esac"
    elif context == "assign":
        use = "v=" + pattern + "; printf '<%s>\\n' \"$v\"; printf '<%s>' $v; echo"
    elif context == "ls-count":
        use = "set -- " + pattern + "; echo \"$#\""
    else:
        use = "a=(" + pattern + "); printf '<%s>' \"${a[@]}\"; echo \"${#a[@]}\""
    bash_only = options or context == "array" or any(t in pattern for t in ("@(", "!(", "+(", "*(", "?(", "**"))
    return "pathname-expansion", shell_BASH if bash_only else shell_ALL, shell_program(
        shell_OBSERVE, "mkdir -p dir/sub; for f in " + " ".join(shell_quote(f) for f in shell_GLOB_FILES) +
        "; do case $f in */*) mkdir -p \"${f%/*}\";; esac; : > \"$f\"; done; ln -sf a.txt link", "p='*.txt'",
        *options, noglob, use + " 2>/dev/null", "echo \"status=$?\"")


def shell_lang_brace_expansion(rng):
    pattern = rng.choice(("{a,b}", "{a,b}{1,2}", "x{a,{b,c}}y", "{a,\"b,c\"}", "{a,}", "{,b}", "{a}", "{}", "{a,b",
                          "a,b}", "{1..5}", "{5..1}", "{01..03}", "{-3..3}", "{1..10..3}", "{10..1..-3}", "{1..3..0}",
                          "{a..e}", "{e..a}", "{a..g..2}", "{1..a}", "{a..1}", "{01..10}", "{-01..2}", "{1..1200}",
                          "\"{a,b}\"", "\\{a,b\\}", "'{a,b}'", "{$x,c}", "{a,$(echo s)}", "{a,b}$x", "${x}{a,b}",
                          "{a..c}{1..2}", "{,,}", "{a,,b}", "{a,{b,{c,d}}}", "pre{a,b}post", "{ a,b}", "{a, b}",
                          "{1..3}{a..b}", "{{a,b},c}", "{a}{b}", "{a,b}}", "{{a,b}", "{1..3.5}", "{x..y..z}"))
    braceexpand = rng.choice(("", "", "set +B", "set -B", "set +o braceexpand"))
    context = rng.choice(("count", "observe", "for", "assign", "case", "redirect"))
    if context == "count":
        use = "set -- " + pattern + "; printf '%s:%s:%s\\n' \"$#\" \"$1\" \"${2-}\""
    elif context == "observe":
        use = "observe " + pattern
    elif context == "for":
        use = "for v in " + pattern + "; do printf '<%s>' \"$v\"; done; echo"
    elif context == "assign":
        use = "v=" + pattern + "; printf '<%s>\\n' \"$v\""
    elif context == "case":
        use = "case a1 in " + pattern + ") echo match;; *) echo none;; esac"
    else:
        use = "echo data > " + pattern + " 2>/dev/null; echo \"redirect=$?\"; ls | wc -l"
    if "1200" in pattern and context in ("observe", "for"):
        use = "set -- " + pattern + "; echo \"$#:$1:${1200-}\""
    return "brace-expansion", shell_BASH, shell_program(shell_OBSERVE, "x=X", braceexpand, use, "echo \"status=$?\"")


def shell_lang_tilde(rng):
    home = rng.choice(("HOME=/hh", "HOME=", "unset HOME", "HOME=/hh/", "HOME='/h h'", "HOME=~"))
    form = rng.choice(("~", "~/x", "~/", "~x", "~+", "~-", "~+/x", "a~", "\"~\"", "'~'",
                       "~\"/x\"", "\\~", "${x-~}", "${x:-~/y}", "${x:=~}", "${x:+~}", "x=~", "x=~/y:~", "x=a:~",
                       "x=~:~", "x=~a:b", "x=\"~\"", "/~", "~/*", "~[", "$x~", "~~", "PATH=~/bin:$PATH", "~-/x",
                       "~1", "~+1"))
    if form.startswith("x=") or form.startswith("PATH="):
        use = form + "; printf '<%s>\\n' \"${" + form.split("=")[0] + "}\""
    else:
        use = "unset x; printf '<%s>' " + form + " END; echo"
    return "tilde", shell_ALL, shell_program("cd /tmp && OLDPWD=/var && cd - >/dev/null 2>&1", home, use + " 2>/dev/null",
                                             "echo \"status=$?\"")


def shell_lang_command_substitution(rng):
    inner = rng.choice(("echo x", "echo 'a b'", "printf 'a\\n\\n\\n'", "printf '%s' \"a  b\"", "true", "false",
                        "echo \"$(echo deep)\"", "echo $(echo $(echo deeper))", "echo ')'", "echo '('", "case x in x) echo y;; esac",
                        "(echo sub)", "( ( echo nested ) )", "echo x # comment )", "cat <<EOF\nhere )\nEOF",
                        "cat <<'EOF'\n$x )\nEOF", "echo one\necho two", "for i in 1 2; do echo $i; done",
                        "echo \"\\$x\"", "echo \\$x", "printf '%s' 'tick`s'", "echo a\\\nb", "echo $?", "exit 7",
                        "kill -TERM $$", "printf 'x\\0y'", "echo \"$x\"", "x=changed; echo $x", "echo ${x:-}"))
    style = rng.choice(("dollar", "dollar", "backtick", "file"))
    if style == "backtick":
        text = "`" + inner.replace("`", "\\`").replace("$", "\\$").replace("\\", "\\\\") + "`"
        if "\n" in inner:
            text = "`" + inner + "`"
    elif style == "file":
        text = "$(<a.txt)"
    else:
        text = "$(" + inner + ")"
    quoted = rng.choice((True, False))
    site = rng.choice(("word", "assign", "case", "arith", "heredoc", "nested-quote"))
    if site == "word":
        use = "observe " + ('"' + text + '"' if quoted else text)
    elif site == "assign":
        use = "v=" + text + "; printf '<%s>\\n' \"$v\""
    elif site == "case":
        use = "case " + ('"' + text + '"' if quoted else text) + " in x) echo x;; '') echo empty;; *) echo other;; esac"
    elif site == "arith":
        use = "echo $(( $(echo 2) + 3 )) $(( ${#x} ))"
    elif site == "heredoc":
        use = "cat <<EOF\n<" + text + ">\nEOF"
    else:
        use = "printf '<%s>\\n' \"pre $(echo \"a  b\") post\" \"$(echo \"$(echo 'in  ner')\")\""
    modes = shell_ALL if style != "file" else shell_BASH
    return "command-substitution-" + style, modes, shell_program(shell_OBSERVE, "x=X", "exec 2>/dev/null", use,
                                                                    "echo \"status=$? x=$x\"")


def shell_lang_process_substitution(rng):
    shape = rng.choice(("cat", "two", "while-read", "wc", "path", "joined", "digit", "nested", "in-subst", "diff",
                        "function", "for", "pipeline", "if", "exec-keep", "unopened", "writer", "many"))
    if shape == "cat":
        script = "cat <(echo x)"
    elif shape == "two":
        script = "cat <(echo x) <(echo y)"
    elif shape == "while-read":
        script = "while read -r l; do echo \"<$l>\"; done < <(printf 'a\\nb\\n')"
    elif shape == "wc":
        script = "wc -l < <(printf 'a\\nb\\nc\\n')"
    elif shape == "path":
        script = "echo <(true) | sed 's#/dev/fd/[0-9]*#FD#'"
    elif shape == "joined":
        script = "echo a<(echo b) | sed 's#/dev/fd/[0-9]*#FD#'"
    elif shape == "digit":
        script = "echo 2>(cat) | sed 's#/dev/fd/[0-9]*#FD#'; sleep 0.1"
    elif shape == "nested":
        script = "cat <(cat <(echo deep))"
    elif shape == "in-subst":
        script = "v=$(cat <(echo x)); echo \"$v\""
    elif shape == "diff":
        script = "diff <(echo a) <(echo b); echo \"$?\""
    elif shape == "function":
        script = "f() { cat \"$1\"; }; f <(echo via-function)"
    elif shape == "for":
        script = "for f in <(echo a) <(echo b); do cat \"$f\"; done"
    elif shape == "pipeline":
        script = "cat <(echo x) | tr x y"
    elif shape == "if":
        script = "if grep -q x <(echo x); then echo found; fi"
    elif shape == "exec-keep":
        script = "exec 3< /dev/null; cat <(echo keep) <&3; exec 3<&-"
    elif shape == "unopened":
        script = ": <(echo never); echo \"$?\""
    elif shape == "writer":
        script = "echo z > >(cat > written); sleep 0.3; cat written"
    else:
        script = "for i in $(seq 1 40); do cat <(echo $i); done | wc -l"
    return "process-substitution", shell_BASH, shell_program(script + " 2>/dev/null", "echo \"status=$?\"")


def shell_lang_coproc(rng):
    shape = rng.choice(("named", "default", "simple", "vars", "pid", "wait", "exit", "function", "two", "bare-name",
                        "bare", "redirect", "many-lines"))
    if shape == "named":
        script = "coproc C { read x; echo got $x; }; echo hi >&${C[1]}; read y <&${C[0]}; echo $y; wait $C_PID"
    elif shape == "default":
        script = "coproc { read x; echo got $x; }; echo hi >&${COPROC[1]}; read y <&${COPROC[0]}; echo $y; wait"
    elif shape == "simple":
        script = "coproc tr a-z A-Z; echo abc >&${COPROC[1]}; exec {COPROC[1]}>&-; read y <&${COPROC[0]}; echo $y; wait"
    elif shape == "vars":
        script = "coproc C { cat; }; echo ${#C[@]}; declare -p C | sed 's/[0-9][0-9]*/N/g'; exec {C[1]}>&-; wait"
    elif shape == "pid":
        script = "coproc C { cat; }; [ \"$C_PID\" -gt 1 ] && echo pid; exec {C[1]}>&-; wait; echo \"$?\""
    elif shape == "wait":
        script = "coproc C { exit 3; }; wait \"$C_PID\"; echo \"$?\""
    elif shape == "exit":
        script = "coproc C { exit 5; }; sleep 0.1; wait $C_PID; echo $?"
    elif shape == "function":
        script = "f() { coproc C { echo inner; }; read y <&${C[0]}; echo \"$y\"; wait; }; f"
    elif shape == "two":
        script = "coproc A { echo a; }; coproc B { echo b; }; read x <&${A[0]}; read y <&${B[0]}; echo \"$x$y\"; wait"
    elif shape == "bare-name":
        script = "coproc C; echo \"$?\""
    elif shape == "bare":
        script = "coproc; echo \"$?\""
    elif shape == "redirect":
        script = "coproc C { echo out; echo err >&2; } 2>/dev/null; read y <&${C[0]}; echo \"$y\"; wait"
    else:
        script = "coproc C { seq 1 5; }; exec 3<&${C[0]}; while read -r l <&3; do printf '%s ' \"$l\"; done; echo; wait"
    return "coproc", shell_BASH, shell_program(script + " 2>/dev/null", "echo \"status=$?\"")


def shell_lang_heredoc(rng):
    strip = rng.choice((False, True))
    delimiter = rng.choice(("EOF", "'EOF'", '"EOF"', "E\\OF", "'E OF'", "-EOF", "EOF-", "1", "$x", "\"$x\"", "EOF EOF",
                            "ab", "END)", "'END)'", "X" * 40))
    body_lines = rng.sample(("plain", "$x", "${x}", "$(echo sub)", "$((1+1))", "\\$x", "a\\\nb", "a\\\\b", "`echo tick`",
                             "", "\ttabbed", "\t\ttwo tabs", "  spaces", "'quotes' \"double\"", "# not a comment",
                             "EOF inside", "$", "\\", "*", "~", "$?", "\\`", "${x:-default}", "$1", "line with ) paren"),
                            rng.randrange(0, 5))
    tab_prefix = "\t" if strip else ""
    end_word = delimiter.replace("'", "").replace('"', "").replace("\\", "") if not delimiter.startswith("$") else "X"
    if delimiter in ("$x", '"$x"'):
        end_word = "$x"
    body = "".join(tab_prefix + line + "\n" for line in body_lines)
    operator = "<<-" if strip else "<<"
    site = rng.choice(("plain", "pipe", "function", "loop", "case", "subst", "two", "with-redirect", "and-or", "if"))
    heredoc = "cat " + operator + delimiter + "\n" + body + tab_prefix + end_word + "\n"
    if site == "pipe":
        heredoc = "cat " + operator + delimiter + " | tr a-z A-Z\n" + body + tab_prefix + end_word + "\n"
    elif site == "function":
        heredoc = "f() {\n" + heredoc + "}\nf\nf\n"
    elif site == "loop":
        heredoc = "for i in 1 2; do\n" + heredoc + "done\n"
    elif site == "case":
        heredoc = "case y in y)\n" + heredoc + ";; esac\n"
    elif site == "subst":
        heredoc = "v=$(\n" + heredoc + ")\nprintf '<%s>\\n' \"$v\"\n"
    elif site == "two":
        heredoc = "cat " + operator + delimiter + "; cat <<B\n" + body + tab_prefix + end_word + "\nsecond\nB\n"
    elif site == "with-redirect":
        heredoc = "cat " + operator + delimiter + " > out; cat out\n" + body + tab_prefix + end_word + "\n"
    elif site == "and-or":
        heredoc = "false || cat " + operator + delimiter + " && echo and\n" + body + tab_prefix + end_word + "\n"
    elif site == "if":
        heredoc = "if cat " + operator + delimiter + "\n" + body + tab_prefix + end_word + "\nthen echo then; fi\n"
    return "heredoc", shell_ALL, "x=VALUE; set -- p1\n" + heredoc + "echo \"status=$?\"\n"


def shell_lang_here_string(rng):
    word = rng.choice(("word", "\"a  b\"", "$x", "\"$x\"", "'lit $x'", "", "\"\"", "*.txt", "$(echo sub)", "\"value=$x:$((1+1))\"",
                       "$'a\\tb'", "a$x", "{a,b}", "~", "\"multi\nline\""))
    consumer = rng.choice(("cat", "read v; printf '<%s>\\n' \"$v\"", "wc -c", "tr a-z A-Z", "f() { cat; }; f",
                           "cat <<< one", "cat 3<<< data <&3", "od -c | head -2"))
    return "here-string", shell_BASH, shell_program(shell_OBSERVE, "x='X  Y'", ": > a.txt; : > b.txt",
                                                    consumer + " <<< " + word + " 2>/dev/null", "echo \"status=$?\"")


def shell_lang_redirections(rng):
    operator = rng.choice((">", ">>", "<", "<>", ">|", ">&", "<&", "&>", "&>>", ">&-", "<&-", "{fd}>", "{fd}<",
                           "{fd}>&-", "2>", "2>>", "3>", "9>", "10>", "255>", "0<", "1>&2", "2>&1", "3>&1", "1>&3",
                           "2>&-", "1>&-", ">&3", "<&3", "<&0", ">&1", "<&2", "2>&9", ">&99", "0>&1", ">/dev/stdout",
                           ">/dev/stderr", "</dev/stdin", ">/dev/fd/1", ">/dev/fd/2", "</dev/fd/0", ">/dev/null",
                           "</dev/null", ">/dev/fd/9", "1<>"))
    target = rng.choice(("out", "a.txt", "dir", "missing/x", "unreadable", "''", "\"\"", "$empty", "\"$empty\"",
                         "$two", "\"$two\"", "*.txt", "-", "/dev/null", "link", "dangling", "out out", "9", "'x y'"))
    if operator.endswith("-") or operator.startswith(">&") or operator.startswith("<&") or operator.endswith("&1") \
            or operator.endswith("&2") or operator.endswith("&3") or operator.endswith("&9") or operator.endswith("&99") \
            or "/dev/" in operator:
        target = ""
    command = rng.choice(("echo out", "printf '%s\\n' one two", "cat", "cat a.txt", "read v", "echo err >&2", ":",
                          "true", "{ echo grp; echo grperr >&2; }", "( echo sub )", "f() { echo fn; }; f",
                          "while read -r l; do echo \"<$l>\"; done", "if :; then echo if; fi", "exec"))
    order = rng.choice(("after", "before", "between", "exec"))
    redirect = operator + (" " + target if target and not operator.endswith(("-",)) else target)
    if order == "after":
        line = command + " " + redirect
    elif order == "before":
        line = redirect + " " + command
    elif order == "between":
        line = "echo before " + redirect + " after"
    else:
        line = "exec " + redirect + "; " + command + "; exec 3>&- 2>/dev/null"
    extra = rng.choice(("", "", " 2>&1", " 2>/dev/null", " >out2 2>&1", " 2>&1 >out2", " 3>&1 1>&2 2>&3"))
    if order != "exec":
        line = line + extra
    bash_only = operator.startswith("{") or operator.startswith("&") or "/dev/fd" in operator or "/dev/std" in operator \
        or operator in (">&", "<&") and target == ""
    return "redirections", shell_BASH if bash_only else shell_ALL, shell_program(
        "empty=; two='out out'; ln -s nowhere dangling 2>/dev/null; exec 3>&1", "echo start",
        "{ " + line + " ; } 2>err.txt", "echo \"status=$?\"", "[ -s err.txt ] && echo diagnostic; rm -f err.txt",
        "for f in out out2 'x y' 9 -; do [ -e \"$f\" ] && { printf '%s:' \"$f\"; cat \"$f\" 2>/dev/null; echo; }; done; :")


def shell_lang_redirection_persistence(rng):
    shape = rng.choice(("exec-out", "exec-in", "exec-dup", "exec-close", "loop-redirect", "function-redirect",
                        "compound-redirect", "nested-groups", "closed-stdin", "fd-var", "many", "noclobber-exec",
                        "read-from-fd", "write-to-closed", "order-swap"))
    if shape == "exec-out":
        script = "exec 3>out; echo one >&3; echo two >&3; exec 3>&-; cat out; echo three >&3 2>/dev/null; echo \"closed=$?\""
    elif shape == "exec-in":
        script = "printf 'a\\nb\\n' > in; exec 4<in; read x <&4; read y <&4; read z <&4; echo \"$x$y[$z]$?\"; exec 4<&-"
    elif shape == "exec-dup":
        script = "exec 5>&1; echo via5 >&5; exec 1>out; echo captured; exec 1>&5 5>&-; cat out"
    elif shape == "exec-close":
        script = "exec 2>&-; echo err >&2; echo \"status=$?\"; exec 2>&1"
    elif shape == "loop-redirect":
        script = "for i in 1 2; do echo $i; done > out; while read -r l; do echo \"<$l>\"; done < out"
    elif shape == "function-redirect":
        script = "f() { echo body; echo err >&2; } > out 2>&1; f; f; cat out"
    elif shape == "compound-redirect":
        script = "{ echo a; { echo b; } > inner; echo c; } > outer; cat outer inner"
    elif shape == "nested-groups":
        script = "{ { echo x >&3; } 3>&1; } > out; cat out"
    elif shape == "closed-stdin":
        script = "exec 0<&-; read v; echo \"read=$?\"; cat; echo \"cat=$?\""
    elif shape == "fd-var":
        script = "exec {fd}>out; echo via-var >&$fd; echo \"fd>2:$(( fd > 2 ))\"; exec {fd}>&-; cat out"
    elif shape == "many":
        script = "true 3>a 4>b 5>c 6>d 7>e 8>f 9>g; ls | wc -l"
    elif shape == "noclobber-exec":
        script = ": > out; set -C; exec 3>out; echo \"status=$?\"; exec 3>|out; echo \"forced=$?\""
    elif shape == "read-from-fd":
        script = "read v 3<<EOF <&3\nfrom-three\nEOF\necho \"$v\""
    elif shape == "write-to-closed":
        script = "echo x >&7; echo \"status=$?\""
    else:
        script = "{ echo out; echo err >&2; } 2>&1 >out | sed 's/^/pipe:/'; cat out"
    modes = shell_BASH if shape in ("fd-var",) else shell_ALL
    return "redirection-persistence", modes, shell_program(script + " 2>/dev/null", "echo \"end=$?\"")


def shell_lang_pipelines(rng):
    stages = rng.randrange(1, 6)
    parts = []
    for _ in range(stages):
        parts.append(rng.choice(("true", "false", "(exit 3)", "(exit 7)", "echo x", "cat", "tr a-z A-Z",
                                 "{ read v; echo \"<$v>\"; }", "f", "cat missing", "/bin/true", "/bin/false",
                                 "sh -c 'kill -TERM $$'", "exit 5", "x=in cat", "yes | head -1")))
    pipeline = " | ".join(parts)
    negate = rng.choice(("", "", "! "))
    stderr_pipe = rng.choice((False, True, False))
    if stderr_pipe and stages > 1:
        pipeline = pipeline.replace(" | ", " |& ", 1)
    options = rng.choice(("", "set -o pipefail", "shopt -s lastpipe; set +m", "set -o pipefail; shopt -s lastpipe; set +m", "set -e"))
    context = rng.choice(("direct", "background", "if", "and", "subshell", "assign"))
    if context == "direct":
        use = negate + pipeline
    elif context == "background":
        use = pipeline + " & wait $!"
    elif context == "if":
        use = "if " + negate + pipeline + "; then echo then; else echo else; fi"
    elif context == "and":
        use = negate + pipeline + " && echo and || echo or"
    elif context == "subshell":
        use = "( " + negate + pipeline + " )"
    else:
        use = "v=$(" + pipeline + " 2>/dev/null); printf '<%s>\\n' \"$v\""
    bash_only = stderr_pipe or "shopt" in options or "pipefail" in options
    observe = "echo \"status=$?\"" + (" \"pipestatus=${PIPESTATUS[*]}\"" if bash_only else "")
    return "pipelines", shell_BASH if bash_only else shell_ALL, shell_program(
        "f() { cat; echo fn; return 4; }", options, "echo start", use + " 2>/dev/null", observe)


def shell_lang_lists(rng):
    """Random and-or lists with statuses, and the value of $? after each."""
    def command():
        return rng.choice(("true", "false", "(exit 2)", "(exit 5)", "echo e", ":", "! true", "! false", "{ false; }",
                           "( true )", "f2", "f5", "v=1", "v=$(exit 3)", "! v=1"))
    count = rng.randrange(1, 6)
    words = [command()]
    for _ in range(count):
        words.append(rng.choice((" && ", " || ", "; ", "\n", " | ")))
        words.append(command())
    text = "".join(words)
    wrapper = rng.choice(("", "{ %s; }", "( %s )", "if %s; then echo T; else echo F; fi", "! { %s; }",
                          "while %s; do echo W; break; done", "until %s; do echo U; break; done", "f() { %s; }; f"))
    if wrapper:
        text = wrapper.replace("%s", text.replace("\n", "; "))
    return "lists", shell_ALL, shell_program("f2() { return 2; }; f5() { return 5; }", text + " 2>/dev/null",
                                             "echo \"status=$?\"", "wait")


def shell_lang_compound_commands(rng):
    shape = rng.choice(("if-elif", "while-break", "until-continue", "for-list", "for-empty", "for-positional", "nested-loops",
                        "case-multi", "case-fallthrough", "case-test-next", "case-pattern", "group", "subshell", "for-arith",
                        "select", "while-read", "break-levels", "continue-levels", "loop-redirect", "loop-status"))
    n = rng.randrange(1, 5)
    if shape == "if-elif":
        script = "x=%d; if [ $x -eq 1 ]; then echo one; elif [ $x -eq 2 ]; then echo two; elif false; then echo never; else echo other; fi" % n
    elif shape == "while-break":
        script = "i=0; while [ $i -lt 10 ]; do i=$((i+1)); [ $i -eq %d ] && break; done; echo $i" % n
    elif shape == "until-continue":
        script = "i=0; until [ $i -ge %d ]; do i=$((i+1)); [ $i -eq 2 ] && continue; printf '%%s ' $i; done; echo" % (n + 2)
    elif shape == "for-list":
        script = "for v in a 'b c' \"$x\" $x '' *; do printf '<%s>' \"$v\"; done; echo"
    elif shape == "for-empty":
        script = "for v in; do echo never; done; echo \"$?\"; for v in $empty; do echo never; done; echo done"
    elif shape == "for-positional":
        script = "set -- p 'q r' ''; for v; do printf '<%s>' \"$v\"; done; for v do printf '[%s]' \"$v\"; done; echo"
    elif shape == "nested-loops":
        script = "for a in 1 2; do for b in x y; do printf '%s%s ' $a $b; done; done; echo"
    elif shape == "case-multi":
        script = "for w in a b c ab '' '*'; do case $w in a|b) printf A;; a*) printf P;; '') printf E;; \\*) printf S;; *) printf O;; esac; done; echo"
    elif shape == "case-fallthrough":
        script = "case %s in 1) echo one;& 2) echo two;; 3) echo three;& *) echo star;; esac" % rng.choice(("1", "2", "3", "4"))
    elif shape == "case-test-next":
        script = "case %s in 1) echo one;;& [12]) echo digit;;& 3) echo three;; *) echo star;; esac" % rng.choice(("1", "2", "3", "4"))
    elif shape == "case-pattern":
        pattern = rng.choice(("[[:digit:]]*", "[!a]*", "a\\*b", "\"$p\"", "$p", "'a*'", "*.txt", "?", "??*", "[a-c]", "@(a|b)", "a|b|c"))
        script = "p='a*'; for w in a ab a*b 1x '*' 'a b'; do case $w in " + pattern + ") printf Y;; *) printf N;; esac; done; echo"
    elif shape == "group":
        script = "{ echo a; echo b; } | wc -l; { x=1; }; echo $x; { echo c; } > out; cat out"
    elif shape == "subshell":
        script = "x=out; (x=in; cd /; echo $x $PWD); echo $x $(pwd | wc -c)"
    elif shape == "for-arith":
        script = "for ((i = 0; i < %d; i++)); do printf '%%s ' $i; done; echo; for ((;;)); do echo once; break; done" % n
    elif shape == "select":
        script = "select v in a b 'c d'; do echo \"<$v:$REPLY>\"; break; done <<EOF\n%s\nEOF\necho \"$?\"" % rng.choice(("1", "2", "3", "4", "x", "", " 2 "))
    elif shape == "while-read":
        script = "printf 'a b\\nc\\n' | while read -r a b; do printf '<%s|%s>' \"$a\" \"$b\"; done; echo"
    elif shape == "break-levels":
        script = "for a in 1 2; do for b in 1 2; do echo $a$b; break %d; done; done; echo \"$?\"" % rng.choice((1, 2, 3, 0))
    elif shape == "continue-levels":
        script = "for a in 1 2; do for b in 1 2; do echo $a$b; continue %d; done; echo inner-end; done" % rng.choice((1, 2))
    elif shape == "loop-redirect":
        script = "for i in 1 2; do echo $i; done > out < /dev/null; cat out; while read -r l; do echo \"<$l>\"; done < out"
    else:
        script = "for i in 1; do (exit 3); done; echo $?; while false; do :; done; echo $?; until true; do :; done; echo $?"
    bash_only = shape in ("case-fallthrough", "case-test-next", "for-arith", "select") or (shape == "case-pattern" and "@(" in script)
    return "compound-" + shape, shell_BASH if bash_only else shell_ALL, shell_program("x='X Y'; empty=", script + " 2>/dev/null",
                                                                                      "echo \"status=$?\"")


def shell_lang_double_bracket(rng):
    left = rng.choice(("abc", "\"a b\"", "''", "$x", "\"$x\"", "$empty", "\"$empty\"", "$unset", "10", "010", "0x1f", "1+2",
                       "x", "a.txt", "dir", "link", "missing", "-n", "!", "(", "\"$(echo abc)\"", "*.txt", "ab*"))
    operator = rng.choice(("==", "=", "!=", "<", ">", "=~", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "-nt", "-ot", "-ef",
                           "&&", "||", "-z", "-n", "-e", "-f", "-d", "-r", "-w", "-x", "-s", "-L", "-h", "-p", "-S", "-b",
                           "-c", "-g", "-u", "-k", "-O", "-G", "-N", "-t", "-v", "-o", "-R", "-a", "-qq", ""))
    right = rng.choice(("abc", "\"a b\"", "a*", "\"a*\"", "$p", "\"$p\"", "'a'*", "^a.*c$", "^(a)(b)", "\"^a\"", "a\\.c",
                        "[[:alpha:]]+", "$r", "[", "", "x", "3", "1+2", "a.txt", "b.txt", "missing", "pipefail", "noglob",
                        "]]", "\\]\\]", "@(a|b)", "!(abc)", "$(echo abc)"))
    negate = rng.choice(("", "", "! "))
    if operator in ("-z", "-n", "-e", "-f", "-d", "-r", "-w", "-x", "-s", "-L", "-h", "-p", "-S", "-b", "-c", "-g", "-u",
                    "-k", "-O", "-G", "-N", "-t", "-v", "-o", "-R"):
        expression = negate + operator + " " + left
    elif operator in ("&&", "||"):
        expression = negate + left + " " + operator + " " + right
    elif operator == "":
        expression = negate + left
    else:
        expression = negate + left + " " + operator + " " + right
    grouping = rng.choice(("", "", "( %s ) && ! ( 1 -eq 2 )", "%s || -n \"\"", "( %s )"))
    if grouping:
        expression = grouping.replace("%s", expression)
    fold = rng.choice(("", "", "shopt -s nocasematch", "shopt -s extglob"))
    return "double-bracket", shell_BASH, shell_program(
        "x='a b'; empty=; p='a*'; r='^a'; unset unset; ln -sf a.txt link; set " + rng.choice(("-u", "+u")),
        fold, "[[ " + expression + " ]] 2>/dev/null; echo \"status=$?\"",
        "printf '<%s>' \"${BASH_REMATCH[@]}\"; echo", "if [[ " + expression + " ]] 2>/dev/null; then echo T; else echo F; fi")


def shell_lang_functions(rng):
    shape = rng.choice(("posix", "keyword", "keyword-parens", "newline-body", "recursion", "local-dynamic", "return-codes",
                        "positional", "shift-inside", "unset-f", "redefine", "redirect-def", "redirect-call", "funcname",
                        "caller", "nested-def", "reserved-name", "return-outside", "return-sourced", "local-outside",
                        "declare-f", "export-f", "readonly-f", "name-dash", "body-subshell", "body-group-oneline"))
    if shape == "posix":
        script = "f() { echo body $1; return 3; }; f arg; echo $?"
    elif shape == "keyword":
        script = "function f { echo body $1; }; f arg"
    elif shape == "keyword-parens":
        script = "function f() { echo body; }; f"
    elif shape == "newline-body":
        script = "f()\n{\n  echo body\n}\nf"
    elif shape == "recursion":
        depth = rng.choice((3, 20, 100, 300))
        script = "f() { local n=$1; [ \"$n\" -eq 0 ] && return 0; f $((n - 1)); }; f %d; echo \"deep=$?\"" % depth
    elif shape == "local-dynamic":
        script = "g() { echo \"g:$x\"; x=g; }; f() { local x=f; g; echo \"f:$x\"; }; x=outer; f; echo \"outer:$x\""
    elif shape == "return-codes":
        code = rng.choice(("0", "1", "255", "256", "300", "-1", "bad", "", "1 2", "--", "-- 3", "+7", " 3 ", "9999999999"))
        script = "f() { return " + code + "; echo tail; }; f; echo \"status=$?\""
    elif shape == "positional":
        script = "f() { echo \"$#:$1:$2:$*\"; set -- new; echo \"$#:$1\"; }; set -- outer o2; f a 'b c'; echo \"$#:$1\""
    elif shape == "shift-inside":
        script = "f() { shift; echo \"$#:$1\"; shift 5; echo \"$?\"; }; f a b c; echo \"$#\""
    elif shape == "unset-f":
        script = "f() { echo body; }; f; unset -f f; f 2>/dev/null; echo \"$?\"; unset -f missing; echo \"$?\""
    elif shape == "redefine":
        script = "f() { echo one; }; f; f() { echo two; }; f; f() { f() { echo three; }; echo two-again; }; f; f"
    elif shape == "redirect-def":
        script = "f() { echo body; echo err >&2; } > out 2>&1; f; cat out"
    elif shape == "redirect-call":
        script = "f() { echo body; cat; }; echo in | f > out; cat out"
    elif shape == "funcname":
        script = "g() { echo \"${FUNCNAME[*]}:${#BASH_SOURCE[@]}:${#BASH_LINENO[@]}\"; }; f() { g; }; f; echo \"${FUNCNAME-none}\""
    elif shape == "caller":
        script = "f() { caller; caller 0 | cut -d' ' -f1; }; f; caller; echo \"$?\""
    elif shape == "nested-def":
        script = "outer() { inner() { echo inner; }; }; outer; inner; type inner | head -1"
    elif shape == "reserved-name":
        name = rng.choice(("if", "for", "done", "in", "then", "{", "!", "[[", "time", "select", "function"))
        script = name + "() { echo body; }; echo \"def=$?\""
    elif shape == "return-outside":
        script = "return 3; echo after; echo \"$?\""
    elif shape == "return-sourced":
        script = "printf 'echo in; return 7; echo never\\n' > src; . ./src; echo \"$?\"; f() { . ./src; echo tail; }; f; echo \"$?\""
    elif shape == "local-outside":
        script = "local v=1; echo \"$?\""
    elif shape == "declare-f":
        script = "f() { echo a; }; declare -F f; declare -f f | head -1; declare -F missing; echo \"$?\""
    elif shape == "export-f":
        script = "f() { echo exported; }; export -f f; bash -c f; /bin/sh -c 'f 2>/dev/null || echo none'"
    elif shape == "readonly-f":
        script = "f() { echo a; }; readonly -f f; unset -f f 2>/dev/null; echo \"$?\"; f() { echo b; } 2>/dev/null; echo \"$?\"; f"
    elif shape == "name-dash":
        script = rng.choice(("f-g", "f.g", "f:g", "_f", "f2", "1f", "f=g", "a/b")) + "() { echo body; }; echo \"def=$?\""
    elif shape == "body-subshell":
        script = "f() ( x=in; echo $x ); x=out; f; echo $x"
    else:
        script = "f() { echo a; }; f; f() { :; }; f; echo $?"
    bash_only = shape in ("keyword", "keyword-parens", "funcname", "caller", "declare-f", "export-f", "readonly-f",
                          "body-subshell") or (shape == "reserved-name" and script.split("(")[0] in ("[[", "time", "select", "function"))
    return "functions-" + shape, shell_BASH if bash_only else shell_ALL, shell_program(script + " 2>/dev/null",
                                                                                      "echo \"end=$?\"")


shell_SIGNAL_SPELLINGS = ("EXIT", "0", "ERR", "RETURN", "DEBUG", "INT", "2", "SIGINT", "sigint", "int", "TERM", "15",
                          "SIGTERM", "USR1", "10", "SIGUSR1", "USR2", "12", "HUP", "1", "QUIT", "3", "ALRM", "14", "CHLD",
                          "17", "WINCH", "28", "PIPE", "13", "KILL", "9", "STOP", "19", "CONT", "18", "TSTP", "20",
                          "RTMIN", "RTMIN+1", "RTMAX", "64", "65", "999", "BOGUS", "-1", "", "SIG", "SIGRTMIN+3")


def shell_lang_traps(rng):
    shape = rng.choice(("set-and-kill", "listing", "trap-p", "trap-l", "reset", "ignore", "ignore-inherited", "exit-status",
                        "exit-in-trap", "exit-trap-status", "err-trap", "err-errtrace", "return-trap", "debug-trap",
                        "subshell-trap", "function-trap", "multi-line", "self-removing", "waiting", "child-delivery",
                        "kill-l", "status-128", "trap-in-subst", "several-signals", "bad-operand", "numeric-unset"))
    signal = rng.choice(shell_SIGNAL_SPELLINGS)
    deliverable = signal.upper().replace("SIG", "") in ("INT", "TERM", "USR1", "USR2", "HUP", "QUIT", "ALRM", "WINCH", "PIPE", "CHLD") \
        or signal in ("2", "15", "10", "12", "1", "3", "14", "28", "13", "17")
    if not deliverable and shape in ("set-and-kill", "reset", "ignore", "exit-in-trap", "subshell-trap", "numeric-unset", "several-signals"):
        signal = rng.choice(("USR1", "10", "SIGUSR1", "usr1", "TERM", "15", "INT", "HUP", "QUIT", "ALRM"))
    number = signal.upper().replace("SIG", "")
    if shape == "set-and-kill":
        script = "trap 'echo caught' " + signal + "; echo \"set=$?\"; kill -" + number + " $$; echo \"after=$?\""
    elif shape == "listing":
        script = "trap 'echo a' " + signal + "; trap '' QUIT; trap; echo \"$?\""
    elif shape == "trap-p":
        script = "trap 'echo a; echo b' " + signal + "; trap -p " + signal + "; trap -p; trap -p | wc -l"
    elif shape == "trap-l":
        script = "trap -l | wc -l; trap -l | head -1; trap -l | tail -c 40"
    elif shape == "reset":
        script = "trap 'echo a' " + signal + "; trap - " + signal + "; trap; sh -c 'kill -" + number + " $$; echo survived'; echo \"$?\""
    elif shape == "ignore":
        script = "trap '' " + signal + "; kill -" + number + " $$; echo \"ignored=$?\"; sh -c 'kill -" + number + " $$; echo child-alive'"
    elif shape == "ignore-inherited":
        script = "sh -c 'trap \"\" USR1; exec \"$0\" -c \"trap - USR1; trap \\\"echo caught\\\" USR1; kill -USR1 \\$\\$; echo alive; trap -p USR1\"' \"$(readlink /proc/$$/exe)\""
    elif shape == "exit-status":
        script = "trap 'echo \"exit:$?\"' EXIT; " + rng.choice(("exit 3", "false", "true", "(exit 5)", "exit", "false; exit", "kill -TERM $$"))
    elif shape == "exit-in-trap":
        script = "trap 'echo in-trap; exit 9' " + signal + "; kill -" + number + " $$; echo never"
    elif shape == "exit-trap-status":
        script = "trap 'exit " + rng.choice(("7", "", "$?", "300")) + "' EXIT; " + rng.choice(("exit 3", "false", ":"))
    elif shape == "err-trap":
        script = "trap 'echo \"err:$?\"' ERR; false; (exit 3); true; f() { false; }; f; false || echo or; if false; then :; fi; echo done"
    elif shape == "err-errtrace":
        script = "set " + rng.choice(("-E", "+E", "-o errtrace")) + "; trap 'echo err' ERR; f() { false; echo in; }; f; (false); echo done"
    elif shape == "return-trap":
        script = "set " + rng.choice(("-T", "+T", "-o functrace")) + "; trap 'echo ret' RETURN; f() { :; }; f; printf 'echo src\\n' > src; . ./src; echo done"
    elif shape == "debug-trap":
        script = "trap 'echo \"debug:$BASH_COMMAND\"' DEBUG; echo one; x=1; f() { echo in; }; f; trap - DEBUG; echo two"
    elif shape == "subshell-trap":
        script = "trap 'echo parent' " + signal + "; (trap 'echo child' " + signal + "; kill -" + number + " $$); (kill -" + number + " $$); echo done"
    elif shape == "function-trap":
        script = "f() { trap 'echo in-f' USR1; kill -USR1 $$; }; f; kill -USR1 $$; echo done"
    elif shape == "multi-line":
        script = "trap 'echo a\necho b' USR1; kill -USR1 $$; echo c"
    elif shape == "self-removing":
        script = "trap 'echo t1\ntrap - USR1\necho t2' USR1; kill -USR1 $$; trap -p USR1; trap | wc -l"
    elif shape == "waiting":
        script = "trap 'echo got' USR1; (sleep 0.2; kill -USR1 $$) & sleep 1; echo \"after=$?\"; wait"
    elif shape == "child-delivery":
        script = "sleep 1 & p=$!; kill -" + number + " $p; wait $p; echo \"child=$?\""
    elif shape == "kill-l":
        script = "kill -l " + rng.choice(("9", "15", "143", "1", "64", "65", "0", "TERM", "SIGTERM", "term", "128", "", "9 15", "-1", "abc")) + "; echo \"$?\"; kill -l | wc -w"
    elif shape == "status-128":
        script = "sh -c 'kill -" + number + " $$'; echo \"$?\"; sh -c 'kill -KILL $$'; echo \"$?\""
    elif shape == "trap-in-subst":
        script = "trap 'echo trapped' USR1; v=$(kill -USR1 $$; echo sub); echo \"$v\"; echo \"$(trap -p USR1)\""
    elif shape == "several-signals":
        script = "trap 'echo multi' USR1 USR2 " + signal + "; kill -USR1 $$; kill -USR2 $$; trap | wc -l"
    elif shape == "bad-operand":
        script = "trap 'echo x' " + signal + "; echo \"status=$?\"; trap -Z; echo \"$?\"; trap 'x' ; echo \"$?\""
    else:
        numeric = {"USR1": "10", "TERM": "15", "INT": "2", "HUP": "1", "QUIT": "3", "ALRM": "14"}.get(number, "10")
        script = "trap 'echo n' " + numeric + "; trap " + numeric + "; trap; sh -c 'kill -" + numeric + " $$; echo survived'; echo \"$?\""
    bash_only = shape in ("trap-l", "err-trap", "err-errtrace", "return-trap", "debug-trap", "trap-p") or signal in ("ERR", "RETURN", "DEBUG", "RTMIN", "RTMIN+1", "RTMAX", "SIGRTMIN+3", "64", "65")
    return "traps-" + shape, shell_BASH if bash_only else shell_ALL, shell_program(script + " 2>/dev/null", "echo \"end=$?\"")


def shell_lang_subshells(rng):
    shape = rng.choice(("var-isolation", "cd-isolation", "exit-status", "pid", "bashpid", "trap-inherit", "errexit",
                        "nested", "positional", "option-isolation", "function-isolation", "umask", "fd-isolation",
                        "command-subst-side", "background-subshell", "exit-in-subst", "subshell-level"))
    if shape == "var-isolation":
        script = "x=out; (x=in; echo $x); echo $x; (unset x); echo ${x-unset}"
    elif shape == "cd-isolation":
        script = "(cd / && pwd); pwd | wc -c"
    elif shape == "exit-status":
        script = "(exit %d); echo $?; (false); echo $?; (true; false); echo $?; ( : ); echo $?" % rng.randrange(0, 300)
    elif shape == "pid":
        script = "a=$$; b=$( echo $$ ); c=$( ( echo $$ ) ); [ \"$a\" = \"$b\" ] && [ \"$a\" = \"$c\" ] && echo same"
    elif shape == "bashpid":
        script = "[ \"$BASHPID\" = \"$$\" ] && echo same; ( [ \"$BASHPID\" != \"$$\" ] && echo differs ); echo $BASH_SUBSHELL; (echo $BASH_SUBSHELL; (echo $BASH_SUBSHELL))"
    elif shape == "trap-inherit":
        script = "trap 'echo exit-trap' EXIT; (echo sub); (trap 'echo sub-exit' EXIT; :); echo main"
    elif shape == "errexit":
        script = "set -e; (false; echo never); echo after"
    elif shape == "nested":
        depth = rng.randrange(1, 6)
        script = "( " * depth + "echo deep; exit 3" + " )" * depth + "; echo $?"
    elif shape == "positional":
        script = "set -- a b; (set -- c; echo $#); echo $#"
    elif shape == "option-isolation":
        script = "(set -e; set -f; echo \"$-\"); echo \"$-\""
    elif shape == "function-isolation":
        script = "(f() { echo in; }; f); f 2>/dev/null; echo \"$?\""
    elif shape == "umask":
        script = "umask 022; (umask 077; umask); umask"
    elif shape == "fd-isolation":
        script = "(exec 3>out; echo x >&3); echo y >&3 2>/dev/null; echo \"$?\"; cat out"
    elif shape == "command-subst-side":
        script = "x=1; v=$(x=2; echo $x); echo \"$x $v\"; y=$(cd /; pwd); echo \"$y\" | wc -c"
    elif shape == "background-subshell":
        script = "(sleep 0.1; echo bg) & wait; echo done"
    elif shape == "exit-in-subst":
        script = "v=$(echo out; exit 7); echo \"$? <$v>\""
    else:
        script = "echo ${BASH_SUBSHELL-none}; (echo ${BASH_SUBSHELL-none}); echo $(echo ${BASH_SUBSHELL-none})"
    bash_only = shape in ("bashpid", "subshell-level")
    return "subshells-" + shape, shell_BASH if bash_only else shell_ALL, shell_program(script + " 2>/dev/null", "echo \"end=$?\"")


def shell_lang_background_wait(rng):
    shape = rng.choice(("wait-all", "wait-pid", "pid-numeric", "retained-status", "wait-many", "wait-unknown", "wait-consumed",
                        "wait-table", "wait-invalid", "stdin-devnull", "stdin-override", "ignores-int", "pipeline-status",
                        "pipeline-pipefail", "signal-status", "interrupted-wait", "wait-n", "wait-f", "wait-p", "monitor-flag",
                        "dollar-bang", "nounset-bang", "many-jobs", "wait-job-spec", "kill-job-spec"))
    if shape == "wait-all":
        script = "true & false & wait; echo \"$?\""
    elif shape == "wait-pid":
        script = "(exit 7) & wait $!; echo \"$?\""
    elif shape == "pid-numeric":
        script = "sleep 0 & p=$!; case $p in ''|*[!0-9]*) echo bad;; *) echo pid;; esac; wait \"$p\"; echo \"$?\""
    elif shape == "retained-status":
        script = "(exit 7) & p=$!; sleep 0.1; wait \"$p\"; echo \"$?\""
    elif shape == "wait-many":
        script = "(exit 3) & a=$!; (exit 7) & b=$!; wait \"$a\" \"$b\"; echo \"$?\"; wait \"$b\" \"$a\"; echo \"$?\""
    elif shape == "wait-unknown":
        script = "wait 999999; echo \"$?\"; wait nope; echo \"$?\"; wait -1; echo \"$?\""
    elif shape == "wait-consumed":
        script = "(exit 0) & p=$!; wait \"$p\"; wait \"$p\"; echo \"$?\""
    elif shape == "wait-table":
        script = "sleep 0.1 & p=$!; (wait \"$p\"; echo sub:$?); wait \"$p\"; echo parent:$?"
    elif shape == "wait-invalid":
        script = "sleep 0.05 & p=$!; wait \"$p\" bad 2>/dev/null; echo \"first:$?\"; wait \"$p\"; echo \"second:$?\""
    elif shape == "stdin-devnull":
        script = "read stolen &\np=$!\nwait \"$p\"\necho \"read:$?\"\necho after"
    elif shape == "stdin-override":
        script = "printf 'given\\n' > in; read v < in & wait $!; echo \"$?\""
    elif shape == "ignores-int":
        script = "for s in INT QUIT; do (sleep 0.1; echo \"$s-survived\") & p=$!; sleep 0.02; kill -$s $p; wait $p; echo \"$?\"; done"
    elif shape == "pipeline-status":
        script = "false | (exit 7) & p=$!; wait \"$p\"; echo \"$?\""
    elif shape == "pipeline-pipefail":
        script = "set -o pipefail; false | true & p=$!; wait \"$p\"; echo \"$?\""
    elif shape == "signal-status":
        script = "sh -c 'kill -TERM $$' & p=$!; sleep 0.1; wait \"$p\"; echo \"$?\"; sleep 1 & kill -KILL $!; wait $!; echo \"$?\""
    elif shape == "interrupted-wait":
        script = "trap 'echo usr1' USR1; (sleep 0.1; kill -USR1 $$) & sleep 0.5 & t=$!; wait \"$t\"; echo \"w=$?\"; wait; echo done"
    elif shape == "wait-n":
        script = "(exit 3) & (exit 5) & wait -n; echo \"n=$?\"; wait -n; echo \"n=$?\"; wait -n; echo \"n=$?\""
    elif shape == "wait-f":
        script = "sleep 0.2 & p=$!; wait -f \"$p\"; echo \"$?\""
    elif shape == "wait-p":
        script = "(exit 4) & wait -n -p named; echo \"$?:${named:+set}\""
    elif shape == "monitor-flag":
        script = "case $- in *m*) echo on;; *) echo off;; esac; set -m; case $- in *m*) echo on;; *) echo off;; esac; set +m"
    elif shape == "dollar-bang":
        script = "echo \"[$!]\"; true & echo \"${!:+set}\"; wait; echo \"${!:+set}\""
    elif shape == "nounset-bang":
        script = "set -u; echo \"[$!]\"; echo \"$?\""
    elif shape == "many-jobs":
        script = "for i in 1 2 3 4 5 6 7 8; do (exit $i) & done; wait; echo \"$?\"; wait; echo \"$?\""
    elif shape == "wait-job-spec":
        script = "set -m; sleep 0.2 & wait %1; echo \"$?\"; wait %9 2>/dev/null; echo \"$?\""
    else:
        script = "set -m; sleep 2 & kill %1; wait %1; echo \"$?\"; sleep 2 & kill -s TERM %1; wait $!; echo \"$?\""
    bash_only = shape in ("wait-n", "wait-f", "wait-p", "pipeline-pipefail")
    return "background-" + shape, shell_BASH if bash_only else shell_ALL, shell_program(script + " 2>/dev/null", "echo \"end=$?\"")


def shell_lang_errexit_contexts(rng):
    failing = rng.choice(("false", "(exit 3)", "f", "sh -c 'exit 7'", "cat missing", "nosuchcommand", "! true", "x=$(false)",
                          "local x=$(false)", "declare x=$(false)", "eval false", ". ./fails", "false | true", "true | false",
                          "{ false; }", "( false )", "[ 1 -eq 2 ]", "test -f missing", "let 0", "(( 0 ))", "[[ a == b ]]",
                          "false && true", "read v < /dev/null", "return 3"))
    context = rng.choice(("plain", "if-cond", "while-cond", "until-cond", "and-left", "and-right", "or-left", "or-right", "negated",
                          "pipeline-left", "pipeline-right", "function-body", "function-tested", "subshell", "subshell-tested",
                          "group", "group-tested", "subst-assign", "subst-word", "case-word", "for-word", "eval", "trap-exit",
                          "loop-body", "elif", "last-in-and"))
    options = rng.choice(("set -e", "set -e", "set -e -o pipefail", "set -e; shopt -s inherit_errexit", "set +e", "set -eE"))
    body = failing
    if context == "if-cond":
        body = "if " + failing + "; then echo T; else echo F; fi"
    elif context == "while-cond":
        body = "while " + failing + "; do echo W; break; done"
    elif context == "until-cond":
        body = "until " + failing + "; do echo U; break; done"
    elif context == "and-left":
        body = failing + " && echo and"
    elif context == "and-right":
        body = "true && " + failing
    elif context == "or-left":
        body = failing + " || echo or"
    elif context == "or-right":
        body = "false || " + failing
    elif context == "negated":
        body = "! " + failing
    elif context == "pipeline-left":
        body = failing + " | cat"
    elif context == "pipeline-right":
        body = "echo x | " + failing
    elif context == "function-body":
        body = "g() { " + failing + "; echo in-g; }; g"
    elif context == "function-tested":
        body = "g() { " + failing + "; echo in-g; }; g || echo caught; if g; then :; fi"
    elif context == "subshell":
        body = "( " + failing + "; echo in-sub )"
    elif context == "subshell-tested":
        body = "( " + failing + "; echo in-sub ) || echo caught"
    elif context == "group":
        body = "{ " + failing + "; echo in-group; }"
    elif context == "group-tested":
        body = "{ " + failing + "; echo in-group; } || echo caught"
    elif context == "subst-assign":
        body = "v=$(" + failing + "; echo sub); echo \"v=$v\""
    elif context == "subst-word":
        body = "echo \"[$(" + failing + "; echo sub)]\""
    elif context == "case-word":
        body = "case $(" + failing + "; echo w) in w) echo w;; esac"
    elif context == "for-word":
        body = "for i in $(" + failing + "; echo a b); do echo $i; done"
    elif context == "eval":
        body = "eval " + shell_quote(failing)
    elif context == "trap-exit":
        body = "trap 'echo \"exit:$?\"' EXIT; " + failing
    elif context == "loop-body":
        body = "for i in 1 2; do " + failing + "; echo body-$i; done"
    elif context == "elif":
        body = "if false; then :; elif " + failing + "; then echo T; else echo F; fi"
    else:
        body = "true && " + failing + " && echo last"
    bash_only = "shopt" in options or "pipefail" in options or "-eE" in options or failing in ("local x=$(false)", "declare x=$(false)", "let 0", "(( 0 ))", "[[ a == b ]]")
    return "errexit-contexts", shell_BASH if bash_only else shell_ALL, shell_program(
        "f() { return 3; }; printf 'false\\n' > fails", options, "echo start", body + " 2>/dev/null", "echo \"after=$?\"")


def shell_lang_nounset_forms(rng):
    form = rng.choice(("$x", "${x}", "${x-}", "${x:-d}", "${x+set}", "${#x}", "${x#a}", "${x%a}", "${x/a/b}", "${x:0:1}",
                       "${x^^}", "${x@Q}", "$1", "${10}", "$@", "$*", "\"$@\"", "\"$*\"", "${@}", "$#", "$!", "${!x}", "${a[0]}",
                       "${a[@]}", "${a[*]}", "${#a[@]}", "${a[9]}", "${b[@]}", "$((x + 1))", "$((y + 1))", "${x=assigned}",
                       "$0", "$-", "$$", "$?", "${x?custom}", "$_", "${PWD}", "${IFS}", "$LINENO", "${empty}", "${x:?}"))
    context = rng.choice(("echo", "assign", "case", "for", "redirect", "heredoc", "arith", "dbl", "test", "function"))
    setup = rng.choice(("unset x", "x=", "x=abc", "unset x; a=()", "unset x; a=(one)", "set -- p1"))
    if context == "echo":
        use = "echo \"[" + form + "]\""
    elif context == "assign":
        use = "v=" + form + "; echo \"[$v]\""
    elif context == "case":
        use = "case " + form + " in '') echo empty;; *) echo other;; esac"
    elif context == "for":
        use = "for i in " + form + "; do echo \"<$i>\"; done"
    elif context == "redirect":
        use = "echo out > \"out" + form.replace("/", "_").replace("*", "s").replace("@", "a").replace("#", "n").replace("$", "d").replace("!", "b").replace(" ", "_") + "\" 2>/dev/null; ls | wc -l"
    elif context == "heredoc":
        use = "cat <<EOF\n[" + form + "]\nEOF"
    elif context == "arith":
        use = "echo $(( ${x:-0} + 1 ))"
    elif context == "dbl":
        use = "[[ -z " + form + " ]]; echo \"dbl=$?\""
    elif context == "test":
        use = "[ -z \"" + form + "\" ]; echo \"test=$?\""
    else:
        use = "g() { echo \"[" + form + "]\"; }; g; echo tail"
    bash_only = context == "dbl" or any(t in form for t in ("a[", "b[", "^^", "@Q", ":0:1", "/a/b", "${!x}", "$_", "LINENO"))
    return "nounset-forms", shell_BASH if bash_only else shell_ALL, shell_program(
        "empty=; set -u", setup, "echo start", "exec 2>/dev/null", use, "echo \"after=$?\"")


def shell_lang_noclobber(rng):
    target = rng.choice(("existing", "absent", "/dev/null", "link", "devlink", "dangling", "dir", "empty"))
    operator = rng.choice((">", ">|", ">>", "<>", "&>", "2>", "1>", ">&2 >", "3>"))
    setting = rng.choice(("set -C", "set -o noclobber", "set -C; set +C", "set -C; set +o noclobber", ""))
    prefix = {"existing": ": > t", "absent": "rm -f t", "/dev/null": "t=/dev/null", "link": ": > real; ln -s real t",
              "devlink": "ln -s /dev/null t", "dangling": "ln -s nowhere t", "dir": "mkdir t", "empty": ": > t"}[target]
    name = "t" if target != "/dev/null" else "$t"
    use = "echo data " + operator + " " + name
    if operator == ">&2 >":
        use = "echo data >&2 > " + name
    modes = shell_BASH if operator == "&>" else shell_ALL
    return "noclobber", modes, shell_program(prefix, setting, "echo start", use + " 2>/dev/null", "echo \"status=$?\"",
                                             "[ -f t ] && cat t; ls -l t 2>/dev/null | cut -c1 ; :")


def shell_lang_allexport_noglob(rng):
    shape = rng.choice(("assign", "for", "read", "local", "arith", "default", "prefix", "special", "getopts", "unset-export",
                        "noglob-word", "noglob-quoted", "noglob-set-f", "noglob-toggle", "noglob-case", "noglob-redirect"))
    if shape == "assign":
        script = "set -a; v=1; /bin/sh -c 'echo \"${v-unset}\"'; set +a; w=2; /bin/sh -c 'echo \"${w-unset}\"'"
    elif shape == "for":
        script = "set -a; for i in x; do :; done; /bin/sh -c 'echo \"${i-unset}\"'"
    elif shape == "read":
        script = "set -a; echo val | { read r; /bin/sh -c 'echo \"${r-unset}\"'; }; echo val | read r2; /bin/sh -c 'echo \"${r2-unset}\"'"
    elif shape == "local":
        script = "set -a; f() { local l=1; /bin/sh -c 'echo \"${l-unset}\"'; }; f; /bin/sh -c 'echo \"${l-unset}\"'"
    elif shape == "arith":
        script = "set -a; : $((n = 7)); /bin/sh -c 'echo \"${n-unset}\"'"
    elif shape == "default":
        script = "set -a; unset d; : \"${d:=made}\"; /bin/sh -c 'echo \"${d-unset}\"'"
    elif shape == "prefix":
        script = "set -a; t=1 :; /bin/sh -c 'echo \"${t-unset}\"'; t2=1 true; /bin/sh -c 'echo \"${t2-unset}\"'"
    elif shape == "special":
        script = "set -a; IFS=:; export -p | grep -c IFS; PS1=x; export -p | grep -c PS1"
    elif shape == "getopts":
        script = "set -a; set -- -x; getopts x o; /bin/sh -c 'echo \"${o-unset}:${OPTIND-unset}\"'"
    elif shape == "unset-export":
        script = "set -a; v=1; unset v; v=2; export -p | grep -c ' v='"
    elif shape == "noglob-word":
        script = "set -f; echo *.txt a.tx?; set +f; echo *.txt"
    elif shape == "noglob-quoted":
        script = "set -o noglob; x='*.txt'; echo $x \"$x\"; set +o noglob; echo $x"
    elif shape == "noglob-set-f":
        script = "set -f; set -- *; echo \"$#\"; for f in *; do echo \"<$f>\"; done"
    elif shape == "noglob-toggle":
        script = "set -f; echo \"$-\"; set +f; echo \"$-\" | tr -d 'hBs'"
    elif shape == "noglob-case":
        script = "set -f; case a.txt in *.txt) echo match;; esac; case '*' in a*) echo a;; \\*) echo star;; esac"
    else:
        script = "set -f; echo x > *.new; ls | grep -c new"
    return "allexport-noglob", shell_ALL, shell_program(": > a.txt; : > b.txt", script + " 2>/dev/null", "echo \"end=$?\"")


def shell_lang_xtrace_shape(rng):
    ps4 = rng.choice(("+ ", "TRACE ", "", "++", "$LINENO: ", "${FUNCNAME[0]-main}: ", "> ", "\\$ ", "$(echo sub) ", "a\\nb "))
    body = rng.choice(("echo one", "x=1", "x=1 y=2", "x=1 true", "echo $x$x", "echo *.txt", "for i in a b; do :; done",
                       "if true; then echo t; fi", "f() { echo in; }; f", "case x in x) echo c;; esac", "echo out > out",
                       "echo 'a b' \"c d\" e\\ f", "( echo sub )", "{ echo grp; }", "v=$(echo sub)", "echo \"$(echo n)\"",
                       "[ a = a ]", "[[ a == a ]]", "(( 1 + 1 ))", "while false; do :; done", "echo $'\\t'", "exec 3>&1", "set +x; echo quiet",
                       "echo one; echo two", "x='q\"uote'; echo \"$x\"", "echo é", "read v <<EOF\nl\nEOF"))
    where = rng.choice(("set", "set", "set-o", "subshell", "function", "eval"))
    trace = "set -x" if where != "set-o" else "set -o xtrace"
    if where == "subshell":
        use = "( " + trace + "; " + body + " )"
    elif where == "function":
        use = "g() { " + trace + "; " + body + "; }; g"
    elif where == "eval":
        use = trace + "; eval " + shell_quote(body)
    else:
        use = trace + "; " + body
    bash_only = "[[" in body or "((" in body or "$'" in body or "FUNCNAME" in ps4 or "LINENO" in ps4
    return "xtrace-shape", shell_BASH if bash_only else shell_ALL, shell_program(
        ": > a.txt; x=X", "PS4=" + shell_quote(ps4), "exec 2>trace", use, "set +x", "exec 2>&1", "echo ---",
        "cat trace | sed 's/^/|/'", "echo \"end=$?\"")


def shell_lang_verbose(rng):
    body = rng.choice(("echo one\necho two", "x=1\necho $x", "if true; then\necho t\nfi", "echo a; echo b", "cat <<EOF\nhere\nEOF",
                       "f() {\n  echo in\n}\nf", "echo one \\\n two", "# comment\necho after", "eval 'echo evaluated'",
                       ". ./sourced"))
    where = rng.choice(("set", "set-o", "toggle"))
    if where == "toggle":
        use = "set -v\n" + body + "\nset +v\necho quiet"
    elif where == "set-o":
        use = "set -o verbose\n" + body + "\nset +o verbose"
    else:
        use = "set -v\n" + body
    return "verbose", shell_ALL, "printf 'echo sourced\\n' > sourced\nexec 2>verbose\n" + use + "\nexec 2>&1\necho ---\nsed 's/^/|/' verbose\n"


def shell_lang_special_parameters(rng):
    shape = rng.choice(("status", "flags", "count", "zero", "underscore", "pid-stable", "bang", "star-ifs", "at-empty",
                        "at-splice", "shift-forms", "set-dash", "set-forms", "positional-high", "count-after-set", "dollar-dash-set",
                        "readonly-specials"))
    if shape == "readonly-specials":
        name = rng.choice(("SHELLOPTS", "BASHOPTS", "BASHPID", "PPID", "UID", "EUID", "BASH_VERSINFO"))
        script = ("printf '<%s>' \"${" + name + ":+set}\"; " + name + "=changed 2>/dev/null; echo \"assign=$?\"; unset " + name +
                  " 2>/dev/null; echo \"unset=$?\"; printf '<%s>' \"${" + name + ":+set}\"; echo; readonly -p | grep -c \" " + name + "=\"")
        return "special-parameters-" + shape, shell_BASH, shell_program(script, "echo \"end=$?\"")
    if shape == "status":
        script = "true; echo $?; false; echo $?; (exit 255); echo $?; echo $(exit 2)$?; echo $?; { exit 0; }"
    elif shape == "flags":
        script = "printf '<%s>' \"$-\"; set -euxC; printf '<%s>' \"$-\"; set +exC; printf '<%s>' \"$-\"; echo"
    elif shape == "count":
        script = "echo $#; set -- a 'b c' ''; echo $# ${#} \"${#*}\" \"${#@}\""
    elif shape == "zero":
        script = "echo \"${0##*/}\" | sed 's/^-//'; (echo \"${0##*/}\" | sed 's/^-//'); f() { echo \"${0##*/}\" | sed 's/^-//'; }; f"
    elif shape == "underscore":
        script = "echo a b; echo \"$_\"; : x y; echo \"$_\"; echo \"${_-unset}\""
    elif shape == "pid-stable":
        script = "a=$$; (b=$$; [ \"$a\" = \"$b\" ] && echo same); c=$(echo $$); [ \"$a\" = \"$c\" ] && echo same2; [ \"$$\" -gt 1 ] && echo positive"
    elif shape == "bang":
        script = "echo \"[$!]\"; true & p=$!; wait; echo \"[${!:+set}]\"; [ \"$p\" -gt 1 ] && echo pid"
    elif shape == "star-ifs":
        script = "set -- a 'b c' d; IFS=" + shell_quote(rng.choice(("-", "", ":", " ", "xy"))) + "; printf '<%s>' \"$*\" $* \"$@\"; echo; unset IFS; printf '<%s>' \"$*\"; echo"
    elif shape == "at-empty":
        script = "set --; printf '<%s>' \"$@\" \"${@}\" \"$*\" \"${@-none}\" \"${*-none}\" \"${@:-none}\"; echo; for x in \"$@\"; do echo never; done"
    elif shape == "at-splice":
        script = "set -- '' x; for v in pre\"$@\"post; do printf '<%s>' \"$v\"; done; echo; set --; for v in pre\"$@\"post; do printf '<%s>' \"$v\"; done; echo"
    elif shape == "shift-forms":
        script = "set -- a b c d; shift; echo $1; shift 2; echo $1 $#; shift 5; echo \"$?\"; shift 0; echo \"$?\"; shift -1; echo \"$?\"; shift bad; echo \"$?\""
    elif shape == "set-dash":
        script = "set -- -x -y; echo $1 $2; set -- ; echo $#; set --; set -- -- a; echo $1 $2"
    elif shape == "set-forms":
        script = "set a b; echo $#; set -; echo $#; set -- \"\" ; echo $#; set +e -- q; echo $# $1"
    elif shape == "positional-high":
        script = "set -- 1 2 3 4 5 6 7 8 9 10 11 12; echo ${10} ${11} $10 ${12} ${13-none} \"${#}\""
    elif shape == "count-after-set":
        script = "f() { echo $#; set -- x y z; echo $#; }; set -- a; f b c; echo $#"
    else:
        script = "set -o | grep -c ' on' > /dev/null; set -- ; echo \"$-\" | wc -c"
    return "special-parameters-" + shape, shell_BASH if shape == "underscore" else shell_ALL, shell_program(script + " 2>/dev/null", "echo \"end=$?\"")


def shell_lang_aliases(rng):
    shape = rng.choice(("define-run", "trailing-space", "keyword", "recursion", "self", "in-function", "same-line", "quoted-word",
                        "multi-word", "unalias", "redirect-after", "newline-in-alias", "assignment-before",
                        "expand_aliases-off", "posix-mode", "alias-to-builtin", "cycle-through-assignment", "comment-in-alias",
                        "heredoc-in-alias",
                        #       The four below came from hand-written cases that
                        #       this grammar replaced. They are storage shapes a
                        #       random walk does not reach: an alias table slot
                        #       reused rather than freed has to keep the operand,
                        #       the spelling a defined function retained, and a
                        #       held document, across a redefinition.
                        "wrapped-redefinition", "reuse-keeps-function-spelling",
                        "heredoc-survives-replacement"))
    enable = "shopt -s expand_aliases 2>/dev/null; "
    if shape == "define-run":
        script = "alias a='echo aliased'; a"
    elif shape == "trailing-space":
        script = "alias a='echo one '; alias b='two'; a b"
    elif shape == "keyword":
        script = "alias i=if; i true; then echo t; fi"
    elif shape == "recursion":
        script = "alias a=b; alias b=a; a 2>/dev/null; echo \"$?\""
    elif shape == "self":
        script = "alias echo='echo self'; echo x"
    elif shape == "in-function":
        script = "alias a='echo aliased'; f() { a; }; f; unalias a; f"
    elif shape == "same-line":
        script = "alias a='echo aliased'; a; alias b='echo bee' ; b"
    elif shape == "quoted-word":
        script = "alias a='echo aliased'; 'a' 2>/dev/null; echo \"$?\"; \\a 2>/dev/null; echo \"$?\""
    elif shape == "multi-word":
        script = "alias a='echo one two'; a three"
    elif shape == "listing":
        script = "alias a='echo x' b=\"it's\"; alias; alias a; alias missing 2>/dev/null; echo \"$?\"; alias -p 2>/dev/null | wc -l"
    elif shape == "unalias":
        script = "alias a=echo b=echo c=echo; unalias b; alias | wc -l; unalias -a; alias | wc -l; unalias nosuch 2>/dev/null; echo \"$?\""
    elif shape == "redirect-after":
        script = "alias e='echo out'; e>o; cat o; x=1 e>o2; cat o2"
    elif shape == "newline-in-alias":
        script = "alias a='echo one\necho two'; a"
    elif shape == "assignment-before":
        script = "alias a='echo aliased'; X=1 a; X=1 2>/dev/null a"
    elif shape == "expand_aliases-off":
        script = "shopt -u expand_aliases; alias a='echo aliased'; a 2>/dev/null; echo \"$?\""
    elif shape == "posix-mode":
        script = "set -o posix; alias a='echo aliased'; a; alias time='echo t'; time :"
    elif shape == "alias-to-builtin":
        script = "alias cd='echo nocd'; cd /; pwd | wc -c"
    elif shape == "cycle-through-assignment":
        script = "alias a='x=1 a'; a 2>/dev/null; echo \"$?\""
    elif shape == "comment-in-alias":
        script = "alias a='echo one # tail'; a two"
    elif shape == "wrapped-redefinition":
        # A slot reused instead of freed must still carry the operand, and the
        # wrappers must not consume the definition on the way through.
        script = ("f() { builtin alias a=echo; command alias a=printf; alias a; }\n"
                  "f\nf")
    elif shape == "reuse-keeps-function-spelling":
        # A retained function keeps the words it was defined with, whatever the
        # alias becomes afterwards.
        script = ("alias a='echo before'\nf() { a; }\nalias a='echo later'\n"
                  "g() { a; }\nalias a=:\nf; g")
    elif shape == "heredoc-survives-replacement":
        # The document is held inside a substitution and carries an unbalanced
        # parenthesis; the alias is run twice and then redefined.
        script = ("alias a='printf \"<%s>\\n\" \"$(cat <<EOF\n"
                  "inside ) text\nEOF\n)\"'\na\na\nalias a='echo changed'\na")
    else:
        script = "alias a='cat <<EOF'; a\nbody\nEOF"
    modes = shell_BASH if shape in ("expand_aliases-off", "posix-mode", "wrapped-redefinition") else shell_ALL
    return "aliases-" + shape, modes, shell_program(enable + script, "echo \"end=$?\"")


def shell_lang_syntax_errors(rng):
    """A valid program with one token mutated: what ran before, and the status."""
    valid = rng.choice(("if true; then echo t; fi", "for i in 1 2; do echo $i; done", "while false; do :; done", "case x in x) echo c;; esac",
                        "f() { echo in; }; f", "{ echo g; }", "( echo s )", "echo \"quoted\"", "echo 'single'", "echo $(echo sub)",
                        "echo `echo tick`", "echo $((1 + 2))", "cat <<EOF\nbody\nEOF", "true && echo and || echo or",
                        "echo a | cat", "[[ a == a ]] && echo dbl", "(( 1 )) && echo arith", "echo a; echo b", "x=1; echo $x",
                        "echo ${x:-d}", "! false", "echo one &\nwait", "until true; do :; done", "select v in a; do break; done </dev/null",
                        "time true", "function f { echo k; }; f", "echo a >out", "echo a 2>&1", "echo {a,b}", "if true; then\necho t\nelif false; then\n:\nelse\n:\nfi"))
    mutation = rng.choice(("drop", "drop", "insert", "replace", "truncate", "duplicate", "unbalance"))
    tokens = valid.replace("\n", " \n ").split(" ")
    if mutation == "drop" and len(tokens) > 1:
        del tokens[rng.randrange(len(tokens))]
    elif mutation == "insert":
        tokens.insert(rng.randrange(len(tokens) + 1), rng.choice(("fi", "done", "esac", ";;", "|", "&&", "||", ")", "(", "}", "{", "then", "do",
                                                                     "<", ">", ";", "&", "\"", "'", "`", "$(", "((", "[[", "]]", "in", "elif", "else", "!")))
    elif mutation == "replace":
        tokens[rng.randrange(len(tokens))] = rng.choice(("fi", "done", "esac", ";;", "|", "&&", ")", "}", "then", "do", "<", ";", "\"", "'", "`"))
    elif mutation == "truncate":
        tokens = tokens[:rng.randrange(1, len(tokens) + 1)]
    elif mutation == "duplicate":
        at = rng.randrange(len(tokens))
        tokens.insert(at, tokens[at])
    else:
        text = valid
        for pair in ("()", "{}", "[]", "\"\"", "''", "``"):
            text = text.replace(pair[1], "", 1) if pair[1] in text else text
        tokens = text.split(" ")
    mutated = " ".join(tokens).replace(" \n ", "\n")
    position = rng.choice(("alone", "after", "eval", "source", "subshell", "function-body", "dash-c"))
    if position == "alone":
        script = mutated
    elif position == "after":
        script = "echo before\n" + mutated + "\necho after"
    elif position == "eval":
        script = "echo before; eval " + shell_quote(mutated) + "; echo \"eval=$?\""
    elif position == "source":
        script = "printf '%s\\n' " + shell_quote(mutated) + " > bad; echo before; . ./bad; echo \"source=$?\""
    elif position == "subshell":
        script = "echo before; ( " + mutated.replace("\n", "; ") + " ); echo \"sub=$?\""
    elif position == "function-body":
        script = "echo before\nf() {\n" + mutated + "\n}\necho \"def=$?\"; f; echo \"call=$?\""
    else:
        script = "echo before; " + rng.choice(("sh", "bash")) + " -c " + shell_quote(mutated) + " 2>/dev/null; echo \"child=$?\""
    bash_only = any(t in valid for t in ("[[", "((", "select", "time", "function", "{a,b}")) or "bash -c" in script
    return "syntax-errors", shell_BASH if bash_only else shell_ALL, script + "\necho \"end=$?\"\n"


def shell_lang_reserved_words(rng):
    word = rng.choice(("if", "then", "else", "elif", "fi", "for", "in", "do", "done", "case", "esac", "while", "until", "{", "}", "!",
                       "function", "select", "time", "[[", "]]", "coproc", "&&", ";;", "((", "))"))
    position = rng.choice(("argument", "assignment-value", "quoted-command", "escaped-command", "case-pattern", "for-name", "function-name",
                           "after-pipe", "after-semicolon", "heredoc-body", "alias-target", "variable-name"))
    if position == "argument":
        script = "echo " + word + " x"
    elif position == "assignment-value":
        script = "x=" + word + "; echo \"$x\""
    elif position == "quoted-command":
        script = "'" + word + "' 2>/dev/null; echo \"$?\""
    elif position == "escaped-command":
        script = "\\" + word[0] + word[1:] + " 2>/dev/null; echo \"$?\""
    elif position == "case-pattern":
        script = "case " + word + " in " + word.replace("(", "\\(").replace(")", "\\)").replace("[", "\\[").replace("|", "\\|") + ") echo match;; *) echo no;; esac"
    elif position == "for-name":
        script = "for " + word + " in a; do echo body; done"
    elif position == "function-name":
        script = word + "() { echo body; }; echo \"def=$?\""
    elif position == "after-pipe":
        script = "echo x | " + word + " 2>/dev/null; echo \"$?\""
    elif position == "after-semicolon":
        script = "echo a; " + word + " 2>/dev/null; echo \"$?\""
    elif position == "heredoc-body":
        script = "cat <<EOF\n" + word + "\nEOF"
    elif position == "alias-target":
        script = "alias w=" + shell_quote(word) + "; echo defined"
    else:
        script = word + "=1 2>/dev/null; echo \"$?\""
    modes = shell_BASH if word in ("function", "select", "time", "[[", "]]", "coproc", "((", "))") else shell_ALL
    return "reserved-words", modes, shell_program("echo start", script, "echo \"end=$?\"")


def shell_lang_reader_boundaries(rng):
    shape = rng.choice(("long-line", "many-lines", "no-final-newline", "crlf", "nul", "deep-parens", "deep-braces", "long-pipeline",
                        "many-redirects", "backslash-eof", "quote-eof", "heredoc-eof", "long-word", "long-heredoc", "many-heredocs",
                        "long-comment", "boundary-4096", "boundary-65536", "empty-lines", "only-comment", "trailing-spaces"))
    if shape == "long-line":
        n = rng.choice((4000, 4096, 4097, 8191, 70000))
        script = "x=" + "a" * n + "; echo ${#x}"
    elif shape == "many-lines":
        script = "\n".join("i=%d" % k for k in range(rng.choice((500, 2000)))) + "\necho $i"
    elif shape == "no-final-newline":
        return "reader-no-final-newline", shell_ALL, "echo one\necho two"
    elif shape == "crlf":
        script = "echo one\r\necho two\r"
    elif shape == "nul":
        script = "printf 'echo one\\necho \\0two\\necho three\\n' > nul.sh; sh ./nul.sh; echo \"file=$?\""
    elif shape == "deep-parens":
        depth = rng.choice((10, 50, 150))
        script = "echo " + "$(" * depth + "echo deep" + ")" * depth
    elif shape == "deep-braces":
        depth = rng.choice((10, 50, 200))
        script = "{ " * depth + "echo deep; " + "} " * depth
    elif shape == "long-pipeline":
        stages = rng.choice((64, 65, 128, 257))
        script = "echo x" + " | cat" * stages + "; echo \"${PIPESTATUS[0]-none}\""
    elif shape == "many-redirects":
        script = "true" + "".join(" %d>o%d" % (3 + k % 7, k) for k in range(25)) + "; ls | wc -l"
    elif shape == "backslash-eof":
        return "reader-backslash-eof", shell_ALL, "echo one\necho two \\"
    elif shape == "quote-eof":
        return "reader-quote-eof", shell_ALL, "echo one\necho \"open"
    elif shape == "heredoc-eof":
        return "reader-heredoc-eof", shell_ALL, "echo one\ncat <<EOF\nbody\n"
    elif shape == "long-word":
        script = "printf '%s\\n' " + "w" * 50000 + " | wc -c"
    elif shape == "long-heredoc":
        lines = rng.choice((1000, 1800, 4000))
        script = "cat <<EOF | wc -l\n" + "\n".join("line %d" % k for k in range(lines)) + "\nEOF"
    elif shape == "many-heredocs":
        count = rng.choice((9, 40))
        script = " ".join("cat <<E%d;" % k for k in range(count)) + " :\n" + "".join("body%d\nE%d\n" % (k, k) for k in range(count))
    elif shape == "long-comment":
        script = "# " + "c" * 70000 + "\necho after"
    elif shape == "boundary-4096":
        pad = 4096 - len("echo start\n") - len("echo ") - 1
        script = "echo start\necho " + "b" * pad + "\necho after"
    elif shape == "boundary-65536":
        script = "echo start\n: " + "b" * 65530 + "\necho after"
    elif shape == "empty-lines":
        script = "\n\n\necho one\n\n\n\necho two\n\n"
    elif shape == "only-comment":
        script = "# nothing here"
    else:
        script = "echo one   \necho two\t\t\n   \n\t\necho three"
    modes = shell_BASH if "PIPESTATUS" in script else shell_ALL
    return "reader-" + shape, modes, script + "\necho \"end=$?\"\n"


def shell_lang_dot_source(rng):
    shape = rng.choice(("plain", "args", "return", "return-in-function", "break-in-loop", "missing", "missing-command", "path-search",
                        "cwd-fallback", "sourcepath-off", "nested", "set-persists", "shift-inside", "syntax-error", "source-word",
                        "no-operand", "option-end", "dev-stdin", "relative-dot", "unreadable"))
    files = "printf 'echo in:$#:${1-none}; v=set\\n' > f; printf 'return 7; echo never\\n' > r; printf 'set -- replaced\\n' > s; mkdir -p d; printf 'echo pathfile\\n' > d/p; printf 'echo local\\n' > p; printf 'echo \"unclosed\\n' > bad"
    if shape == "plain":
        script = ". ./f; echo \"$v\""
    elif shape == "args":
        script = "set -- outer; . ./f a 'b c'; echo \"$#:$1\""
    elif shape == "return":
        script = ". ./r; echo \"$?\""
    elif shape == "return-in-function":
        script = "g() { . ./r; echo tail; }; g; echo \"$?\""
    elif shape == "break-in-loop":
        script = "printf 'break\\n' > b; for i in 1 2; do . ./b; echo never; done; echo \"$?\""
    elif shape == "missing":
        script = ". ./missing; echo after"
    elif shape == "missing-command":
        script = "command . ./missing; echo \"after:$?\""
    elif shape == "path-search":
        script = "PATH=$PWD/d:$PATH; . p; echo \"$?\""
    elif shape == "cwd-fallback":
        script = "PATH=/nonexistent; . p 2>/dev/null; echo \"$?\"; . ./p"
    elif shape == "sourcepath-off":
        script = "shopt -u sourcepath; PATH=$PWD/d:$PATH; . p; echo \"$?\""
    elif shape == "nested":
        script = "printf '. ./f nested\\n' > n; . ./n outer; echo \"$#\""
    elif shape == "set-persists":
        script = "set -- a b; . ./s; echo \"$#:$1\""
    elif shape == "shift-inside":
        script = "printf 'shift\\n' > sh1; set -- a b c; . ./sh1; echo \"$#:$1\""
    elif shape == "syntax-error":
        script = ". ./bad; echo \"after:$?\""
    elif shape == "source-word":
        script = "source ./f x; echo \"$v\""
    elif shape == "no-operand":
        script = ".; echo \"status:$?\""
    elif shape == "option-end":
        script = ". -- ./f; echo \"$?\"; . -x ./f; echo \"$?\""
    elif shape == "dev-stdin":
        script = "printf 'echo from-stdin\\n' | { . /dev/stdin; echo \"$?\"; }"
    elif shape == "relative-dot":
        script = "cd d && . ./p; echo \"$?\""
    else:
        script = "chmod 000 f; . ./f; echo \"after:$?\"; chmod 644 f"
    modes = shell_BASH if shape in ("sourcepath-off", "source-word") else shell_ALL
    return "dot-source-" + shape, modes, shell_program(files, script + " 2>/dev/null", "echo \"end=$?\"")


def shell_lang_eval_exec(rng):
    shape = rng.choice(("eval-join", "eval-quotes", "eval-status", "eval-syntax", "eval-empty", "eval-multiline", "eval-nested",
                        "exec-redirect", "exec-command", "exec-missing", "exec-nonexec", "exec-in-subshell", "exec-a", "exec-l",
                        "exec-c", "exec-with-assign", "exec-builtin", "exec-function", "exec-empty", "exec-dir"))
    if shape == "eval-join":
        script = "eval echo a b; eval 'echo' 'c d'; v='echo $x'; x=X; eval \"$v\""
    elif shape == "eval-quotes":
        script = "eval 'echo \"a  b\"'; eval \"echo 'c  d'\"; eval echo \\\"e f\\\""
    elif shape == "eval-status":
        script = "eval false; echo $?; eval 'exit 3' ; echo never"
    elif shape == "eval-syntax":
        script = "eval 'echo \"unclosed'; echo \"after:$?\""
    elif shape == "eval-empty":
        script = "false; eval; echo $?; eval ''; echo $?"
    elif shape == "eval-multiline":
        script = "eval 'echo one\necho two'"
    elif shape == "eval-nested":
        script = "eval eval eval echo deep; eval 'eval \"echo \\$x\"'"
    elif shape == "exec-redirect":
        script = "exec > out; echo captured; exec >&2 2>/dev/null; cat out 2>/dev/null"
    elif shape == "exec-command":
        script = "exec echo replaced; echo never"
    elif shape == "exec-missing":
        script = "exec nosuchcommand_xyz; echo never"
    elif shape == "exec-nonexec":
        script = "printf 'echo x\\n' > ne; chmod 600 ne; exec ./ne; echo never"
    elif shape == "exec-in-subshell":
        script = "(exec echo sub); echo after"
    elif shape == "exec-a":
        script = "exec -a named /bin/sh -c 'echo \"$0\"'"
    elif shape == "exec-l":
        script = "exec -l /bin/sh -c 'echo \"$0\"' | sed 's/^-//'"
    elif shape == "exec-c":
        script = "x=1; export x; exec -c /usr/bin/env | wc -l"
    elif shape == "exec-with-assign":
        script = "v=1 exec /bin/sh -c 'echo \"${v-unset}\"'"
    elif shape == "exec-builtin":
        script = "exec echo builtin-word; echo never"
    elif shape == "exec-function":
        script = "f() { echo fn; }; exec f; echo after"
    elif shape == "exec-empty":
        script = "exec; echo \"$?\"; exec ''; echo \"$?\""
    else:
        script = "exec /tmp; echo never"
    modes = shell_BASH if shape in ("exec-a", "exec-l", "exec-c") else shell_ALL
    return "eval-exec-" + shape, modes, shell_program("x=X", script + " 2>/dev/null", "echo \"end=$?\"")


def shell_lang_exit_forms(rng):
    operand = rng.choice(("", "0", "1", "3", "255", "256", "257", "300", "-1", "+7", " 3 ", "9223372036854775807", "9223372036854775808",
                          "-9223372036854775808", "bad", "1 2", "bad 2", "--", "-- 7", "0x10", "010", "1.5", "''", "\"\""))
    where = rng.choice(("top", "subshell", "function", "subst", "pipeline", "trap", "group", "and-or", "after-false", "command"))
    if where == "top":
        script = "exit " + operand + "; echo never"
    elif where == "subshell":
        script = "(exit " + operand + "); echo \"$?\""
    elif where == "function":
        script = "f() { exit " + operand + "; }; f; echo never"
    elif where == "subst":
        script = "v=$(exit " + operand + "); echo \"$?\""
    elif where == "pipeline":
        script = "exit " + operand + " | cat; echo \"$?\""
    elif where == "trap":
        script = "trap 'echo \"trap:$?\"' EXIT; exit " + operand
    elif where == "group":
        script = "{ exit " + operand + "; }; echo never"
    elif where == "and-or":
        script = "false || exit " + operand + "; echo never"
    elif where == "after-false":
        script = "false; exit " + operand
    else:
        script = "command exit " + operand + "; echo \"after:$?\""
    return "exit-forms", shell_ALL, script + " 2>/dev/null\necho \"end=$?\"\n"


def shell_lang_select_time(rng):
    shape = rng.choice(("select-basic", "select-blank", "select-invalid", "select-eof", "select-ps3", "select-positional", "select-empty",
                        "select-continue", "select-layout", "time-format", "time-p", "time-status", "time-pipeline", "time-negate",
                        "time-background", "time-word", "time-empty-format", "time-nested"))
    if shape == "select-basic":
        script = "select v in a b 'c d'; do echo \"<$v:$REPLY>\"; break; done <<EOF\n%s\nEOF" % rng.choice(("1", "2", "3"))
    elif shape == "select-blank":
        script = "select v in a b; do echo \"<$v>\"; break; done <<EOF\n\n2\nEOF"
    elif shape == "select-invalid":
        script = "select v in a b; do echo \"<${v:-none}:$REPLY>\"; break; done <<EOF\n%s\nEOF" % rng.choice(("9", "q", " +2 ", "0", "-1"))
    elif shape == "select-eof":
        script = "select v in a; do echo never; done < /dev/null; echo \"$?\""
    elif shape == "select-ps3":
        script = "PS3='pick> '; select v in a; do break; done <<EOF\n1\nEOF"
    elif shape == "select-positional":
        script = "set -- x y; select v; do echo \"<$v>\"; break; done <<EOF\n2\nEOF"
    elif shape == "select-empty":
        script = "select v in; do echo never; done; echo \"$?\"; select v in $empty; do echo never; done; echo \"$?\""
    elif shape == "select-continue":
        script = "select v in a b; do echo \"<$v>\"; [ \"$v\" = b ] && break; continue; done <<EOF\n1\n2\nEOF"
    elif shape == "select-layout":
        items = " ".join("item%d" % k for k in range(rng.choice((3, 6, 10, 30))))
        script = "COLUMNS=%d; select v in %s; do break; done <<EOF\n1\nEOF" % (rng.choice((20, 80)), items)
    elif shape == "time-format":
        fmt = rng.choice(("%0R", "[%0R][%0U][%0S]", "%%", "end%", "x", "%0lR", "%2P", "a%%b", "%0R %0U %0S"))
        script = "TIMEFORMAT=" + shell_quote(fmt) + "; { time :; } 2>&1"
    elif shape == "time-p":
        script = "{ time -p true; } 2>&1 | sed 's/[0-9.]*$/N/'"
    elif shape == "time-status":
        script = "TIMEFORMAT=%0R; { time false; } 2>&1; echo \"$?\"; { time (exit 3); } 2>&1; echo \"$?\""
    elif shape == "time-pipeline":
        script = "TIMEFORMAT=%0R; { time echo a | cat; } 2>&1"
    elif shape == "time-negate":
        script = "TIMEFORMAT=%0R; { ! time false; } 2>&1; echo \"$?\"; { time ! false; } 2>&1; echo \"$?\""
    elif shape == "time-background":
        script = "TIMEFORMAT=%0R; { time :& } 2>&1; wait"
    elif shape == "time-word":
        script = "echo time; time=5; echo $time; for time in a; do echo $time; done"
    elif shape == "time-empty-format":
        script = "TIMEFORMAT=; { time :; } 2>&1 | wc -l"
    else:
        script = "TIMEFORMAT=%0R; { time time :; } 2>&1"
    return "select-time", shell_BASH, shell_program("empty=", script + " 2>/dev/null", "echo \"end=$?\"")


def shell_lang_lastpipe_pipestatus(rng):
    shape = rng.choice(("builtin-final", "function-final", "group-pipefail", "bang", "monitor-disables", "background-disables",
                        "descriptor-restore", "break-reaches", "errexit-final", "tested-final", "err-trap-final", "exit-trap",
                        "closed-stdin", "ordinary", "external-final", "initial", "simple-updates", "reset-by-assign", "collapse",
                        "unset", "readonly-absent", "readonly-existing", "readonly-pipeline", "declare-p", "redirect-fail",
                        "final-redirect", "left-redirect", "lastpipe-redirect", "lastpipe-readonly", "three-stages", "negated"))
    pre = "set +m; shopt -s lastpipe; "
    if shape == "builtin-final":
        script = pre + "value=old; printf 'new\\n' | read value; printf '%s:%s:%s\\n' \"$value\" \"$?\" \"${PIPESTATUS[*]}\""
    elif shape == "function-final":
        script = pre + "f() { read value; mark=function; return 7; }; printf z | f; printf '%s:%s:%s:%s\\n' \"$value\" \"$mark\" \"$?\" \"${PIPESTATUS[*]}\""
    elif shape == "group-pipefail":
        script = pre + "set -o pipefail; false | { mark=group; true; }; printf '%s:%s:%s\\n' \"$mark\" \"$?\" \"${PIPESTATUS[*]}\""
    elif shape == "bang":
        script = pre + "! false | { mark=yes; false; }; printf '%s:%s:%s\\n' \"$mark\" \"$?\" \"${PIPESTATUS[*]}\""
    elif shape == "monitor-disables":
        script = "set -m; shopt -s lastpipe; value=old; printf 'new\\n' | read value; set +m; echo \"$value\""
    elif shape == "background-disables":
        script = pre + "value=old; printf 'new\\n' | read value & wait; echo \"$value\""
    elif shape == "descriptor-restore":
        script = pre + "printf x | { read x; echo \"$x\"; } > p; read y <<EOF\nstdin\nEOF\ncat p; echo \"$y\""
    elif shape == "break-reaches":
        script = pre + "for i in 1 2; do printf x | { read x; break; }; echo bad; done; echo break-ok"
    elif shape == "errexit-final":
        script = pre + "set -e; true | { false; echo forbidden; }; echo after"
    elif shape == "tested-final":
        script = pre + "set -e; true | { false; echo allowed; } || echo caught; echo after"
    elif shape == "err-trap-final":
        script = pre + "trap 'echo ERR:$?' ERR; true | { false; echo after-false; }; echo done"
    elif shape == "exit-trap":
        script = pre + "trap 'printf \"EXIT:%s:%s\\n\" \"$?\" \"${PIPESTATUS[*]}\"' EXIT; set -e; true | false"
    elif shape == "closed-stdin":
        script = pre + "exec 0<&-; printf x | read value; read after 2>/dev/null; if [ \"$?\" -ne 0 ]; then echo \"closed:$value\"; fi"
    elif shape == "ordinary":
        script = "false | true; printf '%s:%s:%s\\n' \"$?\" \"${PIPESTATUS[0]}\" \"${PIPESTATUS[1]}\""
    elif shape == "external-final":
        script = pre + "/bin/false | /bin/true; printf '%s:%s\\n' \"$?\" \"${PIPESTATUS[*]}\""
    elif shape == "initial":
        script = "printf '%s:%s:%s\\n' \"$PIPESTATUS\" \"${PIPESTATUS[*]}\" \"${#PIPESTATUS[@]}\""
    elif shape == "simple-updates":
        script = "false; printf '%s:%s:%s\\n' \"$?\" \"${PIPESTATUS[*]}\" \"${#PIPESTATUS[@]}\""
    elif shape == "reset-by-assign":
        script = "set -o pipefail; (exit 3) | (exit 7) | true; s=$?; printf '%s:%s:%s:%s\\n' \"$s\" \"${PIPESTATUS[0]}\" \"${PIPESTATUS[1]}\" \"${PIPESTATUS[2]}\""
    elif shape == "collapse":
        script = "false | true; :; printf '%s:%s:%s\\n' \"${!PIPESTATUS[*]}\" \"${PIPESTATUS[*]}\" \"${#PIPESTATUS[@]}\""
    elif shape == "unset":
        script = "unset PIPESTATUS; printf '%s:%s\\n' \"${PIPESTATUS[*]}\" \"${#PIPESTATUS[@]}\""
    elif shape == "readonly-absent":
        script = "readonly PIPESTATUS; false; printf '%s:%s\\n' \"${PIPESTATUS[*]}\" \"${#PIPESTATUS[@]}\""
    elif shape == "readonly-existing":
        script = "false | true; readonly PIPESTATUS; false; printf '%s:%s:%s\\n' \"${!PIPESTATUS[*]}\" \"${PIPESTATUS[*]}\" \"${#PIPESTATUS[@]}\""
    elif shape == "readonly-pipeline":
        script = "false; readonly PIPESTATUS; (exit 3) | (exit 7); printf '%s:%s\\n' \"${!PIPESTATUS[*]}\" \"${PIPESTATUS[*]}\""
    elif shape == "declare-p":
        script = "false | true; saved=$?; declare -p PIPESTATUS"
    elif shape == "redirect-fail":
        script = ": >/no/such/target 2>/dev/null; printf '%s\\n' \"$?\""
    elif shape == "final-redirect":
        script = "set -o pipefail; true | cat < /no/such/input 2>/dev/null; printf '%s:%s:%s\\n' \"$?\" \"${PIPESTATUS[0]}\" \"${PIPESTATUS[1]}\""
    elif shape == "left-redirect":
        script = "set -o pipefail; cat < /no/such/input 2>/dev/null | true; printf '%s:%s:%s\\n' \"$?\" \"${PIPESTATUS[0]}\" \"${PIPESTATUS[1]}\""
    elif shape == "lastpipe-redirect":
        script = pre + "value=old; true | read value < /no/such/input 2>/dev/null; printf '%s:%s:%s\\n' \"$?\" \"$value\" \"${PIPESTATUS[*]}\""
    elif shape == "lastpipe-readonly":
        script = pre + "readonly value=old; (trap '' PIPE; printf new 2>/dev/null; :) | read value 2>/dev/null; printf '%s:%s:%s\\n' \"$?\" \"$value\" \"${PIPESTATUS[*]}\""
    elif shape == "three-stages":
        script = "(exit %d) | (exit %d) | (exit %d); printf '%%s:%%s\\n' \"$?\" \"${PIPESTATUS[*]}\"" % (rng.randrange(8), rng.randrange(8), rng.randrange(8))
    else:
        script = "! (exit 3) | (exit 0); printf '%s:%s\\n' \"$?\" \"${PIPESTATUS[*]}\"; set -o pipefail; ! false | true; echo \"$?\""
    return "lastpipe-pipestatus", shell_BASH, shell_program(script, "echo \"end=$?\"")


def shell_lang_directory_policy(rng):
    shape = rng.choice(("physical-letter", "physical-named", "onecmd-state", "logical-symlink", "logical-dotdot", "physical-symlink",
                        "physical-dotdot", "explicit-P", "explicit-L", "pwd-options", "cdpath-logical", "cdpath-physical", "cd-dash",
                        "readonly-oldpwd", "missing", "home-absent", "oldpwd-absent", "empty-operand", "extra-operand", "pwd-bad-option",
                        "symlink-loop", "overlong", "deleted-cwd", "deleted-cwd-e", "pwd-env", "cd-to-file", "cd-no-permission", "scan-refill"))
    tree = "mkdir -p real/child search; ln -sfn real/child link; ln -sfn ../real/child search/place; ln -sfn loop loop"
    if shape == "physical-letter":
        script = "set -P; case $- in *P*) echo on;; *) echo off;; esac; set +P; case $- in *P*) echo bad;; *) echo off;; esac"
    elif shape == "physical-named":
        script = "set -o physical; case $- in *P*) echo on;; esac; set +o physical; case $- in *P*) echo bad;; *) echo off;; esac"
    elif shape == "onecmd-state":
        script = "set -o onecmd; case $- in *t*) echo on;; esac; set +t; case $- in *t*) echo bad;; *) echo off;; esac"
    elif shape == "logical-symlink":
        script = "cd link; printf '%s:%s:%s\\n' \"${PWD#$ROOT}\" \"$(pwd | sed \"s#$ROOT##\")\" \"$(pwd -P | sed \"s#$ROOT##\")\""
    elif shape == "logical-dotdot":
        script = "cd link; cd ..; printf '%s:%s\\n' \"${PWD#$ROOT}\" \"$(pwd -P | sed \"s#$ROOT##\")\""
    elif shape == "physical-symlink":
        script = "set -P; cd link; printf '%s:%s\\n' \"${PWD#$ROOT}\" \"$(pwd | sed \"s#$ROOT##\")\""
    elif shape == "physical-dotdot":
        script = "set -P; cd link/..; printf '%s:%s\\n' \"${PWD#$ROOT}\" \"$(pwd -P | sed \"s#$ROOT##\")\""
    elif shape == "explicit-P":
        script = "set +P; cd -P link; printf '%s:%s\\n' \"${PWD#$ROOT}\" \"$(pwd -L | sed \"s#$ROOT##\")\""
    elif shape == "explicit-L":
        script = "set -P; cd -L link; printf '%s:%s:%s\\n' \"${PWD#$ROOT}\" \"$(pwd -L | sed \"s#$ROOT##\")\" \"$(pwd -P | sed \"s#$ROOT##\")\""
    elif shape == "pwd-options":
        script = "cd -L link; printf '%s:%s\\n' \"$(pwd -LP | sed \"s#$ROOT##\")\" \"$(pwd -PL | sed \"s#$ROOT##\")\"; set -P; printf '%s\\n' \"$(pwd | sed \"s#$ROOT##\")\""
    elif shape == "cdpath-logical":
        script = "CDPATH=$ROOT/search; cd place; printf 'PWD=%s\\n' \"${PWD#$ROOT}\""
    elif shape == "cdpath-physical":
        script = "set -P; CDPATH=$ROOT/search; cd place; printf 'PWD=%s\\n' \"${PWD#$ROOT}\""
    elif shape == "cd-dash":
        script = "cd real; cd - | sed \"s#$ROOT##\"; echo \"${OLDPWD#$ROOT}\""
    elif shape == "readonly-oldpwd":
        script = "readonly OLDPWD; cd real 2>/dev/null; printf '%s:%s:%s\\n' \"$?\" \"${PWD#$ROOT}\" \"${OLDPWD#$ROOT}\""
    elif shape == "missing":
        script = "cd \"$ROOT/missing\" 2>/dev/null; echo $?"
    elif shape == "home-absent":
        script = "unset HOME; cd 2>/dev/null; echo $?; HOME=; cd 2>/dev/null; echo $?"
    elif shape == "oldpwd-absent":
        script = "unset OLDPWD; cd - 2>/dev/null; echo $?"
    elif shape == "empty-operand":
        script = "cd '' 2>/dev/null; echo $?; pwd | sed \"s#$ROOT##\""
    elif shape == "extra-operand":
        script = "cd \"$ROOT\" \"$ROOT/real\" 2>/dev/null; echo $?"
    elif shape == "pwd-bad-option":
        script = "pwd -x >/dev/null 2>&1; echo $?; cd -x 2>/dev/null; echo $?"
    elif shape == "symlink-loop":
        script = "cd \"$ROOT/loop\" 2>/dev/null; echo $?"
    elif shape == "overlong":
        script = "name=$(awk 'BEGIN { for (i = 0; i < 5000; i++) printf \"a\" }'); cd \"$name\" 2>/dev/null; echo $?"
    elif shape == "deleted-cwd":
        script = "d=$ROOT/gone; mkdir \"$d\"; cd \"$d\"; rmdir \"$d\"; cd -P . 2>/dev/null; printf '%s:%s\\n' \"$?\" \"${PWD#$ROOT}\""
    elif shape == "deleted-cwd-e":
        script = "d=$ROOT/gone; mkdir \"$d\"; cd \"$d\"; rmdir \"$d\"; cd -Pe . 2>/dev/null; printf '%s:%s\\n' \"$?\" \"${PWD#$ROOT}\""
    elif shape == "pwd-env":
        script = "PWD=/nonexistent; pwd | sed \"s#$ROOT##\"; cd real; echo \"${PWD#$ROOT}\""
    elif shape == "cd-to-file":
        script = ": > file; cd file 2>/dev/null; echo $?"
    elif shape == "cd-no-permission":
        script = "mkdir locked; chmod 000 locked; cd locked 2>/dev/null; echo $?; chmod 755 locked"
    else:
        script = ("mkdir scan; cd scan; i=0; while [ $i -lt 160 ]; do : > mw_file$i; mkdir mw_dir$i; : > mw_dir$i/inside; i=$((i+1)); done; "
                  "set -- mw_*; printf '%s:%s\\n' \"$#\" \"$1\"; set -- mw_dir*/inside; printf '%s:%s\\n' \"$#\" \"$1\"")
    bash_only = shape in ("physical-letter", "physical-named", "onecmd-state", "physical-symlink", "physical-dotdot", "explicit-L",
                          "cdpath-physical", "deleted-cwd-e", "pwd-options")
    return "directory-policy-" + shape, shell_BASH if bash_only else shell_ALL, shell_program(
        tree, "ROOT=$PWD", script, "echo \"end=$?\"")


def shell_lang_special_builtin_fatality(rng):
    builtin = rng.choice(("export 1bad=x", "readonly 1bad=x", "export bad-name=x", "unset -Z", "set -Z", "set -o BAD", "shift bad", "shift 2",
                          "shift -1", ": > /no/such/dir/target", "exec -Z", "trap BAD BAD", "eval false", ". /no/such/file", "break bad",
                          "continue bad", "return bad", "exit bad", "times x", "unset", "readonly x=old; x=new", "readonly x=old; x=new true",
                          "readonly x=old; x=new :", "readonly i=old; for i in x y; do echo BODY; done", "export", "readonly",
                          "unset -f -v x", "set -e -Z", "trap", "eval 'echo \"unclosed'", "command"))
    wrapper = rng.choice(("direct", "command", "function", "subshell", "group", "same-line", "next-line", "eval"))
    posix = rng.choice(("", "set -o posix"))
    if wrapper == "direct":
        body = builtin + "; echo AFTER"
    elif wrapper == "command":
        body = "command " + builtin + "; echo \"after:$?\""
    elif wrapper == "function":
        body = "f() { " + builtin + "; echo INNER; }; f; echo \"after:$?\""
    elif wrapper == "subshell":
        body = "( " + builtin + "; echo INNER ); echo \"after:$?\""
    elif wrapper == "group":
        body = "{ " + builtin + "; echo INNER; }; echo \"after:$?\""
    elif wrapper == "same-line":
        body = "f() { " + builtin + "; echo INNER; }; f; echo SAME"
    elif wrapper == "next-line":
        body = "f() {\n" + builtin + "; echo INNER\n}\nf\necho \"outer:$?\""
    else:
        body = "eval " + shell_quote(builtin + "; echo INNER") + "; echo \"after:$?\""
    modes = shell_BASH if posix or "times" in builtin else shell_ALL
    return "special-builtin-fatality", modes, shell_program(posix, "echo start", body + " 2>/dev/null", "echo \"end=$?\"")


def shell_lang_assignment_words(rng):
    shape = rng.choice(("plain-not-exported", "prefix-temporary", "prefix-special", "prefix-function", "prefix-command", "left-to-right",
                        "assignment-only-status", "not-assignment", "after-command", "dollar-name", "array-word", "append", "keyword-k",
                        "tilde-in-assign", "no-split", "no-glob", "declaration-command", "empty-value", "many-prefix", "subst-status",
                        "readonly-prefix", "export-prefix", "exported-restored", "local-prefix", "env-view"))
    if shape == "plain-not-exported":
        script = "x=1; /bin/sh -c 'echo \"${x-unset}\"'"
    elif shape == "prefix-temporary":
        script = "x=old; x=new true; echo \"$x\"; x=new /bin/sh -c 'echo \"$x\"'; echo \"$x\""
    elif shape == "prefix-special":
        script = "x=old; x=new :; echo \"$x\"; y=old; y=new export z=1; echo \"$y\"; w=old; w=new eval :; echo \"$w\""
    elif shape == "prefix-function":
        script = "f() { echo \"in:$x\"; x=body; }; x=old; x=new f; echo \"out:$x\""
    elif shape == "prefix-command":
        script = "x=old; x=new command :; echo \"$x\"; x=new command export y=1; echo \"$x\""
    elif shape == "left-to-right":
        script = "a=one b=$a c=${b}2; echo \"$a $b $c\"; d=1 e=$d /bin/sh -c 'echo \"$d $e\"'"
    elif shape == "assignment-only-status":
        script = "false; x=1; echo $?; x=$(false); echo $?; x=$(true) y=$(exit 3); echo $?"
    elif shape == "not-assignment":
        script = "echo a=b; 'x=1' 2>/dev/null; echo \"$?\"; \\x=1 2>/dev/null; echo \"$?\"; echo \"${x-unset}\""
    elif shape == "after-command":
        script = "echo x=1 y=2; echo \"${x-unset}\""
    elif shape == "dollar-name":
        script = "n=x; $n=1 2>/dev/null; echo \"$?:${x-unset}\"; eval \"$n=2\"; echo \"$x\""
    elif shape == "array-word":
        script = "a[0]=1; echo \"${a[0]}\"; a[1+1]=2; echo \"${a[2]}\"; 'a[0]=3' 2>/dev/null; echo \"$?\""
    elif shape == "append":
        script = "x=a; x+=b; echo \"$x\"; unset y; y+=c; echo \"$y\"; z+=; echo \"[$z]\""
    elif shape == "keyword-k":
        script = "set -k; x=old; echo a x=new b; echo \"$x\"; /bin/sh -c 'echo \"${y-unset}\"' q y=set"
    elif shape == "tilde-in-assign":
        script = "HOME=/hh; x=~; y=~/p:~; z=a:~; w=\"~\"; echo \"$x $y $z $w\""
    elif shape == "no-split":
        script = "v='a  b *'; x=$v; y=$v$v; printf '<%s>\\n' \"$x\" \"$y\""
    elif shape == "no-glob":
        script = ": > a.txt; x=*.txt; echo \"$x\"; echo $x"
    elif shape == "declaration-command":
        script = "v='g h'; export D=$v; command export E=$v; command export P=*; echo \"$D|$E|$P\"; f() { command local L=$v; echo \"[$L]\"; }; f"
    elif shape == "empty-value":
        script = "x=; echo \"[$x]\"; x= ; echo \"[${x+set}]\"; y= true; echo \"[${y-unset}]\""
    elif shape == "many-prefix":
        script = " ".join("A%02d=%d" % (k, k) for k in range(20)) + " /bin/sh -c 'echo \"$A00 $A19\"'; echo \"${A00-unset}\""
    elif shape == "subst-status":
        script = "true; a=$(exit 3) b=$?; echo \"$b:$?\""
    elif shape == "readonly-prefix":
        script = "readonly r=old; r=new true 2>/dev/null; echo \"$?:$r\"; r=new /bin/sh -c 'echo \"$r\"' 2>/dev/null; echo \"$?\""
    elif shape == "export-prefix":
        script = "x=1 export y=2; echo \"${x-unset}:$y\"; /bin/sh -c 'echo \"${x-unset}:${y-unset}\"'"
    elif shape == "exported-restored":
        script = "export x=old; x=new /bin/sh -c 'echo \"$x\"'; /bin/sh -c 'echo \"$x\"'; f() { x=in; }; x=pre f; echo \"$x\"; /bin/sh -c 'echo \"$x\"'"
    elif shape == "local-prefix":
        script = "f() { local x=in; x=pre g; echo \"f:$x\"; }; g() { echo \"g:$x\"; }; x=out; f; echo \"out:$x\""
    else:
        script = "x=1; env | grep -c '^x='; export x; env | grep -c '^x='; unset x; env | grep -c '^x='; x=2 env | grep -c '^x='"
    modes = shell_BASH if shape in ("array-word", "append", "keyword-k") else shell_ALL
    return "assignment-words-" + shape, modes, shell_program(script + " 2>/dev/null", "echo \"end=$?\"")


def shell_lang_job_control_script(rng):
    shape = rng.choice(("jobs-running", "jobs-markers", "jobs-done-once", "waited-forgotten", "jobs-p", "jobs-l", "jobs-r-s", "jobs-n",
                        "pipeline-one-job", "stopped", "killed", "exit-status-line", "bg-announce", "fg-status", "fg-spec", "fg-ambiguous",
                        "disown", "disown-h", "disown-a", "kill-spec", "kill-pipeline", "wait-f-stopped", "wait-n",
                        "pipestatus-monitored", "subshell-jobs", "without-monitor", "job-number-reuse", "jobs-after-exit"))
    pre = "set -m; "
    if shape == "jobs-running":
        script = pre + "sleep 0.3 & jobs; wait"
    elif shape == "jobs-markers":
        script = pre + "sleep 0.3 & sleep 0.3 & jobs; wait"
    elif shape == "jobs-done-once":
        script = pre + "sleep 0.05 & sleep 0.2; jobs; jobs; wait"
    elif shape == "waited-forgotten":
        script = pre + "sleep 0.05 & wait $!; jobs; echo \"[$?]\""
    elif shape == "jobs-p":
        script = pre + "sleep 0.3 & p=$!; [ \"$(jobs -p)\" = \"$p\" ] && echo same; wait"
    elif shape == "jobs-l":
        script = pre + "sleep 0.3 & p=$!; jobs -l | sed \"s/$p/PID/\"; wait"
    elif shape == "jobs-r-s":
        script = pre + "sleep 0.3 & sleep 0.3 & kill -STOP %1; sleep 0.05; jobs -r; jobs -s; kill -CONT %1; wait"
    elif shape == "jobs-n":
        script = pre + "sleep 0.3 & jobs -n; jobs -n; wait"
    elif shape == "pipeline-one-job":
        script = pre + "sleep 0.3 | cat & jobs; wait"
    elif shape == "stopped":
        script = pre + "sleep 2 & kill -STOP %1; sleep 0.05; jobs; kill -KILL %1; wait %1; echo \"$?\""
    elif shape == "killed":
        script = pre + "sleep 5 & kill -KILL %1; sleep 0.05; jobs; jobs; wait"
    elif shape == "exit-status-line":
        script = pre + "(exit 7) & sleep 0.1; jobs; wait"
    elif shape == "bg-announce":
        script = pre + "sleep 5 & kill -STOP %1; sleep 0.05; bg; sleep 0.05; jobs; kill %1; wait"
    elif shape == "fg-status":
        script = pre + "sleep 0.1 & fg %1; echo \"st=$?\""
    elif shape == "fg-spec":
        script = pre + "sleep 0.1 & fg " + rng.choice(("%1", "%sleep", "%?eep", "%%", "%+", "%-", "")) + "; echo \"st=$?\""
    elif shape == "fg-ambiguous":
        script = pre + "sleep 0.2 & sleep 0.2 & fg %sl 2>/dev/null; echo \"$?\"; wait"
    elif shape == "disown":
        script = pre + "sleep 0.2 & disown; jobs | wc -l; wait; echo \"$?\""
    elif shape == "disown-h":
        script = pre + "sleep 0.2 & disown -h %1; jobs | wc -l; wait"
    elif shape == "disown-a":
        script = pre + "sleep 0.2 & sleep 0.2 & disown -a; jobs | wc -l"
    elif shape == "kill-spec":
        script = pre + "sleep 2 & kill " + rng.choice(("%1", "-s TERM %1", "-TERM %1", "-15 %1", "%sleep", "%%", "%9")) + " 2>/dev/null; echo \"$?\"; kill -TERM %1 2>/dev/null; wait %1 2>/dev/null; echo \"$?\""
    elif shape == "kill-pipeline":
        script = pre + "sleep 5 | sleep 5 & kill %1; wait; echo \"$?\"; jobs"
    elif shape == "wait-stopped":
        script = pre + "sleep 5 & kill -TSTP %1; wait %1; echo \"$?\"; kill -KILL %1"
    elif shape == "wait-f-stopped":
        script = pre + "sleep 0.3 & p=$!; kill -STOP $p; (sleep 0.1; kill -CONT $p) & wait -f $p; echo \"$?\""
    elif shape == "wait-n":
        script = pre + "(exit 3) & wait -n; echo \"$?\"; wait -n -p named 2>/dev/null; echo \"$?\""
    elif shape == "suspend":
        script = pre + "suspend 2>/dev/null; echo \"$?\""
    elif shape == "pipestatus-monitored":
        script = pre + "false | true; echo \"${PIPESTATUS[*]}\"; echo a | cat"
    elif shape == "subshell-jobs":
        script = pre + "sleep 0.3 & (jobs | wc -l); jobs | wc -l; wait"
    elif shape == "without-monitor":
        script = "sleep 0.1 & jobs; fg 2>/dev/null; echo \"fg=$?\"; bg 2>/dev/null; echo \"bg=$?\"; wait"
    elif shape == "job-number-reuse":
        script = pre + "sleep 0.3 & (exit 7) & sleep 0.1; jobs; sleep 0.3 | cat & jobs; kill -TERM %2; wait; jobs"
    else:
        script = pre + "sleep 0.3 & exit 0"
    # Listings follow bash's layout under every name; against dash that is
    # the pinned jobs-layout policy, so only status shapes run there.
    posix_shapes = ("fg-status", "fg-spec", "fg-ambiguous", "kill-spec", "subshell-jobs", "job-number-reuse", "jobs-after-exit")
    return "job-control-script-" + shape, shell_ALL if shape in posix_shapes else shell_BASH, shell_program(
        script + " 2>/dev/null", "echo \"end=$?\"")


def shell_lang_utf8_locale(rng):
    locale = rng.choice(("LC_ALL=C.UTF-8", "LC_ALL=C; LC_CTYPE=C.UTF-8; LANG=C.UTF-8", "LC_ALL=; LC_CTYPE=C.UTF-8; LANG=C",
                         "LC_ALL=; LC_CTYPE=; LANG=C.UTF-8", "LC_ALL=C"))
    value = rng.choice(("éΩ界🌙é", "01234567éΩ界🌙z", "é", "aé界z", "e\u0301", "👩\u200d💻"))
    shape = rng.choice(("length", "slice", "pattern", "case", "replace", "glob", "local-restore", "prefix-restore", "printf-width", "read-n"))
    if shape == "length":
        script = "x=" + shell_quote(value) + "; set -- \"$x\"; printf '%s:%s\\n' \"${#x}\" \"${#1}\""
    elif shape == "slice":
        script = "x=" + shell_quote(value) + "; printf '<%s>' \"${x:1:2}\" \"${x: -2:1}\" \"${x:1:-1}\" \"${x:99:1}\"; echo"
    elif shape == "pattern":
        script = "x=" + shell_quote(value) + "; for p in '?' '??' '*?' '?*' '*界?'; do printf '<%s>' \"${x#$p}\" \"${x##$p}\" \"${x%$p}\" \"${x%%$p}\"; done; echo"
    elif shape == "case":
        script = "x=" + shell_quote(value) + "; printf '<%s>' \"${x^^}\" \"${x,,}\" \"${x^}\"; echo"
    elif shape == "replace":
        script = "x=" + shell_quote(value) + "; printf '<%s>' \"${x//[!é]/X}\" \"${x/#[!é]/X}\" \"${x/%[!é]/X}\" \"${x//?/X}\"; echo"
    elif shape == "glob":
        script = ": > u_é; : > u_Ω; : > u_界x; set -- u_? u_??; printf '<%s>' \"$@\"; echo"
    elif shape == "local-restore":
        script = "x=é; f() { local LC_ALL=C.UTF-8; echo \"${#x}\"; }; f; echo \"${#x}\""
    elif shape == "prefix-restore":
        script = "x=é; f() { echo \"${#x}\"; }; LC_ALL=C.UTF-8 f; echo \"${#x}\""
    elif shape == "printf-width":
        script = "printf '[%5s][%-5s]\\n' é é | od -c | head -2"
    else:
        script = "printf 'éΩ界' | { read -n 2 v; echo \"${#v}\"; }"
    return "utf8-locale", shell_BASH, shell_program(locale, script + " 2>/dev/null", "echo \"end=$?\"")


def shell_lang_onecmd_input(rng):
    """set -t and its startup form against one- and many-line inputs."""
    text = rng.choice(("echo one; echo two\necho three\n", "# first line\necho forbidden\n", "\necho forbidden\n",
                       "if true; then\necho one\nfi\necho forbidden\n", "cat <<EOF\none\nEOF\necho forbidden\n",
                       "false\necho forbidden\n", "set +t; echo one\necho two\n", "set -t; echo one\necho forbidden\n",
                       "echo one &&\necho two\necho three\n", "f() {\necho in\n}\nf\necho forbidden\n"))
    form = rng.choice(("stdin", "file", "command", "runtime"))
    if form == "stdin":
        launch = 'printf %s ' + shell_quote(text) + ' | "./$shell_me" -t'
    elif form == "file":
        launch = 'printf %s ' + shell_quote(text) + ' > one.sh; "./$shell_me" -t one.sh'
    elif form == "command":
        launch = '"./$shell_me" -tc ' + shell_quote(text)
    else:
        launch = '"./$shell_me" -c ' + shell_quote("set -t\n" + text)
    return "onecmd-input", shell_ALL, shell_SELF + launch + " 2>/dev/null\necho \"status=$?\"\nrm -f \"./$shell_me\"\n"


UTILITIES = (
    shell_STARTUP,
    shell_SET,
    shell_NAMES_BASH,
    shell_NAMES_POSIX,
    shell_ENVIRONMENT,
    shell_PRIVILEGED,
    shell_TERMINAL_JOBS,
    shell_TERMINAL_SESSION,
    shell_TERMINAL_STARTUP,
    shell_TERMINAL_VANISH,
    shell_POLICY_DASH,
    shell_POLICY_BASH,
)

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
    shell_lang_dollar_single,
    shell_lang_locale_quote,
    shell_lang_parameter_operators,
    shell_lang_parameter_length_trim,
    shell_lang_transforms,
    shell_lang_indirection,
    shell_lang_arithmetic_grammar,
    shell_lang_arithmetic_errors,
    shell_lang_ifs_splitting,
    shell_lang_pathname_expansion,
    shell_lang_brace_expansion,
    shell_lang_tilde,
    shell_lang_command_substitution,
    shell_lang_process_substitution,
    shell_lang_coproc,
    shell_lang_heredoc,
    shell_lang_here_string,
    shell_lang_redirections,
    shell_lang_redirection_persistence,
    shell_lang_pipelines,
    shell_lang_lists,
    shell_lang_compound_commands,
    shell_lang_double_bracket,
    shell_lang_functions,
    shell_lang_traps,
    shell_lang_subshells,
    shell_lang_background_wait,
    shell_lang_errexit_contexts,
    shell_lang_nounset_forms,
    shell_lang_noclobber,
    shell_lang_allexport_noglob,
    shell_lang_xtrace_shape,
    shell_lang_verbose,
    shell_lang_special_parameters,
    shell_lang_aliases,
    shell_lang_syntax_errors,
    shell_lang_reserved_words,
    shell_lang_reader_boundaries,
    shell_lang_dot_source,
    shell_lang_eval_exec,
    shell_lang_exit_forms,
    shell_lang_select_time,
    shell_lang_lastpipe_pipestatus,
    shell_lang_directory_policy,
    shell_lang_special_builtin_fatality,
    shell_lang_assignment_words,
    shell_lang_job_control_script,
    shell_lang_utf8_locale,
    shell_lang_onecmd_input,
)
