#!/bin/sh

# Comparisons shared by the shell-language suites. Each lane supplies
# run_both, shown, won and lost so its execution setup and display stay local.
shell_compare_bash_begin()
{
        held_reference=$reference
        held_subject=$subject
        reference=/bin/bash

        if [ -z "${bash_subject:-}" ]; then
                case $subject in
                /*) bash_target=$subject ;;
                *) bash_target=$(CDPATH= cd -- "$(dirname -- "$subject")" && pwd)/$(basename -- "$subject") ;;
                esac
                bash_subject=$work/bash
                ln -s "$bash_target" "$bash_subject" || return 1
        fi
        subject=$bash_subject
}

shell_compare_bash_end()
{
        reference=$held_reference
        subject=$held_subject
}

check()
{
        name=$1
        shift
        run_both "$@"

        if cmp -s "$work/want" "$work/got"; then
                won
                return 0
        fi

        lost "$name" "want $(shown "$work/want")   got $(shown "$work/got")"
}

answer()
{
        name=$1
        shift
        run_both "$@"

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                won
                return 0
        fi

        lost "$name" \
                "want $(shown "$work/want")[$want_status]   got $(shown "$work/got")[$got_status]"
}

bash_answer()
{
        name=$1
        shift

        [ -x /bin/bash ] || {
                lost "$name" "/bin/bash is required for a Bash extension case"
                return 0
        }

        shell_compare_bash_begin || return 1
        run_both "$@"
        shell_compare_bash_end

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                won
                return 0
        fi

        lost "$name" \
                "bash $(shown "$work/want")[$want_status]   got $(shown "$work/got")[$got_status]"
}

# The recorded answer is an intentional difference from dash. Both halves
# are checked so an implementation change cannot silently become a third one.
differs()
{
        name=$1
        recorded=$2
        recorded_status=$3
        shift 3
        run_both "$@"

        got_ours=$(shown "$work/got")

        if [ "$got_ours" != "$recorded" ] ||
                [ "$got_status" != "$recorded_status" ]; then
                lost "$name" \
                        "recorded ${recorded}[$recorded_status]   now ${got_ours}[$got_status]"
                return 0
        fi

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                lost "$name" "agrees with dash now -- move it into answer"
                return 0
        fi

        won
}
