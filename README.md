![Dawning Kit](https://github.com/user-attachments/assets/ad148eba-423f-4bc5-9d32-1e9005c8ebb7)
<div align="center">
  <a href="https://discord.gg/cxRvzUyzG8">Discord<a>
  —
  <a href="https://kit.dawning.dev">Docs<a>
</div>
<br>
Dawning Kit, Foundational Software Development Kit. Zero dependency: C standard Library, Cross Architecture Assembler.

## Overview
- **`/bit`** Bit Kit: Provides foundational primitives for code generation in a bare bones UNIX environment.
- **`/doc`** Doc Kit: HTML & Markdown utilities.
- **`/linux`** Linux Kit: Modular OS primitives evolved from **Dawning EOS** - a complete experimental Linux distribution that proved zero-dependency, profile-based system building.
- **`/standard`** C Standard: Entirely self-contained C standard library, also pioneering new syntax and clearer semantics.
- **`/test`** Test Kit: Testing utilities, cross architecture

## Bit Kit

Foundational primitives for code generation, provides "ring zero" level utils for building executables,
it's all in shell so portability isn't a concern, and is transparent for your most critical path, where opaque and complex 3rd party binaries might be a concern.
This aims for being ideal for boot strapping toolchains from nothing, a full "compiled yourself down to the last byte"

Usage: 
```sh
. bit/kit.sh
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

`bit_8` `bit_16` `bit_32` `bit_64` `bit128` 

### Elf Executable format
Generates a ELF executable header and outputs a working executable

```sh
. bit/kit.sh

elf_example() {
        bit_8 0x48, 0xc7, 0xc0, 0x3c, 0x00, 0x00, 0x00 # mov $60, %rax
        bit_8 0x48, 0xc7, 0xc7, 0x00, 0x00, 0x00, 0x00 # mov $0, %rdi
        bit_8 0x0f, 0x05                               # x86_64 linux syscall
}

elf bin/program elf_example
```

### Wasm (work in progress)
Bit kit also have wasm primitives for generating WebAssembly modules,

`wasm_var` `wasm_section` `wasm`

```sh
. bit/kit.sh

type_section() {
    wasm_var 1              # 1 type
    bit_8 0x60             # func type  
    bit_8 0x00, 0x01, 0x7F # () -> i32
}

function_section() {
    wasm_var 1              # 1 function
    wasm_var 0              # uses type 0
}

export_section() {
    wasm_var 1              # 1 export
    wasm_var 4              # name length
    bit_8 "main"           # name
    bit_8 0x00, 0x00       # func export, index 0
}

code_section() {
    wasm_var 1              # 1 function
    wasm_var 2              # body size
    bit_8 0x41, 0x00, 0x0B # i32.const 0, end
}

wasm_module() {
    wasm_section 1 type_section
    wasm_section 3 function_section
    wasm_section 7 export_section
    wasm_section 10 code_section
}

wasm example.wasm wasm_module
```

## Doc Kit
primitives to generate HTML and Markdown documentation in HTML.
Works entirely within shell itself, this outputs ugly HTML to not waste space.

Example turning this readme into a HTML file:
```sh
. doc/kit.sh

doc README.md > README.html
```

### Basic css minification
```sh
. doc/kit.sh

less_css "style/*.css" dist/style.css
```

output: `CSS: 2.0 KB → 1.2 KB (37% smaller)`


## Linux Kit
Linux Distro primitives evolved from [**Dawning EOS**](https://github.com/dawnlarsson/dawning-linux)

> [!WARNING]
> This is currently being moved from the Dawning EOS repository to this one.

### Building
Ensure to cd into `dawning-kit/linux` before running build.sh

Minimal config for x86_x64
```sh
sudo sh build.sh arch/x64 debug_none
```

Minimal config for raspberry pis (WIP)
```sh
sudo sh build.sh arch/arm.pi debug_none
```

if you want to run this in a virtual machine for testing:
```sh
sh build.run.sh
```
but, you need https://www.qemu.org/

## Dawning C Standard
> Syntax shapes the way you think. Better thinking should be standardized.

The Dawning C Standard library is a effort to develop a new entirely self contained standard library,
It's also trying to lay the ground work for less error prone DX and type semantics in C.

Traditional type systems and APIs prioritize implementation details over clear expression of intent.

The Type system is explicit about ranges, memory layouts, and semantic meaning. 

Types like `positive`, `bipolar`, and `decimal` **communicate intention**, not just implementation.
The improved clarity aims to make systems programming **safer AND faster** by having **lower cognative load** on the programmer. 


By carefully re-designing the API and type expression, code can become more effective to think, write and audit.

## Test Kit
Provides automated test runner for multiple architectures with QEMU.

```sh
. test/kit.sh

# path / file_name + .<arch>  - becomes the expected usage pattern
test_all /path/to/bin/folder file_name
```


## Support
Did you know this effort has gone 100% out of my pocket?
If you think this project speaks for itself, consider supporting on github sponsors to continue making
projects like these a reality, open & free.

Supporter or not, you can **always** reach me on <a href="https://discord.gg/cxRvzUyzG8">My Discord Server, my primary communication channel</a>
Questions, feedback or support related to any of my projects, or if you need consulting.

## License
Logos, Branding, Trademarks - Copyright Dawn Larsson 2022

Repository:
Apache-2.0 license 
