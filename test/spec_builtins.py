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


# ----------------------------------------------------------------------------
#       Families: seeded whole programs, for the parts of the builtin surface
#       that are a value space rather than an option list. These absorb
#       test/shell_cases_builtin.py, and the value spaces test/shell_io.sh,
#       test/shell_variables.sh and test/builtin.sh asserted by hand.
#
#       A family gets no normalize and no timeout of its own, so anything
#       unstable is reduced inside the script itself.
# ----------------------------------------------------------------------------

def builtins_listing(rng):
    """set/declare/export/readonly quote a value the same way for one name."""
    byte = rng.choice((chr(rng.randrange(1, 128)), "é", "λ", "☃", ""))
    value = rng.choice((byte, "head" + byte, byte + "tail", "a " + byte + "z",
                        "~start", "#start", "", "simple"))
    command = rng.choice(("set", "declare", "declare -p zz", "export -p", "readonly -p"))
    setup = rng.choice(("zz=", "export zz=", "readonly zz="))
    pattern = "^zz=" if command in ("set", "declare") else " zz="
    script = (setup + shlex.quote(value) + "\n" + command +
              " | /bin/grep " + shlex.quote(pattern) + "\n")
    return "builtins-listing", ("bash", "posix"), script


def builtins_query_namespaces(rng):
    """Namespace precedence with PATH and per-name/aggregate status."""
    query = rng.choice(("type -t", "type -at", "type -p", "type -P",
                        "command -v", "command -pv"))
    state = rng.choice(("", "mw_name() { :; }",
                        "shopt -s expand_aliases; alias mw_name='printf alias'",
                        "mw_name() { :; }; shopt -s expand_aliases; alias mw_name=echo",
                        "hash -p /bin/true mw_name"))
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
    return "builtins-query-namespaces", ("bash", "posix"), script


def builtins_declaration_lifecycle(rng):
    """local and declare share storage, but not scope or failure policy."""
    setup = rng.choice(("mw_value=OLD", "export mw_value=OLD",
                        "readonly mw_value=OLD", "declare -i mw_value=17"))
    command = rng.choice(("local", "declare", "typeset"))
    flags = rng.choice(("", "-x", "+x", "-i", "-r"))
    operand = rng.choice(("mw_value", "mw_value=23"))
    if not setup.startswith("readonly") and rng.getrandbits(1):
        operand += " mw_tail=end"
    inherit = rng.choice(("shopt -u localvar_inherit", "shopt -s localvar_inherit"))
    report = "declare -p mw_value mw_tail; printf 'report:%s\\n' \"$?\""
    script = (setup + "\n" + inherit + "\nf() {\n" + command + " " + flags + " " +
              operand + "\nprintf 'declaration:%s\\n' \"$?\"\n" + report +
              "\n}\nf\n" + report + "\n")
    return "builtins-declaration-lifecycle", ("bash", "posix"), script


def builtins_inventory_state(rng):
    """Sorted live names after removal and reinsertion, attributes apart."""
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
            command = ('saved=$(export -pf); unset -f ' + marked +
                       '; eval "$saved"; ' + marked + '; declare -Fx')
            return "builtins-inventory-state", ("bash",), setup + "\n" + command + "\n"
        return "builtins-inventory-state", ("bash", "posix"), setup + "\n" + command + "\n"
    setup = "\n".join(name + "='two words'" for name in names)
    setup += "\nunset " + removed
    if rng.getrandbits(1):
        setup += "\n" + removed + "=replaced"
    marked = rng.choice([name for name in names if name != removed])
    setup += "\n" + rng.choice(("export ", "readonly ")) + marked
    command = rng.choice(("declare -p", "export -p", "readonly -p"))
    return "builtins-inventory-state", ("bash", "posix"), (
        setup + "\n" + command + " | /bin/grep -E '^(declare [^ ]+|export|readonly) mw_'\n")


def builtins_read_fields(rng):
    text = rng.choice((" a  b c ", "a::b:", ":a:b", "a\\ b c", "\\", "", "one\ntwo"))
    text += rng.choice(("", "\n"))
    separator = rng.choice((" ", ":", " :", ""))
    names = rng.choice(("a", "a b", "a b c"))
    option = rng.choice(("", "-r"))
    script = ("printf '%s' " + shlex.quote(text) + " > feed\n" +
              "a=old; b=old; c=old\nIFS=" + shlex.quote(separator) +
              " read " + option + " " + names + " < feed\n" +
              "s=$?\nprintf '%s:<%s>:<%s>:<%s>\\n' \"$s\" \"$a\" \"$b\" \"$c\"\n")
    return "builtins-read-fields", ALL, script


