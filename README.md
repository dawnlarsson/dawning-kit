<img width="1153" height="160" alt="Dawning Linux Header-2" src="https://github.com/user-attachments/assets/675f22bb-fc92-43a2-9ba5-d957ee0d4d56" />

<br><br>

Moonwater is a research distro, think "Linux++"
foundational parts of the common userspace is moved into the moonwater kernel module to provide a immutable working base version of the system regardless of the userspace, performance and latency is a huge part of the why of this project.

Moonwater is also supposed to be super small, sub 15 mb for the entire system.

## moonwater cli

`moonwater` manages the system itself: where it lives, what it does at boot, and
what the machine's own events run.

```sh
moonwater                              where this session runs: live, from a disk, or waiting for an answer
moonwater install DISK [--removable]   erase DISK and install Moonwater on it
moonwater update [DISK]                write this build over an install, keeping its data and settings
moonwater use [DISK]                   run this build with an install's /bowls, /root and /home
moonwater live                         leave the disks alone this session

moonwater bind                         the machine's events, and what each runs
moonwater bind EVENT [COMMAND]         one event; empty puts the default back
moonwater bind poweroff [COMMAND]      the power button [poweroff]
moonwater bind canvas on|off [COMMAND] when the desktop starts or stops, not instead of canvas on|off
moonwater bind init                    list what runs at every boot, with ids
moonwater bind init add "command"      run a command at boot, once the disks are settled
moonwater bind init remove ID|"command"
moonwater bind init mount on|off       mount the data partition over /bowls, /root and /home [on]

moonwater bind exit                    list what runs when the machine powers off or reboots
moonwater bind exit add "command"      run a command before the disks go read-only
moonwater bind exit remove ID|"command"

moonwater canvas                       whether the desktop is on, and on which screens
moonwater canvas on|off                start or stop the desktop [on]

moonwater wifi                         the radio, saved networks, and the networks in range, or why there are none
moonwater wifi on|off                  unblock or block wifi
moonwater wifi add SSID [PASSWORD|-]   remember a network and join it; asks for the password at a terminal,
                                       - reads it from standard input [open if none]
moonwater bluetooth                    the bluetooth radio, and remembered devices
moonwater bluetooth on|off             unblock or block bluetooth
moonwater bluetooth add NAME           remember a bluetooth device
moonwater priority internet            which link to use when cable and wifi are both up
moonwater priority internet wired|wifi wired wins by default
moonwater time                         the clock: local, UTC, and whether NTP has set it
moonwater time sync                    ask an NTP server now, and in auto the zone too
moonwater timezone                     the clock's zone, and whether it is auto or manual
moonwater timezone auto                the zone the network is in [auto]: one Cloudflare request per network
moonwater timezone ZONE                IANA name, country code, +1 or POSIX TZ string; manual from then on
moonwater timezone list                every zone and country code
moonwater ntp                          whether the clock is set from the network
moonwater ntp on|off                   keep asking the network [on]
moonwater ntp filter [on|off]          keep the lowest-delay sample of five [on]
moonwater keyboard                     Canvas layout [us]
moonwater keyboard LAYOUT              us uk de se no dk fi fr es it
moonwater wipe                         forget /home and extra /root; keep the machine

moonwater link                         on or off, this machine's key, peers, what is open
moonwater link on|off                  listen on udp 22348, kept across boots [off]
moonwater link key                     this machine's public key, made on first use
moonwater link pair NAME KEY [HOST[:PORT]]  know a machine by its key, and where it is
moonwater link join NAMESPACE [SECRET] [allow GRANT...]  pair by itself with every machine on this network that joined it
moonwater link leave NAMESPACE [forget]  stop, and maybe forget the machines the group paired
moonwater link forget NAME             stop knowing it
moonwater link allow|deny NAME GRANT...  run shell files log screen channels verbs
moonwater link shell NAME              a terminal on NAME
moonwater link run NAME COMMAND...     one command on NAME: its output, errors and status here
moonwater link push NAME FILE PATH     a file to PATH on NAME, whole or not at all (grant files)
moonwater link pull NAME PATH FILE     PATH on NAME to a file here (grant files)
moonwater link log NAME                follow NAME's kernel log (grant log)
moonwater link serve                   the listener in the foreground (what link on runs)
```

`moonwater link` is ssh by key, over waterlink (`src/waterlink/`): UDP datagrams
sealed with AES-128-GCM after a Noise IK handshake, no users and no passwords.
Pair both ways, as with WireGuard: on each machine `moonwater link key` prints
its key, and `moonwater link pair` gives it the other's. A paired machine may do
nothing until allowed: `moonwater link allow laptop shell run`. `link shell`
puts the local terminal in raw mode, so ^C and friends go to the far side, and
each keystroke leaves in a datagram of its own at once; `link run` passes
standard input through and exits with the far command's status (255 if the link
itself failed). Both ends are Moonwater or Linux running this shell binary; only
a direct address works, there is no NAT traversal. The key (`/root/link.key`,
0600), the peers (`/root/link.peers`), the switch (`/root/link`) and an optional
port (`/root/link.port`) live beside the wifi networks, so install carries them
and wipe keeps them. The machine process starts the listener at boot when the
switch is on and restarts it if it dies.

