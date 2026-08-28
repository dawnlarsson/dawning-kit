#!/bin/sh
#
#       Cases for the term lane.
#
#       Every case runs the same input through the reference the case names
#       and through ours, and compares. Agreeing with the reference is what
#       passing means; there is no separate idea here of the right answer.
#
set -e

farm=${1:-/tmp/mwfarm}
pass=0
fail=0

printf '\n  %s of %s\n' "$pass" "$((pass + fail))"