def builtins_read_ifs_snapshot(rng):
    """Classify once, even when the first assignment replaces IFS itself."""
    separator = rng.choice(("", ":", " \t:", "," * 4097 + ":", "é:"))
    text = rng.choice(("one:two::three:", " first\tsecond:third ",
                       "left\\:quoted:right", "aébé:c", "", "tail\\"))
    text += rng.choice(("", "\n"))
    first = rng.choice(("a", "IFS"))
    raw = rng.choice(("", "-r"))
    setup = ("unset IFS" if rng.randrange(5) == 0 else
             "IFS=" + shlex.quote(separator))
    script = ("printf '%s' " + shlex.quote(text) + " > feed\n" +
              "a=old; b=old; c=old\n" + setup + "\n" +
              "read " + raw + " " + first + " b c < feed\ns=$?\n" +
              "printf '%s:<%s>:<%s>:<%s>\\n' \"$s\" \"$" + first +
              "\" \"$b\" \"$c\"\n")
    # dash stops splitting on an invalid multibyte IFS in the C locale; Bash
    # and our byte-oriented reader classify its individual bytes instead.
    modes = ("bash", "posix") if "é" in separator else ALL
    return "builtins-read-ifs-snapshot", modes, script


def builtins_read_limit_state(rng):
    """Bash's byte/count/delimiter options composed over one retained fd."""
    shape = rng.randrange(4)
    attached = bool(rng.getrandbits(1))
    descriptor = rng.choice((3, 9, 10, 11, 31, 62))

    if shape < 2:
        count = rng.choice((0, 1, 2, 3, 7))
        exact = shape == 1
        option = ("-N" if exact else "-n") + str(count)
        if not attached:
            option = ("-N " if exact else "-n ") + str(count)
        text = rng.choice(("ab:cdef", "a\nbcdef", "a\\\nbcdef"))
        script = (
            "printf '%s' " + shlex.quote(text) + " > feed\n"
            "exec 3<feed\n"
            "IFS= read -r -u3 " + option + " first\n"
            "one=$?\n"
            "IFS= read -r -u 3 " + option + " second\n"
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
            "IFS= read -r -u3 " + option + " first\n"
            "one=$?\n"
            "IFS= read -r -u 3 " + option + " second\n"
            "two=$?\n"
            "printf '%s:<%s>:%s:<%s>\\n' \"$one\" \"$first\" \"$two\" \"$second\"\n"
        )
    else:
        # An empty -d operand means NUL; the script carries the escape, not
        # the byte, so it stays a valid command string for every runner.
        option = "-d ''"
        script = (
            "printf 'aa\\0bb\\0cc' > feed\n"
            "exec 3<feed\n"
            "IFS= read -r -u3 " + option + " first\n"
            "one=$?\n"
            "IFS= read -r -u 3 " + option + " second\n"
            "two=$?\n"
            "printf '%s:<%s>:%s:<%s>\\n' \"$one\" \"$first\" \"$two\" \"$second\"\n"
        )

    # Keep 63 out: Bash's private reader occupies it under the runner's fd
    # limit and read -u63 can consume its script buffer instead of the data.
    script = script.replace("exec 3<", "exec " + str(descriptor) + "<")
    script = script.replace("-u3 ", "-u" + str(descriptor) + " ")
    script = script.replace("-u 3 ", "-u " + str(descriptor) + " ")
    return "builtins-read-limit-state", ("bash", "posix"), script


def builtins_read_array_state(rng):
    separator = rng.choice((":", ",", " ", " :"))
    text = rng.choice(("a:b::c", ":a:b:", " a  b c ", "one,two,,four"))
    option = rng.choice(("-a values", "-avalues"))
    script = (
        "printf '%s\\n' " + shlex.quote(text) + " > feed\n"
        "values=(old stale)\n"
        "IFS=" + shlex.quote(separator) + " read -r " + option + " < feed\n"
        "s=$?\nprintf '%s:%s:<%s>:<%s>:<%s>:<%s>\\n' \"$s\" "
        "\"${#values[@]}\" \"${values[0]-}\" \"${values[1]-}\" "
        "\"${values[2]-}\" \"${values[3]-}\"\n"
    )
    return "builtins-read-array-state", ("bash", "posix"), script


def builtins_printf_formats(rng):
    form = rng.choice(("%s", "<%.3s>", "%d", "%u", "%x", "%o", "%b", "%c",
                       "%f", "%g", "%a", "%A", "%08d", "%-6s", "%#x"))
    values = ("", "a b", "007", "0x10", "-1", "+7", " 2", "'A", "bad", "a\\nb", "12tail",
              "1e3", "0x1p2", "-0", ".5", " ", "08")
    operands = rng.sample(values, rng.randrange(1, 4))
    script = ("printf " + shlex.quote(form + "|") + " " +
              " ".join(shlex.quote(word) for word in operands) +
              "\ns=$?\nprintf '\\nstatus:%s\\n' \"$s\"\n")
    # Default Bash on x86 uses an 80-bit long double and so a different legal
    # leading hex digit and exponent; its POSIX mode and dash stay byte-exact,
    # and the round-trip family below checks the value rather than a spelling.
    modes = ("posix", "dash") if form in ("%a", "%A") else ALL
    return "builtins-printf-operands", modes, script


