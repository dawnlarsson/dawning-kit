"""Grammar for every shell builtin and shell variable, in three engine modes.

Each builtin is a Utility whose script(argv, stdin) wraps the walked words into
a program: a prologue that builds any fixtures the builtin inspects (symlinks,
fifos, executables, timestamped files -- FIXTURES["shell"] has none of these),
the builtin run with the generated words, the exit status, and then every
observable effect the builtin has (variables, positional parameters, PWD, the
directory stack, umask, the trap table, alias and hash listings, declare -p of
the fixed names it touched). Value-heavy surfaces that an option walk cannot
describe -- printf's conversions, read's IFS splitting, the array and nameref
machinery, the dynamic shell variables -- are FAMILIES of seeded programs.

Modes: a POSIX builtin runs in ("bash", "posix", "dash"); a Bash extension in
("bash", "posix"), so it is always compared against a reference that has it.
Diagnostic wording is ours, so stderr policy is "loose" throughout: it still
catches "silent where the reference complained", the signal worth having.
"""

import shlex

from differential import Utility, Option, INPUTS, FIXTURES

ALL = ("bash", "posix", "dash")
BASH = ("bash", "posix")
BASH_ONLY = ("bash",)


# ----------------------------------------------------------------------------
#       Wrapping walked words into a program.
# ----------------------------------------------------------------------------

def builtins_words(argv):
    return " ".join(shlex.quote(word) for word in argv)


def builtins_wrap(command, report="", prologue="", status=True, join=" "):
    """A script that sets up state, runs `command <words>`, prints the status
    and then any further observable effect the report names."""
    def script(argv, stdin):
        body = prologue
        body += command
        if argv:
            body += join + builtins_words(argv)
        body += "\n"
        if status:
            body += 'printf "[%s]\\n" "$?"\n'
        body += report
        return body
    return script


# A one-second gap that both interpreters see identically, so -nt/-ot compare a
# real order rather than one filesystem tick either side happened to land on.
# The nanosecond pair is a second apart in whole seconds but one nanosecond in
# the fraction: the distinction -nt is precisely meant to resolve.
TEST_PROLOGUE = (
    "/usr/bin/touch -d @1000000000 older\n"
    "/usr/bin/touch -d @1000000001 newer\n"
    "/usr/bin/touch -d @1000000001 same\n"
    "/bin/ln newer link 2>/dev/null || /bin/cp newer link\n"
    "/bin/ln -s newer soft\n"
    "/bin/ln -s nowhere dangling\n"
    "/bin/mkdir -p adir\n"
    "/bin/chmod 1755 adir\n"
    ": > suid; /bin/chmod 4644 suid\n"
    ": > sgid; /bin/chmod 2644 sgid\n"
    ": > none; /bin/chmod 0 none\n"
    "/usr/bin/mkfifo afifo 2>/dev/null || :\n"
    "printf '#!/bin/sh\\nexit 0\\n' > anexe; /bin/chmod 755 anexe\n"
    "/usr/bin/touch -d @1000000000.100000000 nsa\n"
    "/usr/bin/touch -d @1000000000.100000001 nsb\n"
)

# Operand shapes for test/[ : the unary operators against real fixtures, the
# binary string and numeric operators, the file-comparison operators, and the
# 0/1/2/3/4-argument counting rules including the leading-! and paren forms.
TEST_OPERANDS = (
    (), ("x",), ("",), ("-n", "x"), ("-z", ""), ("-z", "x"),
    ("!", ""), ("!", "x"), ("!", "-f", "none"),
    ("-e", "newer"), ("-e", "missing"), ("-f", "newer"), ("-f", "adir"),
    ("-d", "adir"), ("-d", "newer"), ("-r", "newer"), ("-r", "none"),
    ("-w", "newer"), ("-x", "anexe"), ("-x", "newer"), ("-s", "newer"),
    ("-s", "none"), ("-h", "soft"), ("-L", "soft"), ("-f", "soft"),
    ("-p", "afifo"), ("-p", "newer"), ("-S", "newer"), ("-b", "newer"),
    ("-c", "/dev/null"), ("-b", "/dev/null"), ("-t", "0"), ("-t", "9"),
    ("-u", "suid"), ("-u", "newer"), ("-g", "sgid"), ("-k", "adir"),
    ("-k", "newer"), ("-O", "newer"), ("-G", "newer"), ("-N", "newer"),
    ("newer", "-nt", "older"), ("older", "-ot", "newer"),
    ("newer", "-nt", "missing"), ("newer", "-ef", "link"),
    ("newer", "-ef", "same"), ("nsb", "-nt", "nsa"), ("nsa", "-nt", "nsb"),
    ("a", "=", "a"), ("a", "=", "b"), ("a", "!=", "b"),
    ("1", "-eq", "1"), ("1", "-eq", "2"), ("2", "-gt", "1"), ("1", "-lt", "2"),
    ("1", "-ne", "2"), ("1", "-ge", "1"), ("1", "-le", "0"), ("1", "-eq", "a"),
    ("=", "=", "="), ("!", "=", "x"), ("!", "-f", "nosuchfile"),
    ("(", "", ")"), ("(", "x", ")"), ("(", "!", "", ")"),
    ("x", "-a", "y"), ("", "-o", "y"), ("!", "x", "=", "x"),
    ("-f", "newer", "-a", "-d", "adir"), ("-z", "x", "-o", "-n", "x"),
)

