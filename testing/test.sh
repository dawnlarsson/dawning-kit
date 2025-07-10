#!/bin/sh

. ../bit/emit.sh

sys_exit() {
        copy
        reg_0
        bit_32 60

        copy
        reg_5
        bit_32 $1

        syscall
}

example() {

        sys_exit 0

        echo "Hello World\n"
        bit_8 0x0
}

echo
hex_dump example

elf program example

echo
./program

echo $?