def builtins_printf_hex_roundtrip(rng):
    form = rng.choice(("%a", "%A", "%+.13a", "%#.13A"))
    value = rng.choice(("0", "-0", "1.5", "08", "0x1p2", "-0x1.8p-12",
                        "0x1.fffffffffffffp+20", "0x1p-1022"))
    script = ("encoded=$(printf " + shlex.quote(form) + " " + shlex.quote(value) +
              ")\ns=$?\nprintf 'status:%s:value:%.17g\\n' \"$s\" \"$encoded\"\n")
    return "builtins-printf-hex-roundtrip", ALL, script


def builtins_printf_dynamic_fields(rng):
    """Width/precision argument consumption, signs and format reuse."""
    kind = rng.randrange(4)
    width = rng.choice((-12, -5, 0, 1, 7, 16))
    precision = rng.choice((-3, -1, 0, 1, 4, 9))

    if kind == 0:
        form = "<%*.*s>|"
        values = (str(width), str(precision), rng.choice(("abcdef", "a b c", "")))
    elif kind == 1:
        form = "<%0*.*d>|"
        values = (str(width), str(precision), rng.choice(("-17", "0", "42", "007")))
    elif kind == 2:
        form = "<%#*.*x>|"
        values = (str(width), str(precision), rng.choice(("0", "15", "255", "0x123")))
    else:
        form = "<%*.*b>|"
        values = (str(width), str(precision),
                  rng.choice(("a\\nb", "tab\\there", "", "abcdef")))

    operands = values * rng.choice((1, 2, 3))
    script = ("printf " + shlex.quote(form) + " " +
              " ".join(shlex.quote(word) for word in operands) +
              "\ns=$?\nprintf '\\nstatus:%s\\n' \"$s\"\n")
    return "builtins-printf-dynamic-fields", ALL, script


def builtins_printf_escapes(rng):
    """The escape language echo -e, %b and the format string share."""
    text = rng.choice((r"a\tb", r"x\0101y", r"x\101y", r"x\01012y", r"x\1012y",
                       r"x\1qy", r"a\cb", r"\x41\x4a", r"\x4", r"\xzz",
                       r"esc\e[0m", r"\a\b\f\v\r", "\\\\", r"tail\\",
                       r"\0", r"\08", r"\777"))
    shape = rng.randrange(3)
    if shape == 0:
        script = "printf '%b|' " + shlex.quote(text) + "\necho\n"
        modes = ALL
    elif shape == 1:
        script = "printf " + shlex.quote(text + "|") + "\necho\n"
        modes = ALL
    else:
        script = "echo -e " + shlex.quote(text) + "\n"
        modes = ("bash", "posix")
    return "builtins-printf-escapes", modes, script


def builtins_printf_collectors(rng):
    """The output and %b field stores must never overwrite one another."""
    value = rng.choice(("", "a\\nb", "x\\ty", "\\141\\142", "x\\cy", "tail\\"))
    width, precision = rng.choice((-20, -1, 0, 2, 12)), rng.choice((-1, 0, 1, 7))
    form = "prefix:<%*.*b>:<%s>|"
    operands = [str(width), str(precision), value, "kept"] * rng.randrange(1, 4)
    return "builtins-printf-collectors", ("bash", "posix"), (
        "out=before\nprintf -v out " + shlex.quote(form) + " " +
        " ".join(map(shlex.quote, operands)) +
        "\nprintf 'first:%s:<%s>\\n' \"$?\" \"$out\"\n"
        "printf -v out '%b:%s' 'z\\n' again\nprintf 'next:<%s>\\n' \"$out\"\n")


