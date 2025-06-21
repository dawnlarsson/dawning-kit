#!/bin/sh

. ../bit/emit.sh

program() {
        copy
        reg_0
        bit_32 60

        copy
        reg_5
        bit_32 0

        syscall
}

echo
hex_dump program

elf program program

echo
./program

echo $?