For a machine nobody will stand in front of, join a group instead of pairing:
`moonwater link join office` makes a 160-bit secret and prints the line to run
on the other machines, and every machine on the same local network that joined
`office` with that secret pairs with it by itself, under the grants its own join
line gave (`allow shell run`, or only the verbs). Run it once on the live stick
before `moonwater install` and the installed machine is in the group from its
first boot; a machine script line `moonwater link join office` inside
`moonwater_init` names only the namespace. The secret is never kept, only what
600,000 rounds of PBKDF2 make of it, in `/root/link.groups`; status warns when
the machine script carries a secret, since any user can read that script through
`/dev/spark`. Machines find each other over mDNS as `_waterlink._udp` (visible
to `dns-sd -B` and avahi-browse), announcing nothing but random labels and
blinded tags, on the local link only.

Commands given to `moonwater` run as root through the shell, exactly as if typed
into a terminal. bind init runs in the background and keeps each command's output
in /run/moonwater/init; bind exit gives each command 10 seconds and all of them 30.

The bind, init and exit settings live inside the boot image. Set them on a live USB
stick and `moonwater install` takes them to the disk; `moonwater update` keeps the
disk's own. On a read-only stick a change lasts only for the session. `reset` is the
keyboard's reset/restart key; a reset button on a PC case is wired to the hardware
and cannot be bound. Bound keys go only to the binding; keys are not grabbed, so an
unbound one still types. `/root/main.moonwater.sh` overlays the kernel's builtin
machine script: see `main.moonwater.sh` in this repository.