def builtins_option_walk(rng):
    """Bundled letters, attached operands, --, and the next option word."""
    command = rng.choice(("cd", "pwd", "exec", "shopt", "hash", "enable"))
    if command == "cd":
        words = rng.choice(("-LP dir", "-PL -- dir", "-- dir", "-L -P dir", "-Lz dir"))
        script = "/bin/mkdir -p dir\ncd " + words + "\nprintf 'status:%s\\n' \"$?\"\npwd\n"
    elif command == "pwd":
        words = rng.choice(("-LP", "-PL -- -z", "-- -z", "-L -P", "-Lz"))
        script = "pwd " + words + "\nprintf 'status:%s\\n' \"$?\"\n"
    elif command == "exec":
        words = rng.choice(("-a named", "-anamed", "-la named", "-a first -a second",
                            "-a '' --", "--", "-a", "-az -Q"))
        tail = "" if words == "-a" else " /bin/sh -c 'printf \"argc:%s\\n\" \"$#\"' x y"
        script = "(exec " + words + tail + ")\nprintf 'status:%s\\n' \"$?\"\necho survived\n"
    elif command == "shopt":
        words = rng.choice(("-sq extglob", "-s -q -- extglob", "-uq extglob", "-qz extglob"))
        script = "shopt " + words + "\nprintf 'status:%s\\n' \"$?\"\nshopt -q extglob; echo $?\n"
    elif command == "hash":
        words = rng.choice(("-p /bin/true", "-p/bin/true", "-rp /bin/true",
                            "-p /bin/false -p/bin/true", "-p /bin/true --", "-Q"))
        script = "hash " + words + " named\nprintf 'status:%s\\n' \"$?\"\nhash -t named 2>/dev/null\n"
    else:
        words = rng.choice(("-n true", "-- true", "-n -- true", "-Q true"))
        script = "enable " + words + "\nprintf 'status:%s\\n' \"$?\"\ntype -t true\n"
    return "builtins-option-walk-" + command, ("bash", "posix"), script


def builtins_mapfile_records(rng):
    """Delimiter positions around the short scan and refill fences."""
    length = rng.choice((0, 1, 15, 16, 17, 31, 32, 33, 4095, 4096, 4097))
    delimiter = rng.choice(("\n", ":", "\0"))
    records = ["x" * length, "", "tail"]
    payload = delimiter.join(records) + (delimiter if rng.choice((False, True)) else "")
    encoded = payload.replace("\0", "\\000")
    trim = rng.choice(("", "-t"))
    origin = rng.choice(("", "-O 3"))
    skip = rng.choice((0, 1))
    options = (trim + " " + origin + " -s " + str(skip) + " -d " +
               shlex.quote("" if delimiter == "\0" else delimiter))
    return "builtins-mapfile-records", ("bash", "posix"), (
        "printf %b " + shlex.quote(encoded) + " > records\na=(old old old old old)\n"
        "mapfile " + options + " a < records\n"
        "printf 'status:%s n:%s\\n' \"$?\" \"${#a[@]}\"; printf '<%s>\\n' \"${a[@]}\"\n")


def builtins_getopts_state(rng):
    spec = rng.choice(("ab:", ":ab:", "a:b", ":a:b"))
    words = rng.choice((("-a", "-b", "value", "tail"), ("-abvalue",),
                        ("-z", "-a"), ("-b",), ("--", "-a"), ("operand", "-a")))
    start = rng.choice((1, 1, 2))
    script = ("set -- " + " ".join(shlex.quote(word) for word in words) + "\n" +
              "OPTIND=" + str(start) + "\nn=0\nwhile getopts " +
              shlex.quote(spec) + " option; do\n" +
              "printf '<%s>:<%s>:%s\\n' \"$option\" \"${OPTARG-unset}\" \"$OPTIND\"\n" +
              "n=$((n+1)); [ \"$n\" -lt 8 ] || break\ndone\n" +
              "printf 'end:%s:%s:<%s>\\n' \"$OPTIND\" \"$option\" \"${OPTARG-unset}\"\n")
    return "builtins-getopts-state", ALL, script


def builtins_getopts_reset(rng):
    reset = rng.choice(("OPTIND=0", "OPTIND=1", "OPTIND=2", "unset OPTIND", ":"))
    word = rng.choice(("-ab", "-abc", "-abvalue"))
    spec = rng.choice(("abc", "ab:", ":ab:"))
    script = ("set -- " + word + " -c tail\ngetopts " + spec + " o\n" +
              "printf '%s:%s:<%s>\\n' \"$o\" \"$OPTIND\" \"${OPTARG-unset}\"\n" +
              reset + "\ngetopts " + spec + " o\n" +
              "printf '%s:%s:<%s>\\n' \"$o\" \"$OPTIND\" \"${OPTARG-unset}\"\n")
    return "builtins-getopts-reset", ALL, script


def builtins_getopts_scope(rng):
    declaration = rng.choice(("local OPTIND=1", "local OPTIND=2", "local OPTIND", ":"))
    parameters = rng.choice(("explicit", "function", "set"))
    calls = rng.randrange(1, 4)
    words = rng.choice(("-xy", "-x -y"))
    setup = "set -- " + words + "\n" if parameters == "set" else ""
    operands = " " + words if parameters == "explicit" else ""
    invocation = "f " + words if parameters == "function" else "f"
    script = ("set -- -ab -c\ngetopts abc o\nf() {\n" + declaration + "\n" + setup +
              ("getopts xy o" + operands + "\n") * calls +
              "printf 'in:%s:%s:<%s>\\n' \"$o\" \"$OPTIND\" \"${OPTARG-unset}\"\n}\n" +
              invocation + "\ngetopts abc o\n" +
              "printf 'out:%s:%s:<%s>\\n' \"$o\" \"$OPTIND\" \"${OPTARG-unset}\"\n")
    return "builtins-getopts-scope", ALL, script


