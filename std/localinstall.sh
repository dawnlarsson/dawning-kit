#!/bin/sh
#
#       Installs a symlink to this checkout of /standard at the filesystem root,
#       which is what the dev and run scripts look for.
#
set -u

link=/standard

if [ "$(basename "$(pwd)")" != "standard" ]; then
        cd standard 2>/dev/null || {
                echo "localinstall: run this from the standard directory" >&2
                exit 1
        }
fi

target=$(pwd)

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
