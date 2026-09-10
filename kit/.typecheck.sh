#!/bin/sh
#       Type-check the kernel side without a kernel build.
#
#       Replays the command kbuild recorded for core.o with -fsyntax-only, so
#       an edit to core.c or anything under canvas/ is checked against the
#       real kernel headers and the real config in about a second, instead of
#       waiting for a full image build to say the same thing.
#
#       Dropped from the recorded command: the dependency file and the objtool
#       step, both of which write where the real build wrote them as root.
set -e
cd "$HOME/dawning-tidyclean/linux"
cmd=$(sed -n 's/^savedcmd_kernel\/moonwater\/core\.o := //p' kernel/moonwater/.core.o.cmd)
cmd=${cmd%%;*}
cmd=$(printf '%s' "$cmd" \
      | sed 's/ -Wp,-MMD,kernel\/moonwater\/\.core\.o\.d//' \
      | sed 's/ -o kernel\/moonwater\/core\.o / -fsyntax-only /; s/ -c / /')
eval "$cmd"