# --- value spaces absorbed from shell_variables.sh --------------------------

def builtins_array_machinery(rng):
    """Indexed and associative storage, slices, keys, append and unset."""
    kind = rng.choice(("indexed", "associative"))
    if kind == "indexed":
        setup = rng.choice(("a=(zero one two)", "a=([2]=two [5]=five)",
                            "declare -a a=(x y z)", "a=()", "a=('two words' b)"))
        change = rng.choice(("a+=(tail)", "a[7]=seven", "a[0]+=more",
                             "unset 'a[1]'", "a=(replaced)", "a[-1]=last",
                             "declare -p a", ":"))
        query = rng.choice(('"${a[@]}"', '"${a[*]}"', '"${!a[@]}"', '"${#a[@]}"',
                            '"${a[@]:1:2}"', '"${a[-1]-none}"', '"${a[0]-none}"',
                            '"${#a[0]}"'))
    else:
        setup = rng.choice(("declare -A a=([k]=one [j]=two)",
                            "declare -A a=()",
                            "declare -A a; a['a b']=spaced; a['x=y']=equals"))
        # No bare declare -p here: the sorted report below carries it, and an
        # associative listing has no defined order to compare unsorted.
        change = rng.choice(("a[new]=added", "a[k]+=more", "unset 'a[k]'",
                             "a=([only]=one)", ":"))
        query = rng.choice(('"${a[@]}"', '"${!a[@]}"', '"${#a[@]}"',
                            '"${a[k]-none}"', '"${a[missing]-none}"'))
    # An associative array has no defined order, so its keys and its
    # declare -p are sorted before comparison; an indexed one is ordered.
    report = ("declare -p a 2>/dev/null\n" if kind == "indexed" else
              "declare -p a 2>/dev/null | /usr/bin/tr ' ' '\\n' | /usr/bin/sort\n")
    script = (setup + "\n" + change + "\n"
              "printf '<%s>' " + query + " | /usr/bin/tr ' ' '\\n' | /usr/bin/sort | /usr/bin/tr '\\n' ' '\necho\n"
              + report)
    return "builtins-array-machinery", ("bash", "posix"), script


def builtins_nameref(rng):
    """declare -n against scalars, elements and whole arrays."""
    body = rng.choice((
        "a=(zero one); declare -n n=a; n[2]=two; declare -p a n",
        "a=([2]=x); declare -n n=a; printf '<%s>:%s:<%s>\\n' \"${n[2]}\" \"${#n[@]}\" \"${!n[@]}\"",
        "declare -A m=([x]=a); declare -n n=m; n[y]=b; printf '<%s>|<%s>:%s\\n' \"${n[x]}\" \"${n[y]}\" \"${#m[@]}\"",
        "a=([2]=x [4]=y); declare -n n=a; unset 'n[2]'; declare -p a",
        "a=([2]=x); declare -n n=a; unset -n n; declare -p a; declare -p n 2>/dev/null; echo $?",
        "a=([2]=x); declare -n n=a; n=(y z); declare -p a",
        "a=([2]=x); declare -n n=a; n+=(y z); declare -p a",
        "a=old; declare -rn n=a; n=new; printf '%s:%s:%s\\n' \"$?\" \"$a\" \"$n\"",
        "readonly a=old; declare -n n=a; n=new 2>/dev/null; printf 'after:%s\\n' \"$?\"",
        "a=([2]=x); n='a[2]'; declare -n n; printf '<%s>\\n' \"$n\"",
        "a=([2]=x); n='a[2]'; declare -n n; n=y; declare -p a",
        "a=([2]=x); n='a[2]'; declare -n n; n+=y; declare -p a",
        "a=([2]=x [4]=z); n='a[2]'; declare -n n; unset n; declare -p a",
        "a=([2]=x [3]=y); i=2; n='a[i]'; declare -n n; printf '<%s>' \"$n\"; i=3; printf ':<%s>\\n' \"$n\"",
        "j=0; i='j++'; declare -n n='a[i]'; n=x; printf '%s:<%s>:<%s>\\n' \"$j\" \"${a[0]-}\" \"${a[1]-}\"",
        "declare -A m=([x]=old); n='m[x]'; declare -n n; n=new; declare -p m",
        "a=x; b=y; declare -n n=a; declare -n n=b; declare -p n; printf '%s:%s\\n' \"$a\" \"$b\"",
        "f() { local a=x b=y; local -n n=a; local -n n=b; declare -p n; }; f",
        "declare -n a=b; declare -n b=a; a[2]=x 2>/dev/null; printf 'status:%s\\n' \"$?\"",
        "n='n[2]'; declare -n n; n=x 2>/dev/null; printf 'after:%s\\n' \"$?\"",
        "a=old; declare -n n=a; export n; declare -p a n",
        "a=old; declare -n n=a; readonly n; declare -p a n",
        "a=old; declare -n n=a; declare -xn n; declare -p a n",
        "a=old; declare -n n=a; declare -rn n; declare -p a n",
        "target=Value; declare -i ref=7; declare -n ref=target; printf '%s:%s\\n' \"$?\" \"$ref\"; declare -p ref",
        "ref=old; declare -iln ref=bad-name 2>/dev/null; printf '%s\\n' \"$?\"; declare -p ref",
        "v=orig; f() { local -n ref=$1; ref=changed; }; f v; printf '%s\\n' \"$v\"",
    ))
    return "builtins-nameref", ("bash", "posix"), body + "\n"


