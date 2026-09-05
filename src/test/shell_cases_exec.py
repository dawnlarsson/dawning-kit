"""Procedural execution cases for shell_generated.py.

The shared runner owns isolation, limits, reference selection and shrinking.
This module varies execution shape only: the same policy is reached through
different nesting, status values, wrappers and scopes instead of replaying a
fixed conformance list.
"""


VALUE_LENGTHS = (1, 7, 31, 127, 511)


def _value(rng, prefix, byte):
    """Vary a storage boundary, not an otherwise irrelevant random label."""
    return prefix + byte * rng.choice(VALUE_LENGTHS)


def _special_prefix(rng):
    old = _value(rng, "old-", "o")
    new = _value(rng, "new-", "n")
    tail = rng.choice((":", "eval :", "export x"))
    return ("special-prefix", ("bash", "posix", "dash"),
            f"x={old}; x={new} {tail}; printf '%s:%s\\n' \"$x\" \"$?\"")


def _command_exception(rng):
    name = rng.choice(("1bad", "bad-name", "9"))
    action = rng.choice((f"export {name}=x", f"readonly {name}=x"))
    return ("special-command-exception", ("bash", "posix"),
            f"command {action}; s=$?; printf 'after:%s\\n' \"$s\"")


def _disabled_special(rng):
    if rng.randrange(2):
        script = ("enable -n :; x=old; x=new :; "
                  "printf 'x=%s s=%s\\n' \"$x\" \"$?\"")
    else:
        script = ("enable -n return; function return { echo FUNCTION; }; "
                  "return; echo AFTER")
    return ("disabled-special", ("bash", "posix"), script)


def _control_status(rng):
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


def _errexit_context(rng):
    code = rng.randrange(2, 10)
    shape = rng.choice((
        f"set -e; (exit {code}) && echo bad; echo after",
        f"set -e; if (exit {code}); then echo bad; fi; echo after",
        f"set -e; ! (exit {code}); echo after",
        f"set -e; (exit {code}) || echo caught; echo after",
    ))
    return ("errexit-tested-context", ("bash", "posix", "dash"), shape)


def _child_exit(rng):
    body = rng.choice((
        "(trap 'echo CHILD' EXIT; echo body)",
        "v=$(trap 'echo CHILD' EXIT; echo body); printf '<%s>\\n' \"$v\"",
        "cat <(trap 'echo CHILD' EXIT; echo body)",
    ))
    modes = ("bash", "posix") if "<(" in body else ("bash", "posix", "dash")
    return ("child-exit-trap", modes,
            f"trap 'echo PARENT' EXIT; {body}")


def _inherited_exit(rng):
    depth = rng.randrange(1, 4)
    body = "echo body"
    for _ in range(depth):
        # Spaces keep nested subshells distinct from Bash's (( arithmetic
        # command token.
        body = f"( {body} )"
    return ("inherited-exit-trap", ("bash", "posix", "dash"),
            f"trap 'echo PARENT' EXIT; {body}")


def _rhs_status(rng):
    first = rng.randrange(1, 8)
    last = rng.randrange(1, 8)
    initial = rng.choice(("true", "false"))
    return ("assignment-rhs-status", ("bash", "posix"),
            f"{initial}; a=$(exit {first}) b=$? c=$(exit {last}) d=$?; "
            "printf '%s:%s:%s\\n' \"$b\" \"$d\" \"$?\"")


def _function_scope(rng):
    # These sizes cross copy/storage boundaries while fixed byte patterns let
    # the runner collapse cases with the same execution shape.
    outer = _value(rng, "outer-", "o")
    prefix = _value(rng, "prefix-", "p")
    inner = _value(rng, "inner-", "i")
    return ("function-prefix-scope", ("bash", "posix", "dash"),
            "f() { printf 'in:%s\\n' \"$x\"; x=" + inner + "; }; "
            f"x={outer}; x={prefix} f; printf 'out:%s:%s\\n' \"$x\" \"$?\"")


def _redirect_cardinality(rng):
    pattern = rng.choice(("?.txt", "a.*", "missing.*"))
    operator = rng.choice((">", ">|"))
    # Redirect pathname expansion is a Bash policy; dash deliberately does
    # not share Bash's default noninteractive glob behavior.
    return ("redirect-cardinality", ("bash", "posix"),
            f"printf 'DATA' {operator} {pattern}; s=$?; "
            "printf 'status:%s\\n' \"$s\"")


def _pipeline_context(rng):
    left = rng.randrange(1, 8)
    right = rng.randrange(1, 8)
    shape = rng.choice((
        f"(exit {left}) | (exit {right}); printf 'p:%s\\n' \"$?\"",
        f"! (exit {left}) | (exit {right}); printf 'p:%s\\n' \"$?\"",
        f"(exit {left}) | true; printf 'p:%s\\n' \"$?\"",
    ))
    return ("pipeline-status", ("bash", "posix", "dash"), shape)


def _subshell_scope(rng):
    outer = _value(rng, "outer-", "o")
    inner = _value(rng, "inner-", "i")
    depth = rng.randrange(1, 4)
    body = f"x={inner}; printf 'in:%s\\n' \"$x\""
    for _ in range(depth):
        body = f"( {body} )"
    return ("subshell-scope", ("bash", "posix", "dash"),
            f"x={outer}; {body}; printf 'out:%s\\n' \"$x\"")


def _composed_status(rng):
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


def _deep_control(rng):
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


def _special_scope(rng):
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


def _descriptor_order(rng):
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


def _readonly_scope(rng):
    old = _value(rng, "old-", "o")
    new = _value(rng, "new-", "n")
    wrapped = rng.randrange(2)
    assign = f"x={new} :"
    if wrapped:
        assign = f"x={new} command :"
    return ("readonly-special-prefix", ("bash", "posix"),
            f"x={old}\nreadonly x\nf() {{\n{assign}\nprintf 'inner:%s:%s\\n' \"$x\" \"$?\"\n}}\n"
            "f\nprintf 'outer:%s:%s\\n' \"$x\" \"$?\"")


GENERATORS = (_special_prefix, _command_exception, _disabled_special,
              _control_status,
              _errexit_context, _child_exit, _inherited_exit, _rhs_status,
              _function_scope, _redirect_cardinality, _pipeline_context,
              _subshell_scope, _composed_status, _deep_control, _special_scope,
              _descriptor_order, _readonly_scope)


def cases(rng, budget):
    """Yield exactly budget balanced, seed-stable execution cases."""
    order = list(GENERATORS)
    rng.shuffle(order)
    for at in range(budget):
        yield order[at % len(order)](rng)
