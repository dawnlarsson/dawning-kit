![Dawning DevKit](https://github.com/user-attachments/assets/92b3ab15-8512-4874-bd94-f6b508de8c61)

Foundational Software Development Kit. Zero dependency: C standard Library, Cross Architecture Assembler.

## Overview
- Bit Kit: `/bit` Provides foundational primitives for code generation in a bare bones UNIX environment.
- C Standard: `/standard` Entirely self-contained C standard library, also pioneering new syntax and clearer semantics.
- Test Kit: `/test` Testing utilities, cross architecture

## Bit Kit

Foundational primitives for code generation, provides "ring zero" level utils for building executables,
it's all in shell so portability isn't a concern, and is transparent for your most critical path, where opaque and complex 3rd party binaries might be a concern.
This aims for being ideal for boot strapping toolchains from nothing, a full "compiled yourself down to the last byte"

Usage: 
```sh
source bit/kit.sh
```

### Primitives:

#### endianness flag
respects endianness, before running any of the primitives you can set big endianness
then unset `BIG_ENDIAN=0` to go back to the default little endian target.
```sh
BIG_ENDIAN=1
```

#### Bytes
Byte functions take any amount of args, each arg is separately represented in the functions size,
you can input hex (0x7f) or chars ( ELF -> "E", "L" "F" ), or plain ints

`bit__8` `bit_16` `bit_32` `bit_64` `bit_128` 

### Elf Executable format
Generates a ELF executable header and outputs a working executable

```sh
exit_program() {
        bit__8 0x48, 0xc7, 0xc0, 0x3c, 0x00, 0x00, 0x00 # mov $60, %rax
        bit__8 0x48, 0xc7, 0xc7, 0x00, 0x00, 0x00, 0x00 # mov $0, %rdi
        bit__8 0x0f, 0x05 # x86_64 linux syscall
}

elf bin/program exit_program
```

### Support
Did you know this effort has gone 100% out of my pocket?
If you think this project speaks for itself, consider supporting on github sponsors to continue making
projects like these a reality, open & free.

Supporter or not, you can **always** reach me on <a href="https://discord.gg/cxRvzUyzG8">My Discord Server, my primary communication channel</a>
Questions, feedback or support related to any of my projects, or if you need consulting.

### License
Logos, Branding, Trademarks - Copyright Dawn Larsson 2022

Repository:
Apache-2.0 license 