# Bash extensions dash's test rejects: == and the string ordering operators.
# -N (modified since last read) is unimplemented in ours -- pinned as a bug.
TEST_EXT_OPERANDS = (
    ("a", "==", "a"), ("a", "==", "b"), ("a", "<", "b"), ("b", "<", "a"),
    ("b", ">", "a"), ("-N", "newer"), ("-N", "missing"),
)


def builtins_bracket(operands):
    return tuple(tuple(op) + ("]",) for op in operands)


# ----------------------------------------------------------------------------
#       The builtins as option grammars.
# ----------------------------------------------------------------------------

UTILITIES = []


def builtins_add(utility):
    UTILITIES.append(utility)


# --- : true false -----------------------------------------------------------
for _cmd, _uname in ((":", "colon"), ("true", "true"), ("false", "false")):
    builtins_add(Utility(_uname,
                         operands=((), ("ignored",), ("-x", "ignored"), ("--",)),
                         stdin=("empty",), stderr="loose", modes=ALL,
                         script=builtins_wrap(_cmd), max_flags=2))

# --- echo -------------------------------------------------------------------
builtins_add(Utility(
    "echo",
    options=(Option("-n"), Option("-e"), Option("-E")),
    operands=((), ("plain",), ("a", "b"), ("-x",), ("-",), ("--",),
              (r"a\tb\n",), (r"x\0101y",), (r"a\cz", "next"), (r"a\x41é",),
              (r"esc\e[0m\a\b\f\v\rtail",), ("-neE", "x")),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap("echo", status=False), max_flags=3))
builtins_add(Utility(
    "echo_xpg",
    options=(Option("-n"), Option("-e"), Option("-E")),
    operands=((r"a\tb",), (r"x\0101y",), ("-x",), ("plain",), (r"a\cz", "next")),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("echo", prologue="shopt -s xpg_echo\n", status=False),
    max_flags=3))

# --- test and [ -------------------------------------------------------------
builtins_add(Utility(
    "test", operands=TEST_OPERANDS, stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap("test", prologue=TEST_PROLOGUE), max_flags=0))
builtins_add(Utility(
    "bracket", operands=builtins_bracket(TEST_OPERANDS), stdin=("empty",),
    stderr="loose", modes=ALL,
    script=builtins_wrap("[", prologue=TEST_PROLOGUE), max_flags=0))
builtins_add(Utility(
    "test_ext", operands=TEST_EXT_OPERANDS, stdin=("empty",), stderr="loose",
    modes=BASH, script=builtins_wrap("test", prologue=TEST_PROLOGUE),
    max_flags=0))
builtins_add(Utility(
    "bracket_ext", operands=builtins_bracket(TEST_EXT_OPERANDS),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("[", prologue=TEST_PROLOGUE), max_flags=0))

# --- cd ---------------------------------------------------------------------
CD_PROLOGUE = (
    "/bin/mkdir -p real one/two two\n"
    "/bin/ln -s real softdir\n"
    "cd \"$PWD\" >/dev/null 2>&1\n"
)
CD_REPORT = 'printf "pwd:%s\\nold:%s\\n" "$PWD" "${OLDPWD-unset}"\n'
builtins_add(Utility(
    "cd",
    options=(Option("-L"), Option("-P"), Option("-e"), Option("-@")),
    operands=((), ("real",), ("softdir",), ("one/two",), ("..",), ("-",),
              ("",), ("missing12345",), ("/tmp",), ("/",), ("real", "extra")),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap("cd", report=CD_REPORT, prologue=CD_PROLOGUE),
    max_flags=3))
# CDPATH search: an entry list read from a fixed layout, deterministic result.
builtins_add(Utility(
    "cd_cdpath",
    operands=(("two",), ("./two",)),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=lambda argv, stdin: (
        "/bin/mkdir -p one/two two\n"
        "CDPATH=" + shlex.quote("$PWD/nowhere:$PWD/one") + "\n"
        "cd " + builtins_words(argv) + " >/dev/null 2>/dev/null\n"
        'printf "[%s]\\n" "$?"\n'
        'printf "in:%s\\n" "${PWD##*/}"\n'),
    max_flags=0))

# --- pwd --------------------------------------------------------------------
builtins_add(Utility(
    "pwd", options=(Option("-L"), Option("-P")),
    operands=((), ("-x",), ("--",)),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap("pwd"), max_flags=2))

