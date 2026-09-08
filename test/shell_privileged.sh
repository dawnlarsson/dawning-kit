#!/bin/sh
# Bash privileged mode is tested only in disposable children with synthetic
# real/effective IDs. No test changes the credentials of this driver process.
set -u

subject=${1:-/tmp/mwsh}
[ -x "$subject" ] || exit 1
[ "$(uname -s)" = Linux ] || {
        echo "  shell_privileged NOT RUN -- Linux credentials required" >&2
        exit 2
}
[ -x /bin/bash ] || {
        echo "  shell_privileged NOT RUN -- no /bin/bash" >&2
        exit 2
}
command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1 || {
        echo "  shell_privileged NOT RUN -- passwordless sudo required" >&2
        exit 2
}

case $subject in
/*) ;;
*) subject=$(CDPATH= cd -- "$(dirname -- "$subject")" && pwd)/$(basename -- "$subject") ;;
esac

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT INT TERM
chmod 755 "$work"
mkdir "$work/names" "$work/home" "$work/search" "$work/glob"
mkdir "$work/search/place"
touch "$work/glob/one" "$work/glob/two"
ln -s "$subject" "$work/names/bash"

compiler=${CC:-cc}
"$compiler" -O2 -Wall -Wextra -x c -o "$work/credential-child" - <<'EOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
        uid_t uid[3];
        gid_t gid[3];
        int saved_only = argc > 1 && !strcmp(argv[1], "--saved");

        if (argc == 2 && !strcmp(argv[1], "--show"))
        {
                if (getresuid(uid, uid + 1, uid + 2) ||
                    getresgid(gid, gid + 1, gid + 2))
                        return 96;
                printf("uid=%u/%u/%u gid=%u/%u/%u\n",
                       (unsigned)uid[0], (unsigned)uid[1], (unsigned)uid[2],
                       (unsigned)gid[0], (unsigned)gid[1], (unsigned)gid[2]);
                return 0;
        }

        if (saved_only)
        {
                argv++;
                argc--;
        }
        if (argc < 2)
                return 99;
        if (setresgid(65534, saved_only ? 65534 : 0, 0) ||
            setresuid(65534, saved_only ? 65534 : 0, 0))
                return 98;
        execv(argv[1], argv + 1);
        return 97;
}
EOF
[ -x "$work/credential-child" ] || exit 1
chmod 755 "$work/credential-child"

printf '%s\n' 'printf "startup-forbidden\n"' > "$work/startup"
chmod 644 "$work/startup"

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$test_dir/tally.sh"

capture()
{
        identity=$1 shell=$2 tag=$3 startup=$4
        shift 4

        if [ "$identity" = mismatch ]; then
                runner="sudo -n $work/credential-child"
        elif [ "$identity" = saved ]; then
                runner="sudo -n $work/credential-child --saved"
        else
                runner=
        fi

        if [ "${special_environment:-0}" = 1 ]; then
                set -- env -i HOME="$work/home" ROOT="$work" \
                        BASH_ENV="$startup" CDPATH="$work/search" \
                        GLOBIGNORE='*' SHELLOPTS='errexit:noglob:xtrace' \
                        BASHOPTS='nullglob' \
                        'BASH_FUNC_poison%%=() { echo POISON; }' \
                        PATH=/usr/bin:/bin LC_ALL=C "$shell" "$@"
        else
                set -- env -i HOME="$work/home" ROOT="$work" \
                        BASH_ENV="$startup" CDPATH="$work/search" \
                        PATH=/usr/bin:/bin LC_ALL=C "$shell" "$@"
        fi

        # shellcheck disable=SC2086
        if timeout 5 $runner "$@" > "$work/$tag.out" \
                2> "$work/$tag.err"; then
                status=0
        else
                status=$?
        fi
        printf '%s\n' "$status" > "$work/$tag.status"
}

compare()
{
        name=$1 identity=$2 startup=$3
        shift 3

        capture "$identity" /bin/bash want "$startup" "$@"
        capture "$identity" "$work/names/bash" got "$startup" "$@"

        if cmp -s "$work/want.out" "$work/got.out" &&
                cmp -s "$work/want.status" "$work/got.status"; then
                won
        else
                lost "$name" \
                        "want $(tr '\n' '|' < "$work/want.out")[$(cat "$work/want.status")], got $(tr '\n' '|' < "$work/got.out")[$(cat "$work/got.status")]"
        fi
}

section privileged
group equal_ids
compare 'letter and named state' plain /dev/null -c \
        'set -p; printf "%s:" "$-"; set +o privileged; printf "%s:" "$-"; set -o privileged; printf "%s\n" "$-"'
compare 'startup letter state' plain /dev/null -p -c 'printf "%s\n" "$-"'
compare 'startup named state' plain /dev/null -o privileged -c 'printf "%s\n" "$-"'
compare 'startup suppresses BASH_ENV' plain "$work/startup" -p -c 'printf "body\n"'
compare 'final startup unset reads BASH_ENV' plain "$work/startup" -p +p -c 'printf "body\n"'
compare 'final startup set suppresses BASH_ENV' plain "$work/startup" +p -p -c 'printf "body\n"'

group inherited_environment
compare 'privileged ignores CDPATH' plain /dev/null -p -c \
        'cd place 2>/dev/null; printf "%s:%s\n" "$?" "${PWD##*/}"'