def builtins_scope_snapshot(rng):
    """A prefix assignment's snapshot of an array, and what a call restores."""
    kind = rng.choice(("indexed", "associative"))
    if kind == "indexed":
        initial = "a=([0]=zero [2]='two words' [31]='v=31' [500]='')"
        key, removed = "2", "31"
    else:
        initial = "declare -A a=([0]=zero [red]='two words' [green]='v=31' [500]='')"
        key, removed = "red", "green"
    change = rng.choice(("a[" + key + "]=changed", "a[" + key + "]+=tail",
                         "unset 'a[" + removed + "]'", "a=()", "unset a",
                         "a[700]=added", "readonly a"))
    script = (initial + "\nf() { " + change + "; }\na=prefix f\n"
              "printf '<%s>:<%s>:<%s>:<%s>:<%s>\\n' \"${#a[@]}\" \"${a[700]-}\" "
              "\"${a[0]-}\" \"${a[" + key + "]-}\" \"${a[" + removed + "]-}\"\n")
    return "builtins-scope-snapshot", ("bash", "posix"), script


def builtins_readonly_scope(rng):
    """readonly and local -r across a call, and what a failed write leaves."""
    body = rng.choice((
        "x=global; f() { declare -r x=local; declare -p x; }; f; x=after; printf '%s\\n' \"$x\"",
        "x=global; f() { local -r x=local; declare -p x; }; f; x=after; printf '%s\\n' \"$x\"",
        "x=global; inner() { printf '<%s>\\n' \"$x\"; }; outer() { local -r x=outer; inner; declare -p x; }; outer",
        "x=global; f() { local x=local; readonly x; declare -p x; }; f; x=after; printf '<%s>\\n' \"$x\"",
        "f() { declare -r absent; declare -p absent; }; f; declare -p absent 2>/dev/null; printf '%s\\n' \"$?\"",
        "x=outer; (f() { local x=inner; readonly x; x=bad; echo no; }; f) 2>/dev/null; printf '%s:%s\\n' \"$?\" \"$x\"",
        "readonly x=global; f() { local x=local; printf '%s:<%s>\\n' \"$?\" \"$x\"; }; f 2>/dev/null",
        "x=outer; f() { local x=inner; unset x; printf '[%s]' \"${x-gone}\"; }; f; printf '|%s|\\n' \"$x\"",
        "r=old; f() { local -r r=keep; printf new | read r 2>/dev/null; printf '%s:%s\\n' \"$?\" \"$r\"; }; f",
        "i=0; a[i=1]=one; a[i+=1]+=two; a[i==2?3:4]=three; printf '%s:%s:%s:%s\\n' \"$i\" \"${a[1]}\" \"${a[2]}\" \"${a[3]}\"",
        "declare -A a; a['x=y']=one; a['x=y']+=two; a['[+]=z']=three; printf '%s:%s:%s\\n' \"${a[x=y]}\" \"${a[[+]=z]}\" \"${#a[@]}\"",
        "X=old; f() { readonly X; }; X=new f; declare -p X; X=x 2>/dev/null; printf 'status:%s\\n' \"$?\"",
    ))
    return "builtins-readonly-scope", ("bash", "posix"), body + "\n"