# --- pushd popd dirs --------------------------------------------------------
DIRS_PROLOGUE = "/bin/mkdir -p da db dc\ncd \"$PWD\" >/dev/null 2>&1\n"
builtins_add(Utility(
    "pushd",
    options=(Option("-n"),),
    operands=(("da",), ("da/../db",), ("+0",), ("+1",), ("-0",), ("+9",),
              (), ("missing12345",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap(
        "pushd", prologue=DIRS_PROLOGUE + "pushd db >/dev/null 2>&1\n",
        report='dirs\n'),
    max_flags=1))
builtins_add(Utility(
    "popd",
    options=(Option("-n"),),
    operands=(("+0",), ("+1",), ("-0",), ("+9",), (), ("extra",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap(
        "popd", prologue=DIRS_PROLOGUE + "pushd da >/dev/null; pushd db >/dev/null\n",
        report='dirs\n'),
    max_flags=1))
builtins_add(Utility(
    "dirs",
    options=(Option("-c"), Option("-l"), Option("-p"), Option("-v")),
    operands=((), ("+0",), ("+1",), ("-0",), ("+9",), ("-9",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap(
        "dirs", prologue=DIRS_PROLOGUE + "pushd da >/dev/null; pushd db >/dev/null\n"),
    max_flags=4))

# --- umask ------------------------------------------------------------------
builtins_add(Utility(
    "umask",
    options=(Option("-S"), Option("-p")),
    operands=((), ("022",), ("0",), ("0777",), ("0177",), ("0077",),
              ("u=rwx,g=rx,o=",), ("a-w",), ("u+r,go=",), ("=rx",),
              ("g=u,o+g",), ("g+X",), ("a+X",), ("g+s",), ("u+w+r",),
              ("u=r=w",), ("u+r,",), (",",), ("zzz",), ("078",), ("g+t",)),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap("umask", prologue="umask 022\n",
                         report='umask\numask -S\n'),
    max_flags=2))

# --- ulimit -----------------------------------------------------------------
# Bash and our shell share the wide letter set; dash has only -f -t -s -c -d -m
# -n -p -v (and -H/-S). Split so each mode is compared against a reference that
# accepts the same letters.
builtins_add(Utility(
    "ulimit",
    options=(Option("-H"), Option("-S"),
             Option("-a"), Option("-c"), Option("-d"), Option("-f"),
             Option("-l"), Option("-m"), Option("-n"), Option("-p"),
             Option("-s"), Option("-t"), Option("-u"), Option("-v"),
             Option("-x"), Option("-i"), Option("-q"), Option("-r"),
             Option("-e")),
    operands=((), ("100",), ("unlimited",), ("hard",), ("soft",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("ulimit"), max_flags=2))
builtins_add(Utility(
    "ulimit_posix",
    options=(Option("-H"), Option("-S"), Option("-a"), Option("-c"),
             Option("-d"), Option("-f"), Option("-m"), Option("-n"),
             Option("-p"), Option("-s"), Option("-t"), Option("-v")),
    operands=((), ("100",), ("unlimited",)),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap("ulimit"), max_flags=2))

# --- alias unalias ----------------------------------------------------------
ALIAS_PROLOGUE = "alias mw_a='echo one'; alias mw_b='echo two'\n"
builtins_add(Utility(
    "alias",
    operands=((), ("mw_a",), ("mw_a", "mw_b"), ("mw_c=echo three",),
              ("mw_a=echo changed",), ("missing12345",), ("mw_d='a b'",),
              ("-p",)),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap("alias", prologue=ALIAS_PROLOGUE,
                         report='alias mw_a 2>/dev/null; alias mw_c 2>/dev/null\n'),
    max_flags=0))
builtins_add(Utility(
    "unalias",
    options=(Option("-a"),),
    operands=((), ("mw_a",), ("mw_a", "mw_b"), ("missing12345",)),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap("unalias", prologue=ALIAS_PROLOGUE,
                         report='alias 2>&1 | /bin/grep mw_ \n'),
    max_flags=1))

# --- hash -------------------------------------------------------------------
HASH_PROLOGUE = ("/bin/mkdir -p hbin\n"
                 "printf '#!/bin/sh\\nexit 0\\n' > hbin/mw_prog\n"
                 "/bin/chmod 755 hbin/mw_prog\nPATH=\"$PWD/hbin:/usr/bin:/bin\"\n")
builtins_add(Utility(
    "hash",
    options=(Option("-r"), Option("-t"), Option("-d"),
             Option("-p", ("/bin/true",), None)),
    operands=(("mw_prog",), ("mw_prog", "sh"), ("missing12345",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("hash", prologue=HASH_PROLOGUE + "hash mw_prog 2>/dev/null\n",
                         report='hash -t mw_prog 2>/dev/null\n'),
    max_flags=2))
# Bare `hash` and `hash -l` print the table; ours writes it dash-style (paths,
# no "hits command" header): a deliberate listing-format difference, pinned.
builtins_add(Utility(
    "hash_list",
    operands=((), ("-l",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("hash", prologue=HASH_PROLOGUE + "hash mw_prog sh 2>/dev/null\n"),
    max_flags=0))
builtins_add(Utility(
    "hash_posix",
    options=(Option("-r"),),
    operands=((), ("mw_prog",), ("missing12345",)),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap("hash", prologue=HASH_PROLOGUE),
    max_flags=1))

# --- type -------------------------------------------------------------------
TYPE_PROLOGUE = ("f_func() { :; }\nalias mw_al='echo x' 2>/dev/null\n"
                 "PATH=/usr/bin:/bin\n")
builtins_add(Utility(
    "type",
    options=(Option("-a"), Option("-f"), Option("-p"), Option("-P"),
             Option("-t")),
    operands=(("cd",), ("echo",), ("f_func",), ("sh",), ("if",), ("missing12345",),
              ("echo", "sh"), ("mw_al",), ("--", "cd")),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("type", prologue=TYPE_PROLOGUE), max_flags=3))
builtins_add(Utility(
    "type_posix",
    operands=(("cd",), ("echo",), ("sh",), ("missing12345",), ("echo", "sh")),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap("type", prologue="PATH=/usr/bin:/bin\n"), max_flags=0))

# --- command ----------------------------------------------------------------
builtins_add(Utility(
    "command",
    options=(Option("-v"), Option("-V"), Option("-p")),
    operands=(("echo",), ("cd",), ("sh",), ("missing12345",), ("echo", "sh"),
              ("--", "printf", "[%s]", "x")),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap("command", prologue="PATH=/usr/bin:/bin\n"),
    max_flags=2))

# --- kill (never a real signal to anything: -l/-L conversions and signal 0) --
builtins_add(Utility(
    "kill_list",
    operands=(("-l",), ("-L",), ("-l", "9"), ("-l", "15"), ("-l", "137"),
              ("-l", "64"), ("-l", "TERM"), ("-l", "SIGTERM"), ("-l", "0"),
              ("-l", "bad")),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("kill"), max_flags=0))
# Only signal 0 (existence probe) and error surfaces reach a real pid; SELF is
# this shell's own pid, so nothing that could stop it is ever delivered.
builtins_add(Utility(
    "kill_send",
    operands=(("-0", "SELF"), ("-s", "0", "SELF"), ("-n", "0", "SELF"),
              ("-0", "999999"), ("%1",), ("%nosuch",), ("-s", "bogus", "SELF"),
              ("--", "-0", "SELF")),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=lambda argv, stdin: (
        "kill " + builtins_words(argv).replace("SELF", "$$") + " 2>/dev/null\n"
        'printf "[%s]\\n" "$?"\n'),
    max_flags=0))

# --- trap -------------------------------------------------------------------
# Report through bare `trap`, which lists only the conditions that have been
# set -- as bash and dash do. (`trap -p` with no signal lists every default in
# this shell, a deliberate difference pinned separately.)
builtins_add(Utility(
    "trap",
    operands=((), ("echo hi", "INT"), ("echo hi", "int"),
              ("echo hi", "SIGINT"), ("echo hi", "2"), ("", "HUP"),
              ("echo x", "EXIT"), ("echo x", "TERM", "HUP"),
              ("echo", "bogus"), ("echo", "64"), ("echo", "65"), ("-", "INT"),
              ("echo hi", "hUp"), ("echo", "40"), ("echo", "USR1")),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap("trap", report='trap 2>/dev/null | /bin/grep -c . \n'),
    max_flags=0))
# -l and -p are Bash extensions dash does not have. `trap -p` with no signal
# lists every default in this shell (bash lists only what is set) -- pinned.
builtins_add(Utility(
    "trap_pl",
    operands=(("-l",), ("-p",), ("-p", "INT"), ("-p", "INT", "TERM")),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap(
        "trap", prologue="trap 'echo x' INT\n",
        report='trap 2>/dev/null | /bin/grep -c . \n'),
    max_flags=0))

# --- set --------------------------------------------------------------------
# Bare `set` lists every variable, and the two shells hold different internal
# variables -- an oracle problem, not a bug -- so a case must carry a flag or
# operand. -o/+o take their name as a separate word, as both references do.
builtins_add(Utility(
    "set",
    options=(Option("-e"), Option("-u"), Option("-x"), Option("-f"),
             Option("-o", ("nounset", "errexit", "pipefail", "noclobber",
                            "posix", "nosuch"), False),
             Option("+o", ("nounset",), False)),
    operands=((), ("--",), ("--", "a", "b"), ("-", "a"), ("a", "b", "c")),
    stdin=("empty",), stderr="loose", modes=ALL, valid=lambda argv: bool(argv),
    script=builtins_wrap(
        "set", report='printf "flags:%s\\n" "$-"\n'
                      'printf "args:%s#%s\\n" "$#" "$*"\n'),
    max_flags=3))
builtins_add(Utility(
    "set_list",
    operands=(("-o",), ("+o",)),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap("set", status=False), max_flags=0))

# --- shift ------------------------------------------------------------------
builtins_add(Utility(
    "shift",
    operands=((), ("1",), ("2",), ("3",), ("0",), ("5",), ("-1",), ("bad",)),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap(
        "shift", prologue="set -- a b c\n",
        report='printf "args:%s#%s\\n" "$#" "$*"\n'),
    max_flags=0))

# --- unset ------------------------------------------------------------------
builtins_add(Utility(
    "unset",
    options=(Option("-v"), Option("-f")),
    operands=(("v",), ("v", "w"), ("missing12345",), ("f_func",), ("PATH",),
              ("arr",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap(
        "unset",
        prologue="v=1; w=2; arr=(x y z); f_func() { :; }\n",
        report='printf "<%s><%s>\\n" "${v-unset}" "${w-unset}"\n'
               'type f_func 2>&1 | /bin/grep -c function\n'),
    max_flags=2))
builtins_add(Utility(
    "unset_posix",
    options=(Option("-v"), Option("-f")),
    operands=(("v",), ("v", "w"), ("missing12345",), ("f_func",)),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap(
        "unset", prologue="v=1; w=2; f_func() { :; }\n",
        report='printf "<%s><%s>\\n" "${v-unset}" "${w-unset}"\n'),
    max_flags=2))

# --- export / readonly ------------------------------------------------------
for name, modes in (("export", ALL), ("readonly", ALL)):
    builtins_add(Utility(
        name,
        options=(Option("-p"),),
        operands=(("v=1",), ("v",), ("v=1", "w=2"), ("v='a b'",), (),
                  ("1bad=x",)),
        stdin=("empty",), stderr="loose", modes=modes,
        script=builtins_wrap(
            name, prologue="v=old\n",
            report='printf "v=<%s>|w=<%s>\\n" "${v-unset}" "${w-unset}"\n'
                   + name + ' -p 2>/dev/null | /bin/grep -Eo "(v|w)=[^ ]*" '
                   '| /bin/sort | /bin/tr "\\n" "|"; echo\n'),
        max_flags=1))
# Bash's -n -f -F on export/readonly are extensions.
builtins_add(Utility(
    "export_flags",
    options=(Option("-n"), Option("-f"), Option("-p")),
    operands=(("v",), ("f_func",), ("missing12345",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap(
        "export", prologue="v=1; export v; f_func() { :; }; export -f f_func\n",
        report='printf "<%s>\\n" "${v-unset}"\n'),
    max_flags=2))

# --- declare / typeset ------------------------------------------------------
for name in ("declare", "typeset"):
    builtins_add(Utility(
        name,
        options=(Option("-a"), Option("-A"), Option("-i"), Option("-l"),
                 Option("-u"), Option("-r"), Option("-x"), Option("-g"),
                 Option("-n"), Option("-p"), Option("-t")),
        operands=(("v=1",), ("v",), ("v=hello",), ("v=2+3",), ("v=ABC",),
                  ("v", "w"), ("arr=(x y z)",), ("-p", "v"), ("v=1", "w=2")),
        stdin=("empty",), stderr="loose", modes=BASH,
        # A case must name something: bare declare / declare -p lists every
        # variable, and the two shells hold different ones (an oracle problem).
        valid=lambda argv: any(not w.startswith("-") for w in argv),
        script=builtins_wrap(
            name, prologue="w=seed\n",
            report='declare -p v 2>/dev/null; declare -p w 2>/dev/null; declare -p arr 2>/dev/null\n'),
        max_flags=3))
builtins_add(Utility(
    "declare_F",
    options=(Option("-f"), Option("-F")),
    operands=((), ("f_one",), ("f_one", "f_two"), ("missing12345",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    # Require -f or -F: bare declare lists all variables (an oracle problem).
    valid=lambda argv: "-f" in argv or "-F" in argv,
    script=builtins_wrap(
        "declare", prologue="f_one() { :; }; f_two() { echo x; }\n"),
    max_flags=2))

# --- local (inside a function) ----------------------------------------------
def builtins_local_script(argv, stdin):
    return ("v=global; w=global2\nf() {\n"
            "local " + builtins_words(argv) + "\n"
            'printf "[%s]\\n" "$?"\n'
            'declare -p v 2>/dev/null; declare -p w 2>/dev/null\n'
            "}\nf\n"
            'printf "after:%s\\n" "$v"\n')


builtins_add(Utility(
    "local",
    options=(Option("-a"), Option("-A"), Option("-i"), Option("-r"),
             Option("-x"), Option("-n"), Option("-l"), Option("-u")),
    operands=(("v=1",), ("v",), ("v=2+3",), ("w",), ("v=x", "w=y")),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_local_script, max_flags=3))

# --- getopts ----------------------------------------------------------------
def builtins_getopts_script(argv, stdin):
    # The spec and words come from operands; the loop is fixed and reports the
    # option letter, OPTARG and OPTIND each pass and the terminal state.
    return ("set -- " + builtins_words(argv[1:]) + "\n"
            "OPTIND=1; n=0\n"
            "while getopts " + shlex.quote(argv[0]) + " opt; do\n"
            'printf "<%s>:<%s>:%s\\n" "$opt" "${OPTARG-unset}" "$OPTIND"\n'
            'n=$((n+1)); [ "$n" -lt 8 ] || break\n'
            "done\n"
            'printf "end:%s:<%s>:<%s>\\n" "$OPTIND" "$opt" "${OPTARG-unset}"\n')


builtins_add(Utility(
    "getopts",
    operands=(("ab", "-a", "-b", "c"), (":ab", "-a", "-z"),
              ("a:", "-aval", "b"), ("a:", "-a", "val", "b"),
              ("ab", "-ab", "c"), ("ab", "--", "-a"), ("ab", "x"),
              (":a:", "-a"), ("a:", "-a"), (":ab", "-az"),
              ("ab", "-a", "-b"), ("a:b", "-b", "-aval")),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_getopts_script, max_flags=0))

# --- read -------------------------------------------------------------------
def builtins_read_report(names):
    return "".join('printf "<%%s>" "${%s-unset}"\n' % n for n in ("a", "b", "c")) + 'echo\n'


def builtins_read_script(argv, stdin):
    # argv = options followed by variable names (a subset of a b c). Feed a
    # fixed record on a file, run read, print status then a/b/c.
    return ("printf '%s' 'one two:three ' > feed\n"
            "a=old; b=old; c=old\n"
            "read " + builtins_words(argv) + " < feed\n"
            'printf "[%s]\\n" "$?"\n'
            + builtins_read_report(("a", "b", "c")))


builtins_add(Utility(
    "read",
    options=(Option("-r"),),
    operands=(("a",), ("a", "b"), ("a", "b", "c")),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_read_script, max_flags=1))
# Bash extensions: -n -N -d -p -s -e -a and error surfaces of -u -t.
def builtins_read_ext_script(argv, stdin):
    return ("printf 'abcdef:gh\\n' > feed\n"
            "a=old; b=old; c=old\n"
            "read " + builtins_words(argv) + " < feed\n"
            'printf "[%s]\\n" "$?"\n'
            + builtins_read_report(("a", "b", "c")))


builtins_add(Utility(
    "read_ext",
    options=(Option("-r"), Option("-s"), Option("-e"),
             Option("-n", ("0", "2", "3"), None),
             Option("-N", ("0", "2", "3"), None),
             Option("-d", (":", ""), None),
             Option("-p", ("prompt:",), None),
             Option("-t", ("0",), None)),
    operands=(("a",), ("a", "b")),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_read_ext_script, max_flags=3))
# read error surfaces that do not depend on timing.
builtins_add(Utility(
    "read_errors",
    operands=(("-Z", "v"), ("1bad",), ("-u", "4294967296", "v"),
              ("-u", "9", "v"), ("-t", "-1", "v"), ("-t", "bad", "v"),
              ("-n", "bad", "v"), ("-N",), ("v", "<&-")),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=lambda argv, stdin: (
        "v=old\nread " + builtins_words(argv) + " </dev/null 2>/dev/null\n"
        'printf "[%s]<%s>\\n" "$?" "${v-unset}"\n'),
    max_flags=0))

# --- mapfile / readarray ----------------------------------------------------
def builtins_mapfile_script(name):
    def script(argv, stdin):
        return ("printf 'r0\\nr1\\nr2\\nr3\\nr4\\n' > recs\n"
                "arr=(old old old old)\n"
                + name + " " + builtins_words(argv) + " arr < recs\n"
                'printf "[%s] n:%s\\n" "$?" "${#arr[@]}"\n'
                'printf "<%s>\\n" "${arr[@]}"\n')
    return script


for name in ("mapfile", "readarray"):
    builtins_add(Utility(
        name,
        options=(Option("-t"), Option("-n", ("0", "2"), None),
                 Option("-O", ("0", "3"), None), Option("-s", ("0", "1"), None),
                 Option("-d", (":",), None), Option("-c", ("2",), None),
                 Option("-u", ("0",), None)),
        operands=((),),
        stdin=("empty",), stderr="loose", modes=BASH,
        script=builtins_mapfile_script(name), max_flags=3))

# --- printf (option surface; conversions are FAMILIES) ----------------------
builtins_add(Utility(
    "printf",
    options=(Option("-v", ("out",), None),),
    operands=(("%s\\n", "x"), ("--", "%s\\n", "x"), ("-x", "value"),
              ("%d\\n", "abc"), ("%y\\n", "x"), ("%s-", "a", "b", "c"),
              ("%s-%s|", "a"), ("abc%",), ("%(%Y)T\\n", "0"),
              ("%(%Y)T\\n", "bad")),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=lambda argv, stdin: (
        "out=before\nprintf " + builtins_words(argv) + " 2>/dev/null\n"
        'printf "|[%s]" "$?"\nprintf "|out=%s\\n" "${out-unset}"\n'),
    max_flags=1))

# --- exec (redirection-only and -a/-c/-l option errors) ---------------------
builtins_add(Utility(
    "exec",
    operands=(("-a", "named", "sh", "-c", "echo $0"),
              ("-c", "sh", "-c", "echo ${X-unset}"),
              ("-l", "sh", "-c", "echo $0"),
              ("-a",), ("-az", "-Q"), ("--",),
              ("3>out",), (">out",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=lambda argv, stdin: (
        "X=set\n(exec " + builtins_words(argv) + ") 2>/dev/null\n"
        'printf "[%s]\\n" "$?"\necho survived\n'),
    max_flags=0))

# --- eval -------------------------------------------------------------------
builtins_add(Utility(
    "eval",
    operands=((), ("echo x",), ("echo", "x", "y"), ("",), ("set -- a b",),
              ("false",), ("exit 4",)),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=builtins_wrap("eval", report='printf "args:%s\\n" "$#"\n'),
    max_flags=0))

# --- . / source -------------------------------------------------------------
SOURCE_PROLOGUE = (
    'printf "echo \\"<\\$0> \\$# <\\$1>\\"\\n" > mw_src\n'
    "/bin/mkdir -p sbin; printf 'echo found-on-path\\n' > sbin/mw_onpath\n"
    "PATH=\"$PWD/sbin:/usr/bin:/bin\"\n")
for name in (".", "source"):
    builtins_add(Utility(
        name if name != "." else "dot",
        operands=(("./mw_src",), ("mw_onpath",), ("./missing12345",), ()),
        stdin=("empty",), stderr="loose", modes=ALL,
        script=builtins_wrap(name, prologue=SOURCE_PROLOGUE),
        max_flags=0))
# Positional arguments to a sourced file are a Bash extension; dash's dot
# ignores them, so this shape is only compared where the reference has it.
for name in (".", "source"):
    builtins_add(Utility(
        (name if name != "." else "dot") + "_args",
        operands=(("./mw_src", "a", "b"),),
        stdin=("empty",), stderr="loose", modes=BASH,
        script=builtins_wrap(name, prologue=SOURCE_PROLOGUE),
        max_flags=0))

# --- return / exit ----------------------------------------------------------
builtins_add(Utility(
    "return",
    operands=((), ("0",), ("3",), ("-1",), ("256",), ("300",), ("bad",)),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=lambda argv, stdin: (
        "f() { return " + builtins_words(argv) + "; }\nf\n"
        'printf "infn:[%s]\\n" "$?"\n'
        "(return " + builtins_words(argv) + ") 2>/dev/null\n"
        'printf "top:[%s]\\n" "$?"\n'),
    max_flags=0))
builtins_add(Utility(
    "exit",
    operands=((), ("0",), ("3",), ("-1",), ("256",), ("300",), ("bad",)),
    stdin=("empty",), stderr="loose", modes=ALL,
    script=lambda argv, stdin: (
        "(exit " + builtins_words(argv) + ") 2>/dev/null\n"
        'printf "[%s]\\n" "$?"\n'),
    max_flags=0))

# --- let --------------------------------------------------------------------
builtins_add(Utility(
    "let",
    operands=(("x=3+4",), ("x=3+4", "y=x*2"), ("x=1<<3",), ("x=2**10",),
              ("x=16#ff",), ("x=0x10",), ("x=010",), ("x=1?2:3",),
              ("x+=2",), ("x=5%3",), ("x=1/0",), ("x=0",), ("x=1",),
              ("x=y=3",), ("x=1==1",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=lambda argv, stdin: (
        "x=1; y=1\nlet " + builtins_words(argv) + " 2>/dev/null\n"
        'printf "[%s] x=%s y=%s\\n" "$?" "$x" "$y"\n'),
    max_flags=0))

# --- shopt ------------------------------------------------------------------
builtins_add(Utility(
    "shopt",
    options=(Option("-s"), Option("-u"), Option("-q"), Option("-p"),
             Option("-o")),
    operands=((), ("extglob",), ("nullglob",), ("nosuch",), ("dotglob",),
              ("extglob", "nullglob"), ("pipefail",), ("--",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("shopt"), max_flags=2))

# --- enable -----------------------------------------------------------------
# Functional surface: disabling and re-enabling a builtin, and the not-a-
# builtin error. Reports the effect on `type` rather than any listing.
builtins_add(Utility(
    "enable",
    operands=(("echo",), ("-n", "echo"), ("true",), ("missing12345",),
              ("-n", "true"), ("--", "true")),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("enable", report='type -t true 2>&1\n'),
    max_flags=0))
# Listings enumerate this shell's own builtin set (different from bash's), and
# loadable builtins are unsupported: deliberate differences, pinned.
builtins_add(Utility(
    "enable_list",
    operands=((), ("-a",), ("-p",), ("-s",), ("-n",),
              ("-f", "/nonexistent", "x"), ("-d", "echo")),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("enable"), max_flags=0))

# --- builtin ----------------------------------------------------------------
builtins_add(Utility(
    "builtin",
    operands=(("echo", "x"), ("cd", "/tmp"), ("true",), ("false",),
              ("missing12345",), ("ls", "-d", "."), ()),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("builtin"), max_flags=0))

# --- caller (option error + outside-function surface) -----------------------
builtins_add(Utility(
    "caller",
    operands=((), ("0",), ("1",), ("99",), ("x",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=lambda argv, stdin: (
        "caller " + builtins_words(argv) + " 2>/dev/null\n"
        'printf "top:[%s]\\n" "$?"\n'),
    max_flags=0))

# --- help -------------------------------------------------------------------
builtins_add(Utility(
    "help",
    operands=(("missing12345",), ("--", "cd"), ("-Z",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=lambda argv, stdin: (
        "help " + builtins_words(argv) + " >/dev/null 2>/dev/null\n"
        'printf "[%s]\\n" "$?"\n'),
    max_flags=0))

# --- times (timing values normalized to their shape) ------------------------
import re as builtins_re
builtins_add(Utility(
    "times", operands=((),), stdin=("empty",), stderr="loose", modes=ALL,
    normalize=lambda channel, data: builtins_re.sub(rb"[0-9]+", b"N", data),
    script=lambda argv, stdin: "times\n", max_flags=0))

# --- compgen ----------------------------------------------------------------
builtins_add(Utility(
    "compgen",
    options=(Option("-A", ("function", "builtin", "alias", "keyword"), None),
             Option("-W", ("one two three",), None),
             Option("-P", ("<",), None), Option("-S", (">",), None),
             Option("-X", ("t*",), None)),
    operands=((), ("b",), ("t",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("compgen", prologue="f_one() { :; }\n",
                         report="", status=True), max_flags=3))

# --- complete / compopt / bind (taken, mostly no-ops: ledger surface) -------
builtins_add(Utility(
    "complete",
    options=(Option("-p"), Option("-r")),
    operands=((), ("-F", "f", "g"), ("-W", "x y", "cmd"), ("-o", "bad", "cmd"),
              ("-Z",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("complete", prologue="f() { :; }\n"), max_flags=2))
builtins_add(Utility(
    "compopt",
    operands=((), ("-o", "nospace"), ("-Z",), ("cmd",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("compopt"), max_flags=0))
builtins_add(Utility(
    "bind",
    options=(Option("-v"), Option("-l"), Option("-p"), Option("-s"),
             Option("-q", ("abort",), None), Option("-f", ("missing",), None)),
    operands=((), ("-Z",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("bind"), max_flags=2))

# --- fc / history -----------------------------------------------------------
# The history store and its event numbering need an interactive reader; here
# only the deterministic surface is compared: clearing, expansion of a literal
# with -p, and the bad-option refusal. Store/numbering behaviour is pinned.
builtins_add(Utility(
    "history",
    operands=((), ("-c",), ("-Z",), ("-p", "text"), ("-p", "!!"),
              ("-s", "an event")),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("history", prologue="history -c 2>/dev/null\n"),
    max_flags=0))
builtins_add(Utility(
    "fc",
    operands=(("-l",), ("-ln",), ("-lr",), ("-l", "1"), ("-l", "1", "2"),
              ("-Z",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap(
        "fc", prologue="history -c 2>/dev/null; printf 'echo one\\necho two\\n' > hf; history -r hf 2>/dev/null\n"),
    max_flags=0))

# --- which (our builtin vs the external GNU program: deliberate) ------------
builtins_add(Utility(
    "which",
    options=(Option("-a"), Option("-s")),
    operands=(("sh",), ("missing12345",), (), ("sh", "cd")),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("which", prologue="PATH=/usr/bin:/bin\n"), max_flags=1))

# --- jobs / bg / fg / disown / suspend / logout (noninteractive surfaces) ---
builtins_add(Utility(
    "jobs",
    options=(Option("-l"), Option("-p"), Option("-r"), Option("-s"),
             Option("-n")),
    operands=((), ("%1",), ("%nosuch",), ("-Z",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=builtins_wrap("jobs"), max_flags=2))
for name in ("bg", "fg", "suspend", "logout", "disown"):
    builtins_add(Utility(
        name,
        operands=((), ("%1",), ("-Z",)) if name != "logout" else ((), ("3",)),
        stdin=("empty",), stderr="loose", modes=BASH,
        script=builtins_wrap(name), max_flags=0))

# --- wait (deterministic: finished child, bad pid) --------------------------
builtins_add(Utility(
    "wait",
    operands=((), ("999999",), ("%1",), ("%nosuch",), ("-n",), ("bad",)),
    stdin=("empty",), stderr="loose", modes=BASH,
    script=lambda argv, stdin: (
        "(exit 7) &\nwait " + builtins_words(argv) + " 2>/dev/null\n"
        'printf "[%s]\\n" "$?"\n'),
    max_flags=0))

# --- poweroff / reboot (machine control: bad-argument surface only) ---------
# Only bad arguments, never a bare invocation: comparing the argument-error
# behaviour is the whole of what the brief asks for these two.
for name in ("poweroff", "reboot"):
    builtins_add(Utility(
        name, operands=(("-Z",),),
        stdin=("empty",), stderr="loose", modes=BASH,
        script=lambda argv, stdin, _n=name: (
            _n + " " + builtins_words(argv) + " 2>/dev/null >/dev/null\n"
            'printf "[%s]\\n" "$?"\n'),
        max_flags=0))


UTILITIES = tuple(UTILITIES)
