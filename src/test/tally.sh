#!/bin/sh

# Shared section and failure accounting for the shell-driven suites.
pass=0
fail=0
current=""
section_name=""
section_pass=0
section_total=0

: "${test_group_width:=14}"
: "${test_case_width:=26}"

section()
{
        [ -z "$section_name" ] ||
                printf '  %-12s %s of %s\n' \
                        "$section_name" "$section_pass" "$section_total"

        [ -z "$section_name" ] || [ -z "${TEST_TALLY:-}" ] ||
                printf '%s %s %s\n' \
                        "$section_name" "$section_pass" "$section_total" \
                        >> "$TEST_TALLY"

        section_name=${1:-}
        section_pass=0
        section_total=0
}

group() { current=$1; }

won()
{
        pass=$((pass + 1))
        section_pass=$((section_pass + 1))
        section_total=$((section_total + 1))
}

lost()
{
        fail=$((fail + 1))
        section_total=$((section_total + 1))
        printf '  %-*s %-*s %s\n' \
                "$test_group_width" "$current" \
                "$test_case_width" "$1" "$2"
}
