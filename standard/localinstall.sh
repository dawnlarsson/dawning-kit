#!/bin/sh

# Installs a symlink to this install of /standard
# a symlink is created at the root of the file system

dir=$(pwd)
curdir=$(basename "$dir")

if [ "$curdir" != "standard" ]; then
        cd standard || {
                echo "This script must be run from the standard directory"
                exit 1
        }
fi

ln -s "$(pwd)" /standard