Bare `moonwater wifi` lists the networks in range strongest first, with signal,
security and channel, `*` on the one joined and `+` on the saved ones, from the
kernel's last scan unless that is older than thirty seconds, when root asks for a
new one and waits five seconds at most. When wifi cannot be used it says why
instead, in one line that `moonwater status` shows too: no wireless hardware, a
card with no driver in the image, the firmware file its driver could not load, an
rfkill switch, wifi switched off, or the reason the last join failed. A password
given on the command line is visible to every user through `ps`; leave it off to be
asked, or pass `-` and pipe it in. Only open and WPA2 (and WPA2/WPA3 mixed) networks
can be joined; a network that asks for WPA3 alone, 802.1X or WEP is saved and not
tried. A network saved while there is no radio is joined when one appears.
Wifi passwords and the internet preference live on the data partition (`/root/wifi`,
`/root/internet`), not in the image, so `moonwater update` keeps them. When a cable
and wifi both have carrier, `/ip watch` uses the preference (`wired` if unset).
The timezone, NTP switch, clock filter and keyboard layout are the same shape
(`/root/timezone`, `/root/ntp`, `/root/ntp.filter`, `/root/keyboard`). NTP starts
with the machine and stays on until turned off: restore asks the network before
init, then the wait loop keeps walking `pool.ntp.org`, Google, Cloudflare and
their addresses until the kernel says the clock is synchronised. Each query
takes five samples and keeps the one with the smallest round-trip delay, which
is how RFC 5905's clock filter refuses a one-sided queue spike; `moonwater ntp
filter off` falls back to a single sample. The kernel adds that offset to
the current time; a kiss-o-death or a server whose root delay or dispersion
is worse than a second is dropped. There is no
zoneinfo directory: every tzdata zone and link (420 zones from tzdata 2026c,
`src/build/zones.py` regenerates them) maps to the POSIX rule its TZif footer
carries, which is right from the zone's last change on and does not replay older
history. `moonwater timezone list` prints them.

The zone is auto until one is set by hand. When the machine gets a default route
(at boot, on a new DHCP lease or wifi network: a different interface, gateway or
gateway hardware address), it makes one HTTPS request to
`speed.cloudflare.com/__down?bytes=0`, whose `timezone` header is Cloudflare's
geolocation of the connection, and takes that zone, or the most populous zone of
its `country` header, or of `loc=` from `cloudflare.com/cdn-cgi/trace` if the
first request fails. A name the table does not hold is never stored, and with no
answer the zone stays what it was. At most one request every three minutes,
backing off to an hour while failing, and none at all in manual mode.
`/root/timezone.mode` says which (`manual`, or `auto` and where it came from),
and `moonwater timezone` and `moonwater status` show it. A `/root/timezone` with no
mode beside it was set by hand before auto existed and stays manual until
`moonwater timezone auto`.
Canvas layouts other than US are the compositor's table, switched live.
The image carries the firmware its wifi and bluetooth drivers load, as zstd files
in `/lib/firmware` with their licences and WHENCE entries beside them, fetched from
linux-firmware at a pinned commit and checked against pinned SHA-256s; no blob is
in this repository. Besides PCI ethernet, the desktop image drives the common USB
ethernet adapters and docks, and the standard CDC ECM and NCM classes.

The same one-line shape covers the rest of the machine through bind events already:
`mute`, `micmute`, `volume_up`, `volume_down`, `brightness_up`, `brightness_down`,
`lid_close`, `lid_open`, `sleep`, `rfkill`, `tablet on`/`tablet off`,
`headphone on`/`headphone off`, `dock on`/`dock off`, `resume`. Those stay events
rather than a second config system. `moonwater bind tablet on` is the same
two-word shape as `moonwater bind canvas on`.

A kiosk is the machine script after wipe, not a second verb. `moonwater wipe`
empties `/home` and everything under `/root` except the overlay
(`/root/main.moonwater.sh`) and the radio files (`wifi`, `wifi.power`,
`bluetooth`, `bluetooth.power`, `internet`) plus `timezone`, `timezone.mode`, `ntp`,
`ntp.server`, `ntp.filter` and `keyboard`. `/bowls` is left alone, so
pre-installed software survives. The builtin script defines no
`moonwater_init`, so the list `moonwater bind init add` keeps is what runs at
boot, and `/home` is kept. A kiosk overlays the script with a `moonwater_init`
that wipes once the boot verdict is `live` or `disk` and then starts whatever
is already there: Chromium, Weston, a bowl binary -- the builtin carries that
function commented out. Start it in the background. `moonwater_init` runs in
the process that reads the event queue, and that process does not start
reading until the function returns, so a kiosk command left in the foreground
is a machine whose power button, lid and keys are queued and never run.
Defining `moonwater_init` takes the init row over: `bind init add` is refused
and the stored list does not run.

Canvas, the desktop, is part of the kernel: a compositor that draws with the CPU
through DRM, so it works on any display the kernel can drive.

`moonwater canvas off` closes every window, the one it was typed in too, and leaves
the kernel's text console with a shell on it; type `moonwater canvas on` there to
get the desktop back. From that console another display server, Weston say, can
take the screen. While another program holds the display, Canvas ignores the
keyboard and mouse until it lets go.

## The machine script

`main.moonwater.sh` at the repository root is the machine script. Clone the
repo, edit that file, rebuild. It is the working example: the three optional
hooks, per-event functions, `moonwater_event` for every event, and the poweroff
and reset fallback. The kernel bakes it into the module. `CONFIG_MOONWATER_MACHINE_SCRIPT`
can point at another path relative to the repository root.

Init starts `/shell -c 'moonwater machine'` with the network, not after the
disks: the process attaches immediately, waits for a boot verdict, then
overlays `/root/main.moonwater.sh` when that file is present and allowed. If
the disks never appear, the builtin still runs. Copying this file to
`/root/main.moonwater.sh` overlays the builtin without rebuilding. The builtin
init wipes userspace on every settled boot; the overlay is how a machine
chooses a kiosk command or keeps `/home`.

`moonwater bind` and `moonwater machine` both ask the module for the overlay.
They do not each scan a private copy. `function moonwater_mute` owns mute at
that line; `function moonwater_canvas` owns both `canvas on` and `canvas off`;
a literal `mute)` arm still owns that event. `moonwater_event`, when present,
runs for every event and does not own the bind table: `*)` is not ownership.
Events the overlay does not name still use the image binds.

The disk file must be a regular file, owned by root, and not group- or
world-writable. A symlink is refused. Moonwater opens it with `O_NOFOLLOW`.
Larger than 64 KiB is refused. A refused file is left alone: the builtin
stays, and a line goes to the kernel log.

`moonwater bind mute` then reads as `mute: /root/main.moonwater.sh:8` when
that function or arm is on disk, or `mute: builtin:8` when it is the baked
copy, and SET is refused:

```
[Moonwater] mute is /root/main.moonwater.sh:8; change it there
```

The same overlay applies to init and exit when `moonwater_init` or
`moonwater_end` exist: listing them names the hook, and add/remove is refused
the same way. `moonwater bind init mount` still works. Without those hooks, the
stored lists still run even if `moonwater_event` is present.

If the process is gone, the image binds spawn `/shell -c` again for events
the overlay does not own. Changing the disk file updates what the CLI prints
on the next `moonwater bind`; the running machine sources the kernel's copy
at start, so a new file is picked up on the next start.

## Bowl

Bowl runs Debian, Arch, or another Linux package manager directly on the Moonwater
kernel. The default fast profile binds only that package's loader and libc,
leaves Moonwater's applets in place, and does not use a VM or supervisor fork;
`--isolated` supplies the complete namespace and root view needed by package
managers. Selected package executables can be exposed on Moonwater's global
path with `bowl expose`.

on a fresh install of moonwater:
```sh
bowl setup <alpine | arch | debian | fedora | nix>
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

The desktop image carries webcam drivers, V4L2 and the USB video class, and
USB audio, so ffmpeg or a browser installed in a bowl finds a USB camera and
its microphones as `/dev/video*` and `/dev/snd/*`. A bowl cannot load a kernel
driver of its own.

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
colour. Keyboard layouts other than US are `moonwater keyboard`.

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
