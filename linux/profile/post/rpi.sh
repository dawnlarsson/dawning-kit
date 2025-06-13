#!/bin/sh
source script/common

source="https://github.com/raspberrypi/firmware/raw/master/"

label $GREEN"Raspberry Pi Post build setup"

mkdir -p artifacts/pi

is_file artifacts/pi/bootcode.bin ||
        wget -q -O artifacts/pi/bootcode.bin $source/boot/bootcode.bin

is_file artifacts/pi/start.elf ||
        wget -q -O artifacts/pi/start.elf $source/boot/start.elf

is_file artifacts/pi/fixup.dat ||
        wget -q -O artifacts/pi/fixup.dat $source/boot/fixup.dat

mkdir -p dist/boot

cp artifacts/pi/* dist/boot/