def builtins_pipestatus(rng):
    """PIPESTATUS is published lazily and survives copies and resets."""
    body = rng.choice((
        "printf '<%s>:%s\\n' \"${PIPESTATUS[*]}\" \"${#PIPESTATUS[@]}\"",
        "false; printf '<%s>:%s\\n' \"${PIPESTATUS[*]}\" \"${#PIPESTATUS[@]}\"",
        "true | false | true; printf '%s:<%s>:<%s>:<%s>\\n' \"${#PIPESTATUS[@]}\" \"${PIPESTATUS[0]}\" \"${PIPESTATUS[1]}\" \"${PIPESTATUS[2]}\"",
        "false | true; copy=(\"${PIPESTATUS[@]}\"); printf '<%s>|<%s>\\n' \"${copy[*]}\" \"${PIPESTATUS[*]}\"",
        "PIPESTATUS=(7 8); printf '%s:<%s>\\n' \"${#PIPESTATUS[@]}\" \"${PIPESTATUS[*]}\"",
        "false | true; copy=${PIPESTATUS[*]}; printf '<%s>|<%s>\\n' \"$copy\" \"${PIPESTATUS[*]}\"",
        "false; eval 'printf \"<%s>\\n\" \"${PIPESTATUS[*]}\"'",
        "unset PIPESTATUS; printf '%s:<%s>\\n' \"${#PIPESTATUS[@]}\" \"${PIPESTATUS[*]}\"",
        "readonly PIPESTATUS; false; declare -p PIPESTATUS",
        "false; readonly PIPESTATUS; declare -p PIPESTATUS",
        ":; declare -p PIPESTATUS",
        ":; set | /bin/grep -q '^PIPESTATUS='; printf '%s\\n' \"$?\"",
        "PIPESTATUS[4]=9; declare -p PIPESTATUS",
        "PIPESTATUS[4]=9; false; declare -p PIPESTATUS",
        "PIPESTATUS[4]=9; false | true; declare -p PIPESTATUS",
        "PIPESTATUS[1]=9; false | true; declare -p PIPESTATUS",
        "false | true; PIPESTATUS[4]=9; declare -p PIPESTATUS",
    ))
    return "builtins-pipestatus", ("bash", "posix"), body + "\n"


def builtins_dynamic_variables(rng):
    """The variables the shell computes: compared by property, not by value."""
    body = rng.choice((
        "first=$RANDOM; second=$RANDOM; [ \"$first\" != \"$second\" ]; printf 'varies:%s\\n' \"$?\"",
        "[ \"$RANDOM\" -ge 0 ] && [ \"$RANDOM\" -le 32767 ]; printf 'ranged:%s\\n' \"$?\"",
        "printf 'seconds:%s\\n' \"$((SECONDS >= 0 && SECONDS < 5))\"",
        "SECONDS=100; printf 'assigned:%s\\n' \"$((SECONDS >= 100 && SECONDS < 105))\"",
        "[ \"$EPOCHSECONDS\" -gt 1000000000 ]; printf 'epoch:%s\\n' \"$?\"",
        "case $EPOCHREALTIME in *.*) echo fractional;; *) echo plain;; esac",
        "[ \"$BASHPID\" = \"$$\" ]; printf 'samepid:%s\\n' \"$?\"",
        "( [ \"$BASHPID\" != \"$$\" ]; printf 'subshell:%s\\n' \"$?\" )",
        "printf 'lineno:%s\\n' \"$LINENO\"\nprintf 'lineno:%s\\n' \"$LINENO\"",
        "f() { printf 'funcname:%s\\n' \"$FUNCNAME\"; }; f",
        "f() { printf 'depth:%s\\n' \"${#FUNCNAME[@]}\"; }; g() { f; }; g",
        "printf 'dollar:%s\\n' \"$(( $$ > 1 ))\"",
        "printf 'flags:%s\\n' \"$-\"",
        "set -u; printf 'flags:%s\\n' \"$-\"",
        "printf 'opterr:%s\\n' \"${OPTERR-unset}\"",
        "printf 'optind:%s\\n' \"$OPTIND\"",
        "printf 'uid:%s\\n' \"$(( UID == EUID ))\"",
        "[ -n \"$BASH_VERSION\" ]; printf 'version:%s\\n' \"$?\"",
        "printf 'versinfo:%s\\n' \"${#BASH_VERSINFO[@]}\"",
        "printf 'subshell:%s\\n' \"$BASH_SUBSHELL\"; ( printf 'inner:%s\\n' \"$BASH_SUBSHELL\" )",
        "printf 'hostype:%s\\n' \"$(( ${#HOSTTYPE} > 0 ))\"",
        "printf 'ppid:%s\\n' \"$(( PPID > 0 ))\"",
        "printf 'shlvl:%s\\n' \"$(( SHLVL >= 1 ))\"",
    ))
    return "builtins-dynamic-variables", ("bash", "posix"), body + "\n"


