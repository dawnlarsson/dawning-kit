"""Bounded lexer/parser variance for shell_generated.py.

The runner owns process isolation, fixtures, limits, comparison and shrinking.
This module only produces deterministic scripts which exercise byte boundaries
that hand state from the input reader to the lexer and parser.
"""

import shlex


ALL = ("bash", "posix", "dash")
TOKEN_LENGTHS = (1, 7, 15, 31, 63, 255, 1023)
DELIMITER_LENGTHS = (1, 7, 31, 127)


def _payload(rng, prefix="v"):
    """Stable bytes at a chosen lexer boundary length.

    Random hex labels made byte-distinct scripts exercise identical grammar
    and defeated the shared runner's exact-script deduplication. Length is a
    real scanner/storage dimension; spelling is intentionally fixed.
    """
    return prefix + "x" * rng.choice(TOKEN_LENGTHS)


def _quotes(rng):
    payload = _payload(rng)
    split = max(1, len(payload) // 2)
    pieces = [payload[:split], "' #)${} '", '"$v"', "\\ ", payload[split:]]
    rng.shuffle(pieces)
    word = "".join(pieces)
    return ("quote-placement", ALL,
            "v='V W'\nprintf '<%s>\\n' " + word + "\n")


def _substitution(rng):
    payload = _payload(rng)
    command = "printf '%s' '" + payload + "'"
    depth = rng.randint(1, 4)
    for level in range(depth):
        if level & 1:
            command = "printf '%s' \"$(" + command + ")\""
        else:
            command = "printf '[%s]' \"$(" + command + ")\""
    return (f"substitution-depth-{depth}", ALL,
            "printf '<%s>\\n' \"$(" + command + ")\"\n")


def _heredoc(rng):
    payload = _payload(rng, "body-")
    delimiter = "MW_" + "D" * rng.choice(DELIMITER_LENGTHS)
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
    return ("heredoc-quoted" if quoted else "heredoc-expanded", ALL, script)


def _comment_boundary(rng):
    payload = _payload(rng)
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
    return (f"comment-continuation-{shape}", ALL, script)


def _operators(rng):
    payload = _payload(rng)
    shape = rng.randrange(4)
    scripts = (
        "{ printf '<%s>' '" + payload + "'; false; } || printf '<fallback>\\n'\n",
        "true && (printf '<%s>' '" + payload + "'; printf '<sub>')\nprintf '<end>\\n'\n",
        "false || { printf '<%s>' '" + payload + "'; true; } && printf '<and>\\n'\n",
        "printf '<%s>' '" + payload + "' | { cat; printf '<pipe>\\n'; }\n",
    )
    return (f"operator-boundary-{shape}", ALL, scripts[shape])


def _redirection(rng):
    payload = _payload(rng)
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
    return (f"redirection-boundary-{shape}", ALL, script)


def _syntax_mutation(rng):
    shape = rng.randrange(6)
    scripts = (
        "printf '<before>\\n'\nprintf '%s\\n' \"unterminated\n",
        "printf '<before>\\n'\nprintf '%s\\n' 'unterminated\n",
        "printf '<before>\\n'\nprintf '%s\\n' \"$(printf nested\"\n",
        "v=abc\nprintf '<before>\\n'\nprintf '%s\\n' \"${v#'a}\"\n",
        "printf '<before>\\n'\nif true; then printf '<open>'\n",
        "printf '<before>\\n'\nprintf '%s\\n' $((1 + (2 * 3)\n",
    )
    return (f"syntax-mutation-{shape}", ALL, scripts[shape])


def _stray_terminator(rng):
    # Each spelling is legal only while a matching grammar production owns
    # it. A non-interactive reader must reject the whole input, not recover at
    # the next physical line as an interactive prompt would.
    token = rng.choice(("}", ")", "then", "else", "fi", "do", "done",
                        "esac", ";;"))
    return ("stray-terminator-" + token.replace(";", "semi"), ALL,
            token + "\nprintf '<after>\\n'\n")


def _function_metadata(rng):
    suffix = "n" * rng.choice(TOKEN_LENGTHS)
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


def _nested_syntax(rng):
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
    return ("nested-syntax-boundary", ALL,
            setup + body + "printf 'outer-after:%s\\n' \"$?\"\n")


GENERATORS = (_quotes, _substitution, _heredoc, _comment_boundary,
              _operators, _redirection, _syntax_mutation, _stray_terminator,
              _function_metadata, _nested_syntax)


def cases(rng, budget):
    """Yield a balanced, deterministic number of scripts for this domain."""
    for index in range(budget):
        yield GENERATORS[index % len(GENERATORS)](rng)