compare 'privileged ignores assigned CDPATH' plain /dev/null -p -c \
        'CDPATH="$ROOT/search"; cd place 2>/dev/null; printf "%s:%s\n" "$?" "${PWD##*/}"'
compare 'runtime unset enables CDPATH' plain /dev/null -p -c \
        'set +p; cd place 2>/dev/null; printf "%s:%s\n" "$?" "${PWD##*/}"'
special_environment=1
compare 'special imports have no behavior' plain /dev/null -p -c \
        'printf "visible:%s:%s\n" "$CDPATH" "$GLOBIGNORE"; case $- in *e*|*f*|*x*) echo option-imported;; *) echo options-ignored;; esac; if shopt -q nullglob; then echo shopt-imported; else echo shopt-ignored; fi; cd "$ROOT/glob"; printf "glob:<%s>\n" * no-match-*; if poison 2>/dev/null; then echo function-imported; else echo function-ignored; fi'
special_environment=0

group unequal_ids
show='$ROOT/credential-child --show'
compare 'default drops credentials' mismatch /dev/null -c "$show"
compare 'letter preserves credentials' mismatch /dev/null -p -c \
        "$show; printf \"%s\\n\" \"\$-\""
compare 'named preserves credentials' mismatch /dev/null -o privileged -c "$show"
compare 'runtime unset drops credentials' mismatch /dev/null -p -c \
        "$show; set +p; $show; printf \"%s\\n\" \"\$-\""
compare 'runtime named unset drops' mismatch /dev/null -p -c \
        "set +o privileged; $show"
compare 'dropped identity cannot return' mismatch /dev/null -c \
        "set -p; $show; printf \"%s\\n\" \"\$-\""
compare 'startup plus then minus stays dropped' mismatch /dev/null +p -p -c \
        "$show; printf \"%s\\n\" \"\$-\""
compare 'startup minus then plus drops' mismatch /dev/null -p +p -c \
        "$show; printf \"%s\\n\" \"\$-\""
compare 'default suppresses BASH_ENV' mismatch "$work/startup" -c 'printf "body\n"'
compare 'preserve suppresses BASH_ENV' mismatch "$work/startup" -p -c 'printf "body\n"'
compare 'preserve ignores CDPATH' mismatch /dev/null -p -c \
        'cd place 2>/dev/null; printf "%s:%s\n" "$?" "${PWD##*/}"'
compare 'drop enables CDPATH' mismatch /dev/null -p -c \
        'set +p; cd place 2>/dev/null; printf "%s:%s\n" "$?" "${PWD##*/}"'

group saved_ids
compare 'ordinary startup retains saved ID' saved /dev/null -c "$show"
compare 'runtime unset discards saved ID' saved /dev/null -p -c \
        "set +p; $show"
compare 'direct unset discards saved ID' saved /dev/null -c \
        "set +p; $show"
compare 'startup unset discards saved ID' saved /dev/null +p -c "$show"

section ""
printf '  %-12s %s of %s\n' total "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
