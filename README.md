![Dawning Kit](https://github.com/user-attachments/assets/ad148eba-423f-4bc5-9d32-1e9005c8ebb7)

Dawning Kit, Foundational Software Development Kit. Zero dependency: C standard Library, Cross Architecture Assembler.

## Overview
- **`bit.sh`** Bit Kit: Provides foundational primitives for code generation in a bare bones UNIX environment.
- **`emit.sh`** Emit Kit: Cross architecture instruction emitters built on top of Bit Kit.
- **`doc.sh`** Doc Kit: HTML & Markdown utilities.
- **`test.sh`** Test Kit: Testing utilities, cross architecture.
- **`utils.sh`** Shared helpers (sizes, file iteration) used by the kits above.
- **`/linux`** Linux Kit: Modular OS primitives evolved from **Dawning EOS** - a complete experimental Linux distribution that proved zero-dependency, profile-based system building.
- **`/standard`** C Standard: Entirely self-contained C standard library, also pioneering new syntax and clearer semantics.

## Example Use


```sh
git clone https://github.com/dawnlarsson/dawning-kit 2>/dev/null || true

. dawning-kit/doc.sh
```

```sh
# Required for nested usage of dawning-kit
KIT_DIR=$(pwd)/dawning-kit

. "$KIT_DIR/bit.sh"
```

## Bit Kit

Foundational primitives for code generation, provides "ring zero" level utils for building executables,
it's all in shell and transparent for your most critical path, where opaque and complex 3rd party binaries might be a concern.
This aims for being ideal for boot strapping toolchains from nothing, a full "compiled yourself down to the last byte"

`bit.sh` and `emit.sh` are POSIX shell and are tested under both `bash` and `dash`.
`doc.sh`, `utils.sh` and `test.sh` use bash features and need `bash` (3.2 or newer,
so the version macOS ships is fine). The scripts under `/linux` and `/standard`
are POSIX and are meant to be run with `sh`.

Usage: 
```sh
. dawning-kit/bit.sh
```

### Primitives:

#### endianness flag
respects endianness, before running any of the primitives you can set big endianness
```sh
ENDIAN=BIG | big | 1
```

#### Bytes
Byte functions take any amount of args, each arg is separately represented in the functions size,
you can input hex (0x7f) or chars ( ELF -> "E", "L" "F" ), or plain ints

`bit_8` `bit_16` `bit_32` `bit_64` `bit128` `hex_dump` `elf`

Each function always emits exactly its own width. A value that does not fit is
masked to the width and a warning goes to stderr, so the byte count never
changes with the input; set `BIT_QUIET=1` to silence the warnings. Negative
values are encoded two's complement. Arguments may be separated by spaces,
commas, or both.

### Elf Executable format
Generates an ELF executable header and outputs a working executable.

`elf <output> <generator> [arch]` -- `arch` is `x86_64` (the default), `aarch64`
or `riscv64`. The generator must emit code for the same architecture.

```sh
. dawning-kit/bit.sh

elf_example() {
        bit_8 0x48, 0xc7, 0xc0, 0x3c, 0x00, 0x00, 0x00 # mov $60, %rax
        bit_8 0x48, 0xc7, 0xc7, 0x00, 0x00, 0x00, 0x00 # mov $0, %rdi
        bit_8 0x0f, 0x05                               # x86_64 linux syscall
}

elf bin/program elf_example

# or for arm64
elf bin/program elf_example aarch64
```

### Hex Dump
```sh
. dawning-kit/bit.sh

example() {
        bit_8 0x48, 0xc7, 0xc0, 0x3c, 0x00, 0x00, 0x00 # mov $60, %rax
        bit_8 0x48, 0xc7, 0xc7, 0x00, 0x00, 0x00, 0x00 # mov $0, %rdi
        bit_8 0x0f, 0x05                               # x86_64 linux syscall
}

hex_dump example
```

output:
```sh
example 16 bytes
00000000   48 c7 c0 3c 00 00 00 48 c7 c7 00 00 00 00 0f 05   H..<...H........
```

### Wasm (work in progress)
Bit kit also have wasm primitives for generating WebAssembly modules,

`wasm_var` `wasm_svar` `wasm_section` `wasm_body` `wasm`

```sh
. dawning-kit/bit.sh

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

code_body() {
    wasm_var 0              # 0 local declarations
    bit_8 0x41, 0x00, 0x0B # i32.const 0, end
}

code_section() {
    wasm_var 1              # 1 function
    wasm_body code_body     # length prefixed function body
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

Text, code blocks and attributes are HTML escaped, and `href`/`src` values are
restricted to `http`, `https`, `mailto`, `tel` and `data:image/` -- anything else
becomes `#`. Lines that already contain an HTML tag are passed through verbatim,
so you can drop raw HTML into a document on purpose.

Example turning this readme into a HTML file:
```sh
. dawning-kit/doc.sh

doc README.md > README.html
```

### Basic css minification
```sh
. dawning-kit/doc.sh

less_css "style/*.css" dist/style.css
```

output: `CSS: 2.0 KB → 1.2 KB (37% smaller)`

## Dawning EOS (Linux Kit)
aims to provide an easy and highly configurable Linux distro, 
leveraging Dawning Kit to build an immutable core with zero 3rd party dependencies. 

These primitives evolved from [**Archived Dawning EOS R&D Repo**](https://github.com/dawnlarsson/dawning-linux)

### Trying it

```sh
sh launch
```

Builds an image and boots it in a window with a working mouse. The kernel is
built on another machine over ssh, because building it wants a Linux toolchain
and a case sensitive filesystem; set `DAWNING_BUILD_HOST` to something you can
reach (it defaults to `box`). QEMU runs locally, so the window and the pointer
are real.

`sh launch --shell` puts the console on the terminal instead of opening a
window, and `sh launch --run` boots the last image without rebuilding. Extra
profiles can be named: `sh launch desktop`.

### On real hardware

```sh
sh launch --usb
```

Builds an image and writes it to a USB stick, after showing you which
removable disks it can see and making you type the name of the one to erase.
The kernel is built with the EFI stub, so it is itself an EFI application and
goes straight to `\EFI\BOOT\BOOTX64.EFI` with no bootloader involved.

On the machine: boot it, pick the stick from the firmware boot menu, and make
sure it is booting UEFI rather than legacy. Secure Boot has to be off, because
this kernel is not signed.

If the screen stays black, write `sh launch --usb console` instead. That
builds the same system with the compositor turned off so the framebuffer
console takes the screen and prints the boot log -- which on a desktop with no
serial port is the difference between reading the failure and guessing at it.

### Building
Ensure to cd into `dawning-kit/linux` before running build.sh

The kernel tarball is verified against a PGP signature pinned in
`script/kernel_setup` before it is extracted. If the signature does not verify,
the build stops.

Currently tracking **Linux 7.2**. Moving to another release means editing
`kernel_version` in `script/kernel_setup` and pasting the matching signature
from `cdn.kernel.org/pub/linux/kernel/vX.x/linux-VERSION.tar.sign`; the
download URL is derived from the version. A longterm release is the safer
choice if you care more about stability than about being current. Note that profiles are shell-evaluated as root during the
`pre`/`post` build steps, so treat adding a profile as running code as root.

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

### Glue: assembly for every architecture, in one file

Assembly in `linux/src` goes in a `.asm` file, one file per thing rather than
one per machine. Each block names the architectures it is for; the rest are
deleted before the file ever reaches a compiler, and what survives is handed
to whatever assembler the toolchain provides.

```asm
#include <linux/linkage.h>

SYM_FUNC_START(dawning_ticks)

#> arch x86_64
        rdtsc
        shl     $32, %rdx
        or      %rdx, %rax
        RET

#> arch arm64
        mrs     x0, cntvct_el0
        ret

#> arch other
#error "dawning_ticks: no tick counter for this architecture"

#> shared

SYM_FUNC_END(dawning_ticks)
```

The directives:

| | |
| --- | --- |
| `#> arch <name> [name ...]` | begin a block for these architectures |
| `#> arch other` | begin the block for every architecture no other block claimed |
| `#> shared` | go back to emitting for all of them |

Everything before the first `#> arch` is shared, which is where the includes
and anything portable belong. A `#> arch` ends the block before it, and so
does the end of the file.

Nothing is translated. A block holds the native syntax of its architecture
verbatim, so kernel assembly can be pasted in unchanged, and an assembler
error names a line of the `.asm` rather than a line of anything generated.
Architecture names are the ones `emit.sh` accepts and are normalized the same
way, so `arm64` and `aarch64` are one machine. An unknown name is an error,
and so is a file with architecture blocks but none for the one being built --
`#> arch other` with nothing under it says the omission was deliberate.

A `.asm` dropped in `linux/src` is picked up by `src/Makefile`, translated for
whichever architecture the kernel is configured for, and linked into the
Dawning module. Declare what it defines near the top of `src/dawning_core.c`
to call it from C.

To put one in the place of a file the kernel itself builds, a profile says

```sh
#> glue entry_dawning.asm arch/x86/entry/entry_64.S
```

The target has to be a file the kernel's Makefiles already compile -- this
replaces it, it cannot add a new object to somebody else's directory. What was
there is kept beside it as `entry_64.S.dawning-orig`, every build regenerates
from the `.asm` rather than from the replaced file, and removing the line puts
the original back on the next build.

The translator is `linux/src/glue` and runs standalone:

```sh
sh linux/src/glue arm64 some.asm some.S
```

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
. dawning-kit/test.sh

# expects <bin_folder>/<file_name>.<arch> for each architecture,
# for example bin/hello.x86_64 and bin/hello.aarch64
test_all bin hello
```

`test_all` prints a summary and returns non-zero if any architecture failed, so
it can gate CI. Architectures whose QEMU binary is not installed are skipped
rather than failed. A `timeout` implementation is used when present
(`timeout`, or `gtimeout` from coreutils on macOS).


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
