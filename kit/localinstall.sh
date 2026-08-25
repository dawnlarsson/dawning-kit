#!/bin/sh
#
#       Installs a symlink to this checkout's src/ at /standard, which is what
#       the dev and run scripts look for and what a C file including
#       </standard/library.c> from outside the tree resolves through.
#
#       The link keeps its old name. It is a path other people's files may
#       already have written down, and renaming it would break those without
#       buying anything.
#
set -u

link=/standard

# shellcheck disable=SC1007
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

target="$here/../src"
# shellcheck disable=SC1007
target=$(CDPATH= cd -- "$target" 2>/dev/null && pwd) || {
        echo "localinstall: cannot find src/ next to $here" >&2
        exit 1
}

if [ -L "$link" ]; then
        current=$(readlink "$link")
        if [ "$current" = "$target" ]; then
                echo "localinstall: $link already points here, nothing to do"
                exit 0
        fi
        echo "localinstall: $link currently points at $current" >&2
        echo "localinstall: remove it first with: sudo rm $link" >&2
        exit 1
fi

if [ -e "$link" ]; then
        echo "localinstall: $link exists and is not a symlink, refusing to touch it" >&2
        exit 1
fi

# Writing to / needs root on every platform, and is blocked outright by SIP on
# macOS. Say so rather than failing with a bare 'Permission denied'.
if ! ln -s "$target" "$link" 2>/dev/null; then
        echo "localinstall: could not create $link" >&2
        echo "localinstall: try  sudo ln -s \"$target\" $link" >&2
        echo "localinstall: on macOS the root directory is protected by SIP and this will not work;" >&2
        echo "localinstall: set KIT_DIR or call the scripts by path instead." >&2
        exit 1
fi

echo "localinstall: $link -> $target"