def builtins_umask_symbolic(rng):
    """Symbolic umask clauses, each read back both ways."""
    start = rng.choice(("0", "022", "0777", "0177", "0077", "0027"))
    mode = rng.choice(("u=rwx,g=rx,o=", "a-w", "u+r,go=", "=rx", "g=u,o+g",
                       "g+X", "a+X", "u+w+r", "u=r=w", "a=", "u-r", "og+w",
                       "u+s", "a+rwx", "u=,g=,o=", "0", "755", "0644"))
    script = ("umask " + start + "\numask " + mode +
              "\nprintf 'status:%s\\n' \"$?\"\numask\numask -S\n")
    return "builtins-umask-symbolic", ALL, script


def builtins_test_forms(rng):
    """POSIX resolves test by counting words before it looks at them."""
    body = rng.choice((
        "test x; echo $?", "test ''; echo $?",
        "test ! ''; echo $?; test ! x; echo $?",
        "test = = =; echo $?", "test ! = x; echo $?",
        "test '(' '' ')'; echo $?; test '(' x ')'; echo $?",
        "test ! x = x; echo $?", "test '(' ! '' ')'; echo $?",
        "test x -a y; echo $?; test '' -o y; echo $?",
        "[ x = x ]; echo $?", "[ ! x ]; echo $?",
        "test -n x -a -z ''; echo $?",
        "test 1 -lt 2 -a 2 -lt 3; echo $?",
        "[ -n a -o -n b ]; echo $?",
        "test '(' x -a y ')' -o z; echo $?",
        "test; echo $?", "test '' ; echo $?",
        "[ ]; echo $?", "[ '' ]; echo $?",
        "test a b c d e 2>/dev/null; echo $?",
        "test -z; echo $?", "test -n; echo $?",
    ))
    return "builtins-test-forms", ALL, body + "\n"




def builtins_allexport(rng):
    """set -a: which builtins' assignments reach a child's environment."""
    body = rng.choice((
        "unset READV; set -a; printf 'value\\n' | { read READV; /bin/sh -c 'echo \"$READV\"'; }",
        "unset OPTION OPTARG; OPTIND=1; set -a; set -- -x value; getopts x: OPTION; "
        "/bin/sh -c 'echo \"$OPTION:$OPTARG:$OPTIND\"'",
        "/bin/mkdir -p target; unset PWD OLDPWD; set -a; cd target; "
        "/bin/sh -c 'echo \"${PWD##*/}:${OLDPWD##*/}\"'",
        "unset FIXED; set -a; readonly FIXED=value; /bin/sh -c 'echo \"$FIXED\"'",
        "unset V; set -a; V=plain; /bin/sh -c 'echo \"$V\"'",
        "unset V; set -a; set +a; V=plain; /bin/sh -c 'echo \"${V-unset}\"'",
        "unset V; set -a; unset V; V=late; /bin/sh -c 'echo \"$V\"'",
    ))
    return "builtins-allexport", ALL, body + "\n"


def builtins_history_file(rng):
    """The history store as files: read, write, append, add and clear."""
    total = rng.choice((0, 1, 3, 7))
    keep = rng.choice((0, 1, 2, 7, 100))
    steps = rng.choice((
        "history -r initial; history",
        "history -r initial; history -w saved; /bin/cat saved",
        "history -r initial; history -a appended; /bin/cat appended",
        "history -r initial; history -s added; history",
        "history -r initial; history -c; history; printf 'cleared:%s\\n' \"$?\"",
        "history -r initial; history -d 1 2>/dev/null; history",
        "history -r initial; history -n initial; history",
        "history -r missing12345 2>/dev/null; printf 'missing:%s\\n' \"$?\"",
    ))
    make = ("/usr/bin/awk 'BEGIN { for (i = 1; i <= " + str(total) +
            "; i++) print \"old-\" i }' > initial\n")
    script = ("set -o history 2>/dev/null\nHISTFILE=$PWD/hist\nHISTSIZE=" +
              str(keep) + "\n" + make + "history -c\n" + steps + "\n")
    return "builtins-history-file", ("bash", "posix"), script


FAMILIES = (
    builtins_listing, builtins_query_namespaces, builtins_declaration_lifecycle,
    builtins_inventory_state, builtins_read_fields, builtins_read_ifs_snapshot,
    builtins_read_limit_state, builtins_read_array_state, builtins_printf_formats,
    builtins_printf_hex_roundtrip, builtins_printf_dynamic_fields,
    builtins_printf_escapes, builtins_printf_collectors, builtins_option_walk,
    builtins_mapfile_records, builtins_getopts_state, builtins_getopts_reset,
    builtins_getopts_scope, builtins_array_machinery, builtins_nameref,
    builtins_scope_snapshot, builtins_readonly_scope, builtins_pipestatus,
    builtins_dynamic_variables, builtins_umask_symbolic, builtins_test_forms,
    builtins_allexport, builtins_history_file,
)
