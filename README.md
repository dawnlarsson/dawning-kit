<img width="1155" height="130" alt="Dawning Linux Header (1) (1)" src="https://github.com/user-attachments/assets/30e9c273-0f93-456a-ae8d-1a3010f3bc24" />

<br><br>

Moonwater is a research distro, think "Linux++"
foundational parts of the common userspace is moved into the moonwater kernel module to provide a immutable working base version of the system regardless of the userspace, performance and latency is a huge part of the why of this project.

Moonwater is also supposed to be super small, sub 15 mb for the entire system.

## The build tool

The whole build path is one C program, `src/build/build.c`, built on this
project's own freestanding stack. Bootstrap it with one command, from the
repository root:

```sh
cc -O2 -static -nostdlib -nostartfiles -fno-stack-protector -fno-builtin -w \
   -o build src/build/build.c
```

That is all a bare machine needs: a C compiler and an assembler. Nothing is
linked and no library is required. `sh build.sh` runs exactly that line and
then hands over, so either front door works and both mean the same thing.

Besides the build, the tool is the pieces the build is made of:

```sh
./build config <profile ...>                    compose artifacts/.config
./build verify-config <config> [profile ...]    what the profiles did not get
./build asm <arch> <in.asm> <out.S>             one architecture out of a .asm
./build spark <source> <output> [debug]         link a spark program
./build freestanding [-v] [--run] [--watch] [source] [output]
./build floor [arch]                            prove the ISA floor
./build key <name>                              a value from artifacts/.config
```

Nothing in it names this project: the paths, the linker script, the entry
symbol, the flag sets, the ISA floor, the image layout and the kernel version
are settings, overridable one at a time with `--set name=value`. Point them at
another tree and the same guarantees apply there.

Building a kernel wants a Linux toolchain and a case-sensitive filesystem, so
on anything else -- a Mac, most obviously -- point `--host` at a machine that
has them, or set `MOONWATER_BUILD_HOST` once and forget about it. Everything
after the build is local either way: QEMU runs on this machine so the window,
the mouse and the keyboard are real.

## Building a desktop or server image

The default build starts the in-kernel Canvas desktop. Two profiles provide
console-first images without changing the shell, utilities, Spark loader or
automatic network setup:

```sh
# Keep Canvas in the image, but start the shell directly on the kernel console.
sh build.sh arch/x64 debug_none limbo desktop serial terminal

# Do not compile or link Canvas or its architecture-specific renderer assembly.
sh build.sh arch/x64 debug_none limbo desktop serial server
```

The same split is available in Kconfig. `CONFIG_MOONWATER_CANVAS_AUTOSTART=n`
keeps Canvas compiled but leaves the display to the framebuffer, virtual or
serial console. `CONFIG_MOONWATER_CANVAS=n` removes Canvas completely. A
console-first configuration must not use the `drm_client_lib.active=` kernel
argument because that argument deliberately suppresses the framebuffer
console used in place of Canvas.

## Selecting the bundled userspace

The shell and utility payloads are independent Kconfig components. Both
default to enabled, preserving the complete image:

| `MOONWATER_SHELL` | `MOONWATER_UTILITIES` | Bundled payload |
| --- | --- | --- |
| `y` | `y` | One `/shell` image providing PID 1, `/bin/sh` and all applets |
| `y` | `n` | PID 1 and the shell, without the general utility registry |
| `n` | `y` | Utility-only multicall image, without `/init` or `/bin/sh` |
| `n` | `n` | No bundled userspace program |

The latter two configurations need another initramfs to provide `/init`.
The utility-only image stays at `/shell` because that path is part of Spark's
accelerated tool-spawn ABI, but an unknown applet returns 127 rather than
starting an interactive shell.

`fs/` is Moonwater's generated staging tree and each build clears its program
entries, so it is not an overlay input for a replacement init. A profile that
disables the shell must point `CONFIG_INITRAMFS_SOURCE` at a separate,
user-owned tree or archive, or boot with an external root filesystem.

`CONFIG_MOONWATER_UTIL_LINUX=n` removes the util-linux applet roots while
keeping the general utility surface. When the shell and utilities are enabled,
`CONFIG_MOONWATER_SHELL_MONITOR=n` omits the native monitor applet and its thin
launcher scripts. Disabled registry roots are discarded by the section linker,
so these settings reduce the compiled payload as well as the installed names.

## The tests

One command, and everything is under `test/`:

```
sh test/run                     every lane
sh test/run shell text          named lanes only
sh test/run bench               the benchmarks instead
sh test/run bench --list        what there is to measure
```

Two lanes want a built image and will say so rather than pass quietly:
`boot` starts it under QEMU and talks to the shell it reaches, and
`canvas` reads the desktop back a pixel at a time. Point them at one
with `MOONWATER_IMAGE=dist/bootx64.efi` if it is not where they look.

Three files, and no more: `test/run` is the command, `test/checks.c` holds
every C check and benchmark as a `CHECK_<name>` or `BENCH_<name>` section, and
`test/differential.py` is the engine, the grammars and the pinned rows.

Nobody writes cases. Each program is declared as a grammar -- its options, the
values they take, the operand shapes, the inputs worth feeding it -- and the
engine walks that surface, running the system's tool and ours in the same
recreated directory and comparing the exit status, standard output, the effect
on the directory and the diagnostic. Agreeing is passing; there is no separate
idea of a right answer. New coverage goes into a grammar, never into a new
file.

Where we answer differently on purpose, a pinned row says so and says why, and
fails if the difference ever disappears. Where a case has no determined answer
-- a terminal transcript interleaved by timing, a namespace the kernel fills
as it pleases -- it is recorded by name and counted in neither column, so it
cannot move a result either way.

## Bowl

Bowl runs Debian, Arch, or another Linux userspace directly on the Moonwater
kernel. The default fast profile merges the distribution's packages with the
host filesystem without a VM or supervisor fork; `--isolated` supplies the
complete namespace and root view needed by package managers. Selected package
executables can be exposed on Moonwater's global path with `bowl expose`.

See [the Bowl runtime notes](src/bowl/README.md) for the current commands and
installation flow.

## "Moonwater"?
Some believe that if you leave a bowl of water outside under a full moon, it absorbs celestial energy.
I thought it was a funny name for this, as much of this project is unproven and experimental.

## License
Apache-2.0 license
