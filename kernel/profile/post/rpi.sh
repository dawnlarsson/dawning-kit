#!/bin/sh
set -e
. kit/common

source="https://github.com/raspberrypi/firmware/raw/master"

label $GREEN"Raspberry Pi Post build setup"

mkdir -p artifacts/pi

for firmware in bootcode.bin start.elf fixup.dat; do
        cached="artifacts/pi/$firmware"
        if ! is_file "$cached"; then
                wget -q -O "$cached.part" "$source/boot/$firmware"
                mv "$cached.part" "$cached"
        fi
done

mkdir -p dist/boot

cp artifacts/pi/* dist/boot/
