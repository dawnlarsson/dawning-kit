<img width="1155" height="130" alt="Dawning Linux Header (1) (1)" src="https://github.com/user-attachments/assets/30e9c273-0f93-456a-ae8d-1a3010f3bc24" />

<br><br>

Moonwater is a research distro, think "Linux++"
foundational parts of the common userspace is moved into the moonwater kernel module to provide a immutable working base version of the system regardless of the userspace, performance and latency is a huge part of the why of this project.

Moonwater is also supposed to be super small, sub 15 mb for the entire system.

## moonwater cli

`moonwater` manages the system itself: where it lives, what it does at boot, and
what the machine's own buttons do.

```sh
moonwater                              where this session runs: live, from a disk, or waiting for an answer
moonwater install DISK [--removable]   erase DISK and install Moonwater on it
moonwater update [DISK]                write this build over an install, keeping its data and settings
moonwater use [DISK]                   run this build with an install's /bowls, /root and /home
moonwater live                         leave the disks alone this session

moonwater init                         list what runs at every boot, with ids
moonwater init add "command"           run a command at boot, once the disks are settled
moonwater init remove ID|"command"
moonwater init mount on|off            mount the data partition over /bowls, /root and /home [on]

moonwater exit                         list what runs when the machine powers off or reboots
moonwater exit add "command"           run a command before the disks go read-only
moonwater exit remove ID|"command"

moonwater button power                 show what the power button runs
moonwater button power "command"       run a command when it is pressed [poweroff]
```

Commands given to `moonwater` run as root through the shell, exactly as if typed
into a terminal. init runs in the background and keeps each command's output in
/run/moonwater/init; exit gives each command 10 seconds and all of them 30.

The init and exit settings live inside the boot image. Set them on a live USB stick
and `moonwater install` takes them to the disk; `moonwater update` keeps the disk's
own. On a read-only stick a change lasts only for the session.

Canvas, the desktop, is part of the kernel: a compositor that draws with the CPU
through DRM, so it works on any display the kernel can drive.

## Bowl

Bowl runs Debian, Arch, or another Linux package directly on the Moonwater
kernel. The default fast profile binds only that package's loader and libc,
leaves Moonwater's applets in place, and does not use a VM or supervisor fork;
`--isolated` supplies the complete namespace and root view needed by package
managers. Selected package executables can be exposed on Moonwater's global
path with `bowl expose`.

on a fresh install of moonwater:
```sh
bowl setup <alpine | arch | debian | fedora>
```
then just use the package manger from the distro like normal:
```sh
pacman -Syu package_name
```

in other words, your software can be installed from multiple places on one system 
accelerated by Moonwaters native and very fast coreutils, 
util-linux, and Moonwater Shell (supports all of bash, dash)

`bowl setup` checks there is room before it downloads anything and says how
much it needs. On a live USB session bowls are kept in memory, so a large
install can run out of room; `moonwater install DISK` puts bowls on a data
partition instead. If a setup or package install still runs out of space,
bowl says how low free space went.

An `--isolated` bowl sees the host's kernel settings (`/proc/sys`, `/sys`)
read-only, so a package's install scripts cannot change the host. A bowl is
not a security sandbox: its programs run as root and can reach the host's
devices.

## The terminal

Canvas opens a terminal at boot and on Control-Shift-T. It answers as
`xterm-256color`, and programs inside a bowl find the same entry in their own
terminfo, so nano, vim, less, htop, btop and ncurses programs draw as they do
in xterm or tmux.

- 256 colours; a true-colour request is drawn in the nearest of them.
- UTF-8 throughout: ideographs and emoji take two columns and combining marks
  take none, so text lines up as programs count it. Characters the built-in
  VGA font has no glyph for draw as a box.
- Full-screen programs get their own screen, and quitting one gives back the
  shell's screen and scrollback. Resizing keeps what is on screen where it
  was, and lines scrolled away under a progress bar such as apt's stay in
  scrollback.
- Keys: Return sends carriage return, Control with a symbol sends the control
  code xterm does (Control-\\, Control-], Control-^, Control-_ and Control-/,
  and Control-Space for NUL), Shift-Tab and modified arrows carry their
  modifiers. Mouse reporting, focus events, cursor shapes, synchronized
  output, colour and mode queries and window size reports are answered.
- A shell killed by a signal it was not sent by the window -- out of memory,
  for instance -- leaves the window open with a line saying so; close it with
  its button.

Not there yet: pasting (there is no clipboard), true colour kept as true
colour, and keyboard layouts other than US.

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

## Boot options

`moonwater.cursor_plane` decides how Canvas shows the pointer. It is on by
default: the pointer rides the graphics card's own cursor plane, so moving it
touches no pixels. If a machine's graphics driver draws a broken or missing
hardware cursor, put this on the kernel command line and Canvas draws the
pointer itself:
```sh
moonwater.cursor_plane=0
```
to check what a machine is using, read
`/sys/module/moonwater/parameters/cursor_plane` (`Y` or `N`), or run
`pointer`: its `cursor plane knob` line is the setting, and `cursor planes`
says how many screens actually have one.

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

## "Moonwater"?
Some believe that if you leave a bowl of water outside under a full moon, it absorbs celestial energy.
I thought it was a funny name for this, as much of this project is unproven and experimental.

## License
Apache-2.0 